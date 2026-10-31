// Transcribed from the FidelityFX SDK (MIT): ffx_api.h, ffx_api_types.h,
// ffx_upscale.h — local copy /home/ren/Projects/bloodborne_pc/tools/fsr4cap/ffx/.
// Only the members this addon reads; layout must match that SDK release.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <atomic>
#include <mutex>
#include <unordered_map>

#include "nr_runner.hpp"


// The SDK declares everything inside extern "C"; these are plain aggregates either
// way, so the vendored slice is transcribed as C++ declarations inside a namespace.
// The namespace also keeps the names from colliding with any other vendored copy.
namespace ffx_api {

// --- ffx_api.h -------------------------------------------------------------

typedef uint64_t ffxStructType_t;

typedef struct ffxApiHeader {
  ffxStructType_t type;       ///< The structure type; the corresponding value sits nearby with a similar name.
  struct ffxApiHeader* pNext; ///< Next structure, optional parameters/extensions. May be null.
} ffxApiHeader;

typedef ffxApiHeader ffxCreateContextDescHeader;
typedef ffxApiHeader ffxDispatchDescHeader;

/// A pointer to a function that can receive messages from the runtime. May be null.
typedef void (*ffxApiMessage)(uint32_t type, const wchar_t* message);

// ffx_api.h FfxApiReturnCodes: the dispatch hook reads OK to decide whether the
// Full-mode record may run (a failed dispatch left no output to record from).
enum FfxApiReturnCodes : uint32_t {
  FFX_API_RETURN_OK = 0,                      ///< The operation was successful.
  FFX_API_RETURN_ERROR = 1,                   ///< An error that is not further specified.
  FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE = 2,  ///< The structure type was not recognized.
  FFX_API_RETURN_ERROR_RUNTIME_ERROR = 3,     ///< The runtime or effect reported an error.
  FFX_API_RETURN_NO_PROVIDER = 4,             ///< No provider found for the structure type.
  FFX_API_RETURN_ERROR_MEMORY = 5,            ///< A memory allocation failed.
  FFX_API_RETURN_ERROR_PARAMETER = 6,         ///< A parameter was invalid.
  FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE = 7,  ///< The provider is too old.
};

// The effect-identifier field of the type words; sub-ids live in the low bits.
#define FFX_API_EFFECT_MASK 0x00ff0000u
#define FFX_API_EFFECT_ID_UPSCALE 0x00010000u
#define FFX_API_EFFECT_ID_FRAMEGENERATION 0x00020000u

// Combines an effect id with a sub-version into a type word.
#define FFX_API_MAKE_EFFECT_SUB_ID(effectId, subversion) \
  ((effectId & FFX_API_EFFECT_MASK) | (subversion & ~FFX_API_EFFECT_MASK))

// --- ffx_api_types.h -------------------------------------------------------

/// A structure encapsulating a 2-dimensional point, using 32bit unsigned integers.
struct FfxApiDimensions2D {
  uint32_t width;  ///< The width of a 2-dimensional range.
  uint32_t height; ///< The height of a 2-dimensional range.
};

/// A structure encapsulating a 2-dimensional set of floating point coordinates.
struct FfxApiFloatCoords2D {
  float x; ///< The x coordinate of a 2-dimensional point.
  float y; ///< The y coordinate of a 2-dimensional point.
};

/// A structure describing a resource.
struct FfxApiResourceDescription {
  uint32_t type;   ///< The type of the resource.
  uint32_t format; ///< The surface format.
  union {
    uint32_t width; ///< The width of the texture resource.
    uint32_t size;  ///< The size of the buffer resource.
  };

  union {
    uint32_t height; ///< The height of the texture resource.
    uint32_t stride; ///< The stride of the buffer resource.
  };

  union {
    uint32_t depth;     ///< The depth of the texture resource.
    uint32_t alignment; ///< The alignment of the buffer resource.
  };

