// DLSSNR Linux — ReShade addon entry point.
// Registers the overlay and present hook, logs swapchain ground truth
// (color space, back buffer format, dimensions, Wine/Proton detection),
// and installs the NGX CreateFeature/EvaluateFeature interception
// (ngx_probe.hpp) that the runner and colour bridge are driven by.

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "user32")  // GetAsyncKeyState for the F10 toggle

// ImGui before reshade.hpp so the overlay function table lights up; ImTextureID must be 8 bytes
// to match reshade::api::resource_view.
#define ImTextureID ImU64
#define ImDrawIdx unsigned int
#include <imgui.h>

#include <reshade.hpp>

#include "ngx_probe.hpp"
#include "nr_d3d11.hpp"

namespace {

const char* ColorSpaceName(reshade::api::color_space cs) {
  switch (cs) {
    case reshade::api::color_space::unknown: return "unknown (treat as SDR sRGB)";
    case reshade::api::color_space::srgb: return "srgb (SDR)";
    case reshade::api::color_space::scrgb: return "scrgb (linear HDR BT.709)";
    case reshade::api::color_space::hdr10_pq: return "hdr10_pq (PQ BT.2020)";
    case reshade::api::color_space::hdr10_hlg: return "hdr10_hlg (HLG BT.2020)";
    default: return "unrecognized";
  }
}

void LogWineVersion() {
  char buf[256];
  const auto* ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto wine_get_version = reinterpret_cast<const char* (*)()>(
      GetProcAddress((HMODULE)ntdll, "wine_get_version"));
  if (wine_get_version != nullptr) {
    std::snprintf(buf, sizeof(buf), "environment: Wine/Proton %s", wine_get_version());
  } else {
    std::snprintf(buf, sizeof(buf), "environment: native Windows");
  }
  reshade::log::message(reshade::log::level::info, buf);
}

// get_resource_desc returns a large struct by value from a virtual method —
// only ABI-safe because we build with clang targeting x86_64-pc-windows-msvc
// (matching MSVC-built ReShade). A MinGW GCC build crashes on this call.
void OnInitSwapchain(reshade::api::swapchain* swapchain, bool resize) {
  auto* device = swapchain->get_device();
  const auto back_buffer = swapchain->get_back_buffer(0);
  const auto desc = device->get_resource_desc(back_buffer);
  const auto color_space = swapchain->get_color_space();

  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "swapchain %s: %ux%u format=%u color_space=%u [%s] buffers=%u api=%u",
                resize ? "resized" : "created",
                desc.texture.width, desc.texture.height,
                static_cast<unsigned>(desc.texture.format),
                static_cast<unsigned>(color_space), ColorSpaceName(color_space),
                swapchain->get_back_buffer_count(),
                static_cast<unsigned>(device->get_api()));
  reshade::log::message(reshade::log::level::info, buf);
}

// The NGX module (_nvngx.dll / nvngx_dlss.dll) loads well after the addon,
// so retry the Detours attach on every present until it lands. Detours
// patches function bodies, so attaching after the game cached the function
// pointers still works.
void OnPresent(reshade::api::command_queue*, reshade::api::swapchain*,
               const reshade::api::rect*, const reshade::api::rect*,
               uint32_t, const reshade::api::rect*) {
  ngx_probe::TryInstall();

  // F10 toggles the NR pass, edge-triggered, for instant A/B comparison.
  static bool f10_was_down = false;
  const bool f10_down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
  if (f10_down && !f10_was_down) nr_runner::SetEnabled(!nr_runner::IsEnabled());
  f10_was_down = f10_down;
}

// --- standalone source --------------------------------------------------------
// A D3D12 game without DLSS: run the pass right after the motion-vector effect (iMMERSE
// Launchpad) has rendered, when its texture is current and Generic Depth still has the depth
// buffer in shader-resource state (it reverts that at reshade_finish_effects).

constexpr const char* kDepthNames[] = {"DepthInputTex", "DepthBufferTex", "DepthTex"};
constexpr char kMotionName[] = "MotionVectorsTex";

