/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MuptiActivity.h"

#include <fmt/format.h>

#include "MusaDeviceProperties.h"
#include "Demangle.h"
#include "output_base.h"

namespace KINETO_NAMESPACE {

using namespace libkineto;

// forward declaration
uint32_t contextIdtoDeviceId(uint32_t contextId);

template<>
inline const std::string GpuActivity<MUpti_ActivityKernel6>::name() const {
  return demangle(raw().name);
}

template<>
inline ActivityType GpuActivity<MUpti_ActivityKernel6>::type() const {
  return ActivityType::CONCURRENT_KERNEL;
}

inline bool isWaitEventSync(MUpti_ActivitySynchronizationType type) {
  return (type == MUPTI_ACTIVITY_SYNCHRONIZATION_TYPE_STREAM_WAIT_EVENT);
}

inline bool isEventSync(MUpti_ActivitySynchronizationType type) {
  return (
    type == MUPTI_ACTIVITY_SYNCHRONIZATION_TYPE_EVENT_SYNCHRONIZE ||
    type == MUPTI_ACTIVITY_SYNCHRONIZATION_TYPE_STREAM_WAIT_EVENT);
}

inline std::string eventSyncInfo(
    const MUpti_ActivitySynchronization& act,
    int32_t srcStream,
    int32_t srcCorrId
    ) {
  return fmt::format(R"JSON(
      "wait_on_stream": {},
      "wait_on_musa_event_record_corr_id": {},
      "wait_on_musa_event_id": {},)JSON",
      srcStream,
      srcCorrId,
      act.musaEventId
  );
}

inline const std::string MusaSyncActivity::name() const {
  return syncTypeString(raw().type);
}

inline int64_t MusaSyncActivity::deviceId() const {
  return contextIdtoDeviceId(raw().contextId);
}

inline int64_t MusaSyncActivity::resourceId() const {
  // For Context and Device Sync events stream ID is invalid and
  // set to MUPTI_SYNCHRONIZATION_INVALID_VALUE (-1)
  // converting to an integer will automatically wrap the number to -1
  // in the trace.
  return static_cast<int32_t>(raw().streamId);
}

inline void MusaSyncActivity::log(ActivityLogger& logger) const {
  logger.handleActivity(*this);
}

inline const std::string MusaSyncActivity::metadataJson() const {
  const MUpti_ActivitySynchronization& sync = raw();
  // clang-format off
  return fmt::format(R"JSON(
      "musa_sync_kind": "{}",{}
      "stream": {}, "correlation": {},
      "device": {}, "context": {})JSON",
      syncTypeString(sync.type),
      isEventSync(raw().type) ? eventSyncInfo(raw(), srcStream_, srcCorrId_) : "",
      static_cast<int32_t>(sync.streamId), sync.correlationId,
      deviceId(), sync.contextId);
  // clang-format on
  return "";
}

template<class T>
inline void GpuActivity<T>::log(ActivityLogger& logger) const {
  logger.handleActivity(*this);
}

constexpr int64_t us(int64_t timestamp) {
  // It's important that this conversion is the same here and in the CPU trace.
  // No rounding!
  return timestamp / 1000;
}

template<>
inline const std::string GpuActivity<MUpti_ActivityKernel6>::metadataJson() const {
  const MUpti_ActivityKernel6& kernel = raw();
  float blocksPerSmVal = blocksPerSm(kernel);
  float warpsPerSmVal = warpsPerSm(kernel);

  // clang-format off
  return fmt::format(R"JSON(
      "queued": {}, "device": {}, "context": {},
      "stream": {}, "correlation": {},
      "registers per thread": {},
      "shared memory": {},
      "blocks per SM": {},
      "warps per SM": {},
      "grid": [{}, {}, {}],
      "block": [{}, {}, {}],
      "est. achieved occupancy %": {})JSON",
      us(kernel.queued), kernel.deviceId, kernel.contextId,
      kernel.streamId, kernel.correlationId,
      kernel.registersPerThread,
      kernel.staticSharedMemory + kernel.dynamicSharedMemory,
      std::isinf(blocksPerSmVal) ? "\"inf\"" : std::to_string(blocksPerSmVal),
      std::isinf(warpsPerSmVal) ? "\"inf\"" : std::to_string(warpsPerSmVal),
      kernel.gridX, kernel.gridY, kernel.gridZ,
      kernel.blockX, kernel.blockY, kernel.blockZ,
      (int) (0.5 + kernelOccupancy(kernel) * 100.0));
  // clang-format on
}


inline std::string memcpyName(uint8_t kind, uint8_t src, uint8_t dst) {
  return fmt::format(
      "Memcpy {} ({} -> {})",
      memcpyKindString((MUpti_ActivityMemcpyKind)kind),
      memoryKindString((MUpti_ActivityMemoryKind)src),
      memoryKindString((MUpti_ActivityMemoryKind)dst));
}



