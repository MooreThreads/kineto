/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MusaDeviceProperties.h"

#include <fmt/format.h>
#include <vector>

#include <musa_runtime.h>
#include <musa_occupancy.h>

#include "Logger.h"

namespace KINETO_NAMESPACE {

static const std::vector<musaDeviceProp> createDeviceProps() {
  std::vector<musaDeviceProp> props;
  int device_count;
  musaError_t error_id = musaGetDeviceCount(&device_count);
  // Return empty vector if error.
  if (error_id != musaSuccess) {
    LOG(ERROR) << "musaGetDeviceCount failed with code " << error_id;
    return {};
  }
  VLOG(0) << "Device count is " << device_count;
  for (size_t i = 0; i < device_count; ++i) {
    musaDeviceProp prop;
    error_id = musaGetDeviceProperties(&prop, i);
    // Return empty vector if any device property fail to get.
    if (error_id != musaSuccess) {
      LOG(ERROR) << "musaGetDeviceProperties failed with " << error_id;
      return {};
    }
    // TODO: temp workaround, see https://github.mthreads.com/mthreads/kineto/issues/3
    prop.major = 3;
    props.push_back(prop);
    LOGGER_OBSERVER_ADD_DEVICE(i);
  }
  return props;
}

static const std::vector<musaDeviceProp>& deviceProps() {
  static const std::vector<musaDeviceProp> props = createDeviceProps();
  return props;
}

static const std::string createDevicePropertiesJson(
    size_t id, const musaDeviceProp& props) {
  return fmt::format(R"JSON(
    {{
      "id": {}, "name": "{}", "totalGlobalMem": {},
      "computeMajor": {}, "computeMinor": {},
      "maxThreadsPerBlock": {}, "maxThreadsPerMultiprocessor": {},
      "regsPerBlock": {}, "regsPerMultiprocessor": {}, "warpSize": {},
      "sharedMemPerBlock": {}, "sharedMemPerMultiprocessor": {},
      "numSms": {}, "sharedMemPerBlockOptin": {}
    }})JSON",
      id, props.name, props.totalGlobalMem,
      props.major, props.minor,
      props.maxThreadsPerBlock, props.maxThreadsPerMultiProcessor,
      props.regsPerBlock, props.regsPerMultiprocessor, props.warpSize,
      props.sharedMemPerBlock, props.sharedMemPerMultiprocessor,
      props.multiProcessorCount, props.sharedMemPerBlockOptin);
}

static const std::string createDevicePropertiesJson() {
  std::vector<std::string> jsonProps;
  const auto& props = deviceProps();
  for (size_t i = 0; i < props.size(); i++) {
    jsonProps.push_back(createDevicePropertiesJson(i, props[i]));
  }
  return fmt::format("{}", fmt::join(jsonProps, ","));
}

const std::string& devicePropertiesJson() {
  static std::string devicePropsJson = createDevicePropertiesJson();
  return devicePropsJson;
}

int smCount(uint32_t deviceId) {
  const std::vector<musaDeviceProp> &props = deviceProps();
  return deviceId >= props.size() ? 0 :
     props[deviceId].multiProcessorCount;
}

float blocksPerSm(const MUpti_ActivityKernel6& kernel) {
  return (kernel.gridX * kernel.gridY * kernel.gridZ) /
      (float) smCount(kernel.deviceId);
}

float warpsPerSm(const MUpti_ActivityKernel6& kernel) {
  constexpr int threads_per_warp = 32;
  return blocksPerSm(kernel) *
      (kernel.blockX * kernel.blockY * kernel.blockZ) /
      threads_per_warp;
}

float kernelOccupancy(const MUpti_ActivityKernel6& kernel) {
  float blocks_per_sm = -1.0;
  int sm_count = smCount(kernel.deviceId);
  if (sm_count) {
    blocks_per_sm =
        (kernel.gridX * kernel.gridY * kernel.gridZ) / (float) sm_count;
  }
  return kernelOccupancy(
      kernel.deviceId,
      kernel.registersPerThread,
      kernel.staticSharedMemory,
      kernel.dynamicSharedMemory,
      kernel.blockX,
      kernel.blockY,
      kernel.blockZ,
      blocks_per_sm);
}

float kernelOccupancy(
    uint32_t deviceId,
    uint16_t registersPerThread,
    int32_t staticSharedMemory,
    int32_t dynamicSharedMemory,
    int32_t blockX,
    int32_t blockY,
    int32_t blockZ,
    float blocksPerSm) {
  // Calculate occupancy
  float occupancy = -1.0;
  const std::vector<musaDeviceProp> &props = deviceProps();
  if (deviceId < props.size()) {
    musaOccFuncAttributes occFuncAttr;
    occFuncAttr.maxThreadsPerBlock = INT_MAX;
    occFuncAttr.numRegs = registersPerThread;
    occFuncAttr.sharedSizeBytes = staticSharedMemory;
    occFuncAttr.partitionedGCConfig = PARTITIONED_GC_OFF;
    occFuncAttr.shmemLimitConfig = FUNC_SHMEM_LIMIT_DEFAULT;
    occFuncAttr.maxDynamicSharedSizeBytes = 0;
    const musaOccDeviceState occDeviceState = {};
    int blockSize = blockX * blockY * blockZ;
    size_t dynamicSmemSize = dynamicSharedMemory;
    musaOccResult occ_result;
    musaOccDeviceProp prop(props[deviceId]);
    musaOccError status = musaOccMaxActiveBlocksPerMultiprocessor(
          &occ_result, &prop, &occFuncAttr, &occDeviceState,
          blockSize, dynamicSmemSize);
    if (status == MUSA_OCC_SUCCESS) {
      if (occ_result.activeBlocksPerMultiprocessor < blocksPerSm) {
        blocksPerSm = occ_result.activeBlocksPerMultiprocessor;
      }
      occupancy = blocksPerSm * blockSize /
          (float) props[deviceId].maxThreadsPerMultiProcessor;
    } else {
      LOG_EVERY_N(ERROR, 1000) << "Failed to calculate occupancy, status = "
                               << status;
    }
  }
  return occupancy;
}

} // namespace KINETO_NAMESPACE
