// Milestone 5a: the visible loop. The DLSSNR model runs once per frame off the game's DLSS-SR
// output and its answer is copied back over that output, so the game's own post-processing and
// tone-mapping consume the enhanced frame. F10 toggles the pass for an instant A/B.
//
// (Milestone 4 proved the mechanism: the game-local nvngx_dlssnr.dll is loaded directly through a
// forwarder DLL whose name contains "nvngx.dll" -- the snippet's caller gate -- bypassing driver
// dispatch, which fails FAIL_OutOfDate under Proton because the NGX OTA updater is unavailable.)
//
// Recipe from OptiScaler_DLSSNR (MIT). The parameter block is the driver core's own capability
// block -- a freshly allocated one lacks the snippet/preset callbacks and create fails
// UnableToInitializeFeature. Its setters are not laid out the way the SDK header declares: the
// 64-bit setter is vtable slot 0 (resources go through it), the uint setter slot 3, and the float
// setter's slot is discovered by round-tripping a value (typed Get() works normally).
//
// This stage feeds the model the linear frame as-is. The display-referred encode with a measured
// white point -- the actual color bridge -- is stage 5b; the F10 A/B this stage provides is how
// that work gets judged.
//
// Inputs, from the fork and our WuWa capture: Color and Output at display resolution, Depth and
// MVec at render resolution (the guides the game hands DLSS), each with its own subrect; MV scale
// passed through from the game, never derived.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#ifdef NR_HOST_MOCK
#include "../test/unit/ffx_mock.hpp"
#else
#include <windows.h>
#include <bcrypt.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include "nr_compose.hpp"
#pragma comment(lib, "bcrypt")
#pragma comment(lib, "version")
#endif

namespace nr_runner {

// --- driver parameter block, driven through its vtable ------------------

constexpr int kVtSetUll = 0;
constexpr int kVtSetUint = 3;

using PFN_SetUll = void(__thiscall*)(void*, const char*, unsigned long long);
using PFN_SetFloat = void(__thiscall*)(void*, const char*, float);
using PFN_SetUint = void(__thiscall*)(void*, const char*, unsigned int);

inline int float_slot = -1;

inline void SetUInt(void* params, const char* name, unsigned int v) {
  void** vt = *reinterpret_cast<void***>(params);
  reinterpret_cast<PFN_SetUint>(vt[kVtSetUint])(params, name, v);
}

inline void SetFloat(void* params, const char* name, float v) {
  void** vt = *reinterpret_cast<void***>(params);
  reinterpret_cast<PFN_SetFloat>(vt[float_slot < 0 ? 1 : float_slot])(params, name, v);
}

inline void SetResource(void* params, const char* name, ID3D12Resource* v) {
  void** vt = *reinterpret_cast<void***>(params);
  reinterpret_cast<PFN_SetUll>(vt[kVtSetUll])(params, name, (unsigned long long)v);
}

// Find the float slot by writing a known value through each candidate setter and reading it back
// with the typed getter. 0.3125 is exact in binary, so the comparison is clean.
inline void DiscoverFloatSlot(NVSDK_NGX_Parameter* params) {
  if (float_slot >= 0) return;
  void** vt = *reinterpret_cast<void***>(params);
  for (int slot = 0; slot < 8; ++slot) {
    const float probe = 0.3125f;
    float read_back = 0.0f;
    reinterpret_cast<PFN_SetFloat>(vt[slot])(params, "NRProbe.Float", probe);
    if (params->Get("NRProbe.Float", &read_back) == NVSDK_NGX_Result_Success && read_back == probe) {
      float_slot = slot;
      ngx_probe::Logf("nr-fwd: float parameters go through vtable slot %d", slot);
      return;
    }
  }
  float_slot = 1;  // the header's answer, as a last resort
  ngx_probe::Log("nr-fwd: no float slot round-tripped; falling back to the header's slot 1");
}

// --- state ---------------------------------------------------------------

using PFN_FwdInit = int(__cdecl*)(const wchar_t*, const wchar_t*, ID3D12Device*, void*);
using PFN_FwdCreate = void*(__cdecl*)(ID3D12GraphicsCommandList*, void*, int*);
using PFN_FwdEvaluate = int(__cdecl*)(ID3D12GraphicsCommandList*, void*, void*);
using PFN_FwdRelease = void(__cdecl*)(void*);
using PFN_GetCapabilityParams = int(__cdecl*)(NVSDK_NGX_Parameter**);

constexpr uint32_t kFullLogs = 3;        // first evals logged in full
constexpr uint32_t kHeartbeatEvery = 600;
constexpr uint32_t kMaxFailStreak = 3;   // consecutive eval failures before the pass disables itself
constexpr int kGraveyardFrames = 240;    // frames a retired resource waits before release
constexpr int kMaxPasses = 4;            // Pass Count ceiling, as in RenoDX

struct Grave {
  ID3D12Resource* resource = nullptr;
  void* feature = nullptr;
  int frames = 0;
};

// The three independent axes of the colour bridge. Declared here because State holds one of
// each; the helpers that turn them into a Bridge live further down with the compose code.
enum Encoding : int {
  kEncAuto = 0,
  kEncLinear = 1,
  kEncSrgb = 2,
  kEncPq = 3,
  kEncScrgb = 4,
  kEncScrgbNl = 5,
  // Not an encoding: the value the pre-split "Measured (legacy)" mode was saved under. Load
  // translates it into encoding + wp_source + curve; nothing else may compare against it.
  kEncLegacyMeasured = 6,
};

// Measured exposes like classic auto-exposure (Reinhard et al. 2002): the frame's log-average
// lands at this fraction of white, i.e. white = average / key, so the average reads as mid-grey
// under either curve. Without it the average itself would be white, and Knee, which caps near 2x
// white, would clip much of an ordinary frame.
constexpr float kMeasuredKey = 0.18f;

// What the encode divides by.
enum WhitePointSource : int {
  kWpFixed = 0,     // DiffuseWhiteToSource(encoding, nits): a constant
  kWpMeasured = 1,  // the frame's own EMA-smoothed log-average: tracks the scene
};

// The curve the model is shown through.
enum CurveKind : int {
  kCurveKnee = 0,     // exact below 0.8x white, rolls off above; caps near 2x white
  kCurveReinhard = 1,  // smooth everywhere, caps near 450x white
};

struct State {
  bool gave_up = false;
  bool enabled = true;

  HMODULE forwarder = nullptr;
  PFN_FwdInit init = nullptr;
  PFN_FwdCreate create = nullptr;
  PFN_FwdEvaluate evaluate = nullptr;
  PFN_FwdRelease release = nullptr;

  NVSDK_NGX_Parameter* caps = nullptr;
  bool snippet_inited = false;
  void* feature = nullptr;               // pass 1
  ID3D12Resource* output = nullptr;      // the model's answer (the last pass's output)
  // Pass Count: every further pass is its own feature (its own temporal history) fed the previous
  // pass's answer. inter[k] is pass k+1's output and pass k+2's input; all stay in proxy space.
  void* extra[kMaxPasses - 1] = {};
  ID3D12Resource* inter[kMaxPasses - 1] = {};
  int built_passes = 1;                  // the pass count the current features were built for
  ID3D12Resource* color_copy = nullptr;  // the original, copied aside for the resolve
  ID3D12Resource* proxy = nullptr;       // the display-referred picture the model is shown
  DXGI_FORMAT output_format = DXGI_FORMAT_UNKNOWN;

  // Compose controls (fork semantics: transfer 0 = bypass, 1 = the model's picture; max_ratio
  // caps how far the model may move any pixel's luminance either way; colour 0 keeps the game's
  // own hue exactly -- only brightness carries the model's verdict).
  float transfer = 1.0f;
  float max_ratio = 4.0f;  // a safety net only; not exposed
  float colour_strength = 0.0f;

  // The colour bridge is three independent choices, not one bundled mode:
  //   encoding  -- how to decode the buffer to linear (what its numbers mean);
  //   wp_source -- what to divide by (a constant, or the frame's own measured average);
  //   curve     -- how to compress the result for the model.
  // They used to be one enum, which is why a scene-referred buffer could only ever be right
  // in one of them. encoding: 0 Auto, 1 Linear BT.709, 2 sRGB, 3 BT.2100 PQ, 4 scRGB,
  // 5 scRGB-nl. 6 is not an encoding: it is the pre-split "Measured (legacy)" value, read
  // once at load and translated into the three fields.
  int encoding = 0;
  int wp_source = kWpFixed;
  int curve = kCurveKnee;
  bool diffuse_white_override = false;
  float diffuse_white_nits = 100.0f;

  // The frame's log-average luminance, EMA-smoothed; -1 until the first reading lands.
  // wp_scale is the user's multiplier on top. Measured only reads it, but it is fed every
  // frame whatever the source, so switching to Measured is instant.
  float wp_ema = -1.0f;
  float wp_scale = 1.0f;
  // Readings to drop before the EMA is trusted again. The readback ring holds kReadbackRing
  // frames of the *old* decode's units after an encoding change, and seeding from those would
  // put a value in the wrong units into the EMA for a second.
  int wp_discard = 0;
  // The resolved decode the EMA was measured under, or -1 before the first pass. A change
  // means every reading in the EMA and the readback ring is in the old decode's units.
  int wp_encoding = -1;

  // What the last frame actually used, for the overlay.
  int active_encoding = 0;
  float active_white_point = 0.0f;

  // Debug view: 0 off, 1 proxy, 2 model answer, 3 difference x20.
  int debug_view = 0;

  // Model tuning, read once when the feature is built; changing them retires the feature.
  // The three strengths mirror the RenoDX DLSS5 addon's intensities: intensity is the model's
  // overall hand (DLSSNR.Intensity), local_structure is the fine-detail synthesis
  // (LocalStructureStrength), local_tone is the broad local-contrast/tone shaping (LocalToneStrength).
  float intensity = 1.0f;
  float local_structure = 1.0f;
  float global_tone = 1.0f;
  float local_tone = 1.0f;
  bool auto_mask = true;
  float skin_structure = 1.0f;
  bool ui_correction = true;
  int preset = 0;
  int style = 0;
  int pass_count = 1;

  // Geometry the pass runs at: the game's DLSS-SR feature, or the back buffer in standalone mode.
  unsigned render_w = 0, render_h = 0;
  unsigned out_w = 0, out_h = 0;
  int create_flags = 0;
  // The game's DLSS-SR geometry as last created, kept apart so a switch back from standalone
  // restores it.
  unsigned dlss_render_w = 0, dlss_render_h = 0;
  unsigned dlss_out_w = 0, dlss_out_h = 0;
  int dlss_create_flags = 0;

  std::atomic<int> dlss_resolution{0};
  // Render: the DLSS-SR color input's own size. A game with dynamic resolution allocates it at
  // the largest render size and renders a subrect of it (High on Life: 1116x628 holding
  // 1114x627), so the pass covers the whole texture and DLSS-SR still reads only its subrect.
  unsigned color_in_w = 0, color_in_h = 0;

