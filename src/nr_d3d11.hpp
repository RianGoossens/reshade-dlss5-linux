// Standalone mode for D3D11 games: the transport.
//
// The model only runs on D3D12, so a D3D11 game's frame is carried across: right after the
// motion-vector effect has rendered, the back buffer, ReShade's depth and the motion vectors are
// copied into textures both APIs can see, the runner's standalone pass runs on a private D3D12
// device, and the result is copied back into the back buffer. One shared fence orders the two.
//
// The sharing recipe -- a D3D12 texture created shared with simultaneous access and opened on
// D3D11, a shared D3D12 fence opened as an ID3D11Fence, depth converted to R32_FLOAT by a small
// compute shader because depth formats cannot be shared, and a 12_x feature level for the private
// device -- follows dlss5-bridge by NIGos (MIT), which proved it under Proton.

#pragma once

#include <windows.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <cstdio>

#include "nr_runner.hpp"

namespace nr_d3d11 {

struct Shared {
  ID3D12Resource* tex12 = nullptr;
  ID3D11Texture2D* tex11 = nullptr;
  UINT w = 0, h = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct State {
  bool failed = false;
  ID3D11Device* dev11 = nullptr;          // the game's (native) device, not owned
  ID3D11DeviceContext4* ctx4 = nullptr;
  ID3D12Device* dev12 = nullptr;          // the private device the model runs on
  ID3D12Fence* fence12 = nullptr;
  ID3D11Fence* fence11 = nullptr;
  UINT64 value = 0;
  Shared color, depth, motion;
  ID3D11UnorderedAccessView* depth_uav = nullptr;
  ID3D11ComputeShader* depth_cs = nullptr;
};

inline State d;

inline bool Fail(const char* why, HRESULT hr = S_OK) {
  ngx_probe::Logf("nr-d3d11: %s (0x%08x) -- D3D11 standalone disabled", why, (unsigned)hr);
  d.failed = true;
  return false;
}

using PFN_D3D12CreateDevice_ = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using PFN_D3DCompile_ = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
                                         ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                         ID3DBlob**);

constexpr char kDepthCs[] =
    "Texture2D<float> src : register(t0);\n"
    "RWTexture2D<float> dst : register(u0);\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "  uint w, h; dst.GetDimensions(w, h);\n"
    "  if (id.x >= w || id.y >= h) return;\n"
    "  dst[id.xy] = src.Load(int3(id.xy, 0));\n"
    "}\n";

inline bool EnsureDevice(ID3D11Device* dev11, ID3D11DeviceContext* ctx) {
  if (d.dev12 != nullptr) return true;
  d.dev11 = dev11;
  HRESULT hr = ctx->QueryInterface(IID_PPV_ARGS(&d.ctx4));
  if (FAILED(hr)) return Fail("ID3D11DeviceContext4 unavailable", hr);

  // The private device on the game's own adapter, at the highest feature level it grants: a
  // D3D12 runtime that is not NVIDIA's (vkd3d-proton) can gate capabilities on it.
  IDXGIDevice* dxgi_dev = nullptr;
  IDXGIAdapter* adapter = nullptr;
  if (SUCCEEDED(dev11->QueryInterface(IID_PPV_ARGS(&dxgi_dev)))) {
    dxgi_dev->GetAdapter(&adapter);
    dxgi_dev->Release();
  }
  HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
  auto create = d3d12 != nullptr ? (PFN_D3D12CreateDevice_)GetProcAddress(d3d12, "D3D12CreateDevice")
                                 : nullptr;
  if (create == nullptr) {
    if (adapter != nullptr) adapter->Release();
    return Fail("D3D12CreateDevice not found");
  }
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                      D3D_FEATURE_LEVEL_11_0};
  hr = E_FAIL;
  for (D3D_FEATURE_LEVEL level : levels) {
    hr = create(adapter, level, IID_PPV_ARGS(&d.dev12));
    if (SUCCEEDED(hr)) {
      ngx_probe::Logf("nr-d3d11: private D3D12 device at feature level 0x%x", (unsigned)level);
      break;
    }
  }
  if (adapter != nullptr) adapter->Release();
  if (FAILED(hr) || d.dev12 == nullptr) return Fail("D3D12CreateDevice failed", hr);

