#pragma once
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include "rr_weapon_depth.h"
// Subsurface Scattering coverage experiment (development, a panel checkbox, off by default).
//
// The SSS-masked lighting source (rrlighting::SssHistory2) takes native temporal lobe 2 on
// Subsurface Scattering pixels. Its baseline decides "SSS pixel" from an earlier frame's RR
// Subsurface Scattering guide, read at the same screen pixel: the guide is 1 to 3 frames old
// (it is resolved after the lighting pass), so camera and object motion move that decision
// onto another surface, and a zero guide value (a luminance difference) does not mean "no
// scattering material".
//
// The experiment keeps the RR guide exactly as it is and decides from a separate coverage
// record instead:
// - Coverage: which pixels the game's Subsurface Scattering draw shaded, from its own stencil
//   test. The owned stencil initializer writes Marker (instead of 0) where the stencil
//   rejects, so the resolve can tell "not drawn" from "drawn, guide 0"; the RR guide still
//   gets 0 there (shaders/rr_sss_deinterleave.hlsl).
// - Surface key: this frame's hardware depth and coarse normal from the prepass
//   PackedSurface, written right after the prepass (shaders/rr_sss_coverage_key.hlsl).
// - At a later frame's lighting pass each pixel is reprojected into the coverage record's
//   frame with both frames' cameras and its own depth, for the record's actual age, and the
//   record is used only where the stored surface there matches (depth and normal); off-screen,
//   disoccluded, mismatched and unknown pixels take raw lighting (the defined fallback the
//   baseline uses whenever it has no guide).
// Current-frame coverage before the lighting pass is not used: the Subsurface Scattering
// draw (and so its stencil test and material inputs) runs after it, and the timing of the
// stencil buffer at the lighting pass is not proven. The game's motion vectors are not used
// for the same reason: they are only known to be written by the time of the SR call. So the
// reprojection is camera-only; moving objects are covered only by the surface match (see
// RejectReason), never by assumed motion.
namespace rrssscov {
// Which predicate the SSS-masked lighting source uses.
enum class Selection : unsigned { Baseline, Coverage };
// Always the coverage selection (Beta 3), with no panel control; Baseline stays for tests.
inline std::atomic<unsigned> selection{unsigned(Selection::Coverage)};
// Count both predicates' choices every frame for the panel (diagnostic only).
inline std::atomic<bool> compare{};
inline bool Wanted() {
    return selection.load(std::memory_order_relaxed) == unsigned(Selection::Coverage) ||
           compare.load(std::memory_order_relaxed);
}

// The stencil initializer's value for stencil-rejected samples while coverage is recorded.
// It is outside the guide's valid range (|value| < 65504, rrsss::ValidScalar), so it can only
// collide with a Subsurface Scattering output that was already unusable; the resolve gives
// the RR guide 0 for it, as for the baseline initializer's 0.
inline constexpr float Marker = 65504.0f;
// Resolved coverage per full-resolution pixel (R8_UINT).
enum Coverage : uint32_t { NotDrawn = 0, Drawn = 1, Unknown = 2 };

// History ring: one entry per frame, indexed by frame number. A record up to MaxAge frames
// old never shares an entry with the frame reading it.
inline constexpr unsigned Ring = 4, MaxAge = 3;
static_assert(MaxAge < Ring);
inline constexpr unsigned Entry(unsigned frame) {
    return frame % Ring;
}

// ---- Surface key (R32_UINT), CPU mirror of shaders/rr_sss_coverage_key.hlsl ----
// High 16 bits: 0 for sky or an unusable depth; else for world pixels the float16 of the
// hardware (reverse-Z) depth, and for the first-person weapon slice (depth above
// rrweapondepth::Slice) bit 15 set and the float16 of the weapon camera's own depth t.
// Low 16 bits: PackedSurface.y's octahedral normal (2 x 8 bits).
inline constexpr uint32_t WeaponBit = 0x8000u;
// IEEE binary16 with round-to-nearest-even, as f32tof16; v is finite and >= 0 here.
inline uint16_t Half(float v) {
    uint32_t b;
    std::memcpy(&b, &v, 4);
    const uint32_t sign = (b >> 16) & 0x8000u;
    int32_t e = int32_t((b >> 23) & 255) - 127 + 15;
    uint32_t m = b & 0x7fffffu;
    if (((b >> 23) & 255) == 255)
        return uint16_t(sign | 0x7c00u | (m ? 0x200u : 0));
    if (e >= 31)
        return uint16_t(sign | 0x7c00u);
    if (e <= 0) {
        if (e < -10)
            return uint16_t(sign);
        m |= 0x800000u;
        const uint32_t shift = uint32_t(14 - e);
        uint32_t h = m >> shift;
        const uint32_t rest = m & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rest > half || (rest == half && (h & 1)))
            ++h;
        return uint16_t(sign | h);
    }
    uint32_t h = (uint32_t(e) << 10) | (m >> 13);
    const uint32_t rest = m & 0x1fffu;
    if (rest > 0x1000u || (rest == 0x1000u && (h & 1)))
        ++h;
    return uint16_t(sign | h);
}
inline float Float(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000u) << 16, e = (h >> 10) & 31, m = h & 1023;
    uint32_t b;
    if (e == 0) {
        if (!m)
            b = sign;
        else {
            int shift = 0;
            uint32_t mm = m;
            while (!(mm & 1024)) {
                mm <<= 1;
                ++shift;
            }
            b = sign | (uint32_t(127 - 15 - shift + 1) << 23) | ((mm & 1023) << 13);
        }
    } else if (e == 31)
        b = sign | 0x7f800000u | (m << 13);
    else
        b = sign | ((e - 15 + 127) << 23) | (m << 13);
    float v;
    std::memcpy(&v, &b, 4);
    return v;
}
inline uint32_t Key(float depth, uint32_t surface_y) {
    uint32_t high = 0;
    if (std::isfinite(depth) && depth > 0 && depth <= 1) {
        if (depth > rrweapondepth::Slice)
            high = WeaponBit |
                   (Half(std::fmin((depth - rrweapondepth::Slice) * 64, 1.0f)) & 0x7fffu);
        else
            high = Half(depth) & 0x7fffu;
    }
    return (high << 16) | (surface_y & 0xffffu);
}
struct Surface {
    bool present{}, weapon{};
    float depth{}; // hardware depth, or the weapon camera's t
    float normal[3]{};
};
inline void DecodeNormal(uint32_t packed, float n[3]) {
    // rr_guide_math.hlsli RRDecodeOct(..., 255): 8-bit octahedral.
    float x = float(packed & 255u) * (2.0f / 255.0f) - 1.0f,
          y = float((packed >> 8) & 255u) * (2.0f / 255.0f) - 1.0f;
    const float z = 1.0f - std::fabs(x) - std::fabs(y), fold = std::fmax(-z, 0.0f);
    x += x > 0 ? -fold : fold;
    y += y > 0 ? -fold : fold;
    const float l = std::sqrt(x * x + y * y + z * z);
    n[0] = x / l;
    n[1] = y / l;
    n[2] = z / l;
}
inline Surface Decode(uint32_t key) {
    Surface s;
    const uint32_t high = key >> 16;
    if (!(high & 0x7fffu))
        return s;
    s.present = true;
    s.weapon = (high & WeaponBit) != 0;
    s.depth = Float(uint16_t(high & 0x7fffu));
    DecodeNormal(key & 0xffffu, s.normal);
    return s;
}

