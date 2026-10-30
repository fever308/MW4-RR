#include <d3d12.h>
#include "../src/rr_scene_shape.h"
#include "../src/rr_settings_model.h"
#include "../src/rr_vrs_config.h"
#include "../src/rr_game_config.h"
#include "../src/rr_live_policy.h"
#include "../src/rr_guide_health.h"
#include "../src/rr_lighting_source.h"
#include "../src/rr_sss_mask.h"
#include "../src/rr_specular_distance.h"
#include "../src/rr_trace_model.h"
#include "../src/rr_water.h"
#include "../src/rr_gate_stats.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

constexpr rrsettings::Preferences defaults;
static_assert(defaults.blend);
static_assert(defaults.rr && defaults.sss && defaults.glass && defaults.hit &&
              defaults.weapon_depth && defaults.preset == 6);
static_assert(defaults.tolerate_isolated && defaults.water);
// The Subsurface Scattering coverage selection and its counters are experiments: off.
static_assert(!defaults.sss_coverage && !defaults.sss_coverage_compare);
// The edge-aware Subsurface Scattering fill is an experiment: off by default.
static_assert(!defaults.sss_edge_fill);
// The water responsivity mask starts on, and only preset F wants it.
static_assert(rrwater::Settings{}.responsivity && rrwater::Settings{}.strength == 1.0f);
static_assert(rrwater::Wanted(rrwater::Settings{}, true) &&
              !rrwater::Wanted(rrwater::Settings{}, false));
static_assert(rrwater::Flags(true) == 1 && rrwater::Flags(false) == 0);
// Only the SSS source reads the SSS guide.
static_assert(rrlighting::Raw == 0 && rrlighting::SssHistory2 == 1 && rrlighting::Count == 2);
static_assert(!rrlighting::SssMasked(rrlighting::Raw) &&
              rrlighting::SssMasked(rrlighting::SssHistory2) &&
              !rrlighting::SssMasked(rrlighting::Count));
// A guide serves the next few frames of its generation and extent, across wrap.
constexpr rrsssmask::Record guide{true, 100, 7, 1280, 720};
static_assert(rrsssmask::Usable(guide, 101, 7, 1280, 720) &&
              rrsssmask::Usable(guide, 100 + rrsssmask::MaxAge, 7, 1280, 720));
static_assert(!rrsssmask::Usable(guide, 100, 7, 1280, 720) &&
              !rrsssmask::Usable(guide, 101 + rrsssmask::MaxAge, 7, 1280, 720) &&
              !rrsssmask::Usable(guide, 99, 7, 1280, 720));
static_assert(!rrsssmask::Usable(guide, 101, 8, 1280, 720) &&
              !rrsssmask::Usable(guide, 101, 7, 1920, 720) &&
              !rrsssmask::Usable(guide, 101, 7, 1280, 1080));
static_assert(!rrsssmask::Usable({}, 1, 0, 0, 0));
static_assert(rrsssmask::Usable({true, 0xffffffffu, 1, 8, 8}, 1, 1, 8, 8));
static_assert(std::string_view(rrsettings::QualityLabel(0)) == "Performance");
static_assert(std::string_view(rrsettings::QualityLabel(5)) == "Native / DLAA");
static_assert(std::string_view(rrsettings::QualityLabel(-1)) == "Not observed yet");
static_assert(rrsettings::HotkeyToggles(true, false, false, true));
static_assert(!rrsettings::HotkeyToggles(true, true, false, true));
static_assert(!rrsettings::HotkeyToggles(true, false, true, true));
static_assert(!rrsettings::HotkeyToggles(true, false, false, false));
static_assert(!rrsettings::HotkeyToggles(false, false, false, true));
// The slot's stats buffer: the gate's health words and non-finite counts fit the 512 bytes.
static_assert(RR_STATS_NON_FINITE_COLOR == 256 && RR_STATS_NON_FINITE_OUTPUT == 260 &&
              RR_STATS_NON_FINITE_OUTPUT + 4 <= rrgatestats::StatsBytes &&
              rrgatestats::StatsBytes == 512);

