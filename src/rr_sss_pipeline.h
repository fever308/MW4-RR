#pragma once
#include <reshade.hpp>
#include <array>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <cstdio>
#include <sstream>
#include "feed_fg_identity.h"
#include "rr_sss_pipeline_proof.h"
#include "rr_sss_stencil_shader.h"
#include "rr_sss_vertex_proof.h"
#include "rr_sss_runtime_shader.h"
namespace rrsss {
// Borrowed nonchanged subobject payloads are valid only during init_pipeline.
// Create the alternate synchronously; never save this patch for a later draw.
struct PipelinePatch {
    std::array<reshade::api::pipeline_subobject, 64> objects{};
    std::array<reshade::api::format, 4> formats{};
    reshade::api::shader_desc shader{};
    std::shared_ptr<const std::vector<unsigned char>> prepared;
    reshade::api::blend_desc blend{};
    reshade::api::depth_stencil_desc inverse{};
    bool helper_marker{};
    unsigned count{};
    const char *status = "not-prepared";
    PipelinePatch() = default;
    PipelinePatch(const PipelinePatch &) = delete;
    PipelinePatch &operator=(const PipelinePatch &) = delete;
    static unsigned long long Hash(const void *p, size_t n) {
        auto *b = static_cast<const unsigned char *>(p);
        unsigned long long h = 14695981039346656037ull;
        for (size_t i = 0; i < n; ++i)
            h = (h ^ b[i]) * 1099511628211ull;
        return h;
    }
    // rejected names why a structurally similar build was refused.
    static bool IsTarget(unsigned n, const reshade::api::pipeline_subobject *objects,
                         const char *&rejected) {
        rejected = nullptr;
        if (!objects || !n || n > 64)
            return false;
        for (unsigned i = 0; i < n; ++i) {
            const auto &o = objects[i];
            if (o.type != reshade::api::pipeline_subobject_type::pixel_shader || o.count != 1 ||
                !o.data)
                continue;
            const auto &s = *static_cast<const reshade::api::shader_desc *>(o.data);
            if (!s.code || !s.code_size || s.code_size > 4 * 1024 * 1024)
                continue;
            // The container check is cheap and skips unrelated shaders.
            rrtiming::Global().pixel_shaders.fetch_add(1, std::memory_order_relaxed);
            if (!StructuralCandidate(s.code, s.code_size))
                continue;
            rrtiming::Global().candidates.fetch_add(1, std::memory_order_relaxed);
            if (Recognized().Has(Hash(s.code, s.code_size), s.code_size))
                return true;
            // Another build is prepared here, once per input, so that its
            // identity is known before any of its draws. Its refusal is
            // reported once, not for every pipeline that uses it.
            const char *status = nullptr;
            bool attempted = false;
            if (RuntimeShader().Get(s.code, s.code_size, ReplayCompiler(), status, &attempted))
                return true;
            if (attempted)
                rejected = status;
        }
        return false;
    }
    // Only called for the exact target at pipeline creation; no per-frame work.
    // Describe all gates together so a first rejection cannot hide a second one.