  // Where the frame comes from. Standalone runs off the back buffer, ReShade's depth and a
  // motion-vector effect, for a D3D12 game with no DLSS session to hook.
  std::atomic<int> source{0};                 // Source
  std::atomic<ULONGLONG> last_dlss_tick{0};   // last DLSS-SR evaluate seen (GetTickCount64)
  std::atomic<ULONGLONG> last_fsr_tick{0};    // last FSR3 upscale dispatch seen (GetTickCount64)
  // Auto's standalone arm window anchor (0 = not armed yet): set on the first ask, so
  // SourceChoice's kStandaloneArmMs wait starts where StandaloneAllowed's old `start` did.
  std::atomic<ULONGLONG> armed_tick{0};
  std::atomic<int> frame_source{0};  // FrameSource: 0 Auto, 1 Force DLSS, 2 Force FSR3, 3 Force standalone
  bool sa_flip_x = false, sa_flip_y = false;  // motion-vector direction, standalone only
  bool temporal = true;            // false resets the model's history every frame (diagnostic)
  ID3D12Resource* sa_frame = nullptr;  // UAV-capable copy of the back buffer
  // Reduced resolution: when depth is smaller than the screen (a game that renders its scene
  // below the output resolution), the model runs at depth's size. sa_frame is then that size;
  // these carry the rest.
  // Model resolution, the user's choice (kResFull / kResRender). The overlay writes it; the frame
  // paths read it.
  std::atomic<int> model_resolution{0};
  bool sa_reduced = false;
  ID3D12Resource* sa_given = nullptr;  // the downscaled frame as the model was given it
  ID3D12Resource* sa_delta = nullptr;  // the model's change, RGBA16F
  ID3D12Resource* sa_mv = nullptr;     // motion vectors at the model's size
  ID3D12Resource* sa_full = nullptr;   // the full-resolution result
  ID3D12Resource* sa_depth = nullptr;  // depth scaled up to the screen (Full, smaller depth)
  DXGI_FORMAT sa_format = DXGI_FORMAT_UNKNOWN;
  HMODULE ngx_core = nullptr;      // _nvngx.dll, when standalone had to start NGX itself
  // Standalone records on its own command list, made from ReShade's proxy device (the device
  // every resource's GetDevice hands out), and runs it on its own queue between ReShade's work
  // and the present. ReShade's own list is unwrapped; mixing the two crashes the model's driver
  // calls, because ReShade rewrites descriptor handles for heaps made through the proxy.
  ID3D12CommandQueue* sa_queue = nullptr;
  ID3D12CommandAllocator* sa_alloc[3] = {};
  ID3D12GraphicsCommandList* sa_list = nullptr;
  ID3D12Fence* sa_fence_in = nullptr;   // game queue -> ours
  ID3D12Fence* sa_fence_out = nullptr;  // ours -> game queue
  UINT64 sa_value = 0;
  UINT64 sa_alloc_value[3] = {};
  HANDLE sa_event = nullptr;
  bool sa_pending = false;              // a recorded list is waiting for SubmitStandalone
  char sa_status[160] = "";        // why standalone is or is not running, for the overlay
  // Geometry the NR feature was built against; a mismatch retires it.
  unsigned feature_w = 0, feature_h = 0;

  uint32_t eval_count = 0;
  uint32_t fail_streak = 0;

  // Set by the overlay thread (Apply button); consumed on the evaluate thread.
  std::atomic<bool> retire_pending{false};

  std::vector<Grave> graveyard;

  wchar_t game_dir[MAX_PATH] = {};
};

inline State s;

struct DlssGeometry {
  unsigned render_w = 0, render_h = 0;
  unsigned out_w = 0, out_h = 0;
  int create_flags = 0;
  bool valid = false;
};

inline std::mutex dlss_geometry_mutex;
inline DlssGeometry pending_dlss_geometry;

enum Source : int { kSrcNone = 0, kSrcDlss = 1, kSrcStandalone = 2, kSrcFsr = 3 };
// The persisted picker (ini key FrameSource, was StandaloneMode): which session feeds the
// frame. Force is a hard gate on the excluded hook bodies; Auto arbitrates per frame.
enum FrameSource : int { kFsAuto = 0, kFsDlss = 1, kFsFsr = 2, kFsStandalone = 3 };
// Model resolution: Full runs the model on the full-resolution frame; Render at the resolution
// the game renders its scene at, where that is lower (cheaper; see each frame path).
enum ModelResolution : int { kResFull = 0, kResRender = 1 };
// Whether this frame's pass runs before the game's DLSS-SR upscale (model resolution Render) and on
// which geometry. Standalone has no DLSS stage to insert in front of, so it always post-processes
// the frame it was handed.
inline bool PreSr() {
  return s.dlss_resolution.load(std::memory_order_relaxed) == kResRender &&
         s.source.load(std::memory_order_relaxed) != kSrcStandalone;
}
inline bool RecordingModeMatches(bool pre) { return pre == PreSr(); }
inline unsigned PassW() {
  return PreSr() ? (s.color_in_w != 0 ? s.color_in_w : s.render_w) : s.out_w;
}
inline unsigned PassH() {
  return PreSr() ? (s.color_in_h != 0 ? s.color_in_h : s.render_h) : s.out_h;
}

inline const char* ResultName(int r) {
  switch ((unsigned)r) {
    case 0x1: return "Success";
    case 0xBAD00001: return "FAIL_FeatureNotSupported";
    case 0xBAD00002: return "FAIL_PlatformError";
    case 0xBAD00005: return "FAIL_InvalidParameter";
    case 0xBAD00008: return "FAIL_UnsupportedInputFormat";
    case 0xBAD00009: return "FAIL_RWFlagMissing";
    case 0xBAD0000A: return "FAIL_MissingInput";
    case 0xBAD0000B: return "FAIL_UnableToInitializeFeature";
    case 0xBAD0000C: return "FAIL_OutOfDate";
    case 0xBAD0000D: return "FAIL_OutOfGPUMemory";
    case 0xBAD0000E: return "FAIL_UnsupportedFormat";
    case 0xBAD00010: return "FAIL_UnsupportedParameter";
    case 0xBAD00011: return "FAIL_Denied";
    default: return "other";
  }
}

inline bool GiveUp(const char* why) {
  ngx_probe::Logf("nr-fwd: giving up -- %s", why);
  s.gave_up = true;
  return false;
}

inline std::atomic<int> pass_owner{0};

inline bool PassOwnerAcquire(int owner) {
  int expected = 0;
  return pass_owner.compare_exchange_strong(expected, owner, std::memory_order_acquire,
                                            std::memory_order_relaxed);
}

inline void PassOwnerRelease() { pass_owner.store(0, std::memory_order_release); }

class PassGuard {
 public:
  explicit PassGuard(int owner) : acquired_(PassOwnerAcquire(owner)) {}
  ~PassGuard() { if (acquired_) PassOwnerRelease(); }
  PassGuard(const PassGuard&) = delete;
  PassGuard& operator=(const PassGuard&) = delete;
  explicit operator bool() const { return acquired_; }

 private:
  bool acquired_;
};

// Test-only e2e levers (NR_TEST_HOOK_* macros); expands to nothing in release builds.
#include "nr_test_hooks.hpp"

// --- model identity -------------------------------------------------------

// A mismatched nvngx_dlssnr.dll does not fail at the API -- it can report Success on every
// evaluate and crash the game minutes later on an unrelated thread. So the model's version and
// SHA-256 are logged at init (on a background thread; the file is ~160MB), so every user report
// identifies the model that was actually running. Several builds of 310.8.0 with the same size
// circulate and have run fine, so no single hash is singled out as the right one.

inline HANDLE model_id_thread = nullptr;

inline DWORD WINAPI ModelIdentityThread(LPVOID arg) {
  wchar_t* path = static_cast<wchar_t*>(arg);

  char version[64] = "unknown";
  DWORD handle = 0;
  if (const DWORD info_size = GetFileVersionInfoSizeW(path, &handle)) {
    void* info = std::malloc(info_size);
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT ffi_len = 0;
    if (info != nullptr && GetFileVersionInfoW(path, 0, info_size, info) &&
        VerQueryValueW(info, L"\\", reinterpret_cast<void**>(&ffi), &ffi_len) && ffi != nullptr) {
      std::snprintf(version, sizeof(version), "%u.%u.%u.%u", HIWORD(ffi->dwFileVersionMS),
                    LOWORD(ffi->dwFileVersionMS), HIWORD(ffi->dwFileVersionLS),
                    LOWORD(ffi->dwFileVersionLS));
    }
    std::free(info);
  }

  char sha_hex[65] = "unreadable";
  unsigned long long total = 0;
  const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    constexpr DWORD kChunk = 1 << 20;
    void* buf = std::malloc(kChunk);
    if (buf != nullptr &&
        BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
      unsigned char digest[32];
      DWORD read = 0;
      bool ok = true;
      while (ReadFile(file, buf, kChunk, &read, nullptr) && read > 0) {
        if (BCryptHashData(hash, static_cast<PUCHAR>(buf), read, 0) != 0) {
          ok = false;
          break;
        }
        total += read;
      }
      if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0)
        for (int i = 0; i < 32; ++i) std::snprintf(sha_hex + i * 2, 3, "%02x", digest[i]);
    }
    if (hash != nullptr) BCryptDestroyHash(hash);
    if (alg != nullptr) BCryptCloseAlgorithmProvider(alg, 0);
    std::free(buf);
    CloseHandle(file);
  }

  ngx_probe::Logf("nr-fwd: model nvngx_dlssnr.dll -- version %s, %llu bytes, sha256=%s", version,
                  total, sha_hex);
  std::free(path);
  return 0;
}

inline void LogModelIdentityAsync(const wchar_t* model_path) {
  wchar_t* copy = _wcsdup(model_path);
  if (copy == nullptr) return;
  model_id_thread = CreateThread(nullptr, 0, ModelIdentityThread, copy, 0, nullptr);
  if (model_id_thread == nullptr) std::free(copy);
}

// --- lifetime ------------------------------------------------------------

// The GPU may still be executing command lists that reference a retired feature or texture, so
// they wait in the graveyard for a couple hundred frames before release. The graveyard grows as
// needed: releasing early instead used to free a texture the GPU was still reading -- three quick
// Apply presses overflowed the old fixed 8 slots and killed the device (issue #1).
inline void Bury(ID3D12Resource* resource, void* feature) {
  if (resource == nullptr && feature == nullptr) return;
  for (auto& g : s.graveyard) {
    if (g.resource == nullptr && g.feature == nullptr) {
      g.resource = resource;
      g.feature = feature;
      g.frames = kGraveyardFrames;
      return;
    }
  }
  s.graveyard.push_back({resource, feature, kGraveyardFrames});
}

