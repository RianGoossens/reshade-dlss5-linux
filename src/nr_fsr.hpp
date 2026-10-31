#pragma once

#include "ffx_api.hpp"  // vendored FFX types + ffx_probe extraction helpers
#include "nr_runner.hpp"

#ifndef NR_HOST_MOCK
#include "ngx_probe.hpp"  // Logf + nr_runner (ngx_probe.hpp includes it, as ffx_api.hpp does)
#endif

namespace nr_fsr {

inline std::atomic<bool> saw_upscale_create{false};

inline nr_runner::Inputs last_inputs;

inline void OnFfxCreateContext(const ffx_api::ffxCreateContextDescHeader* header,
                               void* context = nullptr) {
  for (const ffx_api::ffxApiHeader* h = header; h != nullptr; h = h->pNext) {
    if (h->type != FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE) continue;
    const auto* up = reinterpret_cast<const ffx_api::ffxCreateContextDescUpscale*>(h);
    ffx_probe::RememberContext(context, *up);
    ffx_api::active_upscale_flags = up->flags;
    saw_upscale_create = true;
    ngx_probe::Logf("nr-fwd: FSR3 upscaler context created (flags=0x%x)", up->flags);
    return;  // one upscale create per context chain
  }
}

inline void OnFfxDispatch(void* context,
                          const ffx_api::ffxDispatchDescHeader* header,
                          uint32_t flags = ffx_api::active_upscale_flags) {
  (void)context;
  if (nr_runner::s.frame_source == nr_runner::kFsDlss) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
      ngx_probe::Log("nr-fwd: DLSS session forced \u2014 FSR3 dispatches ignored");
    }
    return;
  }
  nr_runner::s.last_fsr_tick.store(GetTickCount64(), std::memory_order_relaxed);

  const bool upscale = header != nullptr &&
                       header->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  nr_runner::Inputs in;  // stack: this dispatch's working copy, never shared state
  if (upscale) {
    const auto* d = reinterpret_cast<const ffx_api::ffxDispatchDescUpscale*>(header);
    if (ffx_probe::UpscaleInputs(d, &in, flags)) {
      {
        nr_runner::PassGuard guard(nr_runner::kSrcFsr);
        if (!guard) return;
        ffx_probe::OnUpscaleGeometry(*d, in, flags);
        last_inputs = in;
      }  // stamped: the extraction path ran to completion
      if (saw_upscale_create) {
        nr_runner::OnFsrDispatchClaim(GetTickCount64());
      } else {
        static bool said = false;
        if (!said) {
          said = true;
          ngx_probe::Log(
              "nr-fwd: FSR3 dispatch before an upscale context was seen -- pass not run "
              "(context may predate hooks; change in-game FSR quality once to recreate it)");
        }
      }
    }
  }
}

#ifndef NR_HOST_MOCK

// The shared recording half: desc checks, extraction, geometry from the caller.
// `frame` is the dispatch's color resource (Render: the upscale input; Full: the
// upscale output the pass writes back into).
inline void RecordPass(const ffx_api::ffxDispatchDescUpscale* d, ID3D12Resource* frame,
                       uint32_t flags, bool pre) {
  if (!saw_upscale_create) {
    static bool said = false;
    if (!said) {
      said = true;
      ngx_probe::Log(
          "nr-fwd: FSR3 dispatch before an upscale context was seen -- pass not run "
          "(context may predate hooks; change in-game FSR quality once to recreate it)");
    }
    return;
  }
  // D3D12 sanity check: FfxApiResourceDescription carries no backend/API field; a
  // texture type is what the DX12 backend writes from the D3D12 resource desc.
  // Not D3D12-shaped: one bail-out line, the dispatch passes through untouched.
  if (!ffx_probe::IsD3d12TextureType(d->color.description)) {
    ngx_probe::Logf("nr-fwd: FSR3 dispatch not D3D12-shaped (color type=%u) -- pass skipped",
                    d->color.description.type);
    return;
  }
  if (frame == nullptr) return;  // malformed desc; Evaluate's own null guard agrees
  auto* cmd = static_cast<ID3D12GraphicsCommandList*>(d->commandList);
  if (cmd == nullptr) return;

  nr_runner::Inputs in;  // stack: this dispatch's working copy (extraction contract)
  if (!ffx_probe::UpscaleInputs(d, &in, flags)) return;
  in.color = frame;  // Render records from the color input, Full from the output
  // The encode reference follows the DLSS precedent: the source's output format
  // in both modes (PreDlssEvaluate keys off the DLSS-SR output the same way).
  in.output_format = ffx_probe::OutputFormat(d->output.description);

  nr_runner::PassGuard guard(nr_runner::kSrcFsr);
  if (!guard) return;
  if (!nr_runner::RecordingModeMatches(pre)) return;
  if (nr_runner::s.source.load(std::memory_order_relaxed) != nr_runner::kSrcFsr) return;
  const int source = nr_runner::s.frame_source.load(std::memory_order_relaxed);
  if (source == nr_runner::kFsDlss || source == nr_runner::kFsStandalone) return;
  ffx_probe::OnUpscaleGeometry(*d, in, flags);
  if (!nr_runner::PrepareFsrPass(cmd)) return;
  nr_runner::Evaluate(cmd, in);
  // The retire-every test lever, on this path the way OnDlssEvaluated carries it.
  // Qualified by hand: the NR_TEST_HOOK_AFTER_EVALUATE macro expands unqualified
  // for calls inside namespace nr_runner, and nr_fsr is a different namespace.
#ifdef DLSSNR_TEST_HOOKS
  nr_runner::TestHookAfterEvaluate();
#endif
}

// Model resolution Render, before the real dispatch (the PreDlssEvaluate shape):
// the pass enhances the upscale's color input in place, then ffxDispatch upscales
// the enhanced frame. The desc was already classified in OnFfxDispatch.
inline void PreRecord(const ffx_api::ffxDispatchDescHeader* header, uint32_t flags) {
  if (header == nullptr || header->type != FFX_API_DISPATCH_DESC_TYPE_UPSCALE) return;
  const auto* d = reinterpret_cast<const ffx_api::ffxDispatchDescUpscale*>(header);
  RecordPass(d, static_cast<ID3D12Resource*>(d->color.resource), flags, true);
}

// Model resolution Full, after a successful real dispatch (the OnDlssEvaluated
// shape): the frame is the upscale output; RecordPost writes the enhanced frame
// back into it before the game's post-processing reads it.
inline void PostRecord(const ffx_api::ffxDispatchDescHeader* header, uint32_t flags) {
  if (header == nullptr || header->type != FFX_API_DISPATCH_DESC_TYPE_UPSCALE) return;
  const auto* d = reinterpret_cast<const ffx_api::ffxDispatchDescUpscale*>(header);
  RecordPass(d, static_cast<ID3D12Resource*>(d->output.resource), flags, false);
}
#else
// Host unit-test build: the record bodies would pull Evaluate's graph into the
// link, whose mock side (test/unit/ffx_mock.hpp) is declaration-only by design --
// the extraction test exercises classification and extraction, not recording.
inline void PreRecord(const ffx_api::ffxDispatchDescHeader*, uint32_t) {}
inline void PostRecord(const ffx_api::ffxDispatchDescHeader*, uint32_t) {}
#endif  // !NR_HOST_MOCK

}  // namespace nr_fsr
