/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MuptiTimestamp.h"

#include <chrono>

#include "DeviceUtil.h"
#include "Logger.h"

namespace KINETO_NAMESPACE {
namespace {

bool timestampSourceReady{false};

} // namespace

bool& use_mupti_tsc() {
  static bool use_mupti_tsc = true;
  return use_mupti_tsc;
}

void configureMuptiTimestampSource([[maybe_unused]] bool useTsc) {
  // MUPTI 5.1 has no timestamp callback; use epoch conversion.
  use_mupti_tsc() = false;
  timestampSourceReady = true;
}

bool isMuptiTimestampSourceReady() {
  return timestampSourceReady;
}

} // namespace KINETO_NAMESPACE
