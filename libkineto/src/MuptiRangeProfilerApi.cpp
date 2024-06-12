/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <stdio.h>
#include <stdlib.h>
#ifdef HAS_MUPTI
#include <mupti.h>
// #include <nvperf_host.h>
#endif // HAS_MUPTI
#include <mutex>
#include <unordered_map>

#ifdef HAS_MUPTI
#include "mupti_call.h"
#endif

#include "time_since_epoch.h"
#include "Logger.h"
#include "Demangle.h"

// TODO(T90238193)
// @lint-ignore-every CLANGTIDY facebook-hte-RelativeInclude
#include "MuptiRangeProfilerApi.h"

#if HAS_MUPTI_RANGE_PROFILER
#include <mupti.h>
// #include <nvperf_host.h>
#include "mupti_call.h"
#endif // HAS_MUPTI_RANGE_PROFILER

namespace KINETO_NAMESPACE {

TraceSpan MuptiRBProfilerSession::getProfilerTraceSpan() {
  return TraceSpan(
      timeSinceEpoch(profilerStartTs_),
      timeSinceEpoch(profilerStopTs_),
      "__mupti_profiler__"
  );
}

#if HAS_MUPTI_RANGE_PROFILER
constexpr char kRootUserRangeName[] = "__profile__";
constexpr int kCallbacksCountToFlush = 500;

// Should we set Counter availability image ourselves?
// Disabled this right now as this call conflicts with DCGM
// It is not clear why it should conflict except it being a profiler API call
//  TODO Revisit
constexpr bool kSetCounterAvail = false;

// Shared state to track one Mupti Profiler API per Device
namespace {
// per device profiler maps
std::unordered_map<uint32_t, MuptiRBProfilerSession*> profiler_map;
std::unordered_map<uint32_t, bool> enable_flag;
std::unordered_map<uint32_t, bool> disable_flag;

std::mutex contextMutex_;
std::unordered_map<MUcontext, int> ctx_to_dev;
std::set<uint32_t> active_devices;
}

// forward declarations
void __trackMusaCtx(MUcontext ctx, uint32_t device_id, MUpti_CallbackId cbid);
void __trackMusaKernelLaunch(MUcontext ctx, const char* kernelName);

/// Helper functions

// Available raw counters
std::vector<uint8_t> getCounterAvailiability(MUcontext muContext) {
  std::vector<uint8_t> counterAvailabilityImage;
  MUpti_Profiler_GetCounterAvailability_Params getCounterAvailabilityParams = {
      MUpti_Profiler_GetCounterAvailability_Params_STRUCT_SIZE, nullptr};
  getCounterAvailabilityParams.ctx = MUcontext;
  MUPTI_CALL(
      muptiProfilerGetCounterAvailability(&getCounterAvailabilityParams));

  counterAvailabilityImage.clear();
  counterAvailabilityImage.resize(
      getCounterAvailabilityParams.counterAvailabilityImageSize);

  getCounterAvailabilityParams.pCounterAvailabilityImage =
      counterAvailabilityImage.data();
  MUPTI_CALL(
      muptiProfilerGetCounterAvailability(&getCounterAvailabilityParams));

  return counterAvailabilityImage;
}

std::string getChipName(int deviceId) {
  // Get chip name for the musa device
  MUpti_Device_GetChipName_Params getChipNameParams = {
      MUpti_Device_GetChipName_Params_STRUCT_SIZE, nullptr};

  getChipNameParams.deviceIndex = deviceId;
  MUPTI_CALL(muptiDeviceGetChipName(&getChipNameParams));

  return getChipNameParams.pChipName;
}

inline uint32_t getDevID(MUcontext ctx) {
  uint32_t device_id = UINT32_MAX;
  MUPTI_CALL(muptiGetDeviceId(ctx, &device_id));
  if (device_id == UINT32_MAX) {
    LOG(ERROR) << "Could not determine dev id for = " << ctx;
  }
  return device_id;
}

// We use MUPTI Callback functions in three ways :
//   1. Track musa contexts and maintain a list of active GPUs to profile
//   2. Callbacks on kernel launches to track the name of automatic
//      ranges that correspond to names of kernels
//   3. Lastly MUPTI range profiler has to be enabled on the same thread executing
//      the MUSA kernels. We use Callbacks to enable the profiler
//      asynchronously from another thread.

void disableKernelCallbacks();

void trackMusaCtx(
    MUpti_CallbackDomain /*domain*/,
    MUpti_CallbackId cbid,
    const MUpti_CallbackData* cbInfo) {
  auto *d = reinterpret_cast<const MUpti_ResourceData*>(cbInfo);
  auto ctx = d->context;
  uint32_t device_id = getDevID(ctx);

  if (device_id == UINT32_MAX) {
    return;
  }

  __trackMusaCtx(ctx, device_id, cbid);
}

void __trackMusaCtx(MUcontext ctx, uint32_t device_id, MUpti_CallbackId cbid) {
  std::lock_guard<std::mutex> g(contextMutex_);
  if (cbid == MUPTI_CBID_RESOURCE_CONTEXT_CREATED) {
    VLOG(0) << "MUPTI Profiler observed MUSA Context created = "
            << ctx << " device id = " << device_id;
    active_devices.insert(device_id);
  //  TODO Revisit
#if 0
    if constexpr (kSetCounterAvail) {
      if (active_devices.size() == 1) {
      MuptiRBProfilerSession::setCounterAvailabilityImage(
          getCounterAvailiability(ctx));
      }
    }
#endif
    ctx_to_dev[ctx] = device_id;

  } else if (cbid == MUPTI_CBID_RESOURCE_CONTEXT_DESTROY_STARTING) {
    VLOG(0) << "MUPTI Profiler observed MUSA Context destroyed = "
            << ctx << " device id = " << device_id;
    auto it = active_devices.find(device_id);
    if (it != active_devices.end()) {
      active_devices.erase(it);
      ctx_to_dev.erase(ctx);
    }
  }
}

void trackMusaKernelLaunch(
    MUpti_CallbackDomain /*domain*/,
    MUpti_CallbackId /*cbid*/,
    const MUpti_CallbackData* cbInfo) {
  VLOG(1) << " Trace : Callback name = "
          << (cbInfo->symbolName ?  cbInfo->symbolName: "")
          << " context ptr = " << cbInfo->context;
  auto ctx = cbInfo->context;
  // should be in MUPTI_API_ENTER call site
  if (cbInfo->callbackSite != MUPTI_API_ENTER) {
    return;
  }
  __trackMusaKernelLaunch(ctx, cbInfo->symbolName);
}

void __trackMusaKernelLaunch(
    MUcontext ctx,
    const char* kernelName) {
  VLOG(0) << " Tracking kernel name = " << (kernelName ? kernelName : "")
          << " context ptr = " << ctx;

  uint32_t device_id = 0;
  auto it = ctx_to_dev.find(ctx);
  if (it == ctx_to_dev.end()) {
    // Warning here could be too noisy
    VLOG(0) << " Could not find corresponding device to ctx = " << ctx;
    return;
  } else {
    device_id = it->second;
  }

  auto pit = profiler_map.find(device_id);
  if (pit == profiler_map.end() || pit->second == nullptr) {
    return;
  }
  auto profiler = pit->second;

  if (enable_flag[device_id]) {
    LOG(INFO) << "Callback handler is enabling mupti profiler";
    profiler->startAndEnable();
    enable_flag[device_id] = false;

  } else if (disable_flag[device_id]) {
    LOG(INFO) << "Callback handler is disabling mupti profiler";
    profiler->disableAndStop();
    return;
  }

  if (profiler->curRange_ == MUPTI_AutoRange) {
    profiler->logKernelName(kernelName ? kernelName : "__missing__");
  }

  /* TODO add per kernel time logging
  if (measure_per_kernel) {
    profiler->kernelStartTs_.push_back(
        std::chrono::high_resolution_clock::now());
  }
  */

  // periodically flush profiler data from GPU
  if (profiler->numCallbacks_ % kCallbacksCountToFlush == 0) {
    profiler->flushCounterData();
  }
  profiler->numCallbacks_++;
}

void enableKernelCallbacks() {
  auto cbapi = MuptiCallbackApi::singleton();
  bool status = cbapi->enableCallback(
      MUPTI_CB_DOMAIN_RUNTIME_API,
      MUPTI_RUNTIME_TRACE_CBID_musaLaunchKernel_v7000);
  if (!status) {
    LOG(WARNING) << "MUPTI Range Profiler unable to "
                 << "enable musa kernel launch callback";
    return;
  }
  LOG(INFO) << "MUPTI Profiler kernel callbacks enabled";
}

void disableKernelCallbacks() {
  auto cbapi = MuptiCallbackApi::singleton();
  bool status = cbapi->disableCallback(
      MUPTI_CB_DOMAIN_RUNTIME_API,
      MUPTI_RUNTIME_TRACE_CBID_musaLaunchKernel_v7000);
  if (!status) {
    LOG(WARNING) << "MUPTI Range Profiler unable to "
                 << "disable musa kernel launch callback";
    return;
  }
  LOG(INFO) << "MUPTI Profiler kernel callbacks disabled";
}

// static
std::set<uint32_t> MuptiRBProfilerSession::getActiveDevices() {
  std::lock_guard<std::mutex> g(contextMutex_);
  return active_devices;
}

// static
bool MuptiRBProfilerSession::initMupti() {
  // This call will try to load the libnvperf_host.so library and is known
  // to break address sanitizer based flows. Only call this init
  // when you plan to use MUPTI range profiler
  MUpti_Profiler_Initialize_Params profilerInitializeParams = {
      MUpti_Profiler_Initialize_Params_STRUCT_SIZE, nullptr};
  MUptiResult status = MUPTI_CALL_NOWARN(
      muptiProfilerInitialize(&profilerInitializeParams));
  return (status == MUPTI_SUCCESS);
}

// static
void MuptiRBProfilerSession::deInitMupti() {
  MUpti_Profiler_DeInitialize_Params profilerDeInitializeParams = {
      MUpti_Profiler_DeInitialize_Params_STRUCT_SIZE, nullptr};
  MUPTI_CALL(muptiProfilerDeInitialize(&profilerDeInitializeParams));
}

// static
bool MuptiRBProfilerSession::staticInit() {
  // Register MUPTI callbacks
  auto cbapi = MuptiCallbackApi::singleton();
  MUpti_CallbackDomain domain = MUPTI_CB_DOMAIN_RESOURCE;
  bool status = cbapi->registerCallback(
      domain, MuptiCallbackApi::RESOURCE_CONTEXT_CREATED, trackMusaCtx);
  status = status && cbapi->registerCallback(
      domain, MuptiCallbackApi::RESOURCE_CONTEXT_DESTROYED, trackMusaCtx);
  status = status && cbapi->enableCallback(
      domain, MUPTI_CBID_RESOURCE_CONTEXT_CREATED);
  status = status && cbapi->enableCallback(
      domain, MUPTI_CBID_RESOURCE_CONTEXT_DESTROY_STARTING);

  if (!status) {
    LOG(WARNING) << "MUPTI Range Profiler unable to attach musa context "
                 << "create and destroy callbacks";
    MUPTI_CALL(cbapi->getMuptiStatus());
    return false;
  }

  domain = MUPTI_CB_DOMAIN_RUNTIME_API;
  status = cbapi->registerCallback(
      domain, MuptiCallbackApi::MUSA_LAUNCH_KERNEL, trackMusaKernelLaunch);

  if (!status) {
    LOG(WARNING) << "MUPTI Range Profiler unable to attach musa kernel "
                 << "launch callback";
    return false;
  }

  return true;
}

// static
std::vector<uint8_t>& MuptiRBProfilerSession::counterAvailabilityImage() {
  static std::vector<uint8_t> counterAvailabilityImage_;
  return counterAvailabilityImage_;
}


// Setup the profiler sessions
MuptiRBProfilerSession::MuptiRBProfilerSession(
    const MuptiRangeProfilerOptions& opts)
    : metricNames_(opts.metricNames),
      deviceId_(opts.deviceId),
      maxRanges_(opts.maxRanges),
      numNestingLevels_(opts.numNestingLevels),
      muContext_(opts.muContext) {
  // used in unittests only
  if (opts.unitTest) {
    initSuccess_ = true;
    profiler_map[deviceId_] = this;
    return;
  }

  chipName_ = getChipName(opts.deviceId);

  if (!MuptiRBProfilerSession::initMupti()) {
    LOG(ERROR) << "Failed to initialize MUPTI range profiler.";
    return;
  }

  LOG(INFO) << "Initializing MUPTI range profiler session : device = " << deviceId_
            << " chip = " << chipName_;
  /* Generate configuration for metrics, this can also be done offline*/
  NVPW_InitializeHost_Params initializeHostParams = {
      NVPW_InitializeHost_Params_STRUCT_SIZE, nullptr};
  NVPW_CALL(NVPW_InitializeHost(&initializeHostParams));

  if (metricNames_.size()) {
    if (!nvperf::getProfilerConfigImage(
            chipName_,
            metricNames_,
            configImage,
            MuptiRBProfilerSession::counterAvailabilityImage().data())) {
      LOG(ERROR) << "Failed to create configImage or counterDataImagePrefix";
      return;
    }
    if (!nvperf::getCounterDataPrefixImage(
            chipName_,
            metricNames_,
            counterDataImagePrefix)) {
      LOG(ERROR) << "Failed to create counterDataImagePrefix";
      return;
    }
  } else {
    LOG(ERROR) << "No metrics provided to profile";
    return;
  }

  if (!createCounterDataImage()) {
    LOG(ERROR) << "Failed to create counterDataImage";
    return;
  }

  LOG(INFO) << "Size of structs\n"
            << " config image size = " << configImage.size()  << " B"
            << " counter data image prefix = "
            << counterDataImagePrefix.size()  << " B"
            << " counter data image size = " << counterDataImage.size() / 1024
            << " KB"
            << " counter sb image size = "
            << counterDataScratchBuffer.size()  << " B";

  beginPassParams_ = {MUpti_Profiler_BeginPass_Params_STRUCT_SIZE, nullptr};
  endPassParams_ = {MUpti_Profiler_EndPass_Params_STRUCT_SIZE, nullptr};

  initSuccess_ = true;
  profiler_map[deviceId_] = this;
}

void MuptiRBProfilerSession::startInternal(
    MUpti_ProfilerRange profilerRange,
    MUpti_ProfilerReplayMode profilerReplayMode) {
  LOG(INFO) << "Starting profiler session: profiler range = "
            << ((profilerRange == MUPTI_AutoRange) ? "autorange" : "userrange")
            << " replay mode = "
            << ((profilerReplayMode == MUPTI_KernelReplay) ? "kernel" : "user");
  if (!initSuccess_) {
    LOG(WARNING) << __func__ << "() bailing out since initialization failed";
    return;
  }

  if (muContext_ == nullptr) {
    for (const auto& it : ctx_to_dev) {
      if (it.second == deviceId_) {
        muContext_ = it.first;
        break;
      }
    }
    LOG(INFO) << " Mupti Profiler using MUSA context = " << muContext_;
  }

  profilerStartTs_ = std::chrono::high_resolution_clock::now();
  curRange_ = profilerRange;
  curReplay_ = profilerReplayMode;

  MUpti_Profiler_BeginSession_Params beginSessionParams = {
      MUpti_Profiler_BeginSession_Params_STRUCT_SIZE, nullptr};

  beginSessionParams.ctx = muContext_;
  beginSessionParams.counterDataImageSize = counterDataImage.size();
  beginSessionParams.pCounterDataImage = counterDataImage.data();
  beginSessionParams.counterDataScratchBufferSize =
      counterDataScratchBuffer.size();
  beginSessionParams.pCounterDataScratchBuffer = counterDataScratchBuffer.data();
  beginSessionParams.range = profilerRange;
  beginSessionParams.replayMode = profilerReplayMode;
  beginSessionParams.maxRangesPerPass = maxRanges_;
  beginSessionParams.maxLaunchesPerPass = maxRanges_;

  auto status = MUPTI_CALL(muptiProfilerBeginSession(&beginSessionParams));
  if (status != MUPTI_SUCCESS) {
    LOG(WARNING) << "Failed to start MUPTI range profiler";
    initSuccess_ = false;
    return;
  }

  // Set counter configuration
  MUpti_Profiler_SetConfig_Params setConfigParams = {
      MUpti_Profiler_SetConfig_Params_STRUCT_SIZE, nullptr};

  setConfigParams.ctx = muContext_;
  setConfigParams.pConfig = configImage.data();
  setConfigParams.configSize = configImage.size();
  setConfigParams.passIndex = 0;
  setConfigParams.minNestingLevel = 1;
  setConfigParams.numNestingLevels = numNestingLevels_;
  status = MUPTI_CALL(muptiProfilerSetConfig(&setConfigParams));

  if (status != MUPTI_SUCCESS) {
    LOG(WARNING) << "Failed to configure MUPTI range profiler";
    initSuccess_ = false;
    return;
  }
  profilerInitDoneTs_ = std::chrono::high_resolution_clock::now();

  if (curRange_ == MUPTI_AutoRange) {
    enableKernelCallbacks();
  }
  profilingActive_ = true;
}

void MuptiRBProfilerSession::stop() {
  if (!initSuccess_) {
    LOG(WARNING) << __func__ << "() bailing out since initialization failed";
    return;
  }
  LOG(INFO) << "Stop profiler session on device = " << deviceId_;

  MUpti_Profiler_UnsetConfig_Params unsetConfigParams = {
      MUpti_Profiler_UnsetConfig_Params_STRUCT_SIZE, nullptr};
  MUPTI_CALL(muptiProfilerUnsetConfig(&unsetConfigParams));

  MUpti_Profiler_EndSession_Params endSessionParams = {
      MUpti_Profiler_EndSession_Params_STRUCT_SIZE, nullptr};
  MUPTI_CALL(muptiProfilerEndSession(&endSessionParams));

  disableKernelCallbacks();

  profilerStopTs_ = std::chrono::high_resolution_clock::now();
  profilingActive_ = false;
}

void MuptiRBProfilerSession::beginPass() {
  if (!initSuccess_) {
    LOG(WARNING) << __func__ << "() bailing out since initialization failed";
    return;
  }
  MUPTI_CALL(muptiProfilerBeginPass(&beginPassParams_));
}

bool MuptiRBProfilerSession::endPass() {
  if (!initSuccess_) {
    LOG(WARNING) << __func__ << "() bailing out since initialization failed";
    return true;
  }
  MUPTI_CALL(muptiProfilerEndPass(&endPassParams_));
  return endPassParams_.allPassesSubmitted;
}

void MuptiRBProfilerSession::flushCounterData() {
  LOG(INFO) << "Flushing counter data on device = " << deviceId_;
  MUpti_Profiler_FlushCounterData_Params flushCounterDataParams = {
      MUpti_Profiler_FlushCounterData_Params_STRUCT_SIZE, nullptr};
  MUPTI_CALL(muptiProfilerFlushCounterData(&flushCounterDataParams));
}

/// Enable and disable the profiler
void MuptiRBProfilerSession::enable() {
  if (!initSuccess_) {
    LOG(WARNING) << __func__ << "() bailing out since initialization failed";
    return;
  }
  MUpti_Profiler_EnableProfiling_Params enableProfilingParams = {
      MUpti_Profiler_EnableProfiling_Params_STRUCT_SIZE, nullptr};
  MUPTI_CALL(muptiProfilerEnableProfiling(&enableProfilingParams));
}

void MuptiRBProfilerSession::disable() {
  if (!initSuccess_) {
    LOG(WARNING) << __func__ << "() bailing out since initialization failed";
    return;
  }
  MUpti_Profiler_DisableProfiling_Params disableProfilingParams = {
      MUpti_Profiler_DisableProfiling_Params_STRUCT_SIZE, nullptr};
  MUPTI_CALL(muptiProfilerDisableProfiling(&disableProfilingParams));
}

/// User range based profiling
void MuptiRBProfilerSession::pushRange(const std::string& rangeName) {
  LOG(INFO) << " MUPTI pushrange ( " << rangeName << " )";
  MUpti_Profiler_PushRange_Params pushRangeParams = {
      MUpti_Profiler_PushRange_Params_STRUCT_SIZE, nullptr};
  pushRangeParams.pRangeName = rangeName.c_str();
  MUPTI_CALL(muptiProfilerPushRange(&pushRangeParams));
}

void MuptiRBProfilerSession::popRange() {
  LOG(INFO) << " MUPTI pop range";
  MUpti_Profiler_PopRange_Params popRangeParams = {
      MUpti_Profiler_PopRange_Params_STRUCT_SIZE, nullptr};
  MUPTI_CALL(muptiProfilerPopRange(&popRangeParams));
}

void MuptiRBProfilerSession::startAndEnable() {
  startInternal(curRange_, curReplay_);
  if (curReplay_ == MUPTI_UserReplay) {
    beginPass();
  }
  enable();
  if (curRange_ == MUPTI_UserRange) {
    pushRange(kRootUserRangeName);
  }
  enable_flag[deviceId_] = false;
}

void MuptiRBProfilerSession::disableAndStop() {
  if (curRange_ == MUPTI_UserRange) {
    popRange();
  }
  disable();
  if (curReplay_ == MUPTI_UserReplay) {
    endPass();
    flushCounterData();
  }
  stop();
  disable_flag[deviceId_] = false;
}

void MuptiRBProfilerSession::asyncStartAndEnable(
    MUpti_ProfilerRange profilerRange,
    MUpti_ProfilerReplayMode profilerReplayMode) {
  LOG(INFO) << "Starting MUPTI range profiler asynchronously on device = "
            << deviceId_ << " profiler range = "
            << ((profilerRange == MUPTI_AutoRange) ? "autorange" : "userrange")
            << " replay mode = "
            << ((profilerReplayMode == MUPTI_KernelReplay) ? "kernel" : "user");
  curReplay_ = profilerReplayMode;
  curRange_ = profilerRange;
  enable_flag[deviceId_] = true;
  enableKernelCallbacks();
}

void MuptiRBProfilerSession::asyncDisableAndStop() {
  LOG(INFO) << "Stopping MUPTI range profiler asynchronously on device = "
            << deviceId_ << " cu context = " << muContext_;
  disable_flag[deviceId_] = true;
}


MuptiProfilerResult MuptiRBProfilerSession::evaluateMetrics(
    bool verbose) {
  if (!initSuccess_) {
    LOG(WARNING) << "Profiling failed, no results to return";
    return {};
  }
  if (profilingActive_) {
    disableAndStop();
  }

  LOG(INFO) << "Total kernels logged = " << kernelNames_.size();
  if (verbose) {
    for (const auto& kernel : kernelNames_) {
      std::cout << demangle(kernel) << std::endl;
    }
    LOG(INFO) << "Profiler Range data : ";
  }

  auto results = nvperf::evalMetricValues(
      chipName_, counterDataImage, metricNames_, verbose /*verbose*/);

  // profiler end-end duration
  auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      profilerStopTs_ - profilerStartTs_);