  // One fence, made on D3D12 and opened on D3D11, orders the two.
  HANDLE fh = nullptr;
  hr = d.dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&d.fence12));
  if (SUCCEEDED(hr)) hr = d.dev12->CreateSharedHandle(d.fence12, nullptr, GENERIC_ALL, nullptr, &fh);
  ID3D11Device5* dev5 = nullptr;
  if (SUCCEEDED(hr)) hr = dev11->QueryInterface(IID_PPV_ARGS(&dev5));
  if (SUCCEEDED(hr)) hr = dev5->OpenSharedFence(fh, IID_PPV_ARGS(&d.fence11));
  if (dev5 != nullptr) dev5->Release();
  if (fh != nullptr) CloseHandle(fh);
  if (FAILED(hr) || d.fence11 == nullptr) return Fail("shared fence setup failed", hr);

  // Depth formats cannot be shared, so depth is converted to R32_FLOAT on the D3D11 side.
  HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
  auto compile = compiler != nullptr ? (PFN_D3DCompile_)GetProcAddress(compiler, "D3DCompile")
                                     : nullptr;
  if (compile == nullptr) return Fail("d3dcompiler_47.dll unavailable");
  ID3DBlob* code = nullptr;
  ID3DBlob* errors = nullptr;
  hr = compile(kDepthCs, sizeof(kDepthCs) - 1, "nr_depth", nullptr, nullptr, "main", "cs_5_0", 0, 0,
               &code, &errors);
  if (errors != nullptr) errors->Release();
  if (FAILED(hr)) return Fail("depth conversion shader failed to compile", hr);
  hr = dev11->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                  &d.depth_cs);
  code->Release();
  if (FAILED(hr)) return Fail("CreateComputeShader failed", hr);

  ngx_probe::Log("nr-d3d11: transport ready (shared fence, depth conversion)");
  return true;
}

inline void Release(Shared& t) {
  if (t.tex11 != nullptr) t.tex11->Release();
  if (t.tex12 != nullptr) t.tex12->Release();
  t = {};
}

// A texture both APIs see: created shared on D3D12 with simultaneous access (D3D11 cannot do
// D3D12 state transitions), opened on D3D11.
inline bool EnsureShared(Shared& t, UINT w, UINT h, DXGI_FORMAT format, bool uav, const char* name) {
  if (t.tex12 != nullptr && t.w == w && t.h == h && t.format == format) return true;
  if (&t == &d.depth && d.depth_uav != nullptr) {
    d.depth_uav->Release();
    d.depth_uav = nullptr;
  }
  Release(t);
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
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
               (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
  HANDLE handle = nullptr;
  HRESULT hr = d.dev12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                                                D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(&t.tex12));
  if (SUCCEEDED(hr)) hr = d.dev12->CreateSharedHandle(t.tex12, nullptr, GENERIC_ALL, nullptr, &handle);
  ID3D11Device1* dev1 = nullptr;
  if (SUCCEEDED(hr)) hr = d.dev11->QueryInterface(IID_PPV_ARGS(&dev1));
  if (SUCCEEDED(hr)) hr = dev1->OpenSharedResource1(handle, IID_PPV_ARGS(&t.tex11));
  if (dev1 != nullptr) dev1->Release();
  if (handle != nullptr) CloseHandle(handle);
  if (FAILED(hr) || t.tex11 == nullptr) {
    Release(t);
    char why[96];
    std::snprintf(why, sizeof(why), "shared %s texture (%ux%u format %u) failed", name, w, h,
                  (unsigned)format);
    return Fail(why, hr);
  }
  t.w = w;
  t.h = h;
  t.format = format;
  ngx_probe::Logf("nr-d3d11: shared %s %ux%u format=%u", name, w, h, (unsigned)format);
  return true;
}

// The shareable, copy-compatible twin of a back-buffer format.
inline DXGI_FORMAT ColorFormat(DXGI_FORMAT f) {
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

inline DXGI_FORMAT MotionFormat(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    default: return f;
  }
}

