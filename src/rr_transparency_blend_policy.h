#pragma once
#include <d3d12.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include "rr_status.h"
#include "rr_transparency_policy.h"
// Pure policy for the transparency albedo blend (NVIDIA RR guide 3.4.1): the
// captured composites, the owned coverage target and blend-pass control data,
// and a CPU mirror of the blend shader. No live D3D12, ReShade or NGX calls.
namespace rrblend {
// Composites identified by the FNV-1a64 hash and byte size of their pixel shader.
enum class Kind : uint8_t { Oit, Refraction };
struct Target {
    uint64_t hash;
    size_t bytes;
    Kind kind;
};
// 5a24 is very likely another build's b723 (Oct 18 lineage capture, event 604);
// its blend state was never observed, so Classify guards it at pipeline creation.
inline constexpr Target Targets[] = {{0xa36d02f6245a4129ull, 11856, Kind::Oit},
                                     {0xb72393e05df90a78ull, 4812, Kind::Refraction},
                                     {0x5a24807dc07ce595ull, 4780, Kind::Refraction}};
inline const Target *FindTarget(uint64_t hash, size_t bytes) {
    for (const auto &target : Targets)
        if (target.hash == hash && target.bytes == bytes)
            return &target;
    return nullptr;
}

struct Tuning {
    float diffuse = 0.5f;
    float specular = 0.f;
    float roughness = -1.f; // < 0: roughness is left unchanged
    float strength = 1.f;
    bool sky = true;
    bool filter = true;
    bool sss = true; // fade the SSS guide with the same coverage
};
inline float ClampUnit(float value, float fallback) {
    return std::isnan(value) ? fallback : std::clamp(value, 0.f, 1.f);
}
inline Tuning Clamp(Tuning t) {
    const Tuning defaults;
    t.diffuse = ClampUnit(t.diffuse, defaults.diffuse);
    t.specular = ClampUnit(t.specular, defaults.specular);
    t.strength = ClampUnit(t.strength, defaults.strength);
    t.roughness = t.roughness >= 0.f ? (std::min)(t.roughness, 1.f) : -1.f; // NaN: off
    return t;
}

inline constexpr uint32_t SkyFlag = 1u, RoughnessFlag = 2u, FilterFlag = 4u, SssFlag = 8u;
// The least magnitude a faded SSS guide keeps: the smallest normal half float.
// The next frame's SSS-masked lighting reads a nonzero guide as an SSS pixel.
inline constexpr float SssFloor = 6.103515625e-05f;
// Opacity at or below two 10-bit steps is quantization noise of the coverage target.
inline constexpr float OpacityFloor = 2.f / 1023.f;
// Mirrors the blend shader's cbuffer. Word 0 is written at submission time.
struct Control {
    uint32_t admitted, width, height, flags;
    float diffuse[4]; // rgb diffuse target, w unused (0)
    float params[4];  // x specular target, y roughness target, z strength, w floor
    uint32_t reserved[4];
    uint32_t zero[4]; // always 0: source for clearing the stats buffer
    uint32_t padding[44];
};
static_assert(sizeof(Control) == 256);
static_assert(offsetof(Control, admitted) == 0);
static_assert(offsetof(Control, diffuse) == 16);
static_assert(offsetof(Control, params) == 32);
static_assert(offsetof(Control, zero) == 64);
inline Control Make(const Tuning &tuning, uint32_t width, uint32_t height) {
    const auto t = Clamp(tuning);
    Control c{};
    c.width = width;
    c.height = height;
    c.flags = (t.sky ? SkyFlag : 0u) | (t.roughness >= 0.f ? RoughnessFlag : 0u) |
              (t.filter ? FilterFlag : 0u) | (t.sss ? SssFlag : 0u);
    c.diffuse[0] = c.diffuse[1] = c.diffuse[2] = t.diffuse;
    c.params[0] = t.specular;
    c.params[1] = t.roughness >= 0.f ? t.roughness : 0.f;
    c.params[2] = t.strength;
    c.params[3] = OpacityFloor;
    return c;
}

// Indices come from rrsss::RecordingIndex: -1 absent, -2 duplicate. Equal
// indices mean the same command list, recorded in that order.
inline bool Admit(bool baseline, int pre, int capture, int consumer, bool capture_submitted) {
    return baseline && pre >= 0 && capture >= pre && consumer >= capture && !capture_submitted;
}

// Owned transmittance target: the scene target's extent and sample layout.
inline D3D12_RESOURCE_DESC CoverageDesc(const D3D12_RESOURCE_DESC &scene) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Alignment = 0;
    d.Width = scene.Width;
    d.Height = scene.Height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    d.SampleDesc = scene.SampleDesc;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return d;
}
// Each coverage texel holds 2x2 render pixels as its four samples.
inline bool Extent(const D3D12_RESOURCE_DESC &coverage, uint32_t render_w, uint32_t render_h) {
    return rrtrans::Extent(coverage.Width, coverage.Height, render_w, render_h) &&
           render_w <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
           render_h <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION && coverage.SampleDesc.Count == 4 &&
           (coverage.SampleDesc.Quality == 0 ||
            coverage.SampleDesc.Quality == D3D12_STANDARD_MULTISAMPLE_PATTERN);
}

