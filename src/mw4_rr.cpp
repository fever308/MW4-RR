#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include "rr_trace.h"
#include "rr_binding_probe.h"
#include "rr_material.h"
#include "rr_pipeline_state.h"
#include "rr_buffer_inspector.h"
#include "rr_sparse_pipelines.h"
#include "rr_sparse_copy.h"
#include "rr_live.h"
#include "rr_live_proof.h"
#include "rr_sss_pipeline.h"
#include "rr_oit_signature.h"
#include "rr_transparency_coverage_pipeline.h"
#include "rr_scene_shape.h"
#include "rr_vrs_config.h"
#include "rr_game_config.h"
#include <filesystem>
#include <fstream>
#include <sstream>
extern "C" __declspec(dllexport) const char *NAME = "MW4 Ray Reconstruction";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Beta 1: Ray Reconstruction with Subsurface Scattering and transparency guides.";
static void WriteLog(const char *text) {
    rrtrace::LastError preserve;
    reshade::log::message(reshade::log::level::warning, text);
}
static rrlive::Controller &LiveRR() {
    static auto *p = []() {
        auto *v = new rrlive::Controller;
        v->logger = &WriteLog;
        v->lighting_valid = [](UINT64 device, UINT64 revision) {
            const auto h = rrbypass::Health((reshade::api::device *)device);
            return h.temporal && h.spatial && h.sampling && h.corrected && !h.unreplaced &&
                   !h.failed && h.revision == revision;
        };
        return v;
    }();
    return *p;
}
static bool SharedOutputCoverage(ID3D12GraphicsCommandList *);
static void CoverageAfterIndirect(const rrcompute::IndirectCall &call) noexcept;
static rrcompute::Tracker &ComputeState() {
    static auto *p = []() {
        auto *v = new rrcompute::Tracker;
        v->UseSharedOutputContext(&SharedOutputCoverage);
        if (!v->SetAfterIndirect(&CoverageAfterIndirect, &WriteLog))
            WriteLog("Transparency albedo blend unavailable: indirect draw observer not installed");
        return v;
    }();
    return *p;
}
static rrsparsecopy::Layouts &SparseLayouts() {
    static auto *p = new rrsparsecopy::Layouts;
    return *p;
}
static void SparseFailure(const char *why) {
    rrbypass::GlobalFail(why);
}
static rrsparsecopy::Pool &SparsePool() {
    static auto *p = []() {
        auto *v = new rrsparsecopy::Pool;
        v->failure = &SparseFailure;
        return v;
    }();
    return *p;
}
static rrshader::DispatchTracker &ShaderDispatches() {
    static auto *p = new rrshader::DispatchTracker;
    return *p;
}
static rrsss::PipelineRegistry &SssPipelines() {
    static auto *p = []() {
        auto *v = new rrsss::PipelineRegistry;
        v->logger = &WriteLog;
        return v;
    }();
    return *p;
}
static rrblend::CoveragePipelineRegistry &CoveragePipelines() {
    static auto *p = []() {
        auto *v = new rrblend::CoveragePipelineRegistry;
        v->logger = &WriteLog;
        return v;
    }();
    return *p;
}
template <class F> static void GlassOnly(F &&f) noexcept {
    try {
        f();
    } catch (...) {
        WriteLog("Transparency observation failed");
    }
}
static void FrameReset(ID3D12GraphicsCommandList *cl) {
    try {
        LiveRR().Reset(cl);
        SparseLayouts().Reset(cl);
        SparsePool().Reset(cl);
    } catch (...) {
        SparseFailure("Recording reset failed");
    }
}
static uint64_t FrameSubmit(ID3D12CommandQueue *q, UINT n, ID3D12CommandList *const *l) {
    if (rrinternal::active)
        return 0;
    try {
        LiveRR().Before(q, n, l);
        return SparsePool().Before(q, n, l);
    } catch (...) {
        SparseFailure("Submission observation failed");
        return 0;
    }
}
static void FrameSubmitEnd(ID3D12CommandQueue *q, uint64_t token) {
    if (rrinternal::active)
        return;
    try {
        SparsePool().After(q, unsigned(token));
    } catch (...) {
        SparseFailure("Submission completion failed");
    }
    LiveRR().After(q);
}
static void FrameBarriers(ID3D12GraphicsCommandList *cl, UINT n, const D3D12_RESOURCE_BARRIER *b) {
    if (rrinternal::active)
        return;
    LiveRR().Barriers(cl, n, b);
    try {
        SparseLayouts().Observe(cl, n, b);
    } catch (...) {
        SparseLayouts().Lose(cl);
        SparseFailure("Resource-state observation failed");
    }
}
static void OitSceneTransition(ID3D12GraphicsCommandList *,
                               const D3D12_RESOURCE_BARRIER &) noexcept;
