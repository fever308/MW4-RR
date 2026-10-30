// Edge-aware no-history fill (src/rr_edge_fill.h): the weights and outcomes the fill
// shader (shaders/rr_sparse_scale_vrrt.hlsl, RR_FILL_EDGE) evaluates, on synthetic frames.
// CPU only: it runs the shared header, not the compiled shader, and proves nothing about
// GPU timing, the game's inputs or the visual result.
#ifdef _WIN32
#include <d3d12.h>
#include "../src/rr_sparse_copy.h"
#endif
#include "../src/rr_edge_fill.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

using namespace rredgefill;

static int failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                               \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static bool Near(float a, float b, float tolerance = 1e-5f) {
    return std::fabs(a - b) <= tolerance * (1.0f + std::fabs(b));
}

// A synthetic frame at the lighting extent.
struct Frame {
    unsigned w, h;
    std::vector<EdgeSurface> surface;
    std::vector<float4> light;    // unboosted lobe 2
    std::vector<unsigned> weights; // packed native weights word; lobe-2 count in bits 8-19
    Frame(unsigned w, unsigned h) : w(w), h(h), surface(w * h), light(w * h), weights(w * h) {}
    EdgeSurface &S(unsigned x, unsigned y) { return surface[y * w + x]; }
    void Fresh(unsigned x, unsigned y, float4 v) {
        light[y * w + x] = v;
        weights[y * w + x] = 1u << 8;
    }
    void Unsampled(unsigned x, unsigned y) {
        light[y * w + x] = {};
        weights[y * w + x] = 0;
    }
};
static EdgeSurface Surface(float depth, float nx, float ny, float nz, unsigned word = 0x10,
                           unsigned status = 1) {
    return {depth, word, nx, ny, nz, status};
}
struct Result {
    unsigned outcome;
    float4 value;
};
// The shader's EdgeFill on the CPU: same loop, bounds, freshness and weights.
static Result Fill(const Frame &f, unsigned px, unsigned py) {
    const EdgeSurface c = f.surface[py * f.w + px];
    if (!EdgeClassified(c))
        return {RR_EDGE_OUTCOME_UNCLASSIFIED, {}};
    float4 sum{};
    float total = 0;
    for (int dy = -RR_EDGE_RADIUS; dy <= RR_EDGE_RADIUS; ++dy)
        for (int dx = -RR_EDGE_RADIUS; dx <= RR_EDGE_RADIUS; ++dx) {
            const int qx = int(px) + dx, qy = int(py) + dy;
            if ((dx == 0 && dy == 0) || !EdgeInside(qx, qy, f.w, f.h))
                continue;
            const unsigned q = unsigned(qy) * f.w + unsigned(qx);
            if (!EdgeFresh(f.weights[q], f.light[q]))
                continue;
            EdgeAccumulate(sum, total, f.light[q], EdgeWeight(c, f.surface[q], dx, dy));
        }
    float4 mean{};
    if (!EdgeResolve(sum, total, mean))
        return {RR_EDGE_OUTCOME_NO_NEIGHBOUR, {}};
    return {RR_EDGE_OUTCOME_EDGE, mean};
}
// The baseline GroupMean: equal weights, only inside the pixel's 16x16 group.
static float4 Baseline(const Frame &f, unsigned px, unsigned py, unsigned &n) {
    float4 sum{};
    n = 0;
    const int gx = int(px % 16), gy = int(py % 16);
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            const int tx = gx + dx, ty = gy + dy;
            if (tx < 0 || ty < 0 || tx > 15 || ty > 15)
                continue;
            const unsigned q = (py + dy) * f.w + (px + dx);
            if ((py + dy) >= f.h || (px + dx) >= f.w)
                continue;
            if (EdgeFresh(f.weights[q], f.light[q]))
                sum += f.light[q], ++n;
        }
    return n ? sum * (1.0f / n) : float4{};
}

