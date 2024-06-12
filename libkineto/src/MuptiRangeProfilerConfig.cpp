/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <MuptiRangeProfilerConfig.h>
#include <Logger.h>

#include <stdlib.h>
#include <unistd.h>

#include <fmt/format.h>
#include <ostream>


namespace KINETO_NAMESPACE {

// number of ranges affect the size of counter data binary used by
// the MUPTI Profiler. these defaults can be tuned
constexpr int KMaxAutoRanges = 1500; // supports 1500 kernels
constexpr int KMaxUserRanges = 10;   // enable upto 10 sub regions marked by user

constexpr char kMuptiProfilerMetricsKey[] = "MUPTI_PROFILER_METRICS";
constexpr char kMuptiProfilerPerKernelKey[] = "MUPTI_PROFILER_ENABLE_PER_KERNEL";
constexpr char kMuptiProfilerMaxRangesKey[] = "MUPTI_PROFILER_MAX_RANGES";

MuptiRangeProfilerConfig::MuptiRangeProfilerConfig(Config& cfg)
    : parent_(&cfg),
      muptiProfilerPerKernel_(false),
      muptiProfilerMaxRanges_(0) {}

bool MuptiRangeProfilerConfig::handleOption(const std::string& name, std::string& val) {
  VLOG(0) << " handling : " << name << " = " << val;
  // Mupti Range based Profiler configuration
  if (!name.compare(kMuptiProfilerMetricsKey)) {
    activitiesMuptiMetrics_ = splitAndTrim(val, ',');
  } else if (!name.compare(kMuptiProfilerPerKernelKey)) {
    muptiProfilerPerKernel_ = toBool(val);
  } else if (!name.compare(kMuptiProfilerMaxRangesKey)) {
    muptiProfilerMaxRanges_ = toInt64(val);
  } else {
    return false;
  }
  return true;
}

void MuptiRangeProfilerConfig::setDefaults() {
  if (activitiesMuptiMetrics_.size() > 0 && muptiProfilerMaxRanges_ == 0) {
    muptiProfilerMaxRanges_ =
      muptiProfilerPerKernel_ ? KMaxAutoRanges : KMaxUserRanges;
  }
}

void MuptiRangeProfilerConfig::printActivityProfilerConfig(std::ostream& s) const {
  if (activitiesMuptiMetrics_.size() > 0) {
    s << "Mupti Profiler metrics : "
      << fmt::format("{}", fmt::join(activitiesMuptiMetrics_, ", ")) << std::endl;
    s << "Mupti Profiler measure per kernel : "
      << muptiProfilerPerKernel_ << std::endl;
    s << "Mupti Profiler max ranges : " << muptiProfilerMaxRanges_ << std::endl;
  }
}

void MuptiRangeProfilerConfig::registerFactory() {
  Config::addConfigFactory(
      kMuptiProfilerConfigName,
      [](Config& cfg) { return new MuptiRangeProfilerConfig(cfg); });
}

} // namespace KINETO_NAMESPACE
