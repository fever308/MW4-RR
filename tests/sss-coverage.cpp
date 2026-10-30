#include "../src/rr_sss_coverage.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <vector>

// CPU checks of the Subsurface Scattering coverage experiment (src/rr_sss_coverage.h): the
// key encoding, record age and invalidation, the reprojection's direction, scale and age, and
// the per-pixel decision the lighting shader mirrors. No GPU is used; these do not prove that
// the HLSL in shaders/rr_sparse_scale_vrrt.hlsl matches this mirror, nor anything visual.
namespace {
using namespace rrssscov;
bool Near(double a, double b, double tolerance) {
    return std::fabs(a - b) <= tolerance;
}

void HalfFloat() {
    assert(Half(1.0f) == 0x3c00 && Half(0.5f) == 0x3800 && Half(65504.0f) == 0x7bff);
    assert(Half(0.0f) == 0 && Half(0.1f) == 0x2e66);
    assert(Half(1e9f) == 0x7c00);                               // overflow to infinity
    assert(Half(std::ldexp(1.0f, -24)) == 1);                   // smallest subnormal
    assert(Half(std::ldexp(1.0f, -26)) == 0);                   // below half of it
    assert(Half(std::ldexp(1.5f, -24)) == 2);                   // tie to even
    assert(Half(std::ldexp(1.0f, -10) * (1 + 1.0f / 2048)) == 0x1400); // tie to even (down)
    for (uint32_t h = 1; h < 0x7c00; ++h)
        assert(Half(Float(uint16_t(h))) == h);
    assert(Float(0x3c00) == 1.0f && Float(0x0001) == std::ldexp(1.0f, -24));
}

void Keys() {
    const uint32_t normal = 0x12345678u;
    // Sky, unusable and out-of-range depth: no surface; the normal bits are kept.
    for (float d : {0.0f, -0.0f, -0.5f, 1.5f, std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::quiet_NaN(),
                    std::numeric_limits<float>::infinity()}) {
        const auto k = Key(d, normal);
        assert((k >> 16) == 0 && (k & 0xffffu) == 0x5678u && !Decode(k).present);
    }
    auto k = Key(0.5f, normal);
    assert((k >> 16) == 0x3800 && Decode(k).present && !Decode(k).weapon &&
           Decode(k).depth == 0.5f);
    // Far world depth keeps its relative precision (float16 near/z).
    for (float d : {1e-2f, 1e-3f, 1e-4f, 3e-5f}) {
        const auto s = Decode(Key(d, 0));
        assert(s.present && Near(s.depth, d, d * 1.0 / 1024));
    }
    // The weapon slice stores the weapon camera's own depth t, with bit 15.
    const float t = 0.25f, weapon = rrweapondepth::Slice + (1 - rrweapondepth::Slice) * t;
    k = Key(weapon, 0);
    assert((k >> 16) == (WeaponBit | Half(0.25f)));
    assert(Decode(k).weapon && Near(Decode(k).depth, 0.25, 1e-3));
    // Exactly the slice is world depth, as rr_convert_guides.hlsl's alternate test.
    assert(!Decode(Key(rrweapondepth::Slice, 0)).weapon);
    // Normals: 8+8 bit octahedral, unit length.
    float n[3];
    for (uint32_t p : {0x0000u, 0x7f7fu, 0xffffu, 0x80c0u, 0x2040u}) {
        DecodeNormal(p, n);
        assert(Near(n[0] * n[0] + n[1] * n[1] + n[2] * n[2], 1, 1e-5));
    }
    DecodeNormal(0x8080u, n); // ~(0, 0, 1)
    assert(n[2] > 0.99f);
}

Camera Still(unsigned frame) {
    Camera c;
    c.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    // Infinite reverse-Z, row-vector, near 1: hardware depth = 1 / view z.
    c.projection = {1, 0, 0, 0, 0, 1.33333337f, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0};
    c.frame = frame;
    c.valid = true;
    return c;
}
// The world-to-view of a camera at world position (x, y, z) with identity axes.
Camera At(unsigned frame, float x, float y, float z) {
    auto c = Still(frame);
    c.view[12] = -x;
    c.view[13] = -y;
    c.view[14] = -z;
    return c;
}

void Records() {
    Record r{true, 100, 7, 64, 48, Still(100)};
    assert(Check(r, 101, 7, 64, 48, 0, false) == FrameReason::Ready);
    assert(Check(r, 103, 7, 64, 48, 0, false) == FrameReason::Ready); // MaxAge
    assert(Check(r, 104, 7, 64, 48, 0, false) == FrameReason::Age);
    assert(Check(r, 100, 7, 64, 48, 0, false) == FrameReason::Age); // same frame
    assert(Check(r, 99, 7, 64, 48, 0, false) == FrameReason::Age);  // from the future (wraps)
    assert(Check(r, 101, 8, 64, 48, 0, false) == FrameReason::Generation);
    assert(Check(r, 101, 7, 66, 48, 0, false) == FrameReason::Extent); // resize
    assert(Check(r, 101, 7, 64, 50, 0, false) == FrameReason::Extent);
    assert(Check(r, 101, 7, 64, 48, 1, false) == FrameReason::Viewport);
    assert(Check(r, 101, 7, 64, 48, 0, true) == FrameReason::Reset);
    Record none;
    assert(Check(none, 101, 7, 64, 48, 0, false) == FrameReason::NoRecord);
    auto bad = r;
    bad.camera.valid = false;
    assert(Check(bad, 101, 7, 64, 48, 0, false) == FrameReason::Camera);
    // Frame counter wrap: a record at UINT_MAX is one frame old at 0.
    Record wrap{true, 0xffffffffu, 7, 64, 48, Still(0xffffffffu)};
    assert(Check(wrap, 0, 7, 64, 48, 0, false) == FrameReason::Ready);
    // A record never shares its entry with a frame up to MaxAge newer.
    for (unsigned f = 0; f < 40; ++f)
        for (unsigned a = 1; a <= MaxAge; ++a)
            assert(Entry(f) != Entry(f + a));
    for (unsigned i = 0; i < unsigned(FrameReason::Count); ++i)
        assert(Label(FrameReason(i)) && *Label(FrameReason(i)));
}

void Cameras() {
    Constants c{};
    assert(Make(Still(2), Still(1), 64, 48, 1, false, c));
    assert(c.flags == 0 && c.age == 1 && c.width == 64 && c.height == 48);
    assert(Make(Still(2), Still(1), 64, 48, 1, true, c) && c.flags == ApplyFlag);
    // Projection or view that cannot be reprojected.
    auto shear = Still(1);
    shear.projection[4] = 0.5f;
    assert(!Make(Still(2), shear, 64, 48, 1, true, c));
    auto ortho = Still(1);
    ortho.projection[11] = 0;
    assert(!Make(ortho, Still(1), 64, 48, 1, true, c));
    auto flipped = Still(1); // clip w = -view z
    flipped.projection[11] = -1;
    assert(!Make(flipped, Still(1), 64, 48, 1, true, c));
    auto scaled = Still(2);
    scaled.view[0] = 2; // not rigid
    assert(!Make(scaled, Still(1), 64, 48, 1, true, c));
    auto invalid = Still(1);
    invalid.valid = false;
    assert(!Make(Still(2), invalid, 64, 48, 1, true, c));
    assert(!Make(Still(2), Still(1), 0, 48, 1, true, c));
}

// Frames 2 and 3 of the strafing capture 20261025-181459, as RR got them (also used by the
// earlier sheet motion test, df13cb6). The game's own motion on a wall at pixel (1800, 1000),
// linear depth 78.83962, is (32.59375, -0.15857) with RR's motion scale -1: previous minus
// current pixel position = -(32.59375, -0.15857). The background at (2000, 300), depth
// 488.70367: (5.30469, -0.08807).
Camera Capture(bool current) {
    Camera c;
    c.view = current ? std::array<float, 16>{-0.6374321579933167f, 0.0304717980325222f, 0.7699041366577148f, 0,
                                            -0.7705068588256836f, -0.025536073371767998f, -0.6369205117225647f, 0,
                                            -0.00025221449322998524f, 0.9992093443870544f, -0.03975621983408928f, 0,
                                            -21089.29296875f, -3674.0791015625f, -16766.29296875f, 1}
                     : std::array<float, 16>{-0.6374349594116211f, 0.03037012182176113f, 0.7699058055877686f, 0,
                                            -0.7705044746398926f, -0.02551218867301941f, -0.6369243264198303f, 0,
                                            -0.00029851324507035315f, 0.9992130398750305f, -0.03966265171766281f, 0,
                                            -21091.3671875f, -3673.493896484375f, -16766.673828125f, 1};
    c.projection = {1, 0, 0, 0, 0, 1.77777779f, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0};
    c.frame = current ? 45255 : 45254;
    c.valid = true;
    return c;
}
void RealMotion() {
    Constants c{};
    assert(Make(Capture(true), Capture(false), 2224, 1248, 1, true, c));
    struct Sample {
        uint32_t x, y;
        float z, mx, my, tolerance;
    } samples[] = {{1800, 1000, 78.83962f, 32.59375f, -0.15857f, 0.6f},
                   {2000, 300, 488.70367f, 5.30469f, -0.08807f, 0.2f}};
    for (const auto &s : samples) {
        // Through the key's float16 hardware depth, as the shader sees it.
        const auto surface = Decode(Key(1 / s.z, 0x8080u));
        const auto q = Reproject(c, s.x, s.y, surface);
        assert(q.ok);
        const double gx = s.x + 0.5 - s.mx, gy = s.y + 0.5 - s.my; // the game's motion
        std::fprintf(stderr, "pixel %u,%u: reprojected to %.3f,%.3f, game motion %.3f,%.3f\n", s.x, s.y,
                    q.x, q.y, gx, gy);
        assert(Near(q.x, gx, s.tolerance) && Near(q.y, gy, 0.05));
        // The earlier offline rebuild of the wall pixel (sheet-depth-motion/reproj.py):
        // 32.08898, -0.15973. The remaining ~0.5 px against the game is the ~1.5% x scale
        // difference that work noted; it is under a pixel at this speed.
        if (s.x == 1800)
            assert(Near(q.x, s.x + 0.5 - 32.08898, 0.03) && Near(q.y, s.y + 0.5 + 0.15973, 0.01));
        // Previous view depth: the point barely moves in depth while strafing.
        assert(Near(q.z, s.z, s.z * 0.01));
    }
}

// A synthetic w x h frame pair: keys and coverage stored per pixel.
struct Scene {
    uint32_t w, h;
    std::vector<uint32_t> current, previous, coverage;
    Scene(uint32_t width, uint32_t height)
        : w(width), h(height), current(size_t(width) * height),
          previous(size_t(width) * height), coverage(size_t(width) * height, Unknown) {}
    Pixel At(const Constants &c, uint32_t x, uint32_t y) const {
        return Decide(
            c, x, y, current[size_t(y) * w + x],
            [&](uint32_t px, uint32_t py) { return previous[size_t(py) * w + px]; },
            [&](uint32_t px, uint32_t py) {
                assert(px < w && py < h);
                return coverage[size_t(py) * w + px];
            });
    }
};
constexpr uint32_t Up = 0x8080u; // normal ~(0, 0, 1)

void StillCamera() {
    // Odd extents: the decision has no parity or tile requirement.
    for (auto [w, h] : {std::pair{64u, 48u}, std::pair{63u, 47u}, std::pair{1u, 1u}}) {
        Scene s(w, h);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                s.current[y * w + x] = s.previous[y * w + x] = Key(0.1f, Up);
                s.coverage[y * w + x] = x < w / 2 ? Drawn : NotDrawn;
            }
        Constants c{};
        assert(Make(Still(5), Still(4), w, h, 1, true, c));
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                assert(s.At(c, x, y) == (x < w / 2 ? Pixel::Covered : Pixel::NotCovered));
    }
}

