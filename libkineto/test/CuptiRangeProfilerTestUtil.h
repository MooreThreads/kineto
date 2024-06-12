/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <stdlib.h>
#include <unordered_map>
#include <gtest/gtest.h>

// TODO(T90238193)
// @lint-ignore-every CLANGTIDY facebook-hte-RelativeInclude
#include "MuptiRangeProfilerApi.h"

namespace KINETO_NAMESPACE {

#if HAS_MUPTI_RANGE_PROFILER

class MockMuptiRBProfilerSession : public MuptiRBProfilerSession {
 public:
  explicit MockMuptiRBProfilerSession(const MuptiRangeProfilerOptions& opts)
    : MuptiRBProfilerSession(opts) {}

  void beginPass() override {
    LOG(INFO) << " Mock MUPTI begin pass";
    passes_started++;
  }

  bool endPass() override {
    passes_ended++;
    return true;
  }

  void flushCounterData() override {}

  void pushRange(const std::string& rangeName) override {
    LOG(INFO) << " Mock MUPTI pushrange ( " << rangeName << " )";
    ranges_started++;
  }

  void popRange() override {
    LOG(INFO) << " Mock MUPTI poprange";
    ranges_ended++;
  }

  void stop() override {
    profilerStopTs_ = std::chrono::high_resolution_clock::now();
    runChecks();
  }

  void enable() override {
    enabled = true;
  }
  void disable() override {}

  MuptiProfilerResult evaluateMetrics(bool /*verbose*/) override {
    return getResults()[deviceId()];
  }

protected:
  void startInternal(
      MUpti_ProfilerRange profilerRange,
      MUpti_ProfilerReplayMode profilerReplayMode) override {
    profilerStartTs_ = std::chrono::high_resolution_clock::now();
    curRange_ = profilerRange;
    curReplay_ = profilerReplayMode;
  }

private:
  void runChecks() {
    EXPECT_EQ(passes_started, passes_ended);
    EXPECT_EQ(ranges_started, ranges_ended);
  }

 public:
  int passes_started = 0;
  int passes_ended = 0;
  int ranges_started = 0;
  int ranges_ended = 0;
  bool enabled = false;

  static std::unordered_map<int, MuptiProfilerResult>& getResults();
};

struct MockMuptiRBProfilerSessionFactory : IMuptiRBProfilerSessionFactory {
  std::unique_ptr<MuptiRBProfilerSession> make(
      const MuptiRangeProfilerOptions& _opts) override {
    auto opts = _opts;
    opts.unitTest = true;
    return std::make_unique<MockMuptiRBProfilerSession>(opts);
  }

  MockMuptiRBProfilerSession* asDerived(MuptiRBProfilerSession* base) {
    return dynamic_cast<MockMuptiRBProfilerSession*>(base);
  }
};

inline void simulateMusaContextCreate(MUcontext context, uint32_t dev) {
  testing::trackMusaCtx(
      context, dev, MUPTI_CBID_RESOURCE_CONTEXT_CREATED);
}

inline void simulateMusaContextDestroy(MUcontext context, uint32_t dev) {
  testing::trackMusaCtx(
      context, dev, MUPTI_CBID_RESOURCE_CONTEXT_DESTROY_STARTING);
}

inline void simulateKernelLaunch(
    MUcontext context, const std::string& kernelName) {
  testing::trackMusaKernelLaunch(context, kernelName.c_str());
}

#endif // HAS_MUPTI_RANGE_PROFILER

} // namespace KINETO_NAMESPACE
