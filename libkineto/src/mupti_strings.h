/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <mupti.h>

namespace libkineto {

const char* memoryKindString(MUpti_ActivityMemoryKind kind);
const char* memcpyKindString(MUpti_ActivityMemcpyKind kind);
const char* memoryAtomicKindString(MUpti_ActivityMemoryAtomicKind kind);
const char* memoryAtomicValueKindString(MUpti_ActivityMemoryAtomicValueKind kind);
const char* runtimeCbidName(MUpti_CallbackId cbid);
const char* overheadKindString(MUpti_ActivityOverheadKind kind);
const char* syncTypeString(MUpti_ActivitySynchronizationType kind);

} // namespace libkineto
