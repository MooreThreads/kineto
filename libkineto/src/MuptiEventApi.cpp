/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MuptiEventApi.h"

#include <chrono>

#include "MusaUtil.h"
#include "Logger.h"

using std::vector;

namespace KINETO_NAMESPACE {

MuptiEventApi::MuptiEventApi(MUcontext context)
    : context_(context) {
  MUPTI_CALL(muptiGetDeviceId(context_, (uint32_t*)&device_));
}

MUpti_EventGroupSets* MuptiEventApi::createGroupSets(
    vector<MUpti_EventID>& ids) {
  MUpti_EventGroupSets* group_sets = nullptr;
  MUptiResult res = MUPTI_CALL(muptiEventGroupSetsCreate(
      context_, sizeof(MUpti_EventID) * ids.size(), ids.data(), &group_sets));

  if (res != MUPTI_SUCCESS || group_sets == nullptr) {
    const char* errstr = nullptr;
    MUPTI_CALL(muptiGetResultString(res, &errstr));
    throw std::system_error(EINVAL, std::generic_category(), errstr);
  }

  return group_sets;
}

void MuptiEventApi::destroyGroupSets(MUpti_EventGroupSets* sets) {
  MUPTI_CALL(muptiEventGroupSetsDestroy(sets));
}

bool MuptiEventApi::setContinuousMode() {
  // Avoid logging noise for MUPTI_ERROR_LEGACY_PROFILER_NOT_SUPPORTED
  MUptiResult res = MUPTI_CALL_NOWARN(muptiSetEventCollectionMode(
      context_, MUPTI_EVENT_COLLECTION_MODE_CONTINUOUS));
  if (res == MUPTI_ERROR_LEGACY_PROFILER_NOT_SUPPORTED) {
    return false;
  }
  // Log warning on other errors
  MUPTI_CALL(res);
  return (res == MUPTI_SUCCESS);
}

void MuptiEventApi::enablePerInstance(MUpti_EventGroup eventGroup) {
  uint32_t profile_all = 1;
  MUPTI_CALL(muptiEventGroupSetAttribute(
      eventGroup,
      MUPTI_EVENT_GROUP_ATTR_PROFILE_ALL_DOMAIN_INSTANCES,
      sizeof(profile_all),
      &profile_all));
}

uint32_t MuptiEventApi::instanceCount(MUpti_EventGroup eventGroup) {
  uint32_t instance_count = 0;
  size_t s = sizeof(instance_count);
  MUPTI_CALL(muptiEventGroupGetAttribute(
      eventGroup, MUPTI_EVENT_GROUP_ATTR_INSTANCE_COUNT, &s, &instance_count));
  return instance_count;
}

void MuptiEventApi::enableGroupSet(MUpti_EventGroupSet& set) {
  MUptiResult res = MUPTI_CALL_NOWARN(muptiEventGroupSetEnable(&set));
  if (res != MUPTI_SUCCESS) {
    const char* errstr = nullptr;
    MUPTI_CALL(muptiGetResultString(res, &errstr));
    throw std::system_error(EIO, std::generic_category(), errstr);
  }
}

void MuptiEventApi::disableGroupSet(MUpti_EventGroupSet& set) {
  MUPTI_CALL(muptiEventGroupSetDisable(&set));
}

void MuptiEventApi::readEvent(
    MUpti_EventGroup grp,
    MUpti_EventID id,
    vector<int64_t>& vals) {
  size_t s = sizeof(int64_t) * vals.size();
  MUPTI_CALL(muptiEventGroupReadEvent(
      grp,
      MUPTI_EVENT_READ_FLAG_NONE,
      id,
      &s,
      reinterpret_cast<uint64_t*>(vals.data())));
}

vector<MUpti_EventID> MuptiEventApi::eventsInGroup(MUpti_EventGroup grp) {
  uint32_t group_size = 0;
  size_t s = sizeof(group_size);
  MUPTI_CALL(muptiEventGroupGetAttribute(
      grp, MUPTI_EVENT_GROUP_ATTR_NUM_EVENTS, &s, &group_size));
  size_t events_size = group_size * sizeof(MUpti_EventID);
  vector<MUpti_EventID> res(group_size);
  MUPTI_CALL(muptiEventGroupGetAttribute(
      grp, MUPTI_EVENT_GROUP_ATTR_EVENTS, &events_size, res.data()));
  return res;
}

MUpti_EventID MuptiEventApi::eventId(const std::string& name) {
  MUpti_EventID id{0};
  MUPTI_CALL(muptiEventGetIdFromName(device_, name.c_str(), &id));
  return id;
}

} // namespace KINETO_NAMESPACE
