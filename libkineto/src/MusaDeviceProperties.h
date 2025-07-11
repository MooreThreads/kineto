/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <stdint.h>
#include <string>

#ifdef HAS_MUPTI
#include <mupti.h>
#endif

namespace KINETO_NAMESPACE {

int smCount(uint32_t deviceId);

#ifdef HAS_MUPTI
float blocksPerSm(const MUpti_ActivityKernel6& kernel);
float warpsPerSm(const MUpti_ActivityKernel6& kernel);

// Return estimated achieved occupancy for a kernel
float kernelOccupancy(const MUpti_ActivityKernel6& kernel);
float kernelOccupancy(
    uint32_t deviceId,
    uint16_t registersPerThread,
    int32_t staticSharedMemory,
    int32_t dynamicSharedMemory,
    int32_t blockX,
    int32_t blockY,
    int32_t blockZ,
    float blocks_per_sm);

// Return compute properties for each device as a json string
const std::string& devicePropertiesJson();
#endif

} // namespace KINETO_NAMESPACE
