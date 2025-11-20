/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MuptiActivityProfiler.h"
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <limits>
#include "ApproximateClock.h"

#ifdef HAS_MUPTI
#include <mupti.h>
#include "MusaUtil.h"
#endif

#include "Config.h"
#include "MusaDeviceProperties.h"

#include "time_since_epoch.h"
#ifdef HAS_MUPTI
#include "MuptiActivity.h"
#include "MuptiActivity.cpp"
#include "MuptiActivityApi.h"
#endif // HAS_MUPTI
#include "output_base.h"
#include "ActivityBuffers.h"

#include "Logger.h"
#include "ThreadUtil.h"

using namespace std::chrono;
using std::string;

struct CtxEventPair {
  uint32_t ctx = 0;
  uint32_t eventId = 0;

  bool operator==(const CtxEventPair& other) const {
    return (this->ctx == other.ctx) && (this->eventId == other.eventId);
  }
};

template<>
struct std::hash<CtxEventPair> {
  std::size_t operator()(const CtxEventPair& c) const {
    return KINETO_NAMESPACE::detail::hash_combine(
      std::hash<uint32_t>()(c.ctx),
      std::hash<uint32_t>()(c.eventId)
    );
  }
};

struct WaitEventInfo {
  // MUSA stream that the MUSA event was recorded on
  uint32_t stream;
  // Correlation ID of the musaEventRecord event
  uint32_t correlationId;
};

namespace {

// Map (ctx, eventId) -> (stream, corr Id) that recorded the MUSA event
std::unordered_map<CtxEventPair, WaitEventInfo>& waitEventMap() {
  static std::unordered_map<CtxEventPair, WaitEventInfo> waitEventMap_;
  return waitEventMap_;
}

// Map ctx -> deviceId
std::unordered_map<uint32_t, uint32_t>& ctxToDeviceId() {
  static std::unordered_map<uint32_t, uint32_t> ctxToDeviceId_;
  return ctxToDeviceId_;
};

}

