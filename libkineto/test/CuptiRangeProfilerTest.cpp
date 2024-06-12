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

#ifdef __linux__
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#endif

#include "include/libkineto.h"
#include "include/Config.h"
#include "src/ActivityTrace.h"
#include "src/MuptiRangeProfilerConfig.h"
#include "src/MuptiRangeProfiler.h"
#include "src/output_base.h"
#include "src/output_json.h"
#include "src/output_membuf.h"
#include "src/Logger.h"

#include "test/MuptiRangeProfilerTestUtil.h"

using namespace KINETO_NAMESPACE;

#if HAS_MUPTI_RANGE_PROFILER

std::unordered_map<int, MuptiProfilerResult>&
MockMuptiRBProfilerSession::getResults() {
  static std::unordered_map<int, MuptiProfilerResult> results;
  return results;
}

static std::vector<std::string> kCtx0Kernels = {
  "foo", "bar", "baz"};
static std::vector<std::string> kCtx1Kernels = {
  "mercury", "venus", "earth"};

static auto getActivityTypes() {
  static std::set activity_types_{libkineto::ActivityType::MUSA_PROFILER_RANGE};
  return activity_types_;
}

static IMuptiRBProfilerSessionFactory& getFactory() {
  static MockMuptiRBProfilerSessionFactory factory_;
  return factory_;
}

// Create mock MUPTI profiler events and simuulate context
// creation and kernel launches
class MuptiRangeProfilerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::vector<std::string> log_modules(
        {"MuptiRangeProfilerApi.cpp", "MuptiRangeProfiler.cpp"});
    SET_LOG_VERBOSITY_LEVEL(1, log_modules);

    // this is bad but the pointer is never accessed
    ctx0_ = reinterpret_cast<MUcontext>(10);
    ctx1_ = reinterpret_cast<MUcontext>(11);

    simulateMusaContextCreate(ctx0_, 0 /*device_id*/);
    simulateMusaContextCreate(ctx1_, 1 /*device_id*/);

    results_[0] = MuptiProfilerResult{};
    results_[1] = MuptiProfilerResult{};

    // use MockMuptiRBProfilerSession to mock MUPTI Profiler interface
    profiler_ = std::make_unique<MuptiRangeProfiler>(getFactory());

    // used for logging to a file
    loggerFactory.addProtocol("file", [](const std::string& url) {
        return std::unique_ptr<ActivityLogger>(new ChromeTraceLogger(url));
    });
  }

  void TearDown() override {
    simulateMusaContextDestroy(ctx0_, 0 /*device_id*/);
    simulateMusaContextDestroy(ctx1_, 1 /*device_id*/);
  }

  void setupConfig(const std::vector<std::string>& metrics, bool per_kernel) {
    std::string config_str = fmt::format("ACTIVITIES_WARMUP_PERIOD_SECS=0\n "
      "MUPTI_PROFILER_METRICS={}\n "
      "MUPTI_PROFILER_ENABLE_PER_KERNEL={}",
      fmt::join(metrics, ","),
      (per_kernel ? "true" : "false"));

    cfg_ = std::make_unique<Config>();
    cfg_->parse(config_str);
    cfg_->setClientDefaults();
    cfg_->setSelectedActivityTypes(getActivityTypes());

    // setup profiler results
    results_[0].metricNames = metrics;
    results_[1].metricNames = metrics;
    for (int i = 0; i < metrics.size(); i++) {
      measurements_.push_back(0.1 * i);
    }
  }

  int simulateWorkload() {
    for (const auto& k : kCtx0Kernels) {
      simulateKernelLaunch(ctx0_, k);
    }
    for (const auto& k : kCtx1Kernels) {
      simulateKernelLaunch(ctx1_, k);
    }
    return kCtx0Kernels.size() + kCtx1Kernels.size();
  }

  void setupResultsUserRange() {
    // sets up mock results returned by Mock MUPTI interface
    results_[0].rangeVals.emplace_back(
        MuptiRangeMeasurement{"__profile__", measurements_});
    results_[1].rangeVals.emplace_back(
        MuptiRangeMeasurement{"__profile__", measurements_});
  }

  void setupResultsAutoRange() {
    // sets up mock results returned by Mock MUPTI interface
    for (const auto& k : kCtx0Kernels) {
      results_[0].rangeVals.emplace_back(
        MuptiRangeMeasurement{k, measurements_});
    }
    for (const auto& k : kCtx1Kernels) {
      results_[1].rangeVals.emplace_back(
        MuptiRangeMeasurement{k, measurements_});
    }
  }

  std::unique_ptr<Config> cfg_;
  std::unique_ptr<MuptiRangeProfiler> profiler_;
  ActivityLoggerFactory loggerFactory;

  std::vector<double> measurements_;
  std::unordered_map<int, MuptiProfilerResult>& results_
    = MockMuptiRBProfilerSession::getResults();

  MUcontext ctx0_, ctx1_;
};

void checkMetrics(
    const std::vector<std::string>& metrics,
    const std::string& metadataJson) {
  for (const auto& m : metrics) {
    EXPECT_NE(metadataJson.find(m), std::string::npos)
      << "Could not find metdata on metric " << m
      << "\n metadata json = '" << metadataJson << "'";
  }
}

