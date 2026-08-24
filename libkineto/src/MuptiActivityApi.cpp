/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MuptiActivityApi.h"

#include <chrono>
#include <mutex>
#include <thread>

#include "DeviceUtil.h"
#include "Logger.h"

using namespace std::chrono;

namespace KINETO_NAMESPACE {

// Set to 4MB to avoid constantly creating buffers (especially for networks
// that have many small memcpy such as sparseNN). MUPTI recommends between
// 1MB to 10MB.
// Given the kDefaultActivitiesMaxGpuBufferSize is around 128MB, in the worst
// case, there will be 32 buffers contending for the mutex.
constexpr size_t kBufSize(4 * 1024 * 1024);
constexpr uint32_t kMuptiBufferRejectionMinVersion = 27;

inline bool muptiTearDown_() {
  auto teardown_env = getenv("TEARDOWN_MUPTI");
  return teardown_env != nullptr && strcmp(teardown_env, "1") == 0;
}

inline bool muptiLazyInit_() {
  return muptiTearDown_() && getenv("DISABLE_MUPTI_LAZY_REINIT") == nullptr;
}

inline void reenableMuptiCallbacks_(MuptiCallbackApi& cbapi) {
  // Re-enable callbacks from the past if they exist.
  LOG(INFO) << "Re-enable previous MUPTI callbacks - Starting";
  VLOG(1) << "  MUPTI subscriber before reinit:" << cbapi.getMuptiSubscriber();
  cbapi.initCallbackApi();
  if (cbapi.initSuccess()) {
    VLOG(1) << "  MUPTI subscriber after reinit:" << cbapi.getMuptiSubscriber();
    bool status = cbapi.reenableCallbacks();
    if (!status) {
      LOG(WARNING)
          << "Re-enable previous MUPTI callbacks - Failed to reenableCallbacks";
    } else {
      LOG(INFO) << "Re-enable previous MUPTI callbacks - Successful";
    }
  } else {
    LOG(WARNING)
        << "Re-enable previous MUPTI callbacks - Failed to initCallbackApi";
  }
}

MuptiActivityApi& MuptiActivityApi::singleton() {
  static auto* instance = new MuptiActivityApi();
  return *instance;
}

bool MuptiActivityApi::isAvailable(uint32_t& version) const {
  return isGpuAvailable() &&
      MUPTI_CALL_NOWARN(muptiGetVersion(&version)) == MUPTI_SUCCESS;
}

void MuptiActivityApi::pushCorrelationID(int id, CorrelationFlowType type) {
  if (!singleton().externalCorrelationEnabled_.load(
          std::memory_order_relaxed)) {
    return;
  }
  VLOG(2) << "pushCorrelationID(" << id << ")";
  switch (type) {
    case Default:
      MUPTI_CALL(muptiActivityPushExternalCorrelationId(
          MUPTI_EXTERNAL_CORRELATION_KIND_CUSTOM0, id));
      break;
    case User:
      MUPTI_CALL(muptiActivityPushExternalCorrelationId(
          MUPTI_EXTERNAL_CORRELATION_KIND_CUSTOM1, id));
  }
}

void MuptiActivityApi::popCorrelationID(CorrelationFlowType type) {
  if (!singleton().externalCorrelationEnabled_.load(
          std::memory_order_relaxed)) {
    return;
  }
  switch (type) {
    case Default:
      MUPTI_CALL(muptiActivityPopExternalCorrelationId(
          MUPTI_EXTERNAL_CORRELATION_KIND_CUSTOM0, nullptr));
      break;
    case User:
      MUPTI_CALL(muptiActivityPopExternalCorrelationId(
          MUPTI_EXTERNAL_CORRELATION_KIND_CUSTOM1, nullptr));
  }
}

static bool nextActivityRecord(
    uint8_t* buffer,
    size_t valid_size,
    MUpti_Activity*& record) {
  MUptiResult status = MUPTI_CALL_NOWARN(
      muptiActivityGetNextRecord(buffer, valid_size, &record));
  if (status != MUPTI_SUCCESS) {
    if (status != MUPTI_ERROR_MAX_LIMIT_REACHED) {
      MUPTI_CALL(status);
    }
    record = nullptr;
  }
  return record != nullptr;
}

void MuptiActivityApi::setMaxBufferSize(int64_t size) {
  maxGpuBufferCount_ = 1 + size / kBufSize;
}

void MuptiActivityApi::setDeviceBufferSize([[maybe_unused]] size_t size) {
  // Disabled: MUPTI 5.1 does not export muptiActivitySetAttribute.
}

void MuptiActivityApi::setDeviceBufferPoolLimit([[maybe_unused]] size_t limit) {
  // Disabled: MUPTI 5.1 does not export muptiActivitySetAttribute.
}

void MuptiActivityApi::forceLoadMupti() {
  MUPTI_CALL(muptiActivityEnable(MUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL));
}

void MuptiActivityApi::preConfigureMUPTI() {
  if (!isGpuAvailable()) {
    return;
  }
}

void MUPTIAPI MuptiActivityApi::bufferRequestedTrampoline(
    uint8_t** buffer,
    size_t* size,
    size_t* maxNumRecords) {
  singleton().bufferRequested(buffer, size, maxNumRecords);
}

void MuptiActivityApi::bufferRequested(
    uint8_t** buffer,
    size_t* size,
    size_t* maxNumRecords) {
  std::lock_guard<std::mutex> guard(mutex_);
  LOG(VERBOSE) << "MUPTI buffer requested";

  if (!stopCollection &&
      static_cast<int64_t>(allocatedGpuTraceBuffers_.size()) >=
          maxGpuBufferCount_) {
    stopCollection = true;
    LOG(WARNING) << "Exceeded max GPU buffer count ("
                 << allocatedGpuTraceBuffers_.size()
                 << " >= " << maxGpuBufferCount_ << ") - terminating tracing";
  }
  // Rejecting buffers can crash MUPTI before API v27 (MUSA 12.9), so older or
  // unknown versions must receive valid buffers until tracing terminates.
  if (stopCollection && canRejectBuffer_) {
    *buffer = nullptr;
    *size = 0;
    *maxNumRecords = 0;
    return;
  }

  auto buf = std::make_unique<MuptiActivityBuffer>(kBufSize);
  *buffer = buf->data();
  *size = kBufSize;

  allocatedGpuTraceBuffers_[*buffer] = std::move(buf);

  *maxNumRecords = 0;
}

std::unique_ptr<MuptiActivityBufferMap> MuptiActivityApi::activityBuffers() {
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (allocatedGpuTraceBuffers_.empty()) {
      if (readyGpuTraceBuffers_) {
        return std::move(readyGpuTraceBuffers_);
      }
      return nullptr;
    }
  }

