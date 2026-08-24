/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MuptiPMSamplingApi.h"

#include <stdexcept>

#include <musa_runtime_api.h>
#include <mupti_pmsampling.h>
#include <mupti_profiler_host.h>
#include <mupti_profiler_target.h>
#include <mupti_target.h>

#include "DeviceUtil.h"
#include "Logger.h"
#include "ThrowUtil.h"

// The MUPTI APIs used in this file follow a versioned struct pattern: each
// function takes a params struct whose first two fields are structSize (so
// MUPTI knows which version of the struct the caller compiled against) and
// pPriv (an internal pointer, always nullptr from the caller's side). The
// remaining fields are the actual parameters.
//
// NVIDIA's recommended initialization pattern explicitly initializes only
// structSize and relies on C++ aggregate zero-initialization for pPriv and the
// remaining fields, then sets specific fields afterward:
//
//   MUpti_PmSampling_Start_Params params{
//       MUpti_PmSampling_Start_Params_STRUCT_SIZE};
//   params.pPmSamplingObject = samplingObject;
//
// -Wmissing-field-initializers (enabled by -Wextra) warns when an aggregate
// initializer does not provide values for every field, which fires at all of
// the MUPTI params call sites in this file. We suppress it rather than
// enumerating every field, since the struct definitions change between MUPTI
// SDK versions and explicit listing would break whenever NVIDIA adds or
// removes fields.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace KINETO_NAMESPACE {
namespace {

constexpr size_t kHardwareBufferSizeBytes = 64 * 1024 * 1024;
constexpr uint32_t kMaxSamplesPerDecode = 1024;

/*
 * ========================== MUPTI PM SAMPLING API ==========================
 *
 * MUPTI PM sampling periodically collects device metrics and exports completed
 * sample intervals as MuptiPMSample values.
 *
 * The lifecycle here is:
 *
 * 1. [Configure]
 *    Builds the metric configuration, calls muptiPmSamplingEnable(),
 *    initializes the data image (counterDataImage_). The data image is a
 *    caller-owned byte buffer initialized in MUPTI's format.
 *
 * 2. [Start]
 *    Starts hardware counter collection. MUPTI starts writing raw counter
 *    records into its hardware buffer.
 *
 * 3. [Decode] (PERIODICALLY, UNTIL STOP)
 *    Transfers records from the MUPTI's hardware buffer into the data image.
 *    Each completed sample in the data image is translated from MUPTI's format
 *    into a MuptiPMSample and pushed to the output vector. If records remain in
 *    the hardware buffer (COUNTER_DATA_FULL), we reset the image and return
 * false.
 *
 * 4. [Stop]
 *    Instruct MUPTI to stop producing records.
 *
 * 5. [Decode]
 *    Run one more decode to empty the hardware buffer.
 *
 * 6. [Disable]
 *    Disable the sampler, clear all objects.
 *
 * ===========================================================================
 */

} // namespace

MuptiPMSamplingApi::~MuptiPMSamplingApi() {
  disable();
  if (samplingObject_ != nullptr || hostObject_ != nullptr ||
      profilerInitialized_) {
    LOG(WARNING) << "MUPTI PM sampling cleanup failed during destruction; "
                    "MUPTI resources may leak";
  }
}

void MuptiPMSamplingApi::configure(const MuptiPMSamplingConfig& config) {
  if (samplingObject_ != nullptr || hostObject_ != nullptr ||
      profilerInitialized_) {
    KINETO_THROW(
        std::runtime_error,
        "Cannot configure MUPTI PM sampling before the previous "
        "configuration is fully disabled");
  }
  config_ = config;

  // MUPTI expects metric names as std::vector<const char*>
  metricNamePtrs_.clear();
  metricNamePtrs_.reserve(config_.metricNames.size());
  for (const auto& metricName : config_.metricNames) {
    metricNamePtrs_.push_back(metricName.c_str());
  }
  configureMupti();
}

void MuptiPMSamplingApi::ensureConfigured() const {
  if (samplingObject_ == nullptr) {
    KINETO_THROW(
        std::runtime_error, "MUPTI PM sampling: samplingObject_ is null");
  }
  if (hostObject_ == nullptr) {
    KINETO_THROW(std::runtime_error, "MUPTI PM sampling: hostObject_ is null");
  }
  if (counterDataImage_.empty()) {
    KINETO_THROW(
        std::runtime_error, "MUPTI PM sampling: counterDataImage_ is empty");
  }
}

