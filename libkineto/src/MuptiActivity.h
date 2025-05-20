/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <mupti.h>

// TODO(T90238193)
// @lint-ignore-every CLANGTIDY facebook-hte-RelativeInclude
#include "ITraceActivity.h"
#include "GenericTraceActivity.h"
#include "MuptiActivityPlatform.h"
#include "GenericTraceActivity.h"
#include "ThreadUtil.h"
#include "mupti_strings.h"

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
template<class T>
struct MuptiActivity : public ITraceActivity {
  explicit MuptiActivity(const T* activity, const ITraceActivity* linked)
      : activity_(*activity), linked_(linked) {}
  int64_t timestamp() const override {
    return nsToUs(unixEpochTimestamp(activity_.start));
  }
  int64_t duration() const override {
    return nsToUs(activity_.end - activity_.start);
  }
  // TODO(T107507796): Deprecate ITraceActivity
  int64_t correlationId() const override {return 0;}
  int32_t getThreadId() const override {return 0;}
  const ITraceActivity* linkedActivity() const override {return linked_;}
  int flowType() const override {return kLinkAsyncCpuGpu;}
  int flowId() const override {return correlationId();}
  const T& raw() const {return activity_;}
  const TraceSpan* traceSpan() const override {return nullptr;}

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
  int64_t correlationId() const override {return activity_.correlationId;}
  int64_t deviceId() const override {return processId();}
  int64_t resourceId() const override {return threadId_;}
  ActivityType type() const override {return ActivityType::PRIVATEUSE1_RUNTIME;}
  bool flowStart() const override;
  const std::string name() const override {return runtimeCbidName(activity_.cbid);}
  void log(ActivityLogger& logger) const override;
  const std::string metadataJson() const override;

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
  int64_t correlationId() const override {return activity_.correlationId;}
  int64_t deviceId() const override {return processId();}
  int64_t resourceId() const override {return threadId_;}
  ActivityType type() const override {return ActivityType::PRIVATEUSE1_DRIVER;}
  bool flowStart() const override;
  const std::string name() const override;
  void log(ActivityLogger& logger) const override;
  const std::string metadataJson() const override;

 private:
  const int32_t threadId_;
};

// MUpti_ActivityAPI - MUSA runtime activities
struct OverheadActivity : public MuptiActivity<MUpti_ActivityOverhead> {
  explicit OverheadActivity(
      const MUpti_ActivityOverhead* activity,
      const ITraceActivity* linked,
      int32_t threadId=0)
      : MuptiActivity(activity, linked), threadId_(threadId) {}

  int64_t timestamp() const override {
    return nsToUs(unixEpochTimestamp(activity_.start));
  }
  int64_t duration() const override {
    return nsToUs(activity_.end - activity_.start);
  }
  // TODO: Update this with PID ordering
  int64_t deviceId() const override {return -1;}
  int64_t resourceId() const override {return threadId_;}
  ActivityType type() const override {return ActivityType::OVERHEAD;}
  bool flowStart() const override;
  const std::string name() const override {return overheadKindString(activity_.overheadKind);}
  void log(ActivityLogger& logger) const override;
  const std::string metadataJson() const override;

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
  int64_t correlationId() const override {return raw().correlationId;}
  int64_t deviceId() const override;
  int64_t resourceId() const override;
  ActivityType type() const override {return ActivityType::CUDA_SYNC;}
  bool flowStart() const override {return false;}
  const std::string name() const override;
  void log(ActivityLogger& logger) const override;
  const std::string metadataJson() const override;
  const MUpti_ActivitySynchronization& raw() const {return MuptiActivity<MUpti_ActivitySynchronization>::raw();}

 private:
  const int32_t srcStream_;
  const int32_t srcCorrId_;
};


// Base class for GPU activities.
// Can also be instantiated directly.
template<class T>
struct GpuActivity : public MuptiActivity<T> {
  explicit GpuActivity(const T* activity, const ITraceActivity* linked)
      : MuptiActivity<T>(activity, linked) {}
  int64_t correlationId() const override {return raw().correlationId;}
  int64_t deviceId() const override {return raw().deviceId;}
  int64_t resourceId() const override {return raw().streamId;}
  ActivityType type() const override;
  bool flowStart() const override {return false;}
  const std::string name() const override;
  void log(ActivityLogger& logger) const override;
  const std::string metadataJson() const override;
  const T& raw() const {return MuptiActivity<T>::raw();}
};

} // namespace KINETO_NAMESPACE
