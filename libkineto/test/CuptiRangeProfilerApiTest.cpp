/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <array>
#include <set>

#include "include/libkineto.h"
#include "include/Config.h"
#include "src/MuptiRangeProfilerApi.h"

#include "src/Logger.h"
#include "test/MuptiRangeProfilerTestUtil.h"

using namespace KINETO_NAMESPACE;

#if HAS_MUPTI_RANGE_PROFILER

std::unordered_map<int, MuptiProfilerResult>&
MockMuptiRBProfilerSession::getResults() {
  static std::unordered_map<int, MuptiProfilerResult> results;
  return results;
}

MockMuptiRBProfilerSessionFactory mfactory{};

TEST(MuptiRangeProfilerApiTest, contextTracking) {
  std::vector<std::string> log_modules(
      {"MuptiRangeProfilerApi.cpp"});
  SET_LOG_VERBOSITY_LEVEL(1, log_modules);

  std::array<int64_t, 3> data;
  std::array<MUcontext, 3> contexts;
  for (int i = 0; i < data.size(); i++) {
    contexts[i] = reinterpret_cast<MUcontext>(&data[i]);
  }

  // simulate creating contexts, this calls the trackMusaContexts
  // function that would otherwise be called via a callback
  uint32_t dev = 0;
  for (auto ctx : contexts) {
    simulateMusaContextCreate(ctx, dev++);
  }

  EXPECT_EQ(
      MuptiRBProfilerSession::getActiveDevices(),
      std::set<uint32_t>({0, 1, 2}));

  simulateMusaContextDestroy(contexts[1], 1);

  EXPECT_EQ(
      MuptiRBProfilerSession::getActiveDevices(),
      std::set<uint32_t>({0, 2}));

  simulateMusaContextDestroy(contexts[0], 0);
  simulateMusaContextDestroy(contexts[2], 2);

  EXPECT_TRUE(
      MuptiRBProfilerSession::getActiveDevices().empty());
}

TEST(MuptiRangeProfilerApiTest, asyncLaunchUserRange) {
  std::vector<std::string> log_modules(
      {"MuptiRangeProfilerApi.cpp"});
  SET_LOG_VERBOSITY_LEVEL(1, log_modules);

  // this is bad but the pointer is never accessed
  MUcontext ctx0 = reinterpret_cast<MUcontext>(10);
  simulateMusaContextCreate(ctx0, 0 /*device_id*/);

  MuptiRangeProfilerOptions opts{
    .deviceId = 0,
    .muContext = ctx0};

  std::unique_ptr<MuptiRBProfilerSession> session_ = mfactory.make(opts);
  auto session = mfactory.asDerived(session_.get());

  session->asyncStartAndEnable(MUPTI_UserRange, MUPTI_UserReplay);

  simulateKernelLaunch(ctx0, "hello");
  simulateKernelLaunch(ctx0, "foo");
  simulateKernelLaunch(ctx0, "bar");

  session->asyncDisableAndStop();
  // stop happens after next kernel is run
  simulateKernelLaunch(ctx0, "bar");
  simulateMusaContextDestroy(ctx0, 0 /*device_id*/);

  EXPECT_EQ(session->passes_ended, 1);
  EXPECT_EQ(session->ranges_ended, 1);
  EXPECT_TRUE(session->enabled);
}

TEST(MuptiRangeProfilerApiTest, asyncLaunchAutoRange) {
  std::vector<std::string> log_modules(
      {"MuptiRangeProfilerApi.cpp"});
  SET_LOG_VERBOSITY_LEVEL(1, log_modules);

  // this is bad but the pointer is never accessed
  MUcontext ctx0 = reinterpret_cast<MUcontext>(10);
  MUcontext ctx1 = reinterpret_cast<MUcontext>(11);

  simulateMusaContextCreate(ctx0, 0 /*device_id*/);

  MuptiRangeProfilerOptions opts{
    .deviceId = 0,
    .muContext = ctx0};

  std::unique_ptr<MuptiRBProfilerSession> session_ = mfactory.make(opts);
  auto session = mfactory.asDerived(session_.get());

  session->asyncStartAndEnable(MUPTI_AutoRange, MUPTI_KernelReplay);

  simulateKernelLaunch(ctx0, "hello");
  simulateKernelLaunch(ctx0, "foo");
  simulateKernelLaunch(ctx1, "kernel_on_different_device");
  simulateKernelLaunch(ctx0, "bar");

  session->asyncDisableAndStop();
  // stop happens after next kernel is run
  simulateKernelLaunch(ctx0, "bar");
  simulateMusaContextDestroy(ctx0, 0 /*device_id*/);

  EXPECT_EQ(session->passes_ended, 0);
  EXPECT_EQ(session->ranges_ended, 0);
  EXPECT_TRUE(session->enabled);

  EXPECT_EQ(
      session->getKernelNames(),
      std::vector<std::string>({"hello", "foo", "bar"}))
    << "Kernel names were not tracked";
}

#endif // HAS_MUPTI_RANGE_PROFILER
