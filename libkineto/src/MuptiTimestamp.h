/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <mupti.h>

#include <cstdint>

#include "ApproximateClock.h"

namespace KINETO_NAMESPACE {

// Configure the timestamp source shared by MUPTI Activity and PM Sampling.
void configureMuptiTimestampSource(bool useTsc);
bool isMuptiTimestampSourceReady();

// This function allows us to activate/deactivate TSC MUPTI callbacks
// via a killswitch.
bool& use_mupti_tsc();

// If we are running on Windows or are on a MUSA version < 11.6,
// we use the default system clock so no conversion needed.
inline int64_t convertMuptiTimestamp(uint64_t timestamp) {
  static const int64_t epochOffset = getTime() - getTime(true);
  return timestamp + epochOffset;
}

inline int64_t convertMuptiDuration(uint64_t start, uint64_t end) {
  return end - start;
}

} // namespace KINETO_NAMESPACE
