/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <mupti.h>

#include <fmt/format.h>

// TODO(T90238193)
// @lint-ignore-every CLANGTIDY facebook-hte-RelativeInclude
#include "MuptiCbidRegistry.h"
#include "MuptiTimestamp.h"
#include "Demangle.h"
#include "DeviceProperties.h"
#include "GenericTraceActivity.h"
#include "ITraceActivity.h"
#include "Logger.h"
#include "MetadataFieldCatalog.h"
#include "ThreadUtil.h"
#include "TypedMetadataJson.h"
#include "mupti_strings.h"
#include "output_base.h"

namespace libkineto {
class ActivityLogger;
}

namespace KINETO_NAMESPACE {

using namespace libkineto;
struct TraceSpan;

// These classes wrap the various MUPTI activity types
// into subclasses of ITraceActivity so that they can all be accessed
// using the ITraceActivity interface and logged via ActivityLogger.

// Abstract base class, templated on Mupti activity type
template <class T>
struct MuptiActivity : public ITraceActivity {
  explicit MuptiActivity(const T* activity, const ITraceActivity* linked)
      : activity_(*activity), linked_(linked) {}
  int64_t timestamp() const override {
    return convertMuptiTimestamp(activity_.start);
  }

  int64_t duration() const override {
    return convertMuptiDuration(activity_.start, activity_.end);
  }
  // TODO(T107507796): Deprecate ITraceActivity
  int64_t correlationId() const override {
    return 0;
  }
  int32_t getThreadId() const override {
    return 0;
  }
  const ITraceActivity* linkedActivity() const override {
    return linked_;
  }
  int flowType() const override {
    return kLinkAsyncCpuGpu;
  }
  int64_t flowId() const override {
    return correlationId();
  }
  const T& raw() const {
    return activity_;
  }
  const TraceSpan* traceSpan() const override {
    return nullptr;
  }

 protected:
  const T& activity_;
  const ITraceActivity* linked_{nullptr};
};

// MUpti_ActivityAPI - MUSA runtime activities
struct RuntimeActivity : public MuptiActivity<MUpti_ActivityAPI> {
  explicit RuntimeActivity(
      const MUpti_ActivityAPI* activity,
      const ITraceActivity* linked,
      int32_t threadId)
      : MuptiActivity(activity, linked), threadId_(threadId) {}
  int64_t correlationId() const override {
    return activity_.correlationId;
  }
  int64_t deviceId() const override {
    return processId();
  }
  int64_t resourceId() const override {
    return threadId_;
  }
  ActivityType type() const override {
    return ActivityType::PRIVATEUSE1_RUNTIME;
  }
  bool flowStart() const override;
  const std::string name() const override {
    return runtimeCbidName(activity_.cbid);
  }
  void log(ActivityLogger& logger) const override;
  const std::string metadataJson() const override;
  void visitTypedMetadata(ITypedMetadataVisitor& visitor) const override;

 private:
  const int32_t threadId_;
};

// MUpti_ActivityAPI - MUSA driver activities
struct DriverActivity : public MuptiActivity<MUpti_ActivityAPI> {
  explicit DriverActivity(
      const MUpti_ActivityAPI* activity,
      const ITraceActivity* linked,
      int32_t threadId)
      : MuptiActivity(activity, linked), threadId_(threadId) {}
  int64_t correlationId() const override {
    return activity_.correlationId;
  }
  int64_t deviceId() const override {
    return processId();
  }
  int64_t resourceId() const override {
    return threadId_;
  }
  ActivityType type() const override {
    return ActivityType::PRIVATEUSE1_DRIVER;
  }
  bool flowStart() const override;
  const std::string name() const override;
  void log(ActivityLogger& logger) const override;
  const std::string metadataJson() const override;
  void visitTypedMetadata(ITypedMetadataVisitor& visitor) const override;

 private:
  const int32_t threadId_;
};

// MUpti_ActivityAPI - MUSA runtime activities
struct OverheadActivity : public MuptiActivity<MUpti_ActivityOverhead> {
  explicit OverheadActivity(
      const MUpti_ActivityOverhead* activity,
      const ITraceActivity* linked,
      int32_t threadId = 0)
      : MuptiActivity(activity, linked), threadId_(threadId) {}

