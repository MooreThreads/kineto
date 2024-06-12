/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MuptiMetricApi.h"

#include <chrono>

#include "Logger.h"
#include "mupti_call.h"

using std::vector;

namespace KINETO_NAMESPACE {

MUpti_MetricID MuptiMetricApi::idFromName(const std::string& name) {
  MUpti_MetricID metric_id{~0u};
  MUptiResult res =
      MUPTI_CALL(muptiMetricGetIdFromName(device_, name.c_str(), &metric_id));
  if (res == MUPTI_ERROR_INVALID_METRIC_NAME) {
    LOG(WARNING) << "Invalid metric name: " << name;
  }
  return metric_id;
}

// Return a map of event IDs and names for a given metric id.
// Note that many events don't have a name. In that case the name will
// be set to the empty string.
std::map<MUpti_EventID, std::string> MuptiMetricApi::events(
    MUpti_MetricID metric_id) {
  uint32_t num_events = 0;
  MUPTI_CALL(muptiMetricGetNumEvents(metric_id, &num_events));
  vector<MUpti_EventID> ids(num_events);
  size_t array_size = num_events * sizeof(MUpti_EventID);
  MUPTI_CALL(muptiMetricEnumEvents(metric_id, &array_size, ids.data()));
  std::map<MUpti_EventID, std::string> res;
  for (MUpti_EventID id : ids) {
    // Attempt to lookup name from MUPTI
    constexpr size_t kMaxEventNameLength = 64;
    char mupti_name[kMaxEventNameLength];
    size_t size = kMaxEventNameLength;
    MUPTI_CALL(
        muptiEventGetAttribute(id, MUPTI_EVENT_ATTR_NAME, &size, mupti_name));
    mupti_name[kMaxEventNameLength - 1] = 0;

    // MUPTI "helpfully" returns "event_name" when the event is unnamed.
    if (size > 0 && strcmp(mupti_name, "event_name") != 0) {
      res.emplace(id, mupti_name);
    } else {
      res.emplace(id, "");
    }
  }
  return res;
}

MUpti_MetricValueKind MuptiMetricApi::valueKind(MUpti_MetricID metric) {
  MUpti_MetricValueKind res{MUPTI_METRIC_VALUE_KIND_FORCE_INT};
  size_t value_kind_size = sizeof(res);
  MUPTI_CALL(muptiMetricGetAttribute(
      metric, MUPTI_METRIC_ATTR_VALUE_KIND, &value_kind_size, &res));
  return res;
}

MUpti_MetricEvaluationMode MuptiMetricApi::evaluationMode(
    MUpti_MetricID metric) {
  MUpti_MetricEvaluationMode eval_mode{
      MUPTI_METRIC_EVALUATION_MODE_PER_INSTANCE};
  size_t eval_mode_size = sizeof(eval_mode);
  MUPTI_CALL(muptiMetricGetAttribute(
      metric, MUPTI_METRIC_ATTR_EVALUATION_MODE, &eval_mode_size, &eval_mode));
  return eval_mode;
}

// FIXME: Consider caching value kind here
SampleValue MuptiMetricApi::calculate(
    MUpti_MetricID metric,
    MUpti_MetricValueKind kind,
    vector<MUpti_EventID>& events,
    vector<int64_t>& values,
    int64_t duration) {
  MUpti_MetricValue metric_value;
  MUPTI_CALL(muptiMetricGetValue(
      device_,
      metric,
      events.size() * sizeof(MUpti_EventID),
      events.data(),
      values.size() * sizeof(int64_t),
      reinterpret_cast<uint64_t*>(values.data()),
      duration,
      &metric_value));

  switch (kind) {
    case MUPTI_METRIC_VALUE_KIND_DOUBLE:
    case MUPTI_METRIC_VALUE_KIND_PERCENT:
      return SampleValue(metric_value.metricValueDouble);
    case MUPTI_METRIC_VALUE_KIND_UINT64:
    case MUPTI_METRIC_VALUE_KIND_INT64:
    case MUPTI_METRIC_VALUE_KIND_THROUGHPUT:
      return SampleValue(metric_value.metricValueUint64);
    case MUPTI_METRIC_VALUE_KIND_UTILIZATION_LEVEL:
      return SampleValue((int)metric_value.metricValueUtilizationLevel);
    default:
      assert(false);
  }
  return SampleValue(-1);
}

} // namespace KINETO_NAMESPACE