ID3D12Resource* TextureResource(reshade::api::effect_runtime* runtime,
                                reshade::api::effect_texture_variable var) {
  if (var.handle == 0) return nullptr;
  reshade::api::resource_view srv = {}, srv_srgb = {};
  runtime->get_texture_binding(var, &srv, &srv_srgb);
  if (srv.handle == 0) return nullptr;
  const auto res = runtime->get_device()->get_resource_from_view(srv);
  return reinterpret_cast<ID3D12Resource*>(res.handle);
}

// A global preprocessor definition as a number; 1 when unset or unreadable.
float PreprocessorFloat(reshade::api::effect_runtime* runtime, const char* name) {
  char value[32] = {};
  if (!runtime->get_preprocessor_definition(name, value)) return 1.0f;
  const float v = std::strtof(value, nullptr);
  return v > 0.0f ? v : 1.0f;
}

// Launchpad computes optical flow only when an effect asks for it: requests are bits in a 1x1
// texture it clears at the end of its own technique, and effects after it set theirs for the next
// frame. Right after Launchpad that texture is all zero, so setting the optical-flow bit (blue) is
// a clear that keeps every later effect's own request intact.
void RequestLaunchpadMotion(reshade::api::effect_runtime* runtime,
                            reshade::api::command_list* cmd_list, const char* effect) {
  using namespace reshade::api;
  static resource requested = {0};
  static resource_view rtv = {0};
  device* dev = runtime->get_device();
  reshade::api::resource_view srv = {}, srv_srgb = {};
  const effect_texture_variable var = runtime->find_texture_variable(effect, "PredicationBuffer");
  if (var.handle == 0) return;
  runtime->get_texture_binding(var, &srv, &srv_srgb);
  if (srv.handle == 0) return;
  const resource res = dev->get_resource_from_view(srv);
  if (res.handle == 0) return;
  if (res.handle != requested.handle) {
    if (rtv.handle != 0) dev->destroy_resource_view(rtv);
    rtv = {0};
    requested = res;
    const format f = format_to_default_typed(dev->get_resource_desc(res).texture.format);
    if (!dev->create_resource_view(res, resource_usage::render_target, resource_view_desc(f), &rtv))
      rtv = {0};
  }
  if (rtv.handle == 0) return;
  const float request[4] = {0.0f, 0.0f, 1.0f, 0.0f};  // MARTYSMODS_IPC_FEATURE_OPTICALFLOW
  cmd_list->barrier(res, resource_usage::shader_resource, resource_usage::render_target);
  cmd_list->clear_render_target_view(rtv, request);
  cmd_list->barrier(res, resource_usage::render_target, resource_usage::shader_resource);
}

// D3D11: the same inputs, carried to the D3D12 pass by the transport. ReShade's D3D11 handles
// are the native objects themselves.
void OnRenderTechniqueD3D11(reshade::api::effect_runtime* runtime,
                            reshade::api::command_list* cmd_list,
                            reshade::api::resource_view rtv, const char* effect) {
  auto* dev = runtime->get_device();
  auto view = [&](const char* name) {
    reshade::api::resource_view srv = {}, srv_srgb = {};
    const auto var = runtime->find_texture_variable(effect, name);
    if (var.handle != 0) runtime->get_texture_binding(var, &srv, &srv_srgb);
    return srv;
  };
  const reshade::api::resource_view motion_srv = view(kMotionName);
  reshade::api::resource_view depth_srv = {};
  for (const char* depth_name : kDepthNames) {
    depth_srv = view(depth_name);
    if (depth_srv.handle != 0) break;
  }
  char reversed[8] = {};
  const bool depth_inverted =
      runtime->get_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_REVERSED", reversed) &&
      reversed[0] == '1';
  nr_d3d11::OnFrame(
      reinterpret_cast<ID3D11Device*>(dev->get_native()),
      reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native()),
      reinterpret_cast<ID3D11Resource*>(dev->get_resource_from_view(rtv).handle),
      reinterpret_cast<ID3D11ShaderResourceView*>(depth_srv.handle),
      motion_srv.handle != 0
          ? reinterpret_cast<ID3D11Resource*>(dev->get_resource_from_view(motion_srv).handle)
          : nullptr,
      depth_inverted, PreprocessorFloat(runtime, "RESHADE_DEPTH_INPUT_X_SCALE"),
      PreprocessorFloat(runtime, "RESHADE_DEPTH_INPUT_Y_SCALE"));
}