  auto init_dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      profilerInitDoneTs_ - profilerStartTs_);
  LOG(INFO) << "Total profiler time = " << duration_ms.count() << " ms";
  LOG(INFO) << "Total profiler init time = " << init_dur_ms.count() << " ms";

  return results;
}

void MuptiRBProfilerSession::saveCounterData(
    const std::string& /*CounterDataFileName*/,
    const std::string& /*CounterDataSBFileName*/) {
  /* TBD write binary files for counter data and counter scratch buffer */
}

/// Setup counter data
bool MuptiRBProfilerSession::createCounterDataImage() {
  MUpti_Profiler_CounterDataImageOptions counterDataImageOptions;
  counterDataImageOptions.pCounterDataPrefix = counterDataImagePrefix.data();
  counterDataImageOptions.counterDataPrefixSize = counterDataImagePrefix.size();
  counterDataImageOptions.maxNumRanges = maxRanges_;
  counterDataImageOptions.maxNumRangeTreeNodes = maxRanges_;
  counterDataImageOptions.maxRangeNameLength = 64;

  // Calculate size of counter data image
  MUpti_Profiler_CounterDataImage_CalculateSize_Params calculateSizeParams = {
      MUpti_Profiler_CounterDataImage_CalculateSize_Params_STRUCT_SIZE, nullptr};
  calculateSizeParams.pOptions = &counterDataImageOptions;
  calculateSizeParams.sizeofCounterDataImageOptions =
      MUpti_Profiler_CounterDataImageOptions_STRUCT_SIZE;

  MUPTI_CALL(
      muptiProfilerCounterDataImageCalculateSize(&calculateSizeParams));
  counterDataImage.resize(calculateSizeParams.counterDataImageSize);

  // Initialize counter data image
  MUpti_Profiler_CounterDataImage_Initialize_Params initializeParams = {
    MUpti_Profiler_CounterDataImage_Initialize_Params_STRUCT_SIZE, nullptr};
  initializeParams.sizeofCounterDataImageOptions =
    MUpti_Profiler_CounterDataImageOptions_STRUCT_SIZE;
  initializeParams.pOptions = &counterDataImageOptions;
  initializeParams.counterDataImageSize =
    calculateSizeParams.counterDataImageSize;
  initializeParams.pCounterDataImage = counterDataImage.data();
  MUPTI_CALL(muptiProfilerCounterDataImageInitialize(&initializeParams));

  // Calculate counter Scratch Buffer size
  MUpti_Profiler_CounterDataImage_CalculateScratchBufferSize_Params
    scratchBufferSizeParams = {
          MUpti_Profiler_CounterDataImage_CalculateScratchBufferSize_Params_STRUCT_SIZE, nullptr};

  scratchBufferSizeParams.counterDataImageSize =
    calculateSizeParams.counterDataImageSize;
  scratchBufferSizeParams.pCounterDataImage =
    initializeParams.pCounterDataImage;
  MUPTI_CALL(muptiProfilerCounterDataImageCalculateScratchBufferSize(
    &scratchBufferSizeParams));

  counterDataScratchBuffer.resize(
      scratchBufferSizeParams.counterDataScratchBufferSize);

  // Initialize scratch buffer
  MUpti_Profiler_CounterDataImage_InitializeScratchBuffer_Params
    initScratchBufferParams = {
      MUpti_Profiler_CounterDataImage_InitializeScratchBuffer_Params_STRUCT_SIZE, nullptr};

  initScratchBufferParams.counterDataImageSize =
    calculateSizeParams.counterDataImageSize;

  initScratchBufferParams.pCounterDataImage = initializeParams.pCounterDataImage;
  initScratchBufferParams.counterDataScratchBufferSize =
    scratchBufferSizeParams.counterDataScratchBufferSize;
  initScratchBufferParams.pCounterDataScratchBuffer =
    counterDataScratchBuffer.data();

  MUPTI_CALL(muptiProfilerCounterDataImageInitializeScratchBuffer(
      &initScratchBufferParams));

  return true;
}

