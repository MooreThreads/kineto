/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "src/mupti_strings.h"

using namespace KINETO_NAMESPACE;

TEST(MuptiStringsTest, Valid) {
  ASSERT_STREQ(
      runtimeCbidName(MUPTI_RUNTIME_TRACE_CBID_INVALID), "INVALID");
  ASSERT_STREQ(
      runtimeCbidName(MUPTI_RUNTIME_TRACE_CBID_musaDriverGetVersion_v3020),
      "musaDriverGetVersion");
  ASSERT_STREQ(runtimeCbidName
      (MUPTI_RUNTIME_TRACE_CBID_musaDeviceSynchronize_v3020),
      "musaDeviceSynchronize");
  ASSERT_STREQ(
      runtimeCbidName(MUPTI_RUNTIME_TRACE_CBID_musaStreamSetAttribute_ptsz_v11000),
      "musaStreamSetAttribute_ptsz");
#if defined(MUPTI_API_VERSION) && MUPTI_API_VERSION >= 17
  ASSERT_STREQ(
      runtimeCbidName(MUPTI_RUNTIME_TRACE_CBID_musaLaunchKernelExC_v11060),
      "musaLaunchKernelExC");
#endif
}

TEST(MuptiStringsTest, Invalid) {
  ASSERT_STREQ(runtimeCbidName(-1), "INVALID");
  // We can't actually use MUPTI_RUNTIME_TRACE_CBID_SIZE here until we
  // auto-generate the string table, since it may have more entries than
  // the enum in the version used to compile.
  ASSERT_STREQ(runtimeCbidName(1000), "INVALID");
}
