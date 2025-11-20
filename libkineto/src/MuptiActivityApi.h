/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <set>

#ifdef HAS_MUPTI
#include <mupti.h>
#endif

// TODO(T90238193)
// @lint-ignore-every CLANGTIDY facebook-hte-RelativeInclude
#include "ActivityType.h"
#include "MuptiActivityBuffer.h"
#ifdef HAS_MUPTI
#include "MuptiCallbackApi.h"
#endif


namespace KINETO_NAMESPACE {

using namespace libkineto;

#ifndef HAS_MUPTI
using MUpti_Activity = void;
#endif

class MuptiActivityApi {
 public:
  enum CorrelationFlowType {
    Default,
    User
  };
  // Control Variables shared with MuptiCallbackApi for teardown
  std::atomic<uint32_t> teardownMupti_{0};
  std::mutex finalizeMutex_;
  std::condition_variable finalizeCond_;

  MuptiActivityApi() = default;
  MuptiActivityApi(const MuptiActivityApi&) = delete;
  MuptiActivityApi& operator=(const MuptiActivityApi&) = delete;

  virtual ~MuptiActivityApi() = default;

  static MuptiActivityApi& singleton();

  static void pushCorrelationID(int id, CorrelationFlowType type);
  static void popCorrelationID(CorrelationFlowType type);

  void enableMuptiActivities(
      const std::set<ActivityType>& selected_activities,
      bool enablePerThreadBuffers = false);
  void disableMuptiActivities(
      const std::set<ActivityType>& selected_activities);
  void clearActivities();
  void teardownContext();

  virtual std::unique_ptr<MuptiActivityBufferMap> activityBuffers();

  virtual const std::pair<int, size_t> processActivities(
      MuptiActivityBufferMap&,
      const std::function<void(const MUpti_Activity*)>& handler);

  void setMaxBufferSize(int size);
  void setDeviceBufferSize(size_t size);
  void setDeviceBufferPoolLimit(size_t limit);

  std::atomic_bool stopCollection{false};
  int64_t flushOverhead{0};

  static void forceLoadMupti();

  // MUPTI configuraiton that needs to be set before MUSA context creation
  static void preConfigureMUPTI();

 private:
  int maxGpuBufferCount_{0};
  MuptiActivityBufferMap allocatedGpuTraceBuffers_;
  std::unique_ptr<MuptiActivityBufferMap> readyGpuTraceBuffers_;
  std::mutex mutex_;
  std::atomic<uint32_t> tracingEnabled_{0};
  bool externalCorrelationEnabled_{false};

#ifdef HAS_MUPTI
  int processActivitiesForBuffer(
      uint8_t* buf,
      size_t validSize,
      const std::function<void(const MUpti_Activity*)>& handler);
  static void MUPTIAPI bufferRequestedTrampoline(
      uint8_t** buffer,
      size_t* size,
      size_t* maxNumRecords);
  static void MUPTIAPI bufferCompletedTrampoline(
      MUcontext ctx,
      uint32_t streamId,
      uint8_t* buffer,
      size_t /* unused */,
      size_t validSize);
#endif // HAS_MUPTI

 protected:
#ifdef HAS_MUPTI
  void bufferRequested(uint8_t** buffer, size_t* size, size_t* maxNumRecords);
  void bufferCompleted(
      MUcontext ctx,
      uint32_t streamId,
      uint8_t* buffer,
      size_t /* unused */,
      size_t validSize);
#endif
};

} // namespace KINETO_NAMESPACE
