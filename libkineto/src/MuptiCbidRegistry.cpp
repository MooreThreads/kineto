/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MuptiCbidRegistry.h"
#include "ThrowUtil.h"

#include <stdexcept>

namespace KINETO_NAMESPACE {

MuptiCbidRegistry& MuptiCbidRegistry::instance() {
  static MuptiCbidRegistry instance;
  return instance;
}

std::unordered_map<uint32_t, MuptiCbidRegistry::CbidProperties>&
MuptiCbidRegistry::getMapForDomain(CallbackDomain domain) {
  switch (domain) {
    case CallbackDomain::RUNTIME:
      return runtimeCallbacks_;
    case CallbackDomain::DRIVER:
      return driverCallbacks_;
    default:
      KINETO_THROW(std::invalid_argument, "Unknown CallbackDomain");
  }
}

void MuptiCbidRegistry::registerCbid(
    CallbackDomain domain,
    uint32_t cbid,
    bool requiresFlowCorrelation,
    bool isBlocklisted) {
  getMapForDomain(domain)[cbid] =
      CbidProperties{requiresFlowCorrelation, isBlocklisted};
}

void MuptiCbidRegistry::registerCbidRange(
    CallbackDomain domain,
    uint32_t startCbid,
    uint32_t endCbid,
    bool requiresFlowCorrelation,
    bool isBlocklisted) {
  cbidRanges_.emplace_back(
      domain,
      CbidRange{startCbid, endCbid, requiresFlowCorrelation, isBlocklisted});
}

MuptiCbidRegistry::MuptiCbidRegistry() {
  // =========================================================================
  // RUNTIME API - Kernel Launches
  // =========================================================================
  registerCbid(
      /*domain=*/CallbackDomain::RUNTIME,
      /*cbid=*/MUPTI_RUNTIME_TRACE_CBID_musaLaunchKernel_v7000,
      /*requiresFlowCorrelation=*/true,
      /*isBlocklisted=*/false);

  registerCbid(
      /*domain=*/CallbackDomain::RUNTIME,
      /*cbid=*/MUPTI_RUNTIME_TRACE_CBID_musaLaunchCooperativeKernel_v9000,
      /*requiresFlowCorrelation=*/true,
      /*isBlocklisted=*/false);

  registerCbid(
      /*domain=*/CallbackDomain::RUNTIME,
      /*cbid=*/
      MUPTI_RUNTIME_TRACE_CBID_musaLaunchCooperativeKernelMultiDevice_v9000,
      /*requiresFlowCorrelation=*/true,
      /*isBlocklisted=*/false);

#if defined(MUPTI_API_VERSION) && MUPTI_API_VERSION >= 18
  registerCbid(
      /*domain=*/CallbackDomain::RUNTIME,
      /*cbid=*/MUPTI_RUNTIME_TRACE_CBID_musaLaunchKernelExC_v11060,
      /*requiresFlowCorrelation=*/true,
      /*isBlocklisted=*/false);
#endif

  // =========================================================================
  // RUNTIME API - MUSA Graph Operations
  // =========================================================================
  registerCbid(
      /*domain=*/CallbackDomain::RUNTIME,
      /*cbid=*/MUPTI_RUNTIME_TRACE_CBID_musaGraphLaunch_v10000,
      /*requiresFlowCorrelation=*/true,
      /*isBlocklisted=*/false);

  // =========================================================================
  // RUNTIME API - Synchronization Operations
  // =========================================================================
  registerCbid(
      /*domain=*/CallbackDomain::RUNTIME,
      /*cbid=*/MUPTI_RUNTIME_TRACE_CBID_musaStreamSynchronize_v3020,
      /*requiresFlowCorrelation=*/true,
      /*isBlocklisted=*/false);

  registerCbid(
      /*domain=*/CallbackDomain::RUNTIME,
      /*cbid=*/MUPTI_RUNTIME_TRACE_CBID_musaDeviceSynchronize_v3020,
      /*requiresFlowCorrelation=*/true,
      /*isBlocklisted=*/false);

  registerCbid(
      /*domain=*/CallbackDomain::RUNTIME,
      /*cbid=*/MUPTI_RUNTIME_TRACE_CBID_musaStreamWaitEvent_v3020,
      /*requiresFlowCorrelation=*/true,
      /*isBlocklisted=*/false);

  // =========================================================================
  // RUNTIME API - Memory Operations (range-based)
  // =========================================================================
  registerCbidRange(
      /*domain=*/CallbackDomain::RUNTIME,
      /*startCbid=*/MUPTI_RUNTIME_TRACE_CBID_musaMemcpy_v3020,
      /*endCbid=*/MUPTI_RUNTIME_TRACE_CBID_musaMemset2DAsync_v3020,
      /*requiresFlowCorrelation=*/true,
      /*isBlocklisted=*/false);

  // =========================================================================
  // RUNTIME API - Blocklisted (noisy) Callbacks
  // =========================================================================
  registerCbid(
      /*domain=*/CallbackDomain::RUNTIME,
      /*cbid=*/MUPTI_RUNTIME_TRACE_CBID_musaGetDevice_v3020,
      /*requiresFlowCorrelation=*/false,
      /*isBlocklisted=*/true);

  registerCbid(
      /*domain=*/CallbackDomain::RUNTIME,
      /*cbid=*/MUPTI_RUNTIME_TRACE_CBID_musaSetDevice_v3020,
      /*requiresFlowCorrelation=*/false,
      /*isBlocklisted=*/true);

  registerCbid(
      /*domain=*/CallbackDomain::RUNTIME,
      /*cbid=*/MUPTI_RUNTIME_TRACE_CBID_musaGetLastError_v3020,
      /*requiresFlowCorrelation=*/false,
      /*isBlocklisted=*/true);

  registerCbid(
      /*domain=*/CallbackDomain::RUNTIME,
      /*cbid=*/MUPTI_RUNTIME_TRACE_CBID_musaEventCreate_v3020,
      /*requiresFlowCorrelation=*/false,
      /*isBlocklisted=*/true);

  registerCbid(
      /*domain=*/CallbackDomain::RUNTIME,
      /*cbid=*/MUPTI_RUNTIME_TRACE_CBID_musaEventCreateWithFlags_v3020,
      /*requiresFlowCorrelation=*/false,
      /*isBlocklisted=*/true);

  registerCbid(
      /*domain=*/CallbackDomain::RUNTIME,
      /*cbid=*/MUPTI_RUNTIME_TRACE_CBID_musaEventDestroy_v3020,
      /*requiresFlowCorrelation=*/false,
      /*isBlocklisted=*/true);

  // =========================================================================
  // DRIVER API - Kernel Launches
  // =========================================================================
  registerCbid(
      /*domain=*/CallbackDomain::DRIVER,
      /*cbid=*/MUPTI_DRIVER_TRACE_CBID_muLaunchKernel,
      /*requiresFlowCorrelation=*/true,
      /*isBlocklisted=*/false);
#if defined(MUSA_VERSION) && MUSA_VERSION >= 11060
  registerCbid(
      /*domain=*/CallbackDomain::DRIVER,
      /*cbid=*/MUPTI_DRIVER_TRACE_CBID_muLaunchKernelEx,
      /*requiresFlowCorrelation=*/true,
      /*isBlocklisted=*/false);
#endif
  registerCbid(
      /*domain=*/CallbackDomain::DRIVER,
      /*cbid=*/MUPTI_DRIVER_TRACE_CBID_muMemCreate,
      /*requiresFlowCorrelation=*/false,
      /*isBlocklisted=*/false);
  registerCbid(
      /*domain=*/CallbackDomain::DRIVER,
      /*cbid=*/MUPTI_DRIVER_TRACE_CBID_muMemMap,
      /*requiresFlowCorrelation=*/false,
      /*isBlocklisted=*/false);
  registerCbid(
      /*domain=*/CallbackDomain::DRIVER,
      /*cbid=*/MUPTI_DRIVER_TRACE_CBID_muMemUnmap,
      /*requiresFlowCorrelation=*/false,
      /*isBlocklisted=*/false);
  registerCbid(
      /*domain=*/CallbackDomain::DRIVER,
      /*cbid=*/MUPTI_DRIVER_TRACE_CBID_muMemRelease,
      /*requiresFlowCorrelation=*/false,
      /*isBlocklisted=*/false);
  registerCbid(
      /*domain=*/CallbackDomain::DRIVER,
      /*cbid=*/MUPTI_DRIVER_TRACE_CBID_muMemExportToShareableHandle,
      /*requiresFlowCorrelation=*/false,
      /*isBlocklisted=*/false);
  registerCbid(
      /*domain=*/CallbackDomain::DRIVER,
      /*cbid=*/MUPTI_DRIVER_TRACE_CBID_muMemImportFromShareableHandle,
      /*requiresFlowCorrelation=*/false,
      /*isBlocklisted=*/false);
}

bool MuptiCbidRegistry::requiresFlowCorrelation(
    CallbackDomain domain,
    uint32_t cbid) {
  // Check explicit callback IDs first
  const auto& map = getMapForDomain(domain);
  if (auto it = map.find(cbid); it != map.end()) {
    return it->second.requiresFlowCorrelation;
  }
  // Check ranges (for memory operations)
  // TODO: reevaluate the loop once we iterate of many ranges
  for (const auto& [rangeDomain, range] : cbidRanges_) {
    if (rangeDomain == domain && cbid >= range.startCbid &&
        cbid <= range.endCbid) {
      return range.requiresFlowCorrelation;
    }
  }
  return false;
}

bool MuptiCbidRegistry::isBlocklisted(CallbackDomain domain, uint32_t cbid) {
  // Check explicit callbacks first
  const auto& map = getMapForDomain(domain);
  if (auto it = map.find(cbid); it != map.end()) {
    return it->second.isBlocklisted;
  }
  // Check ranges
  // TODO: reevaluate the loop once we iterate of many ranges
  for (const auto& [rangeDomain, range] : cbidRanges_) {
    if (rangeDomain == domain && cbid >= range.startCbid &&
        cbid <= range.endCbid) {
      return range.isBlocklisted;
    }
  }
  return false;
}

bool MuptiCbidRegistry::isRegistered(CallbackDomain domain, uint32_t cbid) {
  const auto& map = getMapForDomain(domain);
  if (map.contains(cbid)) {
    return true;
  }
  for (const auto& [rangeDomain, range] : cbidRanges_) {
    if (rangeDomain == domain && cbid >= range.startCbid &&
        cbid <= range.endCbid) {
      return true;
    }
  }
  return false;
}

} // namespace KINETO_NAMESPACE