// Settings file lines as the game writes them, CRLF included.
static std::string ConfigLine(const std::string &key, const std::string &value) {
    return key + ";57752;20945 = " + value + " // one of a, b\r\n";
}
static const std::pair<const char *, const char *> compliant[] = {
    {"MotionBlur@0", "Off"},
    {"WeaponMotionBlur@0", "false"},
    {"EnableVelocityBasedBlur@0", "false"},
    {"DofEnable@0", "Off"},
    {"DofWeaponDisable@0", "true"},
    {"DofWorldDisable@0", "true"},
    {"AATechniquePreferredMP@0", "DLSS"},
    {"DLSSModeMP@0", "DLSS"},
    {"DLSSModelUI@0", "TRANSFORMER"},
    {"DxrDenoiser@0", "DLSS Ray Reconstruction"},
    {"DynamicSceneResolution@0", "false"},
    {"DxrMode@0", "On"},
    {"DxrMode@1", "High"},
    {"VRS@0", "true"},
    {"DLSSSharpnessMP@0", "0"},
    {"DLSSFrameGeneration@0", "false"},
    {"DLSSFrameGenerationmultiType@0", "Dynamic Auto"},
    {"DLSSPerfModeMP@0", "Maximum Quality"},
    {"MaxFpsInGame@0", "250"},
    {"MaxFpsOutOfFocus@0", "30"},
    {"SustainabilityReduceQualityIdle@0", "full"},
    {"SustainabilityReduceQualityIdle@1", "30 seconds"},
};
// A compliant file with one key's value replaced, or its entry dropped when value is null.
static std::string Config(const std::string &key = {}, const char *value = nullptr) {
    std::string text = "// Settings\r\n\r\n";
    for (const auto &[name, original] : compliant)
        if (key != name)
            text += ConfigLine(name, original);
        else if (value)
            text += ConfigLine(name, value);
    return text;
}
static std::vector<std::string> Lines(const std::string &text, unsigned &warnings) {
    std::vector<std::string> lines;
    warnings = rrgameconfig::Check(text, lines);
    return lines;
}
static bool Has(const std::vector<std::string> &lines, const std::string &line) {
    return std::find(lines.begin(), lines.end(), line) != lines.end();
}
static void Swap(std::string &text, const std::string &from, const std::string &to) {
    const auto at = text.find(from);
    assert(at != std::string::npos);
    text.replace(at, from.size(), to);
}
static std::string Lower(std::string text) {
    for (auto &c : text)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return text;
}
static void GameConfigTests() {
    using namespace rrgameconfig;
    // Entries: name before '@', index up to ';' or '=', value up to '//'; all trimmed.
    const auto entries = Parse("// header\r\n\r\nDxrMode@0;57752;20945 = On // allowed: On, Off\r\n"
                               "DxrMode@1;57752;20946 = High\r\n"
                               "  DxrDenoiser@0 = DLSS Ray Reconstruction  \r\n"
                               "DLSSPerfModeMP@0=Maximum Quality//x\r\nbare line\r\n"
                               "// Hidden@0 = true\r\nLate // note = true\r\nNoSuffix = 5\r\n"
                               "Tabs@2\t=\tz\r\nEmpty@0 =\r\nDxrMode@0;1;2 = Off");
    const char *expected[][3] = {{"DxrMode", "0", "On"},
                                 {"DxrMode", "1", "High"},
                                 {"DxrDenoiser", "0", "DLSS Ray Reconstruction"},
                                 {"DLSSPerfModeMP", "0", "Maximum Quality"},
                                 {"NoSuffix", "", "5"},
                                 {"Tabs", "2", "z"},
                                 {"Empty", "0", ""},
                                 {"DxrMode", "0", "Off"}};
    assert(entries.size() == std::size(expected));
    for (size_t i = 0; i < entries.size(); ++i)
        assert(entries[i].name == expected[i][0] && entries[i].index == expected[i][1] &&
               entries[i].value == expected[i][2]);
    assert(Parse("").empty() && Parse("\r\n\r\n// only\r\n").empty());
    // Find takes the first entry of the name, or of the name and index.
    assert(Find(entries, "dxrmode")->value == "On");
    assert(Find(entries, "DxrMode", "1")->value == "High");
    assert(!Find(entries, "DxrMode", "2") && !Find(entries, "Missing"));

    // A compliant file warns about nothing and logs every value, in this order.
    unsigned warnings{};
    auto lines = Lines(Config(), warnings);
    assert(warnings == 0 && lines.size() == 1);
    assert(lines[0] ==
           "Game config values: EnableVelocityBasedBlur=false, DLSSModelUI=TRANSFORMER, "
           "DxrDenoiser=DLSS Ray Reconstruction, DLSSSharpnessMP=0, DLSSFrameGeneration=false, "
           "DLSSFrameGenerationmultiType=Dynamic Auto, DLSSPerfModeMP=Maximum Quality, "
           "DxrMode@0=On, DxrMode@1=High, VRS=true, MaxFpsInGame=250, MaxFpsOutOfFocus=30, "
           "SustainabilityReduceQualityIdle@0=full, SustainabilityReduceQualityIdle@1=30 seconds");
    // Every entry of a shown name is logged, by index when there are several: a second VRS shows.
    lines = Lines(Config() + ConfigLine("VRS@0", "false"), warnings);
    assert(warnings == 0 && lines[0].find(", VRS@0=true, VRS@0=false, ") != std::string::npos);

    // Each required key: a wrong value, a different case and a missing entry.
    const char *required[][3] = {{"MotionBlur", "Off", "Cinematic Only"},
                                 {"WeaponMotionBlur", "false", "true"},
                                 {"EnableVelocityBasedBlur", "false", "true"},
                                 {"DofEnable", "Off", "Script"},
                                 {"DofWeaponDisable", "true", "false"},
                                 {"DofWorldDisable", "true", "false"},
                                 {"AATechniquePreferredMP", "DLSS", "SMAA"},
                                 {"DLSSModeMP", "DLSS", "Off"},
                                 {"DLSSModelUI", "TRANSFORMER", "CNN"},
                                 {"DxrDenoiser", "DLSS Ray Reconstruction", "Default"},
                                 {"DynamicSceneResolution", "false", "true"}};
    for (const auto &[name, wanted, wrong] : required) {
        const std::string key = std::string(name) + "@0";
        lines = Lines(Config(key, wrong), warnings);
        assert(warnings == 1 && lines.size() == 2);
        assert(lines[1] == std::string("Game config warning: ") + name + "=" + wrong +
                               "; INSTALLATION.txt requires " + wanted);
        for (const auto &spelling : {Lower(wanted), std::string(wanted) + "x"}) {
            lines = Lines(Config(key, spelling.c_str()), warnings);
            assert(warnings == (spelling == Lower(wanted) ? 0u : 1u));
        }
        lines = Lines(Config(key, nullptr), warnings);
        assert(warnings == 1 && lines.size() == 2);
        assert(lines[1] == std::string("Game config warning: ") + name +
                               " not found; INSTALLATION.txt requires " + wanted);
        // The gate looks only at read-only and VRS: a broken rule never turns RR off.
        assert(rrvrsconfig::Allowed(Config(key, wrong), true, true));
    }
    // Every entry of a ruled name must comply, whatever its index; when the name has several,
    // each warning names its entry.
    lines = Lines(Config() + ConfigLine("MotionBlur@1", "On"), warnings);
    assert(warnings == 1 && Has(lines, "Game config warning: MotionBlur@1=On; INSTALLATION.txt "
                                       "requires Off"));
    lines = Lines(Config("MotionBlur@0", "On") + ConfigLine("MotionBlur@1", "Script"), warnings);
    assert(warnings == 2 && lines.size() == 3);
    assert(lines[1] == "Game config warning: MotionBlur@0=On; INSTALLATION.txt requires Off");
    assert(lines[2] == "Game config warning: MotionBlur@1=Script; INSTALLATION.txt requires Off");
    lines = Lines(Config() + ConfigLine("MotionBlur@1", "off"), warnings);
    assert(warnings == 0);
    // Names and values match in any case: a lower-case key line, an upper-case value.
    lines = Lines(Config("MotionBlur@0", nullptr) + ConfigLine("motionblur@0", "On"), warnings);
    assert(warnings == 1 && lines.size() == 2 &&
           lines[1] == "Game config warning: MotionBlur=On; INSTALLATION.txt requires Off");
    lines = Lines(Config("MotionBlur@0", nullptr) + ConfigLine("motionblur@0", "off"), warnings);
    assert(warnings == 0);
    lines = Lines(Config("WeaponMotionBlur@0", "FALSE"), warnings);
    assert(warnings == 0);

    // DxrMode@0 must be On; @1 picks a quality and must not be On.
    lines = Lines(Config("DxrMode@0", "Off"), warnings);
    assert(warnings == 1 && lines[1] == "Game config warning: DxrMode@0=Off; INSTALLATION.txt "
                                        "requires On");
    lines = Lines(Config("DxrMode@0", nullptr), warnings);
    assert(warnings == 1 && lines[1] == "Game config warning: DxrMode@0 not found; "
                                        "INSTALLATION.txt requires On");
    for (const char *on : {"On", "on"}) {
        lines = Lines(Config("DxrMode@1", on), warnings);
        assert(warnings == 1 && lines[1] == std::string("Game config warning: DxrMode@1=") + on +
                                                "; INSTALLATION.txt requires a value other than "
                                                "On");
    }
    for (const char *quality : {"Off", "Low", "Ultra"}) {
        lines = Lines(Config("DxrMode@1", quality), warnings);
        assert(warnings == 0);
    }
    lines = Lines(Config("DxrMode@1", nullptr), warnings); // nothing set to On: no warning
    assert(warnings == 0 && lines[0].find("DxrMode@1=<missing>") != std::string::npos);

    // An empty or unrelated file warns for every required key and logs <missing>.
    for (const char *text : {"", "\r\n// nothing\r\nVRS@0 = true\r\n"}) {
        lines = Lines(text, warnings);
        assert(warnings == 12 && lines.size() == 13);
    }
    lines = Lines("", warnings);
    assert(lines[0] ==
           "Game config values: EnableVelocityBasedBlur=<missing>, DLSSModelUI=<missing>, "
           "DxrDenoiser=<missing>, DLSSSharpnessMP=<missing>, DLSSFrameGeneration=<missing>, "
           "DLSSFrameGenerationmultiType=<missing>, DLSSPerfModeMP=<missing>, "
           "DxrMode@0=<missing>, DxrMode@1=<missing>, VRS=<missing>, MaxFpsInGame=<missing>, "
           "MaxFpsOutOfFocus=<missing>, SustainabilityReduceQualityIdle=<missing>");
    assert(lines[1] == "Game config warning: MotionBlur not found; INSTALLATION.txt requires Off");

    // Notes are advice, not rules: logged after the warnings and never counted.
    const char *frame_generation_note =
        "Game config note: frame generation is on; RR health frames= counts rendered frames only, "
        "judge RR with it off";
    for (const char *on : {"true", "True"}) {
        lines = Lines(Config("DLSSFrameGeneration@0", on), warnings);
        assert(warnings == 0 && lines.size() == 2 && lines[1] == frame_generation_note);
    }
    for (const char *off : {"false", ""}) {
        lines = Lines(Config("DLSSFrameGeneration@0", off), warnings);
        assert(lines.size() == 1);
    }
    lines = Lines(Config("DLSSFrameGeneration@0", nullptr), warnings);
    assert(warnings == 0 && lines.size() == 1);
    lines = Lines(Config("DLSSSharpnessMP@0", "0.5"), warnings);
    assert(warnings == 0 && lines.size() == 2 &&
           lines[1] == "Game config note: DLSSSharpnessMP=0.5 may drive a game sharpening pass "
                       "after RR");
    for (const char *zero : {"0", "0.0", "0.00", "-0.0", "0e5", "off", ""}) {
        lines = Lines(Config("DLSSSharpnessMP@0", zero), warnings);
        assert(lines.size() == 1);
    }
    for (const char *some : {"1", "0.01", "1e-3", "-0.25"}) {
        lines = Lines(Config("DLSSSharpnessMP@0", some), warnings);
        assert(lines.size() == 2 && Has(lines, std::string("Game config note: DLSSSharpnessMP=") +
                                                   some + " may drive a game sharpening pass "
                                                          "after RR"));
    }
    lines = Lines(Config("DLSSSharpnessMP@0", nullptr), warnings);
    assert(lines.size() == 1);
    // Only the first entry of a name is judged for the notes; both are logged.
    lines = Lines(Config() + ConfigLine("DLSSFrameGeneration@0", "true"), warnings);
    assert(lines.size() == 1 &&
           lines[0].find(", DLSSFrameGeneration@0=false, DLSSFrameGeneration@0=true, ") !=
               std::string::npos);
    // Values, then warnings, then both notes in this order; only the warning counts.
    auto mixed = Config("MotionBlur@0", "On");
    Swap(mixed, "DLSSFrameGeneration@0;57752;20945 = false", "DLSSFrameGeneration@0 = true");
    Swap(mixed, "DLSSSharpnessMP@0;57752;20945 = 0", "DLSSSharpnessMP@0 = 0.5");
    lines = Lines(mixed, warnings);
    assert(warnings == 1 && lines.size() == 4);
    assert(lines[0].rfind("Game config values: ", 0) == 0 &&
           lines[0].find("DLSSFrameGeneration=true") != std::string::npos);
    assert(lines[1] == "Game config warning: MotionBlur=On; INSTALLATION.txt requires Off");
    assert(lines[2] == frame_generation_note);
    assert(lines[3].rfind("Game config note: DLSSSharpnessMP=0.5 ", 0) == 0);
    // The state line comes first and names the gate's own answer.
    const auto text = Config("MotionBlur@0", "On");
    File file;
    file.state = FileState::Read;
    file.readonly = true;
    file.text = text;
    lines.clear();
    assert(Report(file, rrvrsconfig::Allowed(file.text, file.readonly, true), lines) == 1);
    assert(lines.size() == 3 && lines[0] == "Game config: read-only=yes, VRS gate allows RR=yes" &&
           lines[1].rfind("Game config values: ", 0) == 0 &&
           lines[2] == "Game config warning: MotionBlur=On; INSTALLATION.txt requires Off");
    file.readonly = false;
    lines.clear();
    assert(Report(file, rrvrsconfig::Allowed(file.text, file.readonly, true), lines) == 1);
    assert(lines[0] == "Game config: read-only=no, VRS gate allows RR=no" && lines.size() == 3);
    file.state = FileState::NotFound;
    lines.clear();
    assert(Report(file, false, lines) == 0 &&
           lines == std::vector<std::string>{"Game config: file not found"});
    file.state = FileState::Unreadable;
    lines.clear();
    assert(Report(file, false, lines) == 0 &&
           lines == std::vector<std::string>{"Game config: file unreadable"});
    assert(File{}.state == FileState::NotFound);
}