MuptiRBProfilerSession::~MuptiRBProfilerSession() {
  if (initSuccess_) {
    MuptiRBProfilerSession::deInitMupti();
  }
}

#else

// Create empty stubs for the API when MUPTI is not present.
MuptiRBProfilerSession::MuptiRBProfilerSession(
    const MuptiRangeProfilerOptions& opts)
    : metricNames_(opts.metricNames),
      deviceId_(opts.deviceId),
      maxRanges_(opts.maxRanges),
      numNestingLevels_(opts.numNestingLevels),
      muContext_(opts.muContext) {};
MuptiRBProfilerSession::~MuptiRBProfilerSession() {}
void MuptiRBProfilerSession::stop() {}
void MuptiRBProfilerSession::enable() {}
void MuptiRBProfilerSession::disable() {}
void MuptiRBProfilerSession::beginPass() {}
bool MuptiRBProfilerSession::endPass() { return true; }
void MuptiRBProfilerSession::flushCounterData() {}
void MuptiRBProfilerSession::pushRange(const std::string& /*rangeName*/) {}
void MuptiRBProfilerSession::popRange() {}
void MuptiRBProfilerSession::startAndEnable() {}
void MuptiRBProfilerSession::disableAndStop() {}
void MuptiRBProfilerSession::asyncStartAndEnable(
    MUpti_ProfilerRange /*profilerRange*/,
    MUpti_ProfilerReplayMode /*profilerReplayMode*/) {}