// The variant keeps only the native dest factor. The composite is then
// dest' = src * g + dest * f(src), and dest * f(src) is its transmittance only
// when f reads the source alone and the source term g never reads dest.
enum class BlendClass { Accept, NoBlend, Factor, Op, Logic, Mask, Targets };
// Dest factors computed from the source alone. The UNORM variant target
// saturates every shader output before blending (o0 and, with dual source,
// o1), so each of these is clamped to [0, 1] in the variant even where the
// native float-target blend sees larger values. a36d (1 - T) and b723
// (saturated alpha) stay exact while T is in [0, 1].
inline bool SourceFactor(D3D12_BLEND factor) {
    switch (factor) {
    case D3D12_BLEND_SRC1_COLOR:
    case D3D12_BLEND_INV_SRC1_COLOR:
    case D3D12_BLEND_SRC_COLOR:
    case D3D12_BLEND_INV_SRC_COLOR:
    case D3D12_BLEND_SRC_ALPHA:
    case D3D12_BLEND_INV_SRC_ALPHA:
    case D3D12_BLEND_SRC1_ALPHA:
    case D3D12_BLEND_INV_SRC1_ALPHA:
        return true;
    default:
        return false;
    }
}
// Source factors known never to read dest. Everything else is refused: the
// dest reads (DEST_*, and SRC_ALPHA_SAT = min(As, 1 - Ad)) and undefined values.
inline bool IgnoresDest(D3D12_BLEND factor) {
    switch (factor) {
    case D3D12_BLEND_ZERO:
    case D3D12_BLEND_ONE:
    case D3D12_BLEND_SRC_COLOR:
    case D3D12_BLEND_INV_SRC_COLOR:
    case D3D12_BLEND_SRC_ALPHA:
    case D3D12_BLEND_INV_SRC_ALPHA:
    case D3D12_BLEND_BLEND_FACTOR:
    case D3D12_BLEND_INV_BLEND_FACTOR:
    case D3D12_BLEND_SRC1_COLOR:
    case D3D12_BLEND_INV_SRC1_COLOR:
    case D3D12_BLEND_SRC1_ALPHA:
    case D3D12_BLEND_INV_SRC1_ALPHA:
    case D3D12_BLEND_ALPHA_FACTOR:
    case D3D12_BLEND_INV_ALPHA_FACTOR:
        return true;
    default:
        return false;
    }
}
inline BlendClass Classify(const D3D12_BLEND_DESC &blend, UINT num_render_targets) {
    const auto &rt = blend.RenderTarget[0];
    if (num_render_targets != 1)
        return BlendClass::Targets;
    if (rt.LogicOpEnable)
        return BlendClass::Logic;
    if (!rt.BlendEnable)
        return BlendClass::NoBlend;
    if (rt.BlendOp != D3D12_BLEND_OP_ADD)
        return BlendClass::Op;
    // Alpha to coverage changes which samples are written.
    if (blend.AlphaToCoverageEnable || !(rt.RenderTargetWriteMask & 7))
        return BlendClass::Mask;
    return SourceFactor(rt.DestBlend) && IgnoresDest(rt.SrcBlend) ? BlendClass::Accept
                                                                  : BlendClass::Factor;
}
// The refusal detail logged with Reason::NoPipeline.
inline const char *Label(BlendClass blend) {
    switch (blend) {
    case BlendClass::Accept:
        return "native-blend-accepted";
    case BlendClass::NoBlend:
        return "native-blend-disabled";
    case BlendClass::Factor:
        return "native-blend-factors-not-transmittance";
    case BlendClass::Op:
        return "native-blend-op-not-add";
    case BlendClass::Logic:
        return "native-logic-op-enabled";
    case BlendClass::Mask:
        return "native-write-mask-or-alpha-to-coverage";
    case BlendClass::Targets:
        return "native-render-target-count";
    default:
        return "native-blend-unknown";
    }
}
// Dest factors of one scalar, the source alpha. They are legal in the alpha
// slots, and every colour channel sees the same transmittance.
inline bool AlphaFactor(D3D12_BLEND factor) {
    switch (factor) {
    case D3D12_BLEND_SRC_ALPHA:
    case D3D12_BLEND_INV_SRC_ALPHA:
    case D3D12_BLEND_SRC1_ALPHA:
    case D3D12_BLEND_INV_SRC1_ALPHA:
        return true;
    default:
        return false;
    }
}
// A refraction variant also carries its transmittance in alpha when the native
// dest factor is alpha-type. A colour-type factor has no single-channel
// transmittance, so alpha is then left alone, as for the OIT resolve.
inline bool RefractionAlpha(const D3D12_BLEND_DESC &native, Kind kind) {
    return kind == Kind::Refraction && AlphaFactor(native.RenderTarget[0].DestBlend);
}
// Valid only for a native blend that Classify returned Accept for.
// RGB: dest' = dest * f(src). Alpha is untouched, or for a refraction variant
// with an alpha-type f, alpha' = alpha * f(src) (2 bits in R10G10B10A2).
inline D3D12_BLEND_DESC PatchBlend(const D3D12_BLEND_DESC &native, Kind kind) {
    D3D12_BLEND_DESC d{};
    d.AlphaToCoverageEnable = FALSE;
    d.IndependentBlendEnable = FALSE;
    for (auto &rt : d.RenderTarget) {
        rt.BlendEnable = FALSE;
        rt.LogicOpEnable = FALSE;
        rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = 0;
    }
    auto &rt = d.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_ZERO;
    rt.DestBlend = native.RenderTarget[0].DestBlend;
    rt.SrcBlendAlpha = D3D12_BLEND_ZERO;
    rt.DestBlendAlpha = D3D12_BLEND_ONE;
    rt.RenderTargetWriteMask = UINT8(native.RenderTarget[0].RenderTargetWriteMask & 7);
    if (RefractionAlpha(native, kind)) {
        rt.DestBlendAlpha = native.RenderTarget[0].DestBlend;
        rt.RenderTargetWriteMask = UINT8(rt.RenderTargetWriteMask | D3D12_COLOR_WRITE_ENABLE_ALPHA);
    }
    return d;
}

