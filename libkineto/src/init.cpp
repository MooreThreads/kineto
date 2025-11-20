/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <memory>
#include <mutex>

// TODO(T90238193)
// @lint-ignore-every CLANGTIDY facebook-hte-RelativeInclude
#include "ActivityProfilerProxy.h"
#include "Config.h"
#include "ConfigLoader.h"
#include "DaemonConfigLoader.h"
#include "MusaUtil.h"
#include "ThreadUtil.h"
#ifdef HAS_MUPTI
#include "MuptiCallbackApi.h"
#include "MuptiActivityApi.h"
#include "MuptiRangeProfiler.h"
#include "EventProfilerController.h"
#endif
#include "libkineto.h"

#include "Logger.h"

namespace KINETO_NAMESPACE {

#if __linux__ || defined(HAS_MUPTI)
static bool initialized = false;

static void initProfilers() {
  if (!initialized) {
    libkineto::api().initProfilerIfRegistered();
    libkineto::api().configLoader().initBaseConfig();
    initialized = true;
    VLOG(0) << "libkineto profilers activated";
  }
}

#endif // __linux__ || defined(HAS_MUPTI)

#ifdef HAS_MUPTI
bool enableEventProfiler() {
  if (getenv("KINETO_ENABLE_EVENT_PROFILER") != nullptr) {
    return true;
  } else {
    return false;
  }
}

static void initProfilersCallback(
    MUpti_CallbackDomain /*domain*/,
    MUpti_CallbackId /*cbid*/,
    const MUpti_CallbackData* /*cbInfo*/) {
  VLOG(0) << "MUSA Context created";
  initProfilers();

  if (enableEventProfiler()) {
    LOG(WARNING) << "Event Profiler is no longer supported in kineto";
  }
}

// Some models suffer from excessive instrumentation code gen
// on dynamic attach which can hang for more than 5+ seconds.
// If the workload was meant to be traced, preload the MUPTI
// to take the performance hit early on.
// https://docs.nvidia.com/mupti/r_main.html#r_overhead
static bool shouldPreloadMuptiInstrumentation() {
#if defined(MUSA_VERSION) && MUSA_VERSION < 11020
  return true;
#else
  return false;
#endif
}

bool setupMuptiInitCallback(bool logOnError) {
  // libmupti will be lazily loaded on this call.
  // If it is not available (e.g. MUSA is not installed),
  // then this call will return an error and we just abort init.
  auto cbapi = MuptiCallbackApi::singleton();
  cbapi->initCallbackApi();

  bool status = false;

  if (cbapi->initSuccess()) {
    const MUpti_CallbackDomain domain = MUPTI_CB_DOMAIN_RESOURCE;
    status = cbapi->registerCallback(
        domain,
        MuptiCallbackApi::RESOURCE_CONTEXT_CREATED,
        initProfilersCallback);
    if (status) {
      status = cbapi->enableCallback(
          domain, MuptiCallbackApi::RESOURCE_CONTEXT_CREATED);
    }
  }

  if (!cbapi->initSuccess() || !status) {
    if (logOnError) {
      MUPTI_CALL(cbapi->getMuptiStatus());
      LOG(WARNING) << "MUPTI initialization failed - "
                   << "MUSA profiler activities will be missing";
      LOG(INFO)
          << "If you see MUPTI_ERROR_INSUFFICIENT_PRIVILEGES, refer to "
          << "https://developer.nvidia.com/nvidia-development-tools-solutions-err-nvgpuctrperm-mupti";
    }
  }

  return status;
}

static std::unique_ptr<MuptiRangeProfilerInit> rangeProfilerInit;
#endif // HAS_MUPTI

} // namespace KINETO_NAMESPACE

// Callback interface with MUPTI and library constructors
using namespace KINETO_NAMESPACE;
extern "C" {

void libkineto_init(bool cpuOnly, bool logOnError) {
  // Start with initializing the log level
  const char* logLevelEnv = getenv("KINETO_LOG_LEVEL");
  if (logLevelEnv) {
    // atoi returns 0 on error, so that's what we want - default to VERBOSE
    static_assert(static_cast<int>(VERBOSE) == 0, "");
    SET_LOG_SEVERITY_LEVEL(atoi(logLevelEnv));
  }

  // Factory to connect to open source daemon if present
#if __linux__
  if (libkineto::isDaemonEnvVarSet()) {
    LOG(INFO) << "Registering daemon config loader, cpuOnly =  " << cpuOnly;
    DaemonConfigLoader::registerFactory();
  }
#endif

#ifdef HAS_MUPTI
  bool initRangeProfiler = true;

  if (!cpuOnly && !libkineto::isDaemonEnvVarSet()) {
    bool success = setupMuptiInitCallback(logOnError);
    cpuOnly = !success;
    initRangeProfiler = success;
  }

  // Initialize MUPTI Range Profiler API
  // Note: the following is a no-op if Range Profiler is not supported
  // currently it is only enabled in fbcode.
  if (!cpuOnly && initRangeProfiler) {
    rangeProfilerInit = std::make_unique<MuptiRangeProfilerInit>();
  }

  if (!cpuOnly && shouldPreloadMuptiInstrumentation()) {
    MuptiActivityApi::forceLoadMupti();
  }
#endif // HAS_MUPTI

  ConfigLoader& config_loader = libkineto::api().configLoader();
  libkineto::api().registerProfiler(
      std::make_unique<ActivityProfilerProxy>(cpuOnly, config_loader));

#if __linux__
  // For open source users that would like to connect to a profiling daemon
  // we should always initialize the profiler at this point.
  if (libkineto::isDaemonEnvVarSet()) {
    initProfilers();
  }
#endif
}

// The musa driver calls this function if the MUSA_INJECTION64_PATH environment
// variable is set. Should be skipped if unset or MUSA_INJECTION64_PATH=none.
int InitializeInjection(void) {
  LOG(INFO) << "Injection mode: Initializing libkineto";
  libkineto_init(false /*cpuOnly*/, true /*logOnError*/);
  return 1;
}

bool hasTestEnvVar() {
  return getenv("GTEST_OUTPUT") != nullptr || getenv("FB_TEST") != nullptr ||
      getenv("PYTORCH_TEST") != nullptr || getenv("TEST_PILOT") != nullptr;
}

void suppressLibkinetoLogMessages() {
  // Only suppress messages if explicit override wasn't provided
  const char* logLevelEnv = getenv("KINETO_LOG_LEVEL");
  // For unit tests, don't suppress log verbosity.
  if (!hasTestEnvVar() && (!logLevelEnv || !*logLevelEnv)) {
    SET_LOG_SEVERITY_LEVEL(ERROR);
  }
}

} // extern C
