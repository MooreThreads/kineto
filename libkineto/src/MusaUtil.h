/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fmt/format.h>

#ifdef HAS_MUPTI
#include <musa.h>
#include <musa_runtime.h>
#include <mupti.h>

#define MUSA_CALL(call)                                      \
  [&]() -> musaError_t {                                     \
    musaError_t _status_ = call;                             \
    if (_status_ != musaSuccess) {                           \
      const char* _errstr_ = musaGetErrorString(_status_);   \
      LOG(WARNING) << fmt::format(                           \
          "function {} failed with error {} ({})",           \
          #call,                                             \
          _errstr_,                                          \
          (int)_status_);                                    \
    }                                                        \
    return _status_;                                         \
  }()

#define MUPTI_CALL(call)                           \
  [&]() -> MUptiResult {                           \
    MUptiResult _status_ = call;                   \
    if (_status_ != MUPTI_SUCCESS) {               \
      const char* _errstr_ = nullptr;              \
      muptiGetResultString(_status_, &_errstr_);   \
      LOG(WARNING) << fmt::format(                 \
          "function {} failed with error {} ({})", \
          #call,                                   \
          _errstr_,                                \
          (int)_status_);                          \
    }                                              \
    return _status_;                               \
  }()

#else
#define MUPTI_CALL(call) call
#endif // HAS_MUPTI

#define MUPTI_CALL_NOWARN(call) call

namespace KINETO_NAMESPACE {

bool isMUSAGpuAvailable();

bool isGpuAvailable();

} // namespace KINETO_NAMESPACE