  int64_t timestamp() const override {
    return convertMuptiTimestamp(activity_.start);
  }

  int64_t duration() const override {
    return convertMuptiDuration(activity_.start, activity_.end);
  }

  // TODO: Update this with PID ordering
  int64_t deviceId() const override {
    return -1;
  }
  int64_t resourceId() const override {
    return threadId_;
  }
  ActivityType type() const override {
    return ActivityType::OVERHEAD;
  }
  bool flowStart() const override;
  const std::string name() const override {
    return overheadKindString(activity_.overheadKind);
  }
  void log(ActivityLogger& logger) const override;
  const std::string metadataJson() const override;
  void visitTypedMetadata(ITypedMetadataVisitor& visitor) const override;

 private:
  const int32_t threadId_;
};

// MUpti_ActivitySynchronization - MUSA synchronization events
struct MusaSyncActivity : public MuptiActivity<MUpti_ActivitySynchronization> {
  explicit MusaSyncActivity(
      const MUpti_ActivitySynchronization* activity,
      const ITraceActivity* linked,
      int32_t srcStream,
      int32_t srcCorrId)
      : MuptiActivity(activity, linked),
        srcStream_(srcStream),
        srcCorrId_(srcCorrId) {}
  int64_t correlationId() const override {
    return raw().correlationId;
  }
  int64_t deviceId() const override;
  int64_t resourceId() const override;
  ActivityType type() const override {
    return ActivityType::MUSA_SYNC;
  }
  bool flowStart() const override {
    return false;
  }
  const std::string name() const override;
  void log(ActivityLogger& logger) const override;
  const std::string metadataJson() const override;
  void visitTypedMetadata(ITypedMetadataVisitor& visitor) const override;
  const MUpti_ActivitySynchronization& raw() const {
    return MuptiActivity<MUpti_ActivitySynchronization>::raw();
  }

 private:
  const int32_t srcStream_;
  const int32_t srcCorrId_;
};

// Use MUpti_ActivityMusaEvent in MUSA 12.8+ for enhanced event tracking
#if MUSA_VERSION >= 12080
using MUpti_ActivityMusaEventType = MUpti_ActivityMusaEvent;
#else
using MUpti_ActivityMusaEventType = MUpti_ActivityMusaEvent;
#endif

// Template specialization for timestamp() and duration() for MusaEvent types
template <>
inline int64_t MuptiActivity<MUpti_ActivityMusaEventType>::timestamp() const {
  return 0;
}

template <>
inline int64_t MuptiActivity<MUpti_ActivityMusaEventType>::duration() const {
  // Set duration to 1 to make events visible in trace
  return 1;
}

// MUpti_ActivityMusaEvent - MUSA event activities
struct MusaEventActivity : public MuptiActivity<MUpti_ActivityMusaEventType> {
  explicit MusaEventActivity(
      const MUpti_ActivityMusaEventType* activity,
      const ITraceActivity* linked)
      : MuptiActivity(activity, linked) {}