static void Weights() {
    const auto flat = Surface(0.01f, 0, 0, 1);
    // Identical surfaces at any distance in the 5x5: full weight.
    CHECK(EdgeWeight(flat, flat, 1, 0) == 1.0f && EdgeWeight(flat, flat, 2, 2) == 1.0f);
    // The centre never weights itself.
    CHECK(EdgeWeight(flat, flat, 0, 0) == 0.0f);
    // Reverse-Z: depth d = near / z, so d_c / d_n = z_n / z_c. 1% farther at 1 px: half of
    // the 2% per pixel tolerance.
    CHECK(Near(EdgeDepthWeight(0.01f, 0.01f / 1.01f, 1.0f), 0.5f, 1e-3f));
    // The same depth step weighs more at 2 px (a plane's depth changes with distance).
    CHECK(EdgeDepthWeight(0.01f, 0.01f / 1.01f, 2.0f) >
          EdgeDepthWeight(0.01f, 0.01f / 1.01f, 1.0f));
    // A 10% step at 1 px, nearer or farther: a different surface.
    CHECK(EdgeDepthWeight(0.01f, 0.01f / 1.1f, 1.0f) == 0.0f &&
          EdgeDepthWeight(0.01f, 0.01f * 1.1f, 1.0f) == 0.0f);
    // The weight is a ratio, independent of the depth's scale (near plane / distance).
    CHECK(Near(EdgeDepthWeight(0.5f, 0.5f / 1.01f, 1.0f),
               EdgeDepthWeight(0.001f, 0.001f / 1.01f, 1.0f), 1e-3f));
    // Normals: 0 at or beyond ~25 degrees, 1 when equal, monotonic between.
    const float c20 = std::cos(20.0f * 3.14159265f / 180.0f),
                s20 = std::sin(20.0f * 3.14159265f / 180.0f);
    const float c30 = std::cos(30.0f * 3.14159265f / 180.0f),
                s30 = std::sin(30.0f * 3.14159265f / 180.0f);
    const float w20 = EdgeNormalWeight(flat, Surface(0.01f, s20, 0, c20));
    CHECK(w20 > 0.0f && w20 < 1.0f);
    CHECK(EdgeNormalWeight(flat, Surface(0.01f, s30, 0, c30)) == 0.0f);
    CHECK(EdgeNormalWeight(flat, Surface(0.01f, 0, 0, -1)) == 0.0f);
    // Unnormalized fp16 guide normals are compared by direction.
    CHECK(Near(EdgeNormalWeight(Surface(0.01f, 0, 0, 0.99f), Surface(0.01f, 0, 0, 1.01f)), 1.0f));
    // Every weight lies in [0, 1].
    for (int i = 0; i <= 40; ++i) {
        const float a = float(i) * 3.14159265f / 80.0f;
        const float wt = EdgeWeight(flat, Surface(0.01f * (1 + i * 0.002f), std::sin(a), 0,
                                                  std::cos(a)),
                                    1, 1);
        CHECK(wt >= 0.0f && wt <= 1.0f);
    }
}

static void Classification() {
    CHECK(EdgeClassified(Surface(0.01f, 0, 0, 1)));
    // Status: only 1 (ordinary) and 3 (special) are surfaces.
    CHECK(EdgeClassified(Surface(0.01f, 0, 0, 1, 0x12, 3)));
    for (unsigned status : {0u, 2u, 4u, 5u})
        CHECK(!EdgeClassified(Surface(0.01f, 0, 0, 1, 0x10, status)));
    // Sky (depth 0), beyond the far end, negative and NaN depth.
    CHECK(!EdgeClassified(Surface(0.0f, 0, 0, 1)) && !EdgeClassified(Surface(1.5f, 0, 0, 1)) &&
          !EdgeClassified(Surface(-0.1f, 0, 0, 1)) &&
          !EdgeClassified(Surface(std::nanf(""), 0, 0, 1)));
    // A zero or non-finite normal (rejected conversion pixels write 0).
    CHECK(!EdgeClassified(Surface(0.01f, 0, 0, 0)) &&
          !EdgeClassified(Surface(0.01f, std::nanf(""), 0, 1)) &&
          !EdgeClassified(Surface(0.01f, INFINITY, 0, 1)));
    // The weapon slice: bit 0 must agree with depth > 63/64, as the conversion requires.
    CHECK(EdgeClassified(Surface(0.995f, 0, 0, 1, 0x11)));
    CHECK(!EdgeClassified(Surface(0.995f, 0, 0, 1, 0x10)) &&
          !EdgeClassified(Surface(0.5f, 0, 0, 1, 0x11)));
}

