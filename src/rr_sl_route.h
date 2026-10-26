#pragma once
#include <d3d12.h>
#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_d.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "rr_evaluation_params.h"
// RR's evaluation through the game's Streamline DLSS-D (sl.dlss_d), on a viewport of RR's own,
// when RR replaces the game's SR. A game with built-in RR calls DLSS-D this way, and add-ons
// beneath RR that take their guides from the game's DLSS calls (DLSS5 NR) accept it; they
// ignore RR's direct NGX call.
namespace rrslroute {
// RR's own viewport beside the game's: its tags, constants and options never touch the game's
// (frame generation keeps reading the game's tags).
inline constexpr uint32_t Viewport(uint32_t game) {
    return 0x52520000u | (game & 0xffffu);
}
// NVSDK_NGX_PerfQuality_Value -> sl::DLSSMode.
inline constexpr sl::DLSSMode Mode(int quality) {
    switch (quality) {
    case 0:
        return sl::DLSSMode::eMaxPerformance;
    case 1:
        return sl::DLSSMode::eBalanced;
    case 2:
        return sl::DLSSMode::eMaxQuality;
    case 3:
        return sl::DLSSMode::eUltraPerformance;
    case 4:
        return sl::DLSSMode::eUltraQuality;
    case 5:
        return sl::DLSSMode::eDLAA;
    default:
        return sl::DLSSMode::eOff;
    }
}
// Inverse of a 4x4 row-major matrix; false when singular.
inline bool Invert(const float *m, float *out) {
    double inv[16], det;
    const double a[16] = {m[0], m[1], m[2],  m[3],  m[4],  m[5],  m[6],  m[7],
                          m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]};
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] +
             a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] -
             a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] +
             a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] -
              a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] -
             a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] +
             a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] -
             a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] +
              a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] +
             a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] -
             a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] +
              a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] -
              a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] -
             a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] +
             a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] -
              a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] +
              a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (!std::isfinite(det) || std::abs(det) < 1e-30)
        return false;
    for (int i = 0; i < 16; ++i)
        out[i] = float(inv[i] / det);
    return true;
}
// DLSS-D options for RR's viewport, from the same values RR's direct evaluation uses.
inline bool Options(const rreval::Options &o, unsigned preset, const float *world_to_view,
                    sl::DLSSDOptions &out) {
    out = {};
    out.mode = Mode(o.quality);
    out.outputWidth = o.out_width;
    out.outputHeight = o.out_height;
    out.preExposure = o.pre_exposure;
    out.exposureScale = o.exposure_scale;
    out.colorBuffersHDR = (o.flags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) ? sl::Boolean::eTrue
                                                                          : sl::Boolean::eFalse;
    out.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
    std::memcpy(&out.worldToCameraView, world_to_view, sizeof(float) * 16);
    if (!Invert(world_to_view, reinterpret_cast<float *>(&out.cameraViewToWorld)))
        return false;
    const auto p = sl::DLSSDPreset(preset);
    out.dlaaPreset = out.qualityPreset = out.balancedPreset = out.performancePreset =
        out.ultraPerformancePreset = out.ultraQualityPreset = p;
    return out.mode != sl::DLSSMode::eOff;
}
// The tags for one evaluation: inputs in non-pixel-shader read and the output in unordered
// access, the states RR's direct evaluation hands NGX. Null optional guides are left out.
struct Tags {
    std::array<sl::Resource, 11> resources{};
    std::array<sl::ResourceTag, 11> tags{
        sl::ResourceTag{nullptr, 0, sl::eValidUntilEvaluate}, {nullptr, 0, sl::eValidUntilEvaluate},
        {nullptr, 0, sl::eValidUntilEvaluate}, {nullptr, 0, sl::eValidUntilEvaluate},
        {nullptr, 0, sl::eValidUntilEvaluate}, {nullptr, 0, sl::eValidUntilEvaluate},
        {nullptr, 0, sl::eValidUntilEvaluate}, {nullptr, 0, sl::eValidUntilEvaluate},
        {nullptr, 0, sl::eValidUntilEvaluate}, {nullptr, 0, sl::eValidUntilEvaluate},
        {nullptr, 0, sl::eValidUntilEvaluate}};
    uint32_t count{};
    Tags() = default;
    Tags(const Tags &) = delete; // each tag points into resources
    Tags &operator=(const Tags &) = delete;
    void Add(ID3D12Resource *r, sl::BufferType type, D3D12_RESOURCE_STATES state, uint32_t left,
             uint32_t top, uint32_t width, uint32_t height) {
        if (!r || count == tags.size())
            return;
        resources[count] = sl::Resource(sl::ResourceType::eTex2d, r, uint32_t(state));
        sl::Extent e{top, left, width, height};
        tags[count] = sl::ResourceTag(&resources[count], type, sl::eValidUntilEvaluate, &e);
        ++count;
    }
};
inline void Build(const rreval::Options &o, const rreval::Resources &r, Tags &t) {
    constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    t.count = 0;
    t.Add(r.color, sl::kBufferTypeScalingInputColor, read, o.color_x, o.color_y, o.width,
          o.height);
    t.Add(r.output, sl::kBufferTypeScalingOutputColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 0,
          0, o.out_width, o.out_height);
    t.Add(r.depth, sl::kBufferTypeDepth, read, o.depth_x, o.depth_y, o.width, o.height);
    t.Add(r.motion, sl::kBufferTypeMotionVectors, read, o.motion_x, o.motion_y, o.width,
          o.height);
    t.Add(r.normal, sl::kBufferTypeNormalRoughness, read, 0, 0, o.width, o.height);
    t.Add(r.diffuse, sl::kBufferTypeAlbedo, read, 0, 0, o.width, o.height);
    t.Add(r.specular, sl::kBufferTypeSpecularAlbedo, read, 0, 0, o.width, o.height);
    t.Add(r.specular_hit, sl::kBufferTypeSpecularHitDistance, read, 0, 0, o.width, o.height);
    t.Add(r.sss, sl::kBufferTypeScreenSpaceSubsurfaceScatteringGuide, read, 0, 0, o.width,
          o.height);
    t.Add(r.before_transparency, sl::kBufferTypeColorBeforeTransparency, read, 0, 0, o.width,
          o.height);
    t.Add(r.responsivity, sl::kBufferTypeResponsivityMask, read, 0, 0, o.width, o.height);
}
} // namespace rrslroute