void MuptiPMSamplingApi::configureMupti() {
  MUpti_Profiler_Initialize_Params params{
      MUpti_Profiler_Initialize_Params_STRUCT_SIZE};
  MUPTI_CALL_THROW(muptiProfilerInitialize(&params));
  profilerInitialized_ = true;

  // MUpti_Device_GetChipName_Params stores device info used in other MUPTI
  // calls.
  MUpti_Device_GetChipName_Params chip{
      MUpti_Device_GetChipName_Params_STRUCT_SIZE};
  chip.deviceIndex = static_cast<size_t>(config_.deviceId);
  MUPTI_CALL_THROW(muptiDeviceGetChipName(&chip));
  if (chip.pChipName == nullptr) {
    KINETO_THROW(
        std::runtime_error, "muptiDeviceGetChipName returned no chip name");
  }

  // Currently, only MUPTI_PM_SAMPLING_TRIGGER_MODE_GPU_TIME_INTERVAL is used
  // in this code. According to MUPTI docs, this trigger mode is not supported
  // on Turing or GA100 and is supported from GA10x onwards.
  musaDeviceProp deviceProperties{};
  const auto musaStatus =
      musaGetDeviceProperties(&deviceProperties, config_.deviceId);
  if (musaStatus != musaSuccess) {
    KINETO_THROW(
        std::runtime_error,
        std::string{"musaGetDeviceProperties failed: "} +
            musaGetErrorString(musaStatus));
  }
  if (deviceProperties.major < 8 ||
      (deviceProperties.major == 8 && deviceProperties.minor < 6)) {
    KINETO_THROW(
        std::runtime_error,
        "MUPTI PM sampling requires a GPU that supports "
        "GPU_TIME_INTERVAL (compute capability 8.6 or newer)");
  }

  // Building the availability image. This is a MUPTI byte buffer describing
  // which raw hardware counters are available on this machine. The first call
  // to muptiPmSamplingGetCounterAvailability is to obtain
  // counterAvailabilityImageSize, the number of bytes we need to allocate for
  // counterAvailabilityImage. Then the second call populates
  // counterAvailabilityImage.
  MUpti_PmSampling_GetCounterAvailability_Params availability{
      MUpti_PmSampling_GetCounterAvailability_Params_STRUCT_SIZE};
  availability.deviceIndex = static_cast<size_t>(config_.deviceId);
  MUPTI_CALL_THROW(muptiPmSamplingGetCounterAvailability(&availability));

  std::vector<uint8_t> counterAvailabilityImage(
      availability.counterAvailabilityImageSize);
  availability.pCounterAvailabilityImage = counterAvailabilityImage.data();
  MUPTI_CALL_THROW(muptiPmSamplingGetCounterAvailability(&availability));

  // MUpti_Profiler_Host_Initialize_Params is the evaluator which translates
  // from requested metric names into the raw counters the GPU should collect.
  // Later on, muptiProfilerHostEvaluateToGpuValues() uses the initialized
  // hostObject_ to convert raw hardware counters into the requested metric
  // values using chip-specific formulas.
  MUpti_Profiler_Host_Initialize_Params host{
      MUpti_Profiler_Host_Initialize_Params_STRUCT_SIZE};
  host.profilerType = MUPTI_PROFILER_TYPE_PM_SAMPLING;
  host.pChipName = chip.pChipName;
  host.pCounterAvailabilityImage = counterAvailabilityImage.data();
  MUPTI_CALL_THROW(muptiProfilerHostInitialize(&host));
  hostObject_ = host.pHostObject;

  // Translating the requested metric strings and passing them to MUPTI.
  // This updates hostObject_ with the high-level metric names inplace.
  MUpti_Profiler_Host_ConfigAddMetrics_Params addMetrics{
      MUpti_Profiler_Host_ConfigAddMetrics_Params_STRUCT_SIZE};
  addMetrics.pHostObject = hostObject_;
  addMetrics.ppMetricNames = metricNamePtrs_.data();
  addMetrics.numMetrics = metricNamePtrs_.size();
  MUPTI_CALL_THROW(muptiProfilerHostConfigAddMetrics(&addMetrics));

  // Allocation/configuration for configImage. This describes
  // the raw (low-level) counters the sampler needs to collect for the requested
  // metrics and how to schedule them.
  MUpti_Profiler_Host_GetConfigImageSize_Params size{
      MUpti_Profiler_Host_GetConfigImageSize_Params_STRUCT_SIZE};
  size.pHostObject = hostObject_;
  MUPTI_CALL_THROW(muptiProfilerHostGetConfigImageSize(&size));

  configImage_.resize(size.configImageSize);
  MUpti_Profiler_Host_GetConfigImage_Params image{
      MUpti_Profiler_Host_GetConfigImage_Params_STRUCT_SIZE};
  image.pHostObject = hostObject_;
  image.configImageSize = configImage_.size();
  image.pConfigImage = configImage_.data();
  MUPTI_CALL_THROW(muptiProfilerHostGetConfigImage(&image));

  // Asking MUPTI how many passes it will take to fetch the requested metrics.
  // We do not want to make multiple passes, so reject the config if this is the
  // case.
  MUpti_Profiler_Host_GetNumOfPasses_Params passes{
      MUpti_Profiler_Host_GetNumOfPasses_Params_STRUCT_SIZE};
  passes.configImageSize = configImage_.size();
  passes.pConfigImage = configImage_.data();
  MUPTI_CALL_THROW(muptiProfilerHostGetNumOfPasses(&passes));
  if (passes.numOfPasses != 1) {
    KINETO_THROW(
        std::runtime_error,
        "MUPTI PM sampling requires one pass; configuration requires " +
            std::to_string(passes.numOfPasses));
  }

  // Enabling PM sampling and setting the config
  MUpti_PmSampling_Enable_Params enable{
      MUpti_PmSampling_Enable_Params_STRUCT_SIZE};
  enable.deviceIndex = static_cast<size_t>(config_.deviceId);
  MUPTI_CALL_THROW(muptiPmSamplingEnable(&enable));
  samplingObject_ = enable.pPmSamplingObject;

  MUpti_PmSampling_SetConfig_Params setConfig{
      MUpti_PmSampling_SetConfig_Params_STRUCT_SIZE};
  setConfig.pPmSamplingObject = samplingObject_;
  setConfig.configSize = configImage_.size();
  setConfig.pConfig = configImage_.data();
  setConfig.hardwareBufferSize = kHardwareBufferSizeBytes;
  setConfig.samplingInterval =
      static_cast<uint64_t>(config_.samplingInterval.count());
  setConfig.triggerMode = MUPTI_PM_SAMPLING_TRIGGER_MODE_GPU_TIME_INTERVAL;
  setConfig.hwBufferAppendMode =
      MUPTI_PM_SAMPLING_HARDWARE_BUFFER_APPEND_MODE_KEEP_LATEST;
  MUPTI_CALL_THROW(muptiPmSamplingSetConfig(&setConfig));

  // Asking MUPTI how large the (counter) data image should be.
  // Since the data image has an opage MUPTI-defined layout, the size
  // depends on sampling config, metrics, etc.
  MUpti_PmSampling_GetCounterDataSize_Params counterDataSize{
      MUpti_PmSampling_GetCounterDataSize_Params_STRUCT_SIZE};
  counterDataSize.pPmSamplingObject = samplingObject_;
  counterDataSize.pMetricNames = metricNamePtrs_.data();
  counterDataSize.numMetrics = metricNamePtrs_.size();
  counterDataSize.maxSamples = kMaxSamplesPerDecode;
  MUPTI_CALL_THROW(muptiPmSamplingGetCounterDataSize(&counterDataSize));

  counterDataImage_.resize(counterDataSize.counterDataSize);
  resetImage();
}

