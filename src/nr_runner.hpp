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

#include <windows.h>

#include <bcrypt.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>

#include "nr_compose.hpp"

#pragma comment(lib, "bcrypt")   // SHA-256 of the model DLL
#pragma comment(lib, "version")  // its file-version resource

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

  // Colour bridge, mirroring the RenoDX DLSS5 addon's Encoding and Diffuse White (nits).
  // encoding: 0 Auto, 1 Linear BT.709, 2 sRGB, 3 BT.2100 PQ, 4 scRGB, 5 scRGB-nl,
  // 6 Measured (the original fork's auto-exposure white point + Reinhard).
  int encoding = 0;
  bool diffuse_white_override = false;
  float diffuse_white_nits = 100.0f;

  // Measured mode only: the log-average luminance, EMA-smoothed; -1 until the first reading
  // lands. wp_scale is the user's multiplier on top.
  float wp_ema = -1.0f;
  float wp_scale = 1.0f;

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

  // Where the frame comes from. Standalone runs off the back buffer, ReShade's depth and a
  // motion-vector effect, for a D3D12 game with no DLSS session to hook.
  int source = 0;                  // Source
  ULONGLONG last_dlss_tick = 0;    // last DLSS-SR evaluate seen (GetTickCount64)
  int standalone_mode = 0;         // 0 Auto, 1 Always, 2 Off
  bool sa_flip_x = false, sa_flip_y = false;  // motion-vector direction, standalone only
  bool temporal = true;            // false resets the model's history every frame (diagnostic)
  ID3D12Resource* sa_frame = nullptr;  // UAV-capable copy of the back buffer
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

enum Source : int { kSrcNone = 0, kSrcDlss = 1, kSrcStandalone = 2 };
enum StandaloneMode : int { kSaAuto = 0, kSaAlways = 1, kSaOff = 2 };

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

// Test-only e2e levers (NR_TEST_HOOK_* macros); expands to nothing in release builds.
#include "nr_test_hooks.hpp"

// --- model identity -------------------------------------------------------

// Feature 18 only runs stably with the exact model build it was tested against. A mismatched
// nvngx_dlssnr.dll does not fail at the API -- it reports Success on every evaluate and crashes
// the game minutes later on an unrelated thread. So the model's version and SHA-256 are logged at
// init (on a background thread; the file is ~160MB), with a warning when it isn't the tested
// build, so every user report identifies the model that was actually running.
constexpr char kTestedModelSha256[] =
    "e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e";

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
  bool hashed = false;
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
      if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
        for (int i = 0; i < 32; ++i) std::snprintf(sha_hex + i * 2, 3, "%02x", digest[i]);
        hashed = true;
      }
    }
    if (hash != nullptr) BCryptDestroyHash(hash);
    if (alg != nullptr) BCryptCloseAlgorithmProvider(alg, 0);
    std::free(buf);
    CloseHandle(file);
  }

  ngx_probe::Logf("nr-fwd: model nvngx_dlssnr.dll -- version %s, %llu bytes, sha256=%s", version,
                  total, sha_hex);
  if (hashed && std::strcmp(sha_hex, kTestedModelSha256) != 0) {
    ngx_probe::Warnf(
        "nr-fwd: this nvngx_dlssnr.dll is NOT the tested model build -- other versions have "
        "crashed the game mid-session with no API error; tested sha256=%s",
        kTestedModelSha256);
  }
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
  if (s.source != kSrcStandalone || s.sa_fence_out == nullptr || s.sa_event == nullptr)
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
  s.dlss_render_w = w;
  s.dlss_render_h = h;
  s.dlss_out_w = ow;
  s.dlss_out_h = oh;
  s.dlss_create_flags = flags;
  if (s.source == kSrcStandalone) return;  // applied when the DLSS source takes over
  s.render_w = w;
  s.render_h = h;
  s.out_w = ow;
  s.out_h = oh;
  s.create_flags = flags;
  if (s.feature != nullptr && (s.feature_w != ow || s.feature_h != oh))
    RetireFeature("geometry changed");
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
    // The driver core's capability block. The game's own DLSS already initialised the core, so this
    // is a read of existing state, not an init.
    HMODULE nvngx = GetModuleHandleA("_nvngx.dll");
    if (nvngx == nullptr) return GiveUp("_nvngx.dll not resident");
    auto get_caps =
        (PFN_GetCapabilityParams)GetProcAddress(nvngx, "NVSDK_NGX_D3D12_GetCapabilityParameters");
    if (get_caps == nullptr) return GiveUp("GetCapabilityParameters not exported by _nvngx.dll");
    if (get_caps(&s.caps) != NVSDK_NGX_Result_Success || s.caps == nullptr) {
      s.caps = nullptr;
      return GiveUp("the NGX core refused its capability parameters");
    }
    DiscoverFloatSlot(s.caps);
  }

  return true;
}