inline void TickGraveyard() {
  for (auto& g : s.graveyard) {
    if (g.resource == nullptr && g.feature == nullptr) continue;
    if (--g.frames > 0) continue;
    if (g.resource != nullptr) g.resource->Release();
    if (g.feature != nullptr && s.release != nullptr) s.release(g.feature);
    g = {};
  }
}

// Standalone owns its queue, so it can wait for the GPU to be done with a retired feature and
// release it at once. The graveyard's grace period exists for the game's own command lists; at
// Pass Count 4 it otherwise holds several sets of four model instances in VRAM at a time.
inline bool StandaloneIdle(DWORD timeout_ms) {
  if (s.source.load(std::memory_order_relaxed) != kSrcStandalone || s.sa_fence_out == nullptr ||
      s.sa_event == nullptr)
    return false;
  if (s.sa_fence_out->GetCompletedValue() >= s.sa_value) return true;
  s.sa_fence_out->SetEventOnCompletion(s.sa_value, s.sa_event);
  return WaitForSingleObject(s.sa_event, timeout_ms) == WAIT_OBJECT_0;
}

inline void Drop(ID3D12Resource* resource, void* feature, bool now) {
  if (!now) {
    Bury(resource, feature);
    return;
  }
  if (resource != nullptr) resource->Release();
  if (feature != nullptr && s.release != nullptr) s.release(feature);
}

inline void RetireFeature(const char* why) {
  if (s.feature == nullptr && s.output == nullptr) return;
  ngx_probe::Logf("nr-fwd: retiring NR feature (%s)", why);
  const bool now = StandaloneIdle(2000);
  Drop(s.output, s.feature, now);
  Drop(s.color_copy, nullptr, now);
  Drop(s.proxy, nullptr, now);
  for (int k = 0; k < kMaxPasses - 1; ++k) {
    Drop(s.inter[k], s.extra[k], now);
    s.inter[k] = nullptr;
    s.extra[k] = nullptr;
  }
  s.feature = nullptr;
  s.output = nullptr;
  s.color_copy = nullptr;
  s.proxy = nullptr;
  s.output_format = DXGI_FORMAT_UNKNOWN;
  s.feature_w = s.feature_h = 0;
}

// Remember the game's DLSS-SR geometry; the NR feature is built against the display resolution and
// its guides against the render resolution.
inline void OnDlssCreate(unsigned w, unsigned h, unsigned ow, unsigned oh, int flags) {
  std::lock_guard<std::mutex> lock(dlss_geometry_mutex);
  pending_dlss_geometry = {w, h, ow, oh, flags, true};
}

inline bool ApplyDlssGeometry() {
  DlssGeometry geometry;
  {
    std::lock_guard<std::mutex> lock(dlss_geometry_mutex);
    geometry = pending_dlss_geometry;
  }
  if (!geometry.valid) return false;
  s.dlss_render_w = s.render_w = geometry.render_w;
  s.dlss_render_h = s.render_h = geometry.render_h;
  s.dlss_out_w = s.out_w = geometry.out_w;
  s.dlss_out_h = s.out_h = geometry.out_h;
  s.dlss_create_flags = s.create_flags = geometry.create_flags;
  if (s.feature != nullptr && (s.feature_w != PassW() || s.feature_h != PassH()))
    RetireFeature("geometry changed");
  return s.dlss_out_w != 0 && s.dlss_out_h != 0;
}

// Process teardown: pointers in the shared block must not outlive us.
inline void Shutdown() {
  if (model_id_thread != nullptr) {
    WaitForSingleObject(model_id_thread, 3000);  // the hash thread must not outlive the DLL
    CloseHandle(model_id_thread);
    model_id_thread = nullptr;
  }
  if (s.caps != nullptr) {
    void* p = s.caps;
    SetUInt(p, "DLSSNR.Enabled", 0u);
    SetResource(p, "DLSSNR.Color", nullptr);
    SetResource(p, "DLSSNR.Depth", nullptr);
    SetResource(p, "DLSSNR.MVec", nullptr);
    SetResource(p, "DLSSNR.Output", nullptr);
  }
  if (s.feature != nullptr && s.release != nullptr) s.release(s.feature);
  s.feature = nullptr;
  if (s.output != nullptr) s.output->Release();
  s.output = nullptr;
  if (s.color_copy != nullptr) s.color_copy->Release();
  s.color_copy = nullptr;
  if (s.proxy != nullptr) s.proxy->Release();
  s.proxy = nullptr;
  if (s.sa_frame != nullptr) s.sa_frame->Release();
  s.sa_frame = nullptr;
  for (ID3D12Resource** r : {&s.sa_given, &s.sa_delta, &s.sa_mv, &s.sa_full, &s.sa_depth}) {
    if (*r != nullptr) (*r)->Release();
    *r = nullptr;
  }
  if (s.sa_fence_out != nullptr && s.sa_event != nullptr &&
      s.sa_fence_out->GetCompletedValue() < s.sa_value) {
    s.sa_fence_out->SetEventOnCompletion(s.sa_value, s.sa_event);
    WaitForSingleObject(s.sa_event, 2000);
  }
  if (s.sa_list != nullptr) s.sa_list->Release();
  for (auto& a : s.sa_alloc) {
    if (a != nullptr) a->Release();
    a = nullptr;
  }
  if (s.sa_queue != nullptr) s.sa_queue->Release();
  if (s.sa_fence_in != nullptr) s.sa_fence_in->Release();
  if (s.sa_fence_out != nullptr) s.sa_fence_out->Release();
  if (s.sa_event != nullptr) CloseHandle(s.sa_event);
  s.sa_list = nullptr;
  s.sa_queue = nullptr;
  s.sa_fence_in = s.sa_fence_out = nullptr;
  s.sa_event = nullptr;
  for (int k = 0; k < kMaxPasses - 1; ++k) {
    if (s.inter[k] != nullptr) s.inter[k]->Release();
    if (s.extra[k] != nullptr && s.release != nullptr) s.release(s.extra[k]);
    s.inter[k] = nullptr;
    s.extra[k] = nullptr;
  }
  for (auto& g : s.graveyard) {
    if (g.resource != nullptr) g.resource->Release();
    if (g.feature != nullptr && s.release != nullptr) s.release(g.feature);
    g = {};
  }
  nr_compose::Release();
}

inline void SetEnabled(bool on) {
  if (s.enabled == on) return;
  s.enabled = on;
  s.fail_streak = 0;
  ngx_probe::Logf("nr-fwd: pass %s", on ? "ENABLED" : "DISABLED");
}

inline bool IsEnabled() { return s.enabled; }

// --- setup ---------------------------------------------------------------

// Everything lives beside the game exe: the forwarder, the model, and our data path.
inline bool ResolveGameDir() {
  if (s.game_dir[0] != 0) return true;
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  wchar_t* slash = wcsrchr(exe, L'\\');
  if (slash == nullptr) return false;
  *slash = 0;
  wcscpy_s(s.game_dir, exe);
  return true;
}

inline bool EnsureNgxCore(ID3D12GraphicsCommandList* cmd);

inline bool EnsureSetup(ID3D12GraphicsCommandList* cmd) {
  if (s.caps != nullptr && s.forwarder != nullptr) return true;
  NR_TEST_HOOKS_INIT();

  if (!ResolveGameDir()) return GiveUp("could not resolve the game directory");

  if (s.forwarder == nullptr) {
    wchar_t path[MAX_PATH] = {};
    swprintf_s(path, L"%s\\nvngx.dll_nrfwd.dll", s.game_dir);
    s.forwarder = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (s.forwarder == nullptr) return GiveUp("nvngx.dll_nrfwd.dll not found beside the game exe");
    s.init = (PFN_FwdInit)GetProcAddress(s.forwarder, "nrfwd_init");
    s.create = (PFN_FwdCreate)GetProcAddress(s.forwarder, "nrfwd_create");
    s.evaluate = (PFN_FwdEvaluate)GetProcAddress(s.forwarder, "nrfwd_evaluate");
    s.release = (PFN_FwdRelease)GetProcAddress(s.forwarder, "nrfwd_release");
    if (s.init == nullptr || s.create == nullptr || s.evaluate == nullptr || s.release == nullptr)
      return GiveUp("forwarder exports did not resolve");
    ngx_probe::Log("nr-fwd: forwarder loaded");
  }

  if (s.caps == nullptr) {
    // The driver core's capability block. In a DLSS game the game's own init already loaded
    // _nvngx.dll, so this is a read of existing state; in a pure FSR3 game nothing loads it,
    // so fall back to loading it ourselves (the load EnsureNgxCore makes for standalone).
    // A module that is not there yet is timing, not a verdict: GetModuleHandle still catches
    // a later load by NVAPI/the game on the next pass, so this logs once and returns false
    // instead of GiveUp -- a module-load miss must never set gave_up and kill every source
    // for the session. LoadLibrary itself is attempted only once: repeating it while NVAPI
    // is absent would rescan the disk every frame.
    HMODULE nvngx = GetModuleHandleA("_nvngx.dll");
    static bool load_tried = false;
    if (nvngx == nullptr && !load_tried) {
      load_tried = true;
      nvngx = LoadLibraryW(L"_nvngx.dll");
      if (nvngx != nullptr)
        ngx_probe::Log("nr-fwd: _nvngx.dll loaded on demand (no DLSS session had it resident)");
    }
    if (nvngx == nullptr) {
      static bool said = false;
      if (!said) {
        said = true;
        ngx_probe::Log("nr-fwd: _nvngx.dll not resident yet -- pass stays idle until it loads "
                       "(will retry)");
      }
      return false;  // retryable, deliberately not GiveUp
    }
    auto get_caps =
        (PFN_GetCapabilityParams)GetProcAddress(nvngx, "NVSDK_NGX_D3D12_GetCapabilityParameters");
    if (get_caps == nullptr) return GiveUp("GetCapabilityParameters not exported by _nvngx.dll");
    if (get_caps(&s.caps) != NVSDK_NGX_Result_Success || s.caps == nullptr) {
      s.caps = nullptr;
      if (s.ngx_core == nullptr && s.source.load(std::memory_order_relaxed) == kSrcFsr) {
        ngx_probe::Log("nr-fwd: FSR3 capabilities unavailable; initializing NGX core");
        EnsureNgxCore(cmd);
      }
      static bool said_caps = false;
      if (!said_caps) {
        said_caps = true;
        ngx_probe::Log("nr-fwd: the NGX core refused its capability parameters -- will retry");
      }
      return false;
    }
    DiscoverFloatSlot(s.caps);
  }

  return true;
}

