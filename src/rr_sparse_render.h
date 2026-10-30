#pragma once
// Included after observer, bindings and audit helpers.
// Latest SSS-masked preparation outcome for the overlay; changes are logged.
static std::atomic<unsigned> sss_mask_state{}, sss_mask_reports{};
static std::atomic<ULONGLONG> sss_mask_time{};
static void ReportSssMask(rrsssmask::State state, const char *detail = nullptr) {
    sss_mask_time = GetTickCount64();
    if (sss_mask_state.exchange(unsigned(state)) == unsigned(state) || sss_mask_reports++ >= 16)
        return;
    char line[256];
    snprintf(line, sizeof(line), "SSS-masked lighting input: %s%s%s", rrsssmask::Label(state),
             detail ? "; " : "", detail ? detail : "");
    WriteLog(line);
}
static rrsssmask::State SssMaskState() {
    return GetTickCount64() - sss_mask_time.load() <= 1000 ? rrsssmask::State(sss_mask_state.load())
                                                           : rrsssmask::State::Idle;
}
// The recognized native cs_spatial_denoiser writes BOTH u0/u1.
// Replacing that dispatch inherits its UAV-state precondition, even when the
// app prepared the outputs on another command list. Binding alone is not used
// as proof: exact pipeline provenance, root/table, views and shapes are required.
// https://learn.microsoft.com/windows/win32/api/d3d12/ne-d3d12-d3d12_resource_states
static bool SparsePair(reshade::api::command_list *list, ID3D12GraphicsCommandList *native,
                       uint64_t effective, bool uav, unsigned first, rrsparsecopy::Pair &out,
                       bool nativeSpatialWrites = false) {
    auto *state = rrbindings::State(list->get_device());
    if (!state || !state->tracker.Healthy())
        return false;
    auto root = state->tracker.CurrentLayout(list->get_native());
    const auto binding = ShaderDispatches().Get(list->get_native());
    const bool contract =
        nativeSpatialWrites && uav && first == 0 && binding.shader == rrbypass::Spatial &&
        rrbypass::SpatialWriteContract(list->get_device(), binding.pipeline, effective);
    for (unsigned i = 0; i < 2; ++i) {
        rrbindings::Value v;
        auto loc = state->tracker.Find(list->get_native(), {uav, first + i});
        bool layoutKnown = false;
        D3D12_RESOURCE_STATES observed{};
        if (!loc.known ||
            !ComputeState().TableBound(native, (ID3D12PipelineState *)effective,
                                       (ID3D12RootSignature *)root, loc.parameter, loc.table))
            return false;
        auto slot = rrbindings::Resolve(list->get_device(), {loc.table}, loc.offset);
        if (!slot.heap)
            return false;
        v = state->tracker.Get(slot);
        if (!v.known || v.buffer || !v.resource ||
            v.view_type != unsigned(reshade::api::resource_view_type::texture_2d) ||
            v.first_level || v.levels != 1 || v.first_layer || v.layers > 1)
            return false;
        if (i == 0 ? (v.view_format != 2 && v.view_format != 3)
                   : (v.view_format != 41 && v.view_format != 42))
            return false;
        out.r[i] = fg::NativeObject<ID3D12Resource>((ID3D12Resource *)v.resource);
        if (!out.r[i])
            return false;
        const auto desc = out.r[i]->GetDesc();
        if (v.width != desc.Width || v.height != desc.Height || v.format != unsigned(desc.Format) ||
            v.view_format != unsigned(desc.Format))
            return false;
        layoutKnown = SparseLayouts().Get(native, out.r[i].Get(), observed);
        if (contract) {
            if (!(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
                return false;
            if (layoutKnown && observed != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                return false;
            out.state[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        } else {
            if (!layoutKnown)
                return false;
            out.state[i] = observed;
        }
    }
    return rrsparsecopy::Shape(out);
}
// Edge-aware no-history fill (rr_edge_fill.h): latest outcome for the panel, changes logged.
static std::atomic<unsigned> edge_fill_state{}, edge_fill_reports{};
static std::atomic<ULONGLONG> edge_fill_time{};
static void ReportEdgeFill(rredgefill::State state, const char *detail = nullptr) {
    edge_fill_time = GetTickCount64();
    if (edge_fill_state.exchange(unsigned(state)) == unsigned(state) || edge_fill_reports++ >= 16)
        return;
    char line[256];
    snprintf(line, sizeof(line), "Subsurface Scattering edge-aware fill: %s%s%s",
             rredgefill::Label(state), detail ? "; " : "", detail ? detail : "");
    WriteLog(line);
}
static rredgefill::State EdgeFillState() {
    return GetTickCount64() - edge_fill_time.load() <= 1000
               ? rredgefill::State(edge_fill_state.load())
               : rredgefill::State::Idle;
}
// The edge fill's surface: the PackedSurface this native spatial dispatch itself binds at
// t13 (its same-surface input) must be this frame's prepass surface in the slot, in a
// tracked readable state on this list; plus that slot's converted guides. Like the
// dispatch's own t0/t1 (SparsePair), a null-sided aliasing barrier on the list does not
// disqualify it: the game reads it here. An aliasing barrier naming it does.
static const char *SparseEdgeInputs(reshade::api::command_list *list,
                                    ID3D12GraphicsCommandList *native, uint64_t effective,
                                    rrsparsecopy::EdgeInputs &out, char (&detail)[96]) {
    auto *state = rrbindings::State(list->get_device());
    if (!state || !state->tracker.Healthy())
        return "binding tracker unavailable";
    if (!LiveRR().EdgeSurface(native, out.surface, out.normal, out.status))
        return "no converted guides for this frame";
    auto root = state->tracker.CurrentLayout(list->get_native());
    auto loc = state->tracker.Find(list->get_native(), {false, 13});
    if (!loc.known || !ComputeState().TableBound(native, (ID3D12PipelineState *)effective,
                                                 (ID3D12RootSignature *)root, loc.parameter,
                                                 loc.table))
        return "native t13 table unproven";
    auto slot = rrbindings::Resolve(list->get_device(), {loc.table}, loc.offset);
    if (!slot.heap)
        return "native t13 descriptor unresolved";
    const auto v = state->tracker.Get(slot);
    if (!v.known || v.buffer || !v.resource ||
        v.view_type != unsigned(reshade::api::resource_view_type::texture_2d) || v.first_level ||
        v.levels != 1 || v.first_layer || v.layers > 1 ||
        v.view_format != unsigned(DXGI_FORMAT_R32G32B32A32_UINT))
        return "native t13 view unexpected";
    auto bound = fg::NativeObject<ID3D12Resource>((ID3D12Resource *)v.resource);
    if (!bound || bound.Get() != out.surface.Get())
        return "native t13 is not this frame's prepass surface";
    rrsparsecopy::Layouts::Alias alias{};
    if (!SparseLayouts().Get(native, bound.Get(), out.surface_state, alias))
        return "prepass surface state not tracked on this list";
    if (alias == rrsparsecopy::Layouts::Alias::Named)
        return "prepass surface named by an aliasing barrier";
    if (!rrsparsecopy::Readable(out.surface_state)) {
        snprintf(detail, sizeof(detail), "prepass surface state 0x%x is not read-only",
                 unsigned(out.surface_state));
        return detail;
    }
    return nullptr;
}
static bool SparseDispatch(reshade::api::command_list *list, uint32_t x, uint32_t y, uint32_t z) {
    const auto binding = ShaderDispatches().Get(list->get_native());
    if (binding.shader != rrbypass::Temporal && binding.shader != rrbypass::Spatial)
        return false;
    auto *device = list->get_device();
    if (device->get_api() != reshade::api::device_api::d3d12 || rrbypass::Health(device).failed)
        return false;
    auto native = fg::NativeObject<ID3D12GraphicsCommandList>(
        (ID3D12GraphicsCommandList *)list->get_native());
    if (!native || !AlbedoInspector().SafeCopies(native.Get()) ||
        !AlbedoInspector().StateCoverage(native.Get())) {
        SparsePool().Abandon(native.Get());
        rrbypass::Fail(device,
                       "RR unsupported native copy context; correction disabled and RR blocked");
        return false;
    }
    const auto effective = rrbypass::Effective(device, binding.pipeline);
    rrsparsecopy::Pair input, output;
    if (binding.shader == rrbypass::Temporal) {
        if (!SparsePair(list, native.Get(), effective, false, 4, input) ||
            !SparsePair(list, native.Get(), effective, true, 0, output) || z != 1 ||
            x != (input.r[0]->GetDesc().Width + 7) / 8 ||
            y != (input.r[0]->GetDesc().Height + 7) / 8) {
            rrbypass::Skip(device, "RR current/native-history bindings or layouts unproven");
            return false;
        }
        rrinternal::Scope ignoreCopies;
        rrcompute::Tracker::Internal ignoreState;
        if (!SparsePool().Snapshot(native.Get(), input, output))
            rrbypass::Fail(device, "RR current snapshot pool/shape validation failed; native "
                                   "temporal continues, RR blocked");
        return false;
    }
    if (!SparsePair(list, native.Get(), effective, false, 0, input)) {
        SparsePool().Abandon(native.Get());
        rrbypass::Skip(device, "RR spatial input proof failed");
        return false;
    }
    if (!SparsePool().Pending(native.Get(), input))
        return false;
    if (!SparsePair(list, native.Get(), effective, true, 0, output, true)) {
        SparsePool().Abandon(native.Get());
        rrbypass::Skip(device, "RR first spatial output proof failed");
        return false;
    }
    // SSS pixels take lobe 2 history first + spatial; the native-root path prepares raw.
    constexpr unsigned lobe = rrlighting::SssHistory2;
    auto scale = rrbypass::Scale(device, binding.pipeline);
    rrinternal::Scope ignoreCopies;
    rrcompute::Tracker::Internal ignoreState;
    const auto rates = LiveRR().SamplingData(native.Get());
    // An admitted guide gates coverage. Only matched, drawn surfaces take native history;
    // without a usable record, prepare raw lighting.
    unsigned mask_age{};
    const auto mask = LiveRR().SssMask(native.Get(), &mask_age);
    rrsparsecopy::CoverageInputs coverage;
    auto coverage_reason = rrssscov::FrameReason::NoStorage;
    const bool coverage_ready =
        mask && LiveRR().SssCoverage(native.Get(), coverage, coverage_reason);
    const auto cbvs = ComputeState().RateCBVs(
        native.Get(), (ID3D12PipelineState *)effective,
        (ID3D12RootSignature *)rrbindings::State(device)->tracker.CurrentLayout(
            list->get_native()));
    rrcompute::Snapshot privateRestore;
    if (!cbvs.valid || mask)
        privateRestore = ComputeState().Capture(native.Get(), (ID3D12PipelineState *)effective);
    const bool privateValid =
        privateRestore.valid && privateRestore.complete && privateRestore.heap_restore_valid &&
        UINT64(privateRestore.root.Get()) ==
            rrbindings::State(device)->tracker.CurrentLayout(list->get_native());
    if (!rates.buffer || (!cbvs.valid && !privateValid)) {
        char text[256];
        snprintf(text, sizeof(text),
                 "RR scale rejected: rates=%d rootCBVs=%d snapshot=%s complete=%d heapRestore=%d",
                 bool(rates.buffer), cbvs.valid, privateRestore.status, privateRestore.complete,
                 privateRestore.heap_restore_valid);
        SparsePool().Abandon(native.Get());
        rrbypass::Fail(device, text);
        return false;
    }
    // Without proven edge-fill inputs, covered pixels use the group-mean fallback.
    const bool edge_wanted = mask && privateValid;
    rrsparsecopy::EdgeInputs edge;
    char edge_detail[96]{};
    const char *edge_why =
        edge_wanted ? SparseEdgeInputs(list, native.Get(), effective, edge, edge_detail) : nullptr;
    bool masked = false, edged = false, covered = false;
    bool replaced = mask && privateValid && coverage_ready &&
                    SparsePool().Apply(native.Get(), input, output, scale.Get(),
                                       (ID3D12PipelineState *)effective, x, y, z, &rates, nullptr,
                                       &privateRestore, lobe, mask.Get(), &masked,
                                       edge_wanted && !edge_why ? &edge : nullptr, &edged,
                                       coverage_ready ? &coverage : nullptr, &covered);
    // A rejected attempt records nothing; the usual path then prepares raw lighting.
    if (!replaced)
        replaced = SparsePool().Apply(native.Get(), input, output, scale.Get(),
                                      (ID3D12PipelineState *)effective, x, y, z, &rates,
                                      cbvs.valid ? &cbvs : nullptr,
                                      cbvs.valid ? nullptr : &privateRestore, lobe);
    if (replaced && LiveRR().SssEnabled()) {
        char why[96]{};
        if (masked)
            snprintf(why, sizeof(why), "guide %u frame(s) old%s", mask_age,
                     covered ? ", coverage record selecting" : "");
        // Without a usable record the coverage reason; with one, the masked preparation
        // itself refused (its private snapshot reason below, or the coverage inputs).
        const bool coverage_why = mask && !masked && privateValid;
        if (coverage_why)
            snprintf(why, sizeof(why), "%s",
                     coverage_ready ? "coverage inputs or shader variant refused (shape, device "
                                      "or pipeline)"
                                    : rrssscov::Label(coverage_reason));
        ReportSssMask(masked ? rrsssmask::State::CoverageActive
                      : !mask ? rrsssmask::State::NoGuide
                              : rrsssmask::State::CoverageWaiting,
                      masked || coverage_why ? why
                      : !mask                              ? nullptr
                      : !privateRestore.valid              ? privateRestore.status
                      : !privateRestore.complete           ? "private snapshot arguments incomplete"
                      : !privateRestore.heap_restore_valid ? "private snapshot heap restore unproven"
                      : !privateValid ? "private snapshot root differs from the bound root"
                                      : "masked preparation rejected (guide, format or shader)");
    }
    if (replaced && masked)
        ReportEdgeFill(edged ? rredgefill::State::Active : rredgefill::State::Unavailable,
                       edged || !edge_wanted ? nullptr
                       : edge_why            ? edge_why
                                             : "surface inputs rejected (shape, format or state)");
    if (replaced) {
        rrbypass::Corrected(device);
    }
    else {
        // Retried after the pause; Before() then fences the unused snapshot.
        SparsePool().Abandon(native.Get());
        rrbypass::Fail(
            device,
            "RR first spatial correction validation failed; native temporal continues, RR blocked");
    }
    return replaced;
}