  uint32_t mipCount; ///< Number of mips (or 0 for full mipchain).
  uint32_t flags;    ///< A set of resource flags.
  uint32_t usage;    ///< Resource usage flags.
};

struct FfxApiResource {
  void* resource;
  FfxApiResourceDescription description;
  uint32_t state;
};

enum FfxApiResourceType : uint32_t {
  FFX_API_RESOURCE_TYPE_BUFFER = 0,
  FFX_API_RESOURCE_TYPE_TEXTURE1D = 1,
  FFX_API_RESOURCE_TYPE_TEXTURE2D = 2,
  FFX_API_RESOURCE_TYPE_TEXTURE_CUBE = 3,
  FFX_API_RESOURCE_TYPE_TEXTURE3D = 4,
};

enum FfxApiSurfaceFormat : uint32_t {
  FFX_API_SURFACE_FORMAT_UNKNOWN = 0,
  FFX_API_SURFACE_FORMAT_R32G32B32A32_TYPELESS = 1,
  FFX_API_SURFACE_FORMAT_R32G32B32A32_UINT = 2,
  FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT = 3,
  FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT = 4,
  FFX_API_SURFACE_FORMAT_R32G32B32_FLOAT = 5,
  FFX_API_SURFACE_FORMAT_R32G32_FLOAT = 6,
  FFX_API_SURFACE_FORMAT_R8_UINT = 7,
  FFX_API_SURFACE_FORMAT_R32_UINT = 8,
  FFX_API_SURFACE_FORMAT_R8G8B8A8_TYPELESS = 9,
  FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM = 10,
  FFX_API_SURFACE_FORMAT_R8G8B8A8_SNORM = 11,
  FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB = 12,
  FFX_API_SURFACE_FORMAT_B8G8R8A8_TYPELESS = 13,
  FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM = 14,
  FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB = 15,
  FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT = 16,
  FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM = 17,
  FFX_API_SURFACE_FORMAT_R16G16_FLOAT = 18,
  FFX_API_SURFACE_FORMAT_R16G16_UINT = 19,
  FFX_API_SURFACE_FORMAT_R16G16_SINT = 20,
  FFX_API_SURFACE_FORMAT_R16_FLOAT = 21,
  FFX_API_SURFACE_FORMAT_R16_UINT = 22,
  FFX_API_SURFACE_FORMAT_R16_UNORM = 23,
  FFX_API_SURFACE_FORMAT_R16_SNORM = 24,
  FFX_API_SURFACE_FORMAT_R8_UNORM = 25,
  FFX_API_SURFACE_FORMAT_R8G8_UNORM = 26,
  FFX_API_SURFACE_FORMAT_R8G8_UINT = 27,
  FFX_API_SURFACE_FORMAT_R32_FLOAT = 28,
  FFX_API_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP = 29,
  FFX_API_SURFACE_FORMAT_R16G16B16A16_TYPELESS = 30,
  FFX_API_SURFACE_FORMAT_R32G32_TYPELESS = 31,
  FFX_API_SURFACE_FORMAT_R10G10B10A2_TYPELESS = 32,
  FFX_API_SURFACE_FORMAT_R16G16_TYPELESS = 33,
  FFX_API_SURFACE_FORMAT_R16_TYPELESS = 34,
  FFX_API_SURFACE_FORMAT_R8_TYPELESS = 35,
  FFX_API_SURFACE_FORMAT_R8G8_TYPELESS = 36,
  FFX_API_SURFACE_FORMAT_R32_TYPELESS = 37,
  FFX_API_SURFACE_FORMAT_R32G32_UINT = 38,
  FFX_API_SURFACE_FORMAT_R8_SNORM = 39,
};

// --- ffx_upscale.h ---------------------------------------------------------

/// Create-time flags of the upscale context (FfxApiCreateContextUpscaleFlags).
enum FfxApiCreateContextUpscaleFlags : uint32_t {
  FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE = (1 << 0),                ///< Input color data is high-dynamic range.
  FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS = (1 << 1), ///< Motion vectors are at display resolution.
  FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION = (1 << 2),///< Motion vectors include the jitter pattern.
  FFX_UPSCALE_ENABLE_DEPTH_INVERTED = (1 << 3),                    ///< Depth buffer data is inverted [1..0].
  FFX_UPSCALE_ENABLE_DEPTH_INFINITE = (1 << 4),                    ///< Depth uses an infinite far plane.
  FFX_UPSCALE_ENABLE_AUTO_EXPOSURE = (1 << 5),                     ///< Automatic exposure is applied.
  FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION = (1 << 6),                ///< The application uses dynamic resolution scaling.
  FFX_UPSCALE_ENABLE_DEBUG_CHECKING = (1 << 7),                    ///< The runtime checks API values and reports issues.
  FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE = (1 << 8),             ///< The color resource holds perceptual colors.
  FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION = (1 << 9),               ///< Debug visualization is allowed.
};

#define FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE \
  FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x00)