// Camera strafes right by dx world units per frame over a plane at view depth z = 10.
// A point that is now at pixel x was at x + shift in the record (scene moves left).
void Strafe() {
    const uint32_t w = 64, h = 48;
    const float z = 10, dx = 0.5f; // projection p0 = 1: ndc shift = dx / z per frame
    const float shift = dx / z * w / 2; // 1.6 px per frame
    for (unsigned age = 1; age <= MaxAge; ++age) {
        Scene s(w, h);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                s.current[y * w + x] = s.previous[y * w + x] = Key(1 / z, Up);
                s.coverage[y * w + x] = x >= 20 && x < 40 ? Drawn : NotDrawn;
            }
        Constants c{};
        assert(Make(At(10, dx * 10, 0, 0), At(10 - age, dx * (10 - age), 0, 0), w, h, age, true,
                    c));
        for (uint32_t x = 0; x < w; ++x) {
            const auto q = Reproject(c, x, 10, Decode(s.current[10 * w + x]));
            // 1e-2: the key's float16 depth (0.1 -> 0.09998) shifts the step by ~0.03%.
            assert(q.ok && Near(q.x, x + 0.5 + shift * age, 1e-2) && Near(q.y, 10.5, 1e-3));
            const auto d = s.At(c, x, 10);
            if (q.x >= w) {
                assert(d == Pixel::OffScreen);
                continue;
            }
            const uint32_t from = uint32_t(q.x);
            assert(d == (from >= 20 && from < 40 ? Pixel::Covered : Pixel::NotCovered));
        }
        // The baseline (same pixel) and the reprojection disagree on a band of ~shift*age px
        // at each edge; the reprojection follows the surface.
        assert(s.At(c, uint32_t(std::ceil(20 - shift * age)), 10) == Pixel::Covered);
        assert(s.At(c, 39, 10) == Pixel::NotCovered || age * shift < 1);
    }
    // Strafing the other way moves off the left edge.
    Scene s(w, h);
    for (auto &k : s.current)
        k = Key(0.1f, Up);
    s.previous = s.current;
    for (auto &v : s.coverage)
        v = Drawn;
    Constants c{};
    assert(Make(At(1, -2, 0, 0), At(0, 0, 0, 0), w, h, 1, true, c));
    assert(s.At(c, 0, 5) == Pixel::OffScreen && s.At(c, w - 1, 5) == Pixel::Covered);
}

