#pragma once
#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_params.h>
namespace rrpreset {
static_assert(unsigned(NVSDK_NGX_RayReconstruction_Hint_Render_Preset_Default) == 0);
static_assert(unsigned(NVSDK_NGX_RayReconstruction_Hint_Render_Preset_D) == 4);
static_assert(unsigned(NVSDK_NGX_RayReconstruction_Hint_Render_Preset_E) == 5);
static_assert(unsigned(NVSDK_NGX_RayReconstruction_Hint_Render_Preset_F) == 6);
inline constexpr unsigned values[] = {0, 4, 5, 6};
inline constexpr const char *names[] = {"Default", "D", "E", "F"};
inline constexpr bool Valid(unsigned p) {
    return p == 0 || p == 4 || p == 5 || p == 6;
}

struct Status {
    unsigned desired = 6, effective = 6;
    bool has_effective{}, pending{};
};
struct Selection {
    unsigned desired = 6, effective = 6;
    bool has_effective{}, draining{};
    constexpr bool Pending() const {
        return draining || (has_effective && desired != effective);
    }
    constexpr bool Request(unsigned value) {
        if (!Valid(value))
            return false;
        desired = value;
        return true;
    }
    constexpr void Retired() {
        has_effective = false;
        draining = true;
    }
    // value: the preset the new RR was created with, which a request may since have changed.
    constexpr void Created(unsigned value) {
        effective = value;
        has_effective = true;
        draining = false;
    }
    constexpr void Created() {
        Created(desired);
    }
};
// Never release a replayable recording, an unsealed evaluation, or GPU work.
inline constexpr bool CanRetire(bool poisoned, bool pre_live, bool sr_live, bool sss_live,
                                bool transparency_live, bool pending_sr, bool recorded_unsealed,
                                bool submitted_unfenced, bool complete) {
    return !poisoned && !pre_live && !sr_live && !sss_live && !transparency_live && !pending_sr &&
           !recorded_unsealed && !submitted_unfenced && complete;
}

inline constexpr const char *keys[] = {
    NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA,
    NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality,
    NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced,
    NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance,
    NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance,
    NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality};
inline void Set(NVSDK_NGX_Parameter &p, unsigned value) {
    for (auto key : keys)
        p.Set(key, value);
}
inline bool Requested(const NVSDK_NGX_Parameter &p, unsigned want) {
    for (auto key : keys) {
        unsigned value{};
        if (NVSDK_NGX_FAILED(p.Get(key, &value)) || value != want)
            return false;
    }
    return true;
}
} // namespace rrpreset
