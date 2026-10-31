// Host source-mock for nr_runner.hpp's platform surface. Included by nr_runner.hpp
// in place of <windows.h>/<bcrypt.h>/<d3d12.h>/<nvsdk_ngx*.h> and "nr_compose.hpp"
// when NR_HOST_MOCK is defined (see nr_runner.hpp's top-of-file block), so the
// extraction unit test can compile the runner with plain g++ on Linux.
//
// Declarations only: everything below is exactly the slice of those headers that
// nr_runner.hpp names. Bodies are intentionally absent -- a function that is
// declared but never called contributes no code to the object, so the missing
// bodies never reach the link (what the test does call are the pure extractors in
// src/ffx_api.hpp). Constant values that flow into asserts are transcribed from the
// same Microsoft/NGX headers the real build uses; the rest are named to satisfy
// compilation.
//
// Must compile standalone: NR_HOST_MOCK may be defined before <cstdint> etc. is
// included anywhere.

#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>

// --- Windows type and macro surface (windows.h slice) ------------------------

#define __cdecl     // g++ on Linux has no MSVC calling conventions; erase them so
#define __thiscall  // nr_runner's function-pointer typedefs compile as plain C++
#define __stdcall
#define WINAPI

typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned long DWORD;
typedef unsigned long long DWORD64;
typedef unsigned long long ULONGLONG;
typedef long HRESULT;
typedef void* HANDLE;
typedef void* HMODULE;
typedef void* LPVOID;
typedef const wchar_t* LPCWSTR;
typedef wchar_t* LPWSTR;
typedef BYTE* PUCHAR;
typedef unsigned int UINT;
typedef unsigned short UINT16;
typedef unsigned long long UINT64;
typedef wchar_t WCHAR;

#define FALSE 0
#define TRUE 1
#define MAX_PATH 260
#define INVALID_HANDLE_VALUE ((HANDLE)(long long)-1)
#define GENERIC_READ 0x80000000u
#define FILE_SHARE_READ 0x00000001u
#define OPEN_EXISTING 3u
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000u
#define WAIT_OBJECT_0 0u
#define LOAD_WITH_ALTERED_SEARCH_PATH 0x00000008u
#define HIWORD(l) ((unsigned int)((((unsigned long long)(l)) >> 16) & 0xffffu))
#define LOWORD(l) ((unsigned int)(((unsigned long long)(l)) & 0xffffu))
#define FAILED(hr) ((HRESULT)(hr) < 0)
#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)

// The IID/CLSID machinery: nr_runner only passes these to IID_PPV_ARGS, which the
// mock redefines (below) to ignore them.
struct GUID {
  unsigned long Data1;
  unsigned short Data2;
  unsigned short Data3;
  unsigned char Data4[8];
};
typedef GUID IID;
typedef const IID& REFIID;
extern const IID __uuidof_placeholder_iid;
#define IID_PPV_ARGS(ppType) __uuidof_placeholder_iid, (void**)(ppType)

// version.dll; NR_HOST_MOCK never sees nr_runner's #pragma comment(lib) lines (they
// sit in the real branch), so nothing links these -- the struct type is all that is
// named (ModelIdentityThread's file-version probe).
struct VS_FIXEDFILEINFO {
  DWORD dwFileVersionMS;
  DWORD dwFileVersionLS;
};

// Kernel32: the model-identity/setup paths name these; unreferenced in the test, so
// declaration-only suffices.
ULONGLONG GetTickCount64();
DWORD GetModuleFileNameW(HMODULE, LPWSTR, DWORD);
HMODULE LoadLibraryExW(LPCWSTR, HANDLE, DWORD);
HMODULE LoadLibraryW(LPCWSTR);
HMODULE GetModuleHandleA(const char*);
void* GetProcAddress(HMODULE, const char*);
HANDLE CreateFileW(LPCWSTR, DWORD, DWORD, void*, DWORD, DWORD, HANDLE);
BOOL ReadFile(HANDLE, void*, DWORD, DWORD*, void*);
BOOL CloseHandle(HANDLE);
DWORD WaitForSingleObject(HANDLE, DWORD);
HANDLE CreateThread(void*, size_t, DWORD(WINAPI*)(LPVOID), LPVOID, DWORD, DWORD*);
void Sleep(DWORD);
HANDLE CreateEventW(void*, BOOL, BOOL, LPCWSTR);

// version.dll, ModelIdentityThread's file-version probe.
DWORD GetFileVersionInfoSizeW(LPCWSTR, DWORD*);
BOOL GetFileVersionInfoW(LPCWSTR, DWORD, DWORD, void*);
BOOL VerQueryValueW(void*, LPCWSTR, void**, UINT*);