void OnRenderTechnique(reshade::api::effect_runtime* runtime,
                       reshade::api::effect_technique technique,
                       reshade::api::command_list* cmd_list, reshade::api::resource_view rtv,
                       reshade::api::resource_view) {
  char name[128] = {};
  runtime->get_technique_name(technique, name);
  if (std::strstr(name, "Launchpad") == nullptr) return;
  char effect[256] = {};
  runtime->get_technique_effect_name(technique, effect);
  RequestLaunchpadMotion(runtime, cmd_list, effect);  // every API: the bridge reads it too
  const auto api = runtime->get_device()->get_api();
  if (api == reshade::api::device_api::d3d11) {
    OnRenderTechniqueD3D11(runtime, cmd_list, rtv, effect);
    return;
  }
  if (api != reshade::api::device_api::d3d12) return;

  ID3D12Resource* motion = TextureResource(runtime, runtime->find_texture_variable(effect, kMotionName));
  ID3D12Resource* depth = nullptr;
  for (const char* depth_name : kDepthNames) {
    depth = TextureResource(runtime, runtime->find_texture_variable(effect, depth_name));
    if (depth != nullptr) break;
  }

  char reversed[8] = {};
  const bool depth_inverted =
      runtime->get_preprocessor_definition("RESHADE_DEPTH_INPUT_IS_REVERSED", reversed) &&
      reversed[0] == '1';
  const float scale_x = PreprocessorFloat(runtime, "RESHADE_DEPTH_INPUT_X_SCALE");
  const float scale_y = PreprocessorFloat(runtime, "RESHADE_DEPTH_INPUT_Y_SCALE");

  auto* back_buffer =
      reinterpret_cast<ID3D12Resource*>(runtime->get_device()->get_resource_from_view(rtv).handle);
  if (nr_runner::OnStandaloneFrame(back_buffer, depth, motion, depth_inverted, scale_x,
                                   scale_y)) {
    // The pass is on its own list: send everything ReShade recorded so far (the motion vectors
    // included) to the game queue first, then slot the pass in behind it.
    auto* queue = runtime->get_command_queue();
    queue->flush_immediate_command_list();
    nr_runner::SubmitStandalone(reinterpret_cast<ID3D12CommandQueue*>(queue->get_native()));
  }
}

// --- settings persistence -------------------------------------------------
// The tuning lives in ReShade's own config (ReShade.ini beside the game exe),
// so it survives a game restart. Loaded once at attach — early enough, since
// the NR feature is only built on the first DLSS evaluate — and saved from the
// overlay whenever a control changes (ReShade batches the actual disk flush).
// The Enabled flag and the debug view are deliberately not persisted: both are
// transient A/B tools, and coming back up in a debug view would read as broken.

constexpr char kConfigSection[] = "ADDON_DLSSNR_LINUX";

float Clamp(float v, float min, float max) { return v < min ? min : (v > max ? max : v); }

