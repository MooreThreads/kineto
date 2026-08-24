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
#include <stdexcept>

#include <musa.h>
#include <musa_runtime.h>
#include <mupti.h>

#include "ThrowUtil.h"

// ok to use fmt::format as error will not occur often. Can't use fmt::print
// easily since LOG(...) can return void, causes compiler error
#define MUSA_CALL(call)                                    \
  [&]() -> musaError_t {                                   \
    musaError_t _status_ = call;                           \
    if (_status_ != musaSuccess) {                         \
      const char* _errstr_ = musaGetErrorString(_status_); \
      LOG(WARNING) << fmt::format(                         \
          "function {} failed with error {} ({})",         \
          #call,                                           \
          _errstr_,                                        \
          (int)_status_);                                  \
    }                                                      \
    return _status_;                                       \
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

// clang-format off
#define MUPTI_CALL_THROW(call)                    \
  do {                                            \
    MUptiResult _status_ = (call);                \
    if (_status_ != MUPTI_SUCCESS) {              \
      const char* _errstr_ = nullptr;             \
      muptiGetResultString(_status_, &_errstr_);  \
      KINETO_THROW(                               \
          std::runtime_error,                     \
          fmt::format(                            \
              "{} failed: {}",                   \
              #call,                              \
              _errstr_ != nullptr                 \
                  ? _errstr_                      \
                  : "unknown MUPTI error"));      \
    }                                             \
  } while (false)
// clang-format on

#elif defined(HAS_ROCTRACER)
#include <hip/hip_runtime.h>
#include <rocprofiler-sdk/version.h>

#define MUSA_CALL(call)                                   \
  {                                                       \
    hipError_t _status_ = call;                           \
    if (_status_ != hipSuccess) {                         \
      const char* _errstr_ = hipGetErrorString(_status_); \
      LOG(WARNING) << fmt::format(                        \
          "function {} failed with error {} ({})",        \
          #call,                                          \
          _errstr_,                                       \
          (int)_status_);                                 \
    }                                                     \
  }

#define MUPTI_CALL(call) call

#else
#define MUPTI_CALL(call) call
#endif // HAS_MUPTI

#define MUPTI_CALL_NOWARN(call) call

namespace KINETO_NAMESPACE {

bool isAMDGpuAvailable();

bool isMUSAGpuAvailable();

bool isGpuAvailable();

} // namespace KINETO_NAMESPACE
