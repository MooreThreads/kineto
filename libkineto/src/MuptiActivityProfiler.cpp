/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MuptiActivityProfiler.h"
#include <mupti.h>
#include <fmt/format.h>
#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ActivityBuffers.h"
#include "Config.h"
#include "MuptiActivity.h"
#include "MuptiActivityApi.h"
#include "MuptiCbidRegistry.h"
#include "MuptiTimestamp.h"
#include "Demangle.h"
#include "DeviceUtil.h"
#include "KernelRegistry.h"
#include "Logger.h"

using std::string;

namespace {

struct CtxEventPair {
  uint32_t ctx = 0;
  uint32_t eventId = 0;

  bool operator==(const CtxEventPair& other) const {
    return (this->ctx == other.ctx) && (this->eventId == other.eventId);
  }
};

struct CtxEventPairHash {
  std::size_t operator()(const CtxEventPair& c) const {
    return KINETO_NAMESPACE::detail::hash_combine(
        std::hash<uint32_t>()(c.ctx), std::hash<uint32_t>()(c.eventId));
  }
};

struct WaitEventInfo {
  // MUSA stream that the MUSA event was recorded on
  uint32_t stream;
  // Correlation ID of the musaEventRecord event
  uint32_t correlationId;
};

// Map (ctx, eventId) -> list of WaitEventInfo for all recorded events.
std::unordered_map<CtxEventPair, std::vector<WaitEventInfo>, CtxEventPairHash>&
waitEventMap() {
  static std::
      unordered_map<CtxEventPair, std::vector<WaitEventInfo>, CtxEventPairHash>
          waitEventMap_;
  return waitEventMap_;
}

// Map ctx -> deviceId
std::unordered_map<uint32_t, uint32_t>& ctxToDeviceId() {
  static std::unordered_map<uint32_t, uint32_t> ctxToDeviceId_;
  return ctxToDeviceId_;
}

} // namespace

