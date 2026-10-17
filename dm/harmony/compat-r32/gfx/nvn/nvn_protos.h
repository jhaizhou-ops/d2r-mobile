// 由 sw/gfx/tools/gen_protos.py 生成，勿手改。NVNboolean 参数已换成 uint32_t。
#pragma once
#include "nvn.h"

typedef void (*NVNT_nvnDeviceBuilderSetDefaults)(NVNdeviceBuilder*);
typedef void (*NVNT_nvnDeviceBuilderSetFlags)(NVNdeviceBuilder*, int);
typedef NVNboolean (*NVNT_nvnDeviceInitialize)(NVNdevice*, const NVNdeviceBuilder*);
typedef void (*NVNT_nvnDeviceFinalize)(NVNdevice*);
typedef void (*NVNT_nvnDeviceSetDebugLabel)(NVNdevice*, const char*);
typedef PFNNVNGENERICFUNCPTRPROC (*NVNT_nvnDeviceGetProcAddress)(const NVNdevice*, const char*);
typedef void (*NVNT_nvnDeviceGetInteger)(const NVNdevice*, NVNdeviceInfo, int*);
typedef uint64_t (*NVNT_nvnDeviceGetCurrentTimestampInNanoseconds)(const NVNdevice*);
typedef void (*NVNT_nvnDeviceSetIntermediateShaderCache)(NVNdevice*, int);
typedef NVNtextureHandle (*NVNT_nvnDeviceGetTextureHandle)(const NVNdevice*, int, int);
typedef NVNtextureHandle (*NVNT_nvnDeviceGetTexelFetchHandle)(const NVNdevice*, int);
typedef NVNimageHandle (*NVNT_nvnDeviceGetImageHandle)(const NVNdevice*, int);
typedef void (*NVNT_nvnDeviceInstallDebugCallback)(NVNdevice*, const PFNNVNDEBUGCALLBACKPROC, void*, uint32_t);
typedef NVNdebugDomainId (*NVNT_nvnDeviceGenerateDebugDomainId)(const NVNdevice*, const char*);
typedef void (*NVNT_nvnDeviceSetWindowOriginMode)(NVNdevice*, NVNwindowOriginMode);
typedef void (*NVNT_nvnDeviceSetDepthMode)(NVNdevice*, NVNdepthMode);
typedef NVNboolean (*NVNT_nvnDeviceRegisterFastClearColor)(NVNdevice*, const float*, NVNformat);
typedef NVNboolean (*NVNT_nvnDeviceRegisterFastClearColori)(NVNdevice*, const int*, NVNformat);
typedef NVNboolean (*NVNT_nvnDeviceRegisterFastClearColorui)(NVNdevice*, const uint32_t*, NVNformat);
typedef NVNboolean (*NVNT_nvnDeviceRegisterFastClearDepth)(NVNdevice*, float);
typedef NVNwindowOriginMode (*NVNT_nvnDeviceGetWindowOriginMode)(const NVNdevice*);
typedef NVNdepthMode (*NVNT_nvnDeviceGetDepthMode)(const NVNdevice*);
typedef uint64_t (*NVNT_nvnDeviceGetTimestampInNanoseconds)(const NVNdevice*, const NVNcounterData*);
typedef void (*NVNT_nvnDeviceApplyDeferredFinalizes)(NVNdevice*, int);
typedef void (*NVNT_nvnDeviceFinalizeCommandHandle)(NVNdevice*, NVNcommandHandle);
typedef void (*NVNT_nvnDeviceWalkDebugDatabase)(const NVNdevice*, NVNdebugObjectType, PFNNVNWALKDEBUGDATABASECALLBACKPROC, void*);
typedef NVNseparateTextureHandle (*NVNT_nvnDeviceGetSeparateTextureHandle)(const NVNdevice*, int);
typedef NVNseparateSamplerHandle (*NVNT_nvnDeviceGetSeparateSamplerHandle)(const NVNdevice*, int);
typedef NVNboolean (*NVNT_nvnDeviceIsExternalDebuggerAttached)(const NVNdevice*);
typedef NVNqueueGetErrorResult (*NVNT_nvnQueueGetError)(NVNqueue*, NVNqueueErrorInfo*);
typedef size_t (*NVNT_nvnQueueGetTotalCommandMemoryUsed)(NVNqueue*);
typedef size_t (*NVNT_nvnQueueGetTotalControlMemoryUsed)(NVNqueue*);
typedef size_t (*NVNT_nvnQueueGetTotalComputeMemoryUsed)(NVNqueue*);
typedef void (*NVNT_nvnQueueResetMemoryUsageCounts)(NVNqueue*);
typedef void (*NVNT_nvnQueueBuilderSetDevice)(NVNqueueBuilder*, NVNdevice*);
typedef void (*NVNT_nvnQueueBuilderSetDefaults)(NVNqueueBuilder*);
typedef void (*NVNT_nvnQueueBuilderSetFlags)(NVNqueueBuilder*, int);
typedef void (*NVNT_nvnQueueBuilderSetCommandMemorySize)(NVNqueueBuilder*, size_t);
typedef void (*NVNT_nvnQueueBuilderSetComputeMemorySize)(NVNqueueBuilder*, size_t);
typedef void (*NVNT_nvnQueueBuilderSetControlMemorySize)(NVNqueueBuilder*, size_t);
typedef size_t (*NVNT_nvnQueueBuilderGetQueueMemorySize)(const NVNqueueBuilder*);
typedef void (*NVNT_nvnQueueBuilderSetQueueMemory)(NVNqueueBuilder*, void*, size_t);
typedef void (*NVNT_nvnQueueBuilderSetCommandFlushThreshold)(NVNqueueBuilder*, size_t);
typedef NVNboolean (*NVNT_nvnQueueInitialize)(NVNqueue*, const NVNqueueBuilder*);
typedef void (*NVNT_nvnQueueFinalize)(NVNqueue*);
typedef void (*NVNT_nvnQueueSetDebugLabel)(NVNqueue*, const char*);
typedef void (*NVNT_nvnQueueSubmitCommands)(NVNqueue*, int, const NVNcommandHandle*);
typedef void (*NVNT_nvnQueueFlush)(NVNqueue*);
typedef void (*NVNT_nvnQueueFinish)(NVNqueue*);
typedef void (*NVNT_nvnQueuePresentTexture)(NVNqueue*, NVNwindow*, int);
typedef NVNqueueAcquireTextureResult (*NVNT_nvnQueueAcquireTexture)(NVNqueue*, NVNwindow*, int*);
typedef void (*NVNT_nvnWindowBuilderSetDevice)(NVNwindowBuilder*, NVNdevice*);
typedef void (*NVNT_nvnWindowBuilderSetDefaults)(NVNwindowBuilder*);
typedef void (*NVNT_nvnWindowBuilderSetNativeWindow)(NVNwindowBuilder*, NVNnativeWindow);
typedef void (*NVNT_nvnWindowBuilderSetTextures)(NVNwindowBuilder*, int, NVNtexture* const*);
typedef void (*NVNT_nvnWindowBuilderSetPresentInterval)(NVNwindowBuilder*, int);
typedef NVNnativeWindow (*NVNT_nvnWindowBuilderGetNativeWindow)(const NVNwindowBuilder*);
typedef int (*NVNT_nvnWindowBuilderGetPresentInterval)(const NVNwindowBuilder*);
typedef NVNboolean (*NVNT_nvnWindowInitialize)(NVNwindow*, const NVNwindowBuilder*);
typedef void (*NVNT_nvnWindowFinalize)(NVNwindow*);
typedef void (*NVNT_nvnWindowSetDebugLabel)(NVNwindow*, const char*);
typedef NVNwindowAcquireTextureResult (*NVNT_nvnWindowAcquireTexture)(NVNwindow*, NVNsync*, int*);
typedef NVNnativeWindow (*NVNT_nvnWindowGetNativeWindow)(const NVNwindow*);
typedef int (*NVNT_nvnWindowGetPresentInterval)(const NVNwindow*);
typedef void (*NVNT_nvnWindowSetPresentInterval)(NVNwindow*, int);
typedef void (*NVNT_nvnWindowSetCrop)(NVNwindow*, int, int, int, int);
typedef void (*NVNT_nvnWindowGetCrop)(const NVNwindow*, NVNrectangle*);
typedef NVNboolean (*NVNT_nvnProgramInitialize)(NVNprogram*, NVNdevice*);
typedef void (*NVNT_nvnProgramFinalize)(NVNprogram*);
typedef void (*NVNT_nvnProgramSetDebugLabel)(NVNprogram*, const char*);
typedef NVNboolean (*NVNT_nvnProgramSetShaders)(NVNprogram*, int, const NVNshaderData*);
typedef void (*NVNT_nvnMemoryPoolBuilderSetDevice)(NVNmemoryPoolBuilder*, NVNdevice*);
typedef void (*NVNT_nvnMemoryPoolBuilderSetDefaults)(NVNmemoryPoolBuilder*);
typedef void (*NVNT_nvnMemoryPoolBuilderSetStorage)(NVNmemoryPoolBuilder*, void*, size_t);
typedef void (*NVNT_nvnMemoryPoolBuilderSetFlags)(NVNmemoryPoolBuilder*, int);
typedef void (*NVNT_nvnMemoryPoolBuilderGetMemory)(const NVNmemoryPoolBuilder*);
typedef size_t (*NVNT_nvnMemoryPoolBuilderGetSize)(const NVNmemoryPoolBuilder*);
typedef NVNmemoryPoolFlags (*NVNT_nvnMemoryPoolBuilderGetFlags)(const NVNmemoryPoolBuilder*);
typedef NVNboolean (*NVNT_nvnMemoryPoolInitialize)(NVNmemoryPool*, const NVNmemoryPoolBuilder*);
typedef void (*NVNT_nvnMemoryPoolSetDebugLabel)(NVNmemoryPool*, const char*);
typedef void (*NVNT_nvnMemoryPoolFinalize)(NVNmemoryPool*);
typedef void* (*NVNT_nvnMemoryPoolMap)(const NVNmemoryPool*);
typedef void (*NVNT_nvnMemoryPoolFlushMappedRange)(const NVNmemoryPool*, ptrdiff_t, size_t);
typedef void (*NVNT_nvnMemoryPoolInvalidateMappedRange)(const NVNmemoryPool*, ptrdiff_t, size_t);
typedef NVNbufferAddress (*NVNT_nvnMemoryPoolGetBufferAddress)(const NVNmemoryPool*);
typedef NVNboolean (*NVNT_nvnMemoryPoolMapVirtual)(NVNmemoryPool*, int, const NVNmappingRequest*);
typedef size_t (*NVNT_nvnMemoryPoolGetSize)(const NVNmemoryPool*);
typedef NVNmemoryPoolFlags (*NVNT_nvnMemoryPoolGetFlags)(const NVNmemoryPool*);
typedef NVNboolean (*NVNT_nvnTexturePoolInitialize)(NVNtexturePool*, const NVNmemoryPool*, ptrdiff_t, int);
typedef void (*NVNT_nvnTexturePoolSetDebugLabel)(NVNtexturePool*, const char*);
typedef void (*NVNT_nvnTexturePoolFinalize)(NVNtexturePool*);
typedef void (*NVNT_nvnTexturePoolRegisterTexture)(const NVNtexturePool*, int, const NVNtexture*, const NVNtextureView*);
typedef void (*NVNT_nvnTexturePoolRegisterImage)(const NVNtexturePool*, int, const NVNtexture*, const NVNtextureView*);
typedef const NVNmemoryPool* (*NVNT_nvnTexturePoolGetMemoryPool)(const NVNtexturePool*);
typedef ptrdiff_t (*NVNT_nvnTexturePoolGetMemoryOffset)(const NVNtexturePool*);
typedef int (*NVNT_nvnTexturePoolGetSize)(const NVNtexturePool*);
typedef NVNboolean (*NVNT_nvnSamplerPoolInitialize)(NVNsamplerPool*, const NVNmemoryPool*, ptrdiff_t, int);
typedef void (*NVNT_nvnSamplerPoolSetDebugLabel)(NVNsamplerPool*, const char*);
typedef void (*NVNT_nvnSamplerPoolFinalize)(NVNsamplerPool*);
typedef void (*NVNT_nvnSamplerPoolRegisterSampler)(const NVNsamplerPool*, int, const NVNsampler*);
typedef void (*NVNT_nvnSamplerPoolRegisterSamplerBuilder)(const NVNsamplerPool*, int, const NVNsamplerBuilder*);
typedef const NVNmemoryPool* (*NVNT_nvnSamplerPoolGetMemoryPool)(const NVNsamplerPool*);
typedef ptrdiff_t (*NVNT_nvnSamplerPoolGetMemoryOffset)(const NVNsamplerPool*);
typedef int (*NVNT_nvnSamplerPoolGetSize)(const NVNsamplerPool*);
typedef void (*NVNT_nvnBufferBuilderSetDevice)(NVNbufferBuilder*, NVNdevice*);
typedef void (*NVNT_nvnBufferBuilderSetDefaults)(NVNbufferBuilder*);
typedef void (*NVNT_nvnBufferBuilderSetStorage)(NVNbufferBuilder*, NVNmemoryPool*, ptrdiff_t, size_t);
typedef NVNmemoryPool (*NVNT_nvnBufferBuilderGetMemoryPool)(const NVNbufferBuilder*);
typedef ptrdiff_t (*NVNT_nvnBufferBuilderGetMemoryOffset)(const NVNbufferBuilder*);
typedef size_t (*NVNT_nvnBufferBuilderGetSize)(const NVNbufferBuilder*);
typedef NVNboolean (*NVNT_nvnBufferInitialize)(NVNbuffer*, const NVNbufferBuilder*);
typedef void (*NVNT_nvnBufferSetDebugLabel)(NVNbuffer*, const char*);
typedef void (*NVNT_nvnBufferFinalize)(NVNbuffer*);
typedef void* (*NVNT_nvnBufferMap)(const NVNbuffer*);
typedef NVNbufferAddress (*NVNT_nvnBufferGetAddress)(const NVNbuffer*);
typedef void (*NVNT_nvnBufferFlushMappedRange)(const NVNbuffer*, ptrdiff_t, size_t);
typedef void (*NVNT_nvnBufferInvalidateMappedRange)(const NVNbuffer*, ptrdiff_t, size_t);
typedef NVNmemoryPool* (*NVNT_nvnBufferGetMemoryPool)(const NVNbuffer*);
typedef ptrdiff_t (*NVNT_nvnBufferGetMemoryOffset)(const NVNbuffer*);
typedef size_t (*NVNT_nvnBufferGetSize)(const NVNbuffer*);
typedef uint64_t (*NVNT_nvnBufferGetDebugID)(const NVNbuffer*);
typedef void (*NVNT_nvnTextureBuilderSetDevice)(NVNtextureBuilder*, NVNdevice*);
typedef void (*NVNT_nvnTextureBuilderSetDefaults)(NVNtextureBuilder*);
typedef void (*NVNT_nvnTextureBuilderSetFlags)(NVNtextureBuilder*, int);
typedef void (*NVNT_nvnTextureBuilderSetTarget)(NVNtextureBuilder*, NVNtextureTarget);
typedef void (*NVNT_nvnTextureBuilderSetWidth)(NVNtextureBuilder*, int);
typedef void (*NVNT_nvnTextureBuilderSetHeight)(NVNtextureBuilder*, int);
typedef void (*NVNT_nvnTextureBuilderSetDepth)(NVNtextureBuilder*, int);
typedef void (*NVNT_nvnTextureBuilderSetSize1D)(NVNtextureBuilder*, int);
typedef void (*NVNT_nvnTextureBuilderSetSize2D)(NVNtextureBuilder*, int, int);
typedef void (*NVNT_nvnTextureBuilderSetSize3D)(NVNtextureBuilder*, int, int, int);
typedef void (*NVNT_nvnTextureBuilderSetLevels)(NVNtextureBuilder*, int);
typedef void (*NVNT_nvnTextureBuilderSetFormat)(NVNtextureBuilder*, NVNformat);
typedef void (*NVNT_nvnTextureBuilderSetSamples)(NVNtextureBuilder*, int);
typedef void (*NVNT_nvnTextureBuilderSetSwizzle)(NVNtextureBuilder*, NVNtextureSwizzle, NVNtextureSwizzle, NVNtextureSwizzle, NVNtextureSwizzle);
typedef void (*NVNT_nvnTextureBuilderSetDepthStencilMode)(NVNtextureBuilder*, NVNtextureDepthStencilMode);
typedef size_t (*NVNT_nvnTextureBuilderGetStorageSize)(const NVNtextureBuilder*);
typedef size_t (*NVNT_nvnTextureBuilderGetStorageAlignment)(const NVNtextureBuilder*);
typedef void (*NVNT_nvnTextureBuilderSetStorage)(NVNtextureBuilder*, NVNmemoryPool*, ptrdiff_t);
typedef void (*NVNT_nvnTextureBuilderSetPackagedTextureData)(NVNtextureBuilder*, const void*);
typedef void (*NVNT_nvnTextureBuilderSetPackagedTextureLayout)(NVNtextureBuilder*, const NVNpackagedTextureLayout*);
typedef void (*NVNT_nvnTextureBuilderSetStride)(NVNtextureBuilder*, ptrdiff_t);
typedef void (*NVNT_nvnTextureBuilderSetGLTextureName)(NVNtextureBuilder*, uint32_t);
typedef NVNstorageClass (*NVNT_nvnTextureBuilderGetStorageClass)(const NVNtextureBuilder*);
typedef NVNtextureFlags (*NVNT_nvnTextureBuilderGetFlags)(const NVNtextureBuilder*);
typedef NVNtextureTarget (*NVNT_nvnTextureBuilderGetTarget)(const NVNtextureBuilder*);
typedef int (*NVNT_nvnTextureBuilderGetWidth)(const NVNtextureBuilder*);
typedef int (*NVNT_nvnTextureBuilderGetHeight)(const NVNtextureBuilder*);
typedef int (*NVNT_nvnTextureBuilderGetDepth)(const NVNtextureBuilder*);
typedef int (*NVNT_nvnTextureBuilderGetLevels)(const NVNtextureBuilder*);
typedef NVNformat (*NVNT_nvnTextureBuilderGetFormat)(const NVNtextureBuilder*);
typedef int (*NVNT_nvnTextureBuilderGetSamples)(const NVNtextureBuilder*);
typedef void (*NVNT_nvnTextureBuilderGetSwizzle)(const NVNtextureBuilder*, NVNtextureSwizzle*, NVNtextureSwizzle*, NVNtextureSwizzle*, NVNtextureSwizzle*);
typedef NVNtextureDepthStencilMode (*NVNT_nvnTextureBuilderGetDepthStencilMode)(const NVNtextureBuilder*);
typedef const void* (*NVNT_nvnTextureBuilderGetPackagedTextureData)(const NVNtextureBuilder*);
typedef ptrdiff_t (*NVNT_nvnTextureBuilderGetStride)(const NVNtextureBuilder*);
typedef void (*NVNT_nvnTextureBuilderGetSparseTileLayout)(const NVNtextureBuilder*, NVNtextureSparseTileLayout*);
typedef uint32_t (*NVNT_nvnTextureBuilderGetGLTextureName)(const NVNtextureBuilder*);
typedef size_t (*NVNT_nvnTextureBuilderGetZCullStorageSize)(const NVNtextureBuilder*);
typedef NVNmemoryPool (*NVNT_nvnTextureBuilderGetMemoryPool)(const NVNtextureBuilder*);
typedef ptrdiff_t (*NVNT_nvnTextureBuilderGetMemoryOffset)(const NVNtextureBuilder*);
typedef void (*NVNT_nvnTextureViewSetDefaults)(NVNtextureView*);
typedef void (*NVNT_nvnTextureViewSetLevels)(NVNtextureView*, int, int);
typedef void (*NVNT_nvnTextureViewSetLayers)(NVNtextureView*, int, int);
typedef void (*NVNT_nvnTextureViewSetFormat)(NVNtextureView*, NVNformat);
typedef void (*NVNT_nvnTextureViewSetSwizzle)(NVNtextureView*, NVNtextureSwizzle, NVNtextureSwizzle, NVNtextureSwizzle, NVNtextureSwizzle);
typedef void (*NVNT_nvnTextureViewSetDepthStencilMode)(NVNtextureView*, NVNtextureDepthStencilMode);
typedef void (*NVNT_nvnTextureViewSetTarget)(NVNtextureView*, NVNtextureTarget);
typedef NVNboolean (*NVNT_nvnTextureViewGetLevels)(const NVNtextureView*, int*, int*);
typedef NVNboolean (*NVNT_nvnTextureViewGetLayers)(const NVNtextureView*, int*, int*);
typedef NVNboolean (*NVNT_nvnTextureViewGetFormat)(const NVNtextureView*, NVNformat*);
typedef NVNboolean (*NVNT_nvnTextureViewGetSwizzle)(const NVNtextureView*, NVNtextureSwizzle*, NVNtextureSwizzle*, NVNtextureSwizzle*, NVNtextureSwizzle*);
typedef NVNboolean (*NVNT_nvnTextureViewGetDepthStencilMode)(const NVNtextureView*, NVNtextureDepthStencilMode*);
typedef NVNboolean (*NVNT_nvnTextureViewGetTarget)(const NVNtextureView*, NVNtextureTarget*);
typedef NVNboolean (*NVNT_nvnTextureViewCompare)(const NVNtextureView*, const NVNtextureView*);
typedef NVNboolean (*NVNT_nvnTextureInitialize)(NVNtexture*, const NVNtextureBuilder*);
typedef size_t (*NVNT_nvnTextureGetZCullStorageSize)(const NVNtexture*);
typedef void (*NVNT_nvnTextureFinalize)(NVNtexture*);
typedef void (*NVNT_nvnTextureSetDebugLabel)(NVNtexture*, const char*);
typedef NVNstorageClass (*NVNT_nvnTextureGetStorageClass)(const NVNtexture*);
typedef ptrdiff_t (*NVNT_nvnTextureGetViewOffset)(const NVNtexture*, const NVNtextureView*);
typedef NVNtextureFlags (*NVNT_nvnTextureGetFlags)(const NVNtexture*);
typedef NVNtextureTarget (*NVNT_nvnTextureGetTarget)(const NVNtexture*);
typedef int (*NVNT_nvnTextureGetWidth)(const NVNtexture*);
typedef int (*NVNT_nvnTextureGetHeight)(const NVNtexture*);
typedef int (*NVNT_nvnTextureGetDepth)(const NVNtexture*);
typedef int (*NVNT_nvnTextureGetLevels)(const NVNtexture*);
typedef NVNformat (*NVNT_nvnTextureGetFormat)(const NVNtexture*);
typedef int (*NVNT_nvnTextureGetSamples)(const NVNtexture*);
typedef void (*NVNT_nvnTextureGetSwizzle)(const NVNtexture*, NVNtextureSwizzle*, NVNtextureSwizzle*, NVNtextureSwizzle*, NVNtextureSwizzle*);
typedef NVNtextureDepthStencilMode (*NVNT_nvnTextureGetDepthStencilMode)(const NVNtexture*);
typedef ptrdiff_t (*NVNT_nvnTextureGetStride)(const NVNtexture*);
typedef NVNtextureAddress (*NVNT_nvnTextureGetTextureAddress)(const NVNtexture*);
typedef void (*NVNT_nvnTextureGetSparseTileLayout)(const NVNtexture*, NVNtextureSparseTileLayout*);
typedef void (*NVNT_nvnTextureWriteTexels)(const NVNtexture*, const NVNtextureView*, const NVNcopyRegion*, const void*);
typedef void (*NVNT_nvnTextureWriteTexelsStrided)(const NVNtexture*, const NVNtextureView*, const NVNcopyRegion*, const void*, ptrdiff_t, ptrdiff_t);
typedef void (*NVNT_nvnTextureReadTexels)(const NVNtexture*, const NVNtextureView*, const NVNcopyRegion*, void*);
typedef void (*NVNT_nvnTextureReadTexelsStrided)(const NVNtexture*, const NVNtextureView*, const NVNcopyRegion*, void*, ptrdiff_t, ptrdiff_t);
typedef void (*NVNT_nvnTextureFlushTexels)(const NVNtexture*, const NVNtextureView*, const NVNcopyRegion*);
typedef void (*NVNT_nvnTextureInvalidateTexels)(const NVNtexture*, const NVNtextureView*, const NVNcopyRegion*);
typedef NVNmemoryPool (*NVNT_nvnTextureGetMemoryPool)(const NVNtexture*);
typedef ptrdiff_t (*NVNT_nvnTextureGetMemoryOffset)(const NVNtexture*);
typedef int (*NVNT_nvnTextureGetStorageSize)(const NVNtexture*);
typedef NVNboolean (*NVNT_nvnTextureCompare)(const NVNtexture*, const NVNtexture*);
typedef uint64_t (*NVNT_nvnTextureGetDebugID)(const NVNtexture*);
typedef void (*NVNT_nvnSamplerBuilderSetDevice)(NVNsamplerBuilder*, NVNdevice*);
typedef void (*NVNT_nvnSamplerBuilderSetDefaults)(NVNsamplerBuilder*);
typedef void (*NVNT_nvnSamplerBuilderSetMinMagFilter)(NVNsamplerBuilder*, NVNminFilter, NVNmagFilter);
typedef void (*NVNT_nvnSamplerBuilderSetWrapMode)(NVNsamplerBuilder*, NVNwrapMode, NVNwrapMode, NVNwrapMode);
typedef void (*NVNT_nvnSamplerBuilderSetLodClamp)(NVNsamplerBuilder*, float, float);
typedef void (*NVNT_nvnSamplerBuilderSetLodBias)(NVNsamplerBuilder*, float);
typedef void (*NVNT_nvnSamplerBuilderSetCompare)(NVNsamplerBuilder*, NVNcompareMode, NVNcompareFunc);
typedef void (*NVNT_nvnSamplerBuilderSetBorderColor)(NVNsamplerBuilder*, const float*);
typedef void (*NVNT_nvnSamplerBuilderSetBorderColori)(NVNsamplerBuilder*, const int*);
typedef void (*NVNT_nvnSamplerBuilderSetBorderColorui)(NVNsamplerBuilder*, const uint32_t*);
typedef void (*NVNT_nvnSamplerBuilderSetMaxAnisotropy)(NVNsamplerBuilder*, float);
typedef void (*NVNT_nvnSamplerBuilderSetReductionFilter)(NVNsamplerBuilder*, NVNsamplerReduction);
typedef void (*NVNT_nvnSamplerBuilderSetLodSnap)(NVNsamplerBuilder*, float);
typedef void (*NVNT_nvnSamplerBuilderGetMinMagFilter)(const NVNsamplerBuilder*, NVNminFilter*, NVNmagFilter*);
typedef void (*NVNT_nvnSamplerBuilderGetWrapMode)(const NVNsamplerBuilder*, NVNwrapMode*, NVNwrapMode*, NVNwrapMode*);
typedef void (*NVNT_nvnSamplerBuilderGetLodClamp)(const NVNsamplerBuilder*, float*, float*);
typedef float (*NVNT_nvnSamplerBuilderGetLodBias)(const NVNsamplerBuilder*);
typedef void (*NVNT_nvnSamplerBuilderGetCompare)(const NVNsamplerBuilder*, NVNcompareMode*, NVNcompareFunc*);
typedef void (*NVNT_nvnSamplerBuilderGetBorderColor)(const NVNsamplerBuilder*, float*);
typedef void (*NVNT_nvnSamplerBuilderGetBorderColori)(const NVNsamplerBuilder*, int*);
typedef void (*NVNT_nvnSamplerBuilderGetBorderColorui)(const NVNsamplerBuilder*, uint32_t*);
typedef float (*NVNT_nvnSamplerBuilderGetMaxAnisotropy)(const NVNsamplerBuilder*);
typedef NVNsamplerReduction (*NVNT_nvnSamplerBuilderGetReductionFilter)(const NVNsamplerBuilder*);
typedef float (*NVNT_nvnSamplerBuilderGetLodSnap)(const NVNsamplerBuilder*);
typedef NVNboolean (*NVNT_nvnSamplerInitialize)(NVNsampler*, const NVNsamplerBuilder*);
typedef void (*NVNT_nvnSamplerFinalize)(NVNsampler*);
typedef void (*NVNT_nvnSamplerSetDebugLabel)(NVNsampler*, const char*);
typedef void (*NVNT_nvnSamplerGetMinMagFilter)(const NVNsampler*, NVNminFilter*, NVNmagFilter*);
typedef void (*NVNT_nvnSamplerGetWrapMode)(const NVNsampler*, NVNwrapMode*, NVNwrapMode*, NVNwrapMode*);
typedef void (*NVNT_nvnSamplerGetLodClamp)(const NVNsampler*, float*, float*);
typedef float (*NVNT_nvnSamplerGetLodBias)(const NVNsampler*);
typedef void (*NVNT_nvnSamplerGetCompare)(const NVNsampler*, NVNcompareMode*, NVNcompareFunc*);
typedef void (*NVNT_nvnSamplerGetBorderColor)(const NVNsampler*, float*);
typedef void (*NVNT_nvnSamplerGetBorderColori)(const NVNsampler*, int*);
typedef void (*NVNT_nvnSamplerGetBorderColorui)(const NVNsampler*, uint32_t*);
typedef float (*NVNT_nvnSamplerGetMaxAnisotropy)(const NVNsampler*);
typedef NVNsamplerReduction (*NVNT_nvnSamplerGetReductionFilter)(const NVNsampler*);
typedef NVNboolean (*NVNT_nvnSamplerCompare)(const NVNsampler*, const NVNsampler*);
typedef uint64_t (*NVNT_nvnSamplerGetDebugID)(const NVNsampler*);
typedef void (*NVNT_nvnBlendStateSetDefaults)(NVNblendState*);
typedef void (*NVNT_nvnBlendStateSetBlendTarget)(NVNblendState*, int);
typedef void (*NVNT_nvnBlendStateSetBlendFunc)(NVNblendState*, NVNblendFunc, NVNblendFunc, NVNblendFunc, NVNblendFunc);
typedef void (*NVNT_nvnBlendStateSetBlendEquation)(NVNblendState*, NVNblendEquation, NVNblendEquation);
typedef void (*NVNT_nvnBlendStateSetAdvancedMode)(NVNblendState*, NVNblendAdvancedMode);
typedef void (*NVNT_nvnBlendStateSetAdvancedOverlap)(NVNblendState*, NVNblendAdvancedOverlap);
typedef void (*NVNT_nvnBlendStateSetAdvancedPremultipliedSrc)(NVNblendState*, uint32_t);
typedef void (*NVNT_nvnBlendStateSetAdvancedNormalizedDst)(NVNblendState*, uint32_t);
typedef int (*NVNT_nvnBlendStateGetBlendTarget)(const NVNblendState*);
typedef void (*NVNT_nvnBlendStateGetBlendFunc)(const NVNblendState*, NVNblendFunc*, NVNblendFunc*, NVNblendFunc*, NVNblendFunc*);
typedef void (*NVNT_nvnBlendStateGetBlendEquation)(const NVNblendState*, NVNblendEquation*, NVNblendEquation*);
typedef NVNblendAdvancedMode (*NVNT_nvnBlendStateGetAdvancedMode)(const NVNblendState*);
typedef NVNblendAdvancedOverlap (*NVNT_nvnBlendStateGetAdvancedOverlap)(const NVNblendState*);
typedef NVNboolean (*NVNT_nvnBlendStateGetAdvancedPremultipliedSrc)(const NVNblendState*);
typedef NVNboolean (*NVNT_nvnBlendStateGetAdvancedNormalizedDst)(const NVNblendState*);
typedef void (*NVNT_nvnColorStateSetDefaults)(NVNcolorState*);
typedef void (*NVNT_nvnColorStateSetBlendEnable)(NVNcolorState*, int, uint32_t);
typedef void (*NVNT_nvnColorStateSetLogicOp)(NVNcolorState*, NVNlogicOp);
typedef void (*NVNT_nvnColorStateSetAlphaTest)(NVNcolorState*, NVNalphaFunc);
typedef NVNboolean (*NVNT_nvnColorStateGetBlendEnable)(const NVNcolorState*, int);
typedef NVNlogicOp (*NVNT_nvnColorStateGetLogicOp)(const NVNcolorState*);
typedef NVNalphaFunc (*NVNT_nvnColorStateGetAlphaTest)(const NVNcolorState*);
typedef void (*NVNT_nvnChannelMaskStateSetDefaults)(NVNchannelMaskState*);
typedef void (*NVNT_nvnChannelMaskStateSetChannelMask)(NVNchannelMaskState*, int, uint32_t, uint32_t, uint32_t, uint32_t);
typedef void (*NVNT_nvnChannelMaskStateGetChannelMask)(const NVNchannelMaskState*, int, NVNboolean*, NVNboolean*, NVNboolean*, NVNboolean*);
typedef void (*NVNT_nvnMultisampleStateSetDefaults)(NVNmultisampleState*);
typedef void (*NVNT_nvnMultisampleStateSetMultisampleEnable)(NVNmultisampleState*, uint32_t);
typedef void (*NVNT_nvnMultisampleStateSetSamples)(NVNmultisampleState*, int);
typedef void (*NVNT_nvnMultisampleStateSetAlphaToCoverageEnable)(NVNmultisampleState*, uint32_t);
typedef void (*NVNT_nvnMultisampleStateSetAlphaToCoverageDither)(NVNmultisampleState*, uint32_t);
typedef NVNboolean (*NVNT_nvnMultisampleStateGetMultisampleEnable)(const NVNmultisampleState*);
typedef int (*NVNT_nvnMultisampleStateGetSamples)(const NVNmultisampleState*);
typedef NVNboolean (*NVNT_nvnMultisampleStateGetAlphaToCoverageEnable)(const NVNmultisampleState*);
typedef NVNboolean (*NVNT_nvnMultisampleStateGetAlphaToCoverageDither)(const NVNmultisampleState*);
typedef void (*NVNT_nvnMultisampleStateSetRasterSamples)(NVNmultisampleState*, int);
typedef int (*NVNT_nvnMultisampleStateGetRasterSamples)(NVNmultisampleState*);
typedef void (*NVNT_nvnMultisampleStateSetCoverageModulationMode)(NVNmultisampleState*, NVNcoverageModulationMode);
typedef NVNcoverageModulationMode (*NVNT_nvnMultisampleStateGetCoverageModulationMode)(const NVNmultisampleState*);
typedef void (*NVNT_nvnMultisampleStateSetCoverageToColorEnable)(NVNmultisampleState*, uint32_t);
typedef NVNboolean (*NVNT_nvnMultisampleStateGetCoverageToColorEnable)(const NVNmultisampleState*);
typedef void (*NVNT_nvnMultisampleStateSetCoverageToColorOutput)(NVNmultisampleState*, int);
typedef int (*NVNT_nvnMultisampleStateGetCoverageToColorOutput)(const NVNmultisampleState*);
typedef void (*NVNT_nvnMultisampleStateSetSampleLocationsEnable)(NVNmultisampleState*, uint32_t);
typedef NVNboolean (*NVNT_nvnMultisampleStateGetSampleLocationsEnable)(const NVNmultisampleState*);
typedef void (*NVNT_nvnMultisampleStateGetSampleLocationsGrid)(NVNmultisampleState*, int*, int*);
typedef void (*NVNT_nvnMultisampleStateSetSampleLocationsGridEnable)(NVNmultisampleState*, uint32_t);
typedef NVNboolean (*NVNT_nvnMultisampleStateGetSampleLocationsGridEnable)(const NVNmultisampleState*);
typedef void (*NVNT_nvnMultisampleStateSetSampleLocations)(NVNmultisampleState*, int, int, const float*);
typedef void (*NVNT_nvnPolygonStateSetDefaults)(NVNpolygonState*);
typedef void (*NVNT_nvnPolygonStateSetCullFace)(NVNpolygonState*, NVNface);
typedef void (*NVNT_nvnPolygonStateSetFrontFace)(NVNpolygonState*, NVNfrontFace);
typedef void (*NVNT_nvnPolygonStateSetPolygonMode)(NVNpolygonState*, NVNpolygonMode);
typedef void (*NVNT_nvnPolygonStateSetPolygonOffsetEnables)(NVNpolygonState*, int);
typedef NVNface (*NVNT_nvnPolygonStateGetCullFace)(const NVNpolygonState*);
typedef NVNfrontFace (*NVNT_nvnPolygonStateGetFrontFace)(const NVNpolygonState*);
typedef NVNpolygonMode (*NVNT_nvnPolygonStateGetPolygonMode)(const NVNpolygonState*);
typedef NVNpolygonOffsetEnable (*NVNT_nvnPolygonStateGetPolygonOffsetEnables)(const NVNpolygonState*);
typedef void (*NVNT_nvnDepthStencilStateSetDefaults)(NVNdepthStencilState*);
typedef void (*NVNT_nvnDepthStencilStateSetDepthTestEnable)(NVNdepthStencilState*, uint32_t);
typedef void (*NVNT_nvnDepthStencilStateSetDepthWriteEnable)(NVNdepthStencilState*, uint32_t);
typedef void (*NVNT_nvnDepthStencilStateSetDepthFunc)(NVNdepthStencilState*, NVNdepthFunc);
typedef void (*NVNT_nvnDepthStencilStateSetStencilTestEnable)(NVNdepthStencilState*, uint32_t);
typedef void (*NVNT_nvnDepthStencilStateSetStencilFunc)(NVNdepthStencilState*, NVNface, NVNstencilFunc);
typedef void (*NVNT_nvnDepthStencilStateSetStencilOp)(NVNdepthStencilState*, NVNface, NVNstencilOp, NVNstencilOp, NVNstencilOp);
typedef NVNboolean (*NVNT_nvnDepthStencilStateGetDepthTestEnable)(const NVNdepthStencilState*);
typedef NVNboolean (*NVNT_nvnDepthStencilStateGetDepthWriteEnable)(const NVNdepthStencilState*);
typedef NVNdepthFunc (*NVNT_nvnDepthStencilStateGetDepthFunc)(const NVNdepthStencilState*);
typedef NVNboolean (*NVNT_nvnDepthStencilStateGetStencilTestEnable)(const NVNdepthStencilState*);
typedef NVNstencilFunc (*NVNT_nvnDepthStencilStateGetStencilFunc)(const NVNdepthStencilState*, NVNface);
typedef void (*NVNT_nvnDepthStencilStateGetStencilOp)(const NVNdepthStencilState*, NVNface, NVNstencilOp*, NVNstencilOp*, NVNstencilOp*);
typedef void (*NVNT_nvnVertexAttribStateSetDefaults)(NVNvertexAttribState*);
typedef void (*NVNT_nvnVertexAttribStateSetFormat)(NVNvertexAttribState*, NVNformat, ptrdiff_t);
typedef void (*NVNT_nvnVertexAttribStateSetStreamIndex)(NVNvertexAttribState*, int);
typedef void (*NVNT_nvnVertexAttribStateGetFormat)(const NVNvertexAttribState*, NVNformat*, ptrdiff_t*);
typedef int (*NVNT_nvnVertexAttribStateGetStreamIndex)(const NVNvertexAttribState*);
typedef void (*NVNT_nvnVertexStreamStateSetDefaults)(NVNvertexStreamState*);
typedef void (*NVNT_nvnVertexStreamStateSetStride)(NVNvertexStreamState*, ptrdiff_t);
typedef void (*NVNT_nvnVertexStreamStateSetDivisor)(NVNvertexStreamState*, int);
typedef ptrdiff_t (*NVNT_nvnVertexStreamStateGetStride)(const NVNvertexStreamState*);
typedef int (*NVNT_nvnVertexStreamStateGetDivisor)(const NVNvertexStreamState*);
typedef NVNboolean (*NVNT_nvnCommandBufferInitialize)(NVNcommandBuffer*, NVNdevice*);
typedef void (*NVNT_nvnCommandBufferFinalize)(NVNcommandBuffer*);
typedef void (*NVNT_nvnCommandBufferSetDebugLabel)(NVNcommandBuffer*, const char*);
typedef void (*NVNT_nvnCommandBufferSetMemoryCallback)(NVNcommandBuffer*, PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC);
typedef void (*NVNT_nvnCommandBufferSetMemoryCallbackData)(NVNcommandBuffer*, void*);
typedef void (*NVNT_nvnCommandBufferAddCommandMemory)(NVNcommandBuffer*, const NVNmemoryPool*, ptrdiff_t, size_t);
typedef void (*NVNT_nvnCommandBufferAddControlMemory)(NVNcommandBuffer*, void*, size_t);
typedef size_t (*NVNT_nvnCommandBufferGetCommandMemorySize)(const NVNcommandBuffer*);
typedef size_t (*NVNT_nvnCommandBufferGetCommandMemoryUsed)(const NVNcommandBuffer*);
typedef size_t (*NVNT_nvnCommandBufferGetCommandMemoryFree)(const NVNcommandBuffer*);
typedef size_t (*NVNT_nvnCommandBufferGetControlMemorySize)(const NVNcommandBuffer*);
typedef size_t (*NVNT_nvnCommandBufferGetControlMemoryUsed)(const NVNcommandBuffer*);
typedef size_t (*NVNT_nvnCommandBufferGetControlMemoryFree)(const NVNcommandBuffer*);
typedef void (*NVNT_nvnCommandBufferBeginRecording)(NVNcommandBuffer*);
typedef NVNcommandHandle (*NVNT_nvnCommandBufferEndRecording)(NVNcommandBuffer*);
typedef void (*NVNT_nvnCommandBufferCallCommands)(NVNcommandBuffer*, int, const NVNcommandHandle*);
typedef void (*NVNT_nvnCommandBufferCopyCommands)(NVNcommandBuffer*, int, const NVNcommandHandle*);
typedef void (*NVNT_nvnCommandBufferBindBlendState)(NVNcommandBuffer*, const NVNblendState*);
typedef void (*NVNT_nvnCommandBufferBindChannelMaskState)(NVNcommandBuffer*, const NVNchannelMaskState*);
typedef void (*NVNT_nvnCommandBufferBindColorState)(NVNcommandBuffer*, const NVNcolorState*);
typedef void (*NVNT_nvnCommandBufferBindMultisampleState)(NVNcommandBuffer*, const NVNmultisampleState*);
typedef void (*NVNT_nvnCommandBufferBindPolygonState)(NVNcommandBuffer*, const NVNpolygonState*);
typedef void (*NVNT_nvnCommandBufferBindDepthStencilState)(NVNcommandBuffer*, const NVNdepthStencilState*);
typedef void (*NVNT_nvnCommandBufferBindVertexAttribState)(NVNcommandBuffer*, int, const NVNvertexAttribState*);
typedef void (*NVNT_nvnCommandBufferBindVertexStreamState)(NVNcommandBuffer*, int, const NVNvertexStreamState*);
typedef void (*NVNT_nvnCommandBufferBindProgram)(NVNcommandBuffer*, const NVNprogram*, int);
typedef void (*NVNT_nvnCommandBufferBindVertexBuffer)(NVNcommandBuffer*, int, NVNbufferAddress, size_t);
typedef void (*NVNT_nvnCommandBufferBindVertexBuffers)(NVNcommandBuffer*, int, int, const NVNbufferRange*);
typedef void (*NVNT_nvnCommandBufferBindUniformBuffer)(NVNcommandBuffer*, NVNshaderStage, int, NVNbufferAddress, size_t);
typedef void (*NVNT_nvnCommandBufferBindUniformBuffers)(NVNcommandBuffer*, NVNshaderStage, int, int, const NVNbufferRange*);
typedef void (*NVNT_nvnCommandBufferBindTransformFeedbackBuffer)(NVNcommandBuffer*, int, NVNbufferAddress, size_t);
typedef void (*NVNT_nvnCommandBufferBindTransformFeedbackBuffers)(NVNcommandBuffer*, int, int, const NVNbufferRange*);
typedef void (*NVNT_nvnCommandBufferBindStorageBuffer)(NVNcommandBuffer*, NVNshaderStage, int, NVNbufferAddress, size_t);
typedef void (*NVNT_nvnCommandBufferBindStorageBuffers)(NVNcommandBuffer*, NVNshaderStage, int, int, const NVNbufferRange*);
typedef void (*NVNT_nvnCommandBufferBindTexture)(NVNcommandBuffer*, NVNshaderStage, int, NVNtextureHandle);
typedef void (*NVNT_nvnCommandBufferBindTextures)(NVNcommandBuffer*, NVNshaderStage, int, int, const NVNtextureHandle*);
typedef void (*NVNT_nvnCommandBufferBindImage)(NVNcommandBuffer*, NVNshaderStage, int, NVNimageHandle);
typedef void (*NVNT_nvnCommandBufferBindImages)(NVNcommandBuffer*, NVNshaderStage, int, int, const NVNimageHandle*);
typedef void (*NVNT_nvnCommandBufferSetPatchSize)(NVNcommandBuffer*, int);
typedef void (*NVNT_nvnCommandBufferSetInnerTessellationLevels)(NVNcommandBuffer*, const float*);
typedef void (*NVNT_nvnCommandBufferSetOuterTessellationLevels)(NVNcommandBuffer*, const float*);
typedef void (*NVNT_nvnCommandBufferSetPrimitiveRestart)(NVNcommandBuffer*, uint32_t, int);
typedef void (*NVNT_nvnCommandBufferBeginTransformFeedback)(NVNcommandBuffer*, NVNbufferAddress);
typedef void (*NVNT_nvnCommandBufferEndTransformFeedback)(NVNcommandBuffer*, NVNbufferAddress);
typedef void (*NVNT_nvnCommandBufferPauseTransformFeedback)(NVNcommandBuffer*, NVNbufferAddress);
typedef void (*NVNT_nvnCommandBufferResumeTransformFeedback)(NVNcommandBuffer*, NVNbufferAddress);
typedef void (*NVNT_nvnCommandBufferDrawTransformFeedback)(NVNcommandBuffer*, NVNdrawPrimitive, NVNbufferAddress);
typedef void (*NVNT_nvnCommandBufferDrawArrays)(NVNcommandBuffer*, NVNdrawPrimitive, int, int);
typedef void (*NVNT_nvnCommandBufferDrawElements)(NVNcommandBuffer*, NVNdrawPrimitive, NVNindexType, int, NVNbufferAddress);
typedef void (*NVNT_nvnCommandBufferDrawElementsBaseVertex)(NVNcommandBuffer*, NVNdrawPrimitive, NVNindexType, int, NVNbufferAddress, int);
typedef void (*NVNT_nvnCommandBufferDrawArraysInstanced)(NVNcommandBuffer*, NVNdrawPrimitive, int, int, int, int);
typedef void (*NVNT_nvnCommandBufferDrawElementsInstanced)(NVNcommandBuffer*, NVNdrawPrimitive, NVNindexType, int, NVNbufferAddress, int, int, int);
typedef void (*NVNT_nvnCommandBufferDrawArraysIndirect)(NVNcommandBuffer*, NVNdrawPrimitive, NVNbufferAddress);
typedef void (*NVNT_nvnCommandBufferDrawElementsIndirect)(NVNcommandBuffer*, NVNdrawPrimitive, NVNindexType, NVNbufferAddress, NVNbufferAddress);
typedef void (*NVNT_nvnCommandBufferMultiDrawArraysIndirectCount)(NVNcommandBuffer*, NVNdrawPrimitive, NVNbufferAddress, NVNbufferAddress, int, ptrdiff_t);
typedef void (*NVNT_nvnCommandBufferMultiDrawElementsIndirectCount)(NVNcommandBuffer*, NVNdrawPrimitive, NVNindexType, NVNbufferAddress, NVNbufferAddress, NVNbufferAddress, int, ptrdiff_t);
typedef void (*NVNT_nvnCommandBufferClearColor)(NVNcommandBuffer*, int, const float*, int);
typedef void (*NVNT_nvnCommandBufferClearColori)(NVNcommandBuffer*, int, const int*, int);
typedef void (*NVNT_nvnCommandBufferClearColorui)(NVNcommandBuffer*, int, const uint32_t*, int);
typedef void (*NVNT_nvnCommandBufferClearDepthStencil)(NVNcommandBuffer*, float, uint32_t, int, int);
typedef void (*NVNT_nvnCommandBufferDispatchCompute)(NVNcommandBuffer*, int, int, int);
typedef void (*NVNT_nvnCommandBufferDispatchComputeIndirect)(NVNcommandBuffer*, NVNbufferAddress);
typedef void (*NVNT_nvnCommandBufferSetViewport)(NVNcommandBuffer*, int, int, int, int);
typedef void (*NVNT_nvnCommandBufferSetViewports)(NVNcommandBuffer*, int, int, const float*);
typedef void (*NVNT_nvnCommandBufferSetViewportSwizzles)(NVNcommandBuffer*, int, int, const NVNviewportSwizzle*);
typedef void (*NVNT_nvnCommandBufferSetScissor)(NVNcommandBuffer*, int, int, int, int);
typedef void (*NVNT_nvnCommandBufferSetScissors)(NVNcommandBuffer*, int, int, const int*);
typedef void (*NVNT_nvnCommandBufferSetDepthRange)(NVNcommandBuffer*, float, float);
typedef void (*NVNT_nvnCommandBufferSetDepthBounds)(NVNcommandBuffer*, uint32_t, float, float);
typedef void (*NVNT_nvnCommandBufferSetDepthRanges)(NVNcommandBuffer*, int, int, const float*);
typedef void (*NVNT_nvnCommandBufferSetTiledCacheAction)(NVNcommandBuffer*, NVNtiledCacheAction);
typedef void (*NVNT_nvnCommandBufferSetTiledCacheTileSize)(NVNcommandBuffer*, int, int);
typedef void (*NVNT_nvnCommandBufferBindSeparateTexture)(NVNcommandBuffer*, NVNshaderStage, int, NVNseparateTextureHandle);
typedef void (*NVNT_nvnCommandBufferBindSeparateSampler)(NVNcommandBuffer*, NVNshaderStage, int, NVNseparateSamplerHandle);
typedef void (*NVNT_nvnCommandBufferBindSeparateTextures)(NVNcommandBuffer*, NVNshaderStage, int, int, const NVNseparateTextureHandle*);
typedef void (*NVNT_nvnCommandBufferBindSeparateSamplers)(NVNcommandBuffer*, NVNshaderStage, int, int, const NVNseparateSamplerHandle*);
typedef void (*NVNT_nvnCommandBufferSetStencilValueMask)(NVNcommandBuffer*, NVNface, int);
typedef void (*NVNT_nvnCommandBufferSetStencilMask)(NVNcommandBuffer*, NVNface, int);
typedef void (*NVNT_nvnCommandBufferSetStencilRef)(NVNcommandBuffer*, NVNface, int);
typedef void (*NVNT_nvnCommandBufferSetBlendColor)(NVNcommandBuffer*, const float*);
typedef void (*NVNT_nvnCommandBufferSetPointSize)(NVNcommandBuffer*, float);
typedef void (*NVNT_nvnCommandBufferSetLineWidth)(NVNcommandBuffer*, float);
typedef void (*NVNT_nvnCommandBufferSetPolygonOffsetClamp)(NVNcommandBuffer*, float, float, float);
typedef void (*NVNT_nvnCommandBufferSetAlphaRef)(NVNcommandBuffer*, float);
typedef void (*NVNT_nvnCommandBufferSetSampleMask)(NVNcommandBuffer*, int);
typedef void (*NVNT_nvnCommandBufferSetRasterizerDiscard)(NVNcommandBuffer*, uint32_t);
typedef void (*NVNT_nvnCommandBufferSetDepthClamp)(NVNcommandBuffer*, uint32_t);
typedef void (*NVNT_nvnCommandBufferSetConservativeRasterEnable)(NVNcommandBuffer*, uint32_t);
typedef void (*NVNT_nvnCommandBufferSetConservativeRasterDilate)(NVNcommandBuffer*, float);
typedef void (*NVNT_nvnCommandBufferSetSubpixelPrecisionBias)(NVNcommandBuffer*, int, int);
typedef void (*NVNT_nvnCommandBufferCopyBufferToTexture)(NVNcommandBuffer*, NVNbufferAddress, const NVNtexture*, const NVNtextureView*, const NVNcopyRegion*, int);
typedef void (*NVNT_nvnCommandBufferCopyTextureToBuffer)(NVNcommandBuffer*, const NVNtexture*, const NVNtextureView*, const NVNcopyRegion*, NVNbufferAddress, int);
typedef void (*NVNT_nvnCommandBufferCopyTextureToTexture)(NVNcommandBuffer*, const NVNtexture*, const NVNtextureView*, const NVNcopyRegion*, const NVNtexture*, const NVNtextureView*, const NVNcopyRegion*, int);
typedef void (*NVNT_nvnCommandBufferCopyBufferToBuffer)(NVNcommandBuffer*, NVNbufferAddress, NVNbufferAddress, size_t, int);
typedef void (*NVNT_nvnCommandBufferClearBuffer)(NVNcommandBuffer*, NVNbufferAddress, size_t, uint32_t);
typedef void (*NVNT_nvnCommandBufferClearTexture)(NVNcommandBuffer*, const NVNtexture*, const NVNtextureView*, const NVNcopyRegion*, const float*, int);
typedef void (*NVNT_nvnCommandBufferClearTexturei)(NVNcommandBuffer*, const NVNtexture*, const NVNtextureView*, const NVNcopyRegion*, const int*, int);
typedef void (*NVNT_nvnCommandBufferClearTextureui)(NVNcommandBuffer*, const NVNtexture*, const NVNtextureView*, const NVNcopyRegion*, const uint32_t*, int);
typedef void (*NVNT_nvnCommandBufferUpdateUniformBuffer)(NVNcommandBuffer*, NVNbufferAddress, size_t, ptrdiff_t, size_t, const void*);
typedef void (*NVNT_nvnCommandBufferReportCounter)(NVNcommandBuffer*, NVNcounterType, NVNbufferAddress);
typedef void (*NVNT_nvnCommandBufferResetCounter)(NVNcommandBuffer*, NVNcounterType);
typedef void (*NVNT_nvnCommandBufferReportValue)(NVNcommandBuffer*, uint32_t, NVNbufferAddress);
typedef void (*NVNT_nvnCommandBufferSetRenderEnable)(NVNcommandBuffer*, uint32_t);
typedef void (*NVNT_nvnCommandBufferSetRenderEnableConditional)(NVNcommandBuffer*, NVNconditionalRenderMode, NVNbufferAddress);
typedef void (*NVNT_nvnCommandBufferSetRenderTargets)(NVNcommandBuffer*, int, const NVNtexture* const*, const NVNtextureView* const*, const NVNtexture*, const NVNtextureView*);
typedef void (*NVNT_nvnCommandBufferDiscardColor)(NVNcommandBuffer*, int);
typedef void (*NVNT_nvnCommandBufferDiscardDepthStencil)(NVNcommandBuffer*);
typedef void (*NVNT_nvnCommandBufferDownsample)(NVNcommandBuffer*, const NVNtexture*, const NVNtexture*);
typedef void (*NVNT_nvnCommandBufferTiledDownsample)(NVNcommandBuffer*, const NVNtexture*, const NVNtexture*);
typedef void (*NVNT_nvnCommandBufferDownsampleTextureView)(NVNcommandBuffer*, const NVNtexture*, const NVNtextureView*, const NVNtexture*, const NVNtextureView*);
typedef void (*NVNT_nvnCommandBufferTiledDownsampleTextureView)(NVNcommandBuffer*, const NVNtexture*, const NVNtextureView*, const NVNtexture*, const NVNtextureView*);
typedef void (*NVNT_nvnCommandBufferBarrier)(NVNcommandBuffer*, int);
typedef void (*NVNT_nvnCommandBufferWaitSync)(NVNcommandBuffer*, const NVNsync*);
typedef void (*NVNT_nvnCommandBufferFenceSync)(NVNcommandBuffer*, NVNsync*, NVNsyncCondition, int);
typedef void (*NVNT_nvnCommandBufferSetTexturePool)(NVNcommandBuffer*, const NVNtexturePool*);
typedef void (*NVNT_nvnCommandBufferSetSamplerPool)(NVNcommandBuffer*, const NVNsamplerPool*);
typedef void (*NVNT_nvnCommandBufferSetShaderScratchMemory)(NVNcommandBuffer*, const NVNmemoryPool*, ptrdiff_t, size_t);
typedef void (*NVNT_nvnCommandBufferSaveZCullData)(NVNcommandBuffer*, NVNbufferAddress, size_t);
typedef void (*NVNT_nvnCommandBufferRestoreZCullData)(NVNcommandBuffer*, NVNbufferAddress, size_t);
typedef void (*NVNT_nvnCommandBufferSetCopyRowStride)(NVNcommandBuffer*, ptrdiff_t);
typedef void (*NVNT_nvnCommandBufferSetCopyImageStride)(NVNcommandBuffer*, ptrdiff_t);
typedef ptrdiff_t (*NVNT_nvnCommandBufferGetCopyRowStride)(const NVNcommandBuffer*);
typedef ptrdiff_t (*NVNT_nvnCommandBufferGetCopyImageStride)(const NVNcommandBuffer*);
typedef void (*NVNT_nvnCommandBufferDrawTexture)(NVNcommandBuffer*, NVNtextureHandle, const NVNdrawTextureRegion*, const NVNdrawTextureRegion*);
typedef NVNboolean (*NVNT_nvnProgramSetSubroutineLinkage)(NVNprogram*, int, const NVNsubroutineLinkageMapPtr*);
typedef void (*NVNT_nvnCommandBufferSetProgramSubroutines)(NVNcommandBuffer*, NVNprogram*, NVNshaderStage, const int, const int, const int*);
typedef void (*NVNT_nvnCommandBufferBindCoverageModulationTable)(NVNcommandBuffer*, const float*);
typedef void (*NVNT_nvnCommandBufferResolveDepthBuffer)(NVNcommandBuffer*);
typedef void (*NVNT_nvnCommandBufferPushDebugGroupStatic)(NVNcommandBuffer*, uint32_t, const char*);
typedef void (*NVNT_nvnCommandBufferPushDebugGroupDynamic)(NVNcommandBuffer*, uint32_t, const char*);
typedef void (*NVNT_nvnCommandBufferPushDebugGroup)(NVNcommandBuffer*, uint32_t, const char*);
typedef void (*NVNT_nvnCommandBufferPopDebugGroup)(NVNcommandBuffer*);
typedef void (*NVNT_nvnCommandBufferPopDebugGroupId)(NVNcommandBuffer*, uint32_t);
typedef void (*NVNT_nvnCommandBufferInsertDebugMarkerStatic)(NVNcommandBuffer*, uint32_t, const char*);
typedef void (*NVNT_nvnCommandBufferInsertDebugMarkerDynamic)(NVNcommandBuffer*, uint32_t, const char*);
typedef void (*NVNT_nvnCommandBufferInsertDebugMarker)(NVNcommandBuffer*, const char*);
typedef PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC (*NVNT_nvnCommandBufferGetMemoryCallback)(const NVNcommandBuffer*);
typedef void (*NVNT_nvnCommandBufferGetMemoryCallbackData)(const NVNcommandBuffer*);
typedef NVNboolean (*NVNT_nvnCommandBufferIsRecording)(const NVNcommandBuffer*);
typedef NVNboolean (*NVNT_nvnSyncInitialize)(NVNsync*, NVNdevice*);
typedef void (*NVNT_nvnSyncFinalize)(NVNsync*);
typedef void (*NVNT_nvnSyncSetDebugLabel)(NVNsync*, const char*);
typedef void (*NVNT_nvnQueueFenceSync)(NVNqueue*, NVNsync*, NVNsyncCondition, int);
typedef NVNsyncWaitResult (*NVNT_nvnSyncWait)(const NVNsync*, uint64_t);
typedef NVNboolean (*NVNT_nvnQueueWaitSync)(NVNqueue*, const NVNsync*);
typedef void (*NVNT_nvnEventBuilderSetDefaults)(NVNeventBuilder*);
typedef void (*NVNT_nvnEventBuilderSetStorage)(NVNeventBuilder*, const NVNmemoryPool*, int64_t);
typedef NVNboolean (*NVNT_nvnEventInitialize)(NVNevent*, const NVNeventBuilder*);
typedef void (*NVNT_nvnEventFinalize)(NVNevent*);
typedef uint32_t (*NVNT_nvnEventGetValue)(const NVNevent*);
typedef void (*NVNT_nvnEventSignal)(NVNevent*, NVNeventSignalMode, uint32_t);
typedef void (*NVNT_nvnCommandBufferWaitEvent)(NVNcommandBuffer*, const NVNevent*, NVNeventWaitMode, uint32_t);
typedef void (*NVNT_nvnCommandBufferSignalEvent)(NVNcommandBuffer*, const NVNevent*, NVNeventSignalMode, NVNeventSignalLocation, int, uint32_t);