// The replay may test against the mirrored DSV but must never write it.
enum class DepthClass { NoDsv, KeepDsv, Refuse };
// A face never changes stencil when it can write no bit or every op keeps.
inline bool KeepsStencil(const D3D12_DEPTH_STENCILOP_DESC1 &face) {
    return face.StencilWriteMask == 0 || (face.StencilFailOp == D3D12_STENCIL_OP_KEEP &&
                                          face.StencilDepthFailOp == D3D12_STENCIL_OP_KEEP &&
                                          face.StencilPassOp == D3D12_STENCIL_OP_KEEP);
}
// DESC2 is the repo's normalized form (rrsss::NormalizeDepthStencil), with a
// write mask per face as ReShade reports it.
inline DepthClass DepthStencilReplayable(const D3D12_DEPTH_STENCIL_DESC2 &d) {
    // The depth bounds test reads the DSV and discards pixels against bounds
    // set on the command list; refused as in rrsss (Status::DepthBounds).
    if (d.DepthBoundsTestEnable)
        return DepthClass::Refuse;
    if (!d.DepthEnable && !d.StencilEnable)
        return DepthClass::NoDsv;
    // D3D never writes depth while the depth test is disabled.
    const bool depth_kept = !d.DepthEnable || d.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ZERO;
    const bool stencil_kept =
        !d.StencilEnable || (KeepsStencil(d.FrontFace) && KeepsStencil(d.BackFace));
    return depth_kept && stencil_kept ? DepthClass::KeepDsv : DepthClass::Refuse;
}
inline DepthClass DepthStencilReplayable(const D3D12_DEPTH_STENCIL_DESC &d) {
    auto face = [&d](const D3D12_DEPTH_STENCILOP_DESC &f) {
        return D3D12_DEPTH_STENCILOP_DESC1{f.StencilFailOp,   f.StencilDepthFailOp,
                                           f.StencilPassOp,   f.StencilFunc,
                                           d.StencilReadMask, d.StencilWriteMask};
    };
    D3D12_DEPTH_STENCIL_DESC2 n{};
    n.DepthEnable = d.DepthEnable;
    n.DepthWriteMask = d.DepthWriteMask;
    n.DepthFunc = d.DepthFunc;
    n.StencilEnable = d.StencilEnable;
    n.FrontFace = face(d.FrontFace);
    n.BackFace = face(d.BackFace);
    return DepthStencilReplayable(n);
}