// <wchar.h> names nr_runner calls (MSVC CRT).
int wcscpy_s(wchar_t*, const wchar_t*);
int swprintf_s(wchar_t*, const wchar_t*, ...);
wchar_t* _wcsdup(const wchar_t*);

// --- bcrypt.h slice (link-only in the real build) ----------------------------

typedef void* BCRYPT_ALG_HANDLE;
typedef void* BCRYPT_HASH_HANDLE;
#define BCRYPT_SHA256_ALGORITHM L"SHA256"
long BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE*, LPCWSTR, LPCWSTR, DWORD);
long BCryptCreateHash(BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE*, void*, DWORD, void*, DWORD,
                             DWORD);
long BCryptHashData(BCRYPT_HASH_HANDLE, PUCHAR, DWORD, DWORD);
long BCryptFinishHash(BCRYPT_HASH_HANDLE, void*, DWORD, DWORD);
long BCryptDestroyHash(BCRYPT_HASH_HANDLE);
long BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE, DWORD);

// --- DXGI format slice (dxgiformat.h values) ---------------------------------

// Task 6 widened this slice: ffx_probe::OutputFormat names every DXGI format the
// FFX surface-format enum can resolve to, so the host build needs those enumerators
// too (values still transcribed from the real dxgiformat.h).
enum DXGI_FORMAT {
  DXGI_FORMAT_UNKNOWN = 0,
  DXGI_FORMAT_R32G32B32A32_TYPELESS = 1,
  DXGI_FORMAT_R32G32B32A32_FLOAT = 2,
  DXGI_FORMAT_R32G32B32A32_UINT = 3,
  DXGI_FORMAT_R32G32B32_FLOAT = 6,
  DXGI_FORMAT_R16G16B16A16_TYPELESS = 9,
  DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
  DXGI_FORMAT_R32G32_TYPELESS = 15,
  DXGI_FORMAT_R32G32_FLOAT = 16,
  DXGI_FORMAT_R32G32_UINT = 17,
  DXGI_FORMAT_R10G10B10A2_TYPELESS = 23,
  DXGI_FORMAT_R10G10B10A2_UNORM = 24,
  DXGI_FORMAT_R11G11B10_FLOAT = 26,
  DXGI_FORMAT_R8G8B8A8_TYPELESS = 27,
  DXGI_FORMAT_R8G8B8A8_UNORM = 28,
  DXGI_FORMAT_R8G8B8A8_UNORM_SRGB = 29,
  DXGI_FORMAT_R8G8B8A8_SNORM = 31,
  DXGI_FORMAT_R16G16_TYPELESS = 33,
  DXGI_FORMAT_R16G16_FLOAT = 34,
  DXGI_FORMAT_R16G16_UINT = 36,
  DXGI_FORMAT_R16G16_SINT = 38,
  DXGI_FORMAT_R32_TYPELESS = 39,
  DXGI_FORMAT_R32_FLOAT = 41,
  DXGI_FORMAT_R32_UINT = 42,
  DXGI_FORMAT_R8G8_TYPELESS = 48,
  DXGI_FORMAT_R8G8_UNORM = 49,
  DXGI_FORMAT_R8G8_UINT = 50,
  DXGI_FORMAT_R16_TYPELESS = 53,
  DXGI_FORMAT_R16_FLOAT = 54,
  DXGI_FORMAT_R16_UNORM = 56,
  DXGI_FORMAT_R16_UINT = 57,
  DXGI_FORMAT_R16_SNORM = 58,
  DXGI_FORMAT_R8_TYPELESS = 60,
  DXGI_FORMAT_R8_UNORM = 61,
  DXGI_FORMAT_R8_UINT = 62,
  DXGI_FORMAT_R8_SNORM = 63,
  DXGI_FORMAT_R9G9B9E5_SHAREDEXP = 67,
  DXGI_FORMAT_B8G8R8A8_UNORM = 87,
  DXGI_FORMAT_B8G8R8X8_UNORM = 88,
  DXGI_FORMAT_B8G8R8A8_TYPELESS = 90,
  DXGI_FORMAT_B8G8R8A8_UNORM_SRGB = 91,
  DXGI_FORMAT_B8G8R8X8_TYPELESS = 92,
  DXGI_FORMAT_B8G8R8X8_UNORM_SRGB = 93,
};

// --- d3d12.h slice -------------------------------------------------------------

