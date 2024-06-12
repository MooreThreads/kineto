/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// Provides data structures to mock MUPTI Callback API
#ifndef HAS_MUPTI

enum MUpti_CallbackDomain {
  MUPTI_CB_DOMAIN_RESOURCE,
  MUPTI_CB_DOMAIN_RUNTIME_API,
};
enum MUpti_CallbackId {
  MUPTI_RUNTIME_TRACE_CBID_musaLaunchKernel_v7000,
  MUPTI_CBID_RESOURCE_CONTEXT_CREATED,
  MUPTI_CBID_RESOURCE_CONTEXT_DESTROY_STARTING,
};

using MUcontext = void*;

struct MUpti_ResourceData {
  MUcontext context;
};

constexpr int MUPTI_API_ENTER = 0;
constexpr int MUPTI_API_EXIT = 0;

struct MUpti_CallbackData {
  MUcontext context;
  const char* symbolName;
  int callbackSite;
};
#endif // HAS_MUPTI