    bool Prepare(unsigned n, const reshade::api::pipeline_subobject *source) {
        using namespace reshade::api;
        using T = pipeline_subobject_type;
        count = 0;
        status = "unsupported-pipeline-description";
        if (!source || !n || n > objects.size())
            return false;
        std::array<bool, 64> seen{};
        unsigned pi = ~0u, bi = ~0u, fi = ~0u;
        bool stencil = false;
        format dsv = format::unknown;
        for (unsigned i = 0; i < n; ++i) {
            const auto &o = source[i];
            const auto type = unsigned(o.type);
            if (type >= seen.size() || seen[type] || (o.count && !o.data))
                return false;
            seen[type] = true;
            switch (o.type) {
            case T::vertex_shader: {
                if (o.count != 1)
                    return false;
                auto &s = *static_cast<const shader_desc *>(o.data);
                if (!s.code || !s.code_size)
                    return false;
                break;
            }
            case T::pixel_shader: {
                if (o.count != 1)
                    return false;
                const auto &s = *static_cast<const shader_desc *>(o.data);
                if (!s.code || !s.code_size || s.code_size > 4 * 1024 * 1024 ||
                    !Recognized().Has(Hash(s.code, s.code_size), s.code_size)) {
                    status = "unsupported-sss-shader";
                    return false;
                }
                pi = i;
                shader = s;
                break;
            }
            case T::blend_state: {
                if (o.count != 1)
                    return false;
                blend = *static_cast<const blend_desc *>(o.data);
                if (blend.alpha_to_coverage_enable || blend.blend_enable[0] ||
                    blend.logic_op_enable[0] || blend.render_target_write_mask[0] != 15)
                    return false;
                bi = i;
                break;
            }
            case T::render_target_formats:
                if (o.count != 1 || *static_cast<const format *>(o.data) != format::r11g11b10_float)
                    return false;
                formats = {*static_cast<const format *>(o.data), format::r16_float};
                fi = i;
                break;
            case T::sample_count:
                if (o.count != 1 || *static_cast<const unsigned *>(o.data) != 4)
                    return false;
                break;
            case T::sample_mask:
                if (o.count != 1 || *static_cast<const unsigned *>(o.data) != ~0u)
                    return false;
                break;
            case T::depth_stencil_state: {
                if (o.count != 1)
                    return false;
                const auto &d = *static_cast<const depth_stencil_desc *>(o.data);
                stencil = d.stencil_enable;
                if (d.depth_enable || d.depth_write_mask) {
                    status = "unsupported-native-depth-state";
                    return false;
                }
                if (stencil && (d.front_stencil_read_mask != d.back_stencil_read_mask ||
                                !InverseStencil(static_cast<D3D12_COMPARISON_FUNC>(
                                    unsigned(d.front_stencil_func) + 1)) ||
                                !InverseStencil(static_cast<D3D12_COMPARISON_FUNC>(
                                    unsigned(d.back_stencil_func) + 1)))) {
                    status = "unsupported-asymmetric-stencil-state";
                    return false;
                }
                break;
            }
            case T::depth_stencil_format:
                if (o.count != 1)
                    return false;
                dsv = *static_cast<const format *>(o.data);
                break;
            case T::primitive_topology:
                if (o.count != 1 || *static_cast<const primitive_topology *>(o.data) !=
                                        primitive_topology::triangle_list)
                    return false;
                break;
            case T::rasterizer_state:
                if (o.count != 1)
                    return false;
                break;
            case T::hull_shader:
            case T::domain_shader:
            case T::geometry_shader:
            case T::compute_shader: {
                if (!o.count)
                    break;
                if (o.count != 1)
                    return false;
                const auto &s = *static_cast<const shader_desc *>(o.data);
                if (s.code || s.code_size)
                    return false;
                break;
            }
            case T::stream_output_state:
                if (o.count != 1 ||
                    static_cast<const stream_output_desc *>(o.data)->rasterized_stream != 0)
                    return false;
                break;
            case T::viewport_count:
                if (o.count != 1 || *static_cast<const unsigned *>(o.data) != 1)
                    return false;
                break;
            case T::input_layout:
                break;
            case T::dynamic_pipeline_states: {
                auto *states = static_cast<const dynamic_state *>(o.data);
                for (unsigned j = 0; j < o.count; ++j) {
                    auto s = states[j];
                    if (s != dynamic_state::primitive_topology &&
                        s != dynamic_state::input_element_stride &&
                        s != dynamic_state::blend_constant &&
                        s != dynamic_state::front_stencil_reference_value &&
                        s != dynamic_state::back_stencil_reference_value)
                        return false;
                }
                break;
            }
            case T::flags:
                if (o.count != 1 ||
                    *static_cast<const pipeline_flags *>(o.data) != pipeline_flags::none)
                    return false;
                break;
            // Nonempty unrecognized stages/state could alter coverage or add output.
            default:
                if (o.count)
                    return false;
                break;
            }
        }
        const T required[] = {T::vertex_shader,         T::pixel_shader,
                              T::blend_state,           T::rasterizer_state,
                              T::depth_stencil_state,   T::depth_stencil_format,
                              T::render_target_formats, T::sample_count,
                              T::sample_mask,           T::primitive_topology};
        for (auto t : required)
            if (!seen[unsigned(t)])
                return false;
        if (dsv != (stencil ? format::d32_float_s8_uint : format::unknown)) {
            status = "unsupported-native-dsv-format";
            return false;
        }
        std::copy_n(source, n, objects.begin());
        prepared = RuntimeShader().Get(shader.code, shader.code_size, ReplayCompiler(), status);
        if (!prepared)
            return false;
        shader.code = prepared->data();
        shader.code_size = prepared->size();
        objects[pi].data = &shader;
        blend.blend_enable[1] = blend.logic_op_enable[1] = false;
        blend.render_target_write_mask[1] = 1;
        objects[bi].data = &blend;
        objects[fi].count = 2;
        objects[fi].data = formats.data();
        count = n;
        status = "prepared-native-proof-still-required";
        return true;
    }