enum D3D12_RESOURCE_STATES : unsigned {
  D3D12_RESOURCE_STATE_COMMON = 0,
  D3D12_RESOURCE_STATE_RENDER_TARGET = 0x4,
  D3D12_RESOURCE_STATE_UNORDERED_ACCESS = 0x8,
  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE = 0x40,
  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE = 0x80,
  D3D12_RESOURCE_STATE_COPY_DEST = 0x400,
  D3D12_RESOURCE_STATE_COPY_SOURCE = 0x800,
};

// State flags combine into the same enum (nr_runner's kGuide/kRead constants).
constexpr D3D12_RESOURCE_STATES operator|(D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
  return (D3D12_RESOURCE_STATES)((unsigned)a | (unsigned)b);
}

typedef unsigned D3D12_RESOURCE_FLAGS;
constexpr D3D12_RESOURCE_FLAGS D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS = 0x4;

enum D3D12_RESOURCE_DIMENSION { D3D12_RESOURCE_DIMENSION_TEXTURE2D = 3 };
enum D3D12_TEXTURE_LAYOUT { D3D12_TEXTURE_LAYOUT_UNKNOWN = 0 };
enum D3D12_HEAP_TYPE { D3D12_HEAP_TYPE_DEFAULT = 1 };
enum D3D12_FENCE_FLAGS { D3D12_FENCE_FLAG_NONE = 0 };
enum D3D12_HEAP_FLAGS { D3D12_HEAP_FLAG_NONE = 0 };
enum D3D12_COMMAND_LIST_TYPE { D3D12_COMMAND_LIST_TYPE_DIRECT = 0 };

struct D3D12_HEAP_PROPERTIES {
  D3D12_HEAP_TYPE Type;
};

struct D3D12_SAMPLE_DESC {
  UINT Count;
  UINT Quality;
};

struct D3D12_RESOURCE_DESC {
  D3D12_RESOURCE_DIMENSION Dimension;
  UINT64 Width;
  UINT Height;
  UINT16 DepthOrArraySize;
  UINT16 MipLevels;
  DXGI_FORMAT Format;
  D3D12_SAMPLE_DESC SampleDesc;
  D3D12_TEXTURE_LAYOUT Layout;
  D3D12_RESOURCE_FLAGS Flags;
};

struct D3D12_COMMAND_QUEUE_DESC {
  D3D12_COMMAND_LIST_TYPE Type;
};

struct ID3D12Device;

// nr_runner only ever calls GetDesc() and Release() on a resource; the mock's
// struct counterpart of the D3D12 interface needs nothing else to compile.
struct ID3D12Resource {
  D3D12_RESOURCE_DESC GetDesc() const;
  HRESULT GetDevice(REFIID, void**);
  void Release();
};

struct ID3D12CommandAllocator {
  HRESULT Reset();
  void Release();
};

struct ID3D12Fence {
  UINT64 GetCompletedValue() const;
  HRESULT SetEventOnCompletion(UINT64, HANDLE);
  HRESULT Signal(UINT64);
  void Release();
};

// Real hierarchy: ID3D12GraphicsCommandList derives from ID3D12CommandList, so
// the SubmitStandalone* arrays can hold a graphics list.
struct ID3D12CommandList {
  HRESULT GetDevice(REFIID, void**);
  HRESULT Close();
  void Release();
};

struct ID3D12CommandQueue {
  HRESULT Signal(ID3D12Fence*, UINT64);
  HRESULT Wait(ID3D12Fence*, UINT64);
  void ExecuteCommandLists(UINT, ID3D12CommandList* const*);
  void Release();
};

// The only device entry points nr_runner uses. Bodies are unbound declarations:
// CreateOne/EnsureFeature/etc. are inline definitions that the test never calls, so
// their references to these are never emitted.
struct ID3D12Device {
  HRESULT CreateCommittedResource(const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                  const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const void*,
                                  REFIID, void**);
  HRESULT CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
  HRESULT CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE, REFIID, void**);
  HRESULT CreateCommandList(UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, void*,
                            REFIID, void**);
  HRESULT CreateFence(UINT64, D3D12_FENCE_FLAGS, REFIID, void**);
  void Release();
};

struct ID3D12GraphicsCommandList : ID3D12CommandList {
  HRESULT ResourceBarrier(UINT, const void*);
  HRESULT CopyResource(ID3D12Resource*, ID3D12Resource*);
  HRESULT Reset(ID3D12CommandAllocator*, void*);
};

// --- nvsdk_ngx.h / nvsdk_ngx_defs.h slice -------------------------------------