// Three display-resolution textures in the game's own DLSS output format (accepted by the model
// in milestone 5a): the model's answer, the copied-aside original, and the encoded proxy.
inline bool CreateTextures(ID3D12GraphicsCommandList* cmd, ID3D12Resource* game_output) {
  if (s.output != nullptr) return true;
  ID3D12Device* device = nullptr;
  if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    return GiveUp("could not reach the device from the command list");

  const D3D12_RESOURCE_DESC game_desc = game_output->GetDesc();

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = s.out_w;
  desc.Height = s.out_h;
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
  s.output_format = game_desc.Format;
  ngx_probe::Logf("nr-fwd: textures %ux%u format=%u (answer, original, proxy)", s.out_w, s.out_h,
                  (unsigned)s.output_format);
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

enum Encoding : int {
  kEncAuto = 0,
  kEncLinear = 1,
  kEncSrgb = 2,
  kEncPq = 3,
  kEncScrgb = 4,
  kEncScrgbNl = 5,
  kEncMeasured = 6,
};

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
// thread while the evaluate thread owns the feature and textures, so only a flag is set here;
// the retire itself executes at the top of the next OnDlssEvaluated, on the owning thread.
inline void ApplyModelSettings() { s.retire_pending.store(true, std::memory_order_relaxed); }

// Writes the create-time block and builds one feature; nullptr on failure.
inline void* CreateOne(ID3D12GraphicsCommandList* cmd, int pass) {
  void* p = s.caps;
  SetUInt(p, "DLSSNR.Enabled", 1u);
  SetUInt(p, "DLSSNR.Width", s.out_w);
  SetUInt(p, "DLSSNR.Height", s.out_h);
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
  desc.Width = s.out_w;
  desc.Height = s.out_h;
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
  s.feature_w = s.out_w;
  s.feature_h = s.out_h;
  // Evaluate from the next frame on, so the init work this create recorded executes first.
  return false;
}

// --- the per-frame pass ---------------------------------------------------

// If the model fails after RecordPre already ran, put every state back so the frame proceeds
// with the original picture untouched.
inline void AbortAfterPre(ID3D12GraphicsCommandList* cmd, ID3D12Resource* game_out) {
  nr_compose::Barrier(cmd, game_out, D3D12_RESOURCE_STATE_COPY_SOURCE,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
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
  float mv_scale_x = 1.0f, mv_scale_y = 1.0f;
  bool reset = false;
  bool depth_inverted = false;
  unsigned depth_w = 0, depth_h = 0;  // the depth region actually rendered; 0 = render size
};

inline void Evaluate(ID3D12GraphicsCommandList* cmd, const Inputs& in) {
  ID3D12Resource* color = in.color;
  ID3D12Resource* depth = in.depth;
  ID3D12Resource* motion = in.motion;
  if (color == nullptr || depth == nullptr || motion == nullptr) return;

  if (!CreateTextures(cmd, color)) return;

  // The colour bridge for this frame. Measured mode folds the latest luminance reading (a few
  // frames old, absorbed by the EMA) into the smoothed white point; every other encoding uses a
  // fixed diffuse white, so nothing about the picture the model sees moves with the scene.
  const int encoding = ResolveEncoding(s.encoding, s.output_format);
  nr_compose::Bridge bridge = {};
  if (encoding == kEncMeasured) {
    const float raw_wp = nr_compose::ReadWhitePoint(s.eval_count);
    if (raw_wp > 0.0f && raw_wp == raw_wp) {
      s.wp_ema = (s.wp_ema <= 0.0f) ? raw_wp : s.wp_ema + 0.05f * (raw_wp - s.wp_ema);
    }
    float wp = (s.wp_ema > 0.0f) ? s.wp_ema : 1.0f;
    bridge.white_point = (wp < 0.01f ? 0.01f : (wp > 1000.0f ? 1000.0f : wp)) * s.wp_scale;
    bridge.encoding = kEncLinear;
    bridge.curve = 1;  // CURVE_REINHARD
  } else {
    const float nits =
        s.diffuse_white_override ? s.diffuse_white_nits : AutoDiffuseWhiteNits(encoding);
    bridge.white_point = DiffuseWhiteToSource(encoding, nits < 1.0f ? 1.0f : nits);
    bridge.encoding = (uint32_t)encoding;
    bridge.curve = 0;  // CURVE_KNEE
  }
  s.active_encoding = encoding;
  s.active_white_point = bridge.white_point;

  // Copy the original aside, encode the display-referred proxy, record the measurement.
  if (!nr_compose::RecordPre(cmd, color, s.color_copy, s.proxy, s.out_w, s.out_h, s.eval_count,
                             bridge)) {
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
  SetUInt(p, "DLSSNR.Width", s.out_w);
  SetUInt(p, "DLSSNR.Height", s.out_h);
  SetUInt(p, "DLSSNR.DepthInverted", depth_inverted ? 1u : 0u);
  SetUInt(p, "DLSSNR.Reset", reset != 0 ? 1u : 0u);

  SetUInt(p, "DLSSNR.ColorSubrectBaseX", 0u);
  SetUInt(p, "DLSSNR.ColorSubrectBaseY", 0u);
  SetUInt(p, "DLSSNR.ColorSubrectWidth", s.out_w);
  SetUInt(p, "DLSSNR.ColorSubrectHeight", s.out_h);
  SetUInt(p, "DLSSNR.OutputSubrectBaseX", 0u);
  SetUInt(p, "DLSSNR.OutputSubrectBaseY", 0u);
  SetUInt(p, "DLSSNR.OutputSubrectWidth", s.out_w);
  SetUInt(p, "DLSSNR.OutputSubrectHeight", s.out_h);
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

  // Anchor the model's answer to the original and write the blend into the game's output, which
  // its post-processing reads next.
  nr_compose::RecordPost(cmd, color, s.color_copy, s.proxy, s.output, s.out_w, s.out_h,
                         s.eval_count, s.transfer, s.max_ratio, s.colour_strength, bridge,
                         (uint32_t)s.debug_view, motion, mv_scale_x, mv_scale_y, depth);
}

// Called after every successful game DLSS-SR evaluate, on the game's own command list.
inline void OnDlssEvaluated(ID3D12GraphicsCommandList* cmd, const NVSDK_NGX_Parameter* game_params) {
  if (s.gave_up || cmd == nullptr || game_params == nullptr) return;
  if (s.dlss_out_w == 0 || s.dlss_out_h == 0) return;  // no DLSS-SR create seen yet
  s.last_dlss_tick = GetTickCount64();  // even while disabled: standalone must stay out

  TickGraveyard();
  if (s.retire_pending.exchange(false, std::memory_order_relaxed))
    RetireFeature("model settings changed");
  if (!s.enabled) return;

  if (s.source != kSrcDlss) {
    if (s.source == kSrcStandalone) RetireFeature("the game's DLSS took over from standalone");
    s.source = kSrcDlss;
    s.render_w = s.dlss_render_w;
    s.render_h = s.dlss_render_h;
    s.out_w = s.dlss_out_w;
    s.out_h = s.dlss_out_h;
    s.create_flags = s.dlss_create_flags;
    ngx_probe::Log("nr-fwd: source = the game's DLSS session");
  }

  if (!EnsureSetup(cmd)) return;
  if (!EnsureFeature(cmd)) return;

  // The game's DLSS output is the frame; the model sees its encoded proxy, and the game's DLSS
  // guides are the model's guides.
  Inputs in;
  game_params->Get(NVSDK_NGX_Parameter_Output, &in.color);
  game_params->Get(NVSDK_NGX_Parameter_Depth, &in.depth);
  game_params->Get(NVSDK_NGX_Parameter_MotionVectors, &in.motion);
  game_params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &in.mv_scale_x);
  game_params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &in.mv_scale_y);
  int reset = 0;
  game_params->Get(NVSDK_NGX_Parameter_Reset, &reset);
  in.reset = reset != 0;
  in.depth_inverted = (s.create_flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
  Evaluate(cmd, in);

  NR_TEST_HOOK_AFTER_EVALUATE();
}

// --- standalone source ------------------------------------------------------
//
// A D3D12 game with no DLSS has no session to hook, so the frame comes from ReShade instead: the
// back buffer, ReShade's depth (Generic Depth) and a motion-vector effect's texture (iMMERSE
// Launchpad). The addon then has to start NGX itself, which a DLSS game's own init does otherwise.

constexpr ULONGLONG kDlssQuietMs = 3000;      // DLSS evaluates this recent keep standalone out
constexpr ULONGLONG kStandaloneArmMs = 10000;  // Auto waits this long for a DLSS session first

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
  ngx_probe::Logf("nr-fwd: standalone NGX core init => 0x%x (%s)", (unsigned)r, ResultName(r));
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

inline bool EnsureStandaloneFrame(ID3D12GraphicsCommandList* cmd, ID3D12Resource* back_buffer) {
  const D3D12_RESOURCE_DESC bb = back_buffer->GetDesc();
  const DXGI_FORMAT format = UavTwin(bb.Format);
  if (s.sa_frame != nullptr && s.sa_format == format && s.out_w == (unsigned)bb.Width &&
      s.out_h == bb.Height)
    return true;
  if (s.feature != nullptr) RetireFeature("standalone frame changed size or format");
  if (s.sa_frame != nullptr) {
    Drop(s.sa_frame, nullptr, StandaloneIdle(2000));
    s.sa_frame = nullptr;
  }
  s.out_w = s.render_w = (unsigned)bb.Width;
  s.out_h = s.render_h = bb.Height;
  s.create_flags = 0;

  ID3D12Device* device = nullptr;
  if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) return false;
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = bb.Width;
  desc.Height = bb.Height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                     nullptr, IID_PPV_ARGS(&s.sa_frame));
  device->Release();
  if (FAILED(hr)) {
    s.sa_frame = nullptr;
    ngx_probe::Logf("nr-fwd: standalone frame texture (%ux%u format %u) failed: 0x%08x",
                    s.out_w, s.out_h, (unsigned)format, (unsigned)hr);
    return false;
  }
  s.sa_format = format;
  ngx_probe::Logf("nr-fwd: standalone frame %ux%u format=%u (back buffer format %u)", s.out_w,
                  s.out_h, (unsigned)format, (unsigned)bb.Format);
  return true;
}

