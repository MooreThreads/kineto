/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MuptiActivityApi.h"

#include <assert.h>
#include <chrono>
#include <mutex>
#include <thread>

#include "mupti_call.h"
#include "Logger.h"
#include "Config.h"
#include "MusaUtil.h"

using namespace std::chrono;

namespace KINETO_NAMESPACE {

// Set to 4MB to avoid constantly creating buffers (especially for networks
// that have many small memcpy such as sparseNN). MUPTI recommends between
// 1MB to 10MB.
// Given the kDefaultActivitiesMaxGpuBufferSize is around 128MB, in the worst
// case, there will be 32 buffers contending for the mutex.
constexpr size_t kBufSize(4 * 1024 * 1024);

#ifdef HAS_MUPTI
inline bool muptiTearDown_() {
  auto teardown_env = getenv("TEARDOWN_MUPTI");
  return teardown_env != nullptr && strcmp(teardown_env, "1") == 0;
}

inline bool muptiLazyInit_() {
  return muptiTearDown_() && getenv("DISABLE_MUPTI_LAZY_REINIT") == nullptr;
}

inline void reenableMuptiCallbacks_(std::shared_ptr<MuptiCallbackApi>& cbapi_) {
  // Re-enable callbacks from the past if they exist.
  LOG(INFO) << "Re-enable previous MUPTI callbacks - Starting";
  VLOG(1) << "  MUPTI subscriber before reinit:" << cbapi_->getMuptiSubscriber();
  cbapi_->initCallbackApi();
  if (cbapi_->initSuccess()) {
    VLOG(1) << "  MUPTI subscriber after reinit:" << cbapi_->getMuptiSubscriber();
    bool status = cbapi_->reenableCallbacks();
    if (!status) {
      LOG(WARNING) << "Re-enable previous MUPTI callbacks - Failed to reenableCallbacks";
    } else {
      LOG(INFO) << "Re-enable previous MUPTI callbacks - Successful";
    }
  } else {
    LOG(WARNING) << "Re-enable previous MUPTI callbacks - Failed to initCallbackApi";
  }
}
#endif

MuptiActivityApi& MuptiActivityApi::singleton() {
  static MuptiActivityApi instance;
  return instance;
}

void MuptiActivityApi::pushCorrelationID(int id, CorrelationFlowType type) {
#ifdef HAS_MUPTI
  if (!singleton().externalCorrelationEnabled_) {
    return;
  }
  VLOG(2) << "pushCorrelationID(" << id << ")";
  switch(type) {
    case Default:
      // TODO: MUPTI muptiActivityPushExternalCorrelationId is not yet implemented
      MUPTI_CALL(muptiActivityPushExternalCorrelationId(
        MUPTI_EXTERNAL_CORRELATION_KIND_CUSTOM0, id));
        break;
    case User:
      MUPTI_CALL(muptiActivityPushExternalCorrelationId(
        MUPTI_EXTERNAL_CORRELATION_KIND_CUSTOM1, id));
        break;
  }
#endif
}

void MuptiActivityApi::popCorrelationID(CorrelationFlowType type) {
#ifdef HAS_MUPTI
  if (!singleton().externalCorrelationEnabled_) {
    return;
  }
  switch(type) {
    case Default:
      // TODO: MUPTI muptiActivityPopExternalCorrelationId is not yet implemented
      MUPTI_CALL(muptiActivityPopExternalCorrelationId(
        MUPTI_EXTERNAL_CORRELATION_KIND_CUSTOM0, nullptr));
        break;
    case User:
      MUPTI_CALL(muptiActivityPopExternalCorrelationId(
        MUPTI_EXTERNAL_CORRELATION_KIND_CUSTOM1, nullptr));
        break;
  }
#endif
}

static bool nextActivityRecord(
    uint8_t* buffer,
    size_t valid_size,
    MUpti_Activity*& record) {
#ifdef HAS_MUPTI
  MUptiResult status = MUPTI_CALL_NOWARN(
      muptiActivityGetNextRecord(buffer, valid_size, &record));
  if (status != MUPTI_SUCCESS) {
    if (status != MUPTI_ERROR_MAX_LIMIT_REACHED) {
      MUPTI_CALL(status);
    }
    record = nullptr;
  }
#endif
  return record != nullptr;
}

void MuptiActivityApi::setMaxBufferSize(int size) {
  maxGpuBufferCount_ = 1 + size / kBufSize;
}

void MuptiActivityApi::setDeviceBufferSize(size_t size) {
#ifdef HAS_MUPTI
  size_t valueSize = sizeof(size_t);
  // MUPTI_CALL(muptiActivitySetAttribute(MUPTI_ACTIVITY_ATTR_DEVICE_BUFFER_SIZE, &valueSize, &size));
#endif
}

void MuptiActivityApi::setDeviceBufferPoolLimit(size_t limit) {
#ifdef HAS_MUPTI
  size_t valueSize = sizeof(size_t);
  // MUPTI_CALL(muptiActivitySetAttribute(MUPTI_ACTIVITY_ATTR_DEVICE_BUFFER_POOL_LIMIT, &valueSize, &limit));
#endif
}

void MuptiActivityApi::forceLoadMupti() {
#ifdef HAS_MUPTI
  MUPTI_CALL(muptiActivityEnable(MUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL));
  MUPTI_CALL(muptiActivityDisable(MUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL));
#endif
}

void MuptiActivityApi::preConfigureMUPTI() {
#ifdef HAS_MUPTI
  if (!isGpuAvailable()) {
    return;
  }
#endif
}

#ifdef HAS_MUPTI
void MUPTIAPI MuptiActivityApi::bufferRequestedTrampoline(
    uint8_t** buffer,
    size_t* size,
    size_t* maxNumRecords) {
  singleton().bufferRequested(buffer, size, maxNumRecords);
}

void MuptiActivityApi::bufferRequested(
    uint8_t** buffer, size_t* size, size_t* maxNumRecords) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (allocatedGpuTraceBuffers_.size() >= maxGpuBufferCount_) {
    // comment this to avoid stopping the collection when the buffer is full
    // stopCollection = true;
    LOG(WARNING) << "Exceeded max GPU buffer count ("
                 << allocatedGpuTraceBuffers_.size()
                 << " > " << maxGpuBufferCount_
                 << ") - terminating tracing";
  }