struct ffxCreateContextDescUpscale {
  ffxCreateContextDescHeader header;        ///< Header descriptor, use type FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE.
  uint32_t flags;                          ///< Zero or a combination of FfxApiCreateContextUpscaleFlags.
  FfxApiDimensions2D maxRenderSize;        ///< The maximum size that rendering will be performed at.
  FfxApiDimensions2D maxUpscaleSize;       ///< The presentation-resolution size targeted by upscaling.
  ffxApiMessage fpMessage;                 ///< Optional message callback. May be null.
};

#define FFX_API_DISPATCH_DESC_TYPE_UPSCALE \
  FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x01)
struct ffxDispatchDescUpscale {
  ffxDispatchDescHeader header;       ///< Header descriptor, use type FFX_API_DISPATCH_DESC_TYPE_UPSCALE.
  void* commandList;                  ///< Command list to record upscaling commands into.
  FfxApiResource color;               ///< Color buffer for the current frame (at render resolution).
  FfxApiResource depth;               ///< 32bit depth values for the current frame (at render resolution).
  FfxApiResource motionVectors;       ///< 2-dimensional motion vectors.
  FfxApiResource exposure;            ///< Optional 1x1 exposure resource.
  FfxApiResource reactive;            ///< Optional reactive-object alpha resource.
  FfxApiResource transparencyAndComposition; ///< Optional transparency/composition alpha resource.
  FfxApiResource output;              ///< Output color buffer for the current frame (at presentation resolution).
  FfxApiFloatCoords2D jitterOffset;   ///< The subpixel jitter offset applied to the camera.
  FfxApiFloatCoords2D motionVectorScale; ///< The scale factor to apply to motion vectors.
  FfxApiDimensions2D renderSize;      ///< The resolution that was used for rendering the input resources.
  FfxApiDimensions2D upscaleSize;     ///< The resolution the upscaler will upscale to (optional, maxUpscaleSize otherwise).
  bool enableSharpening;              ///< Enable an additional sharpening pass.
  float sharpness;                    ///< The sharpness value between 0 and 1.
  float frameTimeDelta;               ///< The time elapsed since the last frame (milliseconds).
  float preExposure;                  ///< The pre exposure value (must be > 0.0f).
  bool reset;                         ///< True when the camera moved discontinuously.
  float cameraNear;                   ///< The distance to the near plane of the camera.
  float cameraFar;                    ///< The distance to the far plane of the camera.
  float cameraFovAngleVertical;       ///< The vertical camera field of view (radians).
  float viewSpaceToMetersFactor;      ///< The scale factor to convert view space units to meters.
  uint32_t flags;                     ///< Zero or a combination of FfxApiDispatchFsrUpscaleFlags.
};

extern std::atomic<uint32_t> active_upscale_flags;

}  // namespace ffx_api