// Keys mirror the RenoDX DLSS5 addon's (DirectNeuralRendering*), so a setting reads the same in
// both; the fork's own compose controls keep their names.
void LoadSettings() {
  auto& s = nr_runner::s;

  // Pre-mirror keys, read first so the new names win when both exist.
  reshade::get_config_value(nullptr, kConfigSection, "Intensity", s.intensity);
  reshade::get_config_value(nullptr, kConfigSection, "StructureIntensity", s.local_structure);
  reshade::get_config_value(nullptr, kConfigSection, "GlobalIntensity", s.local_tone);  // was LocalTone
  reshade::get_config_value(nullptr, kConfigSection, "Style", s.style);

  reshade::get_config_value(nullptr, kConfigSection, "DirectNeuralRenderingEncoding", s.encoding);
  reshade::get_config_value(nullptr, kConfigSection, "DirectNeuralRenderingPassCount", s.pass_count);
  reshade::get_config_value(nullptr, kConfigSection, "StandaloneMode", s.standalone_mode);
  reshade::get_config_value(nullptr, kConfigSection, "DirectNeuralRenderingDiffuseWhiteNits",
                            s.diffuse_white_nits);
  reshade::get_config_value(nullptr, kConfigSection, "DirectNeuralRenderingDiffuseWhiteOverride",
                            s.diffuse_white_override);
  reshade::get_config_value(nullptr, kConfigSection, "DirectNeuralRenderingIntensity", s.intensity);
  reshade::get_config_value(nullptr, kConfigSection, "DirectNeuralRenderingStyle", s.style);
  reshade::get_config_value(nullptr, kConfigSection, "DirectNeuralRenderingLocalStructureStrength",
                            s.local_structure);
  reshade::get_config_value(nullptr, kConfigSection, "DirectNeuralRenderingGlobalToneStrength",
                            s.global_tone);
  reshade::get_config_value(nullptr, kConfigSection, "DirectNeuralRenderingLocalToneStrength",
                            s.local_tone);
  reshade::get_config_value(nullptr, kConfigSection, "DirectNeuralRenderingAutoMask", s.auto_mask);
  reshade::get_config_value(nullptr, kConfigSection, "DirectNeuralRenderingSkinStructureStrength",
                            s.skin_structure);
  reshade::get_config_value(nullptr, kConfigSection, "DirectNeuralRenderingUiCorrection",
                            s.ui_correction);

  reshade::get_config_value(nullptr, kConfigSection, "DetailStrength", s.transfer);
  reshade::get_config_value(nullptr, kConfigSection, "ColourStrength", s.colour_strength);
  reshade::get_config_value(nullptr, kConfigSection, "WhitePointScale", s.wp_scale);

  // A hand-edited ini must not push values past what the model or the compose pass tolerate.
  // Intensities are forwarded unvalidated beyond the slider range, as in RenoDX, but not negative.
  s.transfer = Clamp(s.transfer, 0.0f, 1.5f);
  s.colour_strength = Clamp(s.colour_strength, 0.0f, 1.0f);
  s.max_ratio = Clamp(s.max_ratio, 1.0f, 8.0f);
  s.wp_scale = Clamp(s.wp_scale, 0.1f, 4.0f);
  s.diffuse_white_nits = Clamp(s.diffuse_white_nits, 1.0f, 10000.0f);
  s.intensity = Clamp(s.intensity, 0.0f, 4.0f);
  s.local_structure = Clamp(s.local_structure, 0.0f, 4.0f);
  s.global_tone = Clamp(s.global_tone, 0.0f, 4.0f);
  s.local_tone = Clamp(s.local_tone, 0.0f, 4.0f);
  s.skin_structure = Clamp(s.skin_structure, 0.0f, 4.0f);
  if (s.encoding < 0 || s.encoding > nr_runner::kEncMeasured) s.encoding = 0;
  if (s.preset < 0 || s.preset > 7) s.preset = 0;
  if (s.pass_count < 1 || s.pass_count > nr_runner::kMaxPasses) s.pass_count = 1;
  if (s.standalone_mode < 0 || s.standalone_mode > nr_runner::kSaOff) s.standalone_mode = 0;
  if (s.style < 0 || s.style > 2) s.style = 0;
}

