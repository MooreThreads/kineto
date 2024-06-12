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

#endif // HAS_MUPTI
