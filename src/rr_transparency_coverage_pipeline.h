#pragma once
#include <reshade.hpp>
#include <array>
#include <cstdio>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "feed_fg_identity.h"
#include "rr_sss_pipeline_proof.h"
#include "rr_sss_vertex_proof.h"
#include "rr_transparency_blend_policy.h"
#include "rr_transparency_pixel_proof.h"
// Variant PSOs of the transparency composites (rrblend::Targets). A variant is
// the native PSO (same shaders, root signature, raster, depth-stencil and sample
// state) whose only target is the owned R10G10B10A2_UNORM coverage target and
// whose blend keeps only the native dest factor: replaying the composite with
// it multiplies the target by the composite's transmittance. Built once per
// native PSO, synchronously at init_pipeline, like rrsss::PipelineRegistry.
namespace rrblend {
// The entry of targets whose pixel shader this pipeline has, by FNV-1a64 hash
// and size (the hash of OnPipeline's shader table). hash is written only on a match.
inline const Target *PixelTarget(unsigned n, const reshade::api::pipeline_subobject *objects,
                                 uint64_t &hash, std::span<const Target> targets = Targets) {
    if (!objects || !n || n > 64)
        return nullptr;
    for (unsigned i = 0; i < n; ++i) {
        const auto &o = objects[i];
        if (o.type != reshade::api::pipeline_subobject_type::pixel_shader || o.count != 1 ||
            !o.data)
            continue;
        const auto &s = *static_cast<const reshade::api::shader_desc *>(o.data);
        bool sized = false;
        for (const auto &target : targets)
            sized = sized || target.bytes == s.code_size;
        if (!s.code || !sized)
            continue;
        const auto h = rrsss::PipelineProof::ByteHash(s.code, s.code_size);
        for (const auto &target : targets)
            if (target.hash == h && target.bytes == s.code_size) {
                hash = h;
                return &target;
            }
    }
    return nullptr;
}

// ReShade's numbering of the factors a variant uses: ZERO, ONE and the source factors.
inline bool ReShadeFactor(D3D12_BLEND factor, reshade::api::blend_factor &out) {
    using F = reshade::api::blend_factor;
    switch (factor) {
    case D3D12_BLEND_ZERO:
        out = F::zero;
        return true;
    case D3D12_BLEND_ONE:
        out = F::one;
        return true;
    case D3D12_BLEND_SRC_COLOR:
        out = F::source_color;
        return true;
    case D3D12_BLEND_INV_SRC_COLOR:
        out = F::one_minus_source_color;
        return true;
    case D3D12_BLEND_SRC_ALPHA:
        out = F::source_alpha;
        return true;
    case D3D12_BLEND_INV_SRC_ALPHA:
        out = F::one_minus_source_alpha;
        return true;
    case D3D12_BLEND_SRC1_COLOR:
        out = F::source1_color;
        return true;
    case D3D12_BLEND_INV_SRC1_COLOR:
        out = F::one_minus_source1_color;
        return true;
    case D3D12_BLEND_SRC1_ALPHA:
        out = F::source1_alpha;
        return true;
    case D3D12_BLEND_INV_SRC1_ALPHA:
        out = F::one_minus_source1_alpha;
        return true;
    default:
        return false;
    }
}

// The observation the variant must produce: the native one with only the
// coverage target, the PatchBlend blend, no cached PSO and, without a DSV, no
// DSV format. PipelineProof::Equivalent compares it with the created variant.
// Valid only when Classify accepted native.blend and depth is not Refuse.
inline rrsss::NativePipelineObservation Expected(const rrsss::NativePipelineObservation &native,
                                                 Kind kind, DepthClass depth) {
    auto o = native;
    o.cached_bytes = 0;
    o.cached_present = false;
    // PatchBlend sets IndependentBlendEnable FALSE. ReShade likely creates the
    // variant with TRUE, since slots 1-7 differ from RT0; Equivalent compares
    // only RT0 for one RT, so either matches.
    o.blend = PatchBlend(native.blend, kind);
    o.rt_count = 1;
    o.rt_formats.fill(DXGI_FORMAT_UNKNOWN);
    o.rt_formats[0] = DXGI_FORMAT_R10G10B10A2_UNORM;
    if (depth == DepthClass::NoDsv)
        o.depth.format = DXGI_FORMAT_UNKNOWN;
    return o;
}

// The variant's creation subobjects: ReShade's copy of the native ones with the
// blend state, render target formats and (no DSV) DSV format replaced. The
// walk mirrors rrsss::PipelinePatch::Prepare, except that the native blend is
// enabled and the pixel shader bytes are kept. Borrowed payloads are valid only
// during init_pipeline: create the variant synchronously from it.
struct CoveragePatch {
    std::array<reshade::api::pipeline_subobject, 64> objects{};
    reshade::api::blend_desc blend{};
    reshade::api::format target = reshade::api::format::r10g10b10a2_unorm;
    reshade::api::format dsv = reshade::api::format::unknown;
    const reshade::api::shader_desc *vertex{};       // borrowed, for VertexReplayProof
    const reshade::api::shader_desc *pixel_shader{}; // borrowed, for PixelReplayProof
    unsigned count{};
    unsigned problem = ~0u; // the refused subobject type, if one was
    const char *status = "not-prepared";
    CoveragePatch() = default;
    CoveragePatch(const CoveragePatch &) = delete;
    CoveragePatch &operator=(const CoveragePatch &) = delete;