  auto buf = std::make_unique<MuptiActivityBuffer>(kBufSize);
  *buffer = buf->data();
  *size = kBufSize;
  allocatedGpuTraceBuffers_[*buffer] = std::move(buf);
  *maxNumRecords = 0;
}
#endif

std::unique_ptr<MuptiActivityBufferMap>
MuptiActivityApi::activityBuffers() {
#ifdef HAS_MUPTI
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
#endif
  std::lock_guard<std::mutex> guard(mutex_);
  // Transfer ownership of buffers to caller. A new map is created on-demand.
  return std::move(readyGpuTraceBuffers_);
}

#ifdef HAS_MUPTI
int MuptiActivityApi::processActivitiesForBuffer(
    uint8_t* buf,
    size_t validSize,
    std::function<void(const MUpti_Activity*)> handler) {
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
#endif

const std::pair<int, size_t> MuptiActivityApi::processActivities(
    MuptiActivityBufferMap& buffers,
    std::function<void(const MUpti_Activity*)> handler) {
  std::pair<int, size_t> res{0, 0};
#ifdef HAS_MUPTI
  for (auto& pair : buffers) {
    // No lock needed - only accessed from this thread
    auto& buf = pair.second;
    res.first += processActivitiesForBuffer(buf->data(), buf->size(), handler);
    res.second += buf->size();
  }
#endif
  return res;
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
#ifdef HAS_MUPTI
  MUPTI_CALL(muptiActivityFlushAll(0));
#endif
  // FIXME: We might want to make sure we reuse
  // the same memory during warmup and tracing.
  // Also, try to use the amount of memory required
  // for active tracing during warmup.
  std::lock_guard<std::mutex> guard(mutex_);
  // Throw away ready buffers as a result of above flush
  readyGpuTraceBuffers_ = nullptr;
}

#ifdef HAS_MUPTI
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

  std::lock_guard<std::mutex> guard(mutex_);
  auto it = allocatedGpuTraceBuffers_.find(buffer);
  if (it == allocatedGpuTraceBuffers_.end()) {
    LOG(ERROR) << "bufferCompleted called with unknown buffer: "
               << (void*) buffer;
    return;
  }

  if (!readyGpuTraceBuffers_) {
    readyGpuTraceBuffers_ = std::make_unique<MuptiActivityBufferMap>();
  }
  // Set valid size of buffer before moving to ready map
  it->second->setSize(validSize);
  (*readyGpuTraceBuffers_)[it->first] = std::move(it->second);
  allocatedGpuTraceBuffers_.erase(it);

  // report any records dropped from the queue; to avoid unnecessary mupti
  // API calls, we make it report only in verbose mode (it doesn't happen
  // often in our testing anyways)
  if (VLOG_IS_ON(1)) {
    size_t dropped = 0;
    MUPTI_CALL(muptiActivityGetNumDroppedRecords(ctx, streamId, &dropped));
    if (dropped != 0) {
      LOG(WARNING) << "Dropped " << dropped << " activity records";
    }
  }
}
#endif

