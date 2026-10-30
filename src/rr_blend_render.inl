// Render glue of the transparency albedo blend: the coverage captures of the
// OIT resolve (ExecuteIndirect) and of the direct composite draws, and the
// hidden tuning keys. Included in mw4_rr.cpp after rr_glass_render.inl.
// Nothing here suppresses or changes a native call: a capture is recorded next
// to it, binds only the add-on's coverage target with the variant PSO, and
// restores the native PSO and outputs. Not gated on the glass toggle.
#include "rr_blend_arm.h"

// What the ReShade event proved about the bound composite. Held from the proof
// until the capture is recorded: the snapshot keeps the native PSO and output
// mirror, the entry keeps the variant.
struct CoverageArmState {
    rrsss::GraphicsOutputSnapshot snapshot;
    std::shared_ptr<const rrblend::CoveragePipelineRegistry::Entry> entry;
    Microsoft::WRL::ComPtr<ID3D12Resource> scene; // the native render target
    rrblend::Kind kind{};
};
static rrblend::ArmLatch<CoverageArmState> &CoverageArm() noexcept {
    static thread_local rrblend::ArmLatch<CoverageArmState> arm;
    return arm;
}
// One log line per path and reason for the life of the process, so a refused
// capture is visible without repeating every frame.
enum class CoveragePath : unsigned { Resolve, OitDraw, RefractionDraw, Count };
enum class CoverageNote : unsigned {
    NoVariant,
    Context,
    Outputs,
    Query,
    Depth,
    Target,
    View,
    Scene,
    Area,
    List,
    State,
    Unknown,
    Partial,
    Unsupported,
    Replay,
    Exception,
    Count
};
static void CoverageLog(CoveragePath path, CoverageNote note, const char *why,
                        const char *detail = nullptr) noexcept {
    static std::atomic<uint64_t> logged{};
    static_assert(unsigned(CoverageNote::Count) * unsigned(CoveragePath::Count) <= 64);
    const uint64_t bit = 1ull << (unsigned(note) + unsigned(path) * unsigned(CoverageNote::Count));
    if (logged.fetch_or(bit) & bit)
        return;
    static constexpr const char *names[] = {"OIT resolve", "OIT draw", "refraction draw"};
    try {
        char line[320];
        snprintf(line, sizeof(line), "Transparency albedo blend: %s not captured: %s%s%s%s",
                 names[unsigned(path)], why, detail ? " (" : "", detail ? detail : "",
                 detail ? ")" : "");
        WriteLog(line);
    } catch (...) {
    }
}
// The proofs both capture paths need before anything is recorded: the bound
// PSO is a native composite with a variant (never a bound variant: the
// registry finds natives only), the list's outputs are mirrored with one
// render target bound as ReShade reported it, and the viewport and scissor
// cover that whole target. indirect: only the OIT resolve qualifies.
static bool CoverageProven(reshade::api::command_list *list, bool indirect,
                           Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> &native,
                           CoverageArmState &out) {
    AuditGraphicsState g;
    {
        std::lock_guard lock(audit_graphics_mutex);
        auto i = audit_graphics.find(list->get_native());
        if (i == audit_graphics.end())
            return false;
        g = i->second;
    }
    // The hash table holds no shader size: the registry entry, which proved
    // hash and size at creation, completes the identity.
    const rrblend::Target *target = rrblend::Recognized().FindHash(g.pipeline.shader);
    if (target && indirect && target->kind != rrblend::Kind::Oit)
        target = nullptr;
    if (!target || !g.pipeline.pipeline || !LiveRR().TransparencyBlendEnabled())
        return false;
    const auto path = indirect                             ? CoveragePath::Resolve
                      : target->kind == rrblend::Kind::Oit ? CoveragePath::OitDraw
                                                           : CoveragePath::RefractionDraw;
    auto entry = CoveragePipelines().Find(list->get_device(), g.pipeline.pipeline);
    if (!entry || entry->kind != target->kind || !entry->variant ||
        entry->depth == rrblend::DepthClass::Refuse) {
        CoverageLog(path, CoverageNote::NoVariant, "no variant PSO for the bound pipeline");
        return false;
    }
    native = fg::NativeObject<ID3D12GraphicsCommandList>(
        reinterpret_cast<ID3D12GraphicsCommandList *>(list->get_native()));
    if (!native || !AlbedoInspector().SafeCopies(native.Get()) ||
        !AlbedoInspector().StateCoverage(native.Get())) {
        CoverageLog(path, CoverageNote::Context, "unsafe or uncovered list");
        return false;
    }
    auto snapshot = ComputeState().CaptureGraphicsOutputs(
        native.Get(), reinterpret_cast<ID3D12PipelineState *>(g.pipeline.pipeline));
    if (!snapshot.valid || !snapshot.pipeline || !snapshot.outputs || !snapshot.outputs->valid ||
        snapshot.outputs->count != 1 || !snapshot.OriginalTarget().ptr ||
        snapshot.command != native.Get() || snapshot.generation != snapshot.proof.generation ||
        !snapshot.proof.Available()) {
        CoverageLog(path, CoverageNote::Outputs, "native outputs not proven", snapshot.status);
        return false;
    }
    if (!rrblend::QueriesClear(snapshot.proof)) {
        CoverageLog(path, CoverageNote::Query, "a counting query is active or unproven on the list");
        return false;
    }
    if (entry->depth == rrblend::DepthClass::KeepDsv && !snapshot.DepthTarget().ptr) {
        CoverageLog(path, CoverageNote::Depth, "the variant keeps a depth target the list lacks");
        return false;
    }
    if (!g.targets_known || g.count != 1 || !g.targets[0].known ||
        !ComputeState().GraphicsTargetBound(native.Get(), g.target_handle, g.target_serial)) {
        CoverageLog(path, CoverageNote::Target, "render target not proven bound");
        return false;
    }
    const auto &view = g.targets[0];
    if (view.first_level || view.first_layer ||
        (view.view_type != unsigned(reshade::api::resource_view_type::texture_2d_multisample) &&
         view.view_type != unsigned(reshade::api::resource_view_type::texture_2d))) {
        CoverageLog(path, CoverageNote::View, "render target view unsupported");
        return false;
    }
    auto scene =
        fg::NativeObject<ID3D12Resource>(reinterpret_cast<ID3D12Resource *>(view.resource));
    if (!scene) {
        CoverageLog(path, CoverageNote::Scene, "render target identity unavailable");
        return false;
    }
    const auto d = scene->GetDesc();
    const auto &a = snapshot.proof.viewports[0];
    const auto &r = snapshot.proof.scissors[0];
    if (a.TopLeftX != 0 || a.TopLeftY != 0 || a.Width != float(d.Width) ||
        a.Height != float(d.Height) || r.left != 0 || r.top != 0 || r.right != LONG(d.Width) ||
        r.bottom != LONG(d.Height)) {
        CoverageLog(path, CoverageNote::Area, "viewport or scissor is not the whole target");
        return false;
    }
    out.snapshot = std::move(snapshot);
    out.entry = std::move(entry);
    out.scene = std::move(scene);
    out.kind = target->kind;
    return true;
}
// The capture sequence on cl, under the controller's ticket: bracket the
// coverage target, bind the variant and the owned target (and the mirrored
// native DSV for KeepDsv), issue, then restore the native PSO and outputs.
// Restore runs whenever the list was touched; the token ends in
// CoverageCaptured or, after any failure, CoverageFailed.
template <class Issue>
static void CoverageRecord(ID3D12GraphicsCommandList *cl, const CoverageArmState &arm,
                           rrlive::Controller::CoverageToken &&token, Issue &&issue) {
    const auto depth = arm.snapshot.DepthTarget();
    const bool keep = arm.entry->depth == rrblend::DepthClass::KeepDsv;
    bool begun = false, issued = false;
    rrsss::DrawResult result;
    {
        rrinternal::Scope copies;
        rrcompute::Tracker::Internal state;
        result = rrsss::DrawOnce([] {},
                                 [&] {
                                     token.gpu->BeginCapture(cl, token.first);
                                     begun = true;
                                     cl->SetPipelineState(arm.entry->variant.Get());
                                     cl->OMSetRenderTargets(1, &token.rtv, FALSE,
                                                            keep ? &depth : nullptr);
                                 },
                                 [&] { issued = issue(); },
                                 [&] {
                                     arm.snapshot.Restore(cl);
                                     if (begun)
                                         token.gpu->EndCapture(cl);
                                 });
    }
    if (result.failed || !issued)
        LiveRR().CoverageFailed(std::move(token), result.failed ? "capture recording exception"
                                                                : "capture replay refused");
    else
        LiveRR().CoverageCaptured(std::move(token), arm.kind,
                                  arm.kind == rrblend::Kind::Refraction &&
                                      arm.entry->refraction_alpha);
}
// The ReShade indirect event, before the native ExecuteIndirect. Arms the
// capture when the OIT resolve is bound and proven; any other call disarms.
// Never suppresses the call.
static void CoverageIndirectArm(reshade::api::command_list *list,
                                reshade::api::indirect_command type) noexcept {
    auto &arm = CoverageArm();
    arm.Clear();
    try {
        if ((type != reshade::api::indirect_command::unknown &&
             type != reshade::api::indirect_command::draw &&
             type != reshade::api::indirect_command::draw_indexed) ||
            list->get_device()->get_api() != reshade::api::device_api::d3d12)
            return;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> native;
        CoverageArmState state;
        if (!CoverageProven(list, true, native, state))
            return;
        arm.Set(native.Get(), std::move(state));
    } catch (...) {
        arm.Clear();
        CoverageLog(CoveragePath::Resolve, CoverageNote::Exception, "proof exception");
    }
}
// Tracker after_indirect observer: the native ExecuteIndirect was forwarded on
// call.cl. Replays it once into the coverage target when this thread's arm is
// for this list and the PSO and outputs are still the ones the arm proved.
static void CoverageAfterIndirect(const rrcompute::IndirectCall &call) noexcept {
    try {
        const auto path = CoveragePath::Resolve;
        CoverageArmState arm;
        const bool armed = CoverageArm().Armed();
        if (!CoverageArm().Take(call.cl, arm)) {
            // Armed for another list: an unexpected hooked layer between the
            // ReShade event and this detour. Nothing is replayed.
            if (armed)
                CoverageLog(path, CoverageNote::List, "the call ran on another list");
            return;
        }
        const auto cls = rrsig::Read(call.signature);
        if (cls != rrblend::SignatureClass::Replayable) {
            LiveRR().CoverageSignatureRefused();
            const bool partial = cls == rrblend::SignatureClass::PartialConstants;
            CoverageLog(path,
                        partial                                   ? CoverageNote::Partial
                        : cls == rrblend::SignatureClass::Unknown ? CoverageNote::Unknown
                                                                  : CoverageNote::Unsupported,
                        "command signature not replayable",
                        partial                                   ? "partial root constants"
                        : cls == rrblend::SignatureClass::Unknown ? "not observed"
                                                                  : "unsupported arguments");
            return;
        }
        // The native call changes no PSO or output: the same mirror, at the
        // same generation, proves nothing was recorded between event and call.
        const auto now =
            ComputeState().CaptureGraphicsOutputs(call.cl, arm.snapshot.pipeline.Get());
        if (!now.valid || now.outputs != arm.snapshot.outputs ||
            now.generation != arm.snapshot.generation || now.pipeline != arm.snapshot.pipeline) {
            CoverageLog(path, CoverageNote::State, "state changed between event and call",
                        now.status);
            return;
        }
        auto token = LiveRR().CoverageCapture(call.cl, arm.scene.Get(), arm.entry->sample);
        if (!token)
            return;
        CoverageRecord(call.cl, arm, std::move(token), [&] {
            const bool replayed = call.replay && call.replay(call);
            if (!replayed)
                CoverageLog(path, CoverageNote::Replay, "replay refused");
            return replayed;
        });
    } catch (...) {
        CoverageLog(CoveragePath::Resolve, CoverageNote::Exception, "observer exception");
    }
}
// The ReShade draw event, before the native draw: a full-screen composite
// (OIT resolve or refraction) is drawn into the coverage target first. Its PS
// has no UAVs and the capture binds only owned targets, so the order is
// equivalent. Never suppresses the draw.
static void CoverageDraw(reshade::api::command_list *list,
                         const rrsss::ProducerDraw &draw) noexcept {
    auto path = CoveragePath::RefractionDraw;
    try {
        if (draw.count != 3 || draw.instances != 1 ||
            list->get_device()->get_api() != reshade::api::device_api::d3d12)
            return;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> native;
        CoverageArmState arm;
        if (!CoverageProven(list, false, native, arm))
            return;
        path = arm.kind == rrblend::Kind::Oit ? CoveragePath::OitDraw
                                              : CoveragePath::RefractionDraw;
        auto token = LiveRR().CoverageCapture(native.Get(), arm.scene.Get(), arm.entry->sample);
        if (!token)
            return;
        auto *cl = native.Get();
        CoverageRecord(cl, arm, std::move(token), [&] {
            draw.Issue(cl);
            return true;
        });
    } catch (...) {
        CoverageLog(path, CoverageNote::Exception, "draw exception");
    }
}
// Hidden tuning keys, read once with the other settings and never written.
// Missing or unparsable keys keep their defaults; values are clamped.
static void LoadBlendTuning(const char *section) noexcept {
    try {
        rrblend::Tuning t;
        reshade::get_config_value(nullptr, section, "TransparencyBlendDiffuse", t.diffuse);
        reshade::get_config_value(nullptr, section, "TransparencyBlendSpecular", t.specular);
        reshade::get_config_value(nullptr, section, "TransparencyBlendRoughness", t.roughness);
        reshade::get_config_value(nullptr, section, "TransparencyBlendStrength", t.strength);
        reshade::get_config_value(nullptr, section, "TransparencyBlendSky", t.sky);
        reshade::get_config_value(nullptr, section, "TransparencyBlendFilter", t.filter);
        t = rrblend::Clamp(t);
        LiveRR().SetTransparencyBlendTuning(t);
        const rrblend::Tuning d;
        if (t.diffuse != d.diffuse || t.specular != d.specular || t.roughness != d.roughness ||
            t.strength != d.strength || t.sky != d.sky || t.filter != d.filter) {
            char line[256];
            snprintf(line, sizeof(line),
                     "Transparency albedo blend tuning: diffuse=%.3f specular=%.3f roughness=%.3f "
                     "strength=%.3f sky=%d filter=%d",
                     t.diffuse, t.specular, t.roughness, t.strength, int(t.sky), int(t.filter));
            WriteLog(line);
        }
    } catch (...) {
        WriteLog("Transparency albedo blend tuning keys unreadable; defaults kept");
    }
}