// Three textures at the pass geometry (display resolution after the SR upscale, render
// resolution before it), sourced in the game frame's format (accepted by the model in milestone
// 5a): the model's answer, the copied-aside original, and the encoded proxy.
inline bool CreateTextures(ID3D12GraphicsCommandList* cmd, ID3D12Resource* game_output) {
  if (s.output != nullptr) return true;

  const D3D12_RESOURCE_DESC game_desc = game_output->GetDesc();
  // The pass copies the frame whole (the geometry must match or CopyResource hard-fails
  // mid-frame) and writes it in place as a UAV (flagged below when absent). Retiring the
  // feature drops the textures, so a mode switch lands back here and is checked exactly when it
  // must be.
  if (game_desc.Width != PassW() || game_desc.Height != PassH()) {
    // Skipped, not given up on: the next frame or a model resolution switch may well match.
    static bool said = false;
    if (!said)
      ngx_probe::Logf("nr-fwd: skipping -- the game frame is %llux%u, the pass %ux%u",
                      (unsigned long long)game_desc.Width, game_desc.Height, PassW(), PassH());
    said = true;
    return false;
  }
  if (PreSr() && (game_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
    // NGX does not promise the flag on a color *input*, but this addon only ever runs on
    // vkd3d-proton, where the write lands regardless -- Control's input buffer is exactly such
    // a game and renders correctly through it -- so warn with the flags seen instead of
    // disabling the pass; a native D3D12 debug layer would reject this outright.
    ngx_probe::Logf("nr-fwd: warning: the DLSS-SR color input lacks ALLOW_UNORDERED_ACCESS "
                    "(flags=0x%x); writing it anyway under vkd3d",
                    (unsigned)game_desc.Flags);
  }

  ID3D12Device* device = nullptr;
  if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    return GiveUp("could not reach the device from the command list");

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = PassW();
  desc.Height = PassH();
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = game_desc.Format;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  // colorCopy must match the game's output format exactly (CopyResource); the proxy and the
  // model's answer go FP16 -- R11G11B10's 5-bit blue mantissa rounds the encode toward green.
  struct {
    ID3D12Resource** texture;
    DXGI_FORMAT format;
  } textures[3] = {
      {&s.output, DXGI_FORMAT_R16G16B16A16_FLOAT},
      {&s.color_copy, game_desc.Format},
      {&s.proxy, DXGI_FORMAT_R16G16B16A16_FLOAT},
  };
  bool ok = true;
  for (auto& t : textures) {
    desc.Format = t.format;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               IID_PPV_ARGS(t.texture)))) {
      ok = false;
      break;
    }
  }
  device->Release();
  if (!ok) {
    if (s.output != nullptr) s.output->Release();
    if (s.color_copy != nullptr) s.color_copy->Release();
    s.output = s.color_copy = s.proxy = nullptr;
    return GiveUp("texture creation failed");
  }
  const char* mode = s.source.load(std::memory_order_relaxed) == kSrcStandalone ? "standalone"
                     : PreSr()                    ? "model resolution render"
                                                  : "model resolution full";
  ngx_probe::Logf("nr-fwd: textures %ux%u format=%u (answer, original, proxy, %s)", PassW(),
                  PassH(), (unsigned)game_desc.Format, mode);
  return true;
}

// Model tuning, all defaults. Read at create, re-written at evaluate because the block is shared
// with the game's own DLSS, which overwrites values between frames.
inline void WriteTuning(void* p) {
  SetFloat(p, "DLSSNR.Intensity", s.intensity);
  SetUInt(p, "DLSSNR.Style", (unsigned)s.style);
  SetFloat(p, "DLSSNR.LocalStructureStrength", s.local_structure);
  SetFloat(p, "DLSSNR.GlobalToneStrength", s.global_tone);
  SetFloat(p, "DLSSNR.LocalToneStrength", s.local_tone);
  SetUInt(p, "DLSSNR.UseAutoMask", s.auto_mask ? 1u : 0u);
  // -1 leaves characters on the global structure strength, as before, when masking is off.
  SetFloat(p, "DLSSNR.SkinStructureStrength", s.auto_mask ? s.skin_structure : -1.0f);
  SetUInt(p, "DLSSNR.UICorrection", s.ui_correction ? 1u : 0u);
}

// --- colour bridge ----------------------------------------------------------
// Encoding, WhitePointSource and CurveKind are declared with the state above; this is where
// they turn into a Bridge.

inline bool IsUnormColorFormat(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
      return true;
    default:
      return false;
  }
}

// Auto, as in RenoDX: native DLSS output is linear BT.709. A UNORM output is not native DLSS
// output -- it is display-referred SDR (the dlss5-bridge synthetic contract on a D3D11 game), so
// it is sRGB.
inline int ResolveEncoding(int requested, DXGI_FORMAT output_format) {
  if (requested != kEncAuto) return requested;
  return IsUnormColorFormat(output_format) ? kEncSrgb : kEncLinear;
}

// RenoDX's automatic diffuse white per encoding.
inline float AutoDiffuseWhiteNits(int encoding) {
  switch (encoding) {
    case kEncPq:
    case kEncScrgb: return 250.0f;
    case kEncScrgbNl: return 203.0f;
    default: return 100.0f;
  }
}

// The source-linear value that is diffuse white: 1.0 = 100 nits for linear BT.709 and sRGB,
// 1.0 = 80 nits for scRGB, nits for PQ.
inline float DiffuseWhiteToSource(int encoding, float nits) {
  switch (encoding) {
    case kEncPq: return nits;
    case kEncScrgb:
    case kEncScrgbNl: return nits / 80.0f;
    default: return nits / 100.0f;
  }
}

// Called by the overlay after the create-time model settings changed: the model reads them only
// while building the feature, so it is retired and rebuilt. The overlay runs on the present
// thread while the owning thread owns the feature and textures, so only a flag is set here; the
// retire itself executes at the top of the next prepare (PreparePass, PrepareFsrPass or the
// standalone frame path -- whichever source owns the next frame), on that thread under the
// pass-owner token.
inline void ApplyModelSettings() { s.retire_pending.store(true, std::memory_order_relaxed); }

// Writes the create-time block and builds one feature; nullptr on failure.
inline void* CreateOne(ID3D12GraphicsCommandList* cmd, int pass) {
  void* p = s.caps;
  SetUInt(p, "DLSSNR.Enabled", 1u);
  SetUInt(p, "DLSSNR.Width", PassW());
  SetUInt(p, "DLSSNR.Height", PassH());
  SetUInt(p, "CreationNodeMask", 1u);
  SetUInt(p, "VisibilityNodeMask", 1u);
  SetUInt(p, "DLSSNR.Hint.Render.Preset", (unsigned)s.preset);
  WriteTuning(p);

  int create_result = 0;
  void* feature = s.create(cmd, s.caps, &create_result);
  ngx_probe::Logf("nr-fwd: CreateFeature(18) pass %d => 0x%x (%s) handle=%p", pass,
                  (unsigned)create_result, ResultName(create_result), feature);
  return feature;
}

// One FP16 display-resolution texture for a pass's intermediate answer.
inline ID3D12Resource* CreateInter(ID3D12GraphicsCommandList* cmd) {
  ID3D12Device* device = nullptr;
  if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) return nullptr;
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = PassW();
  desc.Height = PassH();
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ID3D12Resource* texture = nullptr;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                             IID_PPV_ARGS(&texture))))
    texture = nullptr;
  device->Release();
  return texture;
}

// The further passes, one create per frame like the first. A pass that cannot be built caps the
// pass count instead of giving up: the passes already built keep running.
inline bool EnsureExtraPasses(ID3D12GraphicsCommandList* cmd) {
  for (int k = 0; k < s.built_passes - 1; ++k) {
    if (s.extra[k] != nullptr) continue;
    if (s.inter[k] == nullptr) s.inter[k] = CreateInter(cmd);
    if (s.inter[k] != nullptr) s.extra[k] = CreateOne(cmd, k + 2);
    if (s.extra[k] == nullptr) {
      ngx_probe::Logf("nr-fwd: pass %d could not be built -- running %d pass(es)", k + 2, k + 1);
      if (s.inter[k] != nullptr) s.inter[k]->Release();
      s.inter[k] = nullptr;
      s.built_passes = k + 1;
      return true;
    }
    return false;  // evaluate from the next frame on, as with the first feature
  }
  return true;
}

inline bool EnsureFeature(ID3D12GraphicsCommandList* cmd) {
  if (s.feature != nullptr) return EnsureExtraPasses(cmd);

  if (!s.snippet_inited) {
    wchar_t snippet[MAX_PATH] = {};
    swprintf_s(snippet, L"%s\\nvngx_dlssnr.dll", s.game_dir);
    // Before init, not after: a mismatched model can crash without ever returning an error, and
    // the identity line must already be in the log when it does.
    LogModelIdentityAsync(snippet);
    ID3D12Device* device = nullptr;
    if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
      return GiveUp("could not reach the device for init");
    const int init_result = s.init(snippet, s.game_dir, device, s.caps);
    device->Release();
    ngx_probe::Logf("nr-fwd: snippet init => 0x%x (%s)", (unsigned)init_result,
                    ResultName(init_result));
    if (init_result != 1) return GiveUp("snippet init failed");
    s.snippet_inited = true;
  }

  s.feature = CreateOne(cmd, 1);
  if (s.feature == nullptr) return GiveUp("feature creation failed");
  s.built_passes = s.pass_count < 1 ? 1 : (s.pass_count > kMaxPasses ? kMaxPasses : s.pass_count);
  s.feature_w = PassW();
  s.feature_h = PassH();
  const char* where = s.source.load(std::memory_order_relaxed) == kSrcStandalone ? "standalone frame"
                      : PreSr()                    ? "render, before the SR upscale"
                                                   : "upscaled, after the SR upscale";
  ngx_probe::Logf("nr-fwd: pass geometry %ux%u (%s)", s.feature_w, s.feature_h, where);
  // Evaluate from the next frame on, so the init work this create recorded executes first.
  return false;
}

// --- the per-frame pass ---------------------------------------------------

