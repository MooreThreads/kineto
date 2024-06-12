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

#include <mupti.h>

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

#define MUPTI_CALL_NOWARN(call) call

#else

#define MUPTI_CALL(call) call
#define MUPTI_CALL_NOWARN(call) call

#endif // HAS_MUPTI
