/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#ifdef HAS_MUPTI
#include <musa.h>
#include <musa_runtime_api.h>
// Using MUSA 11 and above due to usage of API: muptiProfilerGetCounterAvailability.
#if defined(USE_MUPTI_RANGE_PROFILER) && defined(MUSART_VERSION) && MUSART_VERSION >= 10000 && MUSA_VERSION >= 11000
#define HAS_MUPTI_RANGE_PROFILER 1
#endif // MUSART_VERSION > 10.00 and MUSA_VERSION >= 11.00
#endif // HAS_MUPTI

#if HAS_MUPTI_RANGE_PROFILER
#include <mupti.h>
#include <mupti_profiler_target.h>
#include <mupti_target.h>
#else
enum MUpti_ProfilerRange
{
  MUPTI_AutoRange,
  MUPTI_UserRange,
};

enum MUpti_ProfilerReplayMode
{
  MUPTI_KernelReplay,
  MUPTI_UserReplay,
};
#endif // HAS_MUPTI_RANGE_PROFILER

#include <chrono>
#include <mutex>
#include <string>
#include <vector>
#include <set>

// TODO(T90238193)
// @lint-ignore-every CLANGTIDY facebook-hte-RelativeInclude
#include "TraceSpan.h"
#include "MuptiCallbackApi.h"
#include "MuptiNvPerfMetric.h"

/* Mupti Range based profiler session
 * See : https://docs.nvidia.com/mupti/Mupti/r_main.html#r_profiler
 */

namespace KINETO_NAMESPACE {

// Initialize and configure MUPTI Profiler counters.
// - Metric names must be provided as string vector.
// - Supported values by MUPTI can be found at -
//   https://docs.nvidia.com/mupti/Mupti/r_main.html#r_host_metrics_api
struct MuptiRangeProfilerOptions {
  std::vector<std::string> metricNames;
  int deviceId = 0;
  int maxRanges = 1;
  int numNestingLevels = 1;
  MUcontext muContext = 0;
  bool unitTest = false;
};

class MuptiRBProfilerSession {
 public:

  explicit MuptiRBProfilerSession(const MuptiRangeProfilerOptions& opts);

  virtual ~MuptiRBProfilerSession();

  // Start profiling session
  // This function has to be called from the CPU thread running
  // the MUSA context. If this is not the case asyncStartAndEnable()
  // can be used
  void start(
      MUpti_ProfilerRange profilerRange = MUPTI_AutoRange,
      MUpti_ProfilerReplayMode profilerReplayMode = MUPTI_KernelReplay) {
    startInternal(profilerRange, profilerReplayMode);
  }

  // Stop profiling session
  virtual void stop();

  virtual void enable();
  virtual void disable();

  // Profiler passes
  //  GPU hardware has limited performance monitoring resources
  //  the MUPTI profiler may need to run multiple passes to collect
  //  data for a given range
  //  If we use kernel replay model the kernels are automatically replayed
  //  else, you can use the beginPass() and endPass() functions below
  //  for user to manage the replays

  // starts a profiler pass with given kernels in between
  virtual void beginPass();

  // end a profiler pass with given kernels in between
  // returns true if no more passes are required
  virtual bool endPass();

  // flushes the counter data - required if you use user replay
  virtual void flushCounterData();

  // Each pass can contain multiple of ranges
  //  metrics configured in a pass are collected per each range-stack.
  virtual void pushRange(const std::string& rangeName);
  virtual void popRange();

  // utilities for common operations
  void startAndEnable();
  void disableAndStop();

  // Async APIs : these will can be called from another thread
  // outside the MUSA context being profiled
  void asyncStartAndEnable(
      MUpti_ProfilerRange profilerRange = MUPTI_AutoRange,
      MUpti_ProfilerReplayMode profilerReplayMode = MUPTI_KernelReplay);
  void asyncDisableAndStop();

  void printMetrics() {
    evaluateMetrics(true);
  }

 TraceSpan getProfilerTraceSpan();

  virtual MuptiProfilerResult evaluateMetrics(bool verbose = false);

  void saveCounterData(
      const std::string& CounterDataFileName,
      const std::string& CounterDataSBFileName);

  // This is not thread safe so please only call after
  // profiling has stopped
  const std::vector<std::string>& getKernelNames() const {
    return kernelNames_;
  }

  int deviceId() const {
    return deviceId_;
  }

  bool profilingActive() const {
    return profilingActive_;
  }

  static std::set<uint32_t> getActiveDevices();

  static bool initMupti();

  static void deInitMupti();

  static bool staticInit();

  static void setCounterAvailabilityImage(std::vector<uint8_t> img) {
    counterAvailabilityImage() = img;
  }

 protected:
  virtual void startInternal(
      MUpti_ProfilerRange profilerRange,
      MUpti_ProfilerReplayMode profilerReplayMode);

  MUpti_ProfilerRange curRange_ = MUPTI_AutoRange;
  MUpti_ProfilerReplayMode curReplay_ = MUPTI_KernelReplay;

  std::chrono::time_point<std::chrono::high_resolution_clock>
    profilerStartTs_, profilerStopTs_, profilerInitDoneTs_;

 private:

  bool createCounterDataImage();

  // log kernel name that used with callbacks
  void logKernelName(const char* kernel) {
    std::lock_guard<std::mutex> lg(kernelNamesMutex_);
    kernelNames_.emplace_back(kernel);
  }

  std::vector<std::string> metricNames_;
  std::string chipName_;

  uint32_t deviceId_ = 0;
  int maxRanges_;
  int numNestingLevels_;
  MUcontext muContext_;


  // data buffers for configuration and counter data collection
  std::vector<uint8_t> counterDataImagePrefix;
  std::vector<uint8_t> configImage;
  std::vector<uint8_t> counterDataImage;
  std::vector<uint8_t> counterDataScratchBuffer;

  std::mutex kernelNamesMutex_;
  // raw kernel names (not demangled)
  std::vector<std::string> kernelNames_;

  uint32_t numCallbacks_ = 0;

  static std::vector<uint8_t>& counterAvailabilityImage();

#if HAS_MUPTI_RANGE_PROFILER
  MUpti_Profiler_BeginPass_Params beginPassParams_;
  MUpti_Profiler_EndPass_Params endPassParams_;
#endif

  bool initSuccess_ = false;
  bool profilingActive_ = false;

  friend void __trackMusaKernelLaunch(MUcontext ctx, const char* kernelName);
};

// Factory class used by the wrapping MuptiProfiler object
struct IMuptiRBProfilerSessionFactory {
  virtual std::unique_ptr<MuptiRBProfilerSession> make(
      const MuptiRangeProfilerOptions& opts) = 0;
  virtual ~IMuptiRBProfilerSessionFactory() {}
};

struct MuptiRBProfilerSessionFactory : IMuptiRBProfilerSessionFactory {
  std::unique_ptr<MuptiRBProfilerSession> make(
      const MuptiRangeProfilerOptions& opts) override;
};


// called directly only in unit tests
namespace testing {

void trackMusaCtx(MUcontext ctx, uint32_t device_id, MUpti_CallbackId cbid);
void trackMusaKernelLaunch(MUcontext ctx, const char* kernelName);

} // namespace testing

} // namespace KINETO_NAMESPACE
