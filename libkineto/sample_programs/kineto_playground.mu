/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <stdio.h>

#include "kineto_playground.muh"


namespace kineto {

void warmup(void) {
  // Inititalizing MUSA can take a while which we normally do not want to see in Kineto traces.
  // This is done in various ways that take Kineto as dependency. This is our way of doing warmup
  // for kineto_playground
	size_t bytes = 1000;
	float* mem = NULL;
	auto error = musaMalloc(&mem, bytes);
  if (error != musaSuccess) {
    printf("musaMalloc failed during kineto_playground warmup. error code: %d", error);
    return;
  }

  musaFree(mem);
}

float *hA, *dA, *hOut;
int num = 50'000;

void basicMemcpyToDevice(void) {
  size_t size = num * sizeof(float);
  musaError_t err;

  hA = (float*)malloc(size);
  hOut = (float*)malloc(size);
  err = musaMalloc(&dA, size);
  if (err != musaSuccess) {
    printf("musaMalloc failed during %s", __func__);
    return;
  }

  memset(hA, 1, size);
  err = musaMemcpy(dA, hA, size, musaMemcpyHostToDevice);
  if (err != musaSuccess) {
    printf("musaMemcpy failed during %s", __func__);
    return;
  }
}

void basicMemcpyFromDevice(void) {

  size_t size = num * sizeof(float);
  musaError_t err;

  err = musaMemcpy(hOut, dA, size, musaMemcpyDeviceToHost);
  if (err != musaSuccess) {
    printf("musaMemcpy failed during %s", __func__);
    return;
  }

  free(hA);
  free(hOut);
  musaFree(dA);
}

__global__ void square(float* A, int N) {
  int i = blockDim.x * blockIdx.x + threadIdx.x;
  if (i < N) {
    A[i] *= A[i];
  }
}

void playground(void) {
  // Add your experimental MUSA implementation here.
}

void compute(void) {
  int threadsPerBlock = 256;
  int blocksPerGrid = (num + threadsPerBlock - 1) / threadsPerBlock;
  for (int i = 0; i < 10; i++) {
    square<<<blocksPerGrid, threadsPerBlock>>> (dA, num);
  }
}

} // namespace kineto
