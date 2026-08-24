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

TEST(MuptiStringsTest, RuntimeStripsFourDigitVersionSuffix) {
  EXPECT_EQ(
      runtimeCbidName(MUPTI_RUNTIME_TRACE_CBID_musaDriverGetVersion_v3020),
      "musaDriverGetVersion");
  EXPECT_EQ(
      runtimeCbidName(MUPTI_RUNTIME_TRACE_CBID_musaDeviceSynchronize_v3020),
      "musaDeviceSynchronize");
  EXPECT_EQ(
      runtimeCbidName(MUPTI_RUNTIME_TRACE_CBID_musaLaunchKernel_v7000),
      "musaLaunchKernel");
}

TEST(MuptiStringsTest, RuntimeStripsFiveDigitVersionSuffix) {
#if defined(MUPTI_API_VERSION) && MUPTI_API_VERSION >= 18
  EXPECT_EQ(
      runtimeCbidName(MUPTI_RUNTIME_TRACE_CBID_musaLaunchKernelExC_v11060),
      "musaLaunchKernelExC");
#endif
  EXPECT_EQ(
      runtimeCbidName(
          MUPTI_RUNTIME_TRACE_CBID_musaStreamSetAttribute_ptsz_v11000),
      "musaStreamSetAttribute_ptsz");
}

// MUPTI includes the `_v3` API-generation suffix in MUSA 12.3-12.9 and drops it
// in MUSA 13.x. Verify that only the trailing `_v12030` version suffix is
// removed. This also exercises a cbid >= 446 — the range that returned
// "INVALID" before the refactor that motivated D104900166.
TEST(MuptiStringsTest, RuntimeStripsOnlyTrailingVersionSuffix) {
#if defined(MUSA_VERSION) && MUSA_VERSION >= 13000
  EXPECT_EQ(
      runtimeCbidName(MUPTI_RUNTIME_TRACE_CBID_musaStreamGetCaptureInfo_v12030),
      "musaStreamGetCaptureInfo");
#elif defined(MUSA_VERSION) && MUSA_VERSION >= 12030 && MUSA_VERSION < 13000
  EXPECT_EQ(
      runtimeCbidName(
          MUPTI_RUNTIME_TRACE_CBID_musaStreamGetCaptureInfo_v3_v12030),
      "musaStreamGetCaptureInfo_v3");
#endif
}

TEST(MuptiStringsTest, RuntimePreservesNamesWithoutVersionSuffix) {
  EXPECT_EQ(runtimeCbidName(MUPTI_RUNTIME_TRACE_CBID_INVALID), "INVALID");
}

TEST(MuptiStringsTest, RuntimeReturnsInvalidForUnknownCbids) {
  EXPECT_EQ(runtimeCbidName(static_cast<MUpti_CallbackId>(-1)), "INVALID");
  EXPECT_EQ(
      runtimeCbidName(static_cast<MUpti_CallbackId>(0xFFFFFFFF)), "INVALID");
  EXPECT_EQ(runtimeCbidName(100000), "INVALID");
}

TEST(MuptiStringsTest, DriverPreservesNamesWithoutVersionSuffix) {
  EXPECT_EQ(driverCbidName(MUPTI_DRIVER_TRACE_CBID_INVALID), "INVALID");
  EXPECT_EQ(
      driverCbidName(MUPTI_DRIVER_TRACE_CBID_muLaunchKernel), "muLaunchKernel");
  EXPECT_EQ(driverCbidName(MUPTI_DRIVER_TRACE_CBID_muMemCreate), "cuMemCreate");
}

TEST(MuptiStringsTest, DriverReturnsInvalidForUnknownCbids) {
  EXPECT_EQ(driverCbidName(static_cast<MUpti_CallbackId>(-1)), "INVALID");
  EXPECT_EQ(
      driverCbidName(static_cast<MUpti_CallbackId>(0xFFFFFFFF)), "INVALID");
}