int main() {
    GameConfigTests();
    assert(rrvrsconfig::Allowed("VRS@0;123 = true // preserved\n", true, true));
    assert(rrvrsconfig::Allowed("VRS@0;123 = false\n", true, true));
    assert(!rrvrsconfig::Allowed("VRS@0;123 = true\n", false, true));
    assert(!rrvrsconfig::Allowed("VRS@0;123 = true\nVRS@1;123 = true\n", true, true));
    assert(!rrvrsconfig::Allowed("VRS@0;123 = invalid\n", true, true));
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    d.SampleDesc.Count = 4;
    for (auto size : {std::pair{640u, 368u}, {1280u, 720u}, {3440u, 1440u}, {5120u, 2880u}}) {
        d.Width = size.first;
        d.Height = size.second;
        assert(rrglass::Shape(d, 1));
        assert(!rrglass::Shape(d, 0));
    }
    // The SSS mask must be the add-on's R16 guide at the lighting extent.
    D3D12_RESOURCE_DESC lighting{};
    lighting.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    lighting.Width = 1280;
    lighting.Height = 720;
    lighting.DepthOrArraySize = lighting.MipLevels = 1;
    lighting.SampleDesc.Count = 1;
    lighting.Format = DXGI_FORMAT_R32G32B32A32_UINT;
    auto mask = lighting;
    mask.Format = DXGI_FORMAT_R16_FLOAT;
    assert(rrsssmask::Shape(mask, lighting));
    assert(!rrsssmask::Shape(lighting, lighting));
    auto wrong = mask;
    wrong.Width = 640;
    assert(!rrsssmask::Shape(wrong, lighting));
    wrong = mask;
    wrong.Height = 360;
    assert(!rrsssmask::Shape(wrong, lighting));
    wrong = mask;
    wrong.SampleDesc.Count = 4;
    assert(!rrsssmask::Shape(wrong, lighting));
    wrong = mask;
    wrong.DepthOrArraySize = 4;
    assert(!rrsssmask::Shape(wrong, lighting));
    assert(std::string_view(rrsssmask::Label(rrsssmask::State::Active)) == "active");
    // Specular hit distance follows the runtime guide extent, not a fixed resolution.
    D3D12_RESOURCE_DESC h{};
    h.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    h.DepthOrArraySize = h.MipLevels = 1;
    h.SampleDesc.Count = 1;
    h.Format = DXGI_FORMAT_R16_FLOAT;
    const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    for (auto size : {std::pair{640u, 368u}, {1280u, 720u}, {1920u, 1088u}, {2560u, 1440u},
                      {3440u, 1440u}}) {
        h.Width = size.first;
        h.Height = size.second;
        assert(rrspecular::ValidSource(h, size.first, size.second, read));
        assert(rrspecular::ValidSource(h, size.first, size.second,
                                       read | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
        assert(!rrspecular::ValidSource(h, size.first + 2, size.second, read));
        assert(!rrspecular::ValidSource(h, size.first, size.second,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        assert(rrspecular::DispatchShape(size.first, size.second, (size.first + 15) / 16,
                                         (size.second + 15) / 16, 1));
        assert(!rrspecular::DispatchShape(size.first, size.second, (size.first + 7) / 8,
                                          (size.second + 15) / 16, 1));
    }
    h.Format = DXGI_FORMAT_R32_FLOAT;
    assert(!rrspecular::ValidSource(h, UINT(h.Width), h.Height, read));
    assert(rrspecular::DistanceBlur(0xc9ca6daa80593f3aull));
    assert(rrspecular::DistanceBlur(0x4894e5e7e717b468ull));
    assert(!rrspecular::DistanceBlur(0xd63e707761558424ull));
    rrspecular::Diagnostics stats;
    char line[320];
    assert(!stats.Summary(line, sizeof(line)));
    assert(std::string_view(line).find("blurDispatches=0") != std::string_view::npos);
    stats.Dispatch();
    stats.Observe(rrspecular::Stage::Shape);
    assert(stats.Summary(line, sizeof(line)));
    assert(std::string_view(line).find("lastReject=shape") != std::string_view::npos);
    assert(!stats.Summary(line, sizeof(line)));
    assert(stats.Scan(1) && !stats.Scan(1) && stats.Candidate(1) && !stats.Candidate(1));
    d.Width = 16385;
    assert(!rrglass::Shape(d, 1));
    d.Width = 1920;
    d.SampleDesc.Count = 1;
    assert(!rrglass::Shape(d, 1));

    // Reset causes are reported without changing reset decisions.
    rrlive::HistoryPolicy policy;
    assert(policy.Next(1, 0, false) && policy.LastReason() == (rrlive::HistoryPolicy::Rejected |
                                                                rrlive::HistoryPolicy::Gap));
    assert(!policy.Next(2, 0, false) && policy.LastReason() == 0);
    assert(policy.Next(3, 0, true) && policy.LastReason() == rrlive::HistoryPolicy::Cut);
    assert(policy.Next(4, 1, false) && policy.LastReason() == rrlive::HistoryPolicy::Generation);
    assert(policy.Next(6, 1, false) && policy.LastReason() == rrlive::HistoryPolicy::Gap);
    policy.Reject();
    assert(policy.Next(7, 1, false) && policy.LastReason() == rrlive::HistoryPolicy::Rejected);

    // Health checks once per interval and logs a state change, with activity, then starts over.
    // Presentation counters arrive lock-free; each interval takes its own window.
    auto &shown = rrhealth::presentation;
    shown.presents = 50; // before the first interval: discarded
    shown.fg_evaluations = 9;
    rrhealth::Tracker health;
    std::string report;
    assert(!health.Report(1000, report));
    assert(shown.presents == 0 && shown.fg_evaluations == 0);
    // Each guide gets its own count (sss 4, glass 1, hit 2, responsivity 3), so a swap shows.
    health.Frame({0, 1, 0, 0}, {0, 0, true, false, false, true});
    health.Frame({0, 1, 12, 0}, {3, 0, true, false, true, true});
    health.Frame({1, 0, 0, 30}, {1, 7, true, true, true, true});
    health.Frame({0, 1, 0, 0}, {0, 0, true, false, false, false});
    health.Reset(rrlive::HistoryPolicy::Rejected | rrlive::HistoryPolicy::Gap);
    shown.presents += 16;
    shown.unfocused_presents += 5;
    shown.focused = true;
    shown.fg_evaluations += 12;
    assert(!health.Report(1000 + rrhealth::Tracker::Interval - 1, report));
    assert(shown.presents == 16);
    assert(health.Report(1000 + rrhealth::Tracker::Interval, report));
    assert(report.find("frames=4 rejected=1 resets=1") != std::string::npos);
    assert(report.find("[rejected=1 cut=0 generation=0 gap=1]") != std::string::npos);
    assert(report.find("isolatedGuideFrames=1 maxIsolatedPixels=12") != std::string::npos);
    assert(report.find("sanitizedSssFrames=1 maxSanitizedSamples=30") != std::string::npos);
    assert(report.find("maxSanitizedSamples=30 guides[sss=4 glass=1 hit=2 responsivity=3] "
                       "nonFiniteColorFrames=2 maxNonFiniteColorPixels=3 "
                       "nonFiniteOutputFrames=1 maxNonFiniteOutputPixels=7 presents=16 "
                       "unfocusedPresents=5 focused=1 fgEvaluations=12") != std::string::npos);
    assert(shown.presents == 0 && shown.unfocused_presents == 0 && shown.fg_evaluations == 0);
    // An inactive interval logs nothing and still drops its presentation counts.
    shown.presents = 7;
    shown.unfocused_presents = 7;
    shown.fg_evaluations = 7;
    report.clear();
    assert(!health.Report(1000 + 2 * rrhealth::Tracker::Interval, report) && report.empty());
    assert(shown.presents == 0 && shown.unfocused_presents == 0 && shown.fg_evaluations == 0);
    // The next line holds only its own interval; focus is the latest present's.
    health.Frame({0, 1, 0, 0});
    shown.presents = 2;
    shown.unfocused_presents = 2;
    shown.focused = false;
    assert(health.Report(1000 + 3 * rrhealth::Tracker::Interval, report));
    assert(report.find("frames=1 rejected=0 resets=0 [rejected=0 cut=0 generation=0 gap=0] "
                       "isolatedGuideFrames=0 maxIsolatedPixels=0 sanitizedSssFrames=0 "
                       "maxSanitizedSamples=0 guides[sss=0 glass=0 hit=0 responsivity=0] "
                       "nonFiniteColorFrames=0 maxNonFiniteColorPixels=0 "
                       "nonFiniteOutputFrames=0 maxNonFiniteOutputPixels=0 presents=2 "
                       "unfocusedPresents=2 focused=0 fgEvaluations=0") != std::string::npos);
    // The same state again is held back until the slow summary, which counts every frame since
    // the last line; a state change (a rejected frame) logs at its interval.
    const std::uint64_t logged_at = 1000 + 3 * rrhealth::Tracker::Interval;
    report.clear();
    for (unsigned i = 1; i < rrhealth::Tracker::Summary / rrhealth::Tracker::Interval; ++i) {
        health.Frame({0, 1, 0, 0});
        assert(!health.Report(logged_at + i * rrhealth::Tracker::Interval, report));
    }
    health.Frame({0, 1, 0, 0});
    assert(health.Report(logged_at + rrhealth::Tracker::Summary, report));
    assert(report.rfind("RR health (300s): frames=30 rejected=0 ", 0) == 0);
    health.Frame({1, 0, 0, 0});
    assert(health.Report(logged_at + rrhealth::Tracker::Summary + rrhealth::Tracker::Interval,
                         report));
    assert(report.find("frames=1 rejected=1 ") != std::string::npos);
    // The widest line is never truncated.
    rrhealth::Tracker wide;
    assert(!wide.Report(0, report));
    wide.Frame({~0u, 0, ~0u, ~0u}, {~0u, ~0u, true, true, true, true});
    shown.presents = shown.unfocused_presents = shown.fg_evaluations = ~0u;
    assert(wide.Report(~0ull, report));
    assert(report.size() > 400 && report.ends_with(" fgEvaluations=4294967295"));
    shown.presents = shown.unfocused_presents = shown.fg_evaluations = 0;
    // NGX feature kinds the probe tells apart (NVSDK_NGX_Feature values).
    assert(rrtrace::FeatureKind(1) == rrtrace::Kind::SR);
    assert(rrtrace::FeatureKind(13) == rrtrace::Kind::RR);
    assert(rrtrace::FeatureKind(11) == rrtrace::Kind::FG);
    assert(rrtrace::FeatureKind(12) == rrtrace::Kind::Other);
    assert(std::string_view(rrtrace::Name(rrtrace::Kind::FG)) == "FG");

    // Water responsivity needs preset F, a nonzero strength and this frame's water.
    rrwater::Settings water;
    water.responsivity = true;
    assert(rrwater::UseResponsivity(water, true, true, 6));
    assert(!rrwater::UseResponsivity(water, false, true, 6));
    assert(!rrwater::UseResponsivity(water, true, false, 6));
    for (unsigned preset : {0u, 4u, 5u})
        assert(!rrwater::UseResponsivity(water, true, true, preset));
    water.strength = -0.5f; // clamps to 0
    assert(!rrwater::UseResponsivity(water, true, true, 6));
    assert(rrwater::Clamp(2.0f) == 1.0f && rrwater::Clamp(0.25f) == 0.25f);
    assert(rrwater::Clamp(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
}
