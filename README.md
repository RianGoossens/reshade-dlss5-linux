# reshade-dlss5-linux

[![build](https://github.com/RianGoossens/reshade-dlss5-linux/actions/workflows/build.yml/badge.svg)](https://github.com/RianGoossens/reshade-dlss5-linux/actions/workflows/build.yml)
[![license: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)

Maintained by [Rian Goossens](https://github.com/RianGoossens).

A ReShade add-on that makes **NVIDIA DLSS 5 Neural Rendering** (DLSSNR — NGX feature 18)
run on **Linux / Proton**.

It drives the game-local `nvngx_dlssnr.dll` snippet **directly** as feature 18, through a tiny
forwarder DLL whose filename contains `nvngx.dll` to satisfy the snippet's caller gate, so it runs
without the driver's NGX dispatch or its OTA updater.

Highlights:

- **Encoding, white point and curve, independently.** The game's frame is decoded from its
  encoding (Auto, Linear BT.709, sRGB, BT.2100 PQ, scRGB, scRGB-nl), divided by a white point
  that is either a fixed diffuse white in nits or the frame's own measured average, and shown
  through a knee or Reinhard curve. Measured is what a scene-referred buffer needs: its scale
  moves with the scene, so no constant white point tracks it.
- **A gentle curve.** The knee shows the model the frame exactly up to 0.8x diffuse white; only
  highlights above that are rolled off. SDR content reaches the model almost untouched.
  Reinhard, the other choice, compresses smoothly everywhere and reaches ~450x white.
- **RenoDX-style controls**: Style, the model intensities, Auto Mask, Skin Structure Strength and
  UI Correction, applied as soon as a slider is released.
- **Pass Count**: run the model up to four times per frame, each pass refining the previous one.
- **Works in games without DLSS at all.** D3D12, D3D11 and Vulkan games run in **standalone
  mode** off ReShade's depth and iMMERSE Launchpad's motion vectors, with nothing else to install.
  D3D11 and Vulkan games with their own DLSS can go through
  [dlss5-bridge](https://github.com/NIGos/dlss5-bridge) (see below).

Built on Linux with clang targeting the MSVC ABI (`-target x86_64-pc-windows-msvc`) so the
vtables and by-value aggregate returns match MSVC-built ReShade.

## What it does

1. Finds a frame to work on:
   - **A DLSS session.** Hooks the game's NGX `CreateFeature` / `EvaluateFeature` (via Microsoft
     Detours) and, around each DLSS-SR evaluate, takes the frame -- its color input before the
     upscale or its output after it, per *Model resolution* -- plus depth and motion vectors.
     This is how native DLSS games and games through dlss5-bridge run.
   - **An FSR3 session.** Hooks `ffxDispatch` on the FidelityFX loader / upscaler DLLs the game
     already ships (`amd_fidelityfx_loader_dx12.dll` / `amd_fidelityfx_upscaler_dx12.dll`,
     D3D12 only) and takes the frame from the dispatch parameters -- its color input before the
     FSR upscale or its output after it, per *Model resolution* -- plus depth and motion vectors,
     with no ReShade depth or Launchpad setup of its own.
   - **Standalone.** In a game without DLSS (D3D12, D3D11 or Vulkan), takes the back buffer, ReShade's depth and
     iMMERSE Launchpad's motion vectors right after Launchpad runs, and starts NGX itself.
2. Runs the DLSSNR model on that frame (once per pass) and composites its answer back. With DLSS
   the answer returns either before the game's own DLSS-SR upscale (which then scales the
   enhanced frame) or after it, so the game's post-processing consumes the enhanced result;
   standalone writes it to the back buffer.
3. A colour bridge keeps the model, which was trained on display-referred data, from producing a
   veil, noise, or a colour cast. The frame is decoded from its encoding, divided by the chosen
   white point (a fixed diffuse white, or the frame's own measured average), and shown to the
   model display-referred through the chosen curve. The model's answer is then anchored to the
   original, keeping the game's own hue by default.

## Settings

Open the ReShade overlay → *Add-ons* → **DLSSNR Linux**. **F10** toggles the pass for A/B
comparison. Settings are saved to `ReShade.ini` under `[ADDON_DLSSNR_LINUX]`.

At the top, **Source** shows where the frame comes from (the game's DLSS session, its FSR3
session, or standalone) and picks it: *Auto* walks the priority DLSS → FSR3 → standalone — a DLSS
evaluate or FSR3 dispatch within the last 3 seconds claims the frame, and standalone arms 10
seconds after start when neither showed up — or *Force DLSS*, *Force FSR3* and *Force standalone*
pick one outright. Its status line says what it is waiting for.

**Neural Rendering** (the model's own parameters; changing one rebuilds the model on release)

| Setting | Parameter | Notes |
|---|---|---|
| Model resolution | `ModelResolution` | *Full* (default) runs the model on the full-resolution frame. *Render* runs it at the resolution the game renders its scene at, where that is lower: much cheaper, at some loss of fine detail. In a game with DLSS, *Render* runs the model on the frame before DLSS-SR upscales it, at your DLSS preset's render resolution; DLSS-SR then upscales the enhanced frame and smooths some of the detail the model adds (RenoDX's *Render* hook; *Full* is its *Upscaled/Present*). In standalone it applies when the depth buffer is smaller than the screen (see [Depth and motion vectors](#depth-and-motion-vectors)). |
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

**Colour bridge**

Three independent choices. They used to be one bundled mode, which meant a buffer whose scale
moves with the scene could only ever be right in one of them. Configs saved with that old mode
(`Encoding=6`) are migrated on first load to *Auto* + *Measured* + *Reinhard*; a UNORM game gains
the sRGB decode it should have had all along.

| Setting | Notes |
|---|---|
| Encoding | How to decode the buffer to linear. *Auto* picks linear BT.709 for native DLSS output (float formats) and sRGB for 8/10-bit UNORM output, such as a D3D11 game through [dlss5-bridge](https://github.com/NIGos/dlss5-bridge). Independent of the two settings below: changing it leaves them alone, so an A/B across encodings compares the encoding. |
| Diffuse White (nits) + *Measured* | What the encode divides by. The slider sets a constant; the *Measured* checkbox beside it swaps the source for the frame's own average brightness, re-anchored every frame (white = average ÷ 0.18, the standard auto-exposure key, so the average reads as mid-grey under either curve), and disables the slider. Measured is the only source that survives a scene-referred buffer, whose scale can swing several times within one session — a constant on such a buffer leaves the model looking at a near-black frame in dark areas, and its answer there is noise. The measurement runs either way, so the toggle is instant. Defaults as in RenoDX: 100 nits for linear and sRGB, 250 for PQ and scRGB, 203 for scRGB-nl. |
| White point scale | *Measured* only: a multiplier on the measured white point, for scenes auto-exposure gets wrong (mostly dark or mostly bright). |
| Curve | How the frame is compressed for the model. *Knee* passes everything below 0.8× white through untouched and rolls off above, but represents only up to about 2× white — it assumes content already clamped to white, so scene-referred highlights pile up at its cap. *Reinhard* compresses smoothly everywhere and reaches about 450× white. |

**Composition**

| Setting | Notes |
|---|---|
| Detail strength | How far the frame moves toward the model's answer. 0 = bypass. |
| Colour strength | 0 keeps the game's own hue; only brightness carries the model's change. 1 takes the model's colour too. |

**Debug**

| Setting | Notes |
|---|---|
| Debug view | What the model sees, the model's answer, the difference ×20, or the motion vectors it gets (red horizontal, green vertical, depth in blue). |
| Temporal history | Off resets the model's history every frame. Drag that disappears with it off comes from the motion vectors. |
| Flip motion X / Y | Standalone only: reverse a motion-vector axis. The right setting is the one where moving things drag least. |

## Installing the build

You need two files — `dlssnr-linux.addon64` (the add-on) and `nvngx.dll_nrfwd.dll` (the
forwarder). Grab them from a [GitHub Release](../../releases) (every version tag attaches both;
every push also uploads them as a downloadable CI artifact), or build them yourself (see below —
`build.sh` copies them straight into the game folder for you).

See the [**Tested Games**](https://github.com/RianGoossens/reshade-dlss5-linux/wiki/Tested-Games) wiki page for games confirmed working, with the settings each
one needs.

### Prerequisites

- An **RTX 50-series or RTX 40-series** GPU, with a recent NVIDIA driver branch that supports
  DLSS Neural Rendering. The `nvngx_dlssnr.dll` model supports both generations; the add-on has
  been tested on both — RTX 50-series in the original sessions, and an RTX 4090 running Control.
- **Proton** with NVAPI/NGX enabled. In a game with DLSS, **turn DLSS (Super Resolution) on
  in-game**: the add-on runs off its output. Games without DLSS: see
  [Games without DLSS 5 support](#games-without-dlss-5-support).
- **ReShade with add-on support** installed for the game's renderer: the `dxgi` variant for
  D3D12, `d3d11` for D3D11, the layer for Vulkan (see
  [Vulkan games under Proton](#vulkan-games-under-proton)).
- The DLSS Neural Rendering model **`nvngx_dlssnr.dll`** present beside the game executable. It is
  NVIDIA's and is *not* shipped here; the add-on only drives it.
  **The model version matters:** the add-on has been tested with model **310.8.0**. A different
  model version may not fail cleanly: in testing, a mismatched model reported Success on every
  evaluate and then **crashed the game minutes into gameplay**. The add-on logs the model's
  version and SHA-256 to `ReShade.log` at startup (`nr-fwd: model nvngx_dlssnr.dll ...`); include
  that line when reporting a problem.

Tested on RTX 50- and RTX 40-series cards under Proton. The [Tested Games](https://github.com/RianGoossens/reshade-dlss5-linux/wiki/Tested-Games) wiki page lists the games
and setups confirmed working.

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

   **GE-Proton / proton-cachyos** use `PROTON_FORCE_NVAPI` instead:

   ```
   PROTON_FORCE_NVAPI=1 WINEDLLOVERRIDES="dxgi=n,b" %command%
   ```

   For a D3D11 game, where ReShade is installed as `d3d11.dll`, override that instead:
   `WINEDLLOVERRIDES="d3d11=n,b"`.

   See NapXDD's [**Launch Options**](https://github.com/NapXDD/addon-dlssnr-linux/wiki/Launch-Options)
   wiki page for the full story: what each variable does per Proton build, the
   `d3dcompiler_47` override for ReShade effects, how to verify from `ReShade.log`, and the
   logging line to use when reporting a crash.

4. Launch the game, enable **DLSS** in the graphics settings if it has it, then open the ReShade overlay
   (**Home** key) → **Add-ons** tab → **DLSSNR Linux**. Press **F10** any time to A/B toggle the
   pass. `ReShade.log` beside the exe records `nr-fwd:` lines if you need to check it loaded.

> ⚠️ Third-party add-ons in an online game with anti-cheat carry a risk to your account. Use at
> your own risk.

## Games without DLSS 5 support

Native D3D12 games with DLSS need nothing else. For the rest:

| Game | Route |
|---|---|
| D3D12 game with FSR3 | **FSR3 session**: this add-on only, no ReShade depth or Launchpad setup needed; Source → *Auto* picks it when the game has no DLSS. |
| D3D12, D3D11 or Vulkan, no DLSS | **Standalone mode**: this add-on plus iMMERSE Launchpad. |
| D3D11 or Vulkan, with DLSS | dlss5-bridge mirrors the game's DLSS (optional: standalone works too). |

Standalone needs ReShade's depth and Launchpad's motion vectors set up, as described under
[Depth and motion vectors](#depth-and-motion-vectors).

### Games without DLSS: standalone mode

1. Install ReShade for the game's API (`dxgi.dll` for D3D12, `d3d11.dll` for D3D11, the layer for
   Vulkan: see [Vulkan games under Proton](#vulkan-games-under-proton)) and this add-on's two
   files beside the executable. No `nvngx_dlss.dll` and no bridge are needed; while
   dlss5-bridge is installed, *Auto* leaves the game to it.
2. Set up depth and Launchpad as below.
3. Get into gameplay: after about 10 seconds *Auto* starts standalone, and the panel's Source
   line reads **standalone**.

In a D3D12 game the pass runs on the game's own device. A D3D11 or Vulkan game's frame, depth and
motion vectors are copied to a private D3D12 device each frame and the result is copied back. In a
Vulkan game the game's GPU queue waits on an event while the pass runs, so the result lands in
the same frame.

#### Vulkan games under Proton

Proton's built-in Vulkan loader does not load Windows layers, and ReShade on Vulkan is one. So:

1. Run the ReShade installer for the game with **Vulkan** selected. It puts `ReShade64.dll` and
   `ReShade64.json` beside the executable.
2. Put a native 64-bit Khronos `vulkan-1.dll` beside the executable (from the Vulkan Runtime, or
   from another game that ships one) and add `WINEDLLOVERRIDES="vulkan-1=n"` to the launch
   options.
3. Register the layer in the game's prefix. Wine runs games elevated, so the loader ignores layer
   environment variables and reads only the machine-wide registry key:

   ```
   protontricks -c 'wine reg add "HKLM\Software\Khronos\Vulkan\ImplicitLayers" /v "Z:\path\to\game\ReShade64.json" /t REG_DWORD /d 0 /f' <appid>
   ```

   with the game folder's path written as a Windows path (`Z:` is `/`).

The add-on asks Launchpad for its motion vectors itself; Launchpad computes them only when some
effect requests them. If the GPU ever stops finishing frames, standalone switches itself off
after 3 seconds rather than hang the game, and says so in the panel.

### Games with their own DLSS on D3D11 or Vulkan: dlss5-bridge

[dlss5-bridge](https://github.com/NIGos/dlss5-bridge) is a ReShade add-on that gives the game a
private D3D12 DLSS session for this add-on to hook. Download it only from its GitHub releases,
and put `dlss5-bridge.addon64` beside the game executable with this add-on's files.

In `dlss5-bridge.cfg` (written on first launch) always set:

```
unwrap=0
```

With the default `unwrap=1`, the bridge delivers no frames to this add-on under Proton.

#### With the game's own DLSS

The bridge mirrors the game's own DLSS onto its D3D12 session automatically. Turn DLSS on in the
game. *Encoding → Auto* picks the right decode for the bridge's output.

#### Without DLSS (superseded by standalone mode)

The bridge can build a substitute DLAA session from the frame, ReShade's depth, and motion vectors.

1. Copy an **`nvngx_dlss.dll` of version 3.1.13 or newer** from any game that ships DLSS into
   the game folder. The game has none, and the driver doesn't supply one there.
2. Set up depth and Launchpad as below. NVIDIA's hardware optical flow, the bridge's other motion
   source, isn't available under Proton.
3. In `dlss5-bridge.cfg` set:

   ```
   synth=1
   ofa_grid=0
   unwrap=0
   ```

   `synth=1` (*Replace DLSS when the game isn't using its own* in the bridge's panel) enables the
   substitute session, and `ofa_grid=0` takes motion vectors from the ReShade shader instead of
   optical flow.
The bridge's panel shows whether depth and motion inputs are bound. The substitute is a real DLSS
session fed approximated inputs, so text can soften and dense foliage can smear a little.

### Depth and motion vectors

None of this applies to an **FSR3 session**: the add-on takes the game's color, depth and motion
vectors straight from the FSR3 dispatch parameters, so there is nothing here to set up.

1. Install [**iMMERSE**](https://github.com/martymcmodding/iMMERSE) from Marty's Mods (the
   ReShade installer offers it), enable **MartysMods_Launchpad** and move it to the top of the
   effect list. Launchpad estimates motion vectors from the image.
2. Make sure ReShade uses the right depth buffer. ReShade's built-in **Generic Depth** add-on
   (overlay → *Add-ons* tab) lists every depth buffer the game draws to; if it picks the wrong
   one, tick the screen-sized one with the most vertices. Several can show the same draw calls;
   the vertex count tells them apart. Never a square shadow map.
3. Enable the **DisplayDepth** effect to check, and adjust ReShade's global preprocessor
   definitions until near objects are dark and far ones light, then disable it again:
   - `RESHADE_DEPTH_INPUT_IS_REVERSED=1` for most modern engines (Unreal included).
   - `RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=1` if the image is upside down.
   - **Upscaler on (FSR, TSR, a resolution scale)?** The game then draws depth into the top-left
     part of the buffer. Set `RESHADE_DEPTH_INPUT_X_SCALE` and `RESHADE_DEPTH_INPUT_Y_SCALE` to
     1 / the render scale: 1.5 for Quality, 1.724 for Balanced, 2 for Performance, 3 for Ultra
     Performance. Turn dynamic resolution off. Launchpad and the add-on both follow these.
   The add-on follows `IS_REVERSED` and `IS_UPSIDE_DOWN` too (the flip in D3D11 and Vulkan
   standalone).
   - **Depth buffer smaller than the screen?** Some games render the scene into their own smaller
     buffers and draw the UI at full size; Detroit: Become Human keeps a desktop-sized swapchain
     whatever its resolution setting. With **Model resolution → Render**, standalone runs the model
     at the depth buffer's resolution and adds only the model's change, scaled up, onto the
     full-resolution frame: the UI stays sharp and the model costs what it would at the lower
     resolution. The status line reads e.g. *running, 3840x2160 (model at 2560x1440)*, and
     lowering such a game's resolution setting becomes a real performance control. With *Full*,
     depth is scaled up to the screen and the model runs on the full frame.
4. Keep ReShade's **Performance Mode** and **Skip Loading Disabled Effects** off. The bridge reads
   depth through a loaded effect that uses it; with either on, only enabled effects load.
5. Check *Debug → Debug view → Motion vectors*: things that move show red and green.

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
> Lineage: OptiScaler → OptiScaler_DLSSNR (Dagherbou) → addon-dlssnr-linux (NapXDD) → this fork
> (Rian Goossens).
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
- **dlss5-bridge** by NIGos (<https://github.com/NIGos/dlss5-bridge>) — MIT. The D3D11 and Vulkan
  standalone transports follow its recipes for sharing textures with a private D3D12 device under
  Proton: for D3D11 a shared fence, the depth conversion and the 12_x feature level; for Vulkan
  the import of D3D12 textures into the game's device and the event "park" that orders the two
  APIs.
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
