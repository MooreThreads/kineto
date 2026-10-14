/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DeviceProperties.h"

#include "MetadataFieldCatalog.h"
#include "TypedMetadataJson.h"

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <algorithm>
#include <utility>
#include <vector>

#ifdef HAS_MUPTI
#include <musa_occupancy.h>
#include <musa_runtime.h>
#elif defined(HAS_ROCTRACER)
#include <hip/hip_runtime.h>
#elif defined(HAS_XPUPTI)
#include "plugin/xpupti/XpuptiActivityProfiler.h"
#endif

#include "Logger.h"

namespace KINETO_NAMESPACE {

namespace DevicePropertyFields = libkineto::DevicePropertyMetadataFields;

#ifdef HAS_MUPTI
#define gpuDeviceProp musaDeviceProp
#define gpuError_t musaError_t
#define gpuSuccess musaSuccess
#define gpuGetDeviceCount musaGetDeviceCount
#define gpuGetDeviceProperties musaGetDeviceProperties
#elif defined(HAS_ROCTRACER)
#define gpuDeviceProp hipDeviceProp_t
#define gpuError_t hipError_t
#define gpuSuccess hipSuccess
#define gpuGetDeviceCount hipGetDeviceCount
#define gpuGetDeviceProperties hipGetDeviceProperties
#endif

#if defined(HAS_MUPTI) || defined(HAS_ROCTRACER)
static std::vector<gpuDeviceProp> createDeviceProps() {
  std::vector<gpuDeviceProp> props;
  int device_count;
  gpuError_t error_id = gpuGetDeviceCount(&device_count);
  // Return empty vector if error.
  if (error_id != gpuSuccess) {
    LOG(ERROR) << "gpuGetDeviceCount failed with code " << error_id;
    return {};
  }
  VLOG(0) << "Device count is " << device_count;
  for (int i = 0; i < device_count; ++i) {
    gpuDeviceProp prop;
    error_id = gpuGetDeviceProperties(&prop, i);
    // Return empty vector if any device property fail to get.
    if (error_id != gpuSuccess) {
      LOG(ERROR) << "gpuGetDeviceProperties failed with " << error_id;
      return {};
    }
    props.push_back(prop);
    LOGGER_OBSERVER_ADD_DEVICE(i);
  }
  return props;
}

static const std::vector<gpuDeviceProp>& deviceProps() {
  static const std::vector<gpuDeviceProp> props = createDeviceProps();
  return props;
}

// Emit one device's compute properties as typed metadata
static void visitDeviceMetadata(
    size_t id,
    const gpuDeviceProp& props,
    libkineto::ITypedMetadataVisitor& visitor) {
  visitor.visit(DevicePropertyFields::kId, static_cast<uint64_t>(id));
  visitor.visit(DevicePropertyFields::kName, std::string{props.name});
  visitor.visit(
      DevicePropertyFields::kTotalGlobalMem,
      static_cast<uint64_t>(props.totalGlobalMem));
  visitor.visit(
      DevicePropertyFields::kComputeMajor, static_cast<int64_t>(props.major));
  visitor.visit(
      DevicePropertyFields::kComputeMinor, static_cast<int64_t>(props.minor));
  visitor.visit(
      DevicePropertyFields::kMaxThreadsPerBlock,
      static_cast<int64_t>(props.maxThreadsPerBlock));
  visitor.visit(
      DevicePropertyFields::kMaxThreadsPerMultiprocessor,
      static_cast<int64_t>(props.maxThreadsPerMultiProcessor));
  visitor.visit(
      DevicePropertyFields::kRegsPerBlock,
      static_cast<int64_t>(props.regsPerBlock));
  visitor.visit(
      DevicePropertyFields::kWarpSize, static_cast<int64_t>(props.warpSize));
  visitor.visit(
      DevicePropertyFields::kSharedMemPerBlock,
      static_cast<uint64_t>(props.sharedMemPerBlock));
  visitor.visit(
      DevicePropertyFields::kNumSms,
      static_cast<int64_t>(props.multiProcessorCount));
#ifdef HAS_MUPTI
  visitor.visit(
      DevicePropertyFields::kRegsPerMultiprocessor,
      static_cast<int64_t>(props.regsPerMultiprocessor));
  visitor.visit(
      DevicePropertyFields::kSharedMemPerBlockOptin,
      static_cast<uint64_t>(props.sharedMemPerBlockOptin));
  visitor.visit(
      DevicePropertyFields::kSharedMemPerMultiprocessor,
      static_cast<uint64_t>(props.sharedMemPerMultiprocessor));
#elif defined(HAS_ROCTRACER)
  visitor.visit(
      DevicePropertyFields::kMaxSharedMemoryPerMultiProcessor,
      static_cast<uint64_t>(props.maxSharedMemoryPerMultiProcessor));
#endif
}

static std::string createDevicePropertiesJson() {
  const auto& props = deviceProps();
  std::vector<std::string> jsonProps;
  jsonProps.reserve(props.size());
  for (size_t i = 0; i < props.size(); i++) {
    libkineto::internal::JsonTypedMetadataVisitor visitor;
    visitDeviceMetadata(i, props[i], visitor);
    auto json = std::move(visitor).json();
    json.insert(json.begin(), '{');
    json.push_back('}');
    jsonProps.push_back(std::move(json));
  }
  return fmt::format("{}", fmt::join(jsonProps, ","));
}
#endif // HAS_MUPTI || HAS_ROCTRACER

const std::string& devicePropertiesJson() {
#if defined(HAS_MUPTI) || defined(HAS_ROCTRACER)
  static std::string devicePropsJson = createDevicePropertiesJson();
#elif defined(HAS_XPUPTI)
  static std::string devicePropsJson = getXpuDeviceProperties();
#else
  static std::string devicePropsJson;
#endif
  return devicePropsJson;
}

int smCount([[maybe_unused]] uint32_t deviceId) {
#if defined(HAS_MUPTI) || defined(HAS_ROCTRACER)
  const std::vector<gpuDeviceProp>& props = deviceProps();
  return deviceId >= props.size() ? 0 : props[deviceId].multiProcessorCount;
#else
  return 0;
#endif
}

#ifdef HAS_MUPTI
float blocksPerSm(const MUpti_ActivityKernelType& kernel) {
  int sm_count = smCount(kernel.deviceId);
  if (sm_count == 0) {
    return std::numeric_limits<float>::infinity();
  }
  return (kernel.gridX * kernel.gridY * kernel.gridZ) /
      static_cast<float>(sm_count);
}

float warpsPerSm(const MUpti_ActivityKernelType& kernel) {
  constexpr int threads_per_warp = 32;
  return blocksPerSm(kernel) * (kernel.blockX * kernel.blockY * kernel.blockZ) /
      threads_per_warp;
}

OccupancyMetrics computeOccupancyMetrics(
    const MUpti_ActivityKernelType& kernel) {
  OccupancyMetrics metrics;
  const std::vector<musaDeviceProp>& props = deviceProps();
  if (kernel.deviceId >= props.size()) {
    LOG(ERROR) << "Invalid deviceId " << kernel.deviceId
               << " exceeds available devices (" << props.size()
               << "), skipping occupancy calculation";
    return metrics;
  }

  float blocksPerSm = -1.0;
  int sm_count = smCount(kernel.deviceId);
  if (sm_count != 0) {
    blocksPerSm = (kernel.gridX * kernel.gridY * kernel.gridZ) /
        static_cast<float>(sm_count);
  }

  musaOccFuncAttributes occFuncAttr;
  occFuncAttr.maxThreadsPerBlock = INT_MAX;
  occFuncAttr.numRegs = kernel.registersPerThread;
  occFuncAttr.sharedSizeBytes = kernel.staticSharedMemory;
  occFuncAttr.partitionedGCConfig = PARTITIONED_GC_OFF;
  occFuncAttr.shmemLimitConfig = FUNC_SHMEM_LIMIT_DEFAULT;
  occFuncAttr.maxDynamicSharedSizeBytes = 0;
  const musaOccDeviceState occDeviceState = {};
  int blockSize = kernel.blockX * kernel.blockY * kernel.blockZ;
  size_t dynamicSmemSize = kernel.dynamicSharedMemory;
  musaOccDeviceProp prop(props[kernel.deviceId]);
  musaOccError status = musaOccMaxActiveBlocksPerMultiprocessor(
      &metrics.result,
      &prop,
      &occFuncAttr,
      &occDeviceState,
      blockSize,
      dynamicSmemSize);
  if (status == MUSA_OCC_SUCCESS) {
    float effectiveBlocksPerSm = std::min<float>(
        metrics.result.activeBlocksPerMultiprocessor, blocksPerSm);
    metrics.occupancy = effectiveBlocksPerSm * blockSize /
        static_cast<float>(props[kernel.deviceId].maxThreadsPerMultiProcessor);
  } else {
    LOG_EVERY_N(ERROR, 1000)
        << "Failed to calculate occupancy, status = " << status;
  }
  return metrics;
}
#endif // HAS_MUPTI

} // namespace KINETO_NAMESPACE