// ---- Cameras and the reprojection constants ----
// A frame's camera as RR is given it (Slot::view/projection): world-to-view and view-to-clip,
// row-vector (clip = [x y z 1] * view * projection), infinite reverse-Z with clip w = view z.
// The projection carries the frame's jitter in its x/y columns, so reprojecting a pixel
// center with both projections maps between the two frames' jittered sample positions.
struct Camera {
    std::array<float, 16> view{}, projection{};
    unsigned frame{}, viewport{};
    bool valid{}, reset{};
};
// A coverage record: the frame whose Subsurface Scattering pass coverage and surface key are
// in history entry Entry(frame), sealed with that frame's admitted RR guide.
struct Record {
    bool valid{};
    unsigned frame{}, generation{}, width{}, height{};
    Camera camera;
};
// Why a frame cannot use the coverage record (the whole frame then falls back).
enum class FrameReason : unsigned {
    Ready,
    NoRecord,   // no admitted coverage record (or none recorded with coverage)
    Generation, // RR was rebuilt (resize, preset, device) since the record
    Extent,     // record and lighting extents differ
    Viewport,
    Age,        // record is not 1..MaxAge frames older
    Reset,      // this frame's camera reports a history reset (cut)
    Camera,     // a camera or projection cannot be used for reprojection
    NoStorage,  // history storage not allocated or not written for a frame
    Count
};
inline const char *Label(FrameReason r) {
    switch (r) {
    case FrameReason::Ready:
        return "ready";
    case FrameReason::NoRecord:
        return "no Subsurface Scattering coverage record yet";
    case FrameReason::Generation:
        return "record from before RR was rebuilt";
    case FrameReason::Extent:
        return "record size differs from the lighting size";
    case FrameReason::Viewport:
        return "record from another view";
    case FrameReason::Age:
        return "record too old";
    case FrameReason::Reset:
        return "camera history reset this frame";
    case FrameReason::Camera:
        return "camera cannot be reprojected";
    case FrameReason::NoStorage:
        return "coverage history not written for this frame";
    default:
        return "unknown";
    }
}
inline FrameReason Check(const Record &r, unsigned frame, unsigned generation, unsigned width,
                         unsigned height, unsigned viewport, bool reset) {
    if (!r.valid)
        return FrameReason::NoRecord;
    if (r.generation != generation)
        return FrameReason::Generation;
    if (r.width != width || r.height != height)
        return FrameReason::Extent;
    if (r.camera.viewport != viewport)
        return FrameReason::Viewport;
    const unsigned age = frame - r.frame; // wraps for a record "from the future"
    if (age == 0 || age > MaxAge)
        return FrameReason::Age;
    if (reset)
        return FrameReason::Reset;
    if (!r.camera.valid)
        return FrameReason::Camera;
    return FrameReason::Ready;
}