void MuptiActivityApi::enableMuptiActivities(
    const std::set<ActivityType>& selected_activities) {
#ifdef HAS_MUPTI
  // Lazily support re-init of MUPTI Callbacks, if they were finalized before.
  auto cbapi_ = MuptiCallbackApi::singleton();
  if (!tracingEnabled_ && !cbapi_->initSuccess() && muptiLazyInit_()) {
    reenableMuptiCallbacks_(cbapi_);
  }
  cbapi_.reset();

  MUPTI_CALL(
      muptiActivityRegisterCallbacks(bufferRequestedTrampoline, bufferCompletedTrampoline));

  externalCorrelationEnabled_ = false;
  for (const auto& activity : selected_activities) {
    if (activity == ActivityType::GPU_MEMCPY) {
      MUPTI_CALL(muptiActivityEnable(MUPTI_ACTIVITY_KIND_MEMCPY));
    }
    if (activity == ActivityType::GPU_MEMSET) {
      MUPTI_CALL(muptiActivityEnable(MUPTI_ACTIVITY_KIND_MEMSET));
    }
    if (activity == ActivityType::CONCURRENT_KERNEL) {
      MUPTI_CALL(muptiActivityEnable(MUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL));
    }
    if (activity == ActivityType::EXTERNAL_CORRELATION) {
      MUPTI_CALL(muptiActivityEnable(MUPTI_ACTIVITY_KIND_EXTERNAL_CORRELATION));
      externalCorrelationEnabled_ = true;
    }
    if (activity == ActivityType::MUSA_SYNC) {
      MUPTI_CALL(muptiActivityEnable(MUPTI_ACTIVITY_KIND_SYNCHRONIZATION));
    }
    if (activity == ActivityType::MUSA_RUNTIME) {
      MUPTI_CALL(muptiActivityEnable(MUPTI_ACTIVITY_KIND_RUNTIME));
    }
    if (activity == ActivityType::MUSA_DRIVER) {
      MUPTI_CALL(muptiActivityEnable(MUPTI_ACTIVITY_KIND_DRIVER));
    }
    if (activity == ActivityType::OVERHEAD) {
      MUPTI_CALL(muptiActivityEnable(MUPTI_ACTIVITY_KIND_OVERHEAD));
    }
  }

  tracingEnabled_ = 1;
#endif

  // Explicitly enabled, so reset this flag if set
  stopCollection = false;
}