inline std::string memoryAtomicName(uint8_t kind, uint8_t src, uint8_t dst) {
  return fmt::format(
      "Memcpy {} ({} -> {})",
      memoryAtomicKindString((MUpti_ActivityMemoryAtomicKind)kind),
      memoryKindString((MUpti_ActivityMemoryKind)src),
      memoryKindString((MUpti_ActivityMemoryKind)dst));
}

inline std::string memoryAtomicValueName(uint8_t kind, uint8_t dst) {
  return fmt::format(
      "Memcpy {} ({})",
      memoryAtomicValueKindString((MUpti_ActivityMemoryAtomicValueKind)kind),
      memoryKindString((MUpti_ActivityMemoryKind)dst));
}


template<>
inline ActivityType GpuActivity<MUpti_ActivityMemcpy4>::type() const {
  return ActivityType::GPU_MEMCPY;
}

template<>
inline const std::string GpuActivity<MUpti_ActivityMemcpy4>::name() const {
  return memcpyName(raw().copyKind, raw().srcKind, raw().dstKind);
}

inline std::string bandwidth(uint64_t bytes, uint64_t duration) {
  return duration == 0 ? "\"N/A\"" : fmt::format("{}", bytes * 1.0 / duration);
}

template<>
inline const std::string GpuActivity<MUpti_ActivityMemcpy4>::metadataJson() const {
  const MUpti_ActivityMemcpy4& memcpy = raw();
  // clang-format off
  return fmt::format(R"JSON(
      "device": {}, "context": {},
      "stream": {}, "correlation": {},
      "bytes": {}, "memory bandwidth (GB/s)": {})JSON",
      memcpy.deviceId, memcpy.contextId,
      memcpy.streamId, memcpy.correlationId,
      memcpy.bytes, bandwidth(memcpy.bytes, memcpy.end - memcpy.start));
  // clang-format on
}


template<>
inline ActivityType GpuActivity<MUpti_ActivityMemcpy2>::type() const {
  return ActivityType::GPU_MEMCPY;
}

template<>
inline const std::string GpuActivity<MUpti_ActivityMemcpy2>::name() const {
  return memcpyName(raw().copyKind, raw().srcKind, raw().dstKind);
}

template<>
inline const std::string GpuActivity<MUpti_ActivityMemcpy2>::metadataJson() const {
  const MUpti_ActivityMemcpy2& memcpy = raw();
  // clang-format off
  return fmt::format(R"JSON(
      "fromDevice": {}, "inDevice": {}, "toDevice": {},
      "fromContext": {}, "inContext": {}, "toContext": {},
      "stream": {}, "correlation": {},
      "bytes": {}, "memory bandwidth (GB/s)": {})JSON",
      memcpy.srcDeviceId, memcpy.deviceId, memcpy.dstDeviceId,
      memcpy.srcContextId, memcpy.contextId, memcpy.dstContextId,
      memcpy.streamId, memcpy.correlationId,
      memcpy.bytes, bandwidth(memcpy.bytes, memcpy.end - memcpy.start));
  // clang-format on
}

template<>
inline ActivityType GpuActivity<MUpti_ActivityMemoryAtomic>::type() const {
  return ActivityType::GPU_MEMCPY;
}

template<>
inline const std::string GpuActivity<MUpti_ActivityMemoryAtomic>::name() const {
  return memoryAtomicName(raw().atomicKind, raw().srcKind, raw().dstKind);
}

template<>
inline const std::string GpuActivity<MUpti_ActivityMemoryAtomic>::metadataJson() const {
  const MUpti_ActivityMemoryAtomic& memcpy = raw();
  // clang-format off
  return fmt::format(R"JSON(
      "device": {}, "context": {},
      "stream": {}, "correlation": {},
      "element count": {})JSON",
      memcpy.deviceId, memcpy.contextId,
      memcpy.streamId, memcpy.correlationId,
      memcpy.elementCount);
  // clang-format on
}


template<>
inline ActivityType GpuActivity<MUpti_ActivityMemoryAtomicValue>::type() const {
  return ActivityType::GPU_MEMCPY;
}

template<>
inline const std::string GpuActivity<MUpti_ActivityMemoryAtomicValue>::name() const {
  return memoryAtomicValueName(raw().atomicValueKind, raw().dstKind);
}

