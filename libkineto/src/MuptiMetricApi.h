/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <mupti.h>

#include <map>
#include <vector>

#include "SampleListener.h"

namespace KINETO_NAMESPACE {

// C++ interface to MUPTI Metrics C API.
// Virtual methods are here mainly to allow easier testing.
class MuptiMetricApi {
 public:
  explicit MuptiMetricApi(MUdevice device) : device_(device) {}
  virtual ~MuptiMetricApi() {}

  virtual MUpti_MetricID idFromName(const std::string& name);
  virtual std::map<MUpti_EventID, std::string> events(MUpti_MetricID metric_id);

  virtual MUpti_MetricValueKind valueKind(MUpti_MetricID metric);
  virtual MUpti_MetricEvaluationMode evaluationMode(MUpti_MetricID metric);

  virtual SampleValue calculate(
      MUpti_MetricID metric,
      MUpti_MetricValueKind kind,
      std::vector<MUpti_EventID>& events,
      std::vector<int64_t>& values,
      int64_t duration);

 private:
  MUdevice device_;
};

} // namespace KINETO_NAMESPACE
