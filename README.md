# MW4 Ray Reconstruction

A Windows x64 ReShade addon integrating NVIDIA DLSS Ray Reconstruction into the MW4 campaign's Direct3D 12 rendering path. Includes Transparency and Subsurface Scattering guides, live RR preset selection, and a ReShade status panel.

**Beta 2** is the current public beta. It is a beta mostly in name: in the author's own playtesting, roughly 98% of the noise seen before is gone, but some issues may not have been caught yet. That is a personal impression from gameplay, not a measurement, and results vary by scene, hardware, and settings. See the requirements and limitations below before installing or building.

### What's new in Beta 2

- Improved Subsurface Scattering lighting: skin and other subsurface materials use the game's native temporal lighting history where it exists, and a spatial estimate elsewhere.
- Transparency albedo blend: texture seen through glass, particles, and refraction stays steadier while moving.
- Specular hit distance: RR receives the game's specular ray hit distances.
- Weapon depth: the weapon and hands keep their own depth instead of appearing to RR as a flat card.
- Water responsivity mask (preset F): running water is less smeared.
- Compatibility with third-party DLSS add-ons such as DLSS5 NR, together with Ray Reconstruction, frame generation, and Super Resolution replacement. Pre-rendered cutscenes are no longer filtered through such an add-on.
- RR keeps running under heavy GPU memory use: a frame that cannot get memory is skipped and retried instead of turning RR off.

## Campaign only - anti-cheat warning

**This addon has not been tested at all in Multiplayer or DMZ. Those modes use anti-cheat: do not install or load this addon in either mode.** Install it only for the campaign executable, `sp26-cod.exe`, never for COD HQ, Multiplayer, or DMZ executables.