// Shader flags.
inline constexpr uint32_t ApplyFlag = 1; // use the coverage decision (else the baseline's)
// Root constants b4 of the SSS-masked lighting variant (32 words).
struct Constants {
    uint32_t width, height, flags, age;
    float ax, bx, ay, by;                                       // current view position
    float current_scale, current_offset, previous_scale, previous_offset; // depth to view z
    float reproject[16]; // current view position -> previous clip, row-vector
    float depth_tolerance, normal_cos, pad0, pad1;
};
static_assert(sizeof(Constants) == 128);
inline constexpr unsigned ConstantWords = sizeof(Constants) / 4;
// Relative view-depth tolerance against each 2x2 footprint texel, and the minimum cosine
// between the current and stored normals (8-bit octahedral, so loose).
inline constexpr float DepthTolerance = 0.03f, NormalCos = 0.5f;

using Matrix = std::array<double, 16>;
inline Matrix Widen(const std::array<float, 16> &m) {
    Matrix r{};
    for (unsigned i = 0; i < 16; ++i)
        r[i] = m[i];
    return r;
}
inline Matrix Multiply(const Matrix &a, const Matrix &b) {
    Matrix r{};
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < 4; ++j) {
            double v = 0;
            for (unsigned k = 0; k < 4; ++k)
                v += a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = v;
        }
    return r;
}
// Inverse of a row-vector world-to-view transform, which must be rigid (rotation rows plus
// translation, to float precision). The game's rotation rows are orthonormal only to about
// 1e-4 and its translations are large (~2e4 units), so the exact inverse is used rather than
// the transpose: the transpose moves a wall 78 units away by ~0.1 pixel.
inline bool InvertRigid(const Matrix &m, Matrix &out) {
    for (unsigned i = 0; i < 3; ++i)
        if (m[i * 4 + 3] != 0)
            return false;
    if (m[15] != 1)
        return false;
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j) {
            double dot = 0;
            for (unsigned k = 0; k < 3; ++k)
                dot += m[k * 4 + i] * m[k * 4 + j];
            if (!std::isfinite(dot) || std::abs(dot - (i == j ? 1.0 : 0.0)) > 1e-3)
                return false;
        }
    // Gauss-Jordan with partial pivoting on [m | I].
    Matrix a = m;
    out = {};
    for (unsigned i = 0; i < 4; ++i)
        out[i * 5] = 1;
    for (unsigned col = 0; col < 4; ++col) {
        unsigned pivot = col;
        for (unsigned r = col + 1; r < 4; ++r)
            if (std::abs(a[r * 4 + col]) > std::abs(a[pivot * 4 + col]))
                pivot = r;
        if (!(std::abs(a[pivot * 4 + col]) > 1e-12))
            return false;
        for (unsigned k = 0; k < 4; ++k) {
            std::swap(a[col * 4 + k], a[pivot * 4 + k]);
            std::swap(out[col * 4 + k], out[pivot * 4 + k]);
        }
        const double inv = 1 / a[col * 4 + col];
        for (unsigned k = 0; k < 4; ++k) {
            a[col * 4 + k] *= inv;
            out[col * 4 + k] *= inv;
        }
        for (unsigned r = 0; r < 4; ++r)
            if (r != col) {
                const double f = a[r * 4 + col];
                for (unsigned k = 0; k < 4; ++k) {
                    a[r * 4 + k] -= f * a[col * 4 + k];
                    out[r * 4 + k] -= f * out[col * 4 + k];
                }
            }
    }
    return true;
}
// The projection must map view (x, y) to clip without shear, give clip w = view z, and map
// depth as rrweapondepth::Map accepts (infinite reverse-Z, row-vector).
inline bool Projectable(const std::array<float, 16> &p) {
    for (unsigned i : {1u, 3u, 4u, 7u, 12u, 13u, 15u})
        if (std::abs(p[i]) > 1e-6f)
            return false;
    rrweapondepth::Mapping m;
    return p[11] == 1 && std::isfinite(p[0]) && std::isfinite(p[5]) && std::abs(p[0]) > 1e-6f &&
           std::abs(p[5]) > 1e-6f && std::isfinite(p[8]) && std::isfinite(p[9]) &&
           rrweapondepth::Detect(p) == rrweapondepth::Layout::RowVector &&
           rrweapondepth::Map(p, m);
}
inline bool Make(const Camera &current, const Camera &previous, uint32_t width, uint32_t height,
                 unsigned age, bool apply, Constants &c) {
    c = {};
    c.width = width;
    c.height = height;
    c.age = age;
    c.flags = apply ? ApplyFlag : 0u;
    c.depth_tolerance = DepthTolerance;
    c.normal_cos = NormalCos;
    if (!width || !height || !current.valid || !previous.valid ||
        !Projectable(current.projection) || !Projectable(previous.projection))
        return false;
    Matrix inverse{};
    if (!InvertRigid(Widen(current.view), inverse))
        return false;
    const auto m =
        Multiply(Multiply(inverse, Widen(previous.view)), Widen(previous.projection));
    for (unsigned i = 0; i < 16; ++i) {
        c.reproject[i] = float(m[i]);
        if (!std::isfinite(c.reproject[i]))
            return false;
    }
    const auto &p = current.projection;
    c.ax = 1 / p[0];
    c.bx = -p[8] / p[0];
    c.ay = 1 / p[5];
    c.by = -p[9] / p[5];
    rrweapondepth::Mapping a, b;
    rrweapondepth::Map(current.projection, a);
    rrweapondepth::Map(previous.projection, b);
    c.current_scale = a.scale;
    c.current_offset = a.offset;
    c.previous_scale = b.scale;
    c.previous_offset = b.offset;
    return true;
}