  int64_t correlationId() const override {
    return raw().correlationId;
  }
  int64_t deviceId() const override;
  int64_t resourceId() const override;
  ActivityType type() const override {
    return ActivityType::MUSA_EVENT;
  }
  bool flowStart() const override {
    return false;
  }
  const std::string name() const override;
  void log(ActivityLogger& logger) const override;
  const std::string metadataJson() const override;
  void visitTypedMetadata(ITypedMetadataVisitor& visitor) const override;
  const MUpti_ActivityMusaEventType& raw() const {
    return MuptiActivity<MUpti_ActivityMusaEventType>::raw();
  }
};

// Base class for GPU activities.
// Can also be instantiated directly.
template <class T>
struct GpuActivity : public MuptiActivity<T> {
  explicit GpuActivity(const T* activity, const ITraceActivity* linked)
      : MuptiActivity<T>(activity, linked) {}
  int64_t correlationId() const override {
    return raw().correlationId;
  }
  int64_t deviceId() const override {
    return raw().deviceId;
  }
  int64_t resourceId() const override {
    return raw().streamId;
  }
  ActivityType type() const override;
  bool flowStart() const override {
    return false;
  }
  const std::string name() const override;
  void log(ActivityLogger& logger) const override;
  const std::string metadataJson() const override;
  void visitTypedMetadata(ITypedMetadataVisitor& visitor) const override;
  const T& raw() const {
    return MuptiActivity<T>::raw();
  }
};

// forward declaration
uint32_t contextIdtoDeviceId(uint32_t contextId);

template <>
inline const std::string GpuActivity<MUpti_ActivityKernelType>::name() const {
  return demangle(raw().name);
}

template <>
inline ActivityType GpuActivity<MUpti_ActivityKernelType>::type() const {
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

inline int32_t streamIdForTrace(uint32_t streamId) {
  // MUPTI uses all bits set when no stream applies. Preserve the legacy trace
  // representation where that sentinel appears as -1 instead of 4294967295.
  if (streamId == static_cast<uint32_t>(MUPTI_SYNCHRONIZATION_INVALID_VALUE)) {
    return -1;
  }
  return static_cast<int32_t>(streamId);
}

inline const std::string MusaSyncActivity::name() const {
  return syncTypeString(raw().type);
}

inline int64_t MusaSyncActivity::deviceId() const {
  return contextIdtoDeviceId(raw().contextId);
}

inline int64_t MusaSyncActivity::resourceId() const {
  return streamIdForTrace(raw().streamId);
}

inline void MusaSyncActivity::log(ActivityLogger& logger) const {
  logger.handleActivity(*this);
}

inline const std::string MusaSyncActivity::metadataJson() const {
  libkineto::internal::JsonTypedMetadataVisitor visitor;
  visitTypedMetadata(visitor);
  return std::move(visitor).json();
}

inline void MusaSyncActivity::visitTypedMetadata(
    ITypedMetadataVisitor& visitor) const {
  const MUpti_ActivitySynchronization& sync = raw();
  visitor.visit(
      MusaMetadataFields::kMusaSyncKind,
      std::string{syncTypeString(sync.type)});
  if (isEventSync(sync.type)) {
    visitor.visit(
        MusaMetadataFields::kWaitOnStream, static_cast<int64_t>(srcStream_));
    visitor.visit(
        MusaMetadataFields::kWaitOnMusaEventRecordCorrId,
        static_cast<int64_t>(srcCorrId_));
    visitor.visit(
        MusaMetadataFields::kWaitOnMusaEventId,
        static_cast<uint64_t>(sync.musaEventId));
  }
  visitor.visit(
      MusaMetadataFields::kStream,
      static_cast<int64_t>(streamIdForTrace(sync.streamId)));
  visitor.visit(
      MusaMetadataFields::kCorrelation,
      static_cast<uint64_t>(sync.correlationId));
  visitor.visit(MusaMetadataFields::kDevice, deviceId());
  visitor.visit(
      MusaMetadataFields::kContext, static_cast<uint64_t>(sync.contextId));
}

inline const std::string MusaEventActivity::name() const {
  return "musaEvent";
}

inline int64_t MusaEventActivity::deviceId() const {
  return contextIdtoDeviceId(raw().contextId);
}

inline int64_t MusaEventActivity::resourceId() const {
  return streamIdForTrace(raw().streamId);
}

inline void MusaEventActivity::log(ActivityLogger& logger) const {
  logger.handleActivity(*this);
}

inline const std::string MusaEventActivity::metadataJson() const {
  libkineto::internal::JsonTypedMetadataVisitor visitor;
  visitTypedMetadata(visitor);
  return std::move(visitor).json();
}

inline void MusaEventActivity::visitTypedMetadata(
    ITypedMetadataVisitor& visitor) const {
  const MUpti_ActivityMusaEventType& event = raw();
  visitor.visit(
      MusaMetadataFields::kEventId, static_cast<uint64_t>(event.eventId));
  visitor.visit(
      MusaMetadataFields::kStream,
      static_cast<int64_t>(streamIdForTrace(event.streamId)));
  visitor.visit(
      MusaMetadataFields::kCorrelation,
      static_cast<uint64_t>(event.correlationId));
  visitor.visit(MusaMetadataFields::kDevice, deviceId());
  visitor.visit(
      MusaMetadataFields::kContext, static_cast<uint64_t>(event.contextId));
}

template <class T>
inline void GpuActivity<T>::log(ActivityLogger& logger) const {
  logger.handleActivity(*this);
}

constexpr int64_t us(int64_t timestamp) {
  // It's important that this conversion is the same here and in the CPU trace.
  // No rounding!
  return timestamp / 1000;
}

template <class T>
inline void addGraphNodeTypedMetadata(
    [[maybe_unused]] ITypedMetadataVisitor& visitor,
    [[maybe_unused]] const T& activity) {
#if defined(MUSA_VERSION) && MUSA_VERSION >= 12000
  visitor.visit(
      MusaMetadataFields::kGraphId, static_cast<uint64_t>(activity.graphId));
  visitor.visit(
      MusaMetadataFields::kGraphNodeId,
      static_cast<uint64_t>(activity.graphNodeId));
#endif
}

template <class T>
inline void addPriorityTypedMetadata(
    [[maybe_unused]] ITypedMetadataVisitor& visitor,
    [[maybe_unused]] const T& kernel) {
#if defined(MUSA_VERSION) && MUSA_VERSION >= 13010
  visitor.visit(
      MusaMetadataFields::kPriority, static_cast<int64_t>(kernel.priority));
#endif
}

template <class T>
inline void addChannelTypedMetadata(
    [[maybe_unused]] ITypedMetadataVisitor& visitor,
    [[maybe_unused]] const T& activity) {
#if defined(MUSA_VERSION) && MUSA_VERSION >= 12000
  visitor.visit(
      MusaMetadataFields::kChannel, static_cast<uint64_t>(activity.channelID));
  visitor.visit(
      MusaMetadataFields::kChannelType,
      static_cast<int64_t>(activity.channelType));
#endif
}

inline void addBandwidthTypedMetadata(
    [[maybe_unused]] ITypedMetadataVisitor& visitor,
    uint64_t bytes,
    int64_t duration) {
  if (duration == 0) {
    return;
  }
  visitor.visit(
      MusaMetadataFields::kMemoryBandwidthGbps,
      static_cast<double>(bytes) / static_cast<double>(duration));
}

// Convert limitingFactors bitmask to human-readable string
// Based on musaOccLimitingFactor enum from musa_occupancy.h
// This can be found in the MUSA toolkit typically
// /usr/local/musa/targets/x86_64-linux/include/musa_occupancy.h
inline std::string limitingFactorsToString(unsigned int factors) {
  if (factors == 0) {
    return "none";
  }
  constexpr std::pair<unsigned int, const char*> kFactors[] = {
      {OCC_LIMIT_WARPS, "WARPS"},
      {OCC_LIMIT_REGISTERS, "REGS"},
      {OCC_LIMIT_SHARED_MEMORY, "SMEM"},
      {OCC_LIMIT_BLOCKS, "BLOCKS"},
  };
  std::string result;
  for (const auto& [mask, name] : kFactors) {
    if (factors & mask) {
      if (!result.empty()) {
        result += "|";
      }
      result += name;
    }
  }
  return result;
}

template <>
inline void GpuActivity<MUpti_ActivityKernelType>::visitTypedMetadata(
    ITypedMetadataVisitor& visitor) const {
  const MUpti_ActivityKernelType& kernel = raw();
  const float blocksPerSmVal = blocksPerSm(kernel);
  const float warpsPerSmVal = warpsPerSm(kernel);
  const OccupancyMetrics occMetrics = computeOccupancyMetrics(kernel);

  visitor.visit(
      MusaMetadataFields::kQueued, static_cast<uint64_t>(kernel.queued));
  visitor.visit(
      MusaMetadataFields::kDevice, static_cast<int64_t>(kernel.deviceId));
  visitor.visit(
      MusaMetadataFields::kContext, static_cast<uint64_t>(kernel.contextId));
  visitor.visit(
      MusaMetadataFields::kStream, static_cast<int64_t>(kernel.streamId));
  visitor.visit(
      MusaMetadataFields::kCorrelation,
      static_cast<uint64_t>(kernel.correlationId));
  visitor.visit(
      MusaMetadataFields::kRegistersPerThread,
      static_cast<uint64_t>(kernel.registersPerThread));
  visitor.visit(
      MusaMetadataFields::kSharedMemory,
      static_cast<int64_t>(
          kernel.staticSharedMemory + kernel.dynamicSharedMemory));
  visitor.visit(
      MusaMetadataFields::kBlocksPerSm, static_cast<double>(blocksPerSmVal));
  visitor.visit(
      MusaMetadataFields::kWarpsPerSm, static_cast<double>(warpsPerSmVal));
  visitor.visit(
      MusaMetadataFields::kGrid,
      std::vector<int64_t>{
          static_cast<int64_t>(kernel.gridX),
          static_cast<int64_t>(kernel.gridY),
          static_cast<int64_t>(kernel.gridZ)});
  visitor.visit(
      MusaMetadataFields::kBlock,
      std::vector<int64_t>{
          static_cast<int64_t>(kernel.blockX),
          static_cast<int64_t>(kernel.blockY),
          static_cast<int64_t>(kernel.blockZ)});
  visitor.visit(
      MusaMetadataFields::kEstAchievedOccupancyPercent,
      static_cast<int64_t>(std::lround(occMetrics.occupancy * 100.0)));
  visitor.visit(MusaMetadataFields::kOccupancy, [&](auto& d) {
    d.visit(
        MusaMetadataFields::kActiveBlocksPerMultiprocessor,
        static_cast<int64_t>(occMetrics.result.activeBlocksPerMultiprocessor));
    d.visit(
        MusaMetadataFields::kLimitingFactors,
        limitingFactorsToString(occMetrics.result.limitingFactors));
    d.visit(
        MusaMetadataFields::kBlockLimitRegs,
        static_cast<int64_t>(occMetrics.result.blockLimitRegs));
    d.visit(
        MusaMetadataFields::kBlockLimitSharedMem,
        static_cast<int64_t>(occMetrics.result.blockLimitSharedMem));
    d.visit(
        MusaMetadataFields::kBlockLimitWarps,
        static_cast<int64_t>(occMetrics.result.blockLimitWarps));
    d.visit(
        MusaMetadataFields::kBlockLimitBlocks,
        static_cast<int64_t>(occMetrics.result.blockLimitBlocks));
    d.visit(
        MusaMetadataFields::kAllocatedRegistersPerBlock,
        static_cast<int64_t>(occMetrics.result.allocatedRegistersPerBlock));
    d.visit(
        MusaMetadataFields::kAllocatedSharedMemPerBlock,
        static_cast<uint64_t>(occMetrics.result.allocatedSharedMemPerBlock));
  });
  addGraphNodeTypedMetadata(visitor, kernel);
}

template <>
inline const std::string GpuActivity<MUpti_ActivityKernelType>::metadataJson()
    const {
  libkineto::internal::JsonTypedMetadataVisitor visitor;
  visitTypedMetadata(visitor);
  return std::move(visitor).json();
}

inline std::string memcpyName(uint8_t kind, uint8_t src, uint8_t dst) {
  return fmt::format(
      "Memcpy {} ({} -> {})",
      memcpyKindString((MUpti_ActivityMemcpyKind)kind),
      memoryKindString((MUpti_ActivityMemoryKind)src),
      memoryKindString((MUpti_ActivityMemoryKind)dst));
}

template <>
inline ActivityType GpuActivity<MUpti_ActivityMemcpyType>::type() const {
  return ActivityType::GPU_MEMCPY;
}

template <>
inline const std::string GpuActivity<MUpti_ActivityMemcpyType>::name() const {
  return memcpyName(raw().copyKind, raw().srcKind, raw().dstKind);
}

template <>
inline void GpuActivity<MUpti_ActivityMemcpyType>::visitTypedMetadata(
    ITypedMetadataVisitor& visitor) const {
  const MUpti_ActivityMemcpyType& memcpy = raw();
  visitor.visit(
      MusaMetadataFields::kDevice, static_cast<int64_t>(memcpy.deviceId));
  visitor.visit(
      MusaMetadataFields::kContext, static_cast<uint64_t>(memcpy.contextId));
  visitor.visit(
      MusaMetadataFields::kStream, static_cast<int64_t>(memcpy.streamId));
  visitor.visit(
      MusaMetadataFields::kCorrelation,
      static_cast<uint64_t>(memcpy.correlationId));
  visitor.visit(
      MusaMetadataFields::kBytes, static_cast<uint64_t>(memcpy.bytes));
  addBandwidthTypedMetadata(visitor, memcpy.bytes, duration());
  addGraphNodeTypedMetadata(visitor, memcpy);
}

template <>
inline const std::string GpuActivity<MUpti_ActivityMemcpyType>::metadataJson()
    const {
  libkineto::internal::JsonTypedMetadataVisitor visitor;
  visitTypedMetadata(visitor);
  return std::move(visitor).json();
}

template <>
inline ActivityType GpuActivity<MUpti_ActivityMemcpyPtoPType>::type() const {
  return ActivityType::GPU_MEMCPY;
}

template <>
inline const std::string GpuActivity<MUpti_ActivityMemcpyPtoPType>::name()
    const {
  return memcpyName(raw().copyKind, raw().srcKind, raw().dstKind);
}

template <>
inline void GpuActivity<MUpti_ActivityMemcpyPtoPType>::visitTypedMetadata(
    ITypedMetadataVisitor& visitor) const {
  const MUpti_ActivityMemcpyPtoPType& memcpy = raw();
  visitor.visit(
      MusaMetadataFields::kFromDevice,
      static_cast<uint64_t>(memcpy.srcDeviceId));
  visitor.visit(
      MusaMetadataFields::kInDevice, static_cast<uint64_t>(memcpy.deviceId));
  visitor.visit(
      MusaMetadataFields::kToDevice, static_cast<uint64_t>(memcpy.dstDeviceId));
  visitor.visit(
      MusaMetadataFields::kFromContext,
      static_cast<uint64_t>(memcpy.srcContextId));
  visitor.visit(
      MusaMetadataFields::kInContext, static_cast<uint64_t>(memcpy.contextId));
  visitor.visit(
      MusaMetadataFields::kToContext,
      static_cast<uint64_t>(memcpy.dstContextId));
  visitor.visit(
      MusaMetadataFields::kStream, static_cast<int64_t>(memcpy.streamId));
  visitor.visit(
      MusaMetadataFields::kCorrelation,
      static_cast<uint64_t>(memcpy.correlationId));
  visitor.visit(
      MusaMetadataFields::kBytes, static_cast<uint64_t>(memcpy.bytes));
  addBandwidthTypedMetadata(visitor, memcpy.bytes, duration());
  addGraphNodeTypedMetadata(visitor, memcpy);
}

template <>
inline ActivityType GpuActivity<MUpti_ActivityMemoryAtomic>::type() const {
  return ActivityType::GPU_MEMCPY;
}

template <>
inline const std::string GpuActivity<MUpti_ActivityMemoryAtomic>::name() const {
  const auto& atomic = raw();
  return fmt::format(
      "Memcpy2 {} ({} -> {})",
      memoryAtomicKindString(static_cast<MUpti_ActivityMemoryAtomicKind>(atomic.atomicKind)),
      memoryKindString(static_cast<MUpti_ActivityMemoryKind>(atomic.srcKind)),
      memoryKindString(static_cast<MUpti_ActivityMemoryKind>(atomic.dstKind)));
}

template <>
inline void GpuActivity<MUpti_ActivityMemoryAtomic>::visitTypedMetadata(
    ITypedMetadataVisitor& visitor) const {
  const auto& atomic = raw();
  visitor.visit(MusaMetadataFields::kDevice, static_cast<int64_t>(atomic.deviceId));
  visitor.visit(MusaMetadataFields::kContext, static_cast<uint64_t>(atomic.contextId));
  visitor.visit(MusaMetadataFields::kStream, static_cast<int64_t>(atomic.streamId));
  visitor.visit(MusaMetadataFields::kCorrelation, static_cast<uint64_t>(atomic.correlationId));
  visitor.visit(MusaMetadataFields::kElementCount, static_cast<uint64_t>(atomic.elementCount));
  addGraphNodeTypedMetadata(visitor, atomic);
}

template <>
inline ActivityType GpuActivity<MUpti_ActivityMemoryAtomicValue>::type() const {
  return ActivityType::GPU_MEMCPY;
}

template <>
inline const std::string GpuActivity<MUpti_ActivityMemoryAtomicValue>::name() const {
  const auto& atomic = raw();
  return fmt::format(
      "Memcpy3 {} ({})",
      memoryAtomicValueKindString(static_cast<MUpti_ActivityMemoryAtomicValueKind>(atomic.atomicValueKind)),
      memoryKindString(static_cast<MUpti_ActivityMemoryKind>(atomic.dstKind)));
}

template <>
inline void GpuActivity<MUpti_ActivityMemoryAtomicValue>::visitTypedMetadata(
    ITypedMetadataVisitor& visitor) const {
  const auto& atomic = raw();
  visitor.visit(MusaMetadataFields::kDevice, static_cast<int64_t>(atomic.deviceId));
  visitor.visit(MusaMetadataFields::kContext, static_cast<uint64_t>(atomic.contextId));
  visitor.visit(MusaMetadataFields::kStream, static_cast<int64_t>(atomic.streamId));
  visitor.visit(MusaMetadataFields::kCorrelation, static_cast<uint64_t>(atomic.correlationId));
  visitor.visit(MusaMetadataFields::kAtomicValue, static_cast<uint64_t>(atomic.value));
  addGraphNodeTypedMetadata(visitor, atomic);
}

template <>
inline ActivityType GpuActivity<MUpti_ActivityMemoryTransfer>::type() const {
  return ActivityType::GPU_MEMCPY;
}

template <>
inline const std::string GpuActivity<MUpti_ActivityMemoryTransfer>::name() const {
  return "MemTransfer(d2d)";
}

template <>
inline void GpuActivity<MUpti_ActivityMemoryTransfer>::visitTypedMetadata(
    ITypedMetadataVisitor& visitor) const {
  const auto& transfer = raw();
  visitor.visit(MusaMetadataFields::kDevice, static_cast<int64_t>(transfer.deviceId));
  visitor.visit(MusaMetadataFields::kContext, static_cast<uint64_t>(transfer.contextId));
  visitor.visit(MusaMetadataFields::kStream, static_cast<int64_t>(transfer.streamId));
  visitor.visit(MusaMetadataFields::kCorrelation, static_cast<uint64_t>(transfer.correlationId));
  visitor.visit(MusaMetadataFields::kBytes, static_cast<uint64_t>(transfer.bytes));
  addBandwidthTypedMetadata(visitor, transfer.bytes, duration());
  addGraphNodeTypedMetadata(visitor, transfer);
}

template <>
inline const std::string GpuActivity<MUpti_ActivityMemoryAtomic>::metadataJson() const {
  libkineto::internal::JsonTypedMetadataVisitor visitor;
  visitTypedMetadata(visitor);
  return std::move(visitor).json();
}

template <>
inline const std::string GpuActivity<MUpti_ActivityMemoryAtomicValue>::metadataJson() const {
  libkineto::internal::JsonTypedMetadataVisitor visitor;
  visitTypedMetadata(visitor);
  return std::move(visitor).json();
}

template <>
inline const std::string GpuActivity<MUpti_ActivityMemoryTransfer>::metadataJson() const {
  libkineto::internal::JsonTypedMetadataVisitor visitor;
  visitTypedMetadata(visitor);
  return std::move(visitor).json();
}
template <>
inline const std::string GpuActivity<MUpti_ActivityMemsetType>::name() const {
  const char* memory_kind =
      memoryKindString((MUpti_ActivityMemoryKind)raw().memoryKind);
  return fmt::format("Memset ({})", memory_kind);
}

template <>
inline ActivityType GpuActivity<MUpti_ActivityMemsetType>::type() const {
  return ActivityType::GPU_MEMSET;
}

template <>
inline void GpuActivity<MUpti_ActivityMemsetType>::visitTypedMetadata(
    ITypedMetadataVisitor& visitor) const {
  const MUpti_ActivityMemsetType& memset = raw();
  visitor.visit(
      MusaMetadataFields::kDevice, static_cast<int64_t>(memset.deviceId));
  visitor.visit(
      MusaMetadataFields::kContext, static_cast<uint64_t>(memset.contextId));
  visitor.visit(
      MusaMetadataFields::kStream, static_cast<int64_t>(memset.streamId));
  visitor.visit(
      MusaMetadataFields::kCorrelation,
      static_cast<uint64_t>(memset.correlationId));
  visitor.visit(
      MusaMetadataFields::kBytes, static_cast<uint64_t>(memset.bytes));
  addBandwidthTypedMetadata(visitor, memset.bytes, duration());
  addGraphNodeTypedMetadata(visitor, memset);
}

template <>
inline const std::string GpuActivity<MUpti_ActivityMemsetType>::metadataJson()
    const {
  libkineto::internal::JsonTypedMetadataVisitor visitor;
  visitTypedMetadata(visitor);
  return std::move(visitor).json();
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
  libkineto::internal::JsonTypedMetadataVisitor visitor;
  visitTypedMetadata(visitor);
  return std::move(visitor).json();
}

inline void OverheadActivity::visitTypedMetadata(
    [[maybe_unused]] ITypedMetadataVisitor& visitor) const {}

inline bool RuntimeActivity::flowStart() const {
  return MuptiCbidRegistry::instance().requiresFlowCorrelation(
      CallbackDomain::RUNTIME, activity_.cbid);
}

inline const std::string RuntimeActivity::metadataJson() const {
  libkineto::internal::JsonTypedMetadataVisitor visitor;
  visitTypedMetadata(visitor);
  return std::move(visitor).json();
}

inline void RuntimeActivity::visitTypedMetadata(
    ITypedMetadataVisitor& visitor) const {
  visitor.visit(
      MusaMetadataFields::kCbid, static_cast<uint64_t>(activity_.cbid));
  visitor.visit(
      MusaMetadataFields::kCorrelation,
      static_cast<uint64_t>(activity_.correlationId));
}

inline bool isTrackedDriverCbid(const MUpti_ActivityAPI& activity_) {
  return MuptiCbidRegistry::instance().isRegistered(
      CallbackDomain::DRIVER, activity_.cbid);
}

inline bool DriverActivity::flowStart() const {
  return MuptiCbidRegistry::instance().requiresFlowCorrelation(
      CallbackDomain::DRIVER, activity_.cbid);
}

inline const std::string DriverActivity::metadataJson() const {
  libkineto::internal::JsonTypedMetadataVisitor visitor;
  visitTypedMetadata(visitor);
  return std::move(visitor).json();
}

inline void DriverActivity::visitTypedMetadata(
    ITypedMetadataVisitor& visitor) const {
  visitor.visit(
      MusaMetadataFields::kCbid, static_cast<uint64_t>(activity_.cbid));
  visitor.visit(
      MusaMetadataFields::kCorrelation,
      static_cast<uint64_t>(activity_.correlationId));
}

inline const std::string DriverActivity::name() const {
  return driverCbidName(activity_.cbid);
}

template <class T>
inline const std::string GpuActivity<T>::metadataJson() const {
  libkineto::internal::JsonTypedMetadataVisitor visitor;
  visitTypedMetadata(visitor);
  return std::move(visitor).json();
}

template <class T>
inline void GpuActivity<T>::visitTypedMetadata(
    [[maybe_unused]] ITypedMetadataVisitor& visitor) const {}

} // namespace KINETO_NAMESPACE