// If the model fails after RecordPre already ran, put every state back so the frame proceeds
// with the original picture untouched: UAV for the post-SR path (the state the game's output is
// in around DLSS evaluation), NON_PIXEL_SHADER_RESOURCE for the pre-SR path (the state the
// game's DLSS-SR is about to read its color input in).
inline void AbortAfterPre(ID3D12GraphicsCommandList* cmd, ID3D12Resource* game_out) {
  nr_compose::Barrier(cmd, game_out, D3D12_RESOURCE_STATE_COPY_SOURCE,
                      PreSr() ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                              : D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  nr_compose::Barrier(cmd, s.color_copy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  nr_compose::Barrier(cmd, s.proxy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

// One frame's inputs: the frame (UAV-capable, read and written in place), and the guides.
struct Inputs {
  ID3D12Resource* color = nullptr;
  ID3D12Resource* depth = nullptr;
  ID3D12Resource* motion = nullptr;
  // The output resource of the source's own dispatch (FSR's ffxDispatchDescUpscale
  // output): model resolution Full takes the frame from it after the trampoline.
  // Written only by the FSR extraction; the DLSS/standalone paths leave it null.
  ID3D12Resource* output = nullptr;
  float mv_scale_x = 1.0f, mv_scale_y = 1.0f;
  bool reset = false;
  bool depth_inverted = false;
  unsigned depth_w = 0, depth_h = 0;  // the depth region actually rendered; 0 = render size
  // The game's DLSS-SR output format, the reference Auto's encode decision keys off in both
  // model resolutions; UNKNOWN (standalone) falls back to the frame itself.
  DXGI_FORMAT output_format = DXGI_FORMAT_UNKNOWN;
};

inline void Evaluate(ID3D12GraphicsCommandList* cmd, const Inputs& in) {
  ID3D12Resource* color = in.color;
  ID3D12Resource* depth = in.depth;
  ID3D12Resource* motion = in.motion;
  if (color == nullptr || depth == nullptr || motion == nullptr) return;

  // The frame's colorimetry does not move with the model resolution, so Auto resolves the same
  // way in both modes -- from the DLSS-SR output, not whichever resource this mode encodes.
  s.output_format =
      in.output_format != DXGI_FORMAT_UNKNOWN ? in.output_format : color->GetDesc().Format;
  if (!CreateTextures(cmd, color)) return;

  // Pre-SR: the frame is the game's DLSS-SR color input, in NON_PIXEL_SHADER_RESOURCE for the
  // evaluate the original hook is about to make. RecordPre assumes UAV, so hand it over in the
  // state the compose pass expects and hand it back in the state NGX expects (below, after the
  // resolve).
  if (PreSr())
    nr_compose::Barrier(cmd, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  // The colour bridge, as three independent choices rather than one bundled mode.
  //
  // The measurement is fed every frame whatever the source: the lum shaders decode with the
  // bridge's own encoding, so a reading lands in that encoding's units (nits for PQ, buffer
  // units for linear) and the EMA is only meaningful for the encoding that produced it.
  const int encoding = ResolveEncoding(s.encoding, s.output_format);
  {
    // A decode change invalidates both the EMA and the readback ring, which still holds the
    // old decode's units for kReadbackRing frames. Detected here rather than flagged by the
    // overlay: the evaluate thread writes wp_ema/wp_discard (the overlay only reads wp_ema
    // for display), and it sees the new encoding in the same pass it would have used the
    // stale one.
    if (s.wp_encoding != encoding) {
      s.wp_encoding = encoding;
      s.wp_ema = -1.0f;
      s.wp_discard = static_cast<int>(nr_compose::kReadbackRing) - 1;
    }
    const float raw_wp = nr_compose::ReadWhitePoint(s.eval_count);
    if (s.wp_discard > 0) {
      --s.wp_discard;
    } else if (raw_wp > 0.0f && raw_wp == raw_wp) {
      s.wp_ema = (s.wp_ema <= 0.0f) ? raw_wp : s.wp_ema + 0.05f * (raw_wp - s.wp_ema);
    }
  }

  // The constant this encoding would use, also the fallback before the first reading lands.
  const float nits =
      s.diffuse_white_override ? s.diffuse_white_nits : AutoDiffuseWhiteNits(encoding);
  const float fixed_wp = DiffuseWhiteToSource(encoding, nits < 1.0f ? 1.0f : nits);

  nr_compose::Bridge bridge = {};
  bridge.encoding = (uint32_t)encoding;
  bridge.curve = (uint32_t)s.curve;
  if (s.wp_source == kWpMeasured) {
    // Auto-exposure. A scene-referred buffer's scale moves with the scene -- Control's swings
    // 6x within one session -- so no constant can track it; the frame's own average can.
    // The floor matches SourceToRel's own 1e-4 guard: an older 0.01 floor silently doubled
    // the white point in dark scenes, where this buffer's average sits near 0.005.
    float wp = (s.wp_ema > 0.0f) ? s.wp_ema / kMeasuredKey : fixed_wp;
    bridge.white_point = (wp < 1e-4f ? 1e-4f : (wp > 1000.0f ? 1000.0f : wp)) * s.wp_scale;
  } else {
    // White point scale is Measured's own: a fixed white point already has Diffuse White (nits),
    // and a scale left over from a Measured session must not move the fixed picture unseen.
    bridge.white_point = fixed_wp;
  }
  s.active_encoding = encoding;
  s.active_white_point = bridge.white_point;

  // Copy the original aside, encode the display-referred proxy, record the measurement.
  if (!nr_compose::RecordPre(cmd, color, s.color_copy, s.proxy, PassW(), PassH(), s.eval_count,
                             bridge)) {
    // RecordPre fails before its own barriers, so the frame is only in the state the entry
    // barrier above left it in: hand it back for the evaluate this hook is about to make.
    if (PreSr())
      nr_compose::Barrier(cmd, color, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    GiveUp("compose pipeline initialisation failed");
    return;
  }

  const float mv_scale_x = in.mv_scale_x, mv_scale_y = in.mv_scale_y;
  const int reset = (in.reset || !s.temporal) ? 1 : 0;
  const bool depth_inverted = in.depth_inverted;

  void* p = s.caps;
  SetResource(p, "DLSSNR.Depth", depth);
  SetResource(p, "DLSSNR.MVec", motion);

  SetUInt(p, "DLSSNR.Enabled", 1u);
  SetUInt(p, "DLSSNR.Width", PassW());
  SetUInt(p, "DLSSNR.Height", PassH());
  SetUInt(p, "DLSSNR.DepthInverted", depth_inverted ? 1u : 0u);
  SetUInt(p, "DLSSNR.Reset", reset != 0 ? 1u : 0u);

  SetUInt(p, "DLSSNR.ColorSubrectBaseX", 0u);
  SetUInt(p, "DLSSNR.ColorSubrectBaseY", 0u);
  SetUInt(p, "DLSSNR.ColorSubrectWidth", PassW());
  SetUInt(p, "DLSSNR.ColorSubrectHeight", PassH());
  SetUInt(p, "DLSSNR.OutputSubrectBaseX", 0u);
  SetUInt(p, "DLSSNR.OutputSubrectBaseY", 0u);
  SetUInt(p, "DLSSNR.OutputSubrectWidth", PassW());
  SetUInt(p, "DLSSNR.OutputSubrectHeight", PassH());
  SetUInt(p, "DLSSNR.DepthSubrectBaseX", 0u);
  SetUInt(p, "DLSSNR.DepthSubrectBaseY", 0u);
  SetUInt(p, "DLSSNR.DepthSubrectWidth", in.depth_w != 0 ? in.depth_w : s.render_w);
  SetUInt(p, "DLSSNR.DepthSubrectHeight", in.depth_h != 0 ? in.depth_h : s.render_h);
  SetUInt(p, "DLSSNR.MVecSubrectBaseX", 0u);
  SetUInt(p, "DLSSNR.MVecSubrectBaseY", 0u);
  SetUInt(p, "DLSSNR.MVecSubrectWidth", s.render_w);
  SetUInt(p, "DLSSNR.MVecSubrectHeight", s.render_h);

  SetFloat(p, "DLSSNR.MVecScaleX", mv_scale_x);
  SetFloat(p, "DLSSNR.MVecScaleY", mv_scale_y);
  WriteTuning(p);

  // The pass chain: proxy -> pass 1 -> inter[0] -> pass 2 -> ... -> the last pass -> output.
  // Each intermediate answer goes to shader-resource state for the next pass to read, and every
  // one goes back to UNORDERED_ACCESS once the chain is done (or abandoned).
  const int passes = s.built_passes;
  const uint32_t n = ++s.eval_count;
  int result = 1;
  int ran = 0;
  for (int k = 0; k < passes && result == 1; ++k) {
    ID3D12Resource* in = (k == 0) ? s.proxy : s.inter[k - 1];
    ID3D12Resource* out = (k == passes - 1) ? s.output : s.inter[k];
    SetResource(p, "DLSSNR.Color", in);
    SetResource(p, "DLSSNR.Output", out);
    result = s.evaluate(cmd, k == 0 ? s.feature : s.extra[k - 1], s.caps);
    ran = k + 1;
    if (result == 1 && k < passes - 1)
      nr_compose::Barrier(cmd, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  // Intermediates 0..ran-2 were moved to shader-resource state (the failing pass's own output
  // never was); put them back.
  const int moved = (result == 1) ? passes - 1 : ran - 1;
  for (int k = 0; k < moved; ++k)
    nr_compose::Barrier(cmd, s.inter[k], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  if (n <= kFullLogs || n % kHeartbeatEvery == 0) {
    ngx_probe::Logf("nr-fwd: EvaluateFeature #%u x%d => 0x%x (%s)", n, ran, (unsigned)result,
                    ResultName(result));
  }

  if (result != 1) {
    AbortAfterPre(cmd, color);
    if (++s.fail_streak >= kMaxFailStreak) {
      ngx_probe::Logf("nr-fwd: %u consecutive failures (last: %s) -- pass disabled",
                      s.fail_streak, ResultName(result));
      s.enabled = false;
    }
    return;
  }
  s.fail_streak = 0;

  // Anchor the model's answer to the original and write the blend into the frame resource: the
  // game's output for the post-SR path (its post-processing reads it next), the game's DLSS-SR
  // color input for the pre-SR path (the original evaluate reads it next).
  nr_compose::RecordPost(cmd, color, s.color_copy, s.proxy, s.output, PassW(), PassH(),
                         s.eval_count, s.transfer, s.max_ratio, s.colour_strength, bridge,
                         (uint32_t)s.debug_view, motion, mv_scale_x, mv_scale_y, depth);

  // Pre-SR: hand the enhanced color back in the state NGX expects its input color in.
  if (PreSr())
    nr_compose::Barrier(cmd, color, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

inline bool PreparePass(ID3D12GraphicsCommandList* cmd, const NVSDK_NGX_Parameter* game_params) {
  if (s.gave_up || cmd == nullptr || game_params == nullptr) return false;
  if (!ApplyDlssGeometry()) return false;

  TickGraveyard();
  if (s.retire_pending.exchange(false, std::memory_order_relaxed))
    RetireFeature("model settings changed");
  const int pending = s.model_resolution.load(std::memory_order_relaxed);
  if (pending != s.dlss_resolution.load(std::memory_order_relaxed)) {
    s.dlss_resolution.store(pending, std::memory_order_relaxed);
    RetireFeature("model resolution changed");
    return false;
  }
  if (!s.enabled) {
    return false;
  }

  const int cur_source = s.source.load(std::memory_order_relaxed);
  if (cur_source != kSrcDlss) {
    if (cur_source == kSrcStandalone) RetireFeature("the game's DLSS took over from standalone");
    s.source.store(kSrcDlss, std::memory_order_relaxed);
    ngx_probe::Log("nr-fwd: source = the game's DLSS session");
  }

  if (!EnsureSetup(cmd)) {
    return false;
  }
  if (!EnsureFeature(cmd)) {
    return false;
  }
  return true;
}

inline bool PrepareFsrPass(ID3D12GraphicsCommandList* cmd) {
  static std::atomic<unsigned> reported{0};
  const auto skip = [](unsigned bit, const char* reason) {
    if ((reported.fetch_or(bit, std::memory_order_relaxed) & bit) == 0)
      ngx_probe::Logf("nr-fwd: FSR3 preparation skipped: %s", reason);
    return false;
  };
  if (s.gave_up || cmd == nullptr)
    return skip(1, s.gave_up ? "runner gave up (see earlier error)" : "null command list");
  if (s.source.load(std::memory_order_relaxed) != kSrcFsr) return false;
  if (s.frame_source.load(std::memory_order_relaxed) == kFsDlss ||
      s.frame_source.load(std::memory_order_relaxed) == kFsStandalone)
    return false;
  if (s.dlss_out_w == 0 || s.dlss_out_h == 0)
    return skip(2, "zero effective upscale dimensions");

  TickGraveyard();
  if (s.retire_pending.exchange(false, std::memory_order_relaxed))
    RetireFeature("model settings changed");
  const int pending = s.model_resolution.load(std::memory_order_relaxed);
  if (pending != s.dlss_resolution.load(std::memory_order_relaxed)) {
    s.dlss_resolution.store(pending, std::memory_order_relaxed);
    RetireFeature("model resolution changed");
    return false;
  }
  if (!s.enabled) {
    return false;
  }
  s.render_w = s.dlss_render_w;
  s.render_h = s.dlss_render_h;
  s.out_w = s.dlss_out_w;
  s.out_h = s.dlss_out_h;
  s.create_flags = s.dlss_create_flags;
  if (s.feature != nullptr && (s.feature_w != PassW() || s.feature_h != PassH()))
    RetireFeature("geometry changed");

  if (!EnsureSetup(cmd)) return skip(4, "NGX/forwarder setup not ready");
  if (!EnsureFeature(cmd)) return skip(8, "feature initialization not ready");
  return true;
}


// The game's DLSS guides are the model's guides in both modes: depth and motion vectors are
// render-resolution in the game's parameter block either way.
inline void ReadGuides(const NVSDK_NGX_Parameter* game_params, Inputs& in) {
  game_params->Get(NVSDK_NGX_Parameter_Depth, &in.depth);
  game_params->Get(NVSDK_NGX_Parameter_MotionVectors, &in.motion);
  game_params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &in.mv_scale_x);
  game_params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &in.mv_scale_y);
  int reset = 0;
  game_params->Get(NVSDK_NGX_Parameter_Reset, &reset);
  in.reset = reset != 0;
  in.depth_inverted = (s.create_flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
}

// Called after every successful game DLSS-SR evaluate, on the game's own command list.
// Model resolution Full (the default): the frame is the game's DLSS output, so the game's
// post-processing and tone-mapping consume the enhanced frame.
inline bool BeginDlssObservation(ID3D12GraphicsCommandList* cmd,
                                 const NVSDK_NGX_Parameter* game_params) {
  if (cmd == nullptr || game_params == nullptr) return false;
  {
    std::lock_guard<std::mutex> lock(dlss_geometry_mutex);
    if (!pending_dlss_geometry.valid) return false;
  }
  s.last_dlss_tick.store(GetTickCount64(), std::memory_order_relaxed);
  return true;
}

inline void OnDlssEvaluated(ID3D12GraphicsCommandList* cmd, const NVSDK_NGX_Parameter* game_params) {
  if (!BeginDlssObservation(cmd, game_params)) return;
  PassGuard guard(kSrcDlss);
  if (!guard) return;
  if (!RecordingModeMatches(false)) return;
  if (!PreparePass(cmd, game_params)) return;

  Inputs in;
  game_params->Get(NVSDK_NGX_Parameter_Output, &in.color);
  if (in.color != nullptr) in.output_format = in.color->GetDesc().Format;
  ReadGuides(game_params, in);
  Evaluate(cmd, in);

  NR_TEST_HOOK_AFTER_EVALUATE();
}

inline bool WantPrePass() { return PreSr(); }

// Called immediately before the game's own DLSS-SR evaluate, on the game's own command list.
// Model resolution Render: the frame is the DLSS-SR color input at render resolution, enhanced
// in place, so the game's DLSS-SR then upscales the enhanced frame.
inline void PreDlssEvaluate(ID3D12GraphicsCommandList* cmd, const NVSDK_NGX_Parameter* game_params) {
  if (!BeginDlssObservation(cmd, game_params)) return;
  PassGuard guard(kSrcDlss);
  if (!guard) return;
  if (!RecordingModeMatches(true)) return;
  ID3D12Resource* color_in = nullptr;
  if (game_params != nullptr) game_params->Get(NVSDK_NGX_Parameter_Color, &color_in);
  if (color_in != nullptr) {
    const D3D12_RESOURCE_DESC cd = color_in->GetDesc();
    if (cd.Width != s.color_in_w || cd.Height != s.color_in_h) {
      s.color_in_w = (unsigned)cd.Width;
      s.color_in_h = cd.Height;
      if (s.feature != nullptr && PreSr()) RetireFeature("DLSS-SR color input changed size");
    }
  }
  if (!PreparePass(cmd, game_params)) return;

  Inputs in;
  in.color = color_in;
  // Auto's encode decision keys off the DLSS-SR output, fetched here because this mode's frame
  // is the color input; the output resource is always in the game's parameter block.
  ID3D12Resource* dlss_output = nullptr;
  game_params->Get(NVSDK_NGX_Parameter_Output, &dlss_output);
  if (dlss_output != nullptr) in.output_format = dlss_output->GetDesc().Format;
  ReadGuides(game_params, in);
  Evaluate(cmd, in);

  NR_TEST_HOOK_AFTER_EVALUATE();
}

// --- standalone source ------------------------------------------------------
//
// A D3D12 game with no DLSS has no session to hook, so the frame comes from ReShade instead: the
// back buffer, ReShade's depth (Generic Depth) and a motion-vector effect's texture (iMMERSE
// Launchpad). The addon then has to start NGX itself, which a DLSS game's own init does otherwise.

constexpr ULONGLONG kDlssQuietMs = 3000;      // DLSS evaluates this recent claim the DLSS session
constexpr ULONGLONG kFsrQuietMs = 3000;       // FSR dispatches this recent claim the FSR3 session
constexpr ULONGLONG kStandaloneArmMs = 10000;  // Auto waits this long for a DLSS/FSR session first

// Per-frame source arbitration: pure in now/dlss/fsr/frame_source, except Auto's standalone
// arm (s.armed_tick, whose anchor the standalone gate sets). Force returns exactly its source
// or -1 -- a hard gate, never a maybe; Auto walks the maintainer's priority dlss -> fsr ->
// standalone, each stamp strictly inside its quiet window (0 = never seen, never fresh), and
// standalone only once armed. A timing argument per frame, not an enforced invariant: callers
// reach it from different threads with relaxed-atomic stamps.
inline int SourceChoice(ULONGLONG now, ULONGLONG dlss, ULONGLONG fsr, int frame_source) {
  const bool dlss_fresh = dlss != 0 && now - dlss < kDlssQuietMs;
  const bool fsr_fresh = fsr != 0 && now - fsr < kFsrQuietMs;
  switch (frame_source) {
    case kFsDlss: return dlss_fresh ? kSrcDlss : -1;
    case kFsFsr: return fsr_fresh ? kSrcFsr : -1;
    case kFsStandalone: return kSrcStandalone;  // the old Always: no quiet window, no arm
    default: break;  // kFsAuto (hand-edited junk is range-validated at load)
  }
  if (dlss_fresh) return kSrcDlss;
  if (fsr_fresh) return kSrcFsr;
  const ULONGLONG armed = s.armed_tick.load(std::memory_order_relaxed);
  if (armed != 0 && now - armed >= kStandaloneArmMs) return kSrcStandalone;
  return -1;
}

inline void OnFsrDispatchClaim(ULONGLONG now) {
  PassGuard guard(kSrcFsr);
  if (!guard) return;
  if (!s.enabled) return;
  const ULONGLONG fsr = s.last_fsr_tick.load(std::memory_order_relaxed);
  const ULONGLONG dlss = s.last_dlss_tick.load(std::memory_order_relaxed);
  if (SourceChoice(now, dlss, fsr, s.frame_source.load(std::memory_order_relaxed)) != kSrcFsr)
    return;
  const int cur_source = s.source.load(std::memory_order_relaxed);
  if (cur_source == kSrcFsr) return;
  if (cur_source == kSrcStandalone) RetireFeature("the game's FSR3 took over from standalone");
  s.source.store(kSrcFsr, std::memory_order_relaxed);
  ngx_probe::Log("nr-fwd: source = the game's FSR3 session");
}

using PFN_D3D12InitExt = int(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, int,
                                       const NVSDK_NGX_Parameter*);

// NVIDIA's sample application id: a game without DLSS has none of its own.
constexpr unsigned long long kStandaloneAppId = 231313132ull;

inline bool EnsureNgxCore(ID3D12GraphicsCommandList* cmd) {
  if (s.ngx_core != nullptr) return true;
  if (!ResolveGameDir()) return GiveUp("could not resolve the game directory");
  HMODULE core = GetModuleHandleA("_nvngx.dll");
  if (core == nullptr) core = LoadLibraryW(L"_nvngx.dll");
  if (core == nullptr) return GiveUp("standalone: _nvngx.dll could not be loaded (NVAPI off?)");
  auto init = (PFN_D3D12InitExt)GetProcAddress(core, "NVSDK_NGX_D3D12_Init_Ext");
  if (init == nullptr) return GiveUp("standalone: NVSDK_NGX_D3D12_Init_Ext not exported");
  ID3D12Device* device = nullptr;
  if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    return GiveUp("standalone: could not reach the device");
  const int r = init(kStandaloneAppId, s.game_dir, device, (int)NVSDK_NGX_Version_API, nullptr);
  device->Release();
  ngx_probe::Logf("nr-fwd: NGX core init => 0x%x (%s)", (unsigned)r, ResultName(r));
  if (r != 1) return GiveUp("standalone: NGX core init failed");
  s.ngx_core = core;
  return true;
}

// The UAV-capable twin of a back-buffer format: same copy family, no sRGB view.
inline DXGI_FORMAT UavTwin(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
  }
}

inline ID3D12Resource* MakeUavTexture(ID3D12Device* device, UINT w, UINT h, DXGI_FORMAT format) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = w;
  desc.Height = h;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ID3D12Resource* r = nullptr;
  const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                     nullptr, IID_PPV_ARGS(&r));
  if (FAILED(hr)) {
    ngx_probe::Logf("nr-fwd: standalone texture %ux%u format %u failed: 0x%08x", w, h,
                    (unsigned)format, (unsigned)hr);
    return nullptr;
  }
  return r;
}

// The textures the pass works on: sa_frame at the model's size (work_w x work_h), plus, when that
// is below the back buffer's size, the reduced-resolution set, or, when depth has to be scaled up
// to it, sa_depth.
inline bool EnsureStandaloneFrame(ID3D12GraphicsCommandList* cmd, ID3D12Resource* back_buffer,
                                  UINT work_w, UINT work_h, bool depth_up) {
  const D3D12_RESOURCE_DESC bb = back_buffer->GetDesc();
  const DXGI_FORMAT format = UavTwin(bb.Format);
  const bool reduced = work_w != (UINT)bb.Width || work_h != bb.Height;
  if (s.sa_frame != nullptr && s.sa_format == format && s.out_w == work_w && s.out_h == work_h &&
      s.sa_reduced == reduced && (s.sa_depth != nullptr) == depth_up &&
      (!reduced || (s.sa_full != nullptr && s.sa_full->GetDesc().Width == bb.Width &&
                    s.sa_full->GetDesc().Height == bb.Height)))
    return true;
  if (s.feature != nullptr) RetireFeature("standalone frame changed size or format");
  const bool idle = StandaloneIdle(2000);
  for (ID3D12Resource** r :
       {&s.sa_frame, &s.sa_given, &s.sa_delta, &s.sa_mv, &s.sa_full, &s.sa_depth}) {
    if (*r != nullptr) Drop(*r, nullptr, idle);
    *r = nullptr;
  }
  s.out_w = s.render_w = work_w;
  s.out_h = s.render_h = work_h;
  s.create_flags = 0;
  s.sa_reduced = reduced;

  ID3D12Device* device = nullptr;
  if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) return false;
  bool ok = (s.sa_frame = MakeUavTexture(device, work_w, work_h, format)) != nullptr;
  if (ok && reduced) {
    ok = (s.sa_given = MakeUavTexture(device, work_w, work_h, format)) != nullptr &&
         (s.sa_delta = MakeUavTexture(device, work_w, work_h, DXGI_FORMAT_R16G16B16A16_FLOAT)) != nullptr &&
         (s.sa_mv = MakeUavTexture(device, work_w, work_h, DXGI_FORMAT_R16G16_FLOAT)) != nullptr &&
         (s.sa_full = MakeUavTexture(device, (UINT)bb.Width, bb.Height, format)) != nullptr;
  }
  if (ok && depth_up)
    ok = (s.sa_depth = MakeUavTexture(device, work_w, work_h, DXGI_FORMAT_R32_FLOAT)) != nullptr;
  device->Release();
  if (!ok) return false;
  s.sa_format = format;
  ngx_probe::Logf("nr-fwd: standalone frame %ux%u format=%u (back buffer %llux%u format %u)%s",
                  s.out_w, s.out_h, (unsigned)format, (unsigned long long)bb.Width, bb.Height,
                  (unsigned)bb.Format,
                  reduced ? ", model at render resolution" : depth_up ? ", depth scaled up" : "");
  return true;
}

// Whether standalone may run now, with the reason kept for the overlay. The source
// arbitration itself is SourceChoice's; this adds standalone's own preconditions (the
// bridge, a resident nvngx_dlss.dll, the arm window's anchor) and the status strings.
inline bool StandaloneAllowed() {
  const ULONGLONG now = GetTickCount64();
  // The arm window starts on the first ask, as StandaloneAllowed's old `start` did; the
  // choice below (and Force standalone's bypass of it) reads this anchor.
  ULONGLONG armed = s.armed_tick.load(std::memory_order_relaxed);
  if (armed == 0) {
    s.armed_tick.store(armed = now, std::memory_order_relaxed);
  }
  const int fs = s.frame_source.load(std::memory_order_relaxed);
  // Hard gates: the excluded hooks' bodies stay inert, so their stamps never arrive
  // either -- Force DLSS/FSR3 means standalone never arms (what old Off meant, for both).
  if (fs == kFsDlss || fs == kFsFsr) {
    std::snprintf(s.sa_status, sizeof(s.sa_status), "off (source forced to %s)",
                  fs == kFsDlss ? "DLSS" : "FSR3");
    return false;
  }
  const ULONGLONG last_dlss = s.last_dlss_tick.load(std::memory_order_relaxed);
  const ULONGLONG last_fsr = s.last_fsr_tick.load(std::memory_order_relaxed);
  // The old Always: no bridge/nvngx/arm checks, and it defers only to a fresh DLSS
  // session -- never to FSR. Under Force standalone the FSR pass is excluded (the
  // SourceChoice force branch claims kSrcStandalone unconditionally), so an FSR stamp
  // deferring here would leave an FSR-active game with no frame source at all. The DLSS
  // path is not gated under Force standalone, so a fresh evaluate can still own the frame.
  if (fs == kFsStandalone) {
    if (last_dlss != 0 && now - last_dlss < kDlssQuietMs) {
      std::snprintf(s.sa_status, sizeof(s.sa_status),
                    "standing by: a DLSS session is the source");
      return false;
    }
    return true;
  }
  const int choice = SourceChoice(now, last_dlss, last_fsr, fs);
  if (choice == kSrcDlss) {
    std::snprintf(s.sa_status, sizeof(s.sa_status), "standing by: a DLSS session is the source");
    return false;
  }
  if (choice == kSrcFsr) {
    std::snprintf(s.sa_status, sizeof(s.sa_status), "waiting: FSR3 session active");
    return false;
  }
  // Auto from here. These come before the arm the way StandaloneAllowed always ordered
  // them: a bridge or a DLSS-loaded game blocks standalone outright, not merely for the
  // arm window.
  if (GetModuleHandleA("dlss5-bridge.addon64") != nullptr) {
    std::snprintf(s.sa_status, sizeof(s.sa_status),
                  "standing by: dlss5-bridge is installed and feeds this add-on");
    return false;
  }
  // nvngx_dlss.dll resident before standalone started NGX means the game loaded it; after
  // that, standalone's own NGX init may have loaded it, so it says nothing.
  if (s.last_dlss_tick.load(std::memory_order_relaxed) != 0 ||
      (s.ngx_core == nullptr && GetModuleHandleA("nvngx_dlss.dll") != nullptr)) {
    std::snprintf(s.sa_status, sizeof(s.sa_status),
                  "standing by: this game has DLSS (set Source to Force standalone to force it)");
    return false;
  }
  if (choice == kSrcStandalone) return true;  // both quiet, arm window elapsed
  // choice == -1: both sessions quiet and the arm window still running. The anchor
  // above guarantees armed != 0, and SourceChoice only reaches -1 here while inside
  // kStandaloneArmMs, so the countdown is well-formed (1..10 s, rounded up).
  const ULONGLONG arm_left = kStandaloneArmMs - (now - armed);
  std::snprintf(s.sa_status, sizeof(s.sa_status),
                "waiting: no FSR3 or DLSS activity (standalone armed in %us)",
                (unsigned)((arm_left + 999) / 1000));
  return false;
}

// The game's queue (or, for D3D11, its context) waits on the GPU for each standalone frame. If
// the D3D12 side stops finishing frames, the game would wait forever -- with its CPU stuck in
// Present, where no frame callback can notice. A watchdog thread notices instead: after 3 s
// without progress it signals the fences from the CPU, which releases every wait, and switches
// standalone off.
inline void (*rescue_hook)() = nullptr;  // the D3D11 transport releases its own fence here
inline std::atomic<UINT64> sa_submitted{0};

inline DWORD WINAPI StandaloneWatchdog(LPVOID) {
  // A stall is no progress at all, not merely being behind: while frames flow the GPU is always
  // a frame or two behind the newest submission.
  ULONGLONG stalled_since = 0;
  UINT64 last_completed = 0;
  for (;;) {
    Sleep(250);
    if (s.sa_fence_out == nullptr) continue;
    const UINT64 expected = sa_submitted.load(std::memory_order_relaxed);
    const UINT64 completed = s.sa_fence_out->GetCompletedValue();
    if (completed >= expected || completed != last_completed) {
      last_completed = completed;
      stalled_since = 0;
      continue;
    }
    const ULONGLONG now = GetTickCount64();
    if (stalled_since == 0) {
      stalled_since = now;
      continue;
    }
    if (now - stalled_since < 3000) continue;
    ngx_probe::Logf("nr-fwd: standalone frame %llu has not finished in 3 s -- releasing the "
                    "game's waits and switching standalone off", (unsigned long long)expected);
    s.gave_up = true;
    std::snprintf(s.sa_status, sizeof(s.sa_status),
                  "stopped: the GPU stopped finishing frames (see ReShade.log)");
    s.sa_fence_out->Signal(expected);
    if (rescue_hook != nullptr) rescue_hook();
    return 0;
  }
}

inline bool EnsureStandaloneQueue(ID3D12Resource* back_buffer) {
  if (s.sa_list != nullptr) return true;
  ID3D12Device* device = nullptr;  // the proxy: ReShade's hooked GetDevice returns it
  if (FAILED(back_buffer->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    return GiveUp("standalone: could not reach the device");
  D3D12_COMMAND_QUEUE_DESC qd = {};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  bool ok = SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&s.sa_queue)));
  for (auto& a : s.sa_alloc)
    ok = ok && SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        IID_PPV_ARGS(&a)));
  ok = ok && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.sa_alloc[0],
                                                 nullptr, IID_PPV_ARGS(&s.sa_list)));
  ok = ok && SUCCEEDED(s.sa_list->Close());
  ok = ok && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.sa_fence_in)));
  ok = ok && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.sa_fence_out)));
  device->Release();
  s.sa_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!ok || s.sa_event == nullptr) return GiveUp("standalone: queue/list/fence creation failed");
  ngx_probe::Log("nr-fwd: standalone queue ready (proxy device)");
  if (HANDLE t = CreateThread(nullptr, 0, StandaloneWatchdog, nullptr, 0, nullptr)) CloseHandle(t);
  return true;
}