void MuptiActivityApi::disableMuptiActivities(
    const std::set<ActivityType>& selected_activities) {
#ifdef HAS_MUPTI
  for (const auto& activity : selected_activities) {
    if (activity == ActivityType::GPU_MEMCPY) {
      MUPTI_CALL(muptiActivityDisable(MUPTI_ACTIVITY_KIND_MEMCPY));
    }
    if (activity == ActivityType::GPU_MEMSET) {
      MUPTI_CALL(muptiActivityDisable(MUPTI_ACTIVITY_KIND_MEMSET));
    }
    if (activity == ActivityType::CONCURRENT_KERNEL) {
      MUPTI_CALL(muptiActivityDisable(MUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL));
    }
    if (activity == ActivityType::EXTERNAL_CORRELATION) {
      MUPTI_CALL(muptiActivityDisable(MUPTI_ACTIVITY_KIND_EXTERNAL_CORRELATION));
    }
    if (activity == ActivityType::MUSA_SYNC) {
      MUPTI_CALL(muptiActivityDisable(MUPTI_ACTIVITY_KIND_SYNCHRONIZATION));
    }
    if (activity == ActivityType::MUSA_RUNTIME) {
      MUPTI_CALL(muptiActivityDisable(MUPTI_ACTIVITY_KIND_RUNTIME));
    }
    if (activity == ActivityType::MUSA_DRIVER) {
      MUPTI_CALL(muptiActivityDisable(MUPTI_ACTIVITY_KIND_DRIVER));
    }
    if (activity == ActivityType::OVERHEAD) {
      MUPTI_CALL(muptiActivityDisable(MUPTI_ACTIVITY_KIND_OVERHEAD));
    }
  }
  externalCorrelationEnabled_ = false;
#endif
}

void MuptiActivityApi::teardownContext() {
#ifdef HAS_MUPTI
  if (!tracingEnabled_) {
    return;
  }
  if (muptiTearDown_()) {
    LOG(INFO) << "teardownMupti starting";

    // PyTorch Profiler is synchronous, so teardown needs to be run async in this thread.
    std::thread teardownThread([&] {
      auto cbapi_ = MuptiCallbackApi::singleton();
      if (!cbapi_->initSuccess()) {
        cbapi_->initCallbackApi();
        if (!cbapi_->initSuccess()) {
          LOG(WARNING) << "MUPTI Callback failed to init, skipping teardown";
          return;
        }
      }
      // Subscribe callbacks to call muptiFinalize in the exit callback of these APIs
      bool status = cbapi_->enableCallbackDomain(MUPTI_CB_DOMAIN_RUNTIME_API);
      status = status && cbapi_->enableCallbackDomain(MUPTI_CB_DOMAIN_DRIVER_API);
      if (!status) {
        LOG(WARNING) << "MUPTI Callback failed to enable for domain, skipping teardown";
        return;
      }

      // Force Flush before finalize
      MUPTI_CALL(muptiActivityFlushAll(MUPTI_ACTIVITY_FLAG_FLUSH_FORCED));

      LOG(INFO) << "  MUPTI subscriber before finalize:" << cbapi_->getMuptiSubscriber();
      teardownMupti_ = 1;
      std::unique_lock<std::mutex> lck(finalizeMutex_);
      finalizeCond_.wait(lck, [&]{return teardownMupti_ == 0;});
      lck.unlock();
      LOG(INFO) << "teardownMupti complete";

      teardownMupti_ = 0;
      tracingEnabled_ = 0;

      // Remove the callbacks used specifically for muptiFinalize
      cbapi_->disableCallbackDomain(MUPTI_CB_DOMAIN_RUNTIME_API);
      cbapi_->disableCallbackDomain(MUPTI_CB_DOMAIN_DRIVER_API);

      // Re-init MUPTI Callbacks if Lazy Re-init is not enabled.
      if (!muptiLazyInit_()) {
        reenableMuptiCallbacks_(cbapi_);
      }
      cbapi_.reset();
    });
    teardownThread.detach();
  }
#endif
}

} // namespace KINETO_NAMESPACE