void Rejections() {
    const uint32_t w = 32, h = 32;
    Constants c{};
    assert(Make(Still(3), Still(2), w, h, 1, true, c));
    Scene s(w, h);
    for (uint32_t i = 0; i < w * h; ++i) {
        s.current[i] = s.previous[i] = Key(0.1f, Up);
        s.coverage[i] = Drawn;
    }
    // An object that moved away: the record holds a nearer surface there (depth 4 vs 10).
    for (uint32_t y = 8; y < 16; ++y)
        for (uint32_t x = 8; x < 16; ++x)
            s.previous[y * w + x] = Key(0.25f, Up);
    assert(s.At(c, 11, 11) == Pixel::Disoccluded);
    // Only the 2x2 footprint is compared, and coverage comes from the texel that matches.
    assert(s.At(c, 7, 11) == Pixel::Covered);
    assert(s.At(c, 5, 11) == Pixel::Covered);
    // A depth change within tolerance is the same surface; beyond it is not.
    s.previous[20 * w + 20] = s.previous[20 * w + 21] = s.previous[21 * w + 20] =
        s.previous[21 * w + 21] = Key(0.1f * 1.02f, Up);
    assert(s.At(c, 20, 20) == Pixel::Covered);
    for (uint32_t y = 19; y <= 22; ++y)
        for (uint32_t x = 19; x <= 22; ++x)
            s.previous[y * w + x] = Key(0.1f * 1.1f, Up);
    assert(s.At(c, 20, 20) == Pixel::Disoccluded);
    // A surface with an opposite normal at the same depth.
    for (uint32_t y = 24; y <= 27; ++y)
        for (uint32_t x = 24; x <= 27; ++x)
            s.previous[y * w + x] = Key(0.1f, 0x0000u);
    assert(s.At(c, 25, 25) == Pixel::Normal);
    // Sky in the record where there is now a surface: disoccluded; sky now: Sky.
    s.previous[2 * w + 2] = s.previous[2 * w + 3] = s.previous[3 * w + 2] =
        s.previous[3 * w + 3] = Key(0, Up);
    assert(s.At(c, 2, 2) == Pixel::Disoccluded);
    s.current[0] = Key(0, Up);
    assert(s.At(c, 0, 0) == Pixel::Sky);
    // Unresolved coverage.
    s.coverage[30 * w + 5] = Unknown;
    assert(s.At(c, 5, 30) == Pixel::Unknown);
    // Behind the record's camera.
    Constants back{};
    auto turned = Still(2);
    for (unsigned i = 0; i < 3; ++i)
        turned.view[i * 4 + 2] = -turned.view[i * 4 + 2]; // view z flipped (and x, keep rigid)
    turned.view[0] = -1;
    assert(Make(Still(3), turned, w, h, 1, true, back));
    assert(s.At(back, 16, 16) == Pixel::OffScreen);
    for (unsigned i = 0; i < unsigned(Pixel::Count); ++i)
        (void)Accepted(Pixel(i));
    assert(Accepted(Pixel::Covered) && Accepted(Pixel::NotCovered) && Accepted(Pixel::Sky));
    assert(!Accepted(Pixel::OffScreen) && !Accepted(Pixel::Disoccluded) &&
           !Accepted(Pixel::Normal) && !Accepted(Pixel::Unknown));
}

