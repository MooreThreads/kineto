/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MuptiPMSamplingController.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

#include "Logger.h"

/*
 * ======================== MUPTI PM SAMPLING CONTROLLER
 * ========================
 *
 * Controller for PM Sampling, which manages Kineto-side lifecycle. Hooks onto
 * the MuptiPMSamplingApi. A sampling session follows prepare() -> start() ->
 * stop().
 *
 * TODO LIST (in no particular order):
 *   1. Bound sample counts for large traces.
 *   2. Handle on-demand/async profiling, plus interaction with auto-trace/sync.
 *   3. Push hard-coded constants into user-provided vars.
 *   4. Add tests.
 *   5. Bound drain retries so shutdown cannot block indefinitely.
 *
 * ==============================================================================
 */

namespace KINETO_NAMESPACE {
namespace {

constexpr std::chrono::milliseconds kDecodePollInterval{10};

} // namespace

MuptiPMSamplingController::MuptiPMSamplingController(
    MuptiPMSamplingConfig config)
    : config_(std::move(config)) {}

MuptiPMSamplingController::~MuptiPMSamplingController() {
  static_cast<void>(stop());
}

bool MuptiPMSamplingController::prepare() {
  if (prepared_) {
    LOG(WARNING) << "MUPTI PM sampling is busy";
    return false;
  }
  decodeFailed_.store(false, std::memory_order_relaxed);

  try {
    {
      // Prevent the decode thread from appending to samples while this is being
      // cleared
      std::lock_guard<std::mutex> lock(samplesMutex_);
      samples_.clear();
      discardFirstSample_ = true;
    }
    if (!validateConfig()) {
      return false;
    }
    api_.configure(config_);
  } catch (...) {
    logCurrentException("Failed to prepare MUPTI PM sampling");
    api_.disable();
    return false;
  }
  prepared_ = true;
  return true;
}

void MuptiPMSamplingController::start() {
  if (!prepared_) {
    LOG(WARNING) << "MUPTI PM sampling is not prepared";
    return;
  }
  if (active_) {
    LOG(WARNING) << "MUPTI PM sampling is already running";
    return;
  }

  stopRequested_.store(false, std::memory_order_relaxed);
  try {
    api_.start();
    active_ = true;
    // Decode needs to happen on an async thread as it needs to synchronously
    // fetch new samples from the api
    decodeThread_ = std::thread(&MuptiPMSamplingController::decodeLoop, this);
  } catch (...) {
    logCurrentException("Failed to start MUPTI PM sampling");
    static_cast<void>(stop());
  }
}

bool MuptiPMSamplingController::stop() {
  // Notify the decode thread and join it if possible
  if (decodeThread_.joinable()) {
    {
      // Set the flag under waitMutex_ so the decode thread cannot miss the
      // notification between evaluating wait_for's predicate and blocking.
      std::lock_guard<std::mutex> lock(waitMutex_);
      stopRequested_.store(true, std::memory_order_relaxed);
    }
    waitCondition_.notify_one();
    decodeThread_.join();
  }
  if (!prepared_) {
    return false;
  }

  const bool wasActive = active_;
  // active_ indicates that the PM Sampling API was primed and collected samples
  // so we need to stop() and drain all remaining items in the hardware buffer.
  if (active_) {
    std::vector<MuptiPMSample> decodedSamples;
    try {
      api_.stop();
      if (!decodeFailed_.load(std::memory_order_relaxed)) {
        drain(decodedSamples);
      }
    } catch (...) {
      logCurrentException("Failed to stop or drain MUPTI PM sampling");
    }
    active_ = false;
  }
  api_.disable();
  prepared_ = false;
  return wasActive;
}

int32_t MuptiPMSamplingController::deviceId() const {
  return config_.deviceId;
}

const std::vector<std::string>& MuptiPMSamplingController::metricNames() const {
  return config_.metricNames;
}

std::vector<MuptiPMSample> MuptiPMSamplingController::takeSamples() {
  std::lock_guard<std::mutex> lock(samplesMutex_);
  return std::exchange(samples_, {});
}

void MuptiPMSamplingController::decodeLoop() {
  std::vector<MuptiPMSample> decodedSamples;
  try {
    // stopRequested_ flag signals that the main thread is stopping, we
    // use the waitCondition_ to wake the decode thread to return immediately.
    while (!stopRequested_.load(std::memory_order_relaxed)) {
      if (decodeBatch(decodedSamples)) {
        std::unique_lock<std::mutex> lock(waitMutex_);
        waitCondition_.wait_for(lock, kDecodePollInterval, [this]() {
          return stopRequested_.load(std::memory_order_relaxed);
        });
      }
    }
  } catch (...) {
    // If decodeFailed_ is true, we will skip drain() on the disable path as
    // this is the operation that failed.
    decodeFailed_.store(true, std::memory_order_relaxed);
    logCurrentException("MUPTI PM sampling worker failed");
  }
}

bool MuptiPMSamplingController::decodeBatch(
    std::vector<MuptiPMSample>& decodedSamples) {
  decodedSamples.clear();
  const bool isBufferDrained = api_.decode(decodedSamples);
  std::lock_guard<std::mutex> lock(samplesMutex_);
  for (auto& sample : decodedSamples) {
    // MUPTI recommends discarding the first sample because it may contain
    // outlier values.
    if (discardFirstSample_) {
      LOG(INFO) << "Discarding first MUPTI PM sample because it may contain "
                   "outlier values";
      discardFirstSample_ = false;
      continue;
    }
    if (validateSample(sample)) {
      samples_.push_back(std::move(sample));
    }
  }
  return isBufferDrained;
}

void MuptiPMSamplingController::drain(
    std::vector<MuptiPMSample>& decodedSamples) {
  // This relies on MUPTI eventually reporting that the hardware buffer is
  // drained. If MUPTI enters a bad state, this loop can block indefinitely,
  // including when called during destruction.
  bool isBufferDrained;
  do {
    isBufferDrained = decodeBatch(decodedSamples);
  } while (!isBufferDrained);
}

bool MuptiPMSamplingController::validateConfig() const {
  if (config_.deviceId < 0) {
    LOG(WARNING) << "MUPTI PM sampling device ID must be nonnegative";
    return false;
  }
  if (config_.metricNames.empty() ||
      std::any_of(
          config_.metricNames.begin(),
          config_.metricNames.end(),
          [](const std::string& name) { return name.empty(); })) {
    LOG(WARNING) << "MUPTI PM sampling metrics must not be empty";
    return false;
  }
  if (config_.samplingInterval.count() <= 0) {
    LOG(WARNING) << "MUPTI PM sampling interval must be positive";
    return false;
  }
  return true;
}

bool MuptiPMSamplingController::validateSample(
    const MuptiPMSample& sample) const {
  if (sample.values.size() != config_.metricNames.size()) {
    LOG_FIRST_N(WARNING, 3)
        << "MUPTI PM sample has the wrong number of metric values";
    return false;
  }
  if (sample.rawEndTimestamp < sample.rawStartTimestamp) {
    LOG_FIRST_N(WARNING, 3)
        << "MUPTI PM sample has an invalid timestamp interval";
    return false;
  }
  if (std::any_of(sample.values.begin(), sample.values.end(), [](double value) {
        return !std::isfinite(value);
      })) {
    LOG_FIRST_N(WARNING, 3)
        << "MUPTI PM sample contains a non-finite metric value";
    return false;
  }
  return true;
}

void MuptiPMSamplingController::logCurrentException(const char* fallback) {
  const auto exception = std::current_exception();
  if (exception == nullptr) {
    LOG(WARNING) << fallback;
    return;
  }
  try {
    std::rethrow_exception(exception);
  } catch (const std::exception& error) {
    LOG(WARNING) << error.what();
  } catch (...) {
    LOG(WARNING) << fallback;
  }
}

} // namespace KINETO_NAMESPACE