// Called by the addon right after the motion-vector effect rendered: the back buffer is a render
// target, the depth and motion textures are shader resources. Records the pass on the standalone
// list; the addon then flushes ReShade's own list and calls SubmitStandalone, which orders this
// list after everything ReShade recorded so far and before anything it records next.
// bb_state is the state the frame resource is in on entry and is left in: RENDER_TARGET for a
// D3D12 game's back buffer, COMMON for the D3D11 transport's shared texture.
inline bool OnStandaloneFrame(ID3D12Resource* back_buffer, ID3D12Resource* depth,
                              ID3D12Resource* motion, bool depth_inverted, float depth_scale_x,
                              float depth_scale_y,
                              D3D12_RESOURCE_STATES bb_state = D3D12_RESOURCE_STATE_RENDER_TARGET) {
  if (s.gave_up || back_buffer == nullptr) return false;
  if (!StandaloneAllowed()) return false;
  if (depth == nullptr || motion == nullptr) {
    std::snprintf(s.sa_status, sizeof(s.sa_status), "waiting: %s",
                  depth == nullptr ? "no depth buffer bound (Generic Depth)"
                                   : "no motion vectors (enable iMMERSE Launchpad)");
    return false;
  }
  const D3D12_RESOURCE_DESC bb = back_buffer->GetDesc();
  const D3D12_RESOURCE_DESC dd = depth->GetDesc();
  // Depth smaller than the screen at the same aspect ratio: the game renders its scene below the
  // output resolution. Model resolution Render runs the model at the scene's resolution (reduced
  // mode); Full scales depth up to the screen instead.
  const double aspect_error =
      (double)dd.Width * bb.Height - (double)dd.Height * (double)bb.Width;
  const bool same_aspect =
      aspect_error * aspect_error <= 0.0004 * ((double)bb.Width * bb.Height) *
                                         ((double)bb.Width * bb.Height);
  if (dd.Width > bb.Width || dd.Height > bb.Height || !same_aspect) {
    std::snprintf(s.sa_status, sizeof(s.sa_status),
                  "waiting: depth is %llux%u, the screen %llux%u -- pick the screen-sized buffer",
                  (unsigned long long)dd.Width, dd.Height, (unsigned long long)bb.Width,
                  bb.Height);
    return false;
  }

  PassGuard guard(kSrcStandalone);
  if (!guard) return false;
  TickGraveyard();
  if (s.retire_pending.exchange(false, std::memory_order_relaxed))
    RetireFeature("model settings changed");
  if (!s.enabled) {
    std::snprintf(s.sa_status, sizeof(s.sa_status), "paused (F10)");
    return false;
  }

  const int cur_source = s.source.load(std::memory_order_relaxed);
  if (cur_source != kSrcStandalone) {
    if (cur_source == kSrcDlss) RetireFeature("switching to standalone");
    s.source.store(kSrcStandalone, std::memory_order_relaxed);
    ngx_probe::Log("nr-fwd: source = standalone (back buffer + ReShade depth + motion vectors)");
  }

  if (!EnsureStandaloneQueue(back_buffer)) {
    return false;
  }

  // Reuse the allocator three frames back once the GPU is past it.
  const int slot = (int)(s.sa_value % 3);
  if (s.sa_fence_out->GetCompletedValue() < s.sa_alloc_value[slot]) {
    s.sa_fence_out->SetEventOnCompletion(s.sa_alloc_value[slot], s.sa_event);
    // Still in flight: skip this frame rather than reset an allocator the GPU is using. A stall
    // that lasts is the watchdog's to handle.
    if (WaitForSingleObject(s.sa_event, 1000) != WAIT_OBJECT_0) {
      return false;
    }
  }
  s.sa_alloc[slot]->Reset();
  ID3D12GraphicsCommandList* cmd = s.sa_list;
  cmd->Reset(s.sa_alloc[slot], nullptr);
  s.sa_pending = true;  // from here on the list is submitted, even if the pass bails out

  if (!EnsureNgxCore(cmd)) {
    return true;
  }
  const bool smaller = dd.Width != bb.Width || dd.Height != bb.Height;
  const bool render_res = smaller && s.model_resolution.load(std::memory_order_relaxed) == kResRender;
  const bool depth_up = smaller && !render_res;
  if (!EnsureStandaloneFrame(cmd, back_buffer, render_res ? (UINT)dd.Width : (UINT)bb.Width,
                             render_res ? dd.Height : bb.Height, depth_up)) {
    return true;
  }
  if (!EnsureSetup(cmd)) {
    return true;
  }
  if (!EnsureFeature(cmd)) {  // created this frame; it recorded init work
    return true;
  }

  using nr_compose::Barrier;
  const UINT full_w = (UINT)bb.Width, full_h = bb.Height;
  const uint32_t frame = (uint32_t)s.sa_value;
  constexpr D3D12_RESOURCE_STATES kGuide =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  if (s.sa_depth != nullptr) {
    // Full with a smaller depth buffer: depth up to the screen, the rest as usual.
    nr_compose::ScalePass(cmd, nr_compose::kScaleDepth, depth, depth, s.sa_depth, full_w, full_h,
                          frame);
    Barrier(cmd, s.sa_depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kGuide);
    depth = s.sa_depth;
  }
  if (s.sa_reduced) {
    // Back buffer -> the model's size, kept aside as given; motion vectors to the same grid.
    Barrier(cmd, back_buffer, bb_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    nr_compose::ScalePass(cmd, nr_compose::kScaleDown, back_buffer, back_buffer, s.sa_frame,
                          s.out_w, s.out_h, frame, s.sa_format);
    nr_compose::ScalePass(cmd, nr_compose::kScaleMotion, motion, motion, s.sa_mv, s.out_w,
                          s.out_h, frame);
    Barrier(cmd, s.sa_frame, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmd, s.sa_given, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(s.sa_given, s.sa_frame);
    Barrier(cmd, s.sa_given, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(cmd, s.sa_frame, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(cmd, s.sa_mv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  } else {
    // Back buffer -> the UAV-capable frame copy the pass works on in place.
    Barrier(cmd, back_buffer, bb_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmd, s.sa_frame, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(s.sa_frame, back_buffer);
    Barrier(cmd, s.sa_frame, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(cmd, back_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  }

  // Launchpad's motion vectors are in UV units; NGX scales them to pixels.
  Inputs in;
  in.color = s.sa_frame;
  in.depth = depth;
  in.motion = s.sa_reduced ? s.sa_mv : motion;
  in.mv_scale_x = (float)s.out_w * (s.sa_flip_x ? -1.0f : 1.0f);
  in.mv_scale_y = (float)s.out_h * (s.sa_flip_y ? -1.0f : 1.0f);
  in.depth_inverted = depth_inverted;
  // A game rendering below output resolution (FSR, TSR, a resolution scale) draws depth into the
  // top-left of a screen-sized buffer; ReShade's RESHADE_DEPTH_INPUT_*_SCALE describe that, and
  // the model gets the same region.
  if (depth_scale_x > 1.0f) in.depth_w = (unsigned)((float)s.out_w / depth_scale_x + 0.5f);
  if (depth_scale_y > 1.0f) in.depth_h = (unsigned)((float)s.out_h / depth_scale_y + 0.5f);
  Evaluate(cmd, in);
  if (s.sa_depth != nullptr)
    Barrier(cmd, s.sa_depth, kGuide, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  if (s.sa_reduced) {
    // The model's change, scaled up onto the full-resolution frame.
    constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Barrier(cmd, s.sa_mv,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(cmd, s.sa_frame, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
    Barrier(cmd, s.sa_given, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
    nr_compose::ScalePass(cmd, nr_compose::kScaleDelta, s.sa_frame, s.sa_given, s.sa_delta,
                          s.out_w, s.out_h, frame);
    Barrier(cmd, s.sa_delta, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
    nr_compose::ScalePass(cmd, nr_compose::kScaleUp, back_buffer, s.sa_delta, s.sa_full, full_w,
                          full_h, frame, s.sa_format);
    Barrier(cmd, s.sa_delta, kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(cmd, s.sa_given, kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(cmd, s.sa_frame, kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(cmd, s.sa_full, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmd, back_buffer, kRead, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(back_buffer, s.sa_full);
    Barrier(cmd, s.sa_full, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(cmd, back_buffer, D3D12_RESOURCE_STATE_COPY_DEST, bb_state);
  } else {
    // The result (or, if the pass bailed out, the untouched copy) goes back to the back buffer.
    Barrier(cmd, s.sa_frame, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyResource(back_buffer, s.sa_frame);
    Barrier(cmd, s.sa_frame, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(cmd, back_buffer, D3D12_RESOURCE_STATE_COPY_DEST, bb_state);
  }
  if (s.sa_reduced)
    std::snprintf(s.sa_status, sizeof(s.sa_status), "running, %ux%u (model at %ux%u)", full_w,
                  full_h, s.out_w, s.out_h);
  else
    std::snprintf(s.sa_status, sizeof(s.sa_status), "running, %ux%u", s.out_w, s.out_h);
  return true;
}

// Runs the recorded list between ReShade's flushed work and whatever the game queue does next.
inline void SubmitStandalone(ID3D12CommandQueue* game_queue) {
  if (!s.sa_pending) return;
  s.sa_pending = false;
  if (FAILED(s.sa_list->Close())) {
    GiveUp("standalone: command list failed to close");
    return;
  }
  const UINT64 v = ++s.sa_value;
  s.sa_alloc_value[(v - 1) % 3] = v;
  game_queue->Signal(s.sa_fence_in, v);
  s.sa_queue->Wait(s.sa_fence_in, v);
  ID3D12CommandList* lists[] = {s.sa_list};
  s.sa_queue->ExecuteCommandLists(1, lists);
  s.sa_queue->Signal(s.sa_fence_out, v);
  sa_submitted.store(v, std::memory_order_relaxed);
  game_queue->Wait(s.sa_fence_out, v);
}

// The Vulkan transport's variant: the CPU orders the two APIs. The game's queue was drained
// before the pass was recorded; this runs the pass and waits for it, so the copy back that the
// caller records next sees the result. Returns false if nothing ran or it did not finish.
inline bool SubmitStandaloneAndWait(DWORD timeout_ms) {
  if (!s.sa_pending) return false;
  s.sa_pending = false;
  if (FAILED(s.sa_list->Close())) {
    GiveUp("standalone: command list failed to close");
    return false;
  }
  const UINT64 v = ++s.sa_value;
  s.sa_alloc_value[(v - 1) % 3] = v;
  ID3D12CommandList* lists[] = {s.sa_list};
  s.sa_queue->ExecuteCommandLists(1, lists);
  s.sa_queue->Signal(s.sa_fence_out, v);
  sa_submitted.store(v, std::memory_order_relaxed);
  if (s.sa_fence_out->GetCompletedValue() >= v) return true;
  s.sa_fence_out->SetEventOnCompletion(v, s.sa_event);
  return WaitForSingleObject(s.sa_event, timeout_ms) == WAIT_OBJECT_0;
}

// The D3D11 transport's variant: the list waits on a fence the D3D11 context signals once its
// copies are in, and signals the same fence when the result is ready for it to copy back.
// Returns false if nothing was recorded this frame, so the caller must not wait.
inline bool SubmitStandaloneShared(ID3D12Fence* shared, UINT64 wait_value, UINT64 signal_value) {
  if (!s.sa_pending) return false;
  s.sa_pending = false;
  if (FAILED(s.sa_list->Close())) {
    GiveUp("standalone: command list failed to close");
    return false;
  }
  const UINT64 v = ++s.sa_value;
  s.sa_alloc_value[(v - 1) % 3] = v;
  s.sa_queue->Wait(shared, wait_value);
  ID3D12CommandList* lists[] = {s.sa_list};
  s.sa_queue->ExecuteCommandLists(1, lists);
  s.sa_queue->Signal(s.sa_fence_out, v);
  s.sa_queue->Signal(shared, signal_value);
  sa_submitted.store(v, std::memory_order_relaxed);
  return true;
}

}  // namespace nr_runner
