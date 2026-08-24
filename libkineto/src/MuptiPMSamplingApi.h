/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct MUpti_PmSampling_Object;
struct MUpti_Profiler_Host_Object;

namespace KINETO_NAMESPACE {

struct MuptiPMSample {
  // Raw timestamps produced through the same registered timestamp source as
  // MUPTI Activity API records.
  uint64_t rawStartTimestamp;
  uint64_t rawEndTimestamp;
  std::vector<double> values; // One value per metric
};

struct MuptiPMSamplingConfig {
  int32_t deviceId{-1};
  std::vector<std::string> metricNames;
  std::chrono::nanoseconds samplingInterval{0};
};

class MuptiPMSamplingApi {
 public:
  MuptiPMSamplingApi() = default;
  MuptiPMSamplingApi(const MuptiPMSamplingApi&) = delete;
  MuptiPMSamplingApi& operator=(const MuptiPMSamplingApi&) = delete;
  MuptiPMSamplingApi(MuptiPMSamplingApi&&) = delete;
  MuptiPMSamplingApi& operator=(MuptiPMSamplingApi&&) = delete;

  virtual ~MuptiPMSamplingApi();

  virtual void configure(const MuptiPMSamplingConfig& config);
  virtual void start();
  // Appends one batch and returns true if all current records were decoded.
  virtual bool decode(std::vector<MuptiPMSample>& samples);
  virtual void stop();
  virtual void disable();

 private:
  void ensureConfigured() const;
  void configureMupti();
  void resetImage();
  MuptiPMSample decodeSample(size_t sampleIndex);

  MuptiPMSamplingConfig config_;
  // Points into config_.metricNames and must be rebuilt whenever config_ is
  // replaced. config_ remains unchanged while these pointers are in use.
  std::vector<const char*> metricNamePtrs_;
  // Non-owning pointers to MUPTI-managed objects. disable() ends their
  // lifetimes.
  MUpti_Profiler_Host_Object* hostObject_{nullptr};
  MUpti_PmSampling_Object* samplingObject_{nullptr};
  bool profilerInitialized_{false};
  // MUPTI retains this buffer until PM sampling is disabled.
  std::vector<uint8_t> configImage_;
  std::vector<uint8_t> counterDataImage_;
};

} // namespace KINETO_NAMESPACE
