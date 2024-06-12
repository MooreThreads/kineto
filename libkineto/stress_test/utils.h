/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <musa_runtime_api.h>
#include <musa.h>

#include <stdexcept>
#include <string>

#include "nccl.h"

namespace kineto_stress_test {

inline void checkMusaStatus(musaError_t status, int lineNumber = -1) {
  if (status != musaSuccess) {
    printf(
        "PID %d --> MUSA API failed with status %d: %s at line %d\n",
        getpid(),
        status,
        musaGetErrorString(status),
        lineNumber);
    exit(EXIT_FAILURE);
  }
}

#define MUSA_CHECK(EXPR)                            \
  do {                                              \
    const musaError_t err = EXPR;                   \
    if (err == musaSuccess) {                       \
      break;                                        \
    }                                               \
    std::string error_message;                      \
    error_message.append(__FILE__);                 \
    error_message.append(":");                      \
    error_message.append(std::to_string(__LINE__)); \
    error_message.append(" MUSA error: ");          \
    error_message.append(musaGetErrorString(err));  \
    throw std::runtime_error(error_message);        \
  } while (0)

#define MUSA_KERNEL_LAUNCH_CHECK() MUSA_CHECK(musaGetLastError())

#define MPICHECK(cmd) do {                              \
  int e = cmd;                                          \
  if( e != MPI_SUCCESS ) {                              \
    printf("PID %d --> Failed: MPI error %s:%d '%d'\n", \
        getpid(), __FILE__,__LINE__, e);                \
    exit(EXIT_FAILURE);                                 \
  }                                                     \
} while(0)

#define NCCLCHECK(cmd) do {                                 \
  ncclResult_t r = cmd;                                     \
  if (r!= ncclSuccess) {                                    \
    printf("PID %d --> Failed, NCCL error %s:%d '%s'\n",    \
        getpid(), __FILE__,__LINE__,ncclGetErrorString(r)); \
    exit(EXIT_FAILURE);                                     \
  }                                                         \
} while(0)

} //namespace kineto_stress_test
