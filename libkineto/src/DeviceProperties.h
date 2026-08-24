/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <string>

#ifdef HAS_MUPTI
#include <musa_occupancy.h>
#include <mupti.h>
#endif

namespace KINETO_NAMESPACE {

// Return compute properties for each device as a json string
const std::string& devicePropertiesJson();

int smCount(uint32_t deviceId);

// TODO: Implement the below for HAS_ROCTRACER
#ifdef HAS_MUPTI

// Use newer MUPTI activity structs for extended fields
#if defined(MUSA_VERSION) && MUSA_VERSION >= 13010
using MUpti_ActivityKernelType = MUpti_ActivityKernel6;
#elif defined(MUSA_VERSION) && MUSA_VERSION >= 12000
using MUpti_ActivityKernelType = MUpti_ActivityKernel6;
#else
using MUpti_ActivityKernelType = MUpti_ActivityKernel6;
#endif

#if defined(MUSA_VERSION) && MUSA_VERSION >= 12000
using MUpti_ActivityMemcpyType = MUpti_ActivityMemcpy4;
using MUpti_ActivityMemcpyPtoPType = MUpti_ActivityMemcpyPtoP3;
using MUpti_ActivityMemsetType = MUpti_ActivityMemset3;
#else
using MUpti_ActivityMemcpyType = MUpti_ActivityMemcpy4;
using MUpti_ActivityMemcpyPtoPType = MUpti_ActivityMemcpy4;
using MUpti_ActivityMemsetType = MUpti_ActivityMemset3;
#endif

float blocksPerSm(const MUpti_ActivityKernelType& kernel);
float warpsPerSm(const MUpti_ActivityKernelType& kernel);

// Occupancy results from MUSA occupancy calculator
// Returns musaOccResult from musa_occupancy.h plus a computed occupancy metric
struct OccupancyMetrics {
  float occupancy = -1.0f; // Computed effective occupancy in number of threads
  musaOccResult result =
      {}; // Raw results from musaOccMaxActiveBlocksPerMultiprocessor
};

// Return detailed occupancy metrics including limiting factors
OccupancyMetrics computeOccupancyMetrics(
    const MUpti_ActivityKernelType& kernel);
#endif

} // namespace KINETO_NAMESPACE