namespace KINETO_NAMESPACE {

MuptiActivityProfiler::MuptiActivityProfiler(
    MuptiActivityApi& mupti,
    bool cpuOnly)
    : GenericActivityProfiler(cpuOnly), mupti_(mupti) {}

void MuptiActivityProfiler::recordGpuVersions(uint32_t muptiVersion) {
  int musaRuntimeVersion = 0;
  int musaDriverVersion = 0;
  MUSA_CALL(musaRuntimeGetVersion(&musaRuntimeVersion));
  MUSA_CALL(musaDriverGetVersion(&musaDriverVersion));
  LOG(INFO) << "MUSA versions. MUPTI: " << muptiVersion
            << "; Runtime: " << musaRuntimeVersion
            << "; Driver: " << musaDriverVersion;

  LOGGER_OBSERVER_ADD_PERSISTENT_METADATA(
      "mupti_version", std::to_string(muptiVersion));
  LOGGER_OBSERVER_ADD_PERSISTENT_METADATA(
      "musa_runtime_version", std::to_string(musaRuntimeVersion));
  LOGGER_OBSERVER_ADD_PERSISTENT_METADATA(
      "musa_driver_version", std::to_string(musaDriverVersion));
  addVersionMetadata("mupti_version", std::to_string(muptiVersion));
  addVersionMetadata(
      "musa_runtime_version", std::to_string(musaRuntimeVersion));
  addVersionMetadata("musa_driver_version", std::to_string(musaDriverVersion));
}

void MuptiActivityProfiler::setMaxGpuBufferSize(int64_t size) {
  mupti_.setMaxBufferSize(size);
}

void MuptiActivityProfiler::enableGpuTracing() {
  // @lint-ignore CLANGTIDY facebook-hte-std::call_once
  std::call_once(muptiInitializationOnce_, [this] {
    uint32_t muptiVersion = 0;
    muptiAvailable_ = mupti_.isAvailable(muptiVersion);
    if (muptiAvailable_) {
      recordGpuVersions(muptiVersion);
    } else {
      cpuOnly_ = true;
      VLOG(0) << "MUPTI unavailable; continuing with CPU-only profiling";
    }
  });
  if (!muptiAvailable_) {
    toggleState_.store(false);
    return;
  }

  configureMuptiTimestampSource(config().getTSCTimestampFlag());
  mupti_.enableMuptiActivities(
      derivedConfig_->profileActivityTypes(),
      derivedConfig_->isPerThreadBufferEnabled());
}

void MuptiActivityProfiler::disableGpuTracing() {
  if (!muptiAvailable_) {
    return;
  }
  mupti_.disableMuptiActivities(derivedConfig_->profileActivityTypes());
}

void MuptiActivityProfiler::clearGpuActivities() {
  if (!muptiAvailable_) {
    return;
  }
  mupti_.clearActivities();
}

bool MuptiActivityProfiler::isGpuCollectionStopped() const {
  return mupti_.stopCollection;
}

void MuptiActivityProfiler::synchronizeGpuDevice() {
  if (!muptiAvailable_) {
    return;
  }
  MUSA_CALL(musaDeviceSynchronize());
  mupti_.flushActivities();
}

void MuptiActivityProfiler::pushCorrelationIdImpl(
    uint64_t id,
    CorrelationFlowType type) {
  MuptiActivityApi::CorrelationFlowType muptiType =
      (type == CorrelationFlowType::User)
      ? MuptiActivityApi::CorrelationFlowType::User
      : MuptiActivityApi::CorrelationFlowType::Default;
  MuptiActivityApi::pushCorrelationID(id, muptiType);
}

void MuptiActivityProfiler::popCorrelationIdImpl(CorrelationFlowType type) {
  MuptiActivityApi::CorrelationFlowType muptiType =
      (type == CorrelationFlowType::User)
      ? MuptiActivityApi::CorrelationFlowType::User
      : MuptiActivityApi::CorrelationFlowType::Default;
  MuptiActivityApi::popCorrelationID(muptiType);
}

void MuptiActivityProfiler::onResetTraceData() {
  mupti_.teardownContext();
  KernelRegistry::singleton()->clear();
  waitEventMap().clear();
  ctxToDeviceId().clear();
}

void MuptiActivityProfiler::onFinalizeTrace(
    [[maybe_unused]] const Config& config,
    ActivityLogger& logger) {
  // Overhead info
  overheadInfo_.emplace_back("MUPTI Overhead");
  for (const auto& info : overheadInfo_) {
    logger.handleOverheadInfo(info, captureWindowStartTime_);
  }
}

void MuptiActivityProfiler::processGpuActivities(ActivityLogger& logger) {
  VLOG(0) << "Retrieving GPU activity buffers";
  traceBuffers_->gpu = mupti_.activityBuffers();
  if (VLOG_IS_ON(1)) {
    addOverheadSample(flushOverhead_, mupti_.flushOverhead);
  }
  if (traceBuffers_->gpu) {
    // Pass 1: Preprocess all raw records to populate correlation, event, and
    // context lookup state.
    buildProcessingState(*traceBuffers_->gpu);

    // Pass 2: Materialize activities. buildProcessingState() has already
    // populated correlation, event, and context lookup state;
    // EXTERNAL_CORRELATION is a no-op in handleMuptiActivity.
    const auto count_and_size = mupti_.processActivities(
        *traceBuffers_->gpu, [this, &logger](const MUpti_Activity* record) {
          handleMuptiActivity(record, &logger);
        });
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
}

// Populate ctxToDeviceId from a record with (contextId, deviceId) fields.
template <class T>
static inline void updateCtxToDeviceId(const T* act) {
  if (!ctxToDeviceId().contains(act->contextId)) {
    ctxToDeviceId()[act->contextId] = act->deviceId;
  }
}

// P2P memcpy carries separate source and destination (context, device) pairs.
static inline void updateCtxToDeviceId(
    const MUpti_ActivityMemcpyPtoPType* act) {
  if (!ctxToDeviceId().contains(act->srcContextId)) {
    ctxToDeviceId()[act->srcContextId] = act->srcDeviceId;
  }
  if (!ctxToDeviceId().contains(act->dstContextId)) {
    ctxToDeviceId()[act->dstContextId] = act->dstDeviceId;
  }
}

// Insert a MUSA event record into waitEventMap(), keeping the per-key vector
// sorted by correlationId so getWaitEventInfo() can binary-search it.
static inline void updateWaitEventMap(const MUpti_ActivityMusaEventType* act) {
  auto key = CtxEventPair{act->contextId, act->eventId};
  std::vector<WaitEventInfo>& vec = waitEventMap()[key];
  auto pos = std::lower_bound(
      vec.begin(),
      vec.end(),
      act->correlationId,
      [](const WaitEventInfo& info, uint32_t val) {
        return info.correlationId < val;
      });
  vec.insert(pos, WaitEventInfo{act->streamId, act->correlationId});
}

void MuptiActivityProfiler::buildProcessingState(
    MuptiActivityBufferMap& buffers) {
  mupti_.processActivities(buffers, [this](const MUpti_Activity* record) {
    switch (record->kind) {
      case MUPTI_ACTIVITY_KIND_EXTERNAL_CORRELATION:
        handleCorrelationActivity(
            reinterpret_cast<const MUpti_ActivityExternalCorrelation*>(record));
        break;
      case MUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL:
        updateCtxToDeviceId(
            reinterpret_cast<const MUpti_ActivityKernelType*>(record));
        break;
      case MUPTI_ACTIVITY_KIND_MEMCPY:
        updateCtxToDeviceId(
            reinterpret_cast<const MUpti_ActivityMemcpyType*>(record));
        break;
      case MUPTI_ACTIVITY_KIND_MEMSET:
        updateCtxToDeviceId(
            reinterpret_cast<const MUpti_ActivityMemsetType*>(record));
        break;
      case MUPTI_ACTIVITY_KIND_MEMCPY2:
        updateCtxToDeviceId(
            reinterpret_cast<const MUpti_ActivityMemcpyPtoPType*>(record));
        break;
      case MUPTI_ACTIVITY_KIND_MUSA_EVENT:
        updateWaitEventMap(
            reinterpret_cast<const MUpti_ActivityMusaEventType*>(record));
        break;
      default:
        break;
    }
  });
}

inline void MuptiActivityProfiler::handleCorrelationActivity(
    const MUpti_ActivityExternalCorrelation* correlation) {
  if (correlation->externalKind == MUPTI_EXTERNAL_CORRELATION_KIND_CUSTOM0) {
    cpuCorrelationMap_[correlation->correlationId] = correlation->externalId;
  } else if (
      correlation->externalKind == MUPTI_EXTERNAL_CORRELATION_KIND_CUSTOM1) {
    userCorrelationMap_[correlation->correlationId] = correlation->externalId;
  } else {
    LOG(WARNING) << "Invalid MUpti_ActivityExternalCorrelation sent to "
                    "handleMuptiActivity";
    ecs_.invalid_external_correlation_events++;
  }
}

void MuptiActivityProfiler::handleRuntimeActivity(
    const MUpti_ActivityAPI* activity,
    ActivityLogger* logger) {
  if (MuptiCbidRegistry::instance().isBlocklisted(
          CallbackDomain::RUNTIME, activity->cbid)) {
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
  if (!isTrackedDriverCbid(*activity)) {
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

static std::optional<WaitEventInfo> getWaitEventInfo(
    uint32_t ctx,
    uint32_t eventId,
    uint32_t queryCorrelationId) {
  auto key = CtxEventPair{ctx, eventId};
  auto it = waitEventMap().find(key);
  if (it != waitEventMap().end()) {
    // Records are sorted by correlationId. Find the most recent record
    // that precedes the query (i.e., the last record with
    // correlationId < queryCorrelationId).
    const std::vector<WaitEventInfo>& vec = it->second;
    std::vector<WaitEventInfo>::const_iterator pos = std::upper_bound(
        vec.begin(),
        vec.end(),
        queryCorrelationId,
        [](uint32_t val, const WaitEventInfo& info) {
          return val < info.correlationId;
        });
    if (pos != vec.begin()) {
      return *std::prev(pos);
    }
  }
  return std::nullopt;
}

void MuptiActivityProfiler::handleMusaEventActivity(
    const MUpti_ActivityMusaEventType* activity,
    ActivityLogger* logger) {
  VLOG(2) << ": MUPTI_ACTIVITY_KIND_MUSA_EVENT"
          << " corrId=" << activity->correlationId
          << " eventId=" << activity->eventId
          << " streamId=" << activity->streamId
          << " contextId=" << activity->contextId;

  // Create and log the MUSA event activity
  const ITraceActivity* linked =
      linkedActivity(activity->correlationId, cpuCorrelationMap_);
  const auto& musa_event_activity =
      traceBuffers_->addActivityWrapper(MusaEventActivity(activity, linked));

  if (outOfRange(musa_event_activity)) {
    return;
  }

  auto device_id = contextIdtoDeviceId(activity->contextId);
  if (static_cast<int32_t>(activity->streamId) != -1) {
    recordStream(device_id, activity->streamId, "");
  } else {
    recordDevice(device_id);
  }

  VLOG(2) << "Logging MUSA event activity device = " << device_id
          << " stream = " << activity->streamId;
  musa_event_activity.log(*logger);
  setGpuActivityPresent(true);
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

  if (!config().activitiesMusaSyncWaitEvents() &&
      isWaitEventSync(activity->type)) {
    return;
  }

  auto device_id = contextIdtoDeviceId(activity->contextId);
  int32_t src_stream = -1;
  int32_t src_corrid = -1;
  if (isEventSync(activity->type)) {
    auto maybe_wait_event_info = getWaitEventInfo(
        activity->contextId, activity->musaEventId, activity->correlationId);
    if (maybe_wait_event_info) {
      src_stream = maybe_wait_event_info->stream;
      src_corrid = maybe_wait_event_info->correlationId;
    }
  }

  // Marshal the logging to a functor so we can defer it if needed.
  auto log_event =
      [activity, src_stream, src_corrid, device_id, logger, this]() {
        const ITraceActivity* linked =
            linkedActivity(activity->correlationId, this->cpuCorrelationMap_);
        const auto& musa_sync_activity =
            this->traceBuffers_->addActivityWrapper(
                MusaSyncActivity(activity, linked, src_stream, src_corrid));

        if (outOfRange(musa_sync_activity)) {
          return;
        }

        if (static_cast<int32_t>(activity->streamId) != -1) {
          recordStream(device_id, activity->streamId, "");
        } else {
          recordDevice(device_id);
        }
        VLOG(2) << "Logging sync event device = " << device_id
                << " stream = " << activity->streamId
                << " sync type = " << syncTypeString(activity->type);
        musa_sync_activity.log(*logger);
        setGpuActivityPresent(true);
      };

  if (isWaitEventSync(activity->type)) {
    // Stream Wait Events still need deferral so we can suppress waits on
    // streams that never show GPU work in the trace window.
    DeferredLogEntry entry;
    entry.device = device_id;
    entry.stream = activity->streamId;
    entry.logMe = log_event;

    logQueue_.push_back(entry);
  } else {
    log_event();
  }
}

void MuptiActivityProfiler::logDeferredEvents() {
  // Stream Wait Events tend to be noisy, only pass these events if
  // there was some GPU kernel/memcopy/memset observed on it in the trace
  // window.
  for (const auto& entry : logQueue_) {
    if (!seenDeviceStreams_.contains({entry.device, entry.stream})) {
      VLOG(2) << "Skipping Stream Wait Event on unseen stream = "
              << entry.stream;
    } else {
      entry.logMe();
    }
  }
}

template <class T>
inline void MuptiActivityProfiler::handleGpuActivity(
    const T* act,
    ActivityLogger* logger) {
  const ITraceActivity* linked =
      linkedActivity(act->correlationId, cpuCorrelationMap_);
  const auto& gpu_activity =
      traceBuffers_->addActivityWrapper(GpuActivity<T>(act, linked));
  GenericActivityProfiler::handleGpuActivity(gpu_activity, logger);
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
      // Populated in buildProcessingState().
      break;
    case MUPTI_ACTIVITY_KIND_RUNTIME:
      handleRuntimeActivity(
          reinterpret_cast<const MUpti_ActivityAPI*>(record), logger);
      break;
    case MUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL: {
      const MUpti_ActivityKernelType* kernel =
          reinterpret_cast<const MUpti_ActivityKernelType*>(record);
      // Register all kernels launches so we could correlate them with other
      // events.
      KernelRegistry::singleton()->recordKernel(
          kernel->deviceId, demangle(kernel->name), kernel->correlationId);
      handleGpuActivity(kernel, logger);
      break;
    }
    case MUPTI_ACTIVITY_KIND_SYNCHRONIZATION:
      handleMusaSyncActivity(
          reinterpret_cast<const MUpti_ActivitySynchronization*>(record),
          logger);
      break;
    case MUPTI_ACTIVITY_KIND_MUSA_EVENT:
      handleMusaEventActivity(
          reinterpret_cast<const MUpti_ActivityMusaEventType*>(record), logger);
      break;
    case MUPTI_ACTIVITY_KIND_MEMCPY:
      handleGpuActivity(
          reinterpret_cast<const MUpti_ActivityMemcpyType*>(record), logger);
      break;
    case MUPTI_ACTIVITY_KIND_MEMORY_ATOMIC:
      handleGpuActivity(
          reinterpret_cast<const MUpti_ActivityMemoryAtomic*>(record), logger);
      break;
    case MUPTI_ACTIVITY_KIND_MEMORY_ATOMIC_VALUE:
      handleGpuActivity(
          reinterpret_cast<const MUpti_ActivityMemoryAtomicValue*>(record), logger);
      break;
    case MUPTI_ACTIVITY_KIND_MEMORY_TRANSFER:
      handleGpuActivity(
          reinterpret_cast<const MUpti_ActivityMemoryTransfer*>(record), logger);
      break;
    case MUPTI_ACTIVITY_KIND_MEMSET:
      handleGpuActivity(
          reinterpret_cast<const MUpti_ActivityMemsetType*>(record), logger);
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

} // namespace KINETO_NAMESPACE