namespace KINETO_NAMESPACE {

// Sets the timestamp converter. If nothing is set then the converter just
// returns the input. For this reason, until we add profiler impl of passing in
// TSC converter we just need to guard the callback itself
std::function<time_t(approx_time_t)>& get_time_converter() {
  static std::function<time_t(approx_time_t)> _time_converter =
      [](approx_time_t t) { return t; };
  return _time_converter;
}

#ifdef HAS_MUPTI
bool& use_mupti_tsc() {
  static bool use_mupti_tsc = true;
  return use_mupti_tsc;
}
#endif

ConfigDerivedState::ConfigDerivedState(const Config& config) {
  profileActivityTypes_ = config.selectedActivityTypes();
  profileStartTime_ = config.requestTimestamp();
  profileDuration_ = config.activitiesDuration();
  profileWarmupDuration_ = config.activitiesWarmupDuration();
  profilingByIter_ = config.hasProfileStartIteration();
  perThreadBufferEnabled_ = config.perThreadBufferEnabled();
  if (profilingByIter_) {
    profileStartIter_ = config.profileStartIteration();
    profileEndIter_ = profileStartIter_ + config.activitiesRunIterations();
  } else {
    profileEndIter_ = (std::numeric_limits<decltype(profileEndIter_)>::max)();
    profileEndTime_ = profileStartTime_ + config.activitiesDuration();
  }
}

bool ConfigDerivedState::canStart(
    const std::chrono::time_point<std::chrono::system_clock>& now) const {
  if (profilingByIter_) {
    return true;
  }
  if (profileStartTime_ < now) {
    LOG(ERROR)
        << "Not starting tracing - start timestamp is in the past. Time difference (ms): "
        << duration_cast<milliseconds>(now - profileStartTime_).count();
    return false;
  } else if ((profileStartTime_ - now) < profileWarmupDuration_) {
    LOG(ERROR)
        << "Not starting tracing - insufficient time for warmup. Time to warmup (ms): "
        << duration_cast<milliseconds>(profileStartTime_ - now).count();
    return false;
  }
  return true;
}

bool ConfigDerivedState::isWarmupDone(
    const time_point<system_clock>& now,
    int64_t currentIter) const {
  bool isTimestampBased = !profilingByIter_ && currentIter < 0;
  if (isTimestampBased) {
    // qualify that this check is not being called from application step() API
    // this avoids races between the step() API and periodically invoked
    // profiler run loop step() method
    return now >= profileStartTime_;
  }
  bool isIterationBased = profilingByIter_ && currentIter >= 0;
  if (isIterationBased) {
    return currentIter >= profileStartIter_;
  }
  return false;
}

bool ConfigDerivedState::isCollectionDone(
    const time_point<system_clock>& now,
    int64_t currentIter) const {
  bool isTimestampBased = !profilingByIter_ && currentIter < 0;
  if (isTimestampBased) {
    // qualify that this check is not being called from application step() API
    return now >= profileEndTime_;
  }
  bool isIterationBased = profilingByIter_ && currentIter >= 0;
  if (isIterationBased) {
    return currentIter >= profileEndIter_;
  }
  return false;
}

std::ostream& operator<<(std::ostream& oss, const MuptiActivityProfiler::ErrorCounts& ecs) {
  oss << "Out-of-range = " << ecs.out_of_range_events
      << ", Blocklisted runtime = " << ecs.blocklisted_runtime_events
      << ", Invalid ext correlations = " << ecs.invalid_external_correlation_events
      << ", CPU GPU out-of-order = " << ecs.gpu_and_cpu_op_out_of_order
#if defined(HAS_MUPTI)
      << ", Unexpected MUSA events = " << ecs.unexepected_musa_events
      << ", MUPTI stopped early? = " << ecs.mupti_stopped_early;
#else
      ;
#endif // HAS_MUPTI
  return oss;
}

MuptiActivityProfiler::~MuptiActivityProfiler() {
  if (collectTraceThread_ && collectTraceThread_->joinable()) {
    collectTraceThread_->join();
  }
}

void MuptiActivityProfiler::transferCpuTrace(
    std::unique_ptr<libkineto::CpuTraceBuffer> cpuTrace) {
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  const string& trace_name = cpuTrace->span.name;
  if (currentRunloopState_ != RunloopState::CollectTrace &&
      currentRunloopState_ != RunloopState::ProcessTrace) {
    VLOG(0) << "Trace collection not in progress - discarding span "
            << trace_name;
    return;
  }

  cpuTrace->span.iteration = iterationCountMap_[trace_name]++;

  VLOG(0) << "Received iteration " << cpuTrace->span.iteration << " of span "
          << trace_name << " (" << cpuTrace->activities.size()
          << " activities / " << cpuTrace->gpuOpCount << " gpu activities)";
  traceBuffers_->cpu.push_back(std::move(cpuTrace));
}

MuptiActivityProfiler::MuptiActivityProfiler(
    MuptiActivityApi& mupti,
    bool cpuOnly)
    : mupti_(mupti),
      flushOverhead_{0, 0},
      setupOverhead_{0, 0},
      cpuOnly_{cpuOnly},
      currentRunloopState_{RunloopState::WaitForRequest} {

#ifdef HAS_MUPTI
  if (isGpuAvailable()) {
    logMusaVersions();
  }
#endif
}

#ifdef HAS_MUPTI
void MuptiActivityProfiler::logMusaVersions() {
  // check MUSA versions
  uint32_t muptiVersion = 0;
  int musaRuntimeVersion = 0, musaDriverVersion = 0;
  MUPTI_CALL(muptiGetVersion(&muptiVersion));
  MUSA_CALL(musaRuntimeGetVersion(&musaRuntimeVersion));
  MUSA_CALL(musaDriverGetVersion(&musaDriverVersion));
  LOG(INFO) << "MUSA versions. MUPTI: " << muptiVersion
            << "; Runtime: " << musaRuntimeVersion
            << "; Driver: " << musaDriverVersion;
  LOGGER_OBSERVER_ADD_METADATA(
      "mupti_version", std::to_string(muptiVersion));
  LOGGER_OBSERVER_ADD_METADATA(
      "musa_runtime_version", std::to_string(musaRuntimeVersion));
  LOGGER_OBSERVER_ADD_METADATA(
      "musa_driver_version", std::to_string(musaDriverVersion));
  addVersionMetadata("mupti_version", std::to_string(muptiVersion));
  addVersionMetadata(
      "musa_runtime_version", std::to_string(musaRuntimeVersion));
  addVersionMetadata("musa_driver_version", std::to_string(musaDriverVersion));

}
#endif

void MuptiActivityProfiler::processTraceInternal(ActivityLogger& logger) {
  LOG(INFO) << "Processing " << traceBuffers_->cpu.size() << " CPU buffers";
  VLOG(0) << "Profile time range: " << captureWindowStartTime_ << " - "
          << captureWindowEndTime_;
  for (auto& pair : versionMetadata_) {
    addMetadata(pair.first, pair.second);
  }
  std::vector<std::string> device_properties;
  if (auto props = devicePropertiesJson(); !props.empty()) {
    device_properties.push_back(props);
  }
  for (const auto& session : sessions_) {
    if (auto props = session->getDeviceProperties(); !props.empty()) {
      if (std::find(
              device_properties.begin(), device_properties.end(), props) ==
          device_properties.end()) {
        device_properties.push_back(props);
      }
    }
  }
  logger.handleTraceStart(
      metadata_, fmt::format("{}", fmt::join(device_properties, ",")));
  setCpuActivityPresent(false);
  setGpuActivityPresent(false);
  for (auto& cpu_trace : traceBuffers_->cpu) {
    string trace_name = cpu_trace->span.name;
    VLOG(0) << "Processing CPU buffer for " << trace_name << " ("
            << cpu_trace->span.iteration << ") - "
            << cpu_trace->activities.size() << " records";
    VLOG(0) << "Span time range: " << cpu_trace->span.startTime << " - "
            << cpu_trace->span.endTime;
    processCpuTrace(*cpu_trace, logger);
    LOGGER_OBSERVER_ADD_EVENT_COUNT(cpu_trace->activities.size());
  }

#ifdef HAS_MUPTI
  if (!cpuOnly_) {
    VLOG(0) << "Retrieving GPU activity buffers";
    traceBuffers_->gpu = mupti_.activityBuffers();
    if (VLOG_IS_ON(1)) {
      addOverheadSample(flushOverhead_, mupti_.flushOverhead);
    }
    if (traceBuffers_->gpu) {
      const auto count_and_size = mupti_.processActivities(
          *traceBuffers_->gpu,
          std::bind(
              &MuptiActivityProfiler::handleMuptiActivity,
              this,
              std::placeholders::_1,
              &logger));
      logDeferredEvents();
      LOG(INFO) << "Processed " << count_and_size.first << " GPU records ("
                << count_and_size.second << " bytes)";
      LOGGER_OBSERVER_ADD_EVENT_COUNT(count_and_size.first);

      // resourceOverheadCount_ is set while processing GPU activities
      if (resourceOverheadCount_ > 0) {
        LOG(INFO) << "Allocated " << resourceOverheadCount_
                  << " extra MUPTI buffers.";
      }
      LOGGER_OBSERVER_ADD_METADATA(
          "ResourceOverhead", std::to_string(resourceOverheadCount_));
    }
    if (!gpuActivityPresent()){
      LOG(WARNING) << "GPU trace is empty!";
    }
  }
#endif // HAS_MUPTI
  if (!traceNonEmpty()) {
    LOG(WARNING) << kEmptyTrace;
  }

  for (const auto& session : sessions_) {
    LOG(INFO) << "Processing child profiler trace";
    // cpuActivity() function here is used to get the linked cpuActivity for
    // session's activities. Passing captureWindowStartTime_ and
    // captureWindowEndTime_ in order to specify the range of activities that
    // need to be processed.
    session->processTrace(
        logger,
        std::bind(
            &MuptiActivityProfiler::cpuActivity, this, std::placeholders::_1),
        captureWindowStartTime_,
        captureWindowEndTime_);
  }

  LOG(INFO) << "Record counts: " << ecs_;

  finalizeTrace(*config_, logger);
}

MuptiActivityProfiler::CpuGpuSpanPair& MuptiActivityProfiler::recordTraceSpan(
    TraceSpan& span,
    int gpuOpCount) {
  TraceSpan gpu_span(gpuOpCount, span.iteration, span.name, "GPU: ");
  auto& iterations = traceSpans_[span.name];
  iterations.push_back({span, gpu_span});
  return iterations.back();
}

void MuptiActivityProfiler::processCpuTrace(
    libkineto::CpuTraceBuffer& cpuTrace,
    ActivityLogger& logger) {
  if (cpuTrace.activities.size() == 0) {
    LOG(WARNING) << "CPU trace is empty!";
    return;
  }
  setCpuActivityPresent(true);

  CpuGpuSpanPair& span_pair =
      recordTraceSpan(cpuTrace.span, cpuTrace.gpuOpCount);
  TraceSpan& cpu_span = span_pair.first;
  for (auto const& act : cpuTrace.activities) {
    VLOG(2) << act->correlationId() << ": OP " << act->activityName;
    if (derivedConfig_->profileActivityTypes().count(act->type())) {
      static_assert(
          std::is_same<
              std::remove_reference<decltype(act)>::type,
              const std::unique_ptr<GenericTraceActivity>>::value,
          "handleActivity is unsafe and relies on the caller to maintain not "
          "only lifetime but also address stability.");
      if (act->type() == ActivityType::USER_ANNOTATION &&
          act->duration() <= 0) {
        act->endTime = captureWindowEndTime_;
        act->addMetadata("finished", "false");
      } else {
        act->addMetadata("finished", "true");
      }
      logger.handleActivity(*act);
    }
    clientActivityTraceMap_[act->correlationId()] = &span_pair;
    activityMap_[act->correlationId()] = act.get();

    recordThreadInfo(act->resourceId(), act->getThreadId(), act->deviceId());
  }
  logger.handleTraceSpan(cpu_span);
}

#ifdef HAS_MUPTI
inline void MuptiActivityProfiler::handleCorrelationActivity(
    const MUpti_ActivityExternalCorrelation* correlation) {
  if (correlation->externalKind == MUPTI_EXTERNAL_CORRELATION_KIND_CUSTOM0) {
    cpuCorrelationMap_[correlation->correlationId] = correlation->externalId;
  } else if (
      correlation->externalKind == MUPTI_EXTERNAL_CORRELATION_KIND_CUSTOM1) {
    userCorrelationMap_[correlation->correlationId] = correlation->externalId;
  } else {
    LOG(WARNING)
        << "Invalid MUpti_ActivityExternalCorrelation sent to handleMuptiActivity";
    ecs_.invalid_external_correlation_events++;
  }
}
#endif // HAS_MUPTI

static GenericTraceActivity createUserGpuSpan(
    const libkineto::ITraceActivity& cpuTraceActivity,
    const libkineto::ITraceActivity& gpuTraceActivity) {
  GenericTraceActivity res(
      *cpuTraceActivity.traceSpan(),
      ActivityType::GPU_USER_ANNOTATION,
      cpuTraceActivity.name());
  res.startTime = gpuTraceActivity.timestamp();
  res.device = gpuTraceActivity.deviceId();
  res.resource = gpuTraceActivity.resourceId();
  res.endTime = gpuTraceActivity.timestamp() + gpuTraceActivity.duration();
  res.id = cpuTraceActivity.correlationId();
  return res;
}

void MuptiActivityProfiler::GpuUserEventMap::insertOrExtendEvent(
    const ITraceActivity& cpuTraceActivity,
    const ITraceActivity& gpuTraceActivity) {
  StreamKey key(gpuTraceActivity.deviceId(), gpuTraceActivity.resourceId());
  CorrelationSpanMap& correlationSpanMap = streamSpanMap_[key];
  auto it = correlationSpanMap.find(cpuTraceActivity.correlationId());
  if (it == correlationSpanMap.end()) {
    auto it_success = correlationSpanMap.insert(
        {cpuTraceActivity.correlationId(),
         createUserGpuSpan(cpuTraceActivity, gpuTraceActivity)});
    it = it_success.first;
  }
  GenericTraceActivity& span = it->second;
  if (gpuTraceActivity.timestamp() < span.startTime || span.startTime == 0) {
    span.startTime = gpuTraceActivity.timestamp();
  }
  int64_t gpu_activity_end =
      gpuTraceActivity.timestamp() + gpuTraceActivity.duration();
  if (gpu_activity_end > span.endTime) {
    span.endTime = gpu_activity_end;
  }
}

const MuptiActivityProfiler::CpuGpuSpanPair&
MuptiActivityProfiler::defaultTraceSpan() {
  static TraceSpan span(0, 0, "Unknown", "");
  static CpuGpuSpanPair span_pair(span, span);
  return span_pair;
}

void MuptiActivityProfiler::GpuUserEventMap::logEvents(ActivityLogger* logger) {
  for (auto const& streamMapPair : streamSpanMap_) {
    for (auto const& correlationSpanPair : streamMapPair.second) {
      correlationSpanPair.second.log(*logger);
    }
  }
}

inline bool MuptiActivityProfiler::outOfRange(const ITraceActivity& act) {
  bool out_of_range = act.timestamp() < captureWindowStartTime_ ||
      (act.timestamp() + act.duration()) > captureWindowEndTime_;
  if (out_of_range) {
    VLOG(2) << "TraceActivity outside of profiling window: " << act.name()
            << " (" << act.timestamp() << " < " << captureWindowStartTime_
            << " or " << (act.timestamp() + act.duration()) << " > "
            << captureWindowEndTime_;
    ecs_.out_of_range_events++;
  }
  return out_of_range;
}

#ifdef HAS_MUPTI
inline static bool isBlockListedRuntimeCbid(MUpti_CallbackId cbid) {
  // Some MUSA calls that are very frequent and also not very interesting.
  // Filter these out to reduce trace size.
  if (cbid == MUPTI_RUNTIME_TRACE_CBID_musaGetDevice_v3020 ||
      cbid == MUPTI_RUNTIME_TRACE_CBID_musaSetDevice_v3020 ||
      cbid == MUPTI_RUNTIME_TRACE_CBID_musaGetLastError_v3020 ||
      // Support musaEventRecord and musaEventSynchronize, revisit if others are
      // needed
      cbid == MUPTI_RUNTIME_TRACE_CBID_musaEventCreate_v3020 ||
      cbid == MUPTI_RUNTIME_TRACE_CBID_musaEventCreateWithFlags_v3020 ||
      cbid == MUPTI_RUNTIME_TRACE_CBID_musaEventDestroy_v3020) {
    return true;
  }

  return false;
}

void MuptiActivityProfiler::handleRuntimeActivity(
    const MUpti_ActivityAPI* activity,
    ActivityLogger* logger) {
  if (isBlockListedRuntimeCbid(activity->cbid)) {
    ecs_.blocklisted_runtime_events++;
    return;
  }
  VLOG(2) << activity->correlationId
          << ": MUPTI_ACTIVITY_KIND_RUNTIME, cbid=" << activity->cbid
          << " tid=" << activity->threadId;
  int32_t tid = activity->threadId;
  const auto& it = resourceInfo_.find({processId(), tid});
  if (it != resourceInfo_.end()) {
    tid = it->second.id;
  }
  const ITraceActivity* linked =
      linkedActivity(activity->correlationId, cpuCorrelationMap_);
  const auto& runtime_activity =
      traceBuffers_->addActivityWrapper(RuntimeActivity(activity, linked, tid));
  checkTimestampOrder(&runtime_activity);
  if (outOfRange(runtime_activity)) {
    return;
  }
  runtime_activity.log(*logger);
  setGpuActivityPresent(true);
}

void MuptiActivityProfiler::handleDriverActivity(
    const MUpti_ActivityAPI* activity,
    ActivityLogger* logger) {
  // we only want to collect muLaunchKernel events, for triton kernel launches
  if (!isKernelLaunchApi(*activity)) {
    // XXX should we count other driver events?
    return;
  }
  VLOG(2) << activity->correlationId
          << ": MUPTI_ACTIVITY_KIND_DRIVER, cbid=" << activity->cbid
          << " tid=" << activity->threadId;

  int32_t tid = activity->threadId;
  const auto& it = resourceInfo_.find({processId(), tid});
  if (it != resourceInfo_.end()) {
    tid = it->second.id;
  }
  const ITraceActivity* linked =
      linkedActivity(activity->correlationId, cpuCorrelationMap_);
  const auto& runtime_activity =
      traceBuffers_->addActivityWrapper(DriverActivity(activity, linked, tid));
  checkTimestampOrder(&runtime_activity);
  if (outOfRange(runtime_activity)) {
    return;
  }
  runtime_activity.log(*logger);
  setGpuActivityPresent(true);
}

void MuptiActivityProfiler::handleOverheadActivity(
    const MUpti_ActivityOverhead* activity,
    ActivityLogger* logger) {
  VLOG(2) << ": MUPTI_ACTIVITY_KIND_OVERHEAD"
          << " overheadKind=" << activity->overheadKind;
  const auto& overhead_activity =
      traceBuffers_->addActivityWrapper(OverheadActivity(activity, nullptr));
  // Monitor memory overhead
  if (activity->overheadKind == MUPTI_ACTIVITY_OVERHEAD_MUPTI_RESOURCE) {
    resourceOverheadCount_++;
  }

  if (outOfRange(overhead_activity)) {
    return;
  }
  overhead_activity.log(*logger);
  setGpuActivityPresent(true);
}


std::optional<WaitEventInfo> getWaitEventInfo(
    uint32_t ctx, uint32_t eventId) {
  auto key = CtxEventPair{ctx, eventId};
  auto it = waitEventMap().find(key);
  if (it != waitEventMap().end()) {
    return it->second;
  }
  return std::nullopt;
}

void MuptiActivityProfiler::handleMusaEventActivity(
    const MUpti_ActivityMusaEvent* activity) {
  VLOG(2) << ": MUPTI_ACTIVITY_KIND_MUSA_EVENT"
          << " corrId=" << activity->correlationId
          << " eventId=" << activity->eventId
          << " streamId=" << activity->streamId
          << " contextId=" << activity->contextId;

  // Update the stream, corrID the musaEvent was last recorded on
  auto key = CtxEventPair{activity->contextId, activity->eventId};
  waitEventMap()[key] = WaitEventInfo{activity->streamId, activity->correlationId};
}

void MuptiActivityProfiler::handleMusaSyncActivity(
    const MUpti_ActivitySynchronization* activity,
    ActivityLogger* logger) {
  VLOG(2) << ": MUPTI_ACTIVITY_KIND_SYNCHRONIZATION"
          << " type=" << syncTypeString(activity->type)
          << " corrId=" << activity->correlationId
          << " streamId=" << activity->streamId
          << " eventId=" << activity->musaEventId
          << " contextId=" << activity->contextId;

  if (!config_->activitiesMusaSyncWaitEvents() &&
      isWaitEventSync(activity->type)) {
    return;
  }

  auto device_id = contextIdtoDeviceId(activity->contextId);
  int32_t src_stream = -1, src_corrid = -1;

  if (isEventSync(activity->type)) {
    auto maybe_wait_event_info = getWaitEventInfo(
        activity->contextId, activity->musaEventId);
    if (maybe_wait_event_info) {
      src_stream = maybe_wait_event_info->stream;
      src_corrid = maybe_wait_event_info->correlationId;
    }
  }

  // Marshal the logging to a functor so we can defer it if needed.
  auto log_event = [=](){
    const ITraceActivity* linked =
        linkedActivity(activity->correlationId, cpuCorrelationMap_);
    const auto& musa_sync_activity = traceBuffers_->addActivityWrapper(
        MusaSyncActivity(activity, linked, src_stream, src_corrid));

    if (outOfRange(musa_sync_activity)) {
      return;
    }

    if (int32_t(activity->streamId) != -1) {
      recordStream(device_id, activity->streamId, "");
    } else {
      recordDevice(device_id);
    }
    VLOG(2) << "Logging sync event device = " << device_id
            << " stream = " <<  activity->streamId
            << " sync type = " << syncTypeString(activity->type);
    musa_sync_activity.log(*logger);
    setGpuActivityPresent(true);
  };

  if (isWaitEventSync(activity->type)) {
    // Defer logging wait event syncs till the end so we only
    // log these events if a stream has some GPU kernels on it.
    DeferredLogEntry entry{
      .device = device_id,
      .stream = activity->streamId,
      .logMe = log_event,
    };
    logQueue_.push_back(entry);
  } else {
    log_event();
  }
}

void MuptiActivityProfiler::logDeferredEvents() {
  // Stream Wait Events tend to be noisy, only pass these events if
  // there was some GPU kernel/memcopy/memset observed on it in the trace window.
  for (const auto& entry: logQueue_) {
    if (seenDeviceStreams_.find({entry.device, entry.stream}) ==
        seenDeviceStreams_.end()) {
      VLOG(2) << "Skipping Event Sync as no kernels have run yet on stream = "
              << entry.stream;
    } else {
      entry.logMe();
    }
  }
}
#endif // HAS_MUPTI

inline void MuptiActivityProfiler::updateGpuNetSpan(
    const ITraceActivity& gpuOp) {
  if (!gpuOp.linkedActivity()) {
    VLOG(0) << "Missing linked activity";
    return;
  }
  const auto& it =
      clientActivityTraceMap_.find(gpuOp.linkedActivity()->correlationId());
  if (it == clientActivityTraceMap_.end()) {
    // No correlation id mapping?
    return;
  }
  TraceSpan& gpu_span = it->second->second;
  if (gpuOp.timestamp() < gpu_span.startTime || gpu_span.startTime == 0) {
    gpu_span.startTime = gpuOp.timestamp();
  }
  if ((gpuOp.timestamp() + gpuOp.duration()) > gpu_span.endTime) {
    gpu_span.endTime = gpuOp.timestamp() + gpuOp.duration();
  }
}

// I've observed occasional broken timestamps attached to GPU events...
void MuptiActivityProfiler::checkTimestampOrder(const ITraceActivity* act1) {
  // Correlated GPU runtime activity cannot
  // have timestamp greater than the GPU activity's
  const auto& it = correlatedMusaActivities_.find(act1->correlationId());
  if (it == correlatedMusaActivities_.end()) {
    correlatedMusaActivities_.insert({act1->correlationId(), act1});
    return;
  }

  // Activities may be appear in the buffers out of order.
  // If we have a runtime activity in the map, it should mean that we
  // have a GPU activity passed in, and vice versa.
  const ITraceActivity* act2 = it->second;
  if (act2->type() == ActivityType::PRIVATEUSE1_RUNTIME) {
    // Buffer is out-of-order.
    // Swap so that runtime activity is first for the comparison below.
    std::swap(act1, act2);
  }
  if (act1->timestamp() > act2->timestamp()) {
    LOG_FIRST_N(WARNING, 10) << "GPU op timestamp (" << act2->timestamp()
                             << ") < runtime timestamp (" << act1->timestamp() << ") by "
                             << act1->timestamp() - act2->timestamp() << "us"
                             << " Name: " << act2->name() << " Device: " << act2->deviceId()
                             << " Stream: " << act2->resourceId();
    ecs_.gpu_and_cpu_op_out_of_order++;
  }
}

const ITraceActivity* MuptiActivityProfiler::linkedActivity(
    int32_t correlationId,
    const std::unordered_map<int64_t, int64_t>& correlationMap) {
  const auto& it = correlationMap.find(correlationId);
  if (it != correlationMap.end()) {
    const auto& it2 = activityMap_.find(it->second);
    if (it2 != activityMap_.end()) {
      return it2->second;
    }
  }
  return nullptr;
}


inline void MuptiActivityProfiler::handleGpuActivity(
    const ITraceActivity& act,
    ActivityLogger* logger) {
  if (outOfRange(act)) {
    return;
  }
  checkTimestampOrder(&act);
  VLOG(2) << act.correlationId() << ": " << act.name();

  recordStream(act.deviceId(), act.resourceId(), "");
  seenDeviceStreams_.insert({act.deviceId(), act.resourceId()});

  act.log(*logger);
  setGpuActivityPresent(true);
  updateGpuNetSpan(act);
  if (derivedConfig_->profileActivityTypes().count(
          ActivityType::GPU_USER_ANNOTATION)) {
    const auto& it = userCorrelationMap_.find(act.correlationId());
    if (it != userCorrelationMap_.end()) {
      const auto& it2 = activityMap_.find(it->second);
      if (it2 != activityMap_.end()) {
        recordStream(act.deviceId(), act.resourceId(), "context");
        gpuUserEventMap_.insertOrExtendEvent(*it2->second, act);
      }
    }
  }
}

#ifdef HAS_MUPTI
template <class T>
inline void MuptiActivityProfiler::handleGpuActivity(
    const T* act,
    ActivityLogger* logger) {
  const ITraceActivity* linked =
      linkedActivity(act->correlationId, cpuCorrelationMap_);
  const auto& gpu_activity =
      traceBuffers_->addActivityWrapper(GpuActivity<T>(act, linked));
  handleGpuActivity(gpu_activity, logger);
}

template <class T>
inline void updateCtxToDeviceId(const T* act) {
  if (ctxToDeviceId().count(act->contextId) == 0) {
    ctxToDeviceId()[act->contextId] = act->deviceId;
  }
}

uint32_t contextIdtoDeviceId(uint32_t contextId) {
  auto it = ctxToDeviceId().find(contextId);
  return it != ctxToDeviceId().end() ? it->second : 0;
}

void MuptiActivityProfiler::handleMuptiActivity(
    const MUpti_Activity* record,
    ActivityLogger* logger) {
  switch (record->kind) {
    case MUPTI_ACTIVITY_KIND_EXTERNAL_CORRELATION:
      handleCorrelationActivity(
          reinterpret_cast<const MUpti_ActivityExternalCorrelation*>(record));
      break;
    case MUPTI_ACTIVITY_KIND_RUNTIME:
      handleRuntimeActivity(
          reinterpret_cast<const MUpti_ActivityAPI*>(record), logger);
      break;
    case MUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL:
      //TODO(MUpti_ActivityKernel6->MUpti_ActivityKernel4)
      handleGpuActivity(
          reinterpret_cast<const MUpti_ActivityKernel6*>(record), logger);
      updateCtxToDeviceId(
          reinterpret_cast<const MUpti_ActivityKernel6*>(record));
      break;
    case MUPTI_ACTIVITY_KIND_SYNCHRONIZATION:
      handleMusaSyncActivity(
          reinterpret_cast<const MUpti_ActivitySynchronization*>(record), logger);
      break;
    case MUPTI_ACTIVITY_KIND_MUSA_EVENT:
      handleMusaEventActivity(
          reinterpret_cast<const MUpti_ActivityMusaEvent*>(record));
      break;
    case MUPTI_ACTIVITY_KIND_MEMCPY:
      handleGpuActivity(
          reinterpret_cast<const MUpti_ActivityMemcpy*>(record), logger);
      break;
    case MUPTI_ACTIVITY_KIND_MEMCPY2:
      handleGpuActivity(
          reinterpret_cast<const MUpti_ActivityMemcpy2*>(record), logger);
      break;
    case MUPTI_ACTIVITY_KIND_MEMORY_ATOMIC:
      handleGpuActivity(
          reinterpret_cast<const MUpti_ActivityMemoryAtomic*>(record), logger);
      break;
    case MUPTI_ACTIVITY_KIND_MEMORY_ATOMIC_VALUE:
      handleGpuActivity(
          reinterpret_cast<const MUpti_ActivityMemoryAtomicValue*>(record), logger);
      break;
    case MUPTI_ACTIVITY_KIND_MEMSET:
      handleGpuActivity(
          reinterpret_cast<const MUpti_ActivityMemset*>(record), logger);
      break;
    case MUPTI_ACTIVITY_KIND_OVERHEAD:
      handleOverheadActivity(
          reinterpret_cast<const MUpti_ActivityOverhead*>(record), logger);
      break;
    case MUPTI_ACTIVITY_KIND_DRIVER:
      handleDriverActivity(
          reinterpret_cast<const MUpti_ActivityAPI*>(record), logger);
      break;
    default:
      LOG(WARNING) << "Unexpected activity type: " << record->kind;
      ecs_.unexepected_musa_events++;
      break;
  }
}
#endif // HAS_MUPTI

const ITraceActivity* MuptiActivityProfiler::cpuActivity(int32_t correlationId) {
    const auto& it2 = activityMap_.find(correlationId);
    return (it2 != activityMap_.end()) ? it2->second : nullptr;
}

void MuptiActivityProfiler::configureChildProfilers() {
  // If child profilers are enabled create profiler sessions
  int64_t start_time_ms =
      duration_cast<milliseconds>(
          derivedConfig_->profileStartTime().time_since_epoch())
          .count();
  for (auto& profiler : profilers_) {
    LOG(INFO) << "[Profiler = " << profiler->name() << "] "
              << "Evaluating whether to run child profiler." ;
    auto session = profiler->configure(
        start_time_ms,
        derivedConfig_->profileDuration().count(),
        derivedConfig_->profileActivityTypes(),
        *config_);
    if (session) {
      LOG(INFO) << "[Profiler = " << profiler->name() << "] "
                << "Running child profiler " << profiler->name() << " for "
                << derivedConfig_->profileDuration().count() << " ms";
      sessions_.push_back(std::move(session));
    } else {
      LOG(INFO) << "[Profiler = " << profiler->name() << "] "
                << "Not running child profiler.";
    }
  }
}

void MuptiActivityProfiler::configure(
    const Config& config,
    const time_point<system_clock>& now) {
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  if (isActive()) {
    LOG(WARNING) << "MuptiActivityProfiler already busy, terminating";
    return;
  }

  config_ = config.clone();

  // Ensure we're starting in a clean state
  resetTraceData();

#if !USE_GOOGLE_LOG
  // Add a LoggerObserverCollector to collect all logs during the trace.
  loggerCollectorMetadata_ = std::make_unique<LoggerCollector>();
  Logger::addLoggerObserver(loggerCollectorMetadata_.get());
#endif // !USE_GOOGLE_LOG

  derivedConfig_.reset();
  derivedConfig_ = std::make_unique<ConfigDerivedState>(*config_);

  // Check if now is a valid time to start.
  if (!derivedConfig_->canStart(now)) {
    return;
  }

  if (LOG_IS_ON(INFO)) {
    config_->printActivityProfilerConfig(LIBKINETO_DBG_STREAM);
  }
  if (!cpuOnly_ && !libkineto::api().client()) {
    if (derivedConfig_->isProfilingByIteration()) {
      LOG(INFO) << "GPU-only tracing for " << config_->activitiesRunIterations()
                << " iterations";
    } else {
      LOG(INFO) << "GPU-only tracing for "
                << config_->activitiesDuration().count() << "ms";
    }
  }

  // Set useful metadata into the logger.
  LOGGER_OBSERVER_SET_TRACE_DURATION_MS(config_->activitiesDuration().count());
  LOGGER_OBSERVER_SET_TRACE_ID(config_->requestTraceID());
  LOGGER_OBSERVER_SET_GROUP_TRACE_ID(config_->requestGroupTraceID());
  if (!config_->requestTraceID().empty()) {
    addMetadata("trace_id", "\"" + config_->requestTraceID() + "\"");
  }

#if defined(HAS_MUPTI)
  if (!cpuOnly_) {
    // Enabling MUPTI activity tracing incurs a larger perf hit at first,
    // presumably because structures are allocated and initialized, callbacks
    // are activated etc. After a while the overhead decreases and stabilizes.
    // It's therefore useful to perform some warmup before starting recording.
    LOG(INFO) << "Enabling GPU tracing with max MUPTI buffer size "
              << config_->activitiesMaxGpuBufferSize() / 1024 / 1024 << "MB)";
    mupti_.setMaxBufferSize(config_->activitiesMaxGpuBufferSize());

    time_point<system_clock> timestamp;
    if (VLOG_IS_ON(1)) {
      timestamp = system_clock::now();
    }
#ifdef HAS_MUPTI
#pragma message("---HAS_MUPTI is defined (MUPTI support is enabled)")
    mupti_.enableMuptiActivities(
        config_->selectedActivityTypes(), config_->perThreadBufferEnabled());
#else
#pragma message("---HAS_MUPTI is NOT defined (MUPTI support is disabled)")
    mupti_.enableActivities(config_->selectedActivityTypes());
#endif
    if (VLOG_IS_ON(1)) {
      auto t2 = system_clock::now();
      addOverheadSample(
          setupOverhead_, duration_cast<microseconds>(t2 - timestamp).count());
    }
  }
#endif // HAS_MUPTI

  if (profilers_.size() > 0) {
    configureChildProfilers();
  }

  if (libkineto::api().client()) {
    libkineto::api().client()->prepare(
      config_->isReportInputShapesEnabled(),
      config_->isProfileMemoryEnabled(),
      config_->isWithStackEnabled(),
      config_->isWithFlopsEnabled(),
      config_->isWithModulesEnabled());
  }

  if (derivedConfig_->isProfilingByIteration()) {
    LOG(INFO) << "Tracing starting on iteration = "
              << derivedConfig_->profileStartIteration();
    LOG(INFO) << "Tracing will end on iteration = "
              << derivedConfig_->profileEndIteration();
  } else {
    LOG(INFO) << "Tracing starting in "
              << duration_cast<seconds>(
                     derivedConfig_->profileStartTime() - now)
                     .count()
              << "s";
    LOG(INFO) << "Tracing will end in "
              << duration_cast<seconds>(derivedConfig_->profileEndTime() - now)
                     .count()
              << "s";
  }

  traceBuffers_ = std::make_unique<ActivityBuffers>();
  captureWindowStartTime_ = captureWindowEndTime_ = 0;
  currentRunloopState_ = RunloopState::Warmup;
}

bool MuptiActivityProfiler::getCollectTraceState() {
  std::lock_guard<std::recursive_mutex> guard(collectTraceStateMutex_);
  return isCollectingTrace;
}

void MuptiActivityProfiler::collectTrace(
    bool collection_done,
    const std::chrono::time_point<std::chrono::system_clock>& now) {
  if (libkineto::api().client()) {
    libkineto::api().client()->stop();
  }

#if defined(HAS_MUPTI) || defined(HAS_ROCTRACER)
  if (mupti_.stopCollection) {
    ecs_.mupti_stopped_early = mupti_.stopCollection;
    LOG(ERROR)
        << "State: CollectTrace stopped by MUPTI. (Buffer size configured is "
        << config_->activitiesMaxGpuBufferSize() / 1024 / 1024 << "MB)";
  }
#endif // HAS_MUPTI || HAS_ROCTRACER
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  stopTraceInternal(now);
  VLOG_IF(0, collection_done) << "Reached profile end time";
  UST_LOGGER_MARK_COMPLETED(kCollectionStage);
}

void MuptiActivityProfiler::ensureCollectTraceDone() {
  if (collectTraceThread_ && collectTraceThread_->joinable()) {
    collectTraceThread_->join();
    collectTraceThread_.reset(nullptr);
  }
}
void MuptiActivityProfiler::toggleCollectionDynamic(const bool enable) {
#ifdef HAS_MUPTI
  if (enable) {
    mupti_.enableMuptiActivities(
        derivedConfig_->profileActivityTypes(),
        derivedConfig_->isPerThreadBufferEnabled());
  } else {
    mupti_.disableMuptiActivities(derivedConfig_->profileActivityTypes());
  }
#endif
}

void MuptiActivityProfiler::startTraceInternal(
    const time_point<system_clock>& now) {
  captureWindowStartTime_ = libkineto::timeSinceEpoch(now);
  VLOG(0) << "Warmup -> CollectTrace";
  for (auto& session : sessions_) {
    LOG(INFO) << "Starting child profiler session";
    session->start();
  }
  currentRunloopState_ = RunloopState::CollectTrace;
}

void MuptiActivityProfiler::stopTraceInternal(
    const time_point<system_clock>& now) {
  captureWindowEndTime_ = libkineto::timeSinceEpoch(now);
#if defined(HAS_MUPTI)
  if (!cpuOnly_) {
    time_point<system_clock> timestamp;
    if (VLOG_IS_ON(1)) {
      timestamp = system_clock::now();
    }
#ifdef HAS_MUPTI
    mupti_.disableMuptiActivities(derivedConfig_->profileActivityTypes());
#else
    mupti_.disableActivities(derivedConfig_->profileActivityTypes());
#endif // HAS_MUPTI
    if (VLOG_IS_ON(1)) {
      auto t2 = system_clock::now();
      addOverheadSample(
          setupOverhead_, duration_cast<microseconds>(t2 - timestamp).count());
    }
  }
#endif // HAS_MUPTI

  if (currentRunloopState_ == RunloopState::CollectTrace) {
    VLOG(0) << "CollectTrace -> ProcessTrace";
  } else {
    LOG(WARNING) << "Called stopTrace with state == "
                 << static_cast<std::underlying_type<RunloopState>::type>(
                        currentRunloopState_.load());
  }
  for (auto& session : sessions_) {
    LOG(INFO) << "Stopping child profiler session";
    session->stop();
  }
  currentRunloopState_ = RunloopState::ProcessTrace;
}

void MuptiActivityProfiler::resetInternal() {
  resetTraceData();
  currentRunloopState_ = RunloopState::WaitForRequest;
}

const time_point<system_clock> MuptiActivityProfiler::performRunLoopStep(
    const time_point<system_clock>& now,
    const time_point<system_clock>& nextWakeupTime,
    int64_t currentIter) {
  auto new_wakeup_time = nextWakeupTime;
  bool warmup_done = false, collection_done = false;

  VLOG_IF(1, currentIter >= 0)
      << "Run loop on application step(), iteration = " << currentIter;

  switch (currentRunloopState_) {
    case RunloopState::WaitForRequest:
      VLOG(1) << "State: WaitForRequest";
      // Nothing to do
      break;

    case RunloopState::Warmup:
      VLOG(1) << "State: Warmup";
      warmup_done = derivedConfig_->isWarmupDone(now, currentIter);
#if defined(HAS_MUPTI)
      // Flushing can take a while so avoid doing it close to the start time
      if (!cpuOnly_ && currentIter < 0 &&
          (derivedConfig_->isProfilingByIteration() ||
           nextWakeupTime < derivedConfig_->profileStartTime())) {
        mupti_.clearActivities();
      }

      if (mupti_.stopCollection) {
        // Go to process trace to clear any outstanding buffers etc
        std::lock_guard<std::recursive_mutex> guard(mutex_);
        stopTraceInternal(now);
        resetInternal();
        LOG(ERROR)
            << "State: Warmup stopped by MUPTI. (Buffer size configured is "
            << config_->activitiesMaxGpuBufferSize() / 1024 / 1024 << "MB)";
        UST_LOGGER_MARK_COMPLETED(kWarmUpStage);
        VLOG(0) << "Warmup -> WaitForRequest";
        break;
      }
#endif // HAS_MUPTI

      if (warmup_done) {
        UST_LOGGER_MARK_COMPLETED(kWarmUpStage);
        if (!derivedConfig_->isProfilingByIteration() &&
            (now > derivedConfig_->profileStartTime() + milliseconds(10))) {
          LOG(INFO) << "Tracing started "
                    << duration_cast<milliseconds>(
                           now - derivedConfig_->profileStartTime())
                           .count()
                    << "ms late!";
        } else {
          LOG(INFO) << "Tracing started";
        }
        startTrace(now);
        if (libkineto::api().client()) {
          libkineto::api().client()->start();
        }
        if (nextWakeupTime > derivedConfig_->profileEndTime()) {
          new_wakeup_time = derivedConfig_->profileEndTime();
        }
      } else if (nextWakeupTime > derivedConfig_->profileStartTime()) {
        new_wakeup_time = derivedConfig_->profileStartTime();
      }

      break;

    case RunloopState::CollectTrace:
      VLOG(1) << "State: CollectTrace";
      collection_done = derivedConfig_->isCollectionDone(now, currentIter);

      if (collection_done
#if defined(HAS_MUPTI)
          || mupti_.stopCollection
#endif // HAS_MUPTI
      ){
        // Update runloop state first to prevent further updates to shared state
        LOG(INFO) << "Tracing complete.";
        VLOG_IF(1, currentIter >= 0)
            << "This state change was invoked by application's step() call";

        // currentIter >= 0 means this is called from the step() api of
        // the profile in pytorch main thread, it should be executed in another
        // thread in case pytorch main thread is blocked
        if (currentIter >= 0) {
          // if collectTraceThread_ is already running, there's no need to
          // execute collectTrace twice.
          // Do not call collectTrace when profilerThread_ is collecting Trace.
          // Otherwise, libkineto::api().client()->stop will be called twice,
          // which leads to an unrecoverable ::c10:Error at
          // disableProfiler
          if (!collectTraceThread_ && !getCollectTraceState()) {
            std::lock_guard<std::recursive_mutex> guard(mutex_);
            collectTraceThread_ = std::make_unique<std::thread>(
                &MuptiActivityProfiler::collectTrace,
                this,
                collection_done,
                now);
          }
          break;
        }
        // this is executed in profilerThread_
        {
          std::lock_guard<std::recursive_mutex> guard(collectTraceStateMutex_);
          isCollectingTrace = true;
        }
        collectTrace(collection_done, now);
        {
          std::lock_guard<std::recursive_mutex> guard(collectTraceStateMutex_);
          isCollectingTrace = false;
        }

      } else if (derivedConfig_->isProfilingByIteration()) {
        // nothing to do here
      } else if (now < derivedConfig_->profileEndTime() &&
                 derivedConfig_->profileEndTime() < nextWakeupTime) {
        new_wakeup_time = derivedConfig_->profileEndTime();
      }

      break;

    case RunloopState::ProcessTrace:
      VLOG(1) << "State: ProcessTrace";
      // skip this state transition if it called from the step() api
      // of the profiler.
      // else it could lead to a race between the profiler thread and an
      // application thread calling step()
      if (currentIter >= 0) {
        return new_wakeup_time;
      }

      // Before processing, we should wait for collectTrace thread to be done.
      ensureCollectTraceDone();

      // FIXME: Probably want to allow interruption here
      // for quickly handling trace request via synchronous API
      std::lock_guard<std::recursive_mutex> guard(mutex_);
      processTraceInternal(*logger_);
      UST_LOGGER_MARK_COMPLETED(kPostProcessingStage);
      resetInternal();
      VLOG(0) << "ProcessTrace -> WaitForRequest";
      break;
  }

  return new_wakeup_time;
}

void MuptiActivityProfiler::finalizeTrace(const Config& config, ActivityLogger& logger) {
  LOG(INFO) << "CPU Traces Recorded:";
  {
    for (const auto& it : iterationCountMap_) {
      LOG(INFO) << it.first << ": " << it.second << " span(s) recorded";
    }
    iterationCountMap_.clear();
  }

  // Process names
  int32_t pid = processId();
  string process_name = processName(pid);
  if (!process_name.empty()) {
    logger.handleDeviceInfo(
        {pid, pid, process_name, "CPU"}, captureWindowStartTime_);
    if (!cpuOnly_) {
      // Usually, GPU events use device id as pid (0-7).
      // In some cases, CPU sockets are numbered starting from 0.
      // In the worst case, 8 CPU sockets + 8 GPUs, so the max GPU ID is 15.
      constexpr int kMaxGpuID = 15;
      // sortIndex is gpu + kExceedMaxPid to put GPU tracks at the bottom
      // of the trace timelines.
      for (int gpu = 0; gpu <= kMaxGpuID; gpu++) {
        logger.handleDeviceInfo(
            {gpu, gpu + kExceedMaxPid, process_name, fmt::format("GPU {}", gpu)},
            captureWindowStartTime_);
      }
    }
  }

  // Thread & stream info
  for (auto pair : resourceInfo_) {
    const auto& resource = pair.second;
    logger.handleResourceInfo(resource, captureWindowStartTime_);
  }

  for (auto &session : sessions_){
    auto device_info = session->getDeviceInfo();
    if (device_info != nullptr){
      logger.handleDeviceInfo(*device_info, captureWindowStartTime_);
    }

    auto resource_infos = session->getResourceInfos();
    for (auto resource_info : resource_infos){
      logger.handleResourceInfo(resource_info, captureWindowStartTime_);
    }
  }

  for (const auto& iterations : traceSpans_) {
    for (const auto& span_pair : iterations.second) {
      const TraceSpan& gpu_span = span_pair.second;
      if (gpu_span.opCount > 0) {
        logger.handleTraceSpan(gpu_span);
      }
    }
  }

#ifdef HAS_MUPTI
  // Overhead info
  overheadInfo_.push_back(ActivityLogger::OverheadInfo("MUPTI Overhead"));
  for(const auto& info : overheadInfo_) {
    logger.handleOverheadInfo(info, captureWindowStartTime_);
  }
#endif // HAS_MUPTI

  gpuUserEventMap_.logEvents(&logger);

  for (auto& session : sessions_){
    auto trace_buffer = session->getTraceBuffer();
    if (trace_buffer) {
      // Set child start time to profiling start time if not set
      if (trace_buffer->span.startTime == 0) {
        trace_buffer->span.startTime = captureWindowStartTime_;
      }
      traceBuffers_->cpu.push_back(std::move(trace_buffer));
    }
  }

  // Logger Metadata contains a map of LOGs collected in Kineto
  //   logger_level -> List of log lines
  // This will be added into the trace as metadata.
  std::unordered_map<std::string, std::vector<std::string>>
    loggerMD = getLoggerMetadata();
  logger.finalizeTrace(
      config, std::move(traceBuffers_), captureWindowEndTime_, loggerMD);
}

std::unordered_map<std::string, std::vector<std::string>>
MuptiActivityProfiler::getLoggerMetadata() {
  std::unordered_map<std::string, std::vector<std::string>> loggerMD;

#if !USE_GOOGLE_LOG
  // Save logs from LoggerCollector objects into Trace metadata.
  auto LoggerMDMap = loggerCollectorMetadata_->extractCollectorMetadata();
  for (auto& md : LoggerMDMap) {
    loggerMD[toString(md.first)] = md.second;
  }
#endif // !USE_GOOGLE_LOG
  return loggerMD;
}

void MuptiActivityProfiler::pushCorrelationId(uint64_t id) {
#ifdef HAS_MUPTI
  MuptiActivityApi::pushCorrelationID(id,
    MuptiActivityApi::CorrelationFlowType::Default);
#endif // HAS_MUPTI
  for (auto& session : sessions_) {
    session->pushCorrelationId(id);
  }
}

void MuptiActivityProfiler::popCorrelationId() {
#ifdef HAS_MUPTI
  MuptiActivityApi::popCorrelationID(
    MuptiActivityApi::CorrelationFlowType::Default);
#endif // HAS_MUPTI
  for (auto& session : sessions_) {
    session->popCorrelationId();
  }
}

void MuptiActivityProfiler::pushUserCorrelationId(uint64_t id) {
#ifdef HAS_MUPTI
  MuptiActivityApi::pushCorrelationID(id,
    MuptiActivityApi::CorrelationFlowType::User);
#endif // HAS_MUPTI
  for (auto& session : sessions_) {
    session->pushUserCorrelationId(id);
  }
}

void MuptiActivityProfiler::popUserCorrelationId() {
#ifdef HAS_MUPTI
  MuptiActivityApi::popCorrelationID(
    MuptiActivityApi::CorrelationFlowType::User);
#endif // HAS_MUPTI
  for (auto& session : sessions_) {
    session->popUserCorrelationId();
  }
}

void MuptiActivityProfiler::resetTraceData() {
#if defined(HAS_MUPTI)
  if (!cpuOnly_) {
    mupti_.clearActivities();
    mupti_.teardownContext();
  }
#endif // HAS_MUPTI
  activityMap_.clear();
  cpuCorrelationMap_.clear();
  correlatedMusaActivities_.clear();
  gpuUserEventMap_.clear();
  traceSpans_.clear();
  clientActivityTraceMap_.clear();
  seenDeviceStreams_.clear();
  logQueue_.clear();
  traceBuffers_ = nullptr;
  metadata_.clear();
  sessions_.clear();
  resourceOverheadCount_ = 0;
  ecs_ = ErrorCounts{};
#if !USE_GOOGLE_LOG
  Logger::removeLoggerObserver(loggerCollectorMetadata_.get());
#endif // !USE_GOOGLE_LOG
}


} // namespace KINETO_NAMESPACE