// ---- Per-pixel decision, CPU mirror of CoverageSelect in shaders/rr_sparse_scale_vrrt.hlsl ----
enum class Pixel : unsigned {
    Covered,     // record valid here: the Subsurface Scattering pass drew this surface
    NotCovered,  // record valid here: it did not
    Sky,         // no current surface: not a Subsurface Scattering pixel
    OffScreen,   // reprojects outside the record or behind its camera
    Disoccluded, // the stored surface's depth does not match
    Normal,      // the stored surface's normal does not match
    Unknown,     // the record has no coverage there (sample not resolved)
    Count
};
inline bool Accepted(Pixel p) {
    return p == Pixel::Covered || p == Pixel::NotCovered || p == Pixel::Sky;
}
struct Position {
    float x{}, y{}, z{}; // record pixel coordinates (continuous, centers at +0.5) and view z
    bool ok{};
};
// Where pixel (x, y) with current surface s was in the record's frame.
inline Position Reproject(const Constants &c, uint32_t x, uint32_t y, const Surface &s) {
    Position r;
    if (s.weapon) {
        // The weapon is drawn with its own camera that follows the view: same pixel.
        r = {float(x) + 0.5f, float(y) + 0.5f, s.depth, true};
        return r;
    }
    const float z = c.current_scale / (s.depth + c.current_offset);
    const float nx = (float(x) + 0.5f) / float(c.width) * 2 - 1;
    const float ny = 1 - (float(y) + 0.5f) / float(c.height) * 2;
    const float v[3] = {z * (nx * c.ax + c.bx), z * (ny * c.ay + c.by), z};
    float clip[4];
    for (unsigned j = 0; j < 4; ++j)
        clip[j] = v[0] * c.reproject[j] + v[1] * c.reproject[4 + j] + v[2] * c.reproject[8 + j] +
                  c.reproject[12 + j];
    if (!(clip[3] > 0))
        return r;
    r.x = (clip[0] / clip[3] * 0.5f + 0.5f) * float(c.width);
    r.y = (0.5f - clip[1] / clip[3] * 0.5f) * float(c.height);
    r.z = clip[3];
    r.ok = std::isfinite(r.x) && std::isfinite(r.y);
    return r;
}
// The stored surface's comparable depth: view z for the world, t for the weapon slice.
inline float StoredDepth(const Constants &c, const Surface &s) {
    return s.weapon ? s.depth : c.previous_scale / (s.depth + c.previous_offset);
}
template <class Keys, class Cover>
Pixel Decide(const Constants &c, uint32_t x, uint32_t y, uint32_t current_key, Keys previous_key,
             Cover previous_coverage) {
    const auto s = Decode(current_key);
    if (!s.present)
        return Pixel::Sky;
    const auto q = Reproject(c, x, y, s);
    if (!q.ok || q.x < 0 || q.y < 0 || q.x >= float(c.width) || q.y >= float(c.height))
        return Pixel::OffScreen;
    // 2x2 footprint around q, inside the record. Coverage is read only from a texel that is
    // the same surface on its own (its depth within tolerance of q's, and its normal), the
    // one nearest q: at a silhouette the footprint holds both surfaces, and the texel under
    // q may be the other one.
    const int bx = int(std::floor(q.x - 0.5f)), by = int(std::floor(q.y - 0.5f));
    bool depth_match = false;
    float nearest = INFINITY;
    int sx = 0, sy = 0;
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
            const int tx = bx + dx, ty = by + dy;
            if (tx < 0 || ty < 0 || tx >= int(c.width) || ty >= int(c.height))
                continue;
            const auto t = Decode(previous_key(uint32_t(tx), uint32_t(ty)));
            if (!t.present || t.weapon != s.weapon)
                continue;
            const float d = StoredDepth(c, t);
            if (!(q.z >= d * (1 - c.depth_tolerance) && q.z <= d * (1 + c.depth_tolerance)))
                continue;
            depth_match = true;
            if (s.normal[0] * t.normal[0] + s.normal[1] * t.normal[1] +
                    s.normal[2] * t.normal[2] <
                c.normal_cos)
                continue;
            const float ex = float(tx) + 0.5f - q.x, ey = float(ty) + 0.5f - q.y;
            const float e = ex * ex + ey * ey;
            if (e < nearest) {
                nearest = e;
                sx = tx;
                sy = ty;
            }
        }
    if (!depth_match)
        return Pixel::Disoccluded;
    if (nearest == INFINITY)
        return Pixel::Normal;
    const uint32_t v = previous_coverage(uint32_t(sx), uint32_t(sy));
    return v == Drawn ? Pixel::Covered : v == NotDrawn ? Pixel::NotCovered : Pixel::Unknown;
}
// Whether the SSS-masked source takes native temporal lobe 2 at a pixel. The baseline takes
// it wherever the earlier guide is nonzero; the experiment where the record says Covered.
inline bool Native(const Constants &c, float baseline_guide, Pixel decision) {
    return (c.flags & ApplyFlag) ? decision == Pixel::Covered : !(baseline_guide == 0);
}

// ---- Counters (u2 of the lighting variant, 16 words, zeroed by the key pass) ----
enum Counter : unsigned {
    Pixels,         // SSS-masked lighting pixels evaluated
    BaselineNative, // the baseline would take native history
    CoverageNative, // the coverage record would
    Differ,         // the two disagree
    Applied,        // native history taken (the active selection)
    CountSky,
    CountOffScreen,
    CountDisoccluded,
    CountNormal,
    CountUnknown,
    CountWeapon, // pixels on the first-person weapon slice
    Counters = 16
};
static_assert(unsigned(CountWeapon) < Counters);
inline constexpr unsigned CounterBytes = Counters * 4;
struct Totals {
    uint64_t frames{}, value[Counters]{};
    unsigned age_sum{};
    void Add(const uint32_t *v, unsigned age) {
        ++frames;
        age_sum += age;
        for (unsigned i = 0; i < Counters; ++i)
            value[i] += v[i];
    }
};
} // namespace rrssscov
