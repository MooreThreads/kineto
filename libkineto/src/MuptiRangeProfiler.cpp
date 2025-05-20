/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <Logger.h>
#include <functional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// TODO(T90238193)
// @lint-ignore-every CLANGTIDY facebook-hte-RelativeInclude
#include "output_base.h"
#include "MuptiRangeProfiler.h"
#include "MuptiRangeProfilerConfig.h"
#include "Demangle.h"

namespace KINETO_NAMESPACE {

const ActivityType kProfActivityType = ActivityType::CUDA_PROFILER_RANGE;
const std::set<ActivityType> kSupportedActivities{kProfActivityType};

const std::string kProfilerName{"MuptiRangeProfiler"};

static IMuptiRBProfilerSessionFactory& getFactory() {
  static MuptiRBProfilerSessionFactory factory_;
  return factory_;
}

/* ----------------------------------------
 * Implement MuptiRangeProfilerSession
 * ----------------------------------------
 */

namespace {

MuptiProfilerPrePostCallback muptiProfilerPreRunCb;
MuptiProfilerPrePostCallback muptiProfilerPostRunCb;


/* Following are aliases to a set of MUPTI metrics that can be
 * used to derived measures like FLOPs etc.
 */
std::unordered_map<std::string, std::vector<std::string>> kDerivedMetrics = {
  {"kineto__musa_core_flops", {
    "smsp__sass_thread_inst_executed_op_dadd_pred_on.sum",
    "smsp__sass_thread_inst_executed_op_dfma_pred_on.sum",
    "smsp__sass_thread_inst_executed_op_dmul_pred_on.sum",
    "smsp__sass_thread_inst_executed_op_hadd_pred_on.sum",
    "smsp__sass_thread_inst_executed_op_hfma_pred_on.sum",
    "smsp__sass_thread_inst_executed_op_hmul_pred_on.sum",
    "smsp__sass_thread_inst_executed_op_fadd_pred_on.sum",
    "smsp__sass_thread_inst_executed_op_ffma_pred_on.sum",
    "smsp__sass_thread_inst_executed_op_fmul_pred_on.sum"}},
  {"kineto__tensor_core_insts", {
    "sm__inst_executed_pipe_tensor.sum"}},
};

} // namespace;


MuptiRangeProfilerSession::MuptiRangeProfilerSession(
    const Config& config, IMuptiRBProfilerSessionFactory& factory) {

  // MUPTI APIs can conflict with other monitoring systems like DCGM
  // or NSight / NVProf. The pre and post run hooks enable users to
  // potentially pause other tools like DCGM.
  // By the way consider adding some delay while using dcgmpause() so
  // the change takes effect inside the driver.
  if (muptiProfilerPreRunCb) {
    muptiProfilerPreRunCb();
  }

  const MuptiRangeProfilerConfig& mupti_config =
    MuptiRangeProfilerConfig::get(config);

  std::vector<std::string> mupti_metrics;
  const auto& requested_metrics = mupti_config.activitiesMuptiMetrics();

  for (const auto& metric : requested_metrics) {
    auto it = kDerivedMetrics.find(metric);
    if (it != kDerivedMetrics.end()) {
      // add all the fundamental metrics
      for (const auto& m : it->second) {
        mupti_metrics.push_back(m);
      }
    } else {
      mupti_metrics.push_back(metric);
    }
  }

  // Capture metrics per kernel implies using auto-range mode
  if (mupti_config.muptiProfilerPerKernel()) {
    rangeType_ = MUPTI_AutoRange;
    replayType_ = MUPTI_KernelReplay;
  }

  LOG(INFO) << "Configuring " << mupti_metrics.size()
            << " MUPTI metrics";

  int max_ranges = mupti_config.muptiProfilerMaxRanges();
  for (const auto& m : mupti_metrics) {
    LOG(INFO) << "\t" << m;
  }

  MuptiRangeProfilerOptions opts{
    .metricNames = mupti_metrics,
    .deviceId = 0,
    .maxRanges = max_ranges,
    .numNestingLevels = 1,
    .muContext = nullptr,
    .unitTest = false};

  for (auto device_id : MuptiRBProfilerSession::getActiveDevices()) {
    LOG(INFO) << "Init MUPTI range profiler on gpu = " << device_id
              << " max ranges = " << max_ranges;
    opts.deviceId = device_id;
    profilers_.push_back(factory.make(opts));
  }
}

void MuptiRangeProfilerSession::start() {
  for (auto& profiler: profilers_) {
    // user range or auto range
    profiler->asyncStartAndEnable(rangeType_, replayType_);
  }
}

void MuptiRangeProfilerSession::stop() {
  for (auto& profiler: profilers_) {
    profiler->disableAndStop();
  }
}

void MuptiRangeProfilerSession::addRangeEvents(
    const MuptiProfilerResult& result,
    const MuptiRBProfilerSession* profiler) {
  const auto& metricNames = result.metricNames;
  auto& activities = traceBuffer_.activities;
  bool use_kernel_names = false;
  int num_kernels = 0;

  if (rangeType_ == MUPTI_AutoRange) {
    use_kernel_names = true;
    num_kernels = profiler->getKernelNames().size();
    if (num_kernels != result.rangeVals.size()) {
      LOG(WARNING) << "Number of kernels tracked does not match the "
                   << " number of ranges collected"
                   << " kernel names size = " << num_kernels
                   << " vs ranges = " << result.rangeVals.size();
    }
  }

  // the actual times do not really matter here so
  // we can just split the total span up per range
  int64_t startTime = traceBuffer_.span.startTime,
          duration = traceBuffer_.span.endTime - startTime,
          interval = duration / result.rangeVals.size();

  int ridx = 0;
  for (const auto& measurement : result.rangeVals) {
    bool use_kernel_as_range = use_kernel_names && (ridx < num_kernels);
    traceBuffer_.emplace_activity(
        traceBuffer_.span,
        kProfActivityType,
        use_kernel_as_range ?
          demangle(profiler->getKernelNames()[ridx]) :
          measurement.rangeName
    );
    auto& event = activities.back();
    event->startTime = startTime + interval * ridx;
    event->endTime = startTime + interval * (ridx + 1);
    event->device = profiler->deviceId();

    // add metadata per counter
    for (int i = 0; i < metricNames.size(); i++) {
      event->addMetadata(metricNames[i], measurement.values[i]);
    }
    ridx++;
  }
}

void MuptiRangeProfilerSession::processTrace(ActivityLogger& logger) {
  if (profilers_.size() == 0) {
    LOG(WARNING) << "Nothing to report";
    return;
  }

  traceBuffer_.span = profilers_[0]->getProfilerTraceSpan();
  for (auto& profiler: profilers_) {
    bool verbose = VLOG_IS_ON(1);
    auto result = profiler->evaluateMetrics(verbose);

    LOG(INFO) << "Profiler Range data on gpu = " << profiler->deviceId();
    if (result.rangeVals.size() == 0) {
      LOG(WARNING) << "Skipping profiler results on gpu = "
                   << profiler->deviceId() << " as 0 ranges were found";
      continue;
    }
    addRangeEvents(result, profiler.get());
  }

  for (const auto& event : traceBuffer_.activities) {
    static_assert(
        std::is_same<
            std::remove_reference<decltype(event)>::type,
            const std::unique_ptr<GenericTraceActivity>>::value,
        "handleActivity is unsafe and relies on the caller to maintain not "
        "only lifetime but also address stability.");
    logger.handleActivity(*event);
  }

  LOG(INFO) << "MUPTI Range Profiler added " << traceBuffer_.activities.size()
            << " events";

  if (muptiProfilerPostRunCb) {
    muptiProfilerPostRunCb();
  }
}

std::vector<std::string> MuptiRangeProfilerSession::errors() {
  return {};
}

std::unique_ptr<DeviceInfo> MuptiRangeProfilerSession::getDeviceInfo() {
  return {};
}

std::vector<ResourceInfo> MuptiRangeProfilerSession::getResourceInfos() {
  return {};
}

/* ----------------------------------------
 * Implement MuptiRangeProfiler
 * ----------------------------------------
 */
MuptiRangeProfiler::MuptiRangeProfiler()
  : MuptiRangeProfiler(getFactory()) {}

MuptiRangeProfiler::MuptiRangeProfiler(IMuptiRBProfilerSessionFactory& factory)
  : factory_(factory) {}

void MuptiRangeProfiler::setPreRunCallback(
    MuptiProfilerPrePostCallback fn) {
  muptiProfilerPreRunCb = fn;
}

void MuptiRangeProfiler::setPostRunCallback(
    MuptiProfilerPrePostCallback fn) {
  muptiProfilerPostRunCb = fn;
}

const std::string& MuptiRangeProfiler::name() const {
  return kProfilerName;
}

const std::set<ActivityType>& MuptiRangeProfiler::availableActivities()
    const {
  return kSupportedActivities;
}

// TODO remove activity_types from this interface in the future
std::unique_ptr<IActivityProfilerSession> MuptiRangeProfiler::configure(
    const std::set<ActivityType>& /*activity_types*/,
    const Config& config) {
  const auto& activity_types_ = config.selectedActivityTypes();
  if (activity_types_.find(kProfActivityType) == activity_types_.end()) {
    return nullptr;
  }
  bool has_gpu_event_types = (
      activity_types_.count(ActivityType::GPU_MEMCPY) +
      activity_types_.count(ActivityType::GPU_MEMSET) +
      activity_types_.count(ActivityType::CONCURRENT_KERNEL)
    ) > 0;

  if (has_gpu_event_types) {
    LOG(WARNING) << kProfilerName << " cannot run in combination with"
                << " other musa activity profilers, please configure"
                << " with musa_profiler_range and optionally cpu_op/user_annotations";
    return nullptr;
  }

  return std::make_unique<MuptiRangeProfilerSession>(config, factory_);
}

std::unique_ptr<IActivityProfilerSession>
MuptiRangeProfiler::configure(
    int64_t /*ts_ms*/,
    int64_t /*duration_ms*/,
    const std::set<ActivityType>& activity_types,
    const Config& config) {
  return configure(activity_types, config);
};

/* ----------------------------------------
 * MuptiRangeProfilerInit :
 *    a small wrapper class that ensure the range profiler is created and
 *  initialized.
 * ----------------------------------------
 */
MuptiRangeProfilerInit::MuptiRangeProfilerInit() {
  // register config
  MuptiRangeProfilerConfig::registerFactory();

#ifdef HAS_MUPTI
  success = MuptiRBProfilerSession::staticInit();
#endif

  if (!success) {
    return;
  }

  // Register the activity profiler instance with libkineto api
  api().registerProfilerFactory([&]() {
    return std::make_unique<MuptiRangeProfiler>();
  });
}

} // namespace KINETO_NAMESPACE