  VLOG(1) << "Flushing GPU activity buffers";
  time_point<system_clock> t1;
  if (VLOG_IS_ON(1)) {
    t1 = system_clock::now();
  }
  // Can't hold mutex_ during this call, since bufferCompleted
  // will be called by libmupti and mutex_ is acquired there.
  MUPTI_CALL(muptiActivityFlushAll(MUPTI_ACTIVITY_FLAG_FLUSH_FORCED));
  if (VLOG_IS_ON(1)) {
    flushOverhead =
        duration_cast<microseconds>(system_clock::now() - t1).count();
  }

  std::lock_guard<std::mutex> guard(mutex_);
  // Transfer ownership of buffers to caller. A new map is created on-demand.
  return std::move(readyGpuTraceBuffers_);
}

int MuptiActivityApi::processActivitiesForBuffer(
    uint8_t* buf,
    size_t validSize,
    const std::function<void(const MUpti_Activity*)>& handler) {
  int count = 0;
  if (buf && validSize) {
    MUpti_Activity* record{nullptr};
    while ((nextActivityRecord(buf, validSize, record))) {
      handler(record);
      ++count;
    }
  }
  return count;
}

const std::pair<int, size_t> MuptiActivityApi::processActivities(
    MuptiActivityBufferMap& buffers,
    const std::function<void(const MUpti_Activity*)>& handler) {
  std::pair<int, size_t> res{0, 0};
  for (auto& pair : buffers) {
    // No lock needed - only accessed from this thread
    auto& buf = pair.second;
    res.first += processActivitiesForBuffer(buf->data(), buf->size(), handler);
    res.second += buf->size();
  }
  return res;
}

