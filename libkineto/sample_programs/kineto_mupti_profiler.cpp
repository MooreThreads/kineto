/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <chrono>
#include <thread>
#include <iostream>

#include <libkineto.h>

// @lint-ignore-every CLANGTIDY facebook-hte-RelativeInclude
#include "kineto_playground.muh"

using namespace kineto;

static const std::string kFileName = "./kineto_playground_trace.json";

int main() {
  warmup();

  // Kineto config
  std::set<libkineto::ActivityType> types_mupti_prof = {
    libkineto::ActivityType::CPU_OP,
    libkineto::ActivityType::USER_ANNOTATION,
    libkineto::ActivityType::GPU_USER_ANNOTATION,
    libkineto::ActivityType::GPU_MEMCPY,
    libkineto::ActivityType::GPU_MEMSET,
    libkineto::ActivityType::CONCURRENT_KERNEL,
    libkineto::ActivityType::EXTERNAL_CORRELATION,
    libkineto::ActivityType::CUDA_RUNTIME,
    libkineto::ActivityType::CUDA_DRIVER,
    libkineto::ActivityType::CPU_INSTANT_EVENT,
    libkineto::ActivityType::PYTHON_FUNCTION,
    libkineto::ActivityType::OVERHEAD,
    libkineto::ActivityType::MTIA_RUNTIME,
    libkineto::ActivityType::MTIA_CCP_EVENTS,
    libkineto::ActivityType::CUDA_SYNC,
    libkineto::ActivityType::GLOW_RUNTIME,
    libkineto::ActivityType::CUDA_PROFILER_RANGE,
    libkineto::ActivityType::HPU_OP,
    libkineto::ActivityType::XPU_RUNTIME,
    libkineto::ActivityType::COLLECTIVE_COMM,
    libkineto::ActivityType::MTIA_WORKLOADD,
    libkineto::ActivityType::PRIVATEUSE1_RUNTIME,
    libkineto::ActivityType::PRIVATEUSE1_DRIVER,
  };

  libkineto_init(false, true);
  libkineto::api().initProfilerIfRegistered();

  // Use a special kineto__musa_core_flop metric that counts individual
  // MUSA core floating point instructions by operation type (fma,fadd,fmul,dadd ...)
  // You can also use kineto__tensor_core_insts or any metric
  // or any metric defined by MUPTI Profiler below
  //   https://docs.nvidia.com/mupti/Mupti/r_main.html#r_profiler

  std::string profiler_config = "ACTIVITIES_WARMUP_PERIOD_SECS=0\n "
    "MUPTI_PROFILER_METRICS=kineto__musa_core_flops\n "
    "MUPTI_PROFILER_ENABLE_PER_KERNEL=true";

  auto& profiler = libkineto::api().activityProfiler();
  profiler.prepareTrace(types_mupti_prof, profiler_config);

  // Good to warm up after prepareTrace to get mupti initialization to settle
  warmup();

  profiler.startTrace();
  basicMemcpyToDevice();
  compute();
  basicMemcpyFromDevice();

  auto trace = profiler.stopTrace();
  std::cout << "Stopped and processed trace. Got " << trace->activities()->size() << " activities.";
  trace->save(kFileName);
  return 0;
}
