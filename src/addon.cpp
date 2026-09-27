// DLSSNR Linux — ReShade addon entry point.
// Registers the overlay and present hook, logs swapchain ground truth
// (color space, back buffer format, dimensions, Wine/Proton detection),
// and installs the NGX CreateFeature/EvaluateFeature interception
// (ngx_probe.hpp) that the runner and colour bridge are driven by.

#include <windows.h>

#include <cstdio>

#pragma comment(lib, "user32")  // GetAsyncKeyState for the F10 toggle

// ImGui before reshade.hpp so the overlay function table lights up; ImTextureID must be 8 bytes
// to match reshade::api::resource_view.
#define ImTextureID ImU64
#define ImDrawIdx unsigned int
#include <imgui.h>

#include <reshade.hpp>

#include "ngx_probe.hpp"

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
  if (s.style < 0 || s.style > 2) s.style = 0;
}

void SaveSettings() {
  auto& s = nr_runner::s;
  reshade::set_config_value(nullptr, kConfigSection, "DirectNeuralRenderingEncoding", s.encoding);
  reshade::set_config_value(nullptr, kConfigSection, "DirectNeuralRenderingPassCount", s.pass_count);
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
    ImGui::Combo("Debug view", &s.debug_view,
                 "Off\0What the model sees\0Model answer\0Difference x20\0");
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
      nr_runner::Shutdown();
      reshade::unregister_overlay("DLSSNR Linux", OnDrawOverlay);
      reshade::unregister_event<reshade::addon_event::present>(OnPresent);
      reshade::unregister_event<reshade::addon_event::init_swapchain>(OnInitSwapchain);
      reshade::unregister_addon(h_module);
      break;
  }
  return TRUE;
}