namespace ffx_api {
inline std::atomic<uint32_t> active_upscale_flags{0};
}

// --- probe interface ---------------------------------------------------------
// Install/Uninstall own the detour lifecycle; nr_fsr.hpp drives the extraction.
namespace ffx_probe {

// extern "C" so the test links the definitions at file scope in
// this header by their unmangled names; the definitions below are inline as well --
// the same ODR constraint as active_upscale_flags: several TUs include this header.
extern "C" bool UpscaleInputs(const ffx_api::ffxDispatchDescUpscale* d, nr_runner::Inputs* in,
                               uint32_t flags = ffx_api::active_upscale_flags);
extern "C" void OnUpscaleGeometry(const ffx_api::ffxDispatchDescUpscale& d,
                                   const nr_runner::Inputs& in,
                                   uint32_t flags = ffx_api::active_upscale_flags);
// The recording path's desc lookups (unit-tested in the host build).
extern "C" DXGI_FORMAT OutputFormat(const ffx_api::FfxApiResourceDescription& desc);
extern "C" bool IsD3d12TextureType(const ffx_api::FfxApiResourceDescription& desc);
bool Install();
void Uninstall();
extern bool installed;

}  // namespace ffx_probe

namespace ffx_probe {

struct UpscaleContext {
  ffx_api::FfxApiDimensions2D max_upscale_size;
  uint32_t flags;
};
inline std::atomic<uint32_t> dispatch_dumps{0};
inline std::mutex context_mutex;
inline std::unordered_map<void*, UpscaleContext> upscale_contexts;

inline void RememberContext(void* context, const ffx_api::ffxCreateContextDescUpscale& desc) {
  if (!context) return;
  std::lock_guard<std::mutex> lock(context_mutex);
  upscale_contexts[context] = {desc.maxUpscaleSize, desc.flags};
  dispatch_dumps.store(0, std::memory_order_relaxed);
}

inline void ForgetContext(void* context) {
  std::lock_guard<std::mutex> lock(context_mutex);
  upscale_contexts.erase(context);
}

inline bool ResolveDispatch(void* context, const ffx_api::ffxDispatchDescUpscale& raw,
                            ffx_api::ffxDispatchDescUpscale& resolved, UpscaleContext& info) {
  resolved = raw;
  info = {};
  std::lock_guard<std::mutex> lock(context_mutex);
  const auto it = upscale_contexts.find(context);
  if (it == upscale_contexts.end()) return false;
  info = it->second;
  if (raw.upscaleSize.width == 0 && raw.upscaleSize.height == 0)
    resolved.upscaleSize = info.max_upscale_size;
  return resolved.upscaleSize.width != 0 && resolved.upscaleSize.height != 0;
}

extern "C" inline bool UpscaleInputs(const ffx_api::ffxDispatchDescUpscale* d, nr_runner::Inputs* in,
                                      uint32_t flags) {
  if (d == nullptr || in == nullptr) return false;
  if (d->header.type != FFX_API_DISPATCH_DESC_TYPE_UPSCALE) return false;
  in->color = static_cast<ID3D12Resource*>(d->color.resource);
  in->depth = static_cast<ID3D12Resource*>(d->depth.resource);
  in->motion = static_cast<ID3D12Resource*>(d->motionVectors.resource);
  in->output = static_cast<ID3D12Resource*>(d->output.resource);
  in->mv_scale_x = d->motionVectorScale.x;
  in->mv_scale_y = d->motionVectorScale.y;
  in->reset = d->reset;
  in->depth_inverted =
      (flags & ffx_api::FFX_UPSCALE_ENABLE_DEPTH_INVERTED) != 0;
  return true;
}

extern "C" inline void OnUpscaleGeometry(const ffx_api::ffxDispatchDescUpscale& d,
                                        const nr_runner::Inputs& in, uint32_t flags) {
  nr_runner::s.dlss_render_w = d.renderSize.width;
  nr_runner::s.dlss_render_h = d.renderSize.height;
  nr_runner::s.dlss_out_w = d.upscaleSize.width;
  nr_runner::s.dlss_out_h = d.upscaleSize.height;
  const bool depth_inverted =
      (flags & ffx_api::FFX_UPSCALE_ENABLE_DEPTH_INVERTED) != 0;
  if (depth_inverted)
    nr_runner::s.dlss_create_flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
  else
    nr_runner::s.dlss_create_flags &= ~NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
  nr_runner::s.color_in_w = d.color.description.width;
  nr_runner::s.color_in_h = d.color.description.height;
  (void)in;  // the extraction itself lands in Inputs; geometry is state only
}

extern "C" inline DXGI_FORMAT OutputFormat(const ffx_api::FfxApiResourceDescription& desc) {
  switch (desc.format) {
    case ffx_api::FFX_API_SURFACE_FORMAT_R32G32B32A32_TYPELESS:
      return DXGI_FORMAT_R32G32B32A32_TYPELESS;
    case ffx_api::FFX_API_SURFACE_FORMAT_R32G32B32A32_UINT: return DXGI_FORMAT_R32G32B32A32_UINT;
    case ffx_api::FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT:
      return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case ffx_api::FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case ffx_api::FFX_API_SURFACE_FORMAT_R32G32B32_FLOAT: return DXGI_FORMAT_R32G32B32_FLOAT;
    case ffx_api::FFX_API_SURFACE_FORMAT_R32G32_FLOAT: return DXGI_FORMAT_R32G32_FLOAT;
    case ffx_api::FFX_API_SURFACE_FORMAT_R8_UINT: return DXGI_FORMAT_R8_UINT;
    case ffx_api::FFX_API_SURFACE_FORMAT_R32_UINT: return DXGI_FORMAT_R32_UINT;
    case ffx_api::FFX_API_SURFACE_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case ffx_api::FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case ffx_api::FFX_API_SURFACE_FORMAT_R8G8B8A8_SNORM: return DXGI_FORMAT_R8G8B8A8_SNORM;
    case ffx_api::FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case ffx_api::FFX_API_SURFACE_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case ffx_api::FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case ffx_api::FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case ffx_api::FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
    case ffx_api::FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case ffx_api::FFX_API_SURFACE_FORMAT_R16G16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;
    case ffx_api::FFX_API_SURFACE_FORMAT_R16G16_UINT: return DXGI_FORMAT_R16G16_UINT;
    case ffx_api::FFX_API_SURFACE_FORMAT_R16G16_SINT: return DXGI_FORMAT_R16G16_SINT;
    case ffx_api::FFX_API_SURFACE_FORMAT_R16_FLOAT: return DXGI_FORMAT_R16_FLOAT;
    case ffx_api::FFX_API_SURFACE_FORMAT_R16_UINT: return DXGI_FORMAT_R16_UINT;
    case ffx_api::FFX_API_SURFACE_FORMAT_R16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case ffx_api::FFX_API_SURFACE_FORMAT_R16_SNORM: return DXGI_FORMAT_R16_SNORM;
    case ffx_api::FFX_API_SURFACE_FORMAT_R8_UNORM: return DXGI_FORMAT_R8_UNORM;
    case ffx_api::FFX_API_SURFACE_FORMAT_R8G8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
    case ffx_api::FFX_API_SURFACE_FORMAT_R8G8_UINT: return DXGI_FORMAT_R8G8_UINT;
    case ffx_api::FFX_API_SURFACE_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case ffx_api::FFX_API_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP:
      return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    case ffx_api::FFX_API_SURFACE_FORMAT_R16G16B16A16_TYPELESS:
      return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case ffx_api::FFX_API_SURFACE_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_TYPELESS;
    case ffx_api::FFX_API_SURFACE_FORMAT_R10G10B10A2_TYPELESS:
      return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case ffx_api::FFX_API_SURFACE_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_TYPELESS;
    case ffx_api::FFX_API_SURFACE_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_TYPELESS;
    case ffx_api::FFX_API_SURFACE_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_TYPELESS;
    case ffx_api::FFX_API_SURFACE_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_TYPELESS;
    case ffx_api::FFX_API_SURFACE_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_TYPELESS;
    case ffx_api::FFX_API_SURFACE_FORMAT_R32G32_UINT: return DXGI_FORMAT_R32G32_UINT;
    case ffx_api::FFX_API_SURFACE_FORMAT_R8_SNORM: return DXGI_FORMAT_R8_SNORM;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

extern "C" inline bool IsD3d12TextureType(const ffx_api::FfxApiResourceDescription& desc) {
  switch (desc.type) {
    case ffx_api::FFX_API_RESOURCE_TYPE_TEXTURE1D:
    case ffx_api::FFX_API_RESOURCE_TYPE_TEXTURE2D:
    case ffx_api::FFX_API_RESOURCE_TYPE_TEXTURE_CUBE:
    case ffx_api::FFX_API_RESOURCE_TYPE_TEXTURE3D: return true;
    default: return false;
  }
}

}  // namespace ffx_probe

#ifndef NR_HOST_MOCK

#include "ngx_probe.hpp"  // Logf/Warnf + AttachedMarker (one marker per probe)

namespace nr_fsr {
inline void OnFfxCreateContext(const ffx_api::ffxCreateContextDescHeader* header, void* context);
inline void OnFfxDispatch(void* context, const ffx_api::ffxDispatchDescHeader* header,
                          uint32_t flags);
// The recording halves (defined in nr_fsr.hpp): PreRecord runs before the trampoline
// (model resolution Render), PostRecord after it (model resolution Full).
inline void PreRecord(const ffx_api::ffxDispatchDescHeader* header, uint32_t flags);
inline void PostRecord(const ffx_api::ffxDispatchDescHeader* header, uint32_t flags);
}  // namespace nr_fsr

namespace ffx_probe {

// Heartbeat cadence for the dispatch hook (the constants pattern of
// ngx_probe.hpp:213); the first-3 full dumps moved to nr_fsr, which logs the
// dispatch's resources and sizes.
constexpr uint32_t kHeartbeatEvery = 600;

using ffxContext = void*;
using ffxReturnCode_t = uint32_t;
struct ffxAllocationCallbacks;

static ffxReturnCode_t (*real_CreateContext)(ffxContext*,
                                             ffx_api::ffxCreateContextDescHeader*,
                                             const ffxAllocationCallbacks*) = nullptr;
static ffxReturnCode_t (*real_DestroyContext)(ffxContext*,
                                               const ffxAllocationCallbacks*) = nullptr;
static ffxReturnCode_t (*real_Dispatch)(ffxContext*,
                                        const ffx_api::ffxDispatchDescHeader*) = nullptr;
inline ffxReturnCode_t HookFfxCreateContext(ffxContext* context,
                                            ffx_api::ffxCreateContextDescHeader* desc,
                                            const ffxAllocationCallbacks* memCb) {
  const ffxReturnCode_t r = real_CreateContext(context, desc, memCb);
  if (r == ffx_api::FFX_API_RETURN_OK && context && *context)
    nr_fsr::OnFfxCreateContext(desc, *context);
  return r;
}

inline ffxReturnCode_t HookFfxDestroyContext(ffxContext* context,
                                             const ffxAllocationCallbacks* memCb) {
  void* identity = context ? *context : nullptr;
  const auto r = real_DestroyContext(context, memCb);
  if (r == ffx_api::FFX_API_RETURN_OK) ForgetContext(identity);
  return r;
}

inline ffxReturnCode_t HookFfxDispatch(ffxContext* context,
                                       const ffx_api::ffxDispatchDescHeader* desc) {
  ffx_api::ffxDispatchDescUpscale resolved = {};
  UpscaleContext info = {};
  const auto* effective = desc;
  bool ready = false;
  if (desc && desc->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE) {
    const auto& raw = *reinterpret_cast<const ffx_api::ffxDispatchDescUpscale*>(desc);
    ready = ResolveDispatch(context ? *context : nullptr, raw, resolved, info);
    if (dispatch_dumps.fetch_add(1, std::memory_order_relaxed) < 3)
      ngx_probe::Logf("nr-fwd: FSR3 context=%p raw=%ux%u resolved=%ux%u max=%ux%u color-allocation=%ux%u output-allocation=%ux%u flags=0x%x render=%ux%u cmdlist=%p ready=%u",
          context ? *context : nullptr, raw.upscaleSize.width, raw.upscaleSize.height,
          resolved.upscaleSize.width, resolved.upscaleSize.height,
          info.max_upscale_size.width, info.max_upscale_size.height,
          raw.color.description.width, raw.color.description.height,
          raw.output.description.width, raw.output.description.height, info.flags,
          raw.renderSize.width, raw.renderSize.height, raw.commandList, unsigned(ready));
    if (ready) effective = &resolved.header;
    else {
      static std::atomic<bool> said{false};
      if (!said.exchange(true))
        ngx_probe::Log("nr-fwd: FSR3 context missing or invalid upscale size -- pass skipped; recreate FSR context by changing quality; allocation size is not an upscale-size fallback");
    }
  }
  if (ready || !desc || desc->type != FFX_API_DISPATCH_DESC_TYPE_UPSCALE)
    nr_fsr::OnFfxDispatch(context ? *context : nullptr, effective, info.flags);
  static std::atomic<uint32_t> count{0};
  const uint32_t n = ++count;
  if (n % kHeartbeatEvery == 0) {
    ngx_probe::Logf("ffx-probe: ffxDispatch #%u (alive)", n);
  }
  const bool pre = nr_runner::PreSr();
  if (pre && ready) nr_fsr::PreRecord(effective, info.flags);
  const ffxReturnCode_t r = real_Dispatch(context, desc);
  if (!pre && ready && r == ffx_api::FFX_API_RETURN_OK)
    nr_fsr::PostRecord(effective, info.flags);
  return r;
}

struct HookEntry {
  const char* export_name;
  void** real;
  void* hook;
};

inline const HookEntry kHooks[] = {
    {"ffxCreateContext", reinterpret_cast<void**>(&real_CreateContext),
     reinterpret_cast<void*>(&HookFfxCreateContext)},
    {"ffxDestroyContext", reinterpret_cast<void**>(&real_DestroyContext),
     reinterpret_cast<void*>(&HookFfxDestroyContext)},
    {"ffxDispatch", reinterpret_cast<void**>(&real_Dispatch),
     reinterpret_cast<void*>(&HookFfxDispatch)},
};

// Defined here next to the declaration at the probe interface (same ODR note as
// active_upscale_flags above).
inline bool installed = false;
inline bool blocked = false;  // a state Install must never retry out of
// The module actually hooked (the panel/status reads which upscaler is in use);
// null until the attach succeeds.
inline HMODULE upscaler_module = nullptr;

inline bool Install() {
  if (installed) return true;
  if (blocked) return false;

  const char* candidates[] = {"amd_fidelityfx_loader_dx12.dll",
                              "amd_fidelityfx_upscaler_dx12.dll",
                              "amd_fidelityfx_dx12.dll"};

  HMODULE module = nullptr;
  const char* module_name = nullptr;
  for (const char* candidate : candidates) {
    module = GetModuleHandleA(candidate);
    if (module != nullptr && GetProcAddress(module, "ffxDispatch") != nullptr) {
      module_name = candidate;
      break;
    }
    module = nullptr;
  }
  if (module == nullptr) return false;

  LONG* marker = ngx_probe::AttachedMarker("ffx");
  if (marker != nullptr && *marker != 0) {
    ngx_probe::Warnf("ffx-probe: not hooking (marker)");
    blocked = true;
    return false;
  }

  if (DetourTransactionBegin() != NO_ERROR) {
    ngx_probe::Log("ffx-probe: DetourTransactionBegin failed");
    return false;
  }
  DetourUpdateThread(GetCurrentThread());

  int attached = 0;
  bool failed = false;
  for (const auto& entry : kHooks) {
    FARPROC proc = GetProcAddress(module, entry.export_name);
    if (proc == nullptr) {
      ngx_probe::Logf("ffx-probe: %s not exported by %s", entry.export_name, module_name);
      continue;
    }
    *entry.real = reinterpret_cast<void*>(proc);
    if (DetourAttach(entry.real, entry.hook) != NO_ERROR) {
      ngx_probe::Warnf("ffx-probe: DetourAttach failed for %s!%s", module_name, entry.export_name);
      failed = true;
      break;
    }
    ++attached;
  }

  if (failed || attached == 0) {
    DetourTransactionAbort();
    for (const auto& entry : kHooks) *entry.real = nullptr;
    // A failed attach is deterministic for a given export -- retrying every
    // present would only repeat it (and flood the log). Stay unhooked.
    if (failed) blocked = true;
    return false;
  }
  if (DetourTransactionCommit() != NO_ERROR) {
    // A failed commit aborts the transaction itself: no patches were applied.
    ngx_probe::Log("ffx-probe: DetourTransactionCommit failed");
    for (const auto& entry : kHooks) *entry.real = nullptr;
    blocked = true;
    return false;
  }

  if (marker != nullptr) *marker = 1;
  installed = true;
  upscaler_module = module;
  ngx_probe::Logf("ffx-probe: hooked %d exports on %s", attached, module_name);
  return true;
}

// Mirror of Install, for DllMain(DLL_PROCESS_DETACH) on FreeLibrary: the
// detours must not outlive the DLL image they jump into. Anything left
// attached here keeps the marker set, so no later load hooks on top of it.
inline void Uninstall() {
  if (!installed) return;

  if (DetourTransactionBegin() != NO_ERROR) {
    ngx_probe::Warnf("ffx-probe: DetourTransactionBegin failed on uninstall — detours leak");
    return;
  }
  DetourUpdateThread(GetCurrentThread());

  bool queued[std::size(kHooks)] = {};
  bool all = true;
  int detached = 0;
  for (size_t i = 0; i < std::size(kHooks); ++i) {
    if (*kHooks[i].real == nullptr) continue;
    if (DetourDetach(kHooks[i].real, kHooks[i].hook) != NO_ERROR) {
      ngx_probe::Warnf("ffx-probe: DetourDetach failed for %s — that detour leaks",
                       kHooks[i].export_name);
      all = false;
      continue;
    }
    queued[i] = true;
    ++detached;
  }

  if (DetourTransactionCommit() != NO_ERROR) {
    // A failed commit aborts the transaction itself: everything stays patched.
    ngx_probe::Warnf("ffx-probe: DetourTransactionCommit failed on uninstall — detours leak");
    return;
  }

  for (size_t i = 0; i < std::size(kHooks); ++i) {
    if (queued[i]) *kHooks[i].real = nullptr;
  }
  installed = !all;
  if (all) upscaler_module = nullptr;
  if (LONG* marker = ngx_probe::AttachedMarker("ffx"); marker != nullptr) {
    // Our own marker: cleared only if all detached (a leak keeps the stale
    // state marked so the next image of this probe refuses to re-attach).
    *marker = all ? 0 : 1;
  }
  ngx_probe::Logf("ffx-probe: detached %d hooks", detached);
}

}  // namespace ffx_probe

#endif  // NR_HOST_MOCK (Detours installation section)