// ExecuteIndirect resets every binding its signature sets, so a replay is only
// exact when the signature ends in one draw and sets whole root constants.
enum class SignatureClass { Replayable, PartialConstants, Unsupported, Unknown };
// words(root_parameter) returns the 32-bit value count of a root-constants
// parameter, or -1 when it is not known.
template <class Words>
SignatureClass ClassifySignature(const D3D12_COMMAND_SIGNATURE_DESC &desc, Words &&words) {
    if (!desc.NumArgumentDescs || !desc.pArgumentDescs)
        return SignatureClass::Unsupported;
    const UINT last = desc.NumArgumentDescs - 1;
    for (UINT i = 0; i <= last; ++i) {
        switch (desc.pArgumentDescs[i].Type) {
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
            if (i != last)
                return SignatureClass::Unsupported;
            break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
        case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW:
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
        case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
        case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
            if (i == last)
                return SignatureClass::Unsupported;
            break;
        default:
            return SignatureClass::Unsupported;
        }
    }
    // A known partial write is a definite refusal; report it before unknowns.
    bool unknown = false;
    for (UINT i = 0; i < last; ++i) {
        const auto &a = desc.pArgumentDescs[i];
        if (a.Type != D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT)
            continue;
        if (a.Constant.DestOffsetIn32BitValues != 0)
            return SignatureClass::PartialConstants;
        const int count = words(a.Constant.RootParameterIndex);
        if (count < 0)
            unknown = true;
        else if (a.Constant.Num32BitValuesToSet != UINT(count))
            return SignatureClass::PartialConstants;
    }
    return unknown ? SignatureClass::Unknown : SignatureClass::Replayable;
}

// CPU mirror of shaders/rr_transparency_blend.hlsl for one render pixel.
struct Guides {
    float diffuse[3];
    float specular[3];
    float roughness;
    float sss = 0.f; // the SSS guide, 0 off SSS pixels
};
inline float Saturate(float v) {
    return v > 0.f ? (v < 1.f ? v : 1.f) : 0.f; // NaN -> 0, as HLSL saturate
}
inline float Lerp(float x, float y, float s) {
    return x + s * (y - x); // HLSL lerp
}
inline float Luma(const float (&c)[3]) {
    return c[0] * 0.299f + c[1] * 0.587f + c[2] * 0.114f;
}
// transmittance[dy + 1][dx + 1] is t(q) for q = (x + dx, y + dy), the coverage
// sample that maps to render pixel q. Taps outside width x height are ignored;
// each tap's opacity is saturated, so NaN counts as clear.
inline void Reference(const Control &control, uint32_t x, uint32_t y, uint32_t status,
                      const float (&transmittance)[3][3][3], Guides &inout) {
    if (control.admitted != 1 || x >= control.width || y >= control.height)
        return;
    const bool eligible = status == 1 || status == 3 || (status == 2 && (control.flags & SkyFlag));
    if (!eligible)
        return;
    float o[3]{};
    if (control.flags & FilterFlag) {
        float total = 0.f;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int64_t qx = int64_t(x) + dx, qy = int64_t(y) + dy;
                if (qx < 0 || qy < 0 || qx >= int64_t(control.width) ||
                    qy >= int64_t(control.height))
                    continue;
                const float w = float((dx ? 1 : 2) * (dy ? 1 : 2)); // 1-2-1 x 1-2-1
                for (int ch = 0; ch < 3; ++ch)
                    o[ch] += w * Saturate(1.f - transmittance[dy + 1][dx + 1][ch]);
                total += w;
            }
        for (auto &value : o)
            value /= total;
    } else {
        for (int ch = 0; ch < 3; ++ch)
            o[ch] = Saturate(1.f - transmittance[1][1][ch]);
    }
    const float noise = control.params[3], strength = control.params[2];
    float c[3];
    for (int ch = 0; ch < 3; ++ch)
        c[ch] = Saturate(Saturate((o[ch] - noise) / (1.f - noise)) * strength);
    if ((std::max)(c[0], (std::max)(c[1], c[2])) > 0.f) {
        for (int ch = 0; ch < 3; ++ch) {
            inout.diffuse[ch] = Lerp(inout.diffuse[ch], control.diffuse[ch], c[ch]);
            inout.specular[ch] = Lerp(inout.specular[ch], control.params[0], c[ch]);
        }
        if (control.flags & RoughnessFlag)
            inout.roughness = Lerp(inout.roughness, control.params[1], Luma(c));
        // The colour shows the background's SSS only through the layer's
        // transmittance, about 1 - c: fade the guide by c, weighted as NVIDIA's
        // SSS guide luminance (r + 2g + b) / 4.
        if ((control.flags & SssFlag) && inout.sss != 0.f) {
            const float w = 0.25f * c[0] + 0.5f * c[1] + 0.25f * c[2];
            const float g = std::fabs(inout.sss);
            const float faded = (std::max)(g * (1.f - w), (std::min)(g, SssFloor));
            inout.sss = inout.sss < 0.f ? -faded : faded;
        }
    }
}

