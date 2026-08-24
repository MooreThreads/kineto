/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "mupti_strings.h"

#include <string>

namespace libkineto {

const char* memcpyKindString(MUpti_ActivityMemcpyKind kind) {
  switch (kind) {
    case MUPTI_ACTIVITY_MEMCPY_KIND_HTOD:
      return "HtoD";
    case MUPTI_ACTIVITY_MEMCPY_KIND_DTOH:
      return "DtoH";
    case MUPTI_ACTIVITY_MEMCPY_KIND_HTOA:
      return "HtoA";
    case MUPTI_ACTIVITY_MEMCPY_KIND_ATOH:
      return "AtoH";
    case MUPTI_ACTIVITY_MEMCPY_KIND_ATOA:
      return "AtoA";
    case MUPTI_ACTIVITY_MEMCPY_KIND_ATOD:
      return "AtoD";
    case MUPTI_ACTIVITY_MEMCPY_KIND_DTOA:
      return "DtoA";
    case MUPTI_ACTIVITY_MEMCPY_KIND_DTOD:
      return "DtoD";
    case MUPTI_ACTIVITY_MEMCPY_KIND_HTOH:
      return "HtoH";
    case MUPTI_ACTIVITY_MEMCPY_KIND_PTOP:
      return "PtoP";
    default:
      break;
  }
  return "<unknown>";
}

const char* memoryAtomicKindString(MUpti_ActivityMemoryAtomicKind kind) {
  switch (kind) {
    case MUPTI_ACTIVITY_MEMCPY_ATOMIC_KIND_UNKNOWN:
      return "Unknown";
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_KIND_ATOMIC_ADD32:
      return "Atomic Add32";
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_KIND_ATOMIC_ADD64:
      return "Atomic Add64";
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_KIND_ATOMIC_UMIN32:
      return "Atomic UMin32";
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_KIND_ATOMIC_UMIN64:
      return "Atomic UMin64";
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_KIND_ATOMIC_IMIN32:
      return "Atomic IMin32";
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_KIND_ATOMIC_IMIN64:
      return "Atomic IMin64";
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_KIND_ATOMIC_UMAX32:
      return "Atomic UMax32";
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_KIND_ATOMIC_UMAX64:
      return "Atomic UMax64";
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_KIND_ATOMIC_IMAX32:
      return "Atomic IMax32";
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_KIND_ATOMIC_IMAX64:
      return "Atomic IMax64";
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_KIND_ATOMIC_ADD_F32:
      return "Atomic AddF32";
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_KIND_ATOMIC_ADD_F64:
      return "Atomic AddF64";
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_KIND_ATOMIC_ADD_HF16:
      return "Atomic AddHF16";
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_KIND_ATOMIC_ADD_BF16:
      return "Atomic AddBF16";
    default:
      return "Unrecognized";
  }
}

const char* memoryAtomicValueKindString(
    MUpti_ActivityMemoryAtomicValueKind kind) {
  switch (kind) {
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_VALUE_KIND_UNKNOWN:
      return "Unknown";
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_VALUE_KIND_ATOMIC_ADD64:
      return "Atomic Add64";
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_VALUE_KIND_ATOMIC_SUB64:
      return "Atomic Sub64";
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_VALUE_KIND_ATOMIC_UMIN64:
      return "Atomic UMin64";
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_VALUE_KIND_ATOMIC_IMIN64:
      return "Atomic IMin64";
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_VALUE_KIND_ATOMIC_UMAX64:
      return "Atomic UMax64";
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_VALUE_KIND_ATOMIC_IMAX64:
      return "Atomic IMax64";
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_VALUE_KIND_ATOMIC_AND64:
      return "Atomic And64";
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_VALUE_KIND_ATOMIC_OR64:
      return "Atomic Or64";
    case MUPTI_ACTIVITY_MEMORY_ATOMIC_VALUE_KIND_ATOMIC_XOR64:
      return "Atomic Xor64";
    default:
      return "Unrecognized";
  }
}

const char* memoryKindString(MUpti_ActivityMemoryKind kind) {
  switch (kind) {
    case MUPTI_ACTIVITY_MEMORY_KIND_UNKNOWN:
      return "Unknown";
    case MUPTI_ACTIVITY_MEMORY_KIND_PAGEABLE:
      return "Pageable";
    case MUPTI_ACTIVITY_MEMORY_KIND_PINNED:
      return "Pinned";
    case MUPTI_ACTIVITY_MEMORY_KIND_DEVICE:
      return "Device";
    case MUPTI_ACTIVITY_MEMORY_KIND_ARRAY:
      return "Array";
    case MUPTI_ACTIVITY_MEMORY_KIND_MANAGED:
      return "Managed";
    case MUPTI_ACTIVITY_MEMORY_KIND_DEVICE_STATIC:
      return "Device Static";
    case MUPTI_ACTIVITY_MEMORY_KIND_MANAGED_STATIC:
      return "Managed Static";
    case MUPTI_ACTIVITY_MEMORY_KIND_FORCE_INT:
      return "Force Int";
    default:
      return "Unrecognized";
  }
}

const char* overheadKindString(MUpti_ActivityOverheadKind kind) {
  switch (kind) {
    case MUPTI_ACTIVITY_OVERHEAD_UNKNOWN:
      return "Unknown";
    case MUPTI_ACTIVITY_OVERHEAD_DRIVER_COMPILER:
      return "Driver Compiler";
    case MUPTI_ACTIVITY_OVERHEAD_MUPTI_BUFFER_FLUSH:
      return "Buffer Flush";
    case MUPTI_ACTIVITY_OVERHEAD_MUPTI_INSTRUMENTATION:
      return "Instrumentation";
    case MUPTI_ACTIVITY_OVERHEAD_MUPTI_RESOURCE:
      return "Resource";
#if MUSART_VERSION >= 12040
#endif
#if MUSART_VERSION >= 12070
#endif
    case MUPTI_ACTIVITY_OVERHEAD_FORCE_INT:
      return "Force Int";
    default:
      return "Unrecognized";
  }
}

namespace {

// MUPTI returns identifiers like `musaLaunchKernel_v7000` whose trailing
// `_vNNNN` is the MUSA-version-introduced suffix. Strip it to match the
// existing trace-label convention, while preserving single-digit
// API-generation suffixes like `_v2` / `_v3` (lowest MUSA-version suffix is
// `_v3020` for MUSA 3.2, so 4+ trailing digits is unambiguous).
std::string lookupCbidName(MUpti_CallbackDomain domain, MUpti_CallbackId cbid) {
  const char* raw = nullptr;
  if (muptiGetCallbackName(domain, cbid, &raw) != MUPTI_SUCCESS ||
      raw == nullptr) {
    return "INVALID";
  }
  std::string name = raw;
  if (name.empty()) {
    return "INVALID";
  }
  const auto vPos = name.find_last_not_of("0123456789");
  if (vPos != std::string::npos && vPos >= 1 && name[vPos] == 'v' &&
      name[vPos - 1] == '_' && name.size() - vPos - 1 >= 4) {
    name.resize(vPos - 1);
  }
  return name;
}

} // namespace

std::string runtimeCbidName(MUpti_CallbackId cbid) {
  return lookupCbidName(MUPTI_CB_DOMAIN_RUNTIME_API, cbid);
}

std::string driverCbidName(MUpti_CallbackId cbid) {
  return lookupCbidName(MUPTI_CB_DOMAIN_DRIVER_API, cbid);
}

// From
// https://docs.nvidia.com/mupti/modules.html#group__MUPTI__ACTIVITY__API_1g80e1eb47615e31021f574df8ebbe5d9a
//   enum MUpti_ActivitySynchronizationType
const char* syncTypeString(MUpti_ActivitySynchronizationType kind) {
  switch (kind) {
    case MUPTI_ACTIVITY_SYNCHRONIZATION_TYPE_EVENT_SYNCHRONIZE:
      return "Event Sync";
    case MUPTI_ACTIVITY_SYNCHRONIZATION_TYPE_STREAM_WAIT_EVENT:
      return "Stream Wait Event";
    case MUPTI_ACTIVITY_SYNCHRONIZATION_TYPE_STREAM_SYNCHRONIZE:
      return "Stream Sync";
    case MUPTI_ACTIVITY_SYNCHRONIZATION_TYPE_CONTEXT_SYNCHRONIZE:
      return "Context Sync";
    case MUPTI_ACTIVITY_SYNCHRONIZATION_TYPE_UNKNOWN:
    default:
      return "Unknown Sync";
  }
}
} // namespace libkineto