    // pixel is the Target that PixelTarget found in source; native is the
    // observed native blend, which Classify accepted.
    bool Prepare(unsigned n, const reshade::api::pipeline_subobject *source, const Target &pixel,
                 const D3D12_BLEND_DESC &native, DepthClass depth) {
        using namespace reshade::api;
        using T = pipeline_subobject_type;
        count = 0;
        vertex = pixel_shader = nullptr;
        problem = ~0u;
        auto refuse = [&](const char *why, unsigned type = ~0u) {
            status = why;
            problem = type;
            return false;
        };
        if (!source || !n || n > objects.size())
            return refuse("subobject-count");
        if (depth == DepthClass::Refuse)
            return refuse("depth-stencil-not-replayable");
        std::array<bool, 64> seen{};
        unsigned bi = ~0u, fi = ~0u, di = ~0u;
        format native_dsv = format::unknown;
        for (unsigned i = 0; i < n; ++i) {
            const auto &o = source[i];
            const auto type = unsigned(o.type);
            if (type >= seen.size() || seen[type] || (o.count && !o.data))
                return refuse("subobject-duplicate-or-null", type);
            seen[type] = true;
            switch (o.type) {
            case T::vertex_shader: {
                const auto *s = static_cast<const shader_desc *>(o.data);
                if (o.count != 1 || !s->code || !s->code_size)
                    return refuse("vertex-shader", type);
                vertex = s;
                break;
            }
            case T::pixel_shader: {
                const auto *s = static_cast<const shader_desc *>(o.data);
                if (o.count != 1 || !s->code || s->code_size != pixel.bytes)
                    return refuse("pixel-shader", type);
                pixel_shader = s;
                break;
            }
            case T::blend_state: {
                if (o.count != 1)
                    return refuse("blend-state", type);
                const auto patched = PatchBlend(native, pixel.kind);
                blend = *static_cast<const blend_desc *>(o.data);
                blend.alpha_to_coverage_enable = patched.AlphaToCoverageEnable != FALSE;
                for (unsigned j = 0; j < 8; ++j) {
                    const auto &rt = patched.RenderTarget[j];
                    // PatchBlend only adds, and never enables a logic op.
                    if (rt.BlendOp != D3D12_BLEND_OP_ADD || rt.BlendOpAlpha != D3D12_BLEND_OP_ADD ||
                        rt.LogicOpEnable || rt.LogicOp != D3D12_LOGIC_OP_NOOP ||
                        !ReShadeFactor(rt.SrcBlend, blend.source_color_blend_factor[j]) ||
                        !ReShadeFactor(rt.DestBlend, blend.dest_color_blend_factor[j]) ||
                        !ReShadeFactor(rt.SrcBlendAlpha, blend.source_alpha_blend_factor[j]) ||
                        !ReShadeFactor(rt.DestBlendAlpha, blend.dest_alpha_blend_factor[j]))
                        return refuse("blend-factor-unmapped", type);
                    blend.blend_enable[j] = rt.BlendEnable != FALSE;
                    blend.logic_op_enable[j] = false;
                    blend.color_blend_op[j] = blend.alpha_blend_op[j] = blend_op::add;
                    blend.logic_op[j] = logic_op::noop;
                    blend.render_target_write_mask[j] = rt.RenderTargetWriteMask;
                }
                bi = i;
                break;
            }
            case T::render_target_formats:
                if (o.count != 1)
                    return refuse("render-target-count", type);
                fi = i;
                break;
            case T::sample_count:
                if (o.count != 1 || *static_cast<const unsigned *>(o.data) != 4)
                    return refuse("sample-count", type);
                break;
            // Any mask: the variant mirrors it, and Equivalent compares it.
            case T::sample_mask:
                if (o.count != 1)
                    return refuse("sample-mask", type);
                break;
            case T::depth_stencil_state:
                if (o.count != 1)
                    return refuse("depth-stencil-state", type);
                break;
            case T::depth_stencil_format:
                if (o.count != 1)
                    return refuse("depth-stencil-format", type);
                native_dsv = *static_cast<const format *>(o.data);
                di = i;
                break;
            case T::primitive_topology:
                if (o.count != 1 || *static_cast<const primitive_topology *>(o.data) !=
                                        primitive_topology::triangle_list)
                    return refuse("primitive-topology", type);
                break;
            case T::rasterizer_state:
                if (o.count != 1)
                    return refuse("rasterizer-state", type);
                break;
            case T::hull_shader:
            case T::domain_shader:
            case T::geometry_shader:
            case T::compute_shader: {
                if (!o.count)
                    break;
                const auto &s = *static_cast<const shader_desc *>(o.data);
                if (o.count != 1 || s.code || s.code_size)
                    return refuse("extra-shader-stage", type);
                break;
            }
            case T::stream_output_state:
                if (o.count != 1 ||
                    static_cast<const stream_output_desc *>(o.data)->rasterized_stream != 0)
                    return refuse("stream-output", type);
                break;
            case T::viewport_count:
                if (o.count != 1 || *static_cast<const unsigned *>(o.data) != 1)
                    return refuse("viewport-count", type);
                break;
            case T::input_layout:
                break;
            case T::dynamic_pipeline_states: {
                const auto *states = static_cast<const dynamic_state *>(o.data);
                for (unsigned j = 0; j < o.count; ++j) {
                    const auto s = states[j];
                    if (s != dynamic_state::primitive_topology &&
                        s != dynamic_state::input_element_stride &&
                        s != dynamic_state::blend_constant &&
                        s != dynamic_state::front_stencil_reference_value &&
                        s != dynamic_state::back_stencil_reference_value)
                        return refuse("dynamic-state", type);
                }
                break;
            }
            case T::flags:
                if (o.count != 1 ||
                    *static_cast<const pipeline_flags *>(o.data) != pipeline_flags::none)
                    return refuse("pipeline-flags", type);
                break;
            // Nonempty unrecognized stages or state could alter coverage or add output.
            default:
                if (o.count)
                    return refuse("unsupported-subobject", type);
                break;
            }
        }
        const T required[] = {T::vertex_shader,    T::pixel_shader,        T::blend_state,
                              T::rasterizer_state, T::depth_stencil_state, T::render_target_formats,
                              T::sample_count,     T::sample_mask,         T::primitive_topology};
        for (auto t : required)
            if (!seen[unsigned(t)])
                return refuse("required-subobject-missing", unsigned(t));
        // A kept DSV is the native one, so the variant needs its format.
        if (depth == DepthClass::KeepDsv && native_dsv == format::unknown)
            return refuse("kept-dsv-format-unknown", unsigned(T::depth_stencil_format));
        std::copy_n(source, n, objects.begin());
        objects[bi].data = &blend;
        objects[fi].data = &target;
        if (depth == DepthClass::NoDsv && di != ~0u)
            objects[di].data = &dsv;
        count = n;
        status = "prepared";
        return true;
    }
};

// The coverage target must render, blend and multisample 4x as R10G10B10A2_UNORM.
inline bool CoverageFormatSupported(ID3D12Device *device, const char *&status) noexcept {
    status = "coverage-format-query-failed";
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{DXGI_FORMAT_R10G10B10A2_UNORM};
    if (!device || FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support,
                                                      sizeof(support))))
        return false;
    const UINT needs = D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_BLENDABLE |
                       D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET;
    status = "coverage-format-unsupported";
    if ((support.Support1 & needs) != needs)
        return false;
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{
        DXGI_FORMAT_R10G10B10A2_UNORM, 4, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
    status = "coverage-format-query-failed";
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality,
                                           sizeof(quality))))
        return false;
    status = "coverage-four-sample-target-unavailable";
    if (!quality.NumQualityLevels)
        return false;
    status = "coverage-format-supported";
    return true;
}

