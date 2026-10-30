#ifndef MW4_RR_EDGE_FILL_H
#define MW4_RR_EDGE_FILL_H
// Edge-aware no-history fill, shared by shaders/rr_sparse_scale_vrrt.hlsl (RR_FILL_EDGE)
// and the CPU tests, so both evaluate the same weights.
//
// On SSS-masked lobe 2 (rrlighting::SssHistory2), a pixel with neither a fresh sample nor
// native history is filled from fresh, unboosted lobe-2 samples within RadiusPx. The
// baseline (GroupMean) averages every such sample in its 16x16 group equally. This
// variant reads the whole frame's raw snapshot, so support is bounded only by the
// lighting extent, and weights each sample by whether it lies on the centre's surface:
//  - both pixels classified by this frame's guide conversion (Status 1 ordinary or
//    3 special; 0 padding/rejected, 2 sky, 4 invalid are never used);
//  - the same prepass PackedSurface.w word, the native spatial denoiser's own
//    same-surface test (sss-review/native-spatial-denoiser-notes.md); bit 0 is the
//    weapon camera's depth slice, so world and weapon never mix;
//  - linear depth within DepthTolerance per pixel of distance. The projection is infinite
//    reverse-Z (rr_weapon_depth.h: depth = near / z), so z_n / z_c = d_c / d_n with no
//    projection constants; the weapon slice (d > Slice) is its own camera's reverse-Z
//    t = (d - Slice) / (1 - Slice);
//  - converted guide normals within MinCosine (about 25 degrees).
// Weights fall linearly to 0 at both limits and the result is the weighted mean of the
// valid samples, normalized by their total weight. Spatial weights stay uniform, as in
// the baseline, so a difference is attributable to edge rejection and boundary support.
// Outcomes: Edge (a weighted mean), NoNeighbour (the centre is classified but no sample
// passes: the pixel keeps its own native temporal lobe 2, the selection before the spatial
// fill existed, instead of borrowing another surface's light) and Unclassified (the centre
// has no usable surface: the baseline group mean, so those pixels match the baseline).
#define RR_EDGE_RADIUS 2
#define RR_EDGE_SLICE (63.0f / 64.0f)
#define RR_EDGE_MIN_COSINE 0.906f
#define RR_EDGE_DEPTH_TOLERANCE 0.02f
#define RR_EDGE_OUTCOME_UNCLASSIFIED 0u
#define RR_EDGE_OUTCOME_EDGE 1u
#define RR_EDGE_OUTCOME_NO_NEIGHBOUR 2u

