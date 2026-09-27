# reshade-dlss5-linux

[![build](https://github.com/RianGoossens/reshade-dlss5-linux/actions/workflows/build.yml/badge.svg)](https://github.com/RianGoossens/reshade-dlss5-linux/actions/workflows/build.yml)
[![license: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)

A ReShade add-on that makes **NVIDIA DLSS 5 Neural Rendering** (DLSSNR — NGX feature 18)
run on **Linux / Proton**.

It drives the game-local `nvngx_dlssnr.dll` snippet **directly** as feature 18, through a tiny
forwarder DLL whose filename contains `nvngx.dll` to satisfy the snippet's caller gate, so it runs
without the driver's NGX dispatch or its OTA updater.

Highlights:

- **Encoding + Diffuse White.** The game's frame is decoded from its encoding (Auto, Linear
  BT.709, sRGB, BT.2100 PQ, scRGB, scRGB-nl) and normalised to a fixed diffuse white in nits, so
  what the model sees doesn't shift with scene brightness.
- **A gentle curve.** The model is shown the frame exactly up to 0.8x diffuse white; only
  highlights above that are rolled off. SDR content reaches the model almost untouched.
- **RenoDX-style controls**: Style, the model intensities, Auto Mask, Skin Structure Strength and
  UI Correction, applied as soon as a slider is released.
- **Pass Count**: run the model up to four times per frame, each pass refining the previous one.
- **Works in games without DLSS 5 support**, through
  [dlss5-bridge](https://github.com/NIGos/dlss5-bridge) (see below).

Built on Linux with clang targeting the MSVC ABI (`-target x86_64-pc-windows-msvc`) so the
vtables and by-value aggregate returns match MSVC-built ReShade.

## What it does

1. Hooks the game's NGX `CreateFeature` / `EvaluateFeature` (via Microsoft Detours) to learn the
   live DLSS-SR geometry and formats.
2. After each DLSS-SR evaluate, runs the DLSSNR model on that frame (once per pass) and composites
   its answer back over the game's output, so the game's own tone-mapping consumes the enhanced
   frame.
3. A colour bridge keeps the model, which was trained on display-referred data, from producing a
   veil, noise, or a colour cast. The frame is decoded from its encoding, normalised to diffuse
   white, and shown to the model display-referred. The model's answer is then anchored to the
   original, keeping the game's own hue by default.

## Settings

Open the ReShade overlay → *Add-ons* → **DLSSNR Linux**. **F10** toggles the pass for A/B
comparison. Settings are saved to `ReShade.ini` under `[ADDON_DLSSNR_LINUX]`.

**Neural Rendering** (the model's own parameters; changing one rebuilds the model on release)

| Setting | Parameter | Notes |
|---|---|---|
| Style | `DLSSNR.Style` | Model A, B or C. They look distinctly different. |
| Overall Intensity | `DLSSNR.Intensity` | 0–1. The model caps it at 1. |
| Structure Intensity | `DLSSNR.LocalStructureStrength` | Fine detail the model synthesises. |
| Local Tone Intensity | `DLSSNR.LocalToneStrength` | Local contrast and tone. |
| Auto Mask | `DLSSNR.UseAutoMask` | Detects characters so they can be treated separately. |
| Skin Structure Strength | `DLSSNR.SkinStructureStrength` | Structure on detected characters (needs Auto Mask). |
| UI Correction | `DLSSNR.UICorrection` | Lets the model protect UI it detects. |
| Pass Count | — | 1–4. Each pass is its own model instance fed the previous pass's answer. Every pass costs a full evaluation and its own VRAM. |

RenoDX's *Global Tone Intensity* and the render preset hint are not exposed: the tested
`nvngx_dlssnr.dll` never reads `GlobalToneStrength`, and it embeds a single weight set, so every
preset resolves to the same model.

**Encoding**

| Setting | Notes |
|---|---|
| Encoding | How the game's DLSS output is encoded. *Auto* picks linear BT.709 for native DLSS output (float formats) and sRGB for 8/10-bit UNORM output, such as a D3D11 game through [dlss5-bridge](https://github.com/NIGos/dlss5-bridge). *Measured (legacy)* instead measures the frame's average brightness every frame and uses it as the white point. |
| Diffuse White (nits) | The brightness shown to the model as paper white. Defaults as in RenoDX: 100 for linear and sRGB, 250 for PQ and scRGB, 203 for scRGB-nl. |

**Composition**

| Setting | Notes |
|---|---|
| Detail strength | How far the frame moves toward the model's answer. 0 = bypass. |
| Colour strength | 0 keeps the game's own hue; only brightness carries the model's change. 1 takes the model's colour too. |
| Debug view | What the model sees, the model's answer, or the difference ×20. |

## Installing the build

You need two files — `dlssnr-linux.addon64` (the add-on) and `nvngx.dll_nrfwd.dll` (the
forwarder). Grab them from a [GitHub Release](../../releases) (every version tag attaches both;
every push also uploads them as a downloadable CI artifact), or build them yourself (see below —
`build.sh` copies them straight into the game folder for you).

See NapXDD's [**Tested Games**](https://github.com/NapXDD/addon-dlssnr-linux/wiki/Tested-Games) wiki
page for titles this has been tried in — and please add your own results there.

### Prerequisites

- An **RTX 50-series or RTX 40-series** GPU, with a recent NVIDIA driver branch that supports
  DLSS Neural Rendering. The `nvngx_dlssnr.dll` model supports both generations; this add-on has
  only been tested on RTX 50 so far, but is expected to work on RTX 40 — reports welcome.
- **Proton** with NVAPI/NGX enabled, and **DLSS (Super Resolution) turned on in-game**: this add-on
  runs off the game's DLSS-SR output, so DLSS must be active.
- **ReShade with add-on support** installed for the game's DX12 renderer (the `dxgi` variant).
  D3D11 games work through [dlss5-bridge](https://github.com/NIGos/dlss5-bridge), which gives them
  a D3D12 DLSS session to hook; keep its `unwrap=0`.
- The DLSS Neural Rendering model **`nvngx_dlssnr.dll`** present beside the game executable. It is
  NVIDIA's and is *not* shipped here; the add-on only drives it.
  **The model build matters:** the add-on has only run stably with this exact model —
  `sha256 e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e` (165,840,496 bytes).
  A different model version may not fail cleanly: in testing, a mismatched model reported Success
  on every evaluate and then **crashed the game minutes into gameplay**. The add-on logs the
  model's version and SHA-256 to `ReShade.log` at startup (`nr-fwd: model nvngx_dlssnr.dll ...`)
  and warns when it isn't the tested build.

**Tested environments:**

- addon-dlssnr-linux (NapXDD): NVIDIA GeForce RTX 5070 · Linux driver **610.57.04** · Fedora · KDE Plasma 6 (**X11**
  session, kwin 6.7.3) · Proton.
- This fork: RTX 5070 Ti Laptop · CachyOS · proton-cachyos-slr. Dark Souls Remastered (D3D11,
  dlss5-bridge substitute session) and High on Life (native D3D12 DLSS).

Other RTX 50/40 cards, drivers, and compositors are expected to work but are untested — see NapXDD's [Tested Games](https://github.com/NapXDD/addon-dlssnr-linux/wiki/Tested-Games)
wiki and please report your own setup.

> **First, make sure the game itself runs on Proton.** Check
> [ProtonDB](https://www.protondb.com/) for the game's rating before trying this add-on — if the
> game isn't Playable/Gold/Platinum (or is blocked by anti-cheat) under Proton, the add-on can't
> help. This add-on assumes the game already launches and runs under Proton with DLSS working.

### Steps

1. Install ReShade (add-on support enabled) for the game. Its `dxgi.dll` and `ReShade.ini` should
   sit in the same folder as the game executable.
2. Copy **both** `dlssnr-linux.addon64` and `nvngx.dll_nrfwd.dll` into that folder, next to the
   game executable and `nvngx_dlssnr.dll`.
3. Set Steam launch options so Proton exposes NVAPI/NGX and loads ReShade's `dxgi`. The NVAPI
   variable depends on which Proton build you run:

   **Valve Proton** (Steam's built-in):

   ```
   PROTON_ENABLE_NVAPI=1 WINEDLLOVERRIDES="dxgi=n,b" %command%
   ```

   **GE-Proton / proton-cachyos** (`PROTON_ENABLE_NVAPI` does not exist on these builds):

   ```
   PROTON_FORCE_NVAPI=1 WINEDLLOVERRIDES="dxgi=n,b" %command%
   ```

   See NapXDD's [**Launch Options**](https://github.com/NapXDD/addon-dlssnr-linux/wiki/Launch-Options)
   wiki page for the full story: what each variable does per Proton build, the
   `d3dcompiler_47` override for ReShade effects, how to verify from `ReShade.log`, and the
   logging line to use when reporting a crash.

4. Launch the game, enable **DLSS** in the graphics settings, then open the ReShade overlay
   (**Home** key) → **Add-ons** tab → **DLSSNR Linux**. Press **F10** any time to A/B toggle the
   pass. `ReShade.log` beside the exe records `nr-fwd:` lines if you need to check it loaded.

> ⚠️ Third-party add-ons in an online game with anti-cheat carry a risk to your account. Use at
> your own risk.

## Games without DLSS 5 support

The add-on hooks a D3D12 DLSS Super Resolution session, which only native D3D12 games with DLSS
have. Everything else goes through [dlss5-bridge](https://github.com/NIGos/dlss5-bridge), a
ReShade add-on that gives the game a private D3D12 DLSS session for this add-on to hook. Download
it only from its GitHub releases, and put `dlss5-bridge.addon64` beside the game executable with
this add-on's files.

In `dlss5-bridge.cfg` (written on first launch) always set:

```
unwrap=0
```

With the default `unwrap=1`, the bridge delivers no frames to this add-on under Proton.

### D3D11 or Vulkan games with DLSS

The bridge mirrors the game's own DLSS onto its D3D12 session automatically. Turn DLSS on in the
game. *Encoding → Auto* picks the right decode for the bridge's output. This route hasn't been
tested with this add-on yet; reports welcome.

### Games without any DLSS

The bridge can build a substitute DLAA session from the frame, ReShade's depth, and motion vectors.

1. Copy an **`nvngx_dlss.dll` of version 3.1.13 or newer** from any game that ships DLSS into
   the game folder. The game has none, and the driver doesn't supply one there.
2. Install [**iMMERSE**](https://github.com/martymcmodding/iMMERSE) from Marty's Mods (the
   ReShade installer offers it) and enable the **MartysMods_Launchpad** effect. Launchpad
   computes the motion vectors the bridge feeds to DLSS. NVIDIA's hardware optical flow, the
   bridge's other motion source, isn't available under Proton.
3. In `dlss5-bridge.cfg` set:

   ```
   synth=1
   ofa_grid=0
   unwrap=0
   ```

   `synth=1` (*Replace DLSS when the game isn't using its own* in the bridge's panel) enables the
   substitute session, and `ofa_grid=0` takes motion vectors from the ReShade shader instead of
   optical flow.
4. Make sure ReShade is using the right depth buffer. ReShade's built-in **Generic Depth** add-on
   (overlay → *Add-ons* tab) lists every depth buffer the game draws to; if it picks the wrong one,
   tick the correct one there (the one with the most draw calls is usually it). Enable the
   **DisplayDepth** effect to check: adjust ReShade's depth settings (upside down, reversed,
   logarithmic) until near objects are dark and far ones light, then disable it again. The
   bridge's panel shows whether depth and motion inputs are bound.

Tested under Proton with Dark Souls Remastered (D3D11). The substitute is a real DLSS session fed
approximated inputs, so text can soften and dense
foliage can smear a little.

## Building

Install the toolchain and dependencies once (LLVM/clang + LLD, the xwin-provided MSVC SDK at
`~/.xwin`, a standalone Linux DXC at `~/.local/dxc`, and the RenoDX checkout at `~/projects/renodx`
for the ReShade / DLSS / Detours / ImGui headers):

```bash
bash install-deps.sh
```

It's idempotent — it skips anything already present. Then build:

```bash
bash build.sh
```

`install-deps.sh` supports dnf / apt / pacman / zypper for the system packages (needs `sudo` for
those) and downloads the rest into your home directory. See `build.sh` for the exact paths it
expects.

`build.sh` writes the two files to `build/` — it is a pure build, with no deploy step. To have a
local build drop straight into your game or testbed folders, create an executable
`deploy.local.sh` next to it (untracked; `build.sh` runs it after a successful build if present)
that copies `build/dlssnr-linux.addon64` and `build/nvngx.dll_nrfwd.dll` wherever you need them.
`build.sh --test` compiles the e2e test hooks in and marks the output (`build/.test-build`) so a
deploy script can keep test builds out of game folders — `test/e2e-preset-crash.sh` uses that
build to regression-test the retire/rebuild path against the DLSS SDK sample app. See
[`test/README.md`](test/README.md) for the testbed setup and how to run it.

## Issues & support

Hit a problem, or got it working somewhere new? Please
[**open an issue**](https://github.com/RianGoossens/reshade-dlss5-linux/issues). Include your GPU, NVIDIA driver version, Proton build, the game, and any relevant
`nr-fwd:` lines from `ReShade.log` — especially the `nr-fwd: model nvngx_dlssnr.dll ...` line,
which identifies the model build you were running.

## Credits & acknowledgements

> **This project exists thanks to [NapXDD](https://github.com/NapXDD)'s
> [addon-dlssnr-linux](https://github.com/NapXDD/addon-dlssnr-linux).** NapXDD did the hard part:
> getting feature 18 to run under Proton at all. The forwarder, the NGX hooks, feature creation
> and the compose pipeline are all their work; this project only builds on top of it. NapXDD's
> add-on in turn builds on the Neural Rendering recipe from
> [Dagherbou's OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR), itself a fork of
> [OptiScaler](https://github.com/optiscaler/OptiScaler).
>
> Lineage: OptiScaler → OptiScaler_DLSSNR (Dagherbou) → addon-dlssnr-linux (NapXDD) → this fork.
>
> The file names (`dlssnr-linux.addon64`, `nvngx.dll_nrfwd.dll`) and the `[ADDON_DLSSNR_LINUX]`
> ini section are NapXDD's, kept so this drops in over an addon-dlssnr-linux install; old
> settings are migrated.

This project was studied from, and stands on, the following work. Please support the originals.

- **addon-dlssnr-linux** by NapXDD (<https://github.com/NapXDD/addon-dlssnr-linux>) — GPL-3.0.
  This is a fork of it: the forwarder, the NGX hooks, feature creation and the compose pipeline are
  NapXDD's work.

- **OptiScaler** and the **OptiScaler_DLSSNR** fork
  (<https://github.com/Dagherbou/OptiScaler_DLSSNR>, <https://github.com/optiscaler/OptiScaler>) —
  **GPL-3.0**. The DLSSNR-as-feature-18 recipe comes from here: the forwarder caller-gate trick,
  reuse of the driver core's capability parameter block, discovering the parameter setter vtable
  slots by round-tripping a value, the `DLSSNR.*` parameter names, the subrect/guide wiring, and
  the model Style names. **Because this add-on is derived from GPL-3.0 code, it is licensed
  GPL-3.0 too** (see [LICENSE](LICENSE)).
- **RenoDX** by Carlos Lopez Jr. (<https://github.com/clshortfuse/renodx>) — MIT. The ReShade
  add-on approach, the DLSS5 colour-bridge composition concepts (display-referred encode, anchored
  resolve), the Encoding / Diffuse White model and its defaults, and the layout of the Neural
  Rendering controls.
- **ReShade** by Patrick Mours (<https://github.com/crosire/reshade>) — the add-on SDK / API this
  loads into.
- **Microsoft Detours** (<https://github.com/microsoft/Detours>) — MIT. Used to hook NGX.
- **Dear ImGui** by Omar Ocornut (<https://github.com/ocornut/imgui>) — MIT. Overlay UI.
- **NVIDIA NGX / DLSS SDK** — headers only, used under NVIDIA's SDK licence. The DLSS runtime and
  models are NVIDIA's; this add-on ships none of them.

## AI assistance

AI coding tools were used in the making of this fork.

## Disclaimer

Unofficial, unaffiliated with NVIDIA. Use of third-party add-ons in online games can carry a risk
of anti-cheat action on your account — use at your own risk.
