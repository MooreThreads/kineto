# Copyright (c) Meta Platforms, Inc. and affiliates.

# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

def get_libkineto_api_srcs():
    return [
        "src/ThreadUtil.cpp",
        "src/libkineto_api.cpp",
    ]

def get_libkineto_mupti_srcs(with_api = True):
    return [
        "src/MusaDeviceProperties.cpp",
        "src/MusaUtil.cpp",
        "src/MuptiActivityApi.cpp",
        "src/MuptiActivityPlatform.cpp",
        "src/MuptiCallbackApi.cpp",
        "src/MuptiEventApi.cpp",
        "src/MuptiMetricApi.cpp",
        "src/MuptiRangeProfiler.cpp",
        "src/MuptiRangeProfilerApi.cpp",
        "src/MuptiRangeProfilerConfig.cpp",
        "src/MuptiNvPerfMetric.cpp",
        "src/Demangle.cpp",
        "src/EventProfiler.cpp",
        "src/EventProfilerController.cpp",
        "src/WeakSymbols.cpp",
        "src/mupti_strings.cpp",
    ] + (get_libkineto_cpu_only_srcs(with_api))

def get_libkineto_roctracer_srcs(with_api = True):
    return [
        "src/RoctracerActivityApi.cpp",
        "src/RoctracerLogger.cpp",
    ] + (get_libkineto_cpu_only_srcs(with_api))

def get_libkineto_cpu_only_srcs(with_api = True):
    return [
        "src/AbstractConfig.cpp",
        "src/MuptiActivityProfiler.cpp",
        "src/ActivityProfilerController.cpp",
        "src/ActivityProfilerProxy.cpp",
        "src/ActivityType.cpp",
        "src/Config.cpp",
        "src/ConfigLoader.cpp",
        "src/MuptiActivityApi.cpp",
        "src/DaemonConfigLoader.cpp",
        "src/Demangle.cpp",
        "src/GenericTraceActivity.cpp",
        "src/ILoggerObserver.cpp",
        "src/IpcFabricConfigClient.cpp",
        "src/Logger.cpp",
        "src/LoggingAPI.cpp",
        "src/init.cpp",
        "src/output_csv.cpp",
        "src/output_json.cpp",
    ] + (get_libkineto_api_srcs() if with_api else [])

def get_libkineto_public_headers():
    return [
        "include/AbstractConfig.h",
        "include/ActivityProfilerInterface.h",
        "include/ActivityTraceInterface.h",
        "include/ActivityType.h",
        "include/Config.h",
        "include/ClientInterface.h",
        "include/GenericTraceActivity.h",
        "include/IActivityProfiler.h",
        "include/ILoggerObserver.h",
        "include/ITraceActivity.h",
        "include/LoggingAPI.h",
        "include/TraceSpan.h",
        "include/ThreadUtil.h",
        "include/libkineto.h",
        "include/time_since_epoch.h",
    ]

# kineto code should be updated to not have to
# suppress these warnings.
KINETO_COMPILER_FLAGS = [
    "-fexceptions",
    "-Wno-deprecated-declarations",
    "-Wno-unused-function",
    "-Wno-unused-private-field",
]