#ifdef __cplusplus
#include <atomic>
#include <cmath>
#include <cstdint>
namespace rredgefill {
using uint = unsigned;
struct float4 {
    float x{}, y{}, z{}, w{};
};
inline float4 operator*(float4 a, float s) { return {a.x * s, a.y * s, a.z * s, a.w * s}; }
inline float4 &operator+=(float4 &a, float4 b) {
    a.x += b.x, a.y += b.y, a.z += b.z, a.w += b.w;
    return a;
}
inline float abs(float x) { return std::fabs(x); }
inline float sqrt(float x) { return std::sqrt(x); }
#define RR_EDGE_FN inline
#define RR_EDGE_INOUT(type) type &
#else
#define RR_EDGE_FN
#define RR_EDGE_INOUT(type) inout type
#endif

// One pixel's surface: prepass PackedSurface .x (as float) and .w, the converted guide
// normal and the guide Status.
struct EdgeSurface {
    float depth;
    uint word;
    float nx, ny, nz;
    uint status;
};

RR_EDGE_FN bool EdgeClassified(EdgeSurface s) {
    if (s.status != 1u && s.status != 3u)
        return false;
    // Comparisons, not isfinite: NaN fails them, and fast-math cannot drop them.
    if (!(s.depth > 0.0f) || !(s.depth <= 1.0f))
        return false;
    // The conversion rejects a slice bit that disagrees with the depth; so does this.
    if (((s.word & 1u) != 0u) != (s.depth > RR_EDGE_SLICE))
        return false;
    const float n2 = s.nx * s.nx + s.ny * s.ny + s.nz * s.nz;
    return n2 > 0.25f && n2 < 4.0f;
}
// Reverse-Z depth in the pixel's own camera: proportional to 1 / linear depth.
RR_EDGE_FN float EdgeReverseZ(EdgeSurface s) {
    return (s.word & 1u) != 0u ? (s.depth - RR_EDGE_SLICE) * 64.0f : s.depth;
}
// distance: the neighbour's distance from the centre in pixels (> 0).
RR_EDGE_FN float EdgeDepthWeight(float centre, float neighbour, float distance) {
    if (!(centre > 0.0f) || !(neighbour > 0.0f) || !(distance > 0.0f))
        return 0.0f;
    const float relative = abs(centre / neighbour - 1.0f); // |z_n / z_c - 1|
    const float tolerance = RR_EDGE_DEPTH_TOLERANCE * distance;
    if (!(relative < tolerance))
        return 0.0f;
    return 1.0f - relative / tolerance;
}
RR_EDGE_FN float EdgeNormalWeight(EdgeSurface c, EdgeSurface n) {
    const float lc = sqrt(c.nx * c.nx + c.ny * c.ny + c.nz * c.nz),
                ln = sqrt(n.nx * n.nx + n.ny * n.ny + n.nz * n.nz);
    const float cosine = (c.nx * n.nx + c.ny * n.ny + c.nz * n.nz) / (lc * ln);
    if (!(cosine > RR_EDGE_MIN_COSINE))
        return 0.0f;
    const float w = (cosine - RR_EDGE_MIN_COSINE) / (1.0f - RR_EDGE_MIN_COSINE);
    return w < 1.0f ? w : 1.0f;
}
// Weight of neighbour n (dx, dy pixels from the centre c) in c's fill; 0 = rejected.
RR_EDGE_FN float EdgeWeight(EdgeSurface c, EdgeSurface n, int dx, int dy) {
    if ((dx == 0 && dy == 0) || !EdgeClassified(c) || !EdgeClassified(n) || c.word != n.word)
        return 0.0f;
    const float distance = sqrt(float(dx * dx + dy * dy));
    return EdgeDepthWeight(EdgeReverseZ(c), EdgeReverseZ(n), distance) * EdgeNormalWeight(c, n);
}
// The neighbour pixel exists in the w x h lighting extent (group and strip boundaries
// do not limit support).
RR_EDGE_FN bool EdgeInside(int x, int y, uint w, uint h) {
    return x >= 0 && y >= 0 && uint(x) < w && uint(y) < h;
}
// A fresh lobe-2 sample, as the baseline: a nonzero lobe-2 sample count and a finite,
// nonnegative unboosted value.
RR_EDGE_FN bool EdgeFresh(uint weights, float4 v) {
    return ((weights >> 8) & 4095u) != 0u && v.x >= 0.0f && v.y >= 0.0f && v.z >= 0.0f &&
           v.w >= 0.0f && v.x <= 65504.0f && v.y <= 65504.0f && v.z <= 65504.0f &&
           v.w <= 65504.0f;
}
RR_EDGE_FN void EdgeAccumulate(RR_EDGE_INOUT(float4) sum, RR_EDGE_INOUT(float) total, float4 v,
                               float weight) {
    if (weight > 0.0f) {
        sum += v * weight;
        total += weight;
    }
}
// The normalized result, or false when no sample passed (NoNeighbour).
RR_EDGE_FN bool EdgeResolve(float4 sum, float total, RR_EDGE_INOUT(float4) mean) {
    if (!(total > 0.0f))
        return false;
    mean = sum * (1.0f / total);
    mean.x = mean.x < 65504.0f ? mean.x : 65504.0f;
    mean.y = mean.y < 65504.0f ? mean.y : 65504.0f;
    mean.z = mean.z < 65504.0f ? mean.z : 65504.0f;
    mean.w = mean.w < 65504.0f ? mean.w : 65504.0f;
    return true;
}

#ifdef __cplusplus
// The panel switch, read by the first spatial pass; off until the settings are loaded.
// Changing it selects another shader variant only: no history reset, no RR restart.
inline std::atomic<bool> enabled{};
// Latest outcome of an edge-fill preparation, for the panel.
enum class State : unsigned { Idle, Active, Baseline, Unavailable };
inline constexpr const char *Label(State s) {
    switch (s) {
    case State::Active:
        return "active";
    case State::Baseline:
        return "off (baseline group mean)";
    case State::Unavailable:
        return "unavailable this frame (baseline group mean; see ReShade.log)";
    default:
        return "not running";
    }
}
} // namespace rredgefill
#endif
#endif