void saveTrace(ActivityTrace& /*trace*/) {
  // TODO seems to be hitting a memory bug run with ASAN
#if 0
//#ifdef __linux__
  char filename[] = "/tmp/libkineto_testXXXXXX.json";
  mkstemps(filename, 5);
  trace.save(filename);
  // Check that the expected file was written and that it has some content
  int fd = open(filename, O_RDONLY);
  if (!fd) {
    perror(filename);
  }
  EXPECT_TRUE(fd);
  // Should expect at least 100 bytes
  struct stat buf{};
  fstat(fd, &buf);
  EXPECT_GT(buf.st_size, 100);
#endif
}

TEST_F(MuptiRangeProfilerTest, BasicTest) {

  EXPECT_NE(profiler_->name().size(), 0);
  EXPECT_EQ(profiler_->availableActivities(), getActivityTypes());

  std::set<ActivityType> incorrect_act_types{
    ActivityType::MUSA_RUNTIME, ActivityType::CONCURRENT_KERNEL};

  cfg_ = std::make_unique<Config>();
  cfg_->setClientDefaults();
  cfg_->setSelectedActivityTypes({});
  EXPECT_EQ(
      profiler_->configure(incorrect_act_types, *cfg_).get(), nullptr)
    << "Profiler config should fail for wrong activity type";

  incorrect_act_types.insert(ActivityType::MUSA_PROFILER_RANGE);

  cfg_ = std::make_unique<Config>();
  cfg_->setClientDefaults();
  cfg_->setSelectedActivityTypes(incorrect_act_types);

  EXPECT_EQ(
      profiler_->configure(incorrect_act_types, *cfg_).get(), nullptr)
    << "Profiler config should fail if the activity types is not exclusively"
    << " MUSA_PROFILER_RANGE";
}

TEST_F(MuptiRangeProfilerTest, UserRangeTest) {

  std::vector<std::string> metrics{
    "smsp__sass_thread_inst_executed_op_dadd_pred_on.sum",
    "sm__inst_executed_pipe_tensor.sum",
  };

  setupConfig(metrics, false /*per_kernel*/);

  auto session = profiler_->configure(getActivityTypes(), *cfg_);
  ASSERT_NE(session, nullptr) << "MUPTI Profiler configuration failed";

  session->start();
  simulateWorkload();
  session->stop();

  setupResultsUserRange();

  // Have the profiler process them
  auto logger = std::make_unique<MemoryTraceLogger>(*cfg_);
  session->processTrace(*logger);

  // Just a wrapper to iterate events
  ActivityTrace trace(std::move(logger), loggerFactory);
  EXPECT_EQ(trace.activities()->size(), 2);
  auto activities = *trace.activities();

  // check if we have all counter values encoded
  for (auto& actvity : activities) {
    checkMetrics(metrics, actvity->metadataJson());
    EXPECT_EQ(actvity->type(), ActivityType::MUSA_PROFILER_RANGE);
  }
  EXPECT_EQ(activities[0]->deviceId(), 0);
  EXPECT_EQ(activities[1]->deviceId(), 1);

  saveTrace(trace);
}

TEST_F(MuptiRangeProfilerTest, AutoRangeTest) {

  std::vector<std::string> metrics{
    "smsp__sass_thread_inst_executed_op_dadd_pred_on.sum",
    "sm__inst_executed_pipe_tensor.sum",
  };
  int kernel_count = 0;

  setupConfig(metrics, true /*per_kernel*/);

  auto session = profiler_->configure(getActivityTypes(), *cfg_);
  ASSERT_NE(session, nullptr) << "MUPTI Profiler configuration failed";

  session->start();
  kernel_count = simulateWorkload();
  session->stop();

  setupResultsAutoRange();

  // Have the profiler process them
  auto logger = std::make_unique<MemoryTraceLogger>(*cfg_);
  session->processTrace(*logger);

  // Just a wrapper to iterate events
  ActivityTrace trace(std::move(logger), loggerFactory);
  EXPECT_EQ(trace.activities()->size(), kernel_count);
  auto activities = *trace.activities();

  // check if we have all counter values encoded
  for (auto& actvity : activities) {
    checkMetrics(metrics, actvity->metadataJson());
    EXPECT_EQ(actvity->type(), ActivityType::MUSA_PROFILER_RANGE);
  }

  // check if kernel names are captured
  for (int i = 0; i < kCtx0Kernels.size(); i++) {
    EXPECT_EQ(activities[i]->deviceId(), 0);
    EXPECT_EQ(activities[i]->name(), kCtx0Kernels[i]);
  }

  const size_t offset = kCtx0Kernels.size();
  for (int i = 0; i < kCtx1Kernels.size(); i++) {
    EXPECT_EQ(activities[i + offset]->deviceId(), 1);
    EXPECT_EQ(activities[i + offset]->name(), kCtx1Kernels[i]);
  }

  // check transfer of ownership
  auto traceBuffer = session->getTraceBuffer();
  ASSERT_NE(traceBuffer, nullptr);
  EXPECT_EQ(traceBuffer->activities.size(), kernel_count);

  EXPECT_NE(traceBuffer->span.startTime, 0);
  EXPECT_NE(traceBuffer->span.endTime, 0);
  EXPECT_GT(traceBuffer->span.endTime, traceBuffer->span.startTime);

  saveTrace(trace);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  MuptiRangeProfilerConfig::registerFactory();
  return RUN_ALL_TESTS();
}

#endif // HAS_MUPTI_RANGE_PROFILER
