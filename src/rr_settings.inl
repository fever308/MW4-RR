#pragma once
#include "rr_settings_model.h"
#include "rr_sss_mask.h"
#include "rr_sss_coverage.h"
#include "rr_edge_fill.h"
#include "rr_water.h"
#include "rr_weapon_depth.h"
#include "rr_fg_depth.h"
// CPU settings and UI only. Rendering threads service preset changes.
static std::mutex settings_mutex;
static bool settings_loaded{}, config_allowed{};
static unsigned config_warnings{}; // settings that differ from INSTALLATION.txt
static rrsettings::Preferences preferences;
static bool &requested_rr = preferences.rr;
static bool &requested_sss = preferences.sss;
static bool &requested_blend = preferences.blend;
static bool &requested_glass = preferences.glass;
static bool &requested_tolerate = preferences.tolerate_isolated;
static bool &requested_hit = preferences.hit;
static bool &requested_weapon_depth = preferences.weapon_depth;
static bool &requested_fg_weapon_depth = preferences.fg_weapon_depth;
static bool &requested_water = preferences.water;
static bool &requested_replace_sr = preferences.replace_sr;
static bool &requested_sss_coverage = preferences.sss_coverage;
static bool &requested_sss_coverage_compare = preferences.sss_coverage_compare;
static bool &requested_sss_edge_fill = preferences.sss_edge_fill;
static constexpr const char *SettingsSection = "MW4RayReconstruction";
static void SaveToggles() {
    reshade::set_config_value(nullptr, SettingsSection, "Enabled", requested_rr);
    reshade::set_config_value(nullptr, SettingsSection, "SSS", requested_sss);
    reshade::set_config_value(nullptr, SettingsSection, "TransparencyBlend", requested_blend);
    reshade::set_config_value(nullptr, SettingsSection, "Transparency", requested_glass);
    reshade::set_config_value(nullptr, SettingsSection, "TolerateIsolatedGuidePixels",
                              requested_tolerate);
    reshade::set_config_value(nullptr, SettingsSection, "SpecularHitDistance", requested_hit);
    reshade::set_config_value(nullptr, SettingsSection, "WeaponDepth", requested_weapon_depth);
    reshade::set_config_value(nullptr, SettingsSection, "FgWeaponDepth",
                              requested_fg_weapon_depth);
    reshade::set_config_value(nullptr, SettingsSection, "WaterResponsivity", requested_water);
    reshade::set_config_value(nullptr, SettingsSection, "ReplaceNativeSR", requested_replace_sr);
    reshade::set_config_value(nullptr, SettingsSection, "SubsurfaceScatteringCoverageSelection",
                              requested_sss_coverage);
    reshade::set_config_value(nullptr, SettingsSection, "SubsurfaceScatteringCoverageCompare",
                              requested_sss_coverage_compare);
    reshade::set_config_value(nullptr, SettingsSection, "SubsurfaceScatteringEdgeAwareFill",
                              requested_sss_edge_fill);
}
static void ApplyEnabled(bool allowed) {
    config_allowed = allowed;
    // Before RR can be enabled: RR takes its depth source when it is created. A change restarts
    // a running RR and retries weapon depth held off since the last change.
    if (rrweapondepth::enabled.exchange(requested_weapon_depth) != requested_weapon_depth)
        ++rrweapondepth::changes;
    // Read by RR once per frame (Controller::PrepareFgDepth): applies from the next frame.
    if (rrfgdepth::enabled.exchange(requested_fg_weapon_depth) != requested_fg_weapon_depth)
        ++rrfgdepth::changes;
    LiveRR().SetEnabled(requested_rr && config_allowed);
    LiveRR().SetSssEnabled(requested_sss);
    // A shader variant and predicate choice only: no history reset or RR restart.
    rrssscov::selection = unsigned(requested_sss_coverage ? rrssscov::Selection::Coverage
                                                          : rrssscov::Selection::Baseline);
    rrssscov::compare = requested_sss_coverage_compare;
    rredgefill::enabled = requested_sss_edge_fill;
    LiveRR().SetTransparencyEnabled(requested_glass);
    LiveRR().SetTransparencyBlendEnabled(requested_blend);
    LiveRR().SetSpecularHitEnabled(requested_hit);
    LiveRR().SetReplaceSR(requested_replace_sr);
    AlbedoInspector().observe_transitions = requested_glass;
    LiveRR().SetTolerateIsolated(requested_tolerate);
    rrwater::Settings water;
    water.responsivity = requested_water;
    LiveRR().SetWater(water);
}
static void ApplyEnabled() {
    ApplyEnabled(VrsConfigAllowsRR());
}
static void LoadSettings() {
    if (settings_loaded)
        return;
    settings_loaded = true;
    LoadBlendTuning(SettingsSection);
    unsigned preset = preferences.preset;
    reshade::get_config_value(nullptr, SettingsSection, "Enabled", requested_rr);
    reshade::get_config_value(nullptr, SettingsSection, "SSS", requested_sss);
    reshade::get_config_value(nullptr, SettingsSection, "TransparencyBlend", requested_blend);
    reshade::get_config_value(nullptr, SettingsSection, "Transparency", requested_glass);
    // Always on now: its panel checkbox is gone; the saved key reads on from the next save.
    requested_tolerate = true;
    reshade::get_config_value(nullptr, SettingsSection, "SpecularHitDistance", requested_hit);
    reshade::get_config_value(nullptr, SettingsSection, "WeaponDepth", requested_weapon_depth);
    reshade::get_config_value(nullptr, SettingsSection, "FgWeaponDepth",
                              requested_fg_weapon_depth);
    bool fg_observe = false;
    reshade::get_config_value(nullptr, SettingsSection, "FgWeaponDepthObserve", fg_observe);
    rrfgdepth::observe.store(fg_observe, std::memory_order_relaxed);
    reshade::get_config_value(nullptr, SettingsSection, "WaterResponsivity", requested_water);
    reshade::get_config_value(nullptr, SettingsSection, "ReplaceNativeSR", requested_replace_sr);
    reshade::get_config_value(nullptr, SettingsSection, "SubsurfaceScatteringCoverageSelection",
                              requested_sss_coverage);
    reshade::get_config_value(nullptr, SettingsSection, "SubsurfaceScatteringCoverageCompare",
                              requested_sss_coverage_compare);
    reshade::get_config_value(nullptr, SettingsSection, "SubsurfaceScatteringEdgeAwareFill",
                              requested_sss_edge_fill);
    reshade::get_config_value(nullptr, SettingsSection, "Preset", preset);
    if (!LiveRR().RequestPreset(preset))
        LiveRR().RequestPreset(6);
    // One read for the RR gate and the config report, so the report names the gate's answer.
    const auto config = ReadGameConfig();
    ApplyEnabled(VrsGateAllows(config));
    config_warnings = ReportGameConfig(config, config_allowed);
}
static const char *PresetLabel(unsigned value) {
    for (unsigned i = 0; i < 4; ++i)
        if (rrpreset::values[i] == value)
            return rrpreset::names[i];
    return "Unknown";
}
// The game window has the focus. Lock-free: OnPresent tests it before any lock.
static bool Foreground() {
    DWORD foreground{};
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
    return foreground == GetCurrentProcessId();
}
// times, if given, receives the observer poll, settings lock wait and settings load durations.
static void OnRuntime(reshade::api::effect_runtime *runtime, uint64_t *times = nullptr) {
    const auto begin = rrtiming::Now();
    Observer().Poll();
    const auto polled = rrtiming::Now();
    {
        std::lock_guard lock(settings_mutex);
        const auto locked = rrtiming::Now();
        LoadSettings();
        if (times) {
            times[0] = polled - begin;
            times[1] = locked - polled;
            times[2] = rrtiming::Now() - locked;
        }
        const bool foreground = Foreground();
        static bool down[3]{};
        const int keys[] = {VK_F8, VK_F9, VK_F11};
        const bool ctrl_or_alt = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
                                 (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        bool changed = false;
        for (unsigned i = 0; i < 3; ++i) {
            const bool pressed = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
            if (rrsettings::HotkeyToggles(pressed, down[i], ctrl_or_alt, foreground)) {
                if (i == 0)
                    requested_rr = !requested_rr;
                else if (i == 1)
                    requested_glass = !requested_glass;
                else
                    requested_sss = !requested_sss;
                changed = true;
            }
            down[i] = pressed;
        }
        if (changed) {
            ApplyEnabled();
            SaveToggles();
        }
    }
    SparsePool().Poll();
    LiveRR().Poll();
    // Idle windows are reported a few times, so a missing blur dispatch is visible, and a window
    // whose set of rejection stages differs from the last one's, so new rejections show.
    static ULONGLONG distance_report{};
    static unsigned idle_reports{}, last_rejects{};
    const auto now = GetTickCount64();
    if (now - distance_report >= 10000) {
        distance_report = now;
        char line[320];
        unsigned rejects{};
        const bool active = DistanceStats().Summary(line, sizeof(line), &rejects);
        if (LiveRR().Enabled() && LiveRR().SpecularHitEnabled()) {
            if (!active ? idle_reports++ < 3 : rejects != last_rejects)
                WriteLog(line);
            if (active)
                last_rejects = rejects;
        }
    }
}
// Every present, before OnRuntime takes any lock: lock-free counts for the RR health line.
static void OnPresent(reshade::api::effect_runtime *runtime) {
    const bool focused = Foreground();
    auto &shown = rrhealth::presentation;
    shown.presents.fetch_add(1, std::memory_order_relaxed);
    if (!focused)
        shown.unfocused_presents.fetch_add(1, std::memory_order_relaxed);
    shown.focused.store(focused, std::memory_order_relaxed);
    static std::atomic<bool> first{};
    rrtiming::Global().Milestone(first, "first present");
    OnRuntime(runtime);
    rrtiming::Global().Summary();
}
// Entry and exit records for each runtime (re)creation, so that a long gap
// in the log shows whether it was spent in this handler or before it.
static void OnInitRuntime(reshade::api::effect_runtime *runtime) {
    static std::atomic<unsigned> inits{};
    const unsigned n = inits.fetch_add(1, std::memory_order_relaxed) + 1;
    auto &t = rrtiming::Global();
    char line[240];
    if (n <= 8) {
        std::snprintf(line, sizeof(line), "RR startup: effect runtime init #%u entered at %.1f s",
                      n, double(rrtiming::Now() - t.start) / 1e9);
        t.Log(line);
    }
    uint64_t times[3]{};
    const auto begin = rrtiming::Now();
    OnRuntime(runtime, times);
    if (n <= 8) {
        std::snprintf(line, sizeof(line),
                      "RR startup: effect runtime init #%u handler took %.1f ms (observer %.1f, "
                      "settings wait %.1f, settings %.1f)",
                      n, rrtiming::Ms(rrtiming::Now() - begin), rrtiming::Ms(times[0]),
                      rrtiming::Ms(times[1]), rrtiming::Ms(times[2]));
        t.Log(line);
    }
    t.Summary(true);
}
static void OnSwapchain(reshade::api::swapchain *, bool resize) {
    static std::atomic<unsigned> inits{};
    const unsigned n = inits.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 8)
        return;
    auto &t = rrtiming::Global();
    char line[160];
    std::snprintf(line, sizeof(line), "RR startup: swapchain init #%u (%s) at %.1f s", n,
                  resize ? "resize" : "create", double(rrtiming::Now() - t.start) / 1e9);
    t.Log(line);
}
static void StatusRow(const char *name, rrstatus::State value) {
    ImGui::Text("%s: %s", name, rrstatus::Label(value));
}
// Subsurface Scattering coverage experiment (rr_sss_coverage.h).
static void DrawSssCoverage(bool &changed) {
    ImGui::Indent();
    changed |= ImGui::Checkbox("Subsurface Scattering coverage selection (experiment)",
                               &requested_sss_coverage);
    changed |= ImGui::Checkbox("Compare Subsurface Scattering selections (counters)",
                               &requested_sss_coverage_compare);
    if (LiveRR().Running() && (requested_sss_coverage || requested_sss_coverage_compare)) {
        const auto r = LiveRR().SssCoverageReport();
        if (r.failed)
            ImGui::TextUnformatted("Coverage history: unavailable (see ReShade.log)");
        else if (!r.allocated)
            ImGui::Text("Coverage history: %s",
                        r.waiting ? "waiting for GPU memory" : "waiting for RR");
        else
            ImGui::Text("Coverage record: %s", rrssscov::Label(r.reason));
        if (r.key_skips)
            ImGui::Text("Frames without a surface key (GPU %u or more frames behind): %llu",
                        rrssscov::Ring, (unsigned long long)r.key_skips);
        const auto &t = r.totals;
        const double pixels = double(t.value[rrssscov::Pixels]);
        if (t.frames && pixels > 0) {
            auto pct = [&](unsigned i) { return 100.0 * double(t.value[i]) / pixels; };
            ImGui::Text("Last second: %llu frames, record %.1f frames old on average",
                        (unsigned long long)t.frames, double(t.age_sum) / double(t.frames));
            ImGui::Text("Native history by the previous guide at the same pixel: %.2f%%",
                        pct(rrssscov::BaselineNative));
            ImGui::Text("Native history by reprojected Subsurface Scattering coverage: %.2f%%",
                        pct(rrssscov::CoverageNative));
            ImGui::Text("Pixels where the two differ: %.2f%%  (applied: %.2f%%)",
                        pct(rrssscov::Differ), pct(rrssscov::Applied));
            ImGui::Text("Coverage not usable: off-screen %.2f%%, disoccluded %.2f%%, "
                        "normal %.2f%%, unknown %.2f%%",
                        pct(rrssscov::CountOffScreen), pct(rrssscov::CountDisoccluded),
                        pct(rrssscov::CountNormal), pct(rrssscov::CountUnknown));
            ImGui::Text("Sky %.2f%%, first-person weapon %.2f%%", pct(rrssscov::CountSky),
                        pct(rrssscov::CountWeapon));
        }
    }
    ImGui::TextWrapped(
        "Off is the baseline: Subsurface Scattering pixels are found from an earlier frame's "
        "Subsurface Scattering guide at the same screen position. On, they are found from "
        "where the game's Subsurface Scattering pass drew in that frame, followed to this "
        "frame with the camera and each pixel's depth; pixels whose surface does not match "
        "get raw lighting. The guide RR receives is unchanged. Compare counts both choices "
        "without changing the picture. Switching either does not reset history.");
    ImGui::Unindent();
}
static void DrawSettings(reshade::api::effect_runtime *) {
    std::lock_guard lock(settings_mutex);
    LoadSettings();
    ImGui::TextUnformatted("MW4 Ray Reconstruction — Beta 2");
    bool changed = ImGui::Checkbox("Ray reconstruction (F8)", &requested_rr);
    changed |= ImGui::Checkbox("Transparency guide (F9)", &requested_glass);
    changed |= ImGui::Checkbox("Subsurface Scattering guide (F11)", &requested_sss);
    if (requested_sss && LiveRR().Running())
        ImGui::Text("SSS lighting: %s", rrsssmask::Label(SssMaskState()));
    if (requested_sss) {
        ImGui::Indent();
        changed |= ImGui::Checkbox("Subsurface Scattering edge-aware fill (experiment)",
                                   &requested_sss_edge_fill);
        if (LiveRR().Running())
            ImGui::Text("Edge-aware fill: %s",
                        requested_sss_edge_fill ? rredgefill::Label(EdgeFillState())
                                                : "off (baseline group mean)");
        ImGui::TextWrapped("Off is the baseline. On, a Subsurface Scattering pixel with no ray "
                           "this frame and no history is filled only from nearby rays on the "
                           "same surface (matching depth, normal and material class) instead "
                           "of every ray in its 16x16 block. For A/B comparison; switching it "
                           "does not reset history.");
        ImGui::Unindent();
        DrawSssCoverage(changed);
    }
    changed |= ImGui::Checkbox("Transparency albedo blend", &requested_blend);
    changed |= ImGui::Checkbox("Specular hit distance", &requested_hit);
    changed |= ImGui::Checkbox("Weapon depth", &requested_weapon_depth);
    {
        ImGui::Indent();
        using WeaponState = rrlive::Controller::WeaponState;
        const auto weapon = LiveRR().WeaponDepthState();
        ImGui::Text("Weapon depth: %s",
                    weapon == WeaponState::Active       ? "active"
                    : weapon == WeaponState::Off        ? "off"
                    : weapon == WeaponState::Restarting ? "restarting RR"
                    : weapon == WeaponState::NoMemory
                        ? "off: not enough GPU memory (turn it off and on to retry)"
                    : weapon == WeaponState::Unsupported
                        ? "off: the game's projection is not supported (turn it off and on to "
                          "retry)"
                    : weapon == WeaponState::Refused
                        ? "off: RR failed with it (turn it off and on to retry)"
                    : !LiveRR().Running() ? "waiting for RR"
                                          : "waiting");
        ImGui::TextWrapped("The game draws the weapon and hands with their own camera, so with "
                           "the game's depth RR sees them as one flat card. On, RR gets a copy of "
                           "the game's depth in which they keep their own depth steps, still in "
                           "front of everything else; the rest of the scene keeps the game's "
                           "depth unchanged, so add-ons such as DLSS5 NR keep working. Changing "
                           "it restarts RR, like a preset change.");
        ImGui::Unindent();
    }
    changed |= ImGui::Checkbox("Weapon depth for Frame Generation (experimental)",
                               &requested_fg_weapon_depth);
    {
        ImGui::Indent();
        const auto fg = LiveRR().FgState();
        ImGui::Text("Frame Generation depth: %s", rrfgdepth::Label(fg));
        ImGui::TextWrapped("An experiment: Frame Generation is given a copy of the game's depth "
                           "with the weapon and hands remapped as Weapon depth remaps them for "
                           "RR (world depth unchanged). It needs Ray Reconstruction running, "
                           "which tracks each frame and writes the copy, but not the Weapon "
                           "depth checkbox above. \"Supplied\" means Streamline accepted it, "
                           "not that it looks better. It applies from the next frame, without a "
                           "restart; Frame Generation's history is not reset.");
        ImGui::Unindent();
    }
    changed |= ImGui::Checkbox("Water responsivity mask", &requested_water);
    {
        ImGui::Indent();
        const auto water = LiveRR().WaterStatus();
        ImGui::Text("Water responsivity mask: %s",
                    !requested_water         ? "off"
                    : water.unavailable      ? "unavailable this session (see ReShade.log)"
                    : !water.wanted          ? "waiting (needs RR preset F)"
                    : water.waiting_memory   ? "waiting for GPU memory (retrying)"
                    : water.responsivity     ? "active"
                    : !LiveRR().Running()    ? "waiting for RR"
                                             : "waiting");
        ImGui::TextWrapped("Tells RR to lean on the current frame on running water, which it "
                           "otherwise blurs. Supported on RR preset F only.");
        ImGui::Unindent();
    }
    changed |= ImGui::Checkbox("Replace the game's DLSS upscaling", &requested_replace_sr);
    {
        ImGui::Indent();
        ImGui::TextWrapped("On, RR takes the place of the game's DLSS Super Resolution pass when "
                           "it can, which saves that pass's GPU time. A frame RR rejects then "
                           "shows a softer upscale for a few frames. Off, the game's pass always "
                           "runs and RR draws over it. The path in use is shown below. Add-ons such "
                           "as DLSS5 NR follow RR's call when this is on.");
        if (LiveRR().ReplaceSRPaused())
            ImGui::TextWrapped("Paused: RR refused several replacement frames in a row (see "
                               "ReShade.log). Turn the setting off and on to retry.");
        ImGui::Unindent();
    }
    ImGui::Separator();
    auto preset = LiveRR().PresetStatus();
    int selected = 3;
    for (int i = 0; i < 4; ++i)
        if (rrpreset::values[i] == preset.desired)
            selected = i;
    if (ImGui::Combo("RR preset", &selected, rrpreset::names, 4) &&
        LiveRR().RequestPreset(rrpreset::values[selected]))
        reshade::set_config_value(nullptr, SettingsSection, "Preset", rrpreset::values[selected]);
    preset = LiveRR().PresetStatus();
    if (preset.pending)
        ImGui::TextUnformatted("Applying change after pending frames finish...");
    if (preset.has_effective)
        ImGui::Text("Applied preset request: %s", PresetLabel(preset.effective));
    ImGui::Separator();
    const auto status = LiveRR().Status();
    StatusRow("RR", status.rr);
    StatusRow("Transparency", status.glass);
    StatusRow("Subsurface Scattering", status.sss);
    StatusRow("Transparency albedo blend",
              LiveRR().TransparencyBlendState(rrsig::Attached() && CoveragePipelines().SignatureHooked(),
                                              CoveragePipelines().OitUnavailable()));
    StatusRow("Specular hit distance", status.hit);
    ImGui::Text("Reconstruction path: %s", rrstatus::RouteLabel(status.route));
    ImGui::TextWrapped("Waiting means enabled but not delivered in recent completed frames. A "
                       "guide may be unavailable in some scenes.");
    if (!config_allowed)
        ImGui::TextWrapped("RR configuration unavailable. Check that the campaign settings file is "
                           "read-only and contains a supported VRS value. See README.");
    // "RR still runs" only while the gate allows RR, never under "RR configuration unavailable".
    if (config_warnings)
        ImGui::TextWrapped("Game config: %u %s from INSTALLATION.txt%s. See ReShade.log.",
                           config_warnings,
                           config_warnings == 1 ? "setting differs" : "settings differ",
                           config_allowed ? " (RR still runs)" : "");
    if (status.rr == rrstatus::State::Unavailable)
        ImGui::TextWrapped("The game shut down its NGX runtime. Restart the game to restore RR.");
    const auto options = LiveRR().Options();
    ImGui::Separator();
    ImGui::Text("Observed DLSS mode: %s", rrsettings::QualityLabel(options.quality));
    if (rrresolution::Texture(options.width, options.height) &&
        rrresolution::Texture(options.out_width, options.out_height))
        ImGui::Text("Render: %u x %u    Output: %u x %u", options.width, options.height,
                    options.out_width, options.out_height);
    ImGui::TextWrapped("DLSS quality is configured using DLSSPerfModeMP in the game settings file. "
                       "Restart the game after changing it. RR preset changes above apply live.");
    ImGui::TextWrapped("Keep enhanced FOV, depth of field, and motion blur disabled. See README "
                       "for the exact configuration entries.");
    if (changed) {
        ApplyEnabled();
        SaveToggles();
    }
}