void MuptiActivityApi::flushActivities() {
  MUPTI_CALL(muptiActivityFlushAll(0));
}

void MuptiActivityApi::clearActivities() {
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (allocatedGpuTraceBuffers_.empty()) {
      return;
    }
  }
  // Can't hold mutex_ during this call, since bufferCompleted
  // will be called by libmupti and mutex_ is acquired there.
  MUPTI_CALL(muptiActivityFlushAll(0));
  // FIXME: We might want to make sure we reuse
  // the same memory during warmup and tracing.
  // Also, try to use the amount of memory required
  // for active tracing during warmup.
  std::lock_guard<std::mutex> guard(mutex_);
  // Throw away ready buffers as a result of above flush
  readyGpuTraceBuffers_ = nullptr;
}

void MUPTIAPI MuptiActivityApi::bufferCompletedTrampoline(
    MUcontext ctx,
    uint32_t streamId,
    uint8_t* buffer,
    size_t /* unused */,
    size_t validSize) {
  singleton().bufferCompleted(ctx, streamId, buffer, 0, validSize);
}

void MuptiActivityApi::bufferCompleted(
    MUcontext ctx,
    uint32_t streamId,
    uint8_t* buffer,
    size_t /* unused */,
    size_t validSize) {
  {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = allocatedGpuTraceBuffers_.find(buffer);
    if (it == allocatedGpuTraceBuffers_.end()) {
      LOG(ERROR) << "bufferCompleted called with unknown buffer: "
                 << static_cast<void*>(buffer);
      return;
    }

    if (stopCollection && !canRejectBuffer_) {
      // Prevent buffers from accumulating when MUPTI cannot reject them.
      allocatedGpuTraceBuffers_.erase(it);
      return;
    }

    if (!readyGpuTraceBuffers_) {
      readyGpuTraceBuffers_ = std::make_unique<MuptiActivityBufferMap>();
    }
    // Set valid size of buffer before moving to ready map
    it->second->setSize(validSize);
    (*readyGpuTraceBuffers_)[it->first] = std::move(it->second);
    allocatedGpuTraceBuffers_.erase(it);
  }

  // report any records dropped from the queue; to avoid unnecessary mupti
  // API calls, we make it report only in verbose mode (it doesn't happen
  // often in our testing anyways)
  // Can't hold mutex_ during this call, since muptiActivityGetNumDroppedRecords
  // re-enters MUPTI and can acquire MUPTI's internal lock, while MUPTI
  // callbacks (e.g. bufferRequested) acquire mutex_ under that same lock -
  // causing an ABBA deadlock in multi-threaded environments (e.g. free-threaded
  // Python).
  if (VLOG_IS_ON(1)) {
    size_t dropped = 0;
    MUPTI_CALL(muptiActivityGetNumDroppedRecords(ctx, streamId, &dropped));
    if (dropped != 0) {
      LOG(WARNING) << "Dropped " << dropped << " activity records";
    }
  }
}