Campaign-only use is **not a guarantee of anti-cheat safety or exemption from enforcement**. This project does not claim Activision approval or that the campaign executable is ignored by anti-cheat. Use at your own risk. To the extent permitted by applicable law, the author and contributors accept no liability for account bans, suspensions, lost access, or other consequences of use. See [Activision's Security and Enforcement Policy](https://support.activision.com/articles/call-of-duty-security-and-enforcement-policy).

## Features and controls

Ray Reconstruction, the Transparency Guide, and the Subsurface Scattering Guide are enabled by default. Preset F is the initial preset. Preferences persist in `ReShade.ini`.

| Control | Function |
| --- | --- |
| F8 | Toggle Ray Reconstruction |
| F9 | Toggle the Transparency Guide |
| F11 | Toggle the Subsurface Scattering Guide |
| ReShade Add-ons panel | Feature toggles, status, and live Default/D/E/F preset selection |

F8, F9, and F11 are ignored while Ctrl or Alt is held, so NVIDIA's development DLL debug hotkeys (Ctrl+Alt+F9 and Ctrl+Alt+F11) do not toggle guides.

DLSS quality is configured through `DLSSPerfModeMP` in the game's configuration and requires a restart. Preset changes are handled separately and can apply during gameplay.

Transparency albedo blend (on by default, with a checkbox in the panel and no hotkey) moves RR's albedo guides toward neutral values where the game's transparency layers (OIT glass and particles, and screen-space refraction) cover a pixel, so texture seen through glass is kept while moving. It is controlled by the `TransparencyBlend` key in `ReShade.ini`. Running water is not affected. Optional tuning keys, not shown in the panel:

- `TransparencyBlendDiffuse` (0.5): the diffuse albedo value the guide moves toward.
- `TransparencyBlendSpecular` (0): the specular albedo value the guide moves toward.
- `TransparencyBlendRoughness` (-1): the roughness the guide moves toward; -1 leaves roughness unchanged.
- `TransparencyBlendStrength` (1): overall blend amount, from 0 (no change) to 1.
- `TransparencyBlendSky` (1): 1 also blends where transparency layers cover the sky; 0 leaves sky pixels alone.
- `TransparencyBlendFilter` (1): 1 smooths the blend mask over neighboring pixels; 0 uses it unfiltered.
- `TransparencyBlendSss` (1): 1 also fades the Subsurface Scattering guide by the same coverage, so skin seen through glass is not described as skin to RR while its colour and albedo describe the glass; 0 leaves the guide unchanged. Also shown in the panel as "Fade SSS guide under glass" under the blend checkbox.

The panel also has these checkboxes, all on by default and saved in `ReShade.ini`:

- Specular hit distance (`SpecularHitDistance`): gives RR the game's specular ray hit distances.
- Weapon depth (`WeaponDepth`): gives RR a depth in which the weapon and hands keep their own depth, still in front of the scene, instead of a flat card at the near plane. World depth stays the game's own, in the game's reverse-Z format, so third-party add-ons such as DLSS5 NR keep working. Changing it restarts RR, like a preset change.
- Replace the game's DLSS upscaling (`ReplaceNativeSR`): RR takes the place of the game's DLSS Super Resolution pass when it can. See Architecture below.
- Water responsivity mask (`WaterResponsivity`): tells RR to favor the current frame on running water, which it otherwise blurs. It applies on preset F only.

With the Subsurface Scattering Guide on, lighting on skin and other subsurface materials uses the game's native temporal history where it exists, and a spatial estimate elsewhere. The panel reports this as "SSS lighting".

## Architecture

This is a game-specific rendering integration, not a screen-space ReShade effect. It depends on recognized shader variants, resource bindings, and the game's existing NVIDIA NGX initialization.

The addon observes relevant Direct3D 12 and NGX calls, identifies the campaign's lighting passes, prepares RR inputs, and records a private RR evaluation. Raw lighting is weighted using the game's native variable ray-sampling probabilities. Native temporal lighting history is retained for the game's own consumers.

The Transparency Guide uses an image taken before transparency composition. The Subsurface Scattering Guide is generated through an instrumented variant of a recognized game shader. That variant is prepared in memory from the game-supplied shader when its pipeline loads, validated against the expected result, and reused for the session. Game shader bytecode is not embedded in the addon. Guide use depends on matching resources, frame/view identity, and GPU submission order; an enabled guide can still be unavailable for a particular frame.

Eligible evaluations replace native DLSS Super Resolution ("Replace the game's DLSS upscaling", `ReplaceNativeSR`, on by default). An input the SR command list never transitioned is taken in the non-pixel-shader read state DLSS requires of the game; one seen in a pixel-shader read state is transitioned for RR and back. When replacement is unavailable, the native call is retained and RR may run afterward if its own requirements are met. A replacement rejected during GPU execution uses a current-frame spatial upscale, which can appear softer or noisier. Native Super Resolution history is reset when native evaluations resume after being skipped. The ReShade panel reports the completed reconstruction path.

### Third-party DLSS add-ons

Add-ons that hook NGX evaluation beneath RR, such as the DLSS5 NR add-on, keep working with replacement on: RR's output and depth are created through ReShade's device and announced to other add-ons, so such an add-on follows RR's DLSS call instead of the game's skipped one. With DLSS5 NR running on RR's output directly, pre-rendered cutscenes are no longer filtered through it. RR releases its lock while inside NGX, so an add-on that calls back into the game or frame generation from there cannot deadlock it. Under heavy GPU memory use (DLSS5 NR can run the process far over its VRAM budget), RR keeps its own allocations within a fixed ledger instead of refusing them, and a frame that truly cannot get memory stays native and retries.

Resource ownership follows command-recording and fence lifetimes. Preset or resource-size changes require outstanding work to retire before replacing the RR configuration. Internal rendering calls are distinguished from observed game calls to avoid recursive observation and lock re-entry.

### Source layout

| Area | Responsibility |
| --- | --- |
| `src/mw4_rr.cpp` | ReShade lifecycle and rendering-event integration |
| `src/rr_trace.h` | NGX interception and evaluation routing |
| `src/rr_live.h`, `src/rr_live_gpu.h`, `src/rr_live_*.inl` | RR scheduling, resources, guide admission, and presentation |
| `src/rr_bindings.h`, `src/rr_compute_state.h`, `src/rr_buffer_inspector.h` | Resource bindings, state observation, and command interception |
| `src/rr_sparse_*`, `src/rr_vrrt_*` | Lighting preservation and variable-rate weighting |
| `src/rr_sss_*`, `src/rr_glass_*` | Subsurface Scattering and transparency integration |
| `src/rr_settings.inl`, `src/rr_status.h` | ReShade controls and effective status |
| `shaders/` | Project-owned HLSL |
| `tools/` | Shader preparation and build helpers |
| `tests/` | Device-free regression fixtures |

## Runtime requirements

- Windows x64 and an NVIDIA RTX GPU with working DLSS Ray Reconstruction support.
- The campaign executable `sp26-cod.exe`, using Direct3D 12 and ray tracing.
- ReShade 6.8.0 with full add-on support. The source targets ReShade API 20 and ImGui 1.92.5; other runtime versions are unverified.
- A working NVIDIA Ray Reconstruction runtime (`nvngx_dlssd.dll`) loaded by the game. Keep the existing DLL when installing this addon; a separate replacement is not an installation requirement. Testing used version 310.7.128.0. The addon does not enforce that exact version, and other versions have not been verified.
- Microsoft Visual C++ x64 runtime.

The addon and its shader-reflection companion must be placed together beside the campaign executable:

```text
mw4-rr.addon64
mw4-rr-dxcompiler.dll
```

ReShade, NVIDIA model/runtime DLLs, and game configuration files are not distributed with the addon. Multiplayer, DMZ, and COD HQ are outside this project's scope. See [INSTALLATION.txt](INSTALLATION.txt) for the complete player setup, controls, and uninstall procedure.

## Build

Required tools:

- Visual Studio C++ tools with C++20 support.
- Windows SDK containing FXC.
- Python 3.
- NVIDIA DLSS SDK headers exposing NGX API 1.5.0, supplied separately.
- DirectX Shader Compiler 1.8.2505.1, including its x64 tools and DLLs.

Dependency snapshots are recorded in `external/DEPENDENCIES.json`. Public dependency headers/source and their notices are included under `external/`; the NGX SDK and DXC tools are separate requirements. Building the addon does not require a game installation or captured game shaders.

From the repository root:

```bat
build.cmd "path\to\NGX\include" "path\to\dxc\bin\x64"
```

Outputs are written under `build/`. The companion DLL is the unmodified x64 DXC `dxcompiler.dll`, renamed `mw4-rr-dxcompiler.dll`. Generated shader headers contain only the project-owned HLSL compiled by the build. Runtime-generated game shader variants remain in memory and are not written to disk.

## Tests and validation

After building:

```bat
test.cmd "path\to\NGX\include"
```

The suite uses mock NGX functions and fake COM objects. It covers evaluation routing, parameter restoration, resource identity/state policies, preset lifecycle, submission retirement, internal-call guards, and spatial-fallback coordinates. It does not create a GPU device or launch the game.

Changes to shaders, descriptor layouts, transitions, synchronization, or temporal history additionally require gameplay validation. Relevant cases include camera and object motion, mission changes, supported quality/output-size combinations, guide availability, and live preset changes. Passing CPU tests is not evidence of visual correctness or driver compatibility.

## Configuration and limitations

- Enhanced FOV, motion blur, and depth of field must be disabled. The installation guide includes all required configuration entries.
- The campaign configuration must be read-only. VRS can be enabled or disabled.
- Residual noise and blotches remain on some Subsurface Scattering materials. Transparency coverage and image quality vary by scene.
- Compatibility relies on recognized game shaders and layouts; game updates can require integration changes.
- Unsupported inputs or resource allocation failures can prevent RR or a guide from being used for a frame. Under heavy GPU memory use, optional guides can wait until memory allows them.
- Third-party DLSS add-ons are supported only as far as they have been tested; DLSS5 NR is the one tested. Other add-ons that hook DLSS may behave differently.
- If the game shuts down and reinitializes NGX, the addon retires its integration until the game is restarted.
- Performance overhead and the proportion of frames that replace native Super Resolution have not yet been measured in gameplay.

## Bug reports and contributions

Include the game and driver versions, GPU, output resolution, DLSS quality, affected mission, relevant settings, addon status, and reproduction steps. Attach the campaign `ReShade.log` from the affected run, saving it before relaunching.

For code changes, describe the affected rendering stage and assumptions about resource format, extent, subresources, lifetime, and execution order. Include a focused regression case where practical and state separately what was validated on the CPU and in the game. Keep private game assets and generated native-derived shader files out of source submissions.

## AI use disclosure

This project is developed extensively with AI assistance, including code authoring, shader research, debugging, and documentation. Fever308 directs development and performs hands-on gameplay testing. Contributions help fund the AI token costs involved in that work.

## Support

Support continued development and testing through [Buy Me a Coffee](https://buymeacoffee.com/fever308).

## License and acknowledgments

Project code is licensed under the **GNU General Public License, version 3 only (GPL-3.0-only)**. Copyright (c) 2026 Fever308. You may use, modify, and redistribute covered code, including commercially, under those terms. Distribution of covered binaries must provide the Corresponding Source as required by GPLv3. See [LICENSE](LICENSE) for the unmodified license text. Third-party components and inherited portions retain their notices; see [THIRD_PARTY.md](THIRD_PARTY.md). The project license does not grant rights over game assets or NVIDIA model/runtime DLLs.

Uses ReShade, Dear ImGui, MinHook, NVIDIA SDK interfaces, and DirectX Shader Compiler. Inherited helper portions retain attribution to Jean-Laurent ROUZIES / DLSS5-Feeder and NIGos / dlss5-dx11-bridge.

Unofficial project; unaffiliated with NVIDIA, Microsoft, ReShade, or the game developer.
