/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DeviceUtil.h"

#include <mutex>

namespace KINETO_NAMESPACE {

bool amdGpuAvailable = false;
bool musaGpuAvailable = false;

bool isAMDGpuAvailable() {
#ifdef HAS_ROCTRACER
  static std::once_flag once;
  std::call_once(once, [] {
    // determine AMD GPU availability on the system
    hipError_t error;
    int deviceCount;
    error = hipGetDeviceCount(&deviceCount);
    amdGpuAvailable = (error == hipSuccess && deviceCount > 0);
  });
#endif
  return amdGpuAvailable;
}

bool isMUSAGpuAvailable() {
#ifdef HAS_MUPTI
  static std::once_flag once;
  std::call_once(once, [] {
    // determine MUSA GPU availability on the system
    musaError_t error;
    int deviceCount;
    error = musaGetDeviceCount(&deviceCount);
    musaGpuAvailable = (error == musaSuccess && deviceCount > 0);
  });
#endif
  return musaGpuAvailable;
}

bool isGpuAvailable() {
  bool amd = isAMDGpuAvailable();
  bool musa = isMUSAGpuAvailable();
  return amd || musa;
}

} // namespace KINETO_NAMESPACE
