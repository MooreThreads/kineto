/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <musa_runtime.h>
#include <iostream>
#include <string>

#include <libkineto.h>

// @lint-ignore-every CLANGTIDY facebook-hte-RelativeInclude
#include "kineto_playground.cuh"

#define CHECK_MUSA(call)               \
  do {                                 \
    musaError_t status = call;         \
    if (status != musaSuccess) {       \
      fprintf(                         \
          stderr,                      \
          "MUSA Error at %s:%d: %s\n", \
          __FILE__,                    \
          __LINE__,                    \
          musaGetErrorString(status)); \
      exit(1);                         \
    }                                  \
  } while (0)

using namespace kineto;

static const std::string kFileName = "/tmp/kineto_playground_trace.json";

int main() {
  std::cout << "Warm up " << std::endl;
  warmup();

  // Kineto config
  libkineto_init(false, true);

  // Empty types set defaults to all types
  std::set<libkineto::ActivityType> types;

  auto& profiler = libkineto::api().activityProfiler();
  libkineto::api().initProfilerIfRegistered();
  profiler.prepareTrace(types);

  // Good to warm up after prepareTrace to get mupti initialization to settle
  std::cout << "Warm up " << std::endl;
  warmup();
  std::cout << "Start trace" << std::endl;
  profiler.startTrace();
  std::cout << "Start playground" << std::endl;
  playground();
  CHECK_MUSA(musaDeviceSynchronize());

  std::cout << "Stop Trace" << std::endl;
  auto trace = profiler.stopTrace();
  std::cout << "Stopped and processed trace. Got "
            << trace->activities()->size() << " activities.";
  trace->save(kFileName);
  return 0;
}