// Real NGX result/factor values (nvsdk_ngx_defs.h): Success, the DLSS create-flag
// bits nr_runner tests, and the parameter-key strings ReadGuides reads (values
// asserted nowhere, transcribed for faithfulness).
typedef unsigned NVSDK_NGX_Result;
constexpr NVSDK_NGX_Result NVSDK_NGX_Result_Success = 0x1;
enum NVSDK_NGX_DLSS_Feature_Flags {
  NVSDK_NGX_DLSS_Feature_Flags_None = 0,
  NVSDK_NGX_DLSS_Feature_Flags_IsHDR = 1 << 0,
  NVSDK_NGX_DLSS_Feature_Flags_MVLowRes = 1 << 1,
  NVSDK_NGX_DLSS_Feature_Flags_MVJittered = 1 << 2,
  NVSDK_NGX_DLSS_Feature_Flags_DepthInverted = 1 << 3,
  NVSDK_NGX_DLSS_Feature_Flags_AutoExposure = 1 << 6,
};
constexpr int NVSDK_NGX_Version_API = 0x0000015;

#define NVSDK_NGX_Parameter_Color "Color"
#define NVSDK_NGX_Parameter_Output "Output"
#define NVSDK_NGX_Parameter_Reset "Reset"
#define NVSDK_NGX_Parameter_MotionVectors "MotionVectors"
#define NVSDK_NGX_Parameter_MV_Scale_X "MV.Scale.X"
#define NVSDK_NGX_Parameter_MV_Scale_Y "MV.Scale.Y"
#define NVSDK_NGX_Parameter_Depth "Depth"

// C++ form of nvsdk_ngx_params.h's parameter block: the exact Get overloads
// ReadGuides/OnDlssEvaluated use, no Set/Reset (nr_runner only writes through its
// own vtable setters). Declarations only -- the test never calls ReadGuides.
struct NVSDK_NGX_Parameter {
  virtual NVSDK_NGX_Result Get(const char* InName, unsigned long long* OutValue) const = 0;
  virtual NVSDK_NGX_Result Get(const char* InName, float* OutValue) const = 0;
  virtual NVSDK_NGX_Result Get(const char* InName, double* OutValue) const = 0;
  virtual NVSDK_NGX_Result Get(const char* InName, unsigned int* OutValue) const = 0;
  virtual NVSDK_NGX_Result Get(const char* InName, int* OutValue) const = 0;
  virtual NVSDK_NGX_Result Get(const char* InName, ID3D12Resource** OutValue) const = 0;
  virtual NVSDK_NGX_Result Get(const char* InName, void** OutValue) const = 0;
  virtual void Reset() = 0;
};

// --- nr_compose.hpp stand-in --------------------------------------------------
// Same namespace and call shapes as the real compose pass; every function is a
// declaration-only stub (never called by the test, never emitted). Bridge mirrors
// the real struct's three fields; kScale*/kReadbackRing carry the real values.

namespace nr_compose {

constexpr uint32_t kReadbackRing = 4;
enum ScaleKind { kScaleDown = 0, kScaleMotion = 1, kScaleDelta = 2, kScaleUp = 3, kScaleDepth = 4 };

struct Bridge {
  float white_point;
  uint32_t encoding;
  uint32_t curve;
};

void Release();
void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
             D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
bool RecordPre(ID3D12GraphicsCommandList* cmd, ID3D12Resource* game_out,
               ID3D12Resource* color_copy, ID3D12Resource* proxy, uint32_t w, uint32_t h,
               uint32_t frame, const Bridge& bridge);
void RecordPost(ID3D12GraphicsCommandList* cmd, ID3D12Resource* game_out,
                ID3D12Resource* color_copy, ID3D12Resource* proxy, ID3D12Resource* model_out,
                uint32_t w, uint32_t h, uint32_t frame, float transfer, float max_ratio,
                float colour, const Bridge& bridge, uint32_t debug,
                ID3D12Resource* motion = nullptr, float mv_scale_x = 1.0f,
                float mv_scale_y = 1.0f, ID3D12Resource* depth = nullptr);
bool ScalePass(ID3D12GraphicsCommandList* cmd, ScaleKind kind, ID3D12Resource* t0,
               ID3D12Resource* t1, ID3D12Resource* u0, uint32_t w, uint32_t h, uint32_t frame,
               DXGI_FORMAT t0_format = DXGI_FORMAT_UNKNOWN);
float ReadWhitePoint(uint32_t frame);

}  // namespace nr_compose

// --- ngx_probe logging surface -------------------------------------------------
// nr_runner only ever calls Log/Logf (Warnf appears in the test-hook block, which
// compiles out without DLSSNR_TEST_HOOKS). Declaration-only, like everything above.

namespace ngx_probe {

inline void Log(const char*) {}
inline void Logf(const char*, ...) {}
void Warnf(const char* fmt, ...);

}  // namespace ngx_probe