// At most 16 registry log lines, except that the first ready line and the
// first refused line of each pixel shader are always logged.
class CoverageReportGate {
    std::mutex mutex;
    unsigned lines{};
    std::unordered_map<uint64_t, unsigned> firsts; // hash -> 1 ready, 2 refused

  public:
    bool Admit(uint64_t hash, bool ready) {
        std::lock_guard lock(mutex);
        auto &seen = firsts[hash];
        const unsigned bit = ready ? 1u : 2u;
        const bool first = !(seen & bit);
        seen |= bit;
        if (lines < 16) {
            ++lines;
            return true;
        }
        return first;
    }
};

// Accepted and refused native PSOs per device and kind, for the status row.
struct CoverageTally {
    uint64_t accepted{}, refused{};
};
struct CoverageCounts {
    CoverageTally oit, refraction;
};
class CoverageStatus {
    std::mutex mutex;
    std::unordered_map<reshade::api::device *, CoverageCounts> devices;

  public:
    void Record(reshade::api::device *device, Kind kind, bool accepted) {
        std::lock_guard lock(mutex);
        auto &counts = devices[device];
        auto &tally = kind == Kind::Oit ? counts.oit : counts.refraction;
        ++(accepted ? tally.accepted : tally.refused);
    }
    void Forget(reshade::api::device *device) {
        std::lock_guard lock(mutex);
        devices.erase(device);
    }
    CoverageCounts Counts(reshade::api::device *device) {
        std::lock_guard lock(mutex);
        const auto i = devices.find(device);
        return i != devices.end() ? i->second : CoverageCounts{};
    }
    // Summed over the live devices.
    CoverageCounts Counts() {
        std::lock_guard lock(mutex);
        CoverageCounts sum;
        for (const auto &[device, c] : devices) {
            sum.oit.accepted += c.oit.accepted;
            sum.oit.refused += c.oit.refused;
            sum.refraction.accepted += c.refraction.accepted;
            sum.refraction.refused += c.refraction.refused;
        }
        return sum;
    }
    // Every OIT (a36d) PSO on the live devices was refused, and none accepted.
    bool OitUnavailable() {
        const auto c = Counts();
        return c.oit.refused > 0 && c.oit.accepted == 0;
    }
};