template<>
inline const std::string GpuActivity<MUpti_ActivityMemoryAtomicValue>::metadataJson() const {
  const MUpti_ActivityMemoryAtomicValue& memcpy = raw();
  // clang-format off
  return fmt::format(R"JSON(
      "device": {}, "context": {},"stream": {},
      "correlation": {}, "graph node": {}, "graph": {})JSON",
      memcpy.deviceId, memcpy.contextId, memcpy.streamId,
      memcpy.correlationId, memcpy.graphNodeId, memcpy.graphId);
  // clang-format on
}

template<>
inline const std::string GpuActivity<MUpti_ActivityMemset>::name() const {
  const char* memory_kind =
    memoryKindString((MUpti_ActivityMemoryKind)raw().memoryKind);
  return fmt::format("Memset ({})", memory_kind);
}

template<>
inline ActivityType GpuActivity<MUpti_ActivityMemset>::type() const {
  return ActivityType::GPU_MEMSET;
}

template<>
inline const std::string GpuActivity<MUpti_ActivityMemset>::metadataJson() const {
  const MUpti_ActivityMemset& memset = raw();
  // clang-format off
  return fmt::format(R"JSON(
      "device": {}, "context": {},
      "stream": {}, "correlation": {},
      "bytes": {}, "memory bandwidth (GB/s)": {})JSON",
      memset.deviceId, memset.contextId,
      memset.streamId, memset.correlationId,
      memset.bytes, bandwidth(memset.bytes, memset.end - memset.start));
  // clang-format on
}

inline void RuntimeActivity::log(ActivityLogger& logger) const {
  logger.handleActivity(*this);
}

inline void DriverActivity::log(ActivityLogger& logger) const {
  logger.handleActivity(*this);
}

inline void OverheadActivity::log(ActivityLogger& logger) const {
  logger.handleActivity(*this);
}

inline bool OverheadActivity::flowStart() const {
  return false;
}

inline const std::string OverheadActivity::metadataJson() const {
  return "";
}

inline bool RuntimeActivity::flowStart() const {
  bool should_correlate =
      activity_.cbid == MUPTI_RUNTIME_TRACE_CBID_musaLaunchKernel_v7000 ||
      (activity_.cbid >= MUPTI_RUNTIME_TRACE_CBID_musaMemcpy_v3020 &&
       activity_.cbid <= MUPTI_RUNTIME_TRACE_CBID_musaMemset2DAsync_v3020) ||
      activity_.cbid ==
          MUPTI_RUNTIME_TRACE_CBID_musaLaunchCooperativeKernel_v9000 ||
      activity_.cbid ==
          MUPTI_RUNTIME_TRACE_CBID_musaLaunchCooperativeKernelMultiDevice_v9000 ||
      activity_.cbid == MUPTI_RUNTIME_TRACE_CBID_musaGraphLaunch_v10000 ||
      activity_.cbid == MUPTI_RUNTIME_TRACE_CBID_musaStreamSynchronize_v3020 ||
      activity_.cbid == MUPTI_RUNTIME_TRACE_CBID_musaDeviceSynchronize_v3020 ||
      activity_.cbid == MUPTI_RUNTIME_TRACE_CBID_musaStreamWaitEvent_v3020;

/* Todo: why here set 14
  - when porting mupti, define MUPTI_API_VERSION=14 in musa runtine
  - when porting kineto, the macro definition here is used directly, did not do test and adapt to cover each details.
  - setting to 14, but do not test on S4000 Kuae 1.3.
*/  
#if defined(MUPTI_API_VERSION) && MUPTI_API_VERSION >= 14
  should_correlate |=
      activity_.cbid == MUPTI_RUNTIME_TRACE_CBID_musaLaunchKernelExC_v11060;
#endif
  return should_correlate;
}

inline const std::string RuntimeActivity::metadataJson() const {
  return fmt::format(R"JSON(
      "cbid": {}, "correlation": {})JSON",
      activity_.cbid, activity_.correlationId);
}

inline bool isKernelLaunchApi(const MUpti_ActivityAPI& activity_) {
  return activity_.cbid == MUPTI_DRIVER_TRACE_CBID_muLaunchKernel;
}

inline bool DriverActivity::flowStart() const {
  return isKernelLaunchApi(activity_);
}

inline const std::string DriverActivity::metadataJson() const {
  return fmt::format(R"JSON(
      "cbid": {}, "correlation": {})JSON",
      activity_.cbid, activity_.correlationId);
}

inline const std::string DriverActivity::name() const {
  // currently only muLaunchKernel is expected
  assert(isKernelLaunchApi(activity_));
  // not yet implementing full name matching
  if (activity_.cbid == MUPTI_DRIVER_TRACE_CBID_muLaunchKernel) {
    return "muLaunchKernel";
  } else {
    return "Unknown"; // should not reach here
  }
}

template<class T>
inline const std::string GpuActivity<T>::metadataJson() const {
  return "";
}

} // namespace KINETO_NAMESPACE