// At a silhouette the footprint holds two surfaces: coverage must come from the texel that is
// the current surface, never from the texel under q. The camera strafes over background
// (z 10, not Subsurface Scattering); the record holds a near character (z 2, drawn) from
// x 32. Every current pixel is background.
void Silhouette() {
    const uint32_t w = 64, h = 16;
    const float dx = 0.5f; // 1.6 px per frame at z 10
    Scene s(w, h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            s.current[y * w + x] = Key(0.1f, Up);
            s.previous[y * w + x] = Key(x >= 32 ? 0.5f : 0.1f, Up);
            s.coverage[y * w + x] = x >= 32 ? Drawn : NotDrawn;
        }
    Constants c{};
    assert(Make(At(1, dx, 0, 0), At(0, 0, 0, 0), w, h, 1, true, c));
    const auto q = Reproject(c, 30, 8, Decode(s.current[8 * w + 30]));
    assert(q.ok && uint32_t(q.x) == 32 && uint32_t(std::floor(q.x - 0.5f)) == 31);
    assert(s.At(c, 30, 8) == Pixel::NotCovered); // texel 31, not the character's 32
    for (uint32_t x = 0; x < w; ++x)
        assert(s.At(c, x, 8) != Pixel::Covered);
    // The mirror: the record's character is now the current surface at those pixels, the
    // texel under q is background: never NotCovered from the background.
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            s.previous[y * w + x] = Key(x <= 31 ? 0.1f : 0.5f, Up);
            s.coverage[y * w + x] = x <= 31 ? Drawn : NotDrawn;
            s.current[y * w + x] = Key(0.1f, Up);
        }
    for (uint32_t x = 0; x < w; ++x) {
        const auto d = s.At(c, x, 8);
        const auto r = Reproject(c, x, 8, Decode(s.current[8 * w + x]));
        if (r.ok && r.x < w && uint32_t(r.x) >= 32) // under q is the near surface
            assert(d == Pixel::Covered || d == Pixel::Disoccluded || d == Pixel::OffScreen);
    }
}