// The native PSO keeps its variant alive (SetPrivateDataInterface), so the
// game's PSO lifetime covers both. The registry is an identity lookup only.
inline constexpr GUID CoverageAlternateId = {
    0xba279c56, 0x8e4b, 0x4093, {0xa4, 0x77, 0x39, 0xfc, 0x28, 0x26, 0x1d, 0x87}};
class CoveragePipelineRegistry {
  public:
    struct Entry {
        reshade::api::device *device;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> variant;
        DepthClass depth; // NoDsv: bind no DSV; KeepDsv: bind the mirrored native DSV
        Kind kind;
        DXGI_SAMPLE_DESC sample; // the native sample desc, which the variant has too
        bool refraction_alpha;   // alpha also accumulates the transmittance
    };

  private:
    // What Create made or found, kept until the outcome is logged.
    struct Outcome {
        unsigned problem = ~0u;  // the refused subobject type
        uint64_t detail = ~0ull; // the refused pixel-stage value
        Microsoft::WRL::ComPtr<ID3D12PipelineState> native, variant;
        std::shared_ptr<const Entry> entry;
    };
    std::span<const Target> targets;
    std::mutex mutex;
    std::unordered_map<uint64_t, std::shared_ptr<const Entry>> entries;
    std::unordered_map<reshade::api::device *, bool> signature_hooks;
    const char *last_refusal{};
    CoverageStatus status;
    CoverageReportGate reports;
    static inline thread_local bool creating{};