void SaveSettings() {
  auto& s = nr_runner::s;
  reshade::set_config_value(nullptr, kConfigSection, "DirectNeuralRenderingEncoding", s.encoding);
  reshade::set_config_value(nullptr, kConfigSection, "DirectNeuralRenderingPassCount", s.pass_count);
  reshade::set_config_value(nullptr, kConfigSection, "StandaloneMode", s.standalone_mode);
  reshade::set_config_value(nullptr, kConfigSection, "DirectNeuralRenderingDiffuseWhiteNits",
                            s.diffuse_white_nits);
  reshade::set_config_value(nullptr, kConfigSection, "DirectNeuralRenderingDiffuseWhiteOverride",
                            s.diffuse_white_override);
  reshade::set_config_value(nullptr, kConfigSection, "DirectNeuralRenderingIntensity", s.intensity);
  reshade::set_config_value(nullptr, kConfigSection, "DirectNeuralRenderingStyle", s.style);
  reshade::set_config_value(nullptr, kConfigSection, "DirectNeuralRenderingLocalStructureStrength",
                            s.local_structure);
  reshade::set_config_value(nullptr, kConfigSection, "DirectNeuralRenderingGlobalToneStrength",
                            s.global_tone);
  reshade::set_config_value(nullptr, kConfigSection, "DirectNeuralRenderingLocalToneStrength",
                            s.local_tone);
  reshade::set_config_value(nullptr, kConfigSection, "DirectNeuralRenderingAutoMask", s.auto_mask);
  reshade::set_config_value(nullptr, kConfigSection, "DirectNeuralRenderingSkinStructureStrength",
                            s.skin_structure);
  reshade::set_config_value(nullptr, kConfigSection, "DirectNeuralRenderingUiCorrection",
                            s.ui_correction);
  reshade::set_config_value(nullptr, kConfigSection, "Preset", static_cast<const char*>(nullptr));

  reshade::set_config_value(nullptr, kConfigSection, "DetailStrength", s.transfer);
  reshade::set_config_value(nullptr, kConfigSection, "ColourStrength", s.colour_strength);
  reshade::set_config_value(nullptr, kConfigSection, "HighlightGuard", static_cast<const char*>(nullptr));
  reshade::set_config_value(nullptr, kConfigSection, "WhitePointScale", s.wp_scale);

  // Retired pre-mirror keys, so they cannot override the new ones on the next load.
  reshade::set_config_value(nullptr, kConfigSection, "Intensity",
                            static_cast<const char*>(nullptr));
  reshade::set_config_value(nullptr, kConfigSection, "StructureIntensity",
                            static_cast<const char*>(nullptr));
  reshade::set_config_value(nullptr, kConfigSection, "GlobalIntensity",
                            static_cast<const char*>(nullptr));
  reshade::set_config_value(nullptr, kConfigSection, "Style",
                            static_cast<const char*>(nullptr));
}

// What a control reports back to the panel.
struct Edit {
  bool changed = false;    // save the ini
  bool committed = false;  // a model setting was released: rebuild the feature
  void operator|=(const Edit& o) {
    changed |= o.changed;
    committed |= o.committed;
  }
};

