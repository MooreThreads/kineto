/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <mupti.h>
#include <queue>
#include <string>

namespace KINETO_NAMESPACE {

// C++ interface to MUPTI Events C API.
// Virtual methods are here mainly to allow easier testing.
class MuptiEventApi {
 public:
  explicit MuptiEventApi(MUcontext context_);
  virtual ~MuptiEventApi() {}

  MUdevice device() {
    return device_;
  }

  virtual MUpti_EventGroupSets* createGroupSets(
      std::vector<MUpti_EventID>& ids);
  virtual void destroyGroupSets(MUpti_EventGroupSets* sets);

  virtual bool setContinuousMode();

  virtual void enablePerInstance(MUpti_EventGroup eventGroup);
  virtual uint32_t instanceCount(MUpti_EventGroup eventGroup);

  virtual void enableGroupSet(MUpti_EventGroupSet& set);
  virtual void disableGroupSet(MUpti_EventGroupSet& set);

  virtual void
  readEvent(MUpti_EventGroup g, MUpti_EventID id, std::vector<int64_t>& vals);
  virtual std::vector<MUpti_EventID> eventsInGroup(MUpti_EventGroup g);

  virtual MUpti_EventID eventId(const std::string& name);

 protected:
  // Unit testing
  MuptiEventApi() : context_(nullptr), device_(0) {}

 private:
  MUcontext context_;
  MUdevice device_;
};

} // namespace KINETO_NAMESPACE
