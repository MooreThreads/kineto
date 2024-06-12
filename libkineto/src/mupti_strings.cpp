/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "mupti_strings.h"

namespace libkineto {

const char* memcpyKindString(
    MUpti_ActivityMemcpyKind kind) {
  switch (kind) {
    case MUPTI_ACTIVITY_MEMCPY_KIND_HTOD:
      return "HtoD";
    case MUPTI_ACTIVITY_MEMCPY_KIND_DTOH:
      return "DtoH";
    case MUPTI_ACTIVITY_MEMCPY_KIND_HTOA:
      return "HtoA";
    case MUPTI_ACTIVITY_MEMCPY_KIND_ATOH:
      return "AtoH";
    case MUPTI_ACTIVITY_MEMCPY_KIND_ATOA:
      return "AtoA";
    case MUPTI_ACTIVITY_MEMCPY_KIND_ATOD:
      return "AtoD";
    case MUPTI_ACTIVITY_MEMCPY_KIND_DTOA:
      return "DtoA";
    case MUPTI_ACTIVITY_MEMCPY_KIND_DTOD:
      return "DtoD";
    case MUPTI_ACTIVITY_MEMCPY_KIND_HTOH:
      return "HtoH";
    case MUPTI_ACTIVITY_MEMCPY_KIND_PTOP:
      return "PtoP";
    default:
      break;
  }
  return "<unknown>";
}

const char* memoryKindString(
    MUpti_ActivityMemoryKind kind) {
  switch (kind) {
    case MUPTI_ACTIVITY_MEMORY_KIND_UNKNOWN:
      return "Unknown";
    case MUPTI_ACTIVITY_MEMORY_KIND_PAGEABLE:
      return "Pageable";
    case MUPTI_ACTIVITY_MEMORY_KIND_PINNED:
      return "Pinned";
    case MUPTI_ACTIVITY_MEMORY_KIND_DEVICE:
      return "Device";
    case MUPTI_ACTIVITY_MEMORY_KIND_ARRAY:
      return "Array";
    case MUPTI_ACTIVITY_MEMORY_KIND_MANAGED:
      return "Managed";
    case MUPTI_ACTIVITY_MEMORY_KIND_DEVICE_STATIC:
      return "Device Static";
    case MUPTI_ACTIVITY_MEMORY_KIND_MANAGED_STATIC:
      return "Managed Static";
    case MUPTI_ACTIVITY_MEMORY_KIND_FORCE_INT:
      return "Force Int";
    default:
      return "Unrecognized";
  }
}

const char* overheadKindString(
    MUpti_ActivityOverheadKind kind) {
  switch (kind) {
    case MUPTI_ACTIVITY_OVERHEAD_UNKNOWN:
      return "Unknown";
    case MUPTI_ACTIVITY_OVERHEAD_DRIVER_COMPILER:
      return "Driver Compiler";
    case MUPTI_ACTIVITY_OVERHEAD_MUPTI_BUFFER_FLUSH:
      return "Buffer Flush";
    case MUPTI_ACTIVITY_OVERHEAD_MUPTI_INSTRUMENTATION:
      return "Instrumentation";
    case MUPTI_ACTIVITY_OVERHEAD_MUPTI_RESOURCE:
      return "Resource";
    case MUPTI_ACTIVITY_OVERHEAD_FORCE_INT:
      return "Force Int";
    default:
      return "Unrecognized";
  }
}



static const char* runtimeCbidNames[] = {
    "INVALID",
    "musaDriverGetVersion",
    "musaRuntimeGetVersion",
    "musaGetDeviceCount",
    "musaGetDeviceProperties",
    "musaChooseDevice",
    "musaGetChannelDesc",
    "musaCreateChannelDesc",
    "musaConfigureCall",
    "musaSetupArgument",
    "musaGetLastError",
    "musaPeekAtLastError",
    "musaGetErrorString",
    "musaLaunch",
    "musaFuncSetCacheConfig",
    "musaFuncGetAttributes",
    "musaSetDevice",
    "musaGetDevice",
    "musaSetValidDevices",
    "musaSetDeviceFlags",
    "musaMalloc",
    "musaMallocPitch",
    "musaFree",
    "musaMallocArray",
    "musaFreeArray",
    "musaMallocHost",
    "musaFreeHost",
    "musaHostAlloc",
    "musaHostGetDevicePointer",
    "musaHostGetFlags",
    "musaMemGetInfo",
    "musaMemcpy",
    "musaMemcpy2D",
    "musaMemcpyToArray",
    "musaMemcpy2DToArray",
    "musaMemcpyFromArray",
    "musaMemcpy2DFromArray",
    "musaMemcpyArrayToArray",
    "musaMemcpy2DArrayToArray",
    "musaMemcpyToSymbol",
    "musaMemcpyFromSymbol",
    "musaMemcpyAsync",
    "musaMemcpyToArrayAsync",
    "musaMemcpyFromArrayAsync",
    "musaMemcpy2DAsync",
    "musaMemcpy2DToArrayAsync",
    "musaMemcpy2DFromArrayAsync",
    "musaMemcpyToSymbolAsync",
    "musaMemcpyFromSymbolAsync",
    "musaMemset",
    "musaMemset2D",
    "musaMemsetAsync",
    "musaMemset2DAsync",
    "musaGetSymbolAddress",
    "musaGetSymbolSize",
    "musaBindTexture",
    "musaBindTexture2D",
    "musaBindTextureToArray",
    "musaUnbindTexture",
    "musaGetTextureAlignmentOffset",
    "musaGetTextureReference",
    "musaBindSurfaceToArray",
    "musaGetSurfaceReference",
    "musaGLSetGLDevice",
    "musaGLRegisterBufferObject",
    "musaGLMapBufferObject",
    "musaGLUnmapBufferObject",
    "musaGLUnregisterBufferObject",
    "musaGLSetBufferObjectMapFlags",
    "musaGLMapBufferObjectAsync",
    "musaGLUnmapBufferObjectAsync",
    "musaWGLGetDevice",
    "musaGraphicsGLRegisterImage",
    "musaGraphicsGLRegisterBuffer",
    "musaGraphicsUnregisterResource",
    "musaGraphicsResourceSetMapFlags",
    "musaGraphicsMapResources",
    "musaGraphicsUnmapResources",
    "musaGraphicsResourceGetMappedPointer",
    "musaGraphicsSubResourceGetMappedArray",
    "musaVDPAUGetDevice",
    "musaVDPAUSetVDPAUDevice",
    "musaGraphicsVDPAURegisterVideoSurface",
    "musaGraphicsVDPAURegisterOutputSurface",
    "musaD3D11GetDevice",
    "musaD3D11GetDevices",
    "musaD3D11SetDirect3DDevice",
    "musaGraphicsD3D11RegisterResource",
    "musaD3D10GetDevice",
    "musaD3D10GetDevices",
    "musaD3D10SetDirect3DDevice",
    "musaGraphicsD3D10RegisterResource",
    "musaD3D10RegisterResource",
    "musaD3D10UnregisterResource",
    "musaD3D10MapResources",
    "musaD3D10UnmapResources",
    "musaD3D10ResourceSetMapFlags",
    "musaD3D10ResourceGetSurfaceDimensions",
    "musaD3D10ResourceGetMappedArray",
    "musaD3D10ResourceGetMappedPointer",
    "musaD3D10ResourceGetMappedSize",
    "musaD3D10ResourceGetMappedPitch",
    "musaD3D9GetDevice",
    "musaD3D9GetDevices",
    "musaD3D9SetDirect3DDevice",
    "musaD3D9GetDirect3DDevice",
    "musaGraphicsD3D9RegisterResource",
    "musaD3D9RegisterResource",
    "musaD3D9UnregisterResource",
    "musaD3D9MapResources",
    "musaD3D9UnmapResources",
    "musaD3D9ResourceSetMapFlags",
    "musaD3D9ResourceGetSurfaceDimensions",
    "musaD3D9ResourceGetMappedArray",
    "musaD3D9ResourceGetMappedPointer",
    "musaD3D9ResourceGetMappedSize",
    "musaD3D9ResourceGetMappedPitch",
    "musaD3D9Begin",
    "musaD3D9End",
    "musaD3D9RegisterVertexBuffer",
    "musaD3D9UnregisterVertexBuffer",
    "musaD3D9MapVertexBuffer",
    "musaD3D9UnmapVertexBuffer",
    "musaThreadExit",
    "musaSetDoubleForDevice",
    "musaSetDoubleForHost",
    "musaThreadSynchronize",
    "musaThreadGetLimit",
    "musaThreadSetLimit",
    "musaStreamCreate",
    "musaStreamDestroy",
    "musaStreamSynchronize",
    "musaStreamQuery",
    "musaEventCreate",
    "musaEventCreateWithFlags",
    "musaEventRecord",
    "musaEventDestroy",
    "musaEventSynchronize",
    "musaEventQuery",
    "musaEventElapsedTime",
    "musaMalloc3D",
    "musaMalloc3DArray",
    "musaMemset3D",
    "musaMemset3DAsync",
    "musaMemcpy3D",
    "musaMemcpy3DAsync",
    "musaThreadSetCacheConfig",
    "musaStreamWaitEvent",
    "musaD3D11GetDirect3DDevice",
    "musaD3D10GetDirect3DDevice",
    "musaThreadGetCacheConfig",
    "musaPointerGetAttributes",
    "musaHostRegister",
    "musaHostUnregister",
    "musaDeviceCanAccessPeer",
    "musaDeviceEnablePeerAccess",
    "musaDeviceDisablePeerAccess",
    "musaPeerRegister",
    "musaPeerUnregister",
    "musaPeerGetDevicePointer",
    "musaMemcpyPeer",
    "musaMemcpyPeerAsync",
    "musaMemcpy3DPeer",
    "musaMemcpy3DPeerAsync",
    "musaDeviceReset",
    "musaDeviceSynchronize",
    "musaDeviceGetLimit",
    "musaDeviceSetLimit",
    "musaDeviceGetCacheConfig",
    "musaDeviceSetCacheConfig",
    "musaProfilerInitialize",
    "musaProfilerStart",
    "musaProfilerStop",
    "musaDeviceGetByPCIBusId",
    "musaDeviceGetPCIBusId",
    "musaGLGetDevices",
    "musaIpcGetEventHandle",
    "musaIpcOpenEventHandle",
    "musaIpcGetMemHandle",
    "musaIpcOpenMemHandle",
    "musaIpcCloseMemHandle",
    "musaArrayGetInfo",
    "musaFuncSetSharedMemConfig",
    "musaDeviceGetSharedMemConfig",
    "musaDeviceSetSharedMemConfig",
    "musaCreateTextureObject",
    "musaDestroyTextureObject",
    "musaGetTextureObjectResourceDesc",
    "musaGetTextureObjectTextureDesc",
    "musaCreateSurfaceObject",
    "musaDestroySurfaceObject",
    "musaGetSurfaceObjectResourceDesc",
    "musaMallocMipmappedArray",
    "musaGetMipmappedArrayLevel",
    "musaFreeMipmappedArray",
    "musaBindTextureToMipmappedArray",
    "musaGraphicsResourceGetMappedMipmappedArray",
    "musaStreamAddCallback",
    "musaStreamCreateWithFlags",
    "musaGetTextureObjectResourceViewDesc",
    "musaDeviceGetAttribute",
    "musaStreamDestroy",
    "musaStreamCreateWithPriority",
    "musaStreamGetPriority",
    "musaStreamGetFlags",
    "musaDeviceGetStreamPriorityRange",
    "musaMallocManaged",
    "musaOccupancyMaxActiveBlocksPerMultiprocessor",
    "musaStreamAttachMemAsync",
    "musaGetErrorName",
    "musaOccupancyMaxActiveBlocksPerMultiprocessor",
    "musaLaunchKernel",
    "musaGetDeviceFlags",
    "musaLaunch_ptsz",
    "musaLaunchKernel_ptsz",
    "musaMemcpy_ptds",
    "musaMemcpy2D_ptds",
    "musaMemcpyToArray_ptds",
    "musaMemcpy2DToArray_ptds",
    "musaMemcpyFromArray_ptds",
    "musaMemcpy2DFromArray_ptds",
    "musaMemcpyArrayToArray_ptds",
    "musaMemcpy2DArrayToArray_ptds",
    "musaMemcpyToSymbol_ptds",
    "musaMemcpyFromSymbol_ptds",
    "musaMemcpyAsync_ptsz",
    "musaMemcpyToArrayAsync_ptsz",
    "musaMemcpyFromArrayAsync_ptsz",
    "musaMemcpy2DAsync_ptsz",
    "musaMemcpy2DToArrayAsync_ptsz",
    "musaMemcpy2DFromArrayAsync_ptsz",
    "musaMemcpyToSymbolAsync_ptsz",
    "musaMemcpyFromSymbolAsync_ptsz",
    "musaMemset_ptds",
    "musaMemset2D_ptds",
    "musaMemsetAsync_ptsz",
    "musaMemset2DAsync_ptsz",
    "musaStreamGetPriority_ptsz",
    "musaStreamGetFlags_ptsz",
    "musaStreamSynchronize_ptsz",
    "musaStreamQuery_ptsz",
    "musaStreamAttachMemAsync_ptsz",
    "musaEventRecord_ptsz",
    "musaMemset3D_ptds",
    "musaMemset3DAsync_ptsz",
    "musaMemcpy3D_ptds",
    "musaMemcpy3DAsync_ptsz",
    "musaStreamWaitEvent_ptsz",
    "musaStreamAddCallback_ptsz",
    "musaMemcpy3DPeer_ptds",
    "musaMemcpy3DPeerAsync_ptsz",
    "musaOccupancyMaxActiveBlocksPerMultiprocessorWithFlags",
    "musaMemPrefetchAsync",
    "musaMemPrefetchAsync_ptsz",
    "musaMemAdvise",
    "musaDeviceGetP2PAttribute",
    "musaGraphicsEGLRegisterImage",
    "musaEGLStreamConsumerConnect",
    "musaEGLStreamConsumerDisconnect",
    "musaEGLStreamConsumerAcquireFrame",
    "musaEGLStreamConsumerReleaseFrame",
    "musaEGLStreamProducerConnect",
    "musaEGLStreamProducerDisconnect",
    "musaEGLStreamProducerPresentFrame",
    "musaEGLStreamProducerReturnFrame",
    "musaGraphicsResourceGetMappedEglFrame",
    "musaMemRangeGetAttribute",
    "musaMemRangeGetAttributes",
    "musaEGLStreamConsumerConnectWithFlags",
    "musaLaunchCooperativeKernel",
    "musaLaunchCooperativeKernel_ptsz",
    "musaEventCreateFromEGLSync",
    "musaLaunchCooperativeKernelMultiDevice",
    "musaFuncSetAttribute",
    "musaImportExternalMemory",
    "musaExternalMemoryGetMappedBuffer",
    "musaExternalMemoryGetMappedMipmappedArray",
    "musaDestroyExternalMemory",
    "musaImportExternalSemaphore",
    "musaSignalExternalSemaphoresAsync",
    "musaSignalExternalSemaphoresAsync_ptsz",
    "musaWaitExternalSemaphoresAsync",
    "musaWaitExternalSemaphoresAsync_ptsz",
    "musaDestroyExternalSemaphore",
    "musaLaunchHostFunc",
    "musaLaunchHostFunc_ptsz",
    "musaGraphCreate",
    "musaGraphKernelNodeGetParams",
    "musaGraphKernelNodeSetParams",
    "musaGraphAddKernelNode",
    "musaGraphAddMemcpyNode",
    "musaGraphMemcpyNodeGetParams",
    "musaGraphMemcpyNodeSetParams",
    "musaGraphAddMemsetNode",
    "musaGraphMemsetNodeGetParams",
    "musaGraphMemsetNodeSetParams",
    "musaGraphAddHostNode",
    "musaGraphHostNodeGetParams",
    "musaGraphAddChildGraphNode",
    "musaGraphChildGraphNodeGetGraph",
    "musaGraphAddEmptyNode",
    "musaGraphClone",
    "musaGraphNodeFindInClone",
    "musaGraphNodeGetType",
    "musaGraphGetRootNodes",
    "musaGraphNodeGetDependencies",
    "musaGraphNodeGetDependentNodes",
    "musaGraphAddDependencies",
    "musaGraphRemoveDependencies",
    "musaGraphDestroyNode",
    "musaGraphInstantiate",
    "musaGraphLaunch",
    "musaGraphLaunch_ptsz",
    "musaGraphExecDestroy",
    "musaGraphDestroy",
    "musaStreamBeginCapture",
    "musaStreamBeginCapture_ptsz",
    "musaStreamIsCapturing",
    "musaStreamIsCapturing_ptsz",
    "musaStreamEndCapture",
    "musaStreamEndCapture_ptsz",
    "musaGraphHostNodeSetParams",
    "musaGraphGetNodes",
    "musaGraphGetEdges",
    "musaStreamGetCaptureInfo",
    "musaStreamGetCaptureInfo_ptsz",
    "musaGraphExecKernelNodeSetParams",
    "musaThreadExchangeStreamCaptureMode",
    "musaDeviceGetNvSciSyncAttributes",
    "musaOccupancyAvailableDynamicSMemPerBlock",
    "musaStreamSetFlags",
    "musaStreamSetFlags_ptsz",
    "musaGraphExecMemcpyNodeSetParams",
    "musaGraphExecMemsetNodeSetParams",
    "musaGraphExecHostNodeSetParams",
    "musaGraphExecUpdate",
    "musaGetFuncBySymbol",
    "musaCtxResetPersistingL2Cache",
    "musaGraphKernelNodeCopyAttributes",
    "musaGraphKernelNodeGetAttribute",
    "musaGraphKernelNodeSetAttribute",
    "musaStreamCopyAttributes",
    "musaStreamCopyAttributes_ptsz",
    "musaStreamGetAttribute",
    "musaStreamGetAttribute_ptsz",
    "musaStreamSetAttribute",
    "musaStreamSetAttribute_ptsz",
    "musaDeviceGetTexture1DLinearMaxWidth",
    "musaGraphUpload",
    "musaGraphUpload_ptsz",
    "musaGraphAddMemcpyNodeToSymbol",
    "musaGraphAddMemcpyNodeFromSymbol",
    "musaGraphAddMemcpyNode1D",
    "musaGraphMemcpyNodeSetParamsToSymbol",
    "musaGraphMemcpyNodeSetParamsFromSymbol",
    "musaGraphMemcpyNodeSetParams1D",
    "musaGraphExecMemcpyNodeSetParamsToSymbol",
    "musaGraphExecMemcpyNodeSetParamsFromSymbol",
    "musaGraphExecMemcpyNodeSetParams1D",
    "musaArrayGetSparseProperties",
    "musaMipmappedArrayGetSparseProperties",
    "musaGraphExecChildGraphNodeSetParams",
    "musaGraphAddEventRecordNode",
    "musaGraphEventRecordNodeGetEvent",
    "musaGraphEventRecordNodeSetEvent",
    "musaGraphAddEventWaitNode",
    "musaGraphEventWaitNodeGetEvent",
    "musaGraphEventWaitNodeSetEvent",
    "musaGraphExecEventRecordNodeSetEvent",
    "musaGraphExecEventWaitNodeSetEvent",
    "musaEventRecordWithFlags",
    "musaEventRecordWithFlags_ptsz",
    "musaDeviceGetDefaultMemPool",
    "musaMallocAsync",
    "musaMallocAsync_ptsz",
    "musaFreeAsync",
    "musaFreeAsync_ptsz",
    "musaMemPoolTrimTo",
    "musaMemPoolSetAttribute",
    "musaMemPoolGetAttribute",
    "musaMemPoolSetAccess",
    "musaArrayGetPlane",
    "musaMemPoolGetAccess",
    "musaMemPoolCreate",
    "musaMemPoolDestroy",
    "musaDeviceSetMemPool",
    "musaDeviceGetMemPool",
    "musaMemPoolExportToShareableHandle",
    "musaMemPoolImportFromShareableHandle",
    "musaMemPoolExportPointer",
    "musaMemPoolImportPointer",
    "musaMallocFromPoolAsync",
    "musaMallocFromPoolAsync_ptsz",
    "musaSignalExternalSemaphoresAsync",
    "musaSignalExternalSemaphoresAsync",
    "musaWaitExternalSemaphoresAsync",
    "musaWaitExternalSemaphoresAsync",
    "musaGraphAddExternalSemaphoresSignalNode",
    "musaGraphExternalSemaphoresSignalNodeGetParams",
    "musaGraphExternalSemaphoresSignalNodeSetParams",
    "musaGraphAddExternalSemaphoresWaitNode",
    "musaGraphExternalSemaphoresWaitNodeGetParams",
    "musaGraphExternalSemaphoresWaitNodeSetParams",
    "musaGraphExecExternalSemaphoresSignalNodeSetParams",
    "musaGraphExecExternalSemaphoresWaitNodeSetParams",
    "musaDeviceFlushGPUDirectRDMAWrites",
    "musaGetDriverEntryPoint",
    "musaGetDriverEntryPoint_ptsz",
    "musaGraphDebugDotPrint",
    "musaStreamGetCaptureInfo_v2",
    "musaStreamGetCaptureInfo_v2_ptsz",
    "musaStreamUpdateCaptureDependencies",
    "musaStreamUpdateCaptureDependencies_ptsz",
    "musaUserObjectCreate",
    "musaUserObjectRetain",
    "musaUserObjectRelease",
    "musaGraphRetainUserObject",
    "musaGraphReleaseUserObject",
    "musaGraphInstantiateWithFlags",
    "musaGraphAddMemAllocNode",
    "musaGraphMemAllocNodeGetParams",
    "musaGraphAddMemFreeNode",
    "musaGraphMemFreeNodeGetParams",
    "musaDeviceGraphMemTrim",
    "musaDeviceGetGraphMemAttribute",
    "musaDeviceSetGraphMemAttribute",
    "musaGraphNodeSetEnabled",
    "musaGraphNodeGetEnabled",
    "musaArrayGetMemoryRequirements",
    "musaMipmappedArrayGetMemoryRequirements",
    "musaLaunchKernelExC",
    "musaLaunchKernelExC_ptsz",
    "musaOccupancyMaxPotentialClusterSize",
    "musaOccupancyMaxActiveClusters",
    "musaCreateTextureObject_v2",
    "musaGetTextureObjectTextureDesc_v2",
    "musaGraphInstantiateWithParams",
    "musaGraphInstantiateWithParams_ptsz",
    "musaGraphExecGetFlags",
    "musa439",
    "musaGetDeviceProperties_v2",
    "musaStreamGetId",
    "musaStreamGetId_ptsz",
    "musaGraphInstantiate",
    "musa444",
    "SIZE"
};

const char* runtimeCbidName(MUpti_CallbackId cbid) {
  constexpr int names_size =
      sizeof(runtimeCbidNames) / sizeof(runtimeCbidNames[0]);
  if (cbid < 0 || cbid >= names_size) {
    return runtimeCbidNames[MUPTI_RUNTIME_TRACE_CBID_INVALID];
  }
  return runtimeCbidNames[cbid];
}

// From https://docs.nvidia.com/mupti/modules.html#group__MUPTI__ACTIVITY__API_1g80e1eb47615e31021f574df8ebbe5d9a
//   enum MUpti_ActivitySynchronizationType
const char* syncTypeString(
    MUpti_ActivitySynchronizationType kind) {
  switch (kind) {
    case MUPTI_ACTIVITY_SYNCHRONIZATION_TYPE_EVENT_SYNCHRONIZE:
      return "Event Sync";
    case MUPTI_ACTIVITY_SYNCHRONIZATION_TYPE_STREAM_WAIT_EVENT:
      return "Stream Wait Event";
    case MUPTI_ACTIVITY_SYNCHRONIZATION_TYPE_STREAM_SYNCHRONIZE:
      return "Stream Sync";
    case MUPTI_ACTIVITY_SYNCHRONIZATION_TYPE_CONTEXT_SYNCHRONIZE:
      return "Context Sync";
    case MUPTI_ACTIVITY_SYNCHRONIZATION_TYPE_UNKNOWN:
    default:
      return "Unknown Sync";
  }
  return "<unknown>";
}
} // namespace libkineto