void MuptiPMSamplingApi::resetImage() {
  MUpti_PmSampling_CounterDataImage_Initialize_Params params{
      MUpti_PmSampling_CounterDataImage_Initialize_Params_STRUCT_SIZE};
  params.pPmSamplingObject = samplingObject_;
  params.counterDataSize = counterDataImage_.size();
  params.pCounterData = counterDataImage_.data();
  MUPTI_CALL_THROW(muptiPmSamplingCounterDataImageInitialize(&params));
}

void MuptiPMSamplingApi::start() {
  ensureConfigured();
  MUpti_PmSampling_Start_Params params{
      MUpti_PmSampling_Start_Params_STRUCT_SIZE};
  params.pPmSamplingObject = samplingObject_;
  MUPTI_CALL_THROW(muptiPmSamplingStart(&params));
}

MuptiPMSample MuptiPMSamplingApi::decodeSample(size_t sampleIndex) {
  // Decode a single MUPTI PM sample into MuptiPMSample via
  // muptiProfilerHostEvaluateToGpuValues.
  MUpti_PmSampling_CounterData_GetSampleInfo_Params info{
      MUpti_PmSampling_CounterData_GetSampleInfo_Params_STRUCT_SIZE};
  info.pPmSamplingObject = samplingObject_;
  info.pCounterDataImage = counterDataImage_.data();
  info.counterDataImageSize = counterDataImage_.size();
  info.sampleIndex = sampleIndex;
  MUPTI_CALL_THROW(muptiPmSamplingCounterDataGetSampleInfo(&info));

  MuptiPMSample sample{
      info.startTimestamp,
      info.endTimestamp,
      std::vector<double>(metricNamePtrs_.size())};
  MUpti_Profiler_Host_EvaluateToGpuValues_Params evaluate{
      MUpti_Profiler_Host_EvaluateToGpuValues_Params_STRUCT_SIZE};
  evaluate.pHostObject = hostObject_;
  evaluate.pCounterDataImage = counterDataImage_.data();
  evaluate.counterDataImageSize = counterDataImage_.size();
  evaluate.rangeIndex = sampleIndex;
  evaluate.ppMetricNames = metricNamePtrs_.data();
  evaluate.numMetrics = metricNamePtrs_.size();
  evaluate.pMetricValues = sample.values.data();
  MUPTI_CALL_THROW(muptiProfilerHostEvaluateToGpuValues(&evaluate));
  return sample;
}