    void Log(const std::string &text) {
        if (logger)
            logger(text.c_str());
    }
    static std::string Hex(uint64_t value) {
        char text[24];
        snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(value));
        return text;
    }
    static const char *Name(Kind kind) {
        return kind == Kind::Oit ? "oit" : "refraction";
    }
    static const char *Name(DepthClass depth) {
        return depth == DepthClass::KeepDsv ? "kept" : "none";
    }
    static std::string Description(ID3D12PipelineState *native) {
        return rrsss::PipelineProof::NativeDescription(native);
    }
    // One line per native PSO. why is a static string; an unknown native PSO is
    // described as "unobserved". A variant that was made is described too.
    void Refused(reshade::api::device *device, Kind kind, uint64_t hash, const char *why,
                 const Outcome &outcome) noexcept {
        try {
            {
                std::lock_guard lock(mutex);
                last_refusal = why;
            }
            status.Record(device, kind, false);
            if (!reports.Admit(hash, false))
                return;
            std::string text = "Transparency albedo blend refused PSO " + Hex(hash) + ": " +
                               Label(Reason::NoPipeline) + ": " + why;
            if (outcome.problem != ~0u)
                text += " subobject=" + std::to_string(outcome.problem);
            if (outcome.detail != ~0ull)
                text += " detail=" + Hex(outcome.detail);
            text += " (" + Description(outcome.native.Get()) + ")";
            if (outcome.variant)
                text += " variant=(" + Description(outcome.variant.Get()) + ")";
            Log(text);
        } catch (...) {
        }
    }
    void Accepted(reshade::api::device *device, uint64_t hash, const Outcome &outcome) noexcept {
        try {
            const auto &entry = *outcome.entry;
            status.Record(device, entry.kind, true);
            if (!reports.Admit(hash, true))
                return;
            Log("Transparency albedo blend: pipeline ready for " + Hex(hash) +
                " (kind=" + Name(entry.kind) + " dsv=" + Name(entry.depth) +
                " refractionAlpha=" + (entry.refraction_alpha ? "1" : "0") + "; " +
                Description(outcome.native.Get()) + ")");
        } catch (...) {
        }
    }
    // Returns nullptr and sets out.entry when the variant is ready, else the refusal.
    const char *Create(reshade::api::device *device, reshade::api::pipeline_layout layout,
                       unsigned n, const reshade::api::pipeline_subobject *objects,
                       reshade::api::pipeline original, const Target &pixel, Outcome &out) {
        using namespace rrsss;
        if (!original.handle || !layout.handle)
            return "missing-pipeline-or-layout-handle";
        out.native = fg::NativeObject<ID3D12PipelineState>(
            reinterpret_cast<ID3D12PipelineState *>(original.handle));
        ID3D12PipelineState *native = out.native.Get();
        auto native_device =
            fg::NativeObject<ID3D12Device>(reinterpret_cast<ID3D12Device *>(device->get_native()));
        if (!native || !native_device)
            return "native-object-unavailable";
        if (native_device->GetNodeCount() != 1)
            return "multi-node-device";
        if (!NativePipelineProven(native))
            return NativePipelineStatus(native);
        NativeDepthStencilState depth_state;
        NativePipelineObservation observed;
        if (!NativeDepthStencil(native, depth_state) ||
            !PipelineProof::NativeObservation(native, observed))
            return "native-observation-unavailable";
        const auto blend = Classify(observed.blend, observed.rt_count);
        if (blend != BlendClass::Accept)
            return Label(blend);
        const auto depth = DepthStencilReplayable(depth_state.desc);
        if (depth == DepthClass::Refuse)
            return "native-depth-stencil-written";
        CoveragePatch patch;
        if (!patch.Prepare(n, objects, pixel, observed.blend, depth)) {
            out.problem = patch.problem;
            return patch.status;
        }
        const char *why = nullptr;
        if (!CoverageFormatSupported(native_device.Get(), why))
            return why;
        // The replay runs both stages again, so both must be read-only.
        if (!VertexReplayProof(patch.vertex->code, patch.vertex->code_size, why))
            return why;
        if (!PixelReplayProof(patch.pixel_shader->code, patch.pixel_shader->code_size, why,
                              out.detail))
            return why;
        {
            std::lock_guard lock(mutex);
            if (entries.size() >= 64 && !entries.contains(original.handle))
                return "registry-full";
        }
        struct Guard {
            Guard() {
                creating = true;
            }
            ~Guard() {
                creating = false;
            }
        } guard;
        const auto expected = Expected(observed, pixel.kind, depth);
        {
            PipelineProof::QualityScope quality(native_device.Get(), expected);
            reshade::api::pipeline made{};
            bool created;
            {
                rrtiming::Scope scope(rrtiming::Stage::AlternateCreate);
                created = device->create_pipeline(layout, patch.count, patch.objects.data(), &made);
            }
            if (!created || !made.handle)
                return "variant-creation-failed";
            out.variant = fg::NativeObject<ID3D12PipelineState>(
                reinterpret_cast<ID3D12PipelineState *>(made.handle));
            device->destroy_pipeline(made); // the ComPtr keeps the object
            // ApplyQuality runs Equivalent on the legacy desc before creating it.
            if (!quality.attempted)
                return "variant-not-created-through-legacy-path";
            if (!quality.applied)
                return "variant-state-mismatch-before-create";
        }
        NativePipelineObservation actual;
        if (!out.variant || !NativePipelineProven(out.variant.Get()) ||
            !PipelineProof::NativeObservation(out.variant.Get(), actual))
            return "variant-unproven";
        if (!PipelineProof::Equivalent(expected, actual))
            return "variant-state-mismatch";
        if (FAILED(native->SetPrivateDataInterface(CoverageAlternateId, out.variant.Get())))
            return "variant-lifetime-attachment-failed";
        out.entry = std::make_shared<const Entry>(
            Entry{device, std::move(out.variant), depth, pixel.kind, observed.sample,
                  RefractionAlpha(observed.blend, pixel.kind)});
        std::shared_ptr<const Entry> replaced; // released after the lock
        std::lock_guard lock(mutex);
        replaced = std::exchange(entries[original.handle], out.entry);
        return nullptr;
    }

  public:
    void (*logger)(const char *){};
    // targets: the pixel shaders to build variants for (a test passes its own).
    explicit CoveragePipelineRegistry(std::span<const Target> targets = Targets)
        : targets(targets) {}
    void Initialize(reshade::api::device *device, reshade::api::pipeline_layout layout, unsigned n,
                    const reshade::api::pipeline_subobject *objects,
                    reshade::api::pipeline original) noexcept {
        if (creating || !device || device->get_api() != reshade::api::device_api::d3d12)
            return;
        uint64_t hash = 0;
        const Target *pixel = nullptr;
        try {
            // A recycled handle never keeps the variant of the PSO it named before.
            if (original.handle)
                Forget(original.handle);
            pixel = PixelTarget(n, objects, hash, targets);
            if (!pixel)
                return;
            Outcome outcome;
            if (const char *why = Create(device, layout, n, objects, original, *pixel, outcome))
                Refused(device, pixel->kind, hash, why, outcome);
            else
                Accepted(device, hash, outcome);
        } catch (...) {
            if (pixel)
                Refused(device, pixel->kind, hash, "pipeline-preparation-exception", {});
        }
    }
    // The variant of a native PSO on this device, or nullptr.
    std::shared_ptr<const Entry> Find(reshade::api::device *device, uint64_t original) {
        std::lock_guard lock(mutex);
        const auto i = entries.find(original);
        return i != entries.end() && i->second->device == device ? i->second : nullptr;
    }
    // Erased entries are released after the lock: a variant's last release can
    // reach OnDestroyPipeline, which calls Forget.
    void Forget(uint64_t original) {
        std::shared_ptr<const Entry> gone;
        std::lock_guard lock(mutex);
        if (const auto i = entries.find(original); i != entries.end()) {
            gone = std::move(i->second);
            entries.erase(i);
        }
    }
    void ForgetDevice(reshade::api::device *device) {
        std::vector<std::shared_ptr<const Entry>> gone;
        {
            std::lock_guard lock(mutex);
            signature_hooks.erase(device);
            size_t count = 0;
            for (const auto &[handle, entry] : entries)
                count += entry->device == device;
            try {
                gone.reserve(count);
            } catch (...) {
                // Without the memory, the entries are released under the lock.
            }
            for (auto i = entries.begin(); i != entries.end();)
                if (i->second->device != device) {
                    ++i;
                } else {
                    if (gone.size() < gone.capacity())
                        gone.push_back(std::move(i->second));
                    i = entries.erase(i);
                }
        }
        status.Forget(device);
    }
    // Status row: accepted and refused PSOs per kind, for one device or summed
    // over the live ones, and whether the OIT capture is unavailable.
    CoverageCounts Counts(reshade::api::device *device) {
        return status.Counts(device);
    }
    CoverageCounts Counts() {
        return status.Counts();
    }
    bool OitUnavailable() {
        return status.OitUnavailable();
    }
    // The last refusal reason (a static string), or nullptr.
    const char *LastRefusal() {
        std::lock_guard lock(mutex);
        return last_refusal;
    }
    // rrsig::Attach's result per device; rrsig::Attached() is process-wide.
    void SignatureHook(reshade::api::device *device, bool attached) {
        std::lock_guard lock(mutex);
        signature_hooks.insert_or_assign(device, attached);
    }
    bool SignatureHooked(reshade::api::device *device) {
        std::lock_guard lock(mutex);
        const auto i = signature_hooks.find(device);
        return i != signature_hooks.end() && i->second;
    }
    // True when a device was seen and the hook attached on every live one.
    bool SignatureHooked() {
        std::lock_guard lock(mutex);
        bool all = !signature_hooks.empty();
        for (const auto &[device, attached] : signature_hooks)
            all = all && attached;
        return all;
    }
};
} // namespace rrblend
