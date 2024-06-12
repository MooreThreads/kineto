/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <stdlib.h>
#include <assert.h>
#include <cstdint>
#include <map>
#include <memory>
#include <sys/types.h>
#include <vector>

#include "ITraceActivity.h"

namespace KINETO_NAMESPACE {

class MuptiActivityBuffer {
 public:
  explicit MuptiActivityBuffer(size_t size) : size_(size) {
    buf_.reserve(size);
  }
  MuptiActivityBuffer() = delete;
  MuptiActivityBuffer& operator=(const MuptiActivityBuffer&) = delete;
  MuptiActivityBuffer(MuptiActivityBuffer&&) = default;
  MuptiActivityBuffer& operator=(MuptiActivityBuffer&&) = default;

  size_t size() const {
    return size_;
  }

  void setSize(size_t size) {
    assert(size <= buf_.capacity());
    size_ = size;
  }

  uint8_t* data() {
    return buf_.data();
  }

 private:

  std::vector<uint8_t> buf_;
  size_t size_;

  std::vector<std::unique_ptr<const ITraceActivity>> wrappers_;
};

using MuptiActivityBufferMap =
    std::map<uint8_t*, std::unique_ptr<MuptiActivityBuffer>>;

} // namespace KINETO_NAMESPACE
