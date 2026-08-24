/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <mupti.h>
#include <mutex>
#include "MuptiActivity.h"
#include "MuptiActivityApi.h"
#include "GenericActivityProfiler.h"

namespace KINETO_NAMESPACE {

class MuptiActivityProfiler : public GenericActivityProfiler {
 public:
  MuptiActivityProfiler(MuptiActivityApi& mupti, bool cpuOnly);
  MuptiActivityProfiler(const MuptiActivityProfiler&) = delete;
  MuptiActivityProfiler& operator=(const MuptiActivityProfiler&) = delete;
  ~MuptiActivityProfiler() override = default;

 protected:
  void setMaxGpuBufferSize(int64_t size) override;
  void enableGpuTracing() override;
  void disableGpuTracing() override;
  void clearGpuActivities() override;
  bool isGpuCollectionStopped() const override;
  void processGpuActivities(ActivityLogger& logger) override;
  void synchronizeGpuDevice() override;
  void pushCorrelationIdImpl(uint64_t id, CorrelationFlowType type) override;
  void popCorrelationIdImpl(CorrelationFlowType type) override;
  void onResetTraceData() override;
  void onFinalizeTrace(const Config& config, ActivityLogger& logger) override;

 private:
  void buildProcessingState(MuptiActivityBufferMap& buffers);
  // Process generic MUPTI activity
  void handleMuptiActivity(
      const MUpti_Activity* record,
      ActivityLogger* logger);
  // Process specific GPU activity types
  void handleCorrelationActivity(
      const MUpti_ActivityExternalCorrelation* correlation);
  void handleRuntimeActivity(
      const MUpti_ActivityAPI* activity,
      ActivityLogger* logger);
  void handleDriverActivity(
      const MUpti_ActivityAPI* activity,
      ActivityLogger* logger);
  void handleOverheadActivity(
      const MUpti_ActivityOverhead* activity,
      ActivityLogger* logger);
  void handleMusaEventActivity(
      const MUpti_ActivityMusaEventType* activity,
      ActivityLogger* logger);
  void handleMusaSyncActivity(
      const MUpti_ActivitySynchronization* activity,
      ActivityLogger* logger);
  template <class T>
  void handleGpuActivity(const T* act, ActivityLogger* logger);
  void logDeferredEvents();
  void recordGpuVersions(uint32_t muptiVersion);

  // Defer the MUSA/MUPTI probe until tracing starts, after any worker fork.
  // Cache the result because an unavailable backend stays CPU-only.
  std::once_flag muptiInitializationOnce_;
  bool muptiAvailable_{false};

  // Calls to MUPTI is encapsulated behind this interface
  MuptiActivityApi& mupti_;
};

// Helper function to map context ID to device ID
uint32_t contextIdtoDeviceId(uint32_t contextId);

} // namespace KINETO_NAMESPACE
