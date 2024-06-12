/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>

#include <libkineto.h>
#include <IActivityProfiler.h>

// TODO(T90238193)
// @lint-ignore-every CLANGTIDY facebook-hte-RelativeInclude
#include "MuptiRangeProfilerApi.h"

/* MuptiRangeProfiler :
 *   This profiler object provides an interface to run the MUPTI
 *   Range Based Profiler API.
 */

namespace KINETO_NAMESPACE {

using MuptiProfilerPrePostCallback = std::function<void(void)>;

/* Activity Profiler session encapsulates the MUPTI Range based Profiler
 * API object
 */
class MuptiRangeProfilerSession : public IActivityProfilerSession {
 public:
  explicit MuptiRangeProfilerSession(
      const Config& config,
      IMuptiRBProfilerSessionFactory& factory);

  ~MuptiRangeProfilerSession() override {};

  // start profiling
  void start() override;

  // stop profiling
  void stop() override;

  // process trace events with logger
  void processTrace(libkineto::ActivityLogger& logger) override;

  std::unique_ptr<CpuTraceBuffer> getTraceBuffer() override {
    return std::make_unique<CpuTraceBuffer>(std::move(traceBuffer_));
  }

  // returns errors with this trace
  std::vector<std::string> errors() override;

  // returns device info used in this trace, could be nullptr
  std::unique_ptr<DeviceInfo> getDeviceInfo() override;

  // returns resource info used in this trace, could be empty
  std::vector<ResourceInfo> getResourceInfos() override;

 private:
  void addRangeEvents(
      const MuptiProfilerResult& result,
      const MuptiRBProfilerSession* profiler);

  MUpti_ProfilerRange rangeType_ = MUPTI_UserRange;
  MUpti_ProfilerReplayMode replayType_ = MUPTI_UserReplay;

  CpuTraceBuffer traceBuffer_;
  std::vector<
    std::unique_ptr<MuptiRBProfilerSession>> profilers_;
};


/* This is a wrapper class that refers to the underlying
 * MuptiRangeProfiler. Using a wrapper libkineto can manage the ownership
 * of this object independent of the MuptiRangeProfiler itself.
 */
class MuptiRangeProfiler : public libkineto::IActivityProfiler {
 public:
  explicit MuptiRangeProfiler();

  explicit MuptiRangeProfiler(IMuptiRBProfilerSessionFactory& factory);

  ~MuptiRangeProfiler() override {}

  // name of profiler
  const std::string& name() const override;

  // returns activity types this profiler supports
  const std::set<ActivityType>& availableActivities() const override;

  // sets up the tracing session and provides control to the
  // the activity profiler session object.
  std::unique_ptr<libkineto::IActivityProfilerSession> configure(
      const std::set<libkineto::ActivityType>& activity_types,
      const Config& config) override;

  // asynchronous version of the above with future timestamp and duration.
  std::unique_ptr<libkineto::IActivityProfilerSession> configure(
      int64_t ts_ms,
      int64_t duration_ms,
      const std::set<libkineto::ActivityType>& activity_types,
      const Config& config) override;

  // hooks to enable configuring the environment before and after the
  // profiling sesssion.
  static void setPreRunCallback(MuptiProfilerPrePostCallback fn);
  static void setPostRunCallback(MuptiProfilerPrePostCallback fn);
 private:
  IMuptiRBProfilerSessionFactory& factory_;
};

struct MuptiRangeProfilerInit {
  MuptiRangeProfilerInit();
  bool success = false;
};

} // namespace KINETO_NAMESPACE
