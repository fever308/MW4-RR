struct AuditGraphicsState {
    rrshader::Binding pipeline;
    std::array<rrbindings::Value, 8> targets{};
    unsigned count{};
    bool targets_known{};
    uint64_t target_handle{}, target_serial{};
};
static std::mutex audit_graphics_mutex;
static std::unordered_map<uint64_t, AuditGraphicsState> audit_graphics;
static void AuditResetGraphics(uint64_t list) {
    std::lock_guard lock(audit_graphics_mutex);
    audit_graphics.erase(list);
}
static void AuditBindGraphics(reshade::api::command_list *list, reshade::api::pipeline_stage stage,
                              uint64_t pipeline, uint64_t hash, bool compute) {
    std::lock_guard lock(audit_graphics_mutex);
    const auto id = list->get_native();
    if (!audit_graphics.contains(id) && audit_graphics.size() >= 8192)
        return;
    auto &state = audit_graphics[id];
    if (compute || stage == reshade::api::pipeline_stage::ray_tracing_shader)
        state.pipeline = {};
    else if (stage == reshade::api::pipeline_stage::all ||
             (uint32_t(stage) & uint32_t(reshade::api::pipeline_stage::pixel_shader)))
        state.pipeline = {pipeline, hash};
}
static void AuditRenderTargets(reshade::api::command_list *list, uint32_t count,
                               const reshade::api::resource_view *views,
                               reshade::api::resource_view) {
    if (list->get_device()->get_api() != reshade::api::device_api::d3d12)
        return;
    try {
        auto native = fg::NativeObject<ID3D12GraphicsCommandList>(
            reinterpret_cast<ID3D12GraphicsCommandList *>(list->get_native()));
        const auto serial = ComputeState().GraphicsTargetSerial(native.Get());
        std::lock_guard lock(audit_graphics_mutex);
        const auto id = list->get_native();
        if (!audit_graphics.contains(id) && audit_graphics.size() >= 8192) {
            return;
        }
        auto &target = audit_graphics[id];
        target.targets_known = false;
        target.count = 0;
        if (count > 8 || (count && !views)) {
            return;
        }
        for (unsigned i = 0; i < count; ++i) {
            target.targets[i] = rrbindings::Describe(list->get_device(), views[i]);
            NrResource(list, {target.targets[i].resource}, rrnrorder::Use::RenderTarget);
        }
        target.count = count;
        target.targets_known = true;
        target.target_handle = count == 1 ? views[0].handle : 0;
        target.target_serial = serial;
    } catch (...) {
    }
}
static bool AuditBeginRenderPass(reshade::api::command_list *list, uint32_t count,
                                 const reshade::api::render_pass_render_target_desc *targets,
                                 const reshade::api::render_pass_depth_stencil_desc *,
                                 reshade::api::render_pass_flags) {
    try {
        if (count > 8 || (count && !targets)) {
            std::lock_guard lock(audit_graphics_mutex);
            audit_graphics[list->get_native()].targets_known = false;
            return false;
        }
        std::array<reshade::api::resource_view, 8> views{};
        for (unsigned i = 0; i < count; ++i)
            views[i] = targets[i].view;
        AuditRenderTargets(list, count, views.data(), {});
    } catch (...) {
    }
    return false;
}
static bool AuditEndRenderPass(reshade::api::command_list *list) {
    try {
        std::lock_guard lock(audit_graphics_mutex);
        auto at = audit_graphics.find(list->get_native());
        if (at != audit_graphics.end()) {
            at->second.targets_known = false;
            at->second.count = 0;
        }
    } catch (...) {
    }
    return false;
}