// One frame. Called right after the motion-vector effect rendered, on the game's immediate
// context. depth_srv is ReShade's depth binding; motion is the motion-vector texture.
inline void OnFrame(ID3D11Device* dev11, ID3D11DeviceContext* ctx, ID3D11Resource* back_buffer,
                    ID3D11ShaderResourceView* depth_srv, ID3D11Resource* motion,
                    bool depth_inverted, float scale_x, float scale_y) {
  if (d.failed || back_buffer == nullptr) return;
  if (!nr_runner::StandaloneAllowed()) return;
  if (depth_srv == nullptr || motion == nullptr) {
    std::snprintf(nr_runner::s.sa_status, sizeof(nr_runner::s.sa_status), "waiting: %s",
                  depth_srv == nullptr ? "no depth buffer bound (Generic Depth)"
                                       : "no motion vectors (enable iMMERSE Launchpad)");
    return;
  }
  if (!EnsureDevice(dev11, ctx)) return;

  ID3D11Texture2D* bb_tex = nullptr;
  ID3D11Texture2D* mv_tex = nullptr;
  ID3D11Resource* depth_res = nullptr;
  ID3D11Texture2D* depth_tex = nullptr;
  depth_srv->GetResource(&depth_res);
  back_buffer->QueryInterface(IID_PPV_ARGS(&bb_tex));
  motion->QueryInterface(IID_PPV_ARGS(&mv_tex));
  if (depth_res != nullptr) depth_res->QueryInterface(IID_PPV_ARGS(&depth_tex));
  auto release = [&] {
    if (bb_tex != nullptr) bb_tex->Release();
    if (mv_tex != nullptr) mv_tex->Release();
    if (depth_tex != nullptr) depth_tex->Release();
    if (depth_res != nullptr) depth_res->Release();
  };
  if (bb_tex == nullptr || mv_tex == nullptr || depth_tex == nullptr) {
    release();
    return;
  }
  D3D11_TEXTURE2D_DESC bd = {}, md = {}, dd = {};
  bb_tex->GetDesc(&bd);
  mv_tex->GetDesc(&md);
  depth_tex->GetDesc(&dd);
  if (bd.SampleDesc.Count > 1) {
    release();
    Fail("multisampled back buffer is not supported");
    return;
  }

  if (!EnsureShared(d.color, bd.Width, bd.Height, ColorFormat(bd.Format), false, "colour") ||
      !EnsureShared(d.motion, md.Width, md.Height, MotionFormat(md.Format), false, "motion") ||
      !EnsureShared(d.depth, dd.Width, dd.Height, DXGI_FORMAT_R32_FLOAT, true, "depth")) {
    release();
    return;
  }
  if (d.depth_uav == nullptr &&
      FAILED(dev11->CreateUnorderedAccessView(d.depth.tex11, nullptr, &d.depth_uav))) {
    release();
    Fail("depth UAV creation failed");
    return;
  }

  // D3D11 side in: colour and motion copied, depth converted.
  ctx->CopyResource(d.color.tex11, bb_tex);
  ctx->CopyResource(d.motion.tex11, mv_tex);
  ctx->CSSetShader(d.depth_cs, nullptr, 0);
  ctx->CSSetShaderResources(0, 1, &depth_srv);
  ctx->CSSetUnorderedAccessViews(0, 1, &d.depth_uav, nullptr);
  ctx->Dispatch((dd.Width + 7) / 8, (dd.Height + 7) / 8, 1);
  ID3D11ShaderResourceView* no_srv = nullptr;
  ID3D11UnorderedAccessView* no_uav = nullptr;
  ctx->CSSetShaderResources(0, 1, &no_srv);
  ctx->CSSetUnorderedAccessViews(0, 1, &no_uav, nullptr);
  ctx->CSSetShader(nullptr, nullptr, 0);

  // The pass, on D3D12, between the two halves.
  const UINT64 in_value = ++d.value;
  d.ctx4->Signal(d.fence11, in_value);
  ctx->Flush();
  if (nr_runner::OnStandaloneFrame(d.color.tex12, d.depth.tex12, d.motion.tex12, depth_inverted,
                                   scale_x, scale_y, D3D12_RESOURCE_STATE_COMMON)) {
    const UINT64 out_value = ++d.value;
    if (nr_runner::SubmitStandaloneShared(d.fence12, in_value, out_value)) {
      // D3D11 side out: wait for the result on the GPU and copy it back.
      d.ctx4->Wait(d.fence11, out_value);
      ctx->CopyResource(bb_tex, d.color.tex11);
    }
  }
  release();
}

inline void Shutdown() {
  if (d.depth_uav != nullptr) d.depth_uav->Release();
  if (d.depth_cs != nullptr) d.depth_cs->Release();
  Release(d.color);
  Release(d.depth);
  Release(d.motion);
  if (d.fence11 != nullptr) d.fence11->Release();
  if (d.fence12 != nullptr) d.fence12->Release();
  if (d.ctx4 != nullptr) d.ctx4->Release();
  if (d.dev12 != nullptr) d.dev12->Release();
  d = {};
}

}  // namespace nr_d3d11