// 头文件里没有原型的名字（新版接口）：只能接记名桩
//   nvnDeviceBuilderGetFlags
//   nvnQueueBuilderSetQueuePriority
//   nvnQueueBuilderGetQueuePriority
//   nvnQueueBuilderGetDevice
//   nvnQueueBuilderGetFlags
//   nvnQueueBuilderGetCommandMemorySize
//   nvnQueueBuilderGetComputeMemorySize
//   nvnQueueBuilderGetControlMemorySize
//   nvnQueueBuilderGetCommandFlushThreshold
//   nvnQueueBuilderGetMemorySize
//   nvnQueueBuilderGetMemory
//   nvnWindowBuilderSetNumActiveTextures
//   nvnWindowBuilderGetDevice
//   nvnWindowBuilderGetNumTextures
//   nvnWindowBuilderGetTexture
//   nvnWindowBuilderGetNumActiveTextures
//   nvnWindowSetNumActiveTextures
//   nvnWindowGetNumActiveTextures
//   nvnWindowGetNumTextures
//   nvnProgramSetShadersExt
//   nvnProgramSetSampleShading
//   nvnMemoryPoolBuilderGetDevice
//   nvnBufferBuilderGetDevice
//   nvnTextureBuilderGetDevice
//   nvnTextureBuilderGetPackagedTextureLayout
//   nvnTextureBuilderGetRawStorageClass
//   nvnTextureGetRawStorageClass
//   nvnSamplerBuilderGetDevice
//   nvnCommandBufferSetCommandMemoryCallbackEnabled
//   nvnCommandBufferSetColorReductionEnable
//   nvnCommandBufferSetColorReductionThresholds
//   nvnSyncInitializeFromFencedGLSync
//   nvnSyncCreateGLSync
//   nvnEventBuilderGetStorage
//   nvnEventBuilderGetMemoryPool
//   nvnEventBuilderGetMemoryOffset
//   nvnEventGetMemoryPool
//   nvnEventGetMemoryOffset
//   nvnCommandBufferSetStencilCullCriteria
//   nvnCommandBufferInsertCheckpointStatic
//   nvnCommandBufferInsertCheckpointDynamic
//   nvnDeviceSetAutomaticCheckpointsEnable
//   nvnDeviceSetAutomaticCheckpointsSamplingInterval
//   nvnDeviceSetAutomaticCheckpointsByDebugGroup
//   nvnDeviceSetCheckpointCallStacksEnable
//   nvnDeviceSetCheckpointMaxCallStacksDepth
