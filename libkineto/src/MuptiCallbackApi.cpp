/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

// TODO(T90238193)
// @lint-ignore-every CLANGTIDY facebook-hte-RelativeInclude
#include "MuptiCallbackApi.h"
#include "MuptiActivityApi.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <mutex>

#include "DeviceUtil.h"
#include "Logger.h"

namespace KINETO_NAMESPACE {

// limit on number of handles per callback type
constexpr size_t MAX_CB_FNS_PER_CB = 8;

// Use this value in enabledCallbacks_ set, when all cbids in a domain
// is enabled, not a specific cbid.
constexpr uint32_t MAX_MUPTI_CALLBACK_ID_ALL = 0xffffffff;

/* Callback Table :
 *  Overall goal of the design is to optimize the lookup of function
 *  pointers. The table is structured at two levels and the leaf
 *  elements in the table are std::list to enable fast access/inserts/deletes
 *
 *   <callback domain0> |
 *                     -> cb id 0 -> std::list of callbacks
 *                     ...
 *                     -> cb id n -> std::list of callbacks
 *   <callback domain1> |
 *                    ...
 *  CallbackTable is the finaly table type above
 *  See type declrartions in header file.
 */

/* callback_switchboard : is the global callback handler we register
 *  with MUPTI. The goal is to make it as efficient as possible
 *  to re-direct to the registered callback(s).
 *
 *  Few things to care about :
 *   a) use if/then switches rather than map/hash structures
 *   b) avoid dynamic memory allocations
 *   c) be aware of locking overheads
 */
static void MUPTIAPI callback_switchboard(
    void* /* unused */,
    MUpti_CallbackDomain domain,
    MUpti_CallbackId cbid,
    const MUpti_CallbackData* cbInfo) {
  // __callback_switchboard acquires a reader lock
  // on the callback list
  MuptiCallbackApi::singleton().__callback_switchboard(domain, cbid, cbInfo);
}

void MuptiCallbackApi::__callback_switchboard(
    MUpti_CallbackDomain domain,
    MUpti_CallbackId cbid,
    const MUpti_CallbackData* cbInfo) {
  LOG(INFO) << "Callback: domain = " << domain << ", cbid = " << cbid;
  CallbackList* cblist = nullptr;

  switch (domain) {
    // add the fastest path for kernel launch callbacks
    // as these are the most frequent ones
    case MUPTI_CB_DOMAIN_RUNTIME_API:
      switch (cbid) {
        case MUPTI_RUNTIME_TRACE_CBID_musaLaunchKernel_v7000:
          cblist = &callbacks_.runtime[domainIndex(
              MuptiCallBackID::MUSA_LAUNCH_KERNEL,
              MuptiCallBackID::__RUNTIME_CB_DOMAIN_START)];
          break;
#if defined(MUSA_VERSION) && (MUSA_VERSION >= 11080)
        case MUPTI_RUNTIME_TRACE_CBID_musaLaunchKernelExC_v11060:
          cblist = &callbacks_.runtime[domainIndex(
              MuptiCallBackID::MUSA_LAUNCH_KERNEL_EXC,
              MuptiCallBackID::__RUNTIME_CB_DOMAIN_START)];
          break;
#endif
        default:
          break;
      }
      // This is required to teardown mupti after profiling to prevent QPS
      // slowdown.
      if (MuptiActivityApi::singleton().teardownMupti_) {
        if (cbInfo->callbackSite == MUPTI_API_EXIT) {
          LOG(INFO) << "  Calling muptiFinalize in exit callsite";
          // Teardown MUPTI calling muptiFinalize()
          MUPTI_CALL(muptiUnsubscribe(subscriber_));
          MUPTI_CALL(muptiFinalize());
          initSuccess_ = false;
          subscriber_ = nullptr;
          MuptiActivityApi::singleton().teardownMupti_ = 0;
          MuptiActivityApi::singleton().finalizeCond_.notify_all();
          return;
        }
      }
      break;

    case MUPTI_CB_DOMAIN_RESOURCE:
      switch (cbid) {
        case MUPTI_CBID_RESOURCE_CONTEXT_CREATED:
          cblist = &callbacks_.resource[domainIndex(
              MuptiCallBackID::RESOURCE_CONTEXT_CREATED,
              MuptiCallBackID::__RESOURCE_CB_DOMAIN_START)];
          break;
        case MUPTI_CBID_RESOURCE_CONTEXT_DESTROY_STARTING:
          cblist = &callbacks_.resource[domainIndex(
              MuptiCallBackID::RESOURCE_CONTEXT_DESTROYED,
              MuptiCallBackID::__RESOURCE_CB_DOMAIN_START)];
          break;
        default:
          break;
      }
      break;

    default:
      return;
  }

  // ignore callbacks that are not handled
  if (cblist == nullptr) {
    return;
  }

  // make a copy of the callback list so we avoid holding lock
  // in common case this should be just one func pointer copy
  std::array<MuptiCallbackFn, MAX_CB_FNS_PER_CB> callbacks;
  size_t num_cbs = 0;
  {
    ReaderLockGuard rl(callbackLock_);
    size_t i = 0;
    for (auto it = cblist->begin();
         it != cblist->end() && i < MAX_CB_FNS_PER_CB;
         it++, i++) {
      callbacks[i] = *it;
    }
    num_cbs = i;
  }

  for (size_t i = 0; i < num_cbs; i++) {
    auto fn = callbacks[i];
    fn(domain, cbid, cbInfo);
  }
}

MuptiCallbackApi& MuptiCallbackApi::singleton() {
  static auto* instance = new MuptiCallbackApi();
  return *instance;
}

void MuptiCallbackApi::initCallbackApi() {
  lastMuptiStatus_ = MUPTI_ERROR_UNKNOWN;
  lastMuptiStatus_ = MUPTI_CALL_NOWARN(muptiSubscribe(
      &subscriber_,
      reinterpret_cast<MUpti_CallbackFunc>(callback_switchboard),
      nullptr));

  // TODO: Remove temporarily to work around static initialization order issue
  // betweent this and GLOG.
  // if (lastMuptiStatus_ != MUPTI_SUCCESS) {
  //   LOG(INFO) << "Failed muptiSubscribe, status: " << lastMuptiStatus_;
  // }

  initSuccess_ = (lastMuptiStatus_ == MUPTI_SUCCESS);
}

MuptiCallbackApi::CallbackList* MuptiCallbackApi::CallbackTable::lookup(
    MUpti_CallbackDomain domain,
    MuptiCallBackID cbid) {
  size_t idx;

  switch (domain) {
    case MUPTI_CB_DOMAIN_RESOURCE:
      assert(cbid >= MuptiCallBackID::__RESOURCE_CB_DOMAIN_START);
      assert(cbid < MuptiCallBackID::__RESOURCE_CB_DOMAIN_END);
      idx = domainIndex(cbid, MuptiCallBackID::__RESOURCE_CB_DOMAIN_START);
      return &resource.at(idx);

    case MUPTI_CB_DOMAIN_RUNTIME_API:
      assert(cbid >= MuptiCallBackID::__RUNTIME_CB_DOMAIN_START);
      assert(cbid < MuptiCallBackID::__RUNTIME_CB_DOMAIN_END);
      idx = domainIndex(cbid, MuptiCallBackID::__RUNTIME_CB_DOMAIN_START);
      return &runtime.at(idx);

    default:
      LOG(WARNING) << " Unsupported callback domain : " << domain;
      return nullptr;
  }
}

bool MuptiCallbackApi::registerCallback(
    MUpti_CallbackDomain domain,
    MuptiCallBackID cbid,
    MuptiCallbackFn cbfn) {
  CallbackList* cblist = callbacks_.lookup(domain, cbid);

  if (!cblist) {
    LOG(WARNING) << "Could not register callback -- domain = " << domain
                 << " callback id = " << static_cast<int>(cbid);
    return false;
  }

  // avoid duplicates
  auto it = std::ranges::find(*cblist, cbfn);
  if (it != cblist->end()) {
    LOG(WARNING) << "Adding duplicate callback -- domain = " << domain
                 << " callback id = " << static_cast<int>(cbid);
    return true;
  }

  if (cblist->size() == MAX_CB_FNS_PER_CB) {
    LOG(WARNING) << "Already registered max callback -- domain = " << domain
                 << " callback id = " << static_cast<int>(cbid);
  }

  WriteLockGuard wl(callbackLock_);
  cblist->push_back(cbfn);
  return true;
}

bool MuptiCallbackApi::deleteCallback(
    MUpti_CallbackDomain domain,
    MuptiCallBackID cbid,
    MuptiCallbackFn cbfn) {
  CallbackList* cblist = callbacks_.lookup(domain, cbid);
  if (!cblist) {
    LOG(WARNING) << "Attempting to remove unsupported callback -- domain = "
                 << domain << " callback id = " << static_cast<int>(cbid);
    return false;
  }

  // Locks are not required here as
  //  https://en.cppreference.com/w/cpp/container/list/erase
  //  "References and iterators to the erased elements are invalidated.
  //   Other references and iterators are not affected."
  auto it = std::ranges::find(*cblist, cbfn);
  if (it == cblist->end()) {
    LOG(WARNING) << "Could not find callback to remove -- domain = " << domain
                 << " callback id = " << static_cast<int>(cbid);
    return false;
  }

  WriteLockGuard wl(callbackLock_);
  cblist->erase(it);
  return true;
}

bool MuptiCallbackApi::enableCallback(
    MUpti_CallbackDomain domain,
    MUpti_CallbackId cbid) {
  if (initSuccess_) {
    lastMuptiStatus_ =
        MUPTI_CALL_NOWARN(muptiEnableCallback(1, subscriber_, domain, cbid));
    enabledCallbacks_.insert({domain, cbid});
    return (lastMuptiStatus_ == MUPTI_SUCCESS);
  }
  return false;
}

bool MuptiCallbackApi::disableCallback(
    MUpti_CallbackDomain domain,
    MUpti_CallbackId cbid) {
  enabledCallbacks_.erase({domain, cbid});
  if (initSuccess_) {
    lastMuptiStatus_ =
        MUPTI_CALL_NOWARN(muptiEnableCallback(0, subscriber_, domain, cbid));
    return (lastMuptiStatus_ == MUPTI_SUCCESS);
  }
  return false;
}

bool MuptiCallbackApi::enableCallbackDomain(MUpti_CallbackDomain domain) {
  if (initSuccess_) {
    lastMuptiStatus_ =
        MUPTI_CALL_NOWARN(muptiEnableDomain(1, subscriber_, domain));
    enabledCallbacks_.insert({domain, MAX_MUPTI_CALLBACK_ID_ALL});
    return (lastMuptiStatus_ == MUPTI_SUCCESS);
  }
  return false;
}

bool MuptiCallbackApi::disableCallbackDomain(MUpti_CallbackDomain domain) {
  enabledCallbacks_.erase({domain, MAX_MUPTI_CALLBACK_ID_ALL});
  if (initSuccess_) {
    lastMuptiStatus_ =
        MUPTI_CALL_NOWARN(muptiEnableDomain(0, subscriber_, domain));
    return (lastMuptiStatus_ == MUPTI_SUCCESS);
  }
  return false;
}

bool MuptiCallbackApi::reenableCallbacks() {
  if (initSuccess_) {
    for (auto& cbpair : enabledCallbacks_) {
      if (static_cast<uint32_t>(cbpair.second) == MAX_MUPTI_CALLBACK_ID_ALL) {
        lastMuptiStatus_ =
            MUPTI_CALL_NOWARN(muptiEnableDomain(1, subscriber_, cbpair.first));
      } else {
        lastMuptiStatus_ = MUPTI_CALL_NOWARN(
            muptiEnableCallback(1, subscriber_, cbpair.first, cbpair.second));
      }
    }
    return (lastMuptiStatus_ == MUPTI_SUCCESS);
  }
  return false;
}

} // namespace KINETO_NAMESPACE
