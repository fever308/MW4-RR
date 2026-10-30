static void TransparencyProducerDraw(reshade::api::command_list *list, uint32_t count,
                                     uint32_t instances) noexcept {
    if (!LiveRR().TransparencyEnabled() || count != 3 || instances != 1 ||
        list->get_device()->get_api() != reshade::api::device_api::d3d12)
        return;
    GlassOnly([&] {
        AuditGraphicsState g;
        {
            std::lock_guard lock(audit_graphics_mutex);
            auto i = audit_graphics.find(list->get_native());
            if (i == audit_graphics.end())
                return;
            g = i->second;
        }
        if (!rrsss::Recognized().HasHash(g.pipeline.shader))
            return;
        auto native = fg::NativeObject<ID3D12GraphicsCommandList>(
            reinterpret_cast<ID3D12GraphicsCommandList *>(list->get_native()));
        if (!native || !AlbedoInspector().SafeCopies(native.Get()) ||
            !AlbedoInspector().StateCoverage(native.Get())) {
            return;
        }
        auto out = ComputeState().CaptureGraphicsOutputs(
            native.Get(), reinterpret_cast<ID3D12PipelineState *>(g.pipeline.pipeline));
        rrsss::NativePipelineObservation observed{};
        if (!out.valid || !out.proof.Available() ||
            !rrsss::PipelineProof::NativeObservation(out.pipeline.Get(), observed) ||
            !observed.complete || observed.malformed || observed.unknown ||
            observed.shader_malformed || !observed.shaders[1].code ||
            !rrsss::CompositeShader(g.pipeline.shader, observed.shaders[1].hash,
                                    observed.shaders[1].bytes) ||
            observed.root_identity !=
                UINT64(ComputeState().TransparencyGraphicsRoot(native.Get()).Get())) {
            return;
        }
        if (!g.targets_known || g.count != 1 || !g.targets[0].known ||
            !ComputeState().GraphicsTargetBound(native.Get(), g.target_handle, g.target_serial)) {
            return;
        }
        const auto &v = g.targets[0];
        if (v.view_format != unsigned(DXGI_FORMAT_R11G11B10_FLOAT) || v.first_level ||
            v.first_layer ||
            (v.view_type != unsigned(reshade::api::resource_view_type::texture_2d_multisample) &&
             v.view_type != unsigned(reshade::api::resource_view_type::texture_2d))) {
            return;
        }
        auto source =
            fg::NativeObject<ID3D12Resource>(reinterpret_cast<ID3D12Resource *>(v.resource));
        if (!source || !rrglass::Shape(source->GetDesc(), 1)) {
            return;
        }
        const auto d = source->GetDesc();
        const auto &a = out.proof.viewports[0];
        const auto &r = out.proof.scissors[0];
        if (a.TopLeftX != 0 || a.TopLeftY != 0 || a.Width != float(d.Width) ||
            a.Height != float(d.Height) || r.left != 0 || r.top != 0 || r.right != LONG(d.Width) ||
            r.bottom != LONG(d.Height)) {
            return;
        }
        LiveRR().TransparencyProducerMark(native.Get(), source.Get(), out.generation);
    });
}
static void OitSceneDraw(reshade::api::command_list *list) noexcept {
    const bool live = LiveRR().TransparencyEnabled();
    if (!live || list->get_device()->get_api() != reshade::api::device_api::d3d12)
        return;
    GlassOnly([&] {
        AuditGraphicsState graphics;
        {
            std::lock_guard lock(audit_graphics_mutex);
            auto i = audit_graphics.find(list->get_native());
            if (i == audit_graphics.end())
                return;
            graphics = i->second;
        }
        if (graphics.pipeline.shader != 0xa36d02f6245a4129ull)
            return;
        auto native = fg::NativeObject<ID3D12GraphicsCommandList>(
            reinterpret_cast<ID3D12GraphicsCommandList *>(list->get_native()));
        auto reject = [&](const char *why) {
            if (live)
                LiveRR().TransparencyUnsupported(why);
        };
        if (!native || !AlbedoInspector().SafeCopies(native.Get()) ||
            !AlbedoInspector().StateCoverage(native.Get())) {
            reject("RR OIT anchor unsafe or uncovered context");
            return;
        }
        const auto output = ComputeState().CaptureGraphicsOutputs(
            native.Get(), reinterpret_cast<ID3D12PipelineState *>(graphics.pipeline.pipeline));
        if (!output.valid || !graphics.targets_known || graphics.count != 1 ||
            !graphics.targets[0].known ||
            !ComputeState().GraphicsTargetBound(native.Get(), graphics.target_handle,
                                                graphics.target_serial)) {
            reject("RR OIT anchor native target/pipeline not proven");
            return;
        }
        const auto &view = graphics.targets[0];
        const bool view_type =
            view.view_type == unsigned(reshade::api::resource_view_type::texture_2d_multisample) ||
            view.view_type == unsigned(reshade::api::resource_view_type::texture_2d);
        if (view.view_format != unsigned(DXGI_FORMAT_R11G11B10_FLOAT) || !view_type ||
            view.first_level || view.first_layer) {
            reject("RR OIT anchor target view unsupported");
            return;
        }
        auto source =
            fg::NativeObject<ID3D12Resource>(reinterpret_cast<ID3D12Resource *>(view.resource));
        if (!source) {
            reject("RR glass OIT anchor source identity unavailable");
            return;
        }
        if (live) {
            D3D12_RESOURCE_STATES source_state{};
            // An indirect draw can execute zero draws. Its retained RT binding
            // alone cannot authorize a snapshot after that allocation was aliased.
            const bool render_target_proven =
                SparseLayouts().GetUnaliased(native.Get(), source.Get(), source_state) &&
                source_state == D3D12_RESOURCE_STATE_RENDER_TARGET;
            LiveRR().TransparencyAtDraw(native.Get(), source.Get(), render_target_proven);
            LiveRR().TransparencyAnchor(native.Get(), source.Get());
        }
    });
}
static void OitSceneTransition(ID3D12GraphicsCommandList *cl,
                               const D3D12_RESOURCE_BARRIER &b) noexcept {
    if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
        b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE)
        return;
    if (LiveRR().TransparencyEnabled())
        try {
            LiveRR().TransparencyProducerTransition(cl, b,
                                                    cl && AlbedoInspector().SafeCopies(cl) &&
                                                        AlbedoInspector().StateCoverage(cl));
        } catch (...) {
            LiveRR().TransparencyProducerUnknown(cl);
        }
    if (b.Transition.StateBefore == D3D12_RESOURCE_STATE_UNORDERED_ACCESS &&
        b.Transition.StateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET &&
        LiveRR().TransparencyEnabled() && cl && AlbedoInspector().SafeCopies(cl) &&
        AlbedoInspector().StateCoverage(cl) &&
        (b.Transition.Subresource == 0 ||
         b.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)) {
        try {
            LiveRR().TransparencyPreparation(cl, b.Transition.pResource);
        } catch (...) {
            WriteLog("RR glass preparation observer exception; original barrier already forwarded");
        }
    }
}
static void GlassDraw(reshade::api::command_list *list, bool, uint32_t count,
                      uint32_t instances) noexcept {
    TransparencyProducerDraw(list, count, instances);
    if (LiveRR().TransparencyEnabled() && count == 3 && instances == 1)
        OitSceneDraw(list);
}