static void Separation() {
    const auto world = Surface(63.0f / 64.0f - 1e-4f, 0, 0, 1, 0x10);
    const auto weapon = Surface(63.0f / 64.0f + 1e-4f, 0, 0, 1, 0x11);
    // World and weapon never mix, even at nearly equal hardware depth.
    CHECK(EdgeWeight(world, weapon, 1, 0) == 0.0f && EdgeWeight(weapon, world, 1, 0) == 0.0f);
    // Another material class word (the native denoiser's same-surface test).
    CHECK(EdgeWeight(Surface(0.01f, 0, 0, 1, 0x10), Surface(0.01f, 0, 0, 1, 0x30), 1, 0) == 0.0f);
    // Inside the weapon slice, depth is compared in the weapon camera's own reverse-Z.
    const float t = 0.01f, slice = 63.0f / 64.0f;
    const auto a = Surface(slice + t / 64.0f, 0, 0, 1, 0x11),
               b = Surface(slice + t / 64.0f / 1.01f, 0, 0, 1, 0x11);
    CHECK(Near(EdgeReverseZ(a), t, 1e-3f));
    CHECK(Near(EdgeWeight(a, b, 1, 0), 0.5f, 0.05f));
    CHECK(EdgeWeight(a, Surface(slice + t / 64.0f / 1.2f, 0, 0, 1, 0x11), 1, 0) == 0.0f);
}

static void Normalization() {
    // Weighted mean of mixed weights, normalized by their total.
    float4 sum{};
    float total = 0;
    EdgeAccumulate(sum, total, {1, 2, 3, 4}, 1.0f);
    EdgeAccumulate(sum, total, {3, 6, 9, 12}, 0.5f);
    EdgeAccumulate(sum, total, {100, 100, 100, 100}, 0.0f); // rejected: no contribution
    float4 mean{};
    CHECK(EdgeResolve(sum, total, mean));
    CHECK(Near(mean.x, (1 + 1.5f) / 1.5f) && Near(mean.y, (2 + 3) / 1.5f) &&
          Near(mean.z, (3 + 4.5f) / 1.5f) && Near(mean.w, (4 + 6) / 1.5f));
    // No accepted sample: no result (the shader writes 0, as the baseline does with no sample), never 0/0.
    float4 none{};
    CHECK(!EdgeResolve({}, 0.0f, none));
    // The result stays inside fp16 range.
    CHECK(EdgeResolve({1e9f, 1, 1, 1}, 1.0f, mean) && mean.x == 65504.0f);
    // Freshness as the baseline: a lobe-2 count and finite nonnegative fp16 values.
    CHECK(EdgeFresh(1u << 8, {0, 0, 0, 0}) && !EdgeFresh(0, {1, 1, 1, 1}));
    CHECK(!EdgeFresh(0xff0000ffu & ~0x000fff00u, {1, 1, 1, 1})); // other lobes' bits only
    CHECK(!EdgeFresh(1u << 8, {-1, 0, 0, 0}) && !EdgeFresh(1u << 8, {0, 0, 0, 70000}) &&
          !EdgeFresh(1u << 8, {std::nanf(""), 0, 0, 0}));
}

// Energy: on one surface with constant light the fill reproduces it exactly, whatever the
// mix of weights, so the fill neither brightens nor darkens a uniform patch.
static void Energy() {
    Frame f(32, 32);
    for (unsigned y = 0; y < 32; ++y)
        for (unsigned x = 0; x < 32; ++x) {
            // A tilted plane: depth and normal vary smoothly.
            const float z = 100.0f + 0.3f * float(x);
            f.S(x, y) = Surface(1.0f / z, 0.05f, 0.0f, 0.998f);
            if ((x * 7 + y * 3) % 5 == 0)
                f.Fresh(x, y, {0.25f, 0.5f, 0.75f, 0.1f});
            else
                f.Unsampled(x, y);
        }
    unsigned filled = 0;
    for (unsigned y = 0; y < 32; ++y)
        for (unsigned x = 0; x < 32; ++x)
            if (!f.weights[y * 32 + x]) {
                const auto r = Fill(f, x, y);
                CHECK(r.outcome == RR_EDGE_OUTCOME_EDGE);
                if (r.outcome == RR_EDGE_OUTCOME_EDGE) {
                    ++filled;
                    CHECK(Near(r.value.x, 0.25f) && Near(r.value.y, 0.5f) &&
                          Near(r.value.z, 0.75f) && Near(r.value.w, 0.1f));
                }
            }
    CHECK(filled > 700);
}