// Whether standalone may run now, with the reason kept for the overlay.
inline bool StandaloneAllowed() {
  static const ULONGLONG start = GetTickCount64();
  const ULONGLONG now = GetTickCount64();
  if (s.standalone_mode == kSaOff) {
    std::snprintf(s.sa_status, sizeof(s.sa_status), "off");
    return false;
  }
  if (s.last_dlss_tick != 0 && now - s.last_dlss_tick < kDlssQuietMs) {
    std::snprintf(s.sa_status, sizeof(s.sa_status), "standing by: a DLSS session is the source");
    return false;
  }
  if (s.standalone_mode == kSaAuto) {
    if (GetModuleHandleA("dlss5-bridge.addon64") != nullptr) {
      std::snprintf(s.sa_status, sizeof(s.sa_status),
                    "standing by: dlss5-bridge is installed and feeds this add-on");
      return false;
    }
    // nvngx_dlss.dll resident before standalone started NGX means the game loaded it; after
    // that, standalone's own NGX init may have loaded it, so it says nothing.
    if (s.last_dlss_tick != 0 ||
        (s.ngx_core == nullptr && GetModuleHandleA("nvngx_dlss.dll") != nullptr)) {
      std::snprintf(s.sa_status, sizeof(s.sa_status),
                    "standing by: this game has DLSS (set Standalone to Always to force it)");
      return false;
    }
    if (now - start < kStandaloneArmMs) {
      std::snprintf(s.sa_status, sizeof(s.sa_status), "waiting for a DLSS session first");
      return false;
    }
  }
  return true;
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
  if (dd.Width != bb.Width || dd.Height != bb.Height) {
    std::snprintf(s.sa_status, sizeof(s.sa_status),
                  "waiting: depth is %llux%u, the screen %llux%u -- pick the screen-sized buffer",
                  (unsigned long long)dd.Width, dd.Height, (unsigned long long)bb.Width,
                  bb.Height);
    return false;
  }

  TickGraveyard();
  if (s.retire_pending.exchange(false, std::memory_order_relaxed))
    RetireFeature("model settings changed");
  if (!s.enabled) {
    std::snprintf(s.sa_status, sizeof(s.sa_status), "paused (F10)");
    return false;
  }

  if (s.source != kSrcStandalone) {
    if (s.source == kSrcDlss) RetireFeature("switching to standalone");
    s.source = kSrcStandalone;
    ngx_probe::Log("nr-fwd: source = standalone (back buffer + ReShade depth + motion vectors)");
  }

  if (!EnsureStandaloneQueue(back_buffer)) return false;

  // Reuse the allocator three frames back once the GPU is past it.
  const int slot = (int)(s.sa_value % 3);
  if (s.sa_fence_out->GetCompletedValue() < s.sa_alloc_value[slot]) {
    s.sa_fence_out->SetEventOnCompletion(s.sa_alloc_value[slot], s.sa_event);
    // Still in flight: skip this frame rather than reset an allocator the GPU is using. A stall
    // that lasts is the watchdog's to handle.
    if (WaitForSingleObject(s.sa_event, 1000) != WAIT_OBJECT_0) return false;
  }
  s.sa_alloc[slot]->Reset();
  ID3D12GraphicsCommandList* cmd = s.sa_list;
  cmd->Reset(s.sa_alloc[slot], nullptr);
  s.sa_pending = true;  // from here on the list is submitted, even if the pass bails out

  if (!EnsureNgxCore(cmd)) return true;
  if (!EnsureStandaloneFrame(cmd, back_buffer)) return true;
  if (!EnsureSetup(cmd)) return true;
  if (!EnsureFeature(cmd)) return true;  // created this frame; it recorded init work

  using nr_compose::Barrier;
  // Back buffer -> the UAV-capable frame copy the pass works on in place.
  Barrier(cmd, back_buffer, bb_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Barrier(cmd, s.sa_frame, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  cmd->CopyResource(s.sa_frame, back_buffer);
  Barrier(cmd, s.sa_frame, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Barrier(cmd, back_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

  // Launchpad's motion vectors are in UV units; NGX scales them to pixels.
  Inputs in;
  in.color = s.sa_frame;
  in.depth = depth;
  in.motion = motion;
  in.mv_scale_x = (float)s.out_w * (s.sa_flip_x ? -1.0f : 1.0f);
  in.mv_scale_y = (float)s.out_h * (s.sa_flip_y ? -1.0f : 1.0f);
  in.depth_inverted = depth_inverted;
  // A game rendering below output resolution (FSR, TSR, a resolution scale) draws depth into the
  // top-left of a screen-sized buffer; ReShade's RESHADE_DEPTH_INPUT_*_SCALE describe that, and
  // the model gets the same region.
  if (depth_scale_x > 1.0f) in.depth_w = (unsigned)((float)s.out_w / depth_scale_x + 0.5f);
  if (depth_scale_y > 1.0f) in.depth_h = (unsigned)((float)s.out_h / depth_scale_y + 0.5f);
  Evaluate(cmd, in);

  // The result (or, if the pass bailed out, the untouched copy) goes back to the back buffer.
  Barrier(cmd, s.sa_frame, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  cmd->CopyResource(back_buffer, s.sa_frame);
  Barrier(cmd, s.sa_frame, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Barrier(cmd, back_buffer, D3D12_RESOURCE_STATE_COPY_DEST, bb_state);
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