// A character moves right 3 px over a still background while the camera does not move. The
// camera-only reprojection maps its new pixels to the same pixels in the record, where the
// background was: they are rejected (disoccluded), never classified from the background.
// The baseline reads the record at the same pixel and would.
void MovingObject() {
    const uint32_t w = 48, h = 32;
    Scene s(w, h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const bool now = x >= 13 && x < 23 && y >= 8 && y < 24;
            const bool before = x >= 10 && x < 20 && y >= 8 && y < 24;
            s.current[y * w + x] = Key(now ? 0.5f : 0.05f, Up);       // character at z 2
            s.previous[y * w + x] = Key(before ? 0.5f : 0.05f, Up);   // background at z 20
            s.coverage[y * w + x] = before ? Drawn : NotDrawn;        // the character is skin
        }
    Constants c{};
    assert(Make(Still(8), Still(7), w, h, 1, true, c));
    for (uint32_t x = 20; x < 23; ++x) // newly covered by the character
        assert(s.At(c, x, 16) == Pixel::Disoccluded);
    for (uint32_t x = 11; x < 13; ++x) // newly uncovered background
        assert(s.At(c, x, 16) == Pixel::Disoccluded);
    for (uint32_t x = 14; x < 19; ++x) // character interior: the same surface, still skin
        assert(s.At(c, x, 16) == Pixel::Covered);
    assert(s.At(c, 30, 16) == Pixel::NotCovered);
}