bool MuptiPMSamplingApi::decode(std::vector<MuptiPMSample>& samples) {
  ensureConfigured();
  // Fetch the next result from MUPTI. Decodes the collected counters
  // inline into the counterDataImage_.
  MUpti_PmSampling_DecodeData_Params decode{
      MUpti_PmSampling_DecodeData_Params_STRUCT_SIZE};
  decode.pPmSamplingObject = samplingObject_;
  decode.pCounterDataImage = counterDataImage_.data();
  decode.counterDataImageSize = counterDataImage_.size();
  MUPTI_CALL_THROW(muptiPmSamplingDecodeData(&decode));
  if (decode.overflow != 0) {
    LOG_FIRST_N(WARNING, 3)
        << "MUPTI PM sampling hardware buffer overflowed; older samples were "
           "dropped.";
  }

  const auto reason = decode.decodeStopReason;
  bool isBufferDrained;
  switch (reason) {
    case MUPTI_PM_SAMPLING_DECODE_STOP_REASON_OTHER:
    case MUPTI_PM_SAMPLING_DECODE_STOP_REASON_COUNTER_DATA_FULL:
      isBufferDrained = false;
      break;
    case MUPTI_PM_SAMPLING_DECODE_STOP_REASON_END_OF_RECORDS:
      isBufferDrained = true;
      break;
    default:
      KINETO_THROW(
          std::runtime_error,
          "muptiPmSamplingDecodeData returned unexpected stop reason " +
              std::to_string(static_cast<int>(reason)));
  }

  MUpti_PmSampling_GetCounterDataInfo_Params info{
      MUpti_PmSampling_GetCounterDataInfo_Params_STRUCT_SIZE};
  info.pCounterDataImage = counterDataImage_.data();
  info.counterDataImageSize = counterDataImage_.size();
  MUPTI_CALL_THROW(muptiPmSamplingGetCounterDataInfo(&info));

  samples.reserve(samples.size() + info.numCompletedSamples);
  for (size_t sampleIndex = 0; sampleIndex < info.numCompletedSamples;
       ++sampleIndex) {
    samples.push_back(decodeSample(sampleIndex));
  }

  resetImage();
  return isBufferDrained;
}

void MuptiPMSamplingApi::stop() {
  ensureConfigured();
  MUpti_PmSampling_Stop_Params params{MUpti_PmSampling_Stop_Params_STRUCT_SIZE};
  params.pPmSamplingObject = samplingObject_;
  MUPTI_CALL_THROW(muptiPmSamplingStop(&params));
}

void MuptiPMSamplingApi::disable() {
  // When releasing resources, stop at the first failure to avoid
  // a partially torn-down configuration and preserve the remaining
  // state for a later disable() retry.
  if (samplingObject_ != nullptr) {
    MUpti_PmSampling_Disable_Params params{
        MUpti_PmSampling_Disable_Params_STRUCT_SIZE};
    params.pPmSamplingObject = samplingObject_;
    if (MUPTI_CALL(muptiPmSamplingDisable(&params)) != MUPTI_SUCCESS) {
      return;
    }
    samplingObject_ = nullptr;
  }
  if (hostObject_ != nullptr) {
    MUpti_Profiler_Host_Deinitialize_Params params{
        MUpti_Profiler_Host_Deinitialize_Params_STRUCT_SIZE};
    params.pHostObject = hostObject_;
    if (MUPTI_CALL(muptiProfilerHostDeinitialize(&params)) != MUPTI_SUCCESS) {
      return;
    }
    hostObject_ = nullptr;
  }
  if (profilerInitialized_) {
    MUpti_Profiler_DeInitialize_Params params{
        MUpti_Profiler_DeInitialize_Params_STRUCT_SIZE};
    if (MUPTI_CALL(muptiProfilerDeInitialize(&params)) != MUPTI_SUCCESS) {
      return;
    }
    profilerInitialized_ = false;
  }

  configImage_.clear();
  counterDataImage_.clear();
  config_.deviceId = -1;
  metricNamePtrs_.clear();
  config_.metricNames.clear();
  config_.samplingInterval = std::chrono::nanoseconds::zero();
}

} // namespace KINETO_NAMESPACE

#pragma GCC diagnostic pop