    // marker: the Subsurface Scattering coverage initializer (RRSSSStencilMarkerPS), which
    // differs from the baseline initializer only in its pixel shader.
    bool PrepareHelper(unsigned n, const reshade::api::pipeline_subobject *source,
                       bool marker = false) {
        using namespace reshade::api;
        using T = pipeline_subobject_type;
        if (!Prepare(n, source))
            return false;
        helper_marker = marker;
        shader.code = marker ? RRSSSStencilMarkerPS : RRSSSStencilPS;
        shader.code_size = marker ? sizeof(RRSSSStencilMarkerPS) : sizeof(RRSSSStencilPS);
        formats[0] = format::r16_float;
        for (unsigned i = 0; i < count; ++i) {
            auto &o = objects[i];
            if (o.type == T::render_target_formats)
                o.count = 1;
            if (o.type == T::blend_state) {
                blend.render_target_write_mask[0] = 1;
                blend.blend_enable[0] = blend.logic_op_enable[0] = false;
            }
            if (o.type == T::depth_stencil_state) {
                inverse = *static_cast<const depth_stencil_desc *>(source[i].data);
                inverse.depth_enable = inverse.depth_write_mask = false;
                inverse.depth_func = compare_op::always;
                inverse.front_stencil_write_mask = inverse.back_stencil_write_mask = 0;
                inverse.front_stencil_fail_op = inverse.front_stencil_depth_fail_op =
                    inverse.front_stencil_pass_op = stencil_op::keep;
                inverse.back_stencil_fail_op = inverse.back_stencil_depth_fail_op =
                    inverse.back_stencil_pass_op = stencil_op::keep;
                inverse.front_stencil_func =
                    compare_op(unsigned(InverseStencil(D3D12_COMPARISON_FUNC(
                                   unsigned(inverse.front_stencil_func) + 1))) -
                               1);
                inverse.back_stencil_func =
                    compare_op(unsigned(InverseStencil(D3D12_COMPARISON_FUNC(
                                   unsigned(inverse.back_stencil_func) + 1))) -
                               1);
                o.data = &inverse;
            }
        }
        return true;
    }
    NativePipelineObservation Expected(const NativePipelineObservation &original,
                                       bool helper) const {
        auto o = original;
        const void *code = !helper        ? static_cast<const void *>(prepared->data())
                           : helper_marker ? static_cast<const void *>(RRSSSStencilMarkerPS)
                                           : static_cast<const void *>(RRSSSStencilPS);
        const size_t bytes = !helper        ? prepared->size()
                             : helper_marker ? sizeof(RRSSSStencilMarkerPS)
                                             : sizeof(RRSSSStencilPS);
        o.shaders[1] = {bytes, true, PipelineProof::ByteHash(code, bytes)};
        o.cached_bytes = 0;
        o.cached_present = false;
        o.blend.IndependentBlendEnable = helper ? FALSE : TRUE;
        if (helper) {
            o.rt_count = 1;
            o.rt_formats[0] = DXGI_FORMAT_R16_FLOAT;
            o.blend.RenderTarget[0].BlendEnable = o.blend.RenderTarget[0].LogicOpEnable = FALSE;
            o.blend.RenderTarget[0].RenderTargetWriteMask = 1;
            o.depth.desc = NormalizeDepthStencil(InverseStencilState(original.depth.desc));
        } else {
            o.rt_count = 2;
            o.rt_formats[1] = DXGI_FORMAT_R16_FLOAT;
            o.blend.RenderTarget[1] = o.blend.RenderTarget[0];
            o.blend.RenderTarget[1].BlendEnable = o.blend.RenderTarget[1].LogicOpEnable = FALSE;
            o.blend.RenderTarget[1].RenderTargetWriteMask = 1;
        }
        return o;
    }
};
// The original native PSO retains the alternate, so the game's existing PSO
// lifetime covers both versions. A draw also keeps explicit references until
// its recording and fence retire; the registry is only an identity lookup.
inline constexpr GUID SssAlternateId = {
    0x933aa612, 0x6469, 0x4303, {0xa1, 0x51, 0xc6, 0x8a, 0x54, 0x37, 0xe5, 0x92}};
inline constexpr GUID SssStencilId = {
    0x813aa482, 0x9392, 0x42f2, {0x95, 0x94, 0x84, 0x63, 0x25, 0xea, 0x64, 0x17}};
inline constexpr GUID SssStencilMarkerId = {
    0x2c6f1d0e, 0x7a43, 0x4b8e, {0x9f, 0x21, 0x5d, 0x3a, 0xc8, 0x61, 0x0b, 0xe4}};
class PipelineRegistry {
    struct Entry {
        reshade::api::device *device{};
        Microsoft::WRL::ComPtr<ID3D12PipelineState> alternate;
        StencilSetup stencil;
    };
    std::mutex mutex;
    std::unordered_map<uint64_t, Entry> entries;
    std::atomic<unsigned> reports{};
    static inline thread_local bool creating{};
    void Report(const char *text) noexcept {
        if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
            try {
                if (logger)
                    logger(text);
            } catch (...) {
            }
    }

