/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MusaUtil.h"

#ifdef HAS_MUPTI
#include <musa.h>
#include <musa_runtime.h>
#endif

#include <mutex>

namespace KINETO_NAMESPACE {

bool gpuAvailable = false;

bool isGpuAvailable() {
#ifdef HAS_MUPTI
  static std::once_flag once;
  std::call_once(once, [] {
    // determine GPU availability on the system
    musaError_t error;
    int deviceCount;
    error = musaGetDeviceCount(&deviceCount);
    gpuAvailable = (error == musaSuccess && deviceCount > 0);
  });

  return gpuAvailable;
#else
  return false;
#endif
}

} // namespace KINETO_NAMESPACE
