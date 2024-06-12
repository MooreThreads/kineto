/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "include/Config.h"
#include "src/MuptiRangeProfilerConfig.h"

#include <fmt/format.h>
#include <gtest/gtest.h>
#include <time.h>
#include <chrono>

using namespace KINETO_NAMESPACE;

class MuptiRangeProfilerConfigTest : public ::testing::Test {
 protected:
  void SetUp() override {
    MuptiRangeProfilerConfig::registerFactory();
  }
};

TEST_F(MuptiRangeProfilerConfigTest, ConfigureProfiler) {
  Config cfg;
  std::vector<std::string> metrics = {
    "kineto__musa_core_flops",
    "sm__inst_executed.sum",
    "l1tex__data_bank_conflicts_pipe_lsu.sum",
  };
  auto metricsConfigStr =
        fmt::format("MUPTI_PROFILER_METRICS = {}", fmt::join(metrics, ","));

  EXPECT_TRUE(cfg.parse(metricsConfigStr));
  EXPECT_TRUE(cfg.parse("MUPTI_PROFILER_ENABLE_PER_KERNEL = true"));
  EXPECT_TRUE(cfg.parse("MUPTI_PROFILER_MAX_RANGES = 42"));

  const MuptiRangeProfilerConfig& mupti_cfg =
    MuptiRangeProfilerConfig::get(cfg);

  EXPECT_EQ(mupti_cfg.activitiesMuptiMetrics(), metrics);
  EXPECT_EQ(mupti_cfg.muptiProfilerPerKernel(), true);
  EXPECT_EQ(mupti_cfg.muptiProfilerMaxRanges(), 42);

}

TEST_F(MuptiRangeProfilerConfigTest, RangesDefaults) {
  Config cfg, cfg_auto;

  // do not set max ranges in config, check defaults are sane
  EXPECT_TRUE(cfg.parse("MUPTI_PROFILER_METRICS = kineto__musa_core_flops"));
  EXPECT_TRUE(cfg.parse("MUPTI_PROFILER_ENABLE_PER_KERNEL = false"));

  cfg.setSignalDefaults();

  EXPECT_TRUE(cfg_auto.parse("MUPTI_PROFILER_METRICS = kineto__musa_core_flops"));
  EXPECT_TRUE(cfg_auto.parse("MUPTI_PROFILER_ENABLE_PER_KERNEL = true"));

  cfg_auto.setClientDefaults();

  int user_ranges, auto_ranges;

  user_ranges = MuptiRangeProfilerConfig::get(cfg).muptiProfilerMaxRanges();
  auto_ranges = MuptiRangeProfilerConfig::get(cfg_auto).muptiProfilerMaxRanges();

  EXPECT_GE(user_ranges, 1) << " in user range mode default to at least 1 ranges";
  EXPECT_GE(auto_ranges, 1000) << " in auto range mode default to at least 1000 ranges";

  EXPECT_GT(auto_ranges, user_ranges);
}