  public:
    void (*logger)(const char *){};
    void Initialize(reshade::api::device *device, reshade::api::pipeline_layout layout, unsigned n,
                    const reshade::api::pipeline_subobject *objects,
                    reshade::api::pipeline original) noexcept {
        if (creating || !device || device->get_api() != reshade::api::device_api::d3d12)
            return;
        try {
            const char *rejected = nullptr;
            if (!PipelinePatch::IsTarget(n, objects, rejected)) {
                if (rejected)
                    Report((std::string("RR SSS shader build not supported: ") + rejected).c_str());
                return;
            }
            if (!original.handle || !layout.handle) {
                Report("RR exact SSS shader rejected: missing pipeline or layout handle; baseline "
                       "RR retained");
                return;
            }
            auto native = fg::NativeObject<ID3D12PipelineState>(
                reinterpret_cast<ID3D12PipelineState *>(original.handle));
            PipelinePatch patch;
            if (!patch.Prepare(n, objects)) {
                Report(patch.status);
                return;
            }
            auto native_device = fg::NativeObject<ID3D12Device>(
                reinterpret_cast<ID3D12Device *>(device->get_native()));
            if (!native || !native_device || native_device->GetNodeCount() != 1 ||
                !NativePipelineProven(native.Get())) {
                Report("SSS native pipeline unavailable");
                return;
            }
            {
                std::lock_guard lock(mutex);
                if (entries.size() >= 64 && !entries.contains(original.handle))
                    return;
            }
            struct Guard {
                Guard() {
                    creating = true;
                }
                ~Guard() {
                    creating = false;
                }
            } guard;
            NativeDepthStencilState depth;
            NativePipelineObservation observed;
            StencilSetup stencil;
            if (!NativeDepthStencil(native.Get(), depth) ||
                !PipelineProof::NativeObservation(native.Get(), observed)) {
                Report("RR SSS native depth/stencil state unavailable; baseline RR retained");
                return;
            }
            stencil.samples = observed.sample;
            auto create = [&](PipelinePatch &description, bool helper,
                              Microsoft::WRL::ComPtr<ID3D12PipelineState> &out) {
                const auto expected = description.Expected(observed, helper);
                PipelineProof::QualityScope quality(native_device.Get(), expected);
                reshade::api::pipeline made{};
                bool created;
                {
                    rrtiming::Scope scope(rrtiming::Stage::AlternateCreate);
                    created = device->create_pipeline(layout, description.count,
                                                      description.objects.data(), &made);
                }
                if (!created || !made.handle)
                    return false;
                out = fg::NativeObject<ID3D12PipelineState>(
                    reinterpret_cast<ID3D12PipelineState *>(made.handle));
                device->destroy_pipeline(made);
                NativePipelineObservation actual;
                if (!quality.applied || !out || !NativePipelineProven(out.Get()) ||
                    !PipelineProof::NativeObservation(out.Get(), actual) ||
                    !PipelineProof::Equivalent(expected, actual)) {
                    Report("SSS cloned pipeline state mismatch");
                    out.Reset();
                    return false;
                }
                return true;
            };
            if (depth.desc.StencilEnable) {
                if (!StencilCompatible(depth.desc, depth.format, true)) {
                    Report("RR SSS geometry replay initializer rejected native "
                           "depth/bounds/format/masks; baseline RR retained");
                    return;
                }
                const char *proof = "vertex-stage-missing";
                bool vertex_proven = false;
                uint64_t vertex_hash = 0;
                size_t vertex_bytes = 0;
                for (unsigned i = 0; i < n; ++i)
                    if (objects[i].type == reshade::api::pipeline_subobject_type::vertex_shader &&
                        objects[i].count == 1) {
                        const auto &vs =
                            *static_cast<const reshade::api::shader_desc *>(objects[i].data);
                        vertex_bytes = vs.code_size;
                        if (vs.code && vs.code_size <= 4 * 1024 * 1024)
                            vertex_hash = PipelineProof::ByteHash(vs.code, vs.code_size);
                        vertex_proven = VertexReplayProof(vs.code, vs.code_size, proof);
                    }
                if (!vertex_proven) {
                    const auto detail = std::string("RR SSS geometry replay unavailable: ") +
                                        proof + " vertexBytes=" + std::to_string(vertex_bytes) +
                                        " vertexHash=" + std::to_string(vertex_hash);
                    Report(detail.c_str());
                    return;
                }
                PipelinePatch helper;
                if (!helper.PrepareHelper(n, objects) || !create(helper, true, stencil.pipeline)) {
                    Report(
                        "RR SSS geometry replay initializer creation failed; baseline RR retained");
                    return;
                }
                stencil.required = true;
                stencil.read_mask = depth.desc.FrontFace.StencilReadMask;
                stencil.replay_geometry = true;
                // Optional: without it the Subsurface Scattering coverage selection records
                // no coverage; the guide is unaffected.
                PipelinePatch marker;
                if (!marker.PrepareHelper(n, objects, true) ||
                    !create(marker, true, stencil.marker)) {
                    stencil.marker.Reset();
                    Report("Subsurface Scattering coverage initializer unavailable; the "
                           "coverage selection records no coverage");
                }
            }
            Microsoft::WRL::ComPtr<ID3D12PipelineState> alternate;
            if (!create(patch, false, alternate)) {
                Report("RR SSS MRT scoped clone creation failed; baseline RR retained");
                return;
            }
            const bool retained =
                (!stencil.required || SUCCEEDED(alternate->SetPrivateDataInterface(
                                          SssStencilId, stencil.pipeline.Get()))) &&
                (!stencil.marker || SUCCEEDED(alternate->SetPrivateDataInterface(
                                        SssStencilMarkerId, stencil.marker.Get()))) &&
                SUCCEEDED(native->SetPrivateDataInterface(SssAlternateId, alternate.Get()));
            if (!retained) {
                Report("RR SSS alternate/helper lifetime attachment failed; baseline RR retained");
                return;
            }

            {
                std::lock_guard lock(mutex);
                entries.insert_or_assign(original.handle,
                                         Entry{device, std::move(alternate), std::move(stencil)});
            }
            Report("Subsurface Scattering guide pipeline initialized.");
        } catch (...) {
            Report("RR optional SSS pipeline preparation failed; baseline RR retained");
        }
    }
    Microsoft::WRL::ComPtr<ID3D12PipelineState> Find(reshade::api::device *device,
                                                     uint64_t original) {
        std::lock_guard lock(mutex);
        const auto i = entries.find(original);
        return i != entries.end() && i->second.device == device ? i->second.alternate : nullptr;
    }

    StencilSetup FindStencil(reshade::api::device *device, uint64_t original) {
        std::lock_guard lock(mutex);
        const auto i = entries.find(original);
        return i != entries.end() && i->second.device == device ? i->second.stencil
                                                                : StencilSetup{};
    }
    void Forget(uint64_t original) {
        std::lock_guard lock(mutex);
        entries.erase(original);
    }
    void ForgetDevice(reshade::api::device *device) {
        std::lock_guard lock(mutex);
        for (auto i = entries.begin(); i != entries.end();)
            if (i->second.device == device)
                i = entries.erase(i);
            else
                ++i;
    }
};
} // namespace rrsss