void MuptiActivityApi::enableMuptiActivities(
    const std::set<ActivityType>& selected_activities,
    bool enablePerThreadBuffers) {
  // Lazily support re-init of MUPTI Callbacks, if they were finalized before.
  auto& cbapi = MuptiCallbackApi::singleton();
  if ((tracingEnabled_ == 0u) && !cbapi.initSuccess() && muptiLazyInit_()) {
    reenableMuptiCallbacks_(cbapi);
  }

  uint32_t muptiVersion = 0;
  MUPTI_CALL(muptiGetVersion(&muptiVersion));
  {
    std::lock_guard<std::mutex> guard(mutex_);
    canRejectBuffer_ = muptiVersion >= kMuptiBufferRejectionMinVersion;
    // Reset before callbacks can run for the new trace.
    stopCollection = false;
  }

  (void)enablePerThreadBuffers; // Unsupported by MUPTI 5.1.

  MUPTI_CALL(muptiActivityRegisterCallbacks(
      bufferRequestedTrampoline, bufferCompletedTrampoline));

  externalCorrelationEnabled_.store(false, std::memory_order_relaxed);
  using enum ActivityType;
  for (const auto& activity : selected_activities) {
    if (activity == GPU_MEMCPY) {
      MUPTI_CALL(muptiActivityEnable(MUPTI_ACTIVITY_KIND_MEMCPY));
      MUPTI_CALL(muptiActivityEnable(MUPTI_ACTIVITY_KIND_MEMORY_ATOMIC));
      MUPTI_CALL(
          muptiActivityEnable(MUPTI_ACTIVITY_KIND_MEMORY_ATOMIC_VALUE));
      MUPTI_CALL(muptiActivityEnable(MUPTI_ACTIVITY_KIND_MEMORY_TRANSFER));
    }
    if (activity == GPU_MEMSET) {
      MUPTI_CALL(muptiActivityEnable(MUPTI_ACTIVITY_KIND_MEMSET));
    }
    if (activity == CONCURRENT_KERNEL) {
      MUPTI_CALL(muptiActivityEnable(MUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL));
    }
    if (activity == EXTERNAL_CORRELATION) {
      MUPTI_CALL(muptiActivityEnable(MUPTI_ACTIVITY_KIND_EXTERNAL_CORRELATION));
      externalCorrelationEnabled_.store(true, std::memory_order_relaxed);
    }
    if (activity == MUSA_SYNC) {
#if MUSA_VERSION >= 13000
      // Omitted for MUPTI 5.1: event timestamp control is unavailable
#endif
      MUPTI_CALL(muptiActivityEnable(MUPTI_ACTIVITY_KIND_SYNCHRONIZATION));
    }
    if (activity == PRIVATEUSE1_RUNTIME) {
      MUPTI_CALL(muptiActivityEnable(MUPTI_ACTIVITY_KIND_RUNTIME));
#if (MUSART_VERSION >= 12050)
      // Omitted for MUPTI 5.1: runtime is enabled by activity kind
#endif
    }
    if (activity == PRIVATEUSE1_DRIVER) {
      MUPTI_CALL(muptiActivityEnable(MUPTI_ACTIVITY_KIND_DRIVER));
#if (MUSART_VERSION >= 12050)
      // Omitted for MUPTI 5.1: driver is enabled by activity kind
      // Omitted for MUPTI 5.1: driver is enabled by activity kind
      // Omitted for MUPTI 5.1: driver is enabled by activity kind
#endif
    }
    if (activity == OVERHEAD) {
      MUPTI_CALL(muptiActivityEnable(MUPTI_ACTIVITY_KIND_OVERHEAD));
    }
  }

  tracingEnabled_ = 1;
}

