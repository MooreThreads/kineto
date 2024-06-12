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

#include <assert.h>
#include <chrono>
#include <algorithm>
#include <mutex>
#include <shared_mutex>

#ifdef HAS_MUPTI
#include "mupti_call.h"
#endif
#include "Logger.h"


namespace KINETO_NAMESPACE {

// limit on number of handles per callback type
constexpr size_t MAX_CB_FNS_PER_CB = 8;

// Use this value in enabledCallbacks_ set, when all cbids in a domain
// is enabled, not a specific cbid.
constexpr uint32_t MAX_MUPTI_CALLBACK_ID_ALL = 0xffffffff;

// Reader Writer lock types
using ReaderWriterLock = std::shared_timed_mutex;
using ReaderLockGuard = std::shared_lock<ReaderWriterLock>;
using WriteLockGuard = std::unique_lock<ReaderWriterLock>;

static ReaderWriterLock callbackLock_;

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
#ifdef HAS_MUPTI
static void MUPTIAPI callback_switchboard(
#else
static void callback_switchboard(
#endif
   void* /* unused */,
   MUpti_CallbackDomain domain,
   MUpti_CallbackId cbid,
   const MUpti_CallbackData* cbInfo) {

  // below statement is likey going to call a mutex
  // on the singleton access
  // MuptiCallbackApi::singleton()->__callback_switchboard(
  //     domain, cbid, cbInfo);
}


void MuptiCallbackApi::__callback_switchboard(
   MUpti_CallbackDomain domain,
   MUpti_CallbackId cbid,
   const MUpti_CallbackData* cbInfo) {
  VLOG(0) << "Callback: domain = " << domain << ", cbid = " << cbid;
  CallbackList *cblist = nullptr;

  switch (domain) {
    // add the fastest path for kernel launch callbacks
    // as these are the most frequent ones
    case MUPTI_CB_DOMAIN_RUNTIME_API:
      switch (cbid) {
        case MUPTI_RUNTIME_TRACE_CBID_musaLaunchKernel_v7000:
          cblist = &callbacks_.runtime[
            MUSA_LAUNCH_KERNEL - __RUNTIME_CB_DOMAIN_START];
          break;
        default:
          break;
      }
      // This is required to teardown mupti after profiling to prevent QPS slowdown.
      if (MuptiActivityApi::singleton().teardownMupti_) {
        if (cbInfo->callbackSite == MUPTI_API_EXIT) {
          LOG(INFO) << "  Calling muptiFinalize in exit callsite";
          // Teardown MUPTI calling muptiFinalize()
          MUPTI_CALL(muptiUnsubscribe(subscriber_));
          // TODO: MUPTI muptiFinalize is not yet implemented
          // MUPTI_CALL(muptiFinalize());
          initSuccess_ = false;
          subscriber_ = 0;
          MuptiActivityApi::singleton().teardownMupti_ = 0;
          MuptiActivityApi::singleton().finalizeCond_.notify_all();
          return;
        }
      }
      break;

    case MUPTI_CB_DOMAIN_RESOURCE:
      switch (cbid) {
        case MUPTI_CBID_RESOURCE_CONTEXT_CREATED:
          cblist = &callbacks_.resource[
            RESOURCE_CONTEXT_CREATED - __RESOURCE_CB_DOMAIN_START];
          break;
        case MUPTI_CBID_RESOURCE_CONTEXT_DESTROY_STARTING:
          cblist = &callbacks_.resource[
            RESOURCE_CONTEXT_DESTROYED - __RESOURCE_CB_DOMAIN_START];
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
  int num_cbs = 0;
  {
    ReaderLockGuard rl(callbackLock_);
    int i = 0;
    for (auto it = cblist->begin();
        it != cblist->end() && i < MAX_CB_FNS_PER_CB;
        it++, i++) {
      callbacks[i] = *it;
    }
    num_cbs = i;
  }

  for (int i = 0; i < num_cbs; i++) {
    auto fn = callbacks[i];
    fn(domain, cbid, cbInfo);
  }
}

std::shared_ptr<MuptiCallbackApi> MuptiCallbackApi::singleton() {
	static const std::shared_ptr<MuptiCallbackApi>
		instance = [] {
			std::shared_ptr<MuptiCallbackApi> inst =
				std::shared_ptr<MuptiCallbackApi>(new MuptiCallbackApi());
			return inst;
	}();
  return instance;
}

void MuptiCallbackApi::initCallbackApi() {
#ifdef HAS_MUPTI
  lastMuptiStatus_ = MUPTI_ERROR_UNKNOWN;
  lastMuptiStatus_ = MUPTI_CALL_NOWARN(
    muptiSubscribe(&subscriber_,
      (MUpti_CallbackFunc)callback_switchboard,
      nullptr));
  if (lastMuptiStatus_ != MUPTI_SUCCESS) {
    VLOG(1)  << "Failed muptiSubscribe, status: " << lastMuptiStatus_;
  }

  initSuccess_ = (lastMuptiStatus_ == MUPTI_SUCCESS);
#endif
}

MuptiCallbackApi::CallbackList* MuptiCallbackApi::CallbackTable::lookup(
    MUpti_CallbackDomain domain, MuptiCallBackID cbid) {
  size_t idx;

  switch (domain) {

    case MUPTI_CB_DOMAIN_RESOURCE:
      assert(cbid >= __RESOURCE_CB_DOMAIN_START);
      assert(cbid < __RESOURCE_CB_DOMAIN_END);
      idx = cbid - __RESOURCE_CB_DOMAIN_START;
      return &resource.at(idx);

    case MUPTI_CB_DOMAIN_RUNTIME_API:
      assert(cbid >= __RUNTIME_CB_DOMAIN_START);
      assert(cbid < __RUNTIME_CB_DOMAIN_END);
      idx = cbid - __RUNTIME_CB_DOMAIN_START;
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
                 << " callback id = " << cbid;
    return false;
  }

  // avoid duplicates
  auto it = std::find(cblist->begin(), cblist->end(), cbfn);
  if (it != cblist->end()) {
    LOG(WARNING) << "Adding duplicate callback -- domain = " << domain
                 << " callback id = " << cbid;
    return true;
  }

  if (cblist->size() == MAX_CB_FNS_PER_CB) {
    LOG(WARNING) << "Already registered max callback -- domain = " << domain
                 << " callback id = " << cbid;
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
    LOG(WARNING) << "Attempting to remove unsupported callback -- domain = " << domain
                 << " callback id = " << cbid;
    return false;
  }

  // Locks are not required here as
  //  https://en.cppreference.com/w/cpp/container/list/erase
  //  "References and iterators to the erased elements are invalidated.
  //   Other references and iterators are not affected."
  auto it = std::find(cblist->begin(), cblist->end(), cbfn);
  if (it == cblist->end()) {
    LOG(WARNING) << "Could not find callback to remove -- domain = " << domain
                 << " callback id = " << cbid;
    return false;
  }

  WriteLockGuard wl(callbackLock_);
  cblist->erase(it);
  return true;
}

bool MuptiCallbackApi::enableCallback(
    MUpti_CallbackDomain domain, MUpti_CallbackId cbid) {
#ifdef HAS_MUPTI
  if (initSuccess_) {
    lastMuptiStatus_ = MUPTI_CALL_NOWARN(
        muptiEnableCallback(1, subscriber_, domain, cbid));
    enabledCallbacks_.insert({domain, cbid});
    return (lastMuptiStatus_ == MUPTI_SUCCESS);
  }
#endif
  return false;
}

bool MuptiCallbackApi::disableCallback(
    MUpti_CallbackDomain domain, MUpti_CallbackId cbid) {
#ifdef HAS_MUPTI
  enabledCallbacks_.erase({domain, cbid});
  if (initSuccess_) {
    lastMuptiStatus_ = MUPTI_CALL_NOWARN(
        muptiEnableCallback(0, subscriber_, domain, cbid));
    return (lastMuptiStatus_ == MUPTI_SUCCESS);
  }
#endif
  return false;
}

bool MuptiCallbackApi::enableCallbackDomain(
    MUpti_CallbackDomain domain) {
#ifdef HAS_MUPTI
  if (initSuccess_) {
    lastMuptiStatus_ = MUPTI_CALL_NOWARN(
        muptiEnableDomain(1, subscriber_, domain));
    enabledCallbacks_.insert({domain, MAX_MUPTI_CALLBACK_ID_ALL});
    return (lastMuptiStatus_ == MUPTI_SUCCESS);
  }
#endif
  return false;
}

bool MuptiCallbackApi::disableCallbackDomain(
    MUpti_CallbackDomain domain) {
#ifdef HAS_MUPTI
  enabledCallbacks_.erase({domain, MAX_MUPTI_CALLBACK_ID_ALL});
  if (initSuccess_) {
    lastMuptiStatus_ = MUPTI_CALL_NOWARN(
        muptiEnableDomain(0, subscriber_, domain));
    return (lastMuptiStatus_ == MUPTI_SUCCESS);
  }
#endif
  return false;
}

bool MuptiCallbackApi::reenableCallbacks() {
#ifdef HAS_MUPTI
  if (initSuccess_) {
    for (auto& cbpair : enabledCallbacks_) {
      if ((uint32_t)cbpair.second == MAX_MUPTI_CALLBACK_ID_ALL) {
        lastMuptiStatus_ = MUPTI_CALL_NOWARN(
            muptiEnableDomain(1, subscriber_, cbpair.first));
      } else {
        lastMuptiStatus_ = MUPTI_CALL_NOWARN(
            muptiEnableCallback(1, subscriber_, cbpair.first, cbpair.second));
      }
    }
    return (lastMuptiStatus_ == MUPTI_SUCCESS);
  }
#endif
  return false;
}

} // namespace KINETO_NAMESPACE