// The first-person weapon slice keeps its pixel, and never matches world depth.
void Weapon() {
    const uint32_t w = 16, h = 16;
    Scene s(w, h);
    const float t = 0.02f, weapon = rrweapondepth::Slice + (1 - rrweapondepth::Slice) * t;
    for (uint32_t i = 0; i < w * h; ++i) {
        s.current[i] = s.previous[i] = Key(weapon, Up);
        s.coverage[i] = Drawn;
    }
    Constants c{};
    // Even a large camera move leaves the weapon where it is.
    assert(Make(At(1, 5, 0, 0), At(0, 0, 0, 0), w, h, 1, true, c));
    assert(s.At(c, 8, 8) == Pixel::Covered);
    for (uint32_t i = 0; i < w * h; ++i)
        s.previous[i] = Key(0.1f, Up);
    assert(s.At(c, 8, 8) == Pixel::Disoccluded);
    for (uint32_t i = 0; i < w * h; ++i)
        s.previous[i] = Key(rrweapondepth::Slice + (1 - rrweapondepth::Slice) * t * 2, Up);
    assert(s.At(c, 8, 8) == Pixel::Disoccluded); // the weapon moved in depth
}

void Selections() {
    Constants baseline{}, applied{};
    assert(Make(Still(1), Still(0), 4, 4, 1, false, baseline));
    assert(Make(Still(1), Still(0), 4, 4, 1, true, applied));
    // The baseline is exactly the shader's old test: native unless the guide is (+/-)0.
    assert(!Native(baseline, 0.0f, Pixel::Covered) && !Native(baseline, -0.0f, Pixel::Covered));
    assert(Native(baseline, 1e-8f, Pixel::NotCovered) && Native(baseline, -2.0f, Pixel::Sky));
    assert(Native(baseline, std::numeric_limits<float>::quiet_NaN(), Pixel::Unknown));
    // Applied: only where the record says covered, whatever the guide.
    assert(Native(applied, 0.0f, Pixel::Covered));
    for (auto p : {Pixel::NotCovered, Pixel::Sky, Pixel::OffScreen, Pixel::Disoccluded,
                   Pixel::Normal, Pixel::Unknown})
        assert(!Native(applied, 1.0f, p));
    Totals t;
    uint32_t v[Counters]{};
    v[Pixels] = 100;
    v[Differ] = 7;
    t.Add(v, 1);
    t.Add(v, 3);
    assert(t.frames == 2 && t.value[Pixels] == 200 && t.value[Differ] == 14 && t.age_sum == 4);
    static_assert(sizeof(Constants) == 32 * 4 && CounterBytes == 64);
    // Session state starts at the baseline, with no comparison.
    assert(selection.load() == unsigned(Selection::Baseline) && !compare.load() && !Wanted());
    compare = true;
    assert(Wanted());
    compare = false;
}
} // namespace

int main() {
    HalfFloat();
    Keys();
    Records();
    Cameras();
    RealMotion();
    StillCamera();
    Strafe();
    Rejections();
    MovingObject();
    Silhouette();
    Weapon();
    Selections();
    std::puts("sss-coverage: OK");
    return 0;
}