static void OwnedTransition(ID3D12GraphicsCommandList *cl, const D3D12_RESOURCE_BARRIER &b,
                            rrbuffer::BarrierFn) {
    // Private copies can hold the sparse-pool lock while submission holds the
    // controller lock. Do not re-enter the controller from a private barrier.
    if (rrinternal::active)
        return;
    OitSceneTransition(cl, b);
}
static void SharedOutputPredication(ID3D12GraphicsCommandList *cl, bool on) {
    ComputeState().OutputPredication(cl, on);
}
static void SharedOutputBegin(ID3D12GraphicsCommandList *cl, D3D12_RENDER_PASS_FLAGS flags) {
    ComputeState().OutputBeginPass(cl, flags);
}
static void SharedOutputEnd(ID3D12GraphicsCommandList *cl) {
    ComputeState().OutputEndPass(cl);
}
static rrbuffer::Inspector &AlbedoInspector() {
    static auto *p = []() {
        auto *v = new rrbuffer::Inspector;
        v->logger = &WriteLog;
        v->on_barriers = &FrameBarriers;
        v->on_unknown_states = [](ID3D12GraphicsCommandList *cl) {
            LiveRR().TransparencyProducerUnknown(cl);
            SparseLayouts().Lose(cl);
        };
        v->on_execute = &FrameSubmit;
        v->on_execute_end = &FrameSubmitEnd;
        v->on_reset = &FrameReset;
        v->on_reset_compute = [](ID3D12GraphicsCommandList *cl, ID3D12PipelineState *pso) {
            ComputeState().Reset(cl, pso);
        };
        v->after_transition = &OwnedTransition;
        v->SetOutputObserver(&SharedOutputPredication, &SharedOutputBegin, &SharedOutputEnd);
        return v;
    }();
    return *p;
}
static bool SharedOutputCoverage(ID3D12GraphicsCommandList *cl) {
    return AlbedoInspector().OutputContextCoverage(cl, &SharedOutputPredication, &SharedOutputBegin,
                                                   &SharedOutputEnd);
}
static void FrameCamera(uint32_t f, uint32_t v, const sl::Constants &c) {
    LiveRR().Camera(f, v, c);
}
static void SrBoundary(ID3D12GraphicsCommandList *cl, rrtrace::Kind kind,
                       const NVSDK_NGX_Parameter *p) {
    if (kind != rrtrace::Kind::SR)
        return;
    auto n = fg::NativeObject<ID3D12GraphicsCommandList>(cl);
    LiveRR().BeforeSR(cl, p, rrtrace::Probe::CurrentCall(),
                      n && AlbedoInspector().SafeCopies(n.Get()) &&
                          AlbedoInspector().StateCoverage(n.Get()) &&
                          rrtrace::Probe::ShutdownCovered(rrtrace::Probe::CurrentRuntime()));
}
static bool SrReplacement(ID3D12GraphicsCommandList *cl, const NVSDK_NGX_Parameter *p,
                          NVSDK_NGX_Result &result) {
    auto n = fg::NativeObject<ID3D12GraphicsCommandList>(cl);
    const bool replaced = LiveRR().TryReplaceSR(
        cl, p, rrtrace::Probe::CurrentCall(), rrtrace::Probe::CurrentRuntime(),
        rrtrace::Probe::RuntimeEpoch(),
        n && AlbedoInspector().SafeCopies(n.Get()) && AlbedoInspector().StateCoverage(n.Get()),
        SparseLayouts());
    if (replaced) {
        rrsr::counters.replaced.fetch_add(1, std::memory_order_relaxed);
        ComputeState().Invalidate(n.Get());
        result = NVSDK_NGX_Result_Success;
    }
    return replaced;
}
static bool BeforeNgxShutdown(HMODULE m, ID3D12Device *d) {
    return LiveRR().BeforeShutdown(m, d);
}
static void AfterNgxShutdown(HMODULE m, ID3D12Device *d, bool ok) {
    LiveRR().AfterShutdown(m, d, ok);
}
static void SrCompleted(ID3D12GraphicsCommandList *cl, rrtrace::Kind k,
                        const NVSDK_NGX_Parameter *p, NVSDK_NGX_Result r) {
    // Frame generation is counted for the health line only, lock-free: never the controller.
    if (k == rrtrace::Kind::FG) {
        rrhealth::presentation.fg_evaluations.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (k != rrtrace::Kind::SR)
        return;
    rrsr::counters.native.fetch_add(1, std::memory_order_relaxed);
    auto n = fg::NativeObject<ID3D12GraphicsCommandList>(cl);
    LiveRR().AfterSR(cl, p, rrtrace::Probe::CurrentCall(), r, rrtrace::Probe::CurrentRuntime(),
                     rrtrace::Probe::RuntimeEpoch(),
                     n && AlbedoInspector().SafeCopies(n.Get()) &&
                         AlbedoInspector().StateCoverage(n.Get()),
                     false);
    ComputeState().Invalidate(n.Get());
}
static rrtrace::Probe &Observer() {
    static auto *p = []() {
        auto *v = new rrtrace::Probe;
        v->logger = &WriteLog;
        v->before_evaluate = &SrBoundary;
        v->replace_evaluate = &SrReplacement;
        v->after_evaluate = &SrCompleted;
        v->after_constants = &FrameCamera;
        v->before_shutdown = &BeforeNgxShutdown;
        v->after_shutdown = &AfterNgxShutdown;
        return v;
    }();
    return *p;
}
static bool PinInspector() {
    HMODULE self{};
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_PIN,
                              reinterpret_cast<LPCWSTR>(&PinInspector), &self) != FALSE;
}
static void OnDevice(reshade::api::device *d) {
    try {
        if (d->get_api() == reshade::api::device_api::d3d12 && PinInspector()) {
            d->create_private_data<rrbindings::DeviceState>();
            if (!rrsss::PipelineProof::Attach(reinterpret_cast<ID3D12Device *>(d->get_native())))
                WriteLog("RR optional native SSS PSO proof hooks unavailable; unproven SSS "
                         "pipelines stay off");
            // Before the game creates its command signatures: earlier ones read as Unknown.
            const bool signatures = rrsig::Attach(reinterpret_cast<ID3D12Device *>(d->get_native()),
                                                  ComputeState(), &WriteLog);
            CoveragePipelines().SignatureHook(d, signatures);
            if (!signatures)
                WriteLog("Transparency albedo blend unavailable: command signature hook not "
                         "installed");
        }
        Observer().Poll();
    } catch (...) {
        SparseFailure("RR provenance/context exception; RR blocked");
        WriteLog("binding device initialization failed");
    }
}
static void OnDestroyDevice(reshade::api::device *d) {
    SssPipelines().ForgetDevice(d);
    CoveragePipelines().ForgetDevice(d);
    rrbypass::Forget(d);
    if (rrbindings::State(d))
        d->destroy_private_data<rrbindings::DeviceState>();
}
static void OnQueue(reshade::api::command_queue *q) {
    try {
        if (q->get_device()->get_api() == reshade::api::device_api::d3d12 && PinInspector())
            if (!AlbedoInspector().AttachQueue(
                    reinterpret_cast<ID3D12CommandQueue *>(q->get_native())))
                WriteLog("material array queue hooks unavailable");
    } catch (...) {
        SparseFailure("RR provenance/context exception; RR blocked");
        WriteLog("material array queue attachment failed");
    }
}
static void OnList(reshade::api::command_list *list) {
    try {
        if (list->get_device()->get_api() == reshade::api::device_api::d3d12 && PinInspector()) {
            auto *cl = reinterpret_cast<ID3D12GraphicsCommandList *>(list->get_native());
            if (!AlbedoInspector().AttachList(cl))
                WriteLog("material array list hooks unavailable");
            if (!ComputeState().Attach(cl))
                WriteLog("native compute state hooks unavailable");
        }
    } catch (...) {
        SparseFailure("RR provenance/context exception; RR blocked");
        WriteLog("material array list attachment failed");
    }
}
template <class F> static void WithBindings(reshade::api::device *d, F &&f) {
    auto *state = rrbindings::State(d);
    if (!state)
        return;
    try {
        if (state->tracker.Healthy())
            f(state->tracker);
    } catch (...) {
        state->tracker.Fail();
    }
    if (!state->tracker.Healthy() && !state->reported.exchange(true))
        WriteLog("binding tracker disabled after bounds/API/allocation failure; subsequent "
                 "bindings are unknown");
}
template <class F> static void WithGraphicsBindings(reshade::api::device *d, F &&f) {
    auto *state = rrbindings::State(d);
    if (!state)
        return;
    try {
        if (state->graphics.Healthy())
            f(state->graphics);
    } catch (...) {
        state->graphics.Fail();
    }
}
#include "rr_graphics_state.inl"
static void OnResetList(reshade::api::command_list *list) {
    try {
        AuditResetGraphics(list->get_native());
        ShaderDispatches().Reset(list->get_native());
    } catch (...) {
        SparseFailure("RR provenance/context exception; RR blocked");
        WriteLog("shader dispatch reset tracking failed");
    }
    WithGraphicsBindings(list->get_device(), [&](auto &t) { t.Reset(list->get_native()); });
    WithBindings(list->get_device(), [&](auto &t) { t.Reset(list->get_native()); });
}
static void OnDestroyList(reshade::api::command_list *list) {
    OnResetList(list);
    if (list->get_device()->get_api() == reshade::api::device_api::d3d12) {
        auto native = fg::NativeObject<ID3D12GraphicsCommandList>(
            reinterpret_cast<ID3D12GraphicsCommandList *>(list->get_native()));
        FrameReset(native.Get());
        ComputeState().Forget(native.Get());
        AlbedoInspector().Forget(reinterpret_cast<ID3D12GraphicsCommandList *>(list->get_native()));
    }
}
static void OnSecondary(reshade::api::command_list *caller, reshade::api::command_list *) {

    // A bundle can carry state changes back into its caller. We do not replay
    // secondary-list state, so require fresh caller bindings/pipeline afterwards.
    OnResetList(caller);
}
// The command signature hook reads root-constant counts from these layouts, so a
// recycled root signature address must never keep an earlier one, whatever the
// binding tracker's health.
static void ForgetRootLayout(reshade::api::pipeline_layout layout) noexcept {
    try {
        ComputeState().ForgetRoot(reinterpret_cast<ID3D12RootSignature *>(layout.handle));
    } catch (...) {
        WriteLog("root signature layout could not be forgotten");
    }
}
static void OnLayout(reshade::api::device *d, uint32_t count,
                     const reshade::api::pipeline_layout_param *params,
                     reshade::api::pipeline_layout layout) {
    ForgetRootLayout(layout);
    WithGraphicsBindings(d, [&](auto &t) { t.Layout(layout.handle, count, params); });
    WithBindings(d, [&](auto &t) {
        t.Layout(layout.handle, count, params);
        if (count > 64 || !params)
            return;
        std::array<rrcompute::Shape, 64> shape{};
        using namespace reshade::api;
        for (unsigned i = 0; i < count; ++i) {
            auto &s = shape[i];
            const auto &p = params[i];
            using K = rrcompute::Kind;
            // ReShade groups all static samplers into one trailing pseudo-parameter.
            // They have no command-list setter and are restored with the native root.
            if (p.type == pipeline_layout_param_type::push_descriptors_with_ranges_and_flags &&
                p.descriptor_table_with_flags.count && p.descriptor_table_with_flags.ranges) {
                bool all_static = true;
                for (unsigned j = 0; j < p.descriptor_table_with_flags.count; ++j) {
                    const auto &r = p.descriptor_table_with_flags.ranges[j];
                    all_static &= r.type == descriptor_type::sampler && r.count > 0 &&
                                  r.static_samplers != nullptr;
                }
                if (all_static) {
                    s.ignored = true;
                    continue;
                }
            }
            if (p.type == pipeline_layout_param_type::descriptor_table ||
                p.type == pipeline_layout_param_type::descriptor_table_with_flags)
                s.kind = K::Table;
            else if (p.type == pipeline_layout_param_type::push_constants) {
                s.kind = K::Constants;
                s.words = p.push_constants.count;
            } else {
                const descriptor_range *r = nullptr;
                if (p.type == pipeline_layout_param_type::push_descriptors)
                    r = &p.push_descriptors;
                else if (p.type ==
                             pipeline_layout_param_type::push_descriptors_with_ranges_and_flags &&
                         p.descriptor_table_with_flags.count == 1)
                    r = p.descriptor_table_with_flags.ranges;
                if (r && r->count == 1) {
                    s.shader_register = r->dx_register_index;
                    s.space = r->dx_register_space;
                    s.compute = uint32_t(r->visibility & shader_stage::compute) != 0;
                    s.flags = p.type == pipeline_layout_param_type::push_descriptors
                                  ? 2u
                                  : uint32_t(p.descriptor_table_with_flags.ranges[0].flags);
                    if (r->type == descriptor_type::constant_buffer)
                        s.kind = K::CBV;
                    else if (r->type == descriptor_type::shader_resource_view ||
                             r->type == descriptor_type::buffer_shader_resource_view ||
                             r->type == descriptor_type::acceleration_structure)
                        s.kind = K::SRV;
                    else if (r->type == descriptor_type::unordered_access_view ||
                             r->type == descriptor_type::buffer_unordered_access_view)
                        s.kind = K::UAV;
                }
            }
        }
        ComputeState().Define(reinterpret_cast<ID3D12RootSignature *>(layout.handle), count,
                              shape.data());
    });
}
static void OnDestroyLayout(reshade::api::device *d, reshade::api::pipeline_layout layout) {
    ForgetRootLayout(layout);
    WithGraphicsBindings(d, [&](auto &t) { t.DestroyLayout(layout.handle); });
    WithBindings(d, [&](auto &t) { t.DestroyLayout(layout.handle); });
}
static void OnTables(reshade::api::command_list *list, reshade::api::shader_stage stages,
                     reshade::api::pipeline_layout layout, uint32_t first, uint32_t count,
                     const reshade::api::descriptor_table *tables, uint32_t dynamic_count,
                     const uint32_t *) {
    WithGraphicsBindings(list->get_device(), [&](auto &t) {
        if (dynamic_count)
            t.Fail();
        else
            t.Bind(list->get_native(), stages, layout.handle, first, count, tables);
    });
    WithBindings(list->get_device(), [&](auto &t) {
        if (dynamic_count)
            t.Fail();
        else
            t.Bind(list->get_native(), stages, layout.handle, first, count, tables);
    });
}
static void OnPush(reshade::api::command_list *list, reshade::api::shader_stage stages,
                   reshade::api::pipeline_layout layout, uint32_t parameter,
                   const reshade::api::descriptor_table_update &update) {
    WithGraphicsBindings(list->get_device(), [&](auto &t) {
        t.Push(list->get_native(), stages, layout.handle, parameter, update);
    });
    WithBindings(list->get_device(), [&](auto &t) {
        t.Push(list->get_native(), stages, layout.handle, parameter, update);
    });
}
static bool OnUpdates(reshade::api::device *d, uint32_t count,
                      const reshade::api::descriptor_table_update *updates) {
    WithBindings(d, [&](auto &t) {
        if (count > 4096 || (count && !updates)) {
            t.Fail();
            return;
        }
        for (uint32_t n = 0; n < count && t.Healthy(); ++n) {
            const auto &u = updates[n];
            if (u.count > 4096 || (u.count && !u.descriptors)) {
                t.Fail();
                return;
            }
            auto slot = rrbindings::Resolve(d, u.table, u.binding, u.array_offset);
            if (!slot.heap || uint64_t(slot.offset) + u.count > uint64_t(UINT32_MAX) + 1) {
                t.Fail();
                return;
            }
            for (uint32_t i = 0; i < u.count; ++i) {
                rrbindings::Value value;
                using D = reshade::api::descriptor_type;
                if (u.type == D::texture_shader_resource_view ||
                    u.type == D::texture_unordered_access_view ||
                    u.type == D::shader_resource_view || u.type == D::unordered_access_view ||
                    u.type == D::buffer_shader_resource_view ||
                    u.type == D::buffer_unordered_access_view)
                    value = rrbindings::Describe(
                        d, static_cast<const reshade::api::resource_view *>(u.descriptors)[i]);
                else if (u.type == D::constant_buffer) {
                    const auto &b =
                        static_cast<const reshade::api::buffer_range *>(u.descriptors)[i];
                    value.known = b.buffer.handle && b.size;
                    value.buffer = true;
                    value.resource = b.buffer.handle;
                    value.byte_offset = b.offset;
                    value.byte_size = b.size;
                }
                t.Put({slot.heap, slot.offset + i}, value);
            }
        }
    });
    return false;
}
static bool OnCopies(reshade::api::device *d, uint32_t count,
                     const reshade::api::descriptor_table_copy *copies) {
    WithBindings(d, [&](auto &t) {
        if (count > 4096 || (count && !copies)) {
            t.Fail();
            return;
        }
        for (uint32_t i = 0; i < count && t.Healthy(); ++i) {
            auto &c = copies[i];
            t.Copy(rrbindings::Resolve(d, c.source_table, c.source_binding, c.source_array_offset),
                   rrbindings::Resolve(d, c.dest_table, c.dest_binding, c.dest_array_offset),
                   c.count);
        }
    });
    return false;
}
static std::mutex pipeline_mutex;
struct PipelineInfo {
    uint64_t hash{};
    bool compute{};
};
static std::unordered_map<uint64_t, PipelineInfo> shader_hashes;
static void OnPipeline(reshade::api::device *device, reshade::api::pipeline_layout layout,
                       uint32_t count, const reshade::api::pipeline_subobject *objects,
                       reshade::api::pipeline pipeline) {
    rrtrace::LastError preserve_error;
    SssPipelines().Initialize(device, layout, count, objects, pipeline);
    CoveragePipelines().Initialize(device, layout, count, objects, pipeline);
    try {
        uint64_t hash = 14695981039346656037ull;
        bool found = false, compute = false;
        for (uint32_t i = 0; i < count; ++i) {
            using T = reshade::api::pipeline_subobject_type;
            auto &o = objects[i];
            if (o.type != T::compute_shader && o.type != T::pixel_shader)
                continue;
            auto *shader = static_cast<const reshade::api::shader_desc *>(o.data);
            if (!shader || !shader->code || shader->code_size > 16 * 1024 * 1024)
                continue;
            found = true;
            hash = rrshader::Hash(shader->code, shader->code_size, hash);
            compute = compute || o.type == T::compute_shader;
        }
        if (found) {
            rrbypass::Initialize(device, layout, count, objects, pipeline, compute ? hash : 0);
            {
                std::lock_guard lock(pipeline_mutex);
                if (shader_hashes.size() < 65536)
                    shader_hashes[pipeline.handle] = {rrbypass::OriginalHash(hash), compute};
            }
        }
    } catch (...) {
        SparseFailure("RR provenance/context exception; RR blocked");
        WriteLog("buffer pipeline hash unavailable");
    }
}
static void OnDestroyPipeline(reshade::api::device *, reshade::api::pipeline pipeline) {
    SssPipelines().Forget(pipeline.handle);
    CoveragePipelines().Forget(pipeline.handle);
    rrbypass::Removed(pipeline);
    std::lock_guard lock(pipeline_mutex);
    shader_hashes.erase(pipeline.handle);
}
static void OnBind(reshade::api::command_list *list, reshade::api::pipeline_stage stage,
                   reshade::api::pipeline pipeline) {
    if (list->get_device()->get_api() != reshade::api::device_api::d3d12)
        return;
    try {
        const auto effective =
            pipeline; // Preserve the engine PSO; select private shaders only at dispatch.
        if (stage == reshade::api::pipeline_stage::all) {
            auto native = fg::NativeObject<ID3D12GraphicsCommandList>(
                reinterpret_cast<ID3D12GraphicsCommandList *>(list->get_native()));
            if (native)
                ComputeState().CreationPipeline(
                    native.Get(), reinterpret_cast<ID3D12PipelineState *>(effective.handle));
        }
        PipelineInfo info;
        {
            std::lock_guard lock(pipeline_mutex);
            auto i = shader_hashes.find(pipeline.handle);
            if (i != shader_hashes.end())
                info = i->second;
        }
        AuditBindGraphics(list, stage, pipeline.handle, info.hash, info.compute);
        ShaderDispatches().BindStages(list->get_native(), stage, pipeline.handle, info.hash,
                                      info.compute);
    } catch (...) {
        SparseFailure("RR provenance/context exception; RR blocked");
        WriteLog("pipeline binding unavailable");
    }
}
static bool SssOwnedDraw(reshade::api::command_list *list,
                         const rrsss::ProducerDraw &draw) noexcept {
    if (!LiveRR().Enabled() || !LiveRR().SssEnabled() ||
        list->get_device()->get_api() != reshade::api::device_api::d3d12)
        return false;
    try {
        AuditGraphicsState graphics;
        {
            std::lock_guard lock(audit_graphics_mutex);
            auto i = audit_graphics.find(list->get_native());
            if (i == audit_graphics.end())
                return false;
            graphics = i->second;
        }
        if (graphics.pipeline.shader != 0x9ee7cea57f70d741ull)
            return false;
        auto variant = SssPipelines().Find(list->get_device(), graphics.pipeline.pipeline);
        if (!variant)
            return false;
        auto native = fg::NativeObject<ID3D12GraphicsCommandList>(
            reinterpret_cast<ID3D12GraphicsCommandList *>(list->get_native()));
        auto original = fg::NativeObject<ID3D12PipelineState>(
            reinterpret_cast<ID3D12PipelineState *>(graphics.pipeline.pipeline));
        if (!native || !original)
            return false;
        auto state = ComputeState().CaptureGraphicsOutputs(native.Get(), original.Get());
        const auto stencil =
            SssPipelines().FindStencil(list->get_device(), graphics.pipeline.pipeline);
        return LiveRR().OwnedSssDraw(native.Get(), variant.Get(), state, draw, stencil);
    } catch (...) {
        return false;
    }
}
#include "rr_glass_render.inl"
#include "rr_blend_render.inl"
static bool AuditDraw(reshade::api::command_list *l, uint32_t c, uint32_t i, uint32_t f,
                      uint32_t fi) {
    GlassDraw(l, false, c, i);
    CoverageDraw(l, {false, c, i, f, 0, fi});
    return SssOwnedDraw(l, {false, c, i, f, 0, fi});
}
static bool AuditDrawIndexed(reshade::api::command_list *l, uint32_t c, uint32_t i, uint32_t f,
                             int32_t b, uint32_t fi) {
    GlassDraw(l, true, c, i);
    CoverageDraw(l, {true, c, i, f, b, fi});
    return SssOwnedDraw(l, {true, c, i, f, b, fi});
}
#include "rr_sparse_render.h"
#include "rr_vrrt_render.h"
static bool PreservePrepass(reshade::api::command_list *list, uint32_t x, uint32_t y, uint32_t z) {
    const auto binding = ShaderDispatches().Get(list->get_native());
    if (list->get_device()->get_api() != reshade::api::device_api::d3d12 ||
        binding.shader != 0xc8be52d1ee156111ull)
        return false;
    auto *state = rrbindings::State(list->get_device());
    if (!state || !state->tracker.Healthy())
        return false;
    std::array<rrbindings::Value, 4> values{};
    std::array<rrbindings::Value, 2> cb{};
    const rrbindings::Register regs[] = {{false, 16}, {false, 17}, {true, 17}, {true, 3}};
    for (unsigned i = 0; i < 4; ++i) {
        auto loc = state->tracker.Find(list->get_native(), regs[i]);
        if (loc.known) {
            auto slot = rrbindings::Resolve(list->get_device(), {loc.table}, loc.offset);
            if (slot.heap)
                values[i] = state->tracker.Get(slot);
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        const unsigned reg = i == 0 ? 0 : 3;
        cb[i] = state->tracker.Constant(list->get_native(), reg);
        if (!cb[i].known) {
            auto loc = state->tracker.FindConstant(list->get_native(), reg);
            if (loc.known) {
                auto slot = rrbindings::Resolve(list->get_device(), {loc.table}, loc.offset);
                if (slot.heap)
                    cb[i] = state->tracker.Get(slot);
            }
        }
    }
    auto native = fg::NativeObject<ID3D12GraphicsCommandList>(
        reinterpret_cast<ID3D12GraphicsCommandList *>(list->get_native()));
    if (!native)
        return false;
    const bool groups = values[3].known && x == (values[3].width + 7) / 8 &&
                        y == (values[3].height + 7) / 8 && z == 1;
    rrcompute::Snapshot restore;
    std::array<bool, 2> matches{};
    if (LiveRR().Enabled()) {
        restore = ComputeState().Capture(native.Get(),
                                         reinterpret_cast<ID3D12PipelineState *>(binding.pipeline));
        for (unsigned i = 0; i < 2; ++i)
            matches[i] = rrinputs::NativeConstantMatches(state->tracker, list->get_native(),
                                                         i == 0 ? 0 : 3, cb[i], restore);
    }
    const bool live = LiveRR().Enabled(), safe = live && AlbedoInspector().SafeCopies(native.Get()),
               coverage = live && AlbedoInspector().StateCoverage(native.Get()),
               albedos = live && rrmaterial::ValidAlbedoPair(values[0], values[1]);
    if (live && groups && restore.valid && restore.heap_restore_valid && matches[0] && matches[1] &&
        safe && coverage && albedos) {
        bool valid = true;
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 4> textures;
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> constants;
        std::array<UINT64, 2> offsets{};
        for (unsigned i = 0; i < 4; ++i) {
            auto loc = state->tracker.Find(list->get_native(), regs[i]);
            valid = valid && loc.known && loc.parameter < restore.args.size() &&
                    restore.args[loc.parameter].kind == rrcompute::Kind::Table &&
                    restore.args[loc.parameter].address == loc.table &&
                    state->tracker.CurrentLayout(list->get_native()) == UINT64(restore.root.Get());
            auto &v = values[i];
            textures[i] = fg::NativeObject<ID3D12Resource>((ID3D12Resource *)v.resource);
            if (!v.known || !textures[i]) {
                valid = false;
                break;
            }
            auto d = textures[i]->GetDesc();
            valid = valid && rrlive::GuideBindingMatches(v, d, i);
            if (i >= 2) {
                valid = valid && (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                D3D12_RESOURCE_STATES observed{};
                if (SparseLayouts().Get(native.Get(), textures[i].Get(), observed) &&
                    observed != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                    valid = false;
            }
        }
        valid = valid && values[2].width == values[0].width &&
                values[2].height == values[0].height && values[3].width == 2 * values[0].width &&
                values[3].height == 2 * values[0].height;
        for (unsigned i = 0; i < 2; ++i) {
            constants[i] = fg::NativeObject<ID3D12Resource>((ID3D12Resource *)cb[i].resource);
            offsets[i] = cb[i].byte_offset;
            if (!constants[i]) {
                valid = false;
                break;
            }
            auto d = constants[i]->GetDesc();
            const UINT64 need = i ? 12416 : 128;
            valid = valid && d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
                    offsets[i] % 256 == 0 && offsets[i] <= d.Width &&
                    need <= d.Width - offsets[i] && cb[i].byte_size >= need;
        }
        if (valid && LiveRR().Prepass(native.Get(), textures, constants, offsets, restore, x, y, z))
            return true;
    }
    return false;
}
static rrspecular::Diagnostics &DistanceStats() {
    static auto *p = new rrspecular::Diagnostics;
    return *p;
}
// Copies the native specular hit distance read by cs_ray_hit_distance_blur.
// Only the used tables are proven, as for the sparse copies: a full restore
// snapshot would wrongly require unused roots after ExecuteIndirect. Another
// shader with the same bindings is only logged, once, so a game update that
// changes the blur's hash can be recognized without copying unknown data.
static void PreserveSpecularDistance(reshade::api::command_list *list, uint32_t x, uint32_t y,
                                     uint32_t z) {
    if (list->get_device()->get_api() != reshade::api::device_api::d3d12 ||
        !LiveRR().Enabled() || !LiveRR().SpecularHitEnabled())
        return;
    const auto binding = ShaderDispatches().Get(list->get_native());
    if (!binding.shader)
        return;
    const bool known = rrspecular::DistanceBlur(binding.shader);
    auto native = fg::NativeObject<ID3D12GraphicsCommandList>(
        reinterpret_cast<ID3D12GraphicsCommandList *>(list->get_native()));
    if (!native)
        return;
    using S = rrspecular::Stage;
    auto &stats = DistanceStats();
    if (known)
        stats.Dispatch();
    // Unknown shaders are examined once each, and only inside a live prepass recording.
    // The native spatial denoiser also binds an R16 t10 and the prepass t13; skip it.
    else if (rrbypass::Spatial == rrversion::Canonical(binding.shader) ||
             !LiveRR().CanRaw(native.Get()) || !stats.Scan(binding.shader))
        return;
    auto reject = [&](S stage) {
        if (known)
            stats.Observe(stage);
    };
    auto *state = rrbindings::State(list->get_device());
    if (!state || !state->tracker.Healthy() || !AlbedoInspector().SafeCopies(native.Get()) ||
        !AlbedoInspector().StateCoverage(native.Get()))
        return reject(S::Context);
    const auto root = state->tracker.CurrentLayout(list->get_native());
    if (!root)
        return reject(S::Context);
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> resources;
    std::array<rrbindings::Value, 2> values;
    for (unsigned i = 0; i < 2; ++i) {
        auto loc = state->tracker.Find(list->get_native(), {false, i ? 13u : 10u});
        if (!loc.known ||
            !ComputeState().TableBound(native.Get(),
                                       reinterpret_cast<ID3D12PipelineState *>(binding.pipeline),
                                       reinterpret_cast<ID3D12RootSignature *>(root),
                                       loc.parameter, loc.table))
            return reject(S::Table);
        auto slot = rrbindings::Resolve(list->get_device(), {loc.table}, loc.offset);
        if (!slot.heap)
            return reject(S::Table);
        auto &v = values[i] = state->tracker.Get(slot);
        if (!v.known || v.buffer || !v.resource ||
            v.view_type != unsigned(reshade::api::resource_view_type::texture_2d) ||
            v.first_level || v.levels != 1 || v.first_layer || v.layers > 1)
            return reject(S::View);
        resources[i] =
            fg::NativeObject<ID3D12Resource>(reinterpret_cast<ID3D12Resource *>(v.resource));
        if (!resources[i])
            return reject(S::View);
    }
    const auto &distance = values[0], &surface = values[1];
    if (distance.format != unsigned(DXGI_FORMAT_R16_FLOAT) ||
        distance.view_format != unsigned(DXGI_FORMAT_R16_FLOAT) ||
        surface.view_format != unsigned(DXGI_FORMAT_R32G32B32A32_UINT) ||
        distance.width != surface.width || distance.height != surface.height ||
        !rrspecular::DispatchShape(distance.width, distance.height, x, y, z))
        return reject(S::Shape);
    if (!known) {
        if (LiveRR().PrepassSurface(native.Get(), resources[1].Get()) &&
            stats.Candidate(binding.shader)) {
            char line[192];
            snprintf(line, sizeof(line),
                     "Specular hit distance: unrecognized blur candidate shader %016llx "
                     "(t10 R16 %llux%u, t13 prepass surface); not used",
                     static_cast<unsigned long long>(binding.shader),
                     static_cast<unsigned long long>(distance.width), distance.height);
            WriteLog(line);
        }
        return;
    }
    D3D12_RESOURCE_STATES source_state{};
    if (!SparseLayouts().Get(native.Get(), resources[0].Get(), source_state))
        return reject(S::Layout);
    reject(LiveRR().SpecularDistance(native.Get(), resources[1].Get(), resources[0].Get(),
                                     source_state)
               ? S::Copied
               : S::Slot);
}
static void RestoreLightingPipeline(reshade::api::command_list *list) {
    const auto b = ShaderDispatches().Get(list->get_native());
    if (!rrbypass::Target(b.shader) || !b.pipeline)
        return;
    auto native = fg::NativeObject<ID3D12GraphicsCommandList>(
        (ID3D12GraphicsCommandList *)list->get_native());
    if (native)
        native->SetPipelineState((ID3D12PipelineState *)b.pipeline);
}
static bool OnDispatch(reshade::api::command_list *list, uint32_t x, uint32_t y, uint32_t z) {
    if (rrinternal::active)
        return false;
    try {
        RestoreLightingPipeline(list);
        try {
            PreserveSpecularDistance(list, x, y, z);
        } catch (...) {
            static std::atomic<bool> warned{};
            if (!warned.exchange(true))
                WriteLog("Specular hit distance validation failed; RR continues without it");
        }
        const bool live_prepass = PreservePrepass(list, x, y, z);
        if (live_prepass)
            return true;
        const auto b = ShaderDispatches().Get(list->get_native());
        auto *device = list->get_device();
        auto native = fg::NativeObject<ID3D12GraphicsCommandList>(
            (ID3D12GraphicsCommandList *)list->get_native());
        if (!native || !LiveRR().CanRaw(native.Get()) || rrbypass::Health(device).failed)
            return false;
        if (b.shader == rrbypass::Vrrt) {
            if (!PreserveNativeSampling(list, x, y, z))
                return false;
            rrbypass::Used(device, b.pipeline, b.shader);
            return false;
        }
        if (!LiveRR().SamplingReady(native.Get()))
            return false;
        if (rrbypass::Target(b.shader))
            rrbypass::Used(device, b.pipeline, b.shader);
        const bool corrected = SparseDispatch(list, x, y, z);
        if (corrected) {
            const auto health = rrbypass::Health(device);
            LiveRR().RawCorrected(native.Get(), UINT64(device), health.revision);
            return true;
        }
        // Only later spatial passes in a successfully corrected recording use
        // passthrough, unless the diagnostic explicitly retains those filters.
        // Missing guides, proof failure and OFF all execute native.
        if (b.shader == rrbypass::Spatial && !rrbypass::Health(device).failed &&
            LiveRR().RawSpatialPassthrough(native.Get()))
            rrbypass::Bind(list, reshade::api::pipeline_stage::all_compute, {b.pipeline});
    } catch (...) {
        RestoreLightingPipeline(list);
        SparseFailure("Dispatch failed; RR disabled");
    }
    return false;
}
static bool OnIndirect(reshade::api::command_list *l, reshade::api::indirect_command t,
                       reshade::api::resource, uint64_t, uint32_t, uint32_t) {
    RestoreLightingPipeline(l);
    if (t == reshade::api::indirect_command::unknown || t == reshade::api::indirect_command::draw ||
        t == reshade::api::indirect_command::draw_indexed)
        OitSceneDraw(l);
    CoverageIndirectArm(l, t);
    WithGraphicsBindings(l->get_device(), [&](auto &b) { b.Reset(l->get_native()); });
    WithBindings(l->get_device(), [&](auto &b) { b.Reset(l->get_native()); });
    return false;
}
// The campaign settings file: read-only flag and text, for the RR gate and the config report.
static rrgameconfig::File ReadGameConfig() {
    rrgameconfig::File file;
    wchar_t local[32768]{};
    auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
    if (!size || size >= 32768)
        return file;
    const auto path =
        std::filesystem::path(local) / L"Activision/Call of Duty/players/s.1.0.cod26.txt";
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return file;
    file.readonly = (attributes & FILE_ATTRIBUTE_READONLY) != 0;
    file.state = rrgameconfig::FileState::Unreadable;
    std::ifstream input(path);
    if (!input)
        return file;
    std::ostringstream text;
    text << input.rdbuf();
    file.text = text.str();
    file.state = rrgameconfig::FileState::Read;
    return file;
}
static bool VrsGateAllows(const rrgameconfig::File &file) {
    return file.state == rrgameconfig::FileState::Read &&
           rrvrsconfig::Allowed(file.text, file.readonly, true);
}
static bool VrsConfigAllowsRR() { return VrsGateAllows(ReadGameConfig()); }
// Once per session, from LoadSettings, with the file the RR gate read and its answer. Logs and
// counts only: RR is never blocked by it.
static unsigned ReportGameConfig(const rrgameconfig::File &file, bool gate_allows) {
    try {
        std::vector<std::string> lines;
        const auto warnings = rrgameconfig::Report(file, gate_allows, lines);
        for (const auto &line : lines)
            WriteLog(line.c_str());
        return warnings;
    } catch (...) {
        WriteLog("Game config check failed; RR is not affected");
        return 0;
    }
}
#include "rr_settings.inl"
static bool RegisterRuntimeCallbacks(HMODULE module) {
    if (!reshade::register_addon(module))
        return FALSE;
    reshade::register_overlay("MW4 Ray Reconstruction", &DrawSettings);
    rrbypass::logger = &WriteLog;
    reshade::register_event<reshade::addon_event::init_device>(&OnDevice);
    reshade::register_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
    reshade::register_event<reshade::addon_event::init_command_queue>(&OnQueue);
    reshade::register_event<reshade::addon_event::init_command_list>(&OnList);
    reshade::register_event<reshade::addon_event::init_pipeline_layout>(&OnLayout);
    reshade::register_event<reshade::addon_event::destroy_pipeline_layout>(&OnDestroyLayout);
    reshade::register_event<reshade::addon_event::bind_descriptor_tables>(&OnTables);
    reshade::register_event<reshade::addon_event::push_descriptors>(&OnPush);
    reshade::register_event<reshade::addon_event::update_descriptor_tables>(&OnUpdates);
    reshade::register_event<reshade::addon_event::copy_descriptor_tables>(&OnCopies);
    reshade::register_event<reshade::addon_event::destroy_command_list>(&OnDestroyList);
    reshade::register_event<reshade::addon_event::reset_command_list>(&OnResetList);
    reshade::register_event<reshade::addon_event::execute_secondary_command_list>(&OnSecondary);
    reshade::register_event<reshade::addon_event::init_pipeline>(&OnPipeline);
    reshade::register_event<reshade::addon_event::destroy_pipeline>(&OnDestroyPipeline);
    reshade::register_event<reshade::addon_event::bind_pipeline>(&OnBind);
    reshade::register_event<reshade::addon_event::dispatch>(&OnDispatch);
    reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(&OnIndirect);
    reshade::register_event<reshade::addon_event::begin_render_pass>(&AuditBeginRenderPass);
    reshade::register_event<reshade::addon_event::end_render_pass>(&AuditEndRenderPass);
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(
        &AuditRenderTargets);
    reshade::register_event<reshade::addon_event::draw>(&AuditDraw);
    reshade::register_event<reshade::addon_event::draw_indexed>(&AuditDrawIndexed);
    reshade::register_event<reshade::addon_event::init_effect_runtime>(&OnRuntime);
    reshade::register_event<reshade::addon_event::reshade_present>(&OnPresent);
    return true;
}
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        wchar_t process[MAX_PATH]{};
        GetModuleFileNameW(nullptr, process, MAX_PATH);
        auto *leaf = wcsrchr(process, L'\\');
        if (_wcsicmp(leaf ? leaf + 1 : process, L"sp26-cod.exe") != 0)
            return FALSE;
        return RegisterRuntimeCallbacks(module);
    }
    if (reason == DLL_PROCESS_DETACH)
        reshade::unregister_addon(module);
    return TRUE;
}