// Two surfaces: a near red wall in front of a far blue one. The fill must never borrow the
// other surface's light; the baseline does at the silhouette.
static void Edges() {
    Frame f(48, 32);
    for (unsigned y = 0; y < 32; ++y)
        for (unsigned x = 0; x < 48; ++x) {
            const bool front = x < 20;
            f.S(x, y) = front ? Surface(1.0f / 10.0f, 0, 0, 1) : Surface(1.0f / 50.0f, 0, 0, 1);
            if ((x + y) % 2 == 0)
                f.Fresh(x, y, front ? float4{1, 0, 0, 1} : float4{0, 0, 1, 1});
            else
                f.Unsampled(x, y);
        }
    unsigned bleeding = 0, baseline_bleeding = 0;
    for (unsigned y = 0; y < 32; ++y)
        for (unsigned x = 0; x < 48; ++x) {
            if (f.weights[y * 48 + x])
                continue;
            const bool front = x < 20;
            const auto r = Fill(f, x, y);
            CHECK(r.outcome == RR_EDGE_OUTCOME_EDGE);
            bleeding += front ? r.value.z != 0.0f : r.value.x != 0.0f;
            unsigned n;
            const auto b = Baseline(f, x, y, n);
            baseline_bleeding += front ? b.z != 0.0f : b.x != 0.0f;
        }
    CHECK(bleeding == 0);
    CHECK(baseline_bleeding > 0); // the fixture does exercise an edge
    // Same depth, a 90 degree crease (a box corner): normals separate them.
    Frame g(8, 8);
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x) {
            g.S(x, y) = x < 4 ? Surface(0.02f, 0, 0, 1) : Surface(0.02f, 1, 0, 0);
            if (x != 3)
                g.Fresh(x, y, x < 4 ? float4{1, 1, 1, 1} : float4{5, 5, 5, 5});
            else
                g.Unsampled(x, y);
        }
    const auto crease = Fill(g, 3, 4);
    CHECK(crease.outcome == RR_EDGE_OUTCOME_EDGE && Near(crease.value.x, 1.0f));
}

// Support is bounded by the lighting extent only: 16x16 group and 32-row strip boundaries
// do not cut it, while frame edges and any odd extent never read outside.
static void Boundaries() {
    Frame f(64, 48);
    for (unsigned y = 0; y < 48; ++y)
        for (unsigned x = 0; x < 64; ++x) {
            f.S(x, y) = Surface(0.01f, 0, 0, 1);
            f.Unsampled(x, y);
        }
    // Group boundary: the only samples are in the next group.
    f.Fresh(16, 5, {2, 2, 2, 2});
    f.Fresh(17, 5, {4, 4, 4, 4});
    unsigned n;
    Baseline(f, 15, 5, n);
    CHECK(n == 0); // the baseline sees nothing (and wrote 0)
    auto r = Fill(f, 15, 5);
    CHECK(r.outcome == RR_EDGE_OUTCOME_EDGE && Near(r.value.x, 3.0f));
    // Strip boundary (rows 31/32) behaves the same.
    f.Fresh(40, 32, {6, 6, 6, 6});
    r = Fill(f, 40, 31);
    CHECK(r.outcome == RR_EDGE_OUTCOME_EDGE && Near(r.value.x, 6.0f));
    // Corners only read inside: an isolated corner pixel has no neighbour.
    r = Fill(f, 0, 0);
    CHECK(r.outcome == RR_EDGE_OUTCOME_NO_NEIGHBOUR);
    // Extents of any size, odd heights and 1-wide frames included.
    for (unsigned w : {1u, 2u, 3u, 5u, 6u, 17u}) {
        for (unsigned h : {1u, 2u, 3u, 7u, 33u}) {
            Frame e(w, h);
            for (unsigned y = 0; y < h; ++y)
                for (unsigned x = 0; x < w; ++x) {
                    e.S(x, y) = Surface(0.01f, 0, 0, 1);
                    if ((x + y) % 2)
                        e.Fresh(x, y, {1, 1, 1, 1});
                    else
                        e.Unsampled(x, y);
                }
            for (unsigned y = 0; y < h; ++y)
                for (unsigned x = 0; x < w; ++x) {
                    const auto v = Fill(e, x, y); // reads only within w x h (vector bounds)
                    CHECK(v.outcome != RR_EDGE_OUTCOME_UNCLASSIFIED);
                    if (v.outcome == RR_EDGE_OUTCOME_EDGE)
                        CHECK(Near(v.value.x, 1.0f));
                }
        }
    }
    CHECK(!EdgeInside(-1, 0, 4, 4) && !EdgeInside(0, -1, 4, 4) && !EdgeInside(4, 0, 4, 4) &&
          !EdgeInside(0, 4, 4, 4) && EdgeInside(3, 3, 4, 4) && !EdgeInside(0, 0, 0, 0));
}

