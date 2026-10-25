# MW4 Ray Reconstruction

A Windows x64 ReShade addon integrating NVIDIA DLSS Ray Reconstruction into the MW4 campaign's Direct3D 12 rendering path. Includes Transparency and Subsurface Scattering guides, live RR preset selection, and a ReShade status panel.

**Beta 1** is the initial public beta. See the requirements and limitations below before installing or building.

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

DLSS quality is configured through `DLSSPerfModeMP` in the game's configuration and requires a restart. Preset changes are handled separately and can apply during gameplay.

Transparency albedo blend (on by default, with a checkbox in the panel and no hotkey) moves RR's albedo guides toward neutral values where the game's transparency layers (OIT glass and particles, and screen-space refraction) cover a pixel, so texture seen through glass is kept while moving. It is controlled by the `TransparencyBlend` key in `ReShade.ini`. Running water is not affected. Optional tuning keys, not shown in the panel:

- `TransparencyBlendDiffuse` (0.5): the diffuse albedo value the guide moves toward.
- `TransparencyBlendSpecular` (0): the specular albedo value the guide moves toward.
- `TransparencyBlendRoughness` (-1): the roughness the guide moves toward; -1 leaves roughness unchanged.
- `TransparencyBlendStrength` (1): overall blend amount, from 0 (no change) to 1.
- `TransparencyBlendSky` (1): 1 also blends where transparency layers cover the sky; 0 leaves sky pixels alone.
- `TransparencyBlendFilter` (1): 1 smooths the blend mask over neighboring pixels; 0 uses it unfiltered.
- `TransparencyBlendDebug` (0): 1 shows the blend mask in the albedo guide for inspection.

The indented "Frosted glass pattern" checkbox under the blend (off by default, saved as the `TransparencyBlendPattern` key) gives covered pixels a diffuse albedo that follows the visible glass pattern, the `TransparencyBlendDiffuse` value scaled by the pixel's color over its local average brightness, so RR keeps the pattern instead of smoothing it as noise.

## Architecture

This is a game-specific rendering integration, not a screen-space ReShade effect. It depends on recognized shader variants, resource bindings, and the game's existing NVIDIA NGX initialization.

The addon observes relevant Direct3D 12 and NGX calls, identifies the campaign's lighting passes, prepares RR inputs, and records a private RR evaluation. Raw lighting is weighted using the game's native variable ray-sampling probabilities. Native temporal lighting history is retained for the game's own consumers.

The Transparency Guide uses an image taken before transparency composition. The Subsurface Scattering Guide is generated through an instrumented variant of a recognized game shader. That variant is prepared in memory from the game-supplied shader when its pipeline loads, validated against the expected result, and reused for the session. Game shader bytecode is not embedded in the addon. Guide use depends on matching resources, frame/view identity, and GPU submission order; an enabled guide can still be unavailable for a particular frame.

Eligible evaluations replace native DLSS Super Resolution. When replacement is unavailable, the native call is retained and RR may run afterward if its own requirements are met. A replacement rejected during GPU execution uses a current-frame spatial upscale, which can appear softer or noisier. Native Super Resolution history is reset when native evaluations resume after being skipped. The ReShade panel reports the completed reconstruction path.

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
- Unsupported inputs or resource allocation failures can prevent RR or a guide from being used.
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
