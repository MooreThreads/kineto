/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Config.h"

#include <chrono>
#include <set>
#include <string>
#include <vector>

namespace KINETO_NAMESPACE {

constexpr char kMuptiProfilerConfigName[] = "mupti_rb_profiler";

class MuptiRangeProfilerConfig : public AbstractConfig {
 public:
  bool handleOption(const std::string& name, std::string& val) override;

  void validate(const std::chrono::time_point<std::chrono::system_clock>&
                    fallbackProfileStartTime) override {}

  static MuptiRangeProfilerConfig& get(const Config& cfg) {
    return dynamic_cast<MuptiRangeProfilerConfig&>(
        cfg.feature(kMuptiProfilerConfigName));
  }

  Config& parent() const {
    return *parent_;
  }

  std::vector<std::string> activitiesMuptiMetrics() const {
    return activitiesMuptiMetrics_;
  }

  bool muptiProfilerPerKernel() const {
    return muptiProfilerPerKernel_;
  }

  int64_t muptiProfilerMaxRanges() const {
    return muptiProfilerMaxRanges_;
  }

  void setSignalDefaults() override {
    setDefaults();
  }

  void setClientDefaults() override {
    setDefaults();
  }

  void printActivityProfilerConfig(std::ostream& s) const override;
  void setActivityDependentConfig() override;
  static void registerFactory();

 protected:
  AbstractConfig* cloneDerived(AbstractConfig& parent) const override {
    MuptiRangeProfilerConfig* clone = new MuptiRangeProfilerConfig(*this);
    clone->parent_ = dynamic_cast<Config*>(&parent);
    return clone;
  }

 private:
  MuptiRangeProfilerConfig() = delete;
  explicit MuptiRangeProfilerConfig(Config& parent);
  explicit MuptiRangeProfilerConfig(const MuptiRangeProfilerConfig& other) =
      default;

  // some defaults will depend on other configuration
  void setDefaults();

  // Associated Config object
  Config* parent_;

  // Counter metrics exposed via MUPTI Profiler API
  std::vector<std::string> activitiesMuptiMetrics_;

  // Collect profiler metrics per kernel - autorange made
  bool muptiProfilerPerKernel_{false};

  // max number of ranges to configure the profiler for.
  // this has to be set before hand to reserve space for the output
  int64_t muptiProfilerMaxRanges_ = 0;
};

} // namespace KINETO_NAMESPACE