// Padding and other unclassified pixels: never a source, and an unclassified centre gets
// the baseline (its own outcome).
static void Unclassified() {
    Frame f(8, 8);
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x) {
            // Right half: padding outside the active render extent (Status 0).
            f.S(x, y) = Surface(0.01f, 0, 0, 1, 0x10, x < 4 ? 1 : 0);
            if (x >= 4 || x == 2)
                f.Fresh(x, y, x >= 4 ? float4{9, 9, 9, 9} : float4{1, 1, 1, 1});
            else
                f.Unsampled(x, y);
        }
    auto r = Fill(f, 3, 3);
    CHECK(r.outcome == RR_EDGE_OUTCOME_EDGE && Near(r.value.x, 1.0f));
    CHECK(Fill(f, 5, 3).outcome == RR_EDGE_OUTCOME_UNCLASSIFIED);
    // Sky centre.
    f.S(1, 1) = Surface(0.0f, 0, 0, 0, 0, 2);
    CHECK(Fill(f, 1, 1).outcome == RR_EDGE_OUTCOME_UNCLASSIFIED);
    // Classified centre whose only fresh neighbours are another surface: no neighbour.
    Frame g(5, 5);
    for (unsigned y = 0; y < 5; ++y)
        for (unsigned x = 0; x < 5; ++x) {
            g.S(x, y) = Surface(0.05f, 0, 0, 1);
            g.Fresh(x, y, {3, 3, 3, 3});
        }
    g.S(2, 2) = Surface(0.01f, 0, 0, 1);
    g.Unsampled(2, 2);
    CHECK(Fill(g, 2, 2).outcome == RR_EDGE_OUTCOME_NO_NEIGHBOUR);
}

#ifdef _WIN32
// Resize and lifetime: surface inputs are used only at the lighting extent and formats the
// shader reads; a stale input from another size is refused (the baseline then runs).
static D3D12_RESOURCE_DESC Tex(UINT64 w, UINT h, DXGI_FORMAT f) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Format = f;
    return d;
}
static void Shapes() {
    using rrsparsecopy::EdgeShape;
    for (auto [w, h] : {std::pair<UINT64, UINT>{1280, 720}, {1708, 961}, {2, 1}, {3840, 2160}}) {
        const auto light = Tex(w, h, DXGI_FORMAT_R32G32B32A32_UINT);
        const auto s = Tex(w, h, DXGI_FORMAT_R32G32B32A32_UINT),
                   n = Tex(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT),
                   st = Tex(w, h, DXGI_FORMAT_R32_UINT);
        CHECK(EdgeShape(s, n, st, light));
        CHECK(!EdgeShape(Tex(w + 2, h, DXGI_FORMAT_R32G32B32A32_UINT), n, st, light));
        CHECK(!EdgeShape(s, Tex(w, h + 1, DXGI_FORMAT_R16G16B16A16_FLOAT), st, light));
        CHECK(!EdgeShape(s, n, Tex(w + 1, h, DXGI_FORMAT_R32_UINT), light));
        CHECK(!EdgeShape(Tex(w, h, DXGI_FORMAT_R32G32B32A32_FLOAT), n, st, light));
        CHECK(!EdgeShape(s, Tex(w, h, DXGI_FORMAT_R32G32B32A32_FLOAT), st, light));
        CHECK(!EdgeShape(s, n, Tex(w, h, DXGI_FORMAT_R16_FLOAT), light));
        auto array = s;
        array.DepthOrArraySize = 2;
        CHECK(!EdgeShape(array, n, st, light));
    }
    // Every (lobe, variant) pipeline has its own slot.
    static_assert(rrsparsecopy::VariantIndex(rrlighting::Raw, 0) == 0 &&
                  rrsparsecopy::VariantIndex(rrlighting::SssHistory2, 0) == 4 &&
                  rrsparsecopy::VariantIndex(rrlighting::SssHistory2, rrsparsecopy::EdgeBit) == 5 &&
                  rrsparsecopy::VariantIndex(rrlighting::Count - 1, rrsparsecopy::Variants - 1) <
                      rrlighting::Count * rrsparsecopy::Variants);
    static_assert(rrsparsecopy::PrivateDescriptors == 10 &&
                  rrsparsecopy::EdgeTableParameter == 5);
}
#endif

int main() {
    // The experiment starts off: the production default is the baseline group mean.
    CHECK(!rredgefill::enabled.load());
    CHECK(std::string_view(Label(State::Baseline)).find("baseline") != std::string_view::npos);
    Weights();
    Classification();
    Separation();
    Normalization();
    Energy();
    Edges();
    Boundaries();
    Unclassified();
#ifdef _WIN32
    Shapes();
#endif
    if (failures) {
        std::printf("edge-fill: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("edge-fill: passed\n");
    return 0;
}