void MuptiActivityApi::disableMuptiActivities(
    const std::set<ActivityType>& selected_activities) {
  using enum ActivityType;
  for (const auto& activity : selected_activities) {
    if (activity == GPU_MEMCPY) {
      MUPTI_CALL(muptiActivityDisable(MUPTI_ACTIVITY_KIND_MEMCPY));
      MUPTI_CALL(muptiActivityDisable(MUPTI_ACTIVITY_KIND_MEMORY_ATOMIC));
      MUPTI_CALL(
          muptiActivityDisable(MUPTI_ACTIVITY_KIND_MEMORY_ATOMIC_VALUE));
      MUPTI_CALL(muptiActivityDisable(MUPTI_ACTIVITY_KIND_MEMORY_TRANSFER));
    }
    if (activity == GPU_MEMSET) {
      MUPTI_CALL(muptiActivityDisable(MUPTI_ACTIVITY_KIND_MEMSET));
    }
    if (activity == CONCURRENT_KERNEL) {
      MUPTI_CALL(muptiActivityDisable(MUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL));
    }
    if (activity == EXTERNAL_CORRELATION) {
      MUPTI_CALL(
          muptiActivityDisable(MUPTI_ACTIVITY_KIND_EXTERNAL_CORRELATION));
    }
    if (activity == MUSA_SYNC) {
      MUPTI_CALL(muptiActivityDisable(MUPTI_ACTIVITY_KIND_SYNCHRONIZATION));
    }
    if (activity == PRIVATEUSE1_RUNTIME) {
      MUPTI_CALL(muptiActivityDisable(MUPTI_ACTIVITY_KIND_RUNTIME));
    }
    if (activity == PRIVATEUSE1_DRIVER) {
      MUPTI_CALL(muptiActivityDisable(MUPTI_ACTIVITY_KIND_DRIVER));
    }
    if (activity == OVERHEAD) {
      MUPTI_CALL(muptiActivityDisable(MUPTI_ACTIVITY_KIND_OVERHEAD));
    }
  }
  externalCorrelationEnabled_.store(false, std::memory_order_relaxed);
}

void MuptiActivityApi::teardownContext() {
  if (!tracingEnabled_) {
    return;
  }
  if (tearingDown_) {
    return;
  }
  if (muptiTearDown_()) {
    tearingDown_ = 1;
    LOG(INFO) << "teardownMupti starting";

    // PyTorch Profiler is synchronous, so teardown needs to be run async in
    // this thread.
    std::thread teardownThread([&] {
      auto& cbapi = MuptiCallbackApi::singleton();
      if (!cbapi.initSuccess()) {
        cbapi.initCallbackApi();
        if (!cbapi.initSuccess()) {
          LOG(WARNING) << "MUPTI Callback failed to init, skipping teardown";
          tearingDown_ = 0;
          return;
        }
      }
      // Subscribe callbacks to call muptiFinalize in the exit callback of these
      // APIs
      bool status = cbapi.enableCallbackDomain(MUPTI_CB_DOMAIN_RUNTIME_API);
      status = status && cbapi.enableCallbackDomain(MUPTI_CB_DOMAIN_DRIVER_API);
      if (!status) {
        LOG(WARNING)
            << "MUPTI Callback failed to enable for domain, skipping teardown";
        tearingDown_ = 0;
        return;
      }

      // Force Flush before finalize
      MUPTI_CALL(muptiActivityFlushAll(MUPTI_ACTIVITY_FLAG_FLUSH_FORCED));

      LOG(INFO) << "  MUPTI subscriber before finalize:"
                << cbapi.getMuptiSubscriber();
      teardownMupti_ = 1;
      std::unique_lock<std::mutex> lck(finalizeMutex_);
      finalizeCond_.wait(lck, [&] { return teardownMupti_ == 0; });
      lck.unlock();
      LOG(INFO) << "teardownMupti complete";

      teardownMupti_ = 0;
      tracingEnabled_ = 0;

      // Remove the callbacks used specifically for muptiFinalize
      cbapi.disableCallbackDomain(MUPTI_CB_DOMAIN_RUNTIME_API);
      cbapi.disableCallbackDomain(MUPTI_CB_DOMAIN_DRIVER_API);

      // Re-init MUPTI Callbacks if Lazy Re-init is not enabled.
      if (!muptiLazyInit_()) {
        reenableMuptiCallbacks_(cbapi);
      }
      tearingDown_ = 0;
    });
    teardownThread.detach();
  }
}

} // namespace KINETO_NAMESPACE
