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
#include "DaemonConfigLoader.h"
#ifdef HAS_MUPTI
#include "MuptiCallbackApi.h"
#include "MuptiActivityApi.h"
#include "MuptiRangeProfiler.h"
#include "EventProfilerController.h"
#endif
#include "mupti_call.h"
#include "libkineto.h"

#include "Logger.h"

namespace KINETO_NAMESPACE {

#ifdef HAS_MUPTI
static bool initialized = false;
static std::mutex initMutex;

bool enableEventProfiler() {
  if (getenv("KINETO_ENABLE_EVENT_PROFILER") != nullptr) {
    return true;
  } else {
    return false;
  }
}

static void initProfilers(
    MUpti_CallbackDomain /*domain*/,
    MUpti_CallbackId /*cbid*/,
    const MUpti_CallbackData* cbInfo) {
  VLOG(0) << "MUSA Context created";
  std::lock_guard<std::mutex> lock(initMutex);

  if (!initialized) {
    libkineto::api().initProfilerIfRegistered();
    initialized = true;
    VLOG(0) << "libkineto profilers activated";
  }

  if (!enableEventProfiler()) {
    VLOG(0) << "Kineto EventProfiler disabled, skipping start";
    return;
  } else {
    MUpti_ResourceData* d = (MUpti_ResourceData*)cbInfo;
    MUcontext ctx = d->context;
    ConfigLoader& config_loader = libkineto::api().configLoader();
    config_loader.initBaseConfig();
    auto config = config_loader.getConfigCopy();
    if (config->eventProfilerEnabled()) {
      EventProfilerController::start(ctx, config_loader);
      LOG(INFO) << "Kineto EventProfiler started";
    }
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

static void stopProfiler(
    MUpti_CallbackDomain /*domain*/,
    MUpti_CallbackId /*cbid*/,
    const MUpti_CallbackData* cbInfo) {
  VLOG(0) << "MUSA Context destroyed";
  std::lock_guard<std::mutex> lock(initMutex);

  if (enableEventProfiler()) {
    MUpti_ResourceData* d = (MUpti_ResourceData*)cbInfo;
    MUcontext ctx = d->context;
    EventProfilerController::stopIfEnabled(ctx);
    LOG(INFO) << "Kineto EventProfiler stopped";
  }
}

static std::unique_ptr<MuptiRangeProfilerInit> rangeProfilerInit;
#endif // HAS_MUPTI

} // namespace KINETO_NAMESPACE

// Callback interface with MUPTI and library constructors
using namespace KINETO_NAMESPACE;
extern "C" {

// Return true if no MUPTI errors occurred during init
void libkineto_init(bool cpuOnly, bool logOnError) {
  // Start with initializing the log level
  const char* logLevelEnv = getenv("KINETO_LOG_LEVEL");
  if (logLevelEnv) {
    // atoi returns 0 on error, so that's what we want - default to VERBOSE
    static_assert (static_cast<int>(VERBOSE) == 0, "");
    SET_LOG_SEVERITY_LEVEL(atoi(logLevelEnv));
  }

  // Factory to connect to open source daemon if present
#if __linux__
  if (getenv(kUseDaemonEnvVar) != nullptr) {
    LOG(INFO) << "Registering daemon config loader";
    DaemonConfigLoader::registerFactory();
  }
#endif

#ifdef HAS_MUPTI
  if (!cpuOnly) {
    // libmupti will be lazily loaded on this call.
    // If it is not available (e.g. MUSA is not installed),
    // then this call will return an error and we just abort init.
    auto cbapi = MuptiCallbackApi::singleton();
    cbapi->initCallbackApi();
    bool status = false;
    bool initRangeProfiler = true;

    if (cbapi->initSuccess()){
      const MUpti_CallbackDomain domain = MUPTI_CB_DOMAIN_RESOURCE;
      status = cbapi->registerCallback(
          domain, MuptiCallbackApi::RESOURCE_CONTEXT_CREATED, initProfilers);
      status = status && cbapi->registerCallback(
          domain, MuptiCallbackApi::RESOURCE_CONTEXT_DESTROYED, stopProfiler);

      if (status) {
        status = cbapi->enableCallback(
            domain, MuptiCallbackApi::RESOURCE_CONTEXT_CREATED);
        status = status && cbapi->enableCallback(
            domain, MuptiCallbackApi::RESOURCE_CONTEXT_DESTROYED);
      }
    }

    if (!cbapi->initSuccess() || !status) {
      initRangeProfiler = false;
      cpuOnly = true;
      if (logOnError) {
        MUPTI_CALL(cbapi->getMuptiStatus());
        LOG(WARNING) << "MUPTI initialization failed - "
                     << "MUSA profiler activities will be missing";
        LOG(INFO) << "If you see MUPTI_ERROR_INSUFFICIENT_PRIVILEGES, refer to "
                  << "https://developer.nvidia.com/nvidia-development-tools-solutions-err-nvgpuctrperm-mupti";
      }
    }

    // initialize MUPTI Range Profiler API
    if (initRangeProfiler) {
      rangeProfilerInit = std::make_unique<MuptiRangeProfilerInit>();
    }
  }

  if (shouldPreloadMuptiInstrumentation()) {
    MuptiActivityApi::forceLoadMupti();
  }
#endif // HAS_MUPTI

  ConfigLoader& config_loader = libkineto::api().configLoader();
  libkineto::api().registerProfiler(
      std::make_unique<ActivityProfilerProxy>(cpuOnly, config_loader));

}

// The musa driver calls this function if the MUSA_INJECTION64_PATH environment
// variable is set
int InitializeInjection(void) {
  LOG(INFO) << "Injection mode: Initializing libkineto";
  libkineto_init(false /*cpuOnly*/, true /*logOnError*/);
  return 1;
}

void suppressLibkinetoLogMessages() {
  // Only suppress messages if explicit override wasn't provided
  const char* logLevelEnv = getenv("KINETO_LOG_LEVEL");
  if (!logLevelEnv || !*logLevelEnv) {
    SET_LOG_SEVERITY_LEVEL(ERROR);
  }
}

} // extern C