enum class Reason {
    None,
    Disabled,
    NoSlot,
    NoPipeline,
    Signature,
    Extent,
    Memory,
    Order,
    Split,
    Typed,
    Failed
};
inline const char *Label(Reason reason) {
    switch (reason) {
    case Reason::Disabled:
        return "disabled";
    case Reason::NoSlot:
        return "no-slot";
    case Reason::NoPipeline:
        return "no-pipeline";
    case Reason::Signature:
        return "signature";
    case Reason::Extent:
        return "extent";
    case Reason::Memory:
        return "memory";
    case Reason::Order:
        return "order";
    case Reason::Split:
        return "split";
    case Reason::Typed:
        return "typed-load";
    case Reason::Failed:
        return "failed";
    default:
        return "none";
    }
}
// Log window totals. pixels is the render pixel count of the blend passes that
// ran (width * height each); it is the denominator of eligible%.
struct Counters {
    uint64_t frames{}, captures_oit{}, captures_refr{}, admitted{}, ran{};
    uint64_t pixels{}, eligible{}, covered{}, opacity64{};
    Reason last{Reason::None};
};
// seconds: the span the counters cover.
inline int Report(const Counters &k, char *buf, size_t n, unsigned long long seconds = 10) {
    auto ratio = [](double num, uint64_t den) { return den ? num / double(den) : 0.0; };
    return snprintf(buf, n,
                    "Transparency albedo blend (%llus): frames=%llu captures=%llu oit=%llu "
                    "refr=%llu admitted=%llu eligible=%.1f%% covered=%.1f%% meanOpacity=%.3f "
                    "lastReject=%s",
                    seconds, static_cast<unsigned long long>(k.frames),
                    static_cast<unsigned long long>(k.captures_oit + k.captures_refr),
                    static_cast<unsigned long long>(k.captures_oit),
                    static_cast<unsigned long long>(k.captures_refr),
                    static_cast<unsigned long long>(k.admitted),
                    100.0 * ratio(double(k.eligible), k.pixels),
                    100.0 * ratio(double(k.covered), k.eligible),
                    ratio(double(k.opacity64) / 64.0, k.covered), Label(k.last));
}

inline rrstatus::State StatusOf(bool enabled, bool failed, bool unavailable, bool rr_on,
                                bool rr_failed, bool rr_retired, bool fresh_ran) {
    using rrstatus::State;
    return !enabled                            ? State::Off
           : !rr_on || rr_failed || rr_retired ? State::ParentOff
           : failed                            ? State::Error
           : fresh_ran                         ? State::Active
           : unavailable                       ? State::Unavailable
                                               : State::Waiting;
}
} // namespace rrblend