// Tooltip on the item just drawn, RenoDX-style.
void Tip(const char* text) {
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

// A reset button on the right of the item just drawn.
bool ResetButton(const char* id) {
  ImGui::SameLine();
  ImGui::PushID(id);
  const bool pressed = ImGui::SmallButton("reset");
  ImGui::PopID();
  return pressed;
}

// Slider with a reset button. Committed on release rather than on every drag step.
Edit Slider(const char* label, float* value, float min, float max, const char* fmt,
            float default_value, const char* tip) {
  Edit e;
  e.changed = ImGui::SliderFloat(label, value, min, max, fmt);
  e.committed = ImGui::IsItemDeactivatedAfterEdit();
  Tip(tip);
  if (ResetButton(label)) {
    *value = default_value;
    e.changed = e.committed = true;
  }
  return e;
}

Edit Combo(const char* label, int* value, const char* items, int default_value, const char* tip) {
  Edit e;
  e.changed = e.committed = ImGui::Combo(label, value, items);
  Tip(tip);
  if (ResetButton(label)) {
    *value = default_value;
    e.changed = e.committed = true;
  }
  return e;
}

Edit Checkbox(const char* label, bool* value, const char* tip) {
  Edit e;
  e.changed = e.committed = ImGui::Checkbox(label, value);
  Tip(tip);
  return e;
}

constexpr const char* kEncodingNames[] = {"Auto",  "Linear BT.709", "sRGB", "BT.2100 PQ",
                                          "scRGB", "scRGB-nl",      "Measured (legacy)"};

// The settings panel, under ReShade's Add-ons tab. Laid out like the RenoDX DLSS5 addon's Neural
// Rendering section; the fork's own compose controls follow under their own header.
void OnDrawOverlay(reshade::api::effect_runtime*) {
  auto& s = nr_runner::s;

  bool enabled = nr_runner::IsEnabled();
  if (ImGui::Checkbox("Enable Neural Rendering (F10)", &enabled)) nr_runner::SetEnabled(enabled);

  Edit model;  // create-time model settings: a release rebuilds the feature
  Edit bridge;  // colour bridge and compose: live

  {
    const char* source = s.source == nr_runner::kSrcDlss         ? "the game's DLSS session"
                         : s.source == nr_runner::kSrcStandalone ? "standalone"
                                                                 : "waiting for a frame";
    ImGui::Text("Source: %s", source);
    bridge |= Combo("Standalone##mode", &s.standalone_mode, "Auto\0Always\0Off\0", 0,
                    "For D3D12 games without DLSS: run off the back buffer, ReShade's depth "
                    "(Generic Depth) and iMMERSE Launchpad's motion vectors, with no bridge.\n"
                    "Auto runs it only when the game has no DLSS of its own.");
    if (s.sa_status[0] != 0 && s.source != nr_runner::kSrcDlss)
      ImGui::TextDisabled("Standalone: %s", s.sa_status);
  }

  if (ImGui::CollapsingHeader("Neural Rendering", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (s.style > 2) s.style = 2;
    model |= Combo("Style", &s.style, "Model A\0Model B\0Model C\0", 0,
                   "Selects Neural Rendering Model A, Model B, or Model C through the prerelease "
                   "DLSSNR.Style field.");
    model |= Slider("Overall Intensity", &s.intensity, 0.0f, 1.0f, "%.2f", 1.0f,
                    "DLSSNR.Intensity. The model's overall hand.");
    model |= Slider("Structure Intensity", &s.local_structure, 0.0f, 1.0f, "%.2f", 1.0f,
                    "DLSSNR.LocalStructureStrength: the fine detail the model synthesises.");
    model |= Slider("Local Tone Intensity", &s.local_tone, 0.0f, 1.0f, "%.2f", 1.0f,
                    "DLSSNR.LocalToneStrength: local contrast and tone.");
    model |= Checkbox("Auto Mask", &s.auto_mask,
                      "Allows independent control of characters via automatic masking "
                      "(UseAutoMask).");
    if (!s.auto_mask) ImGui::BeginDisabled();
    model |= Slider("Skin Structure Strength", &s.skin_structure, 0.0f, 1.0f, "%.2f", 1.0f,
                    "Controls structure intensity of detected characters (SkinStructureStrength).");
    if (!s.auto_mask) ImGui::EndDisabled();
    model |= Checkbox("UI Correction", &s.ui_correction,
                      "DLSSNR.UICorrection: lets the model protect UI it detects in the frame.");
    Edit passes;
    passes.changed = ImGui::SliderInt("Pass Count", &s.pass_count, 1, nr_runner::kMaxPasses);
    passes.committed = ImGui::IsItemDeactivatedAfterEdit();
    Tip("Runs the model this many times per frame, each pass refining the previous one's "
        "answer. Every pass costs a full model evaluation and its own VRAM.");
    if (ResetButton("Pass Count")) {
      s.pass_count = 1;
      passes.changed = passes.committed = true;
    }
    if (nr_runner::s.built_passes < s.pass_count && nr_runner::s.feature != nullptr)
      ImGui::TextDisabled("Running %d pass(es): see ReShade.log", nr_runner::s.built_passes);
    model |= passes;
  }

  if (ImGui::CollapsingHeader("Encoding", ImGuiTreeNodeFlags_DefaultOpen)) {
    bridge |= Combo("Encoding##source", &s.encoding,
                    "Auto\0Linear BT.709\0sRGB\0BT.2100 PQ\0scRGB\0scRGB-nl\0Measured (legacy)\0", 0,
                    "How the game's DLSS output is encoded. Auto treats native DLSS output as linear "
                    "BT.709, and a UNORM output (the dlss5-bridge on a D3D11 game) as sRGB.\n"
                    "Measured is the original fork's behaviour: an auto-exposure white point "
                    "measured every frame.");
    const int active = s.active_encoding;
    if (s.encoding == nr_runner::kEncAuto && active != nr_runner::kEncAuto) {
      ImGui::TextDisabled("Auto resolved to: %s", kEncodingNames[active]);
    }

    if (s.encoding == nr_runner::kEncMeasured) {
      bridge |= Slider("White point scale", &s.wp_scale, 0.1f, 4.0f, "%.2f", 1.0f,
                       "Multiplier on the measured white point.");
      if (s.wp_ema > 0.0f) {
        ImGui::Text("Measured white point: %.4f", s.wp_ema);
      } else {
        ImGui::TextUnformatted("Measured white point: (measuring...)");
      }
    } else {
      const int effective = (s.encoding == nr_runner::kEncAuto && active != nr_runner::kEncAuto)
                                ? active
                                : s.encoding;
      const float auto_nits = nr_runner::AutoDiffuseWhiteNits(effective);
      if (!s.diffuse_white_override) s.diffuse_white_nits = auto_nits;
      Edit dw;
      dw.changed = ImGui::SliderFloat("Diffuse White (nits)", &s.diffuse_white_nits, 40.0f, 500.0f,
                                      "%.0f");
      if (dw.changed) s.diffuse_white_override = true;
      Tip("Diffuse-white balance: the brightness the model is shown as paper white. Automatic "
          "values are 100 nits for linear BT.709 and sRGB, 250 nits for BT.2100 PQ and linear "
          "scRGB, and 203 nits for scRGB-nl.\nEnter a custom value directly, or reset to restore "
          "the current encoding's automatic value.");
      if (ResetButton("Diffuse White")) {
        s.diffuse_white_override = false;
        s.diffuse_white_nits = auto_nits;
        dw.changed = true;
      }
      bridge |= dw;
    }
  }

  if (ImGui::CollapsingHeader("Composition")) {
    bridge |= Slider("Detail strength", &s.transfer, 0.0f, 1.5f, "%.2f", 1.0f,
                     "How far the frame moves toward the model's answer. 0 = bypass.");
    bridge |= Slider("Colour strength", &s.colour_strength, 0.0f, 1.0f, "%.2f", 0.0f,
                     "0 keeps the game's own hue exactly; only brightness carries the model's "
                     "verdict. 1 takes the model's colour too.");
  }

  if (ImGui::CollapsingHeader("Debug")) {
    ImGui::Combo("Debug view", &s.debug_view,
                 "Off\0What the model sees\0Model answer\0Difference x20\0Motion vectors\0");
    ImGui::Checkbox("Temporal history", &s.temporal);
    Tip("Off resets the model's history every frame: no ghosting or drag from motion vectors, "
        "but less stable detail. A diagnostic: if drag disappears with this off, the motion "
        "vectors are the cause.");
    ImGui::BeginDisabled(s.source != nr_runner::kSrcStandalone);
    ImGui::Checkbox("Flip motion X", &s.sa_flip_x);
    Tip("Standalone only. Reverses the motion vectors' horizontal direction; the right setting is "
        "the one where moving things drag least.");
    ImGui::SameLine();
    ImGui::Checkbox("Flip motion Y", &s.sa_flip_y);
    Tip("Standalone only. Reverses the motion vectors' vertical direction.");
    ImGui::EndDisabled();
  }

  if (model.committed) nr_runner::ApplyModelSettings();
  if (model.changed || bridge.changed) SaveSettings();
}

}  // namespace

extern "C" __declspec(dllexport) const char* NAME = "DLSSNR Linux";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Runs NVIDIA DLSS 5 Neural Rendering (NGX feature 18) under Linux/Proton by driving the "
    "game-local snippet directly, with a display-referred colour bridge";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lp_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;
      reshade::log::message(reshade::log::level::info,
                            "DLSSNR Linux loaded (clang x86_64-pc-windows-msvc, built on Linux)");
      LogWineVersion();
      LoadSettings();
      reshade::register_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::register_event<reshade::addon_event::present>(OnPresent);
      reshade::register_event<reshade::addon_event::reshade_render_technique>(OnRenderTechnique);
      reshade::register_overlay("DLSSNR Linux", OnDrawOverlay);
      ngx_probe::TryInstall();  // in case NGX is already resident
      break;
    case DLL_PROCESS_DETACH:
      // Non-null lp_reserved: the process is terminating and every other
      // thread is already dead, possibly mid-call under a lock. Unpatching
      // code or waiting on threads here can deadlock the exit path, and the
      // whole address space is going away — do nothing. Null lp_reserved is a
      // real FreeLibrary (ReShade cycles the addon during device creation),
      // where the detours must be removed before this image disappears.
      if (lp_reserved != nullptr) break;
      ngx_probe::Uninstall();
      nr_d3d11::Shutdown();
      nr_runner::Shutdown();
      reshade::unregister_overlay("DLSSNR Linux", OnDrawOverlay);
      reshade::unregister_event<reshade::addon_event::reshade_render_technique>(OnRenderTechnique);
      reshade::unregister_event<reshade::addon_event::present>(OnPresent);
      reshade::unregister_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::unregister_addon(h_module);
      break;
  }
  return TRUE;
}