void MuptiRBProfilerSession::asyncDisableAndStop() {}
MuptiProfilerResult MuptiRBProfilerSession::evaluateMetrics(bool verbose) {
  static MuptiProfilerResult res;
  return res;
};
void MuptiRBProfilerSession::saveCounterData(
    const std::string& /*CounterDataFileName*/,
    const std::string& /*CounterDataSBFileName*/) {}
bool MuptiRBProfilerSession::initMupti() { return false; }
void MuptiRBProfilerSession::deInitMupti() {}
bool MuptiRBProfilerSession::staticInit() { return false; }
std::set<uint32_t> MuptiRBProfilerSession::getActiveDevices() { return {}; }
bool MuptiRBProfilerSession::createCounterDataImage() { return true; }
void MuptiRBProfilerSession::startInternal(
    MUpti_ProfilerRange /*profilerRange*/,
    MUpti_ProfilerReplayMode /*profilerReplayMode*/) {}
std::vector<uint8_t>& MuptiRBProfilerSession::counterAvailabilityImage() {
  static std::vector<uint8_t> _vec;
  return _vec;
}
#endif // HAS_MUPTI_RANGE_PROFILER

std::unique_ptr<MuptiRBProfilerSession>
MuptiRBProfilerSessionFactory::make(const MuptiRangeProfilerOptions& opts) {
  return std::make_unique<MuptiRBProfilerSession>(opts);
}

namespace testing {

void trackMusaCtx(MUcontext ctx, uint32_t device_id, MUpti_CallbackId cbid) {
#if HAS_MUPTI_RANGE_PROFILER
  __trackMusaCtx(ctx, device_id, cbid);
#endif // HAS_MUPTI_RANGE_PROFILER
}

void trackMusaKernelLaunch(MUcontext ctx, const char* kernelName) {
#if HAS_MUPTI_RANGE_PROFILER
  __trackMusaKernelLaunch(ctx, kernelName);
#endif // HAS_MUPTI_RANGE_PROFILER
}

} // namespace testing
} // namespace KINETO_NAMESPACE
