#pragma once
// Execute the private reader immediately before the exact native VRRT shader.
// Its inputs inherit that shader's SRV/CBV preconditions. Never write queues,
// requests, native counters, root signatures or shared descriptors.
static rrbindings::Value SamplingConstant(reshade::api::device *device, UINT64 list, unsigned reg) {
    auto *state = rrbindings::State(device);
    if (!state)
        return {};
    auto value = state->tracker.Constant(list, reg);
    if (!value.known) {
        const auto loc = state->tracker.FindConstant(list, reg);
        if (loc.known) {
            const auto slot = rrbindings::Resolve(device, {loc.table}, loc.offset);
            if (slot.heap)
                value = state->tracker.Get(slot);
        }
    }
    return value;
}
static bool PreserveNativeSampling(reshade::api::command_list *list,
                                   const rrshader::Binding &binding, UINT x, UINT y, UINT z) {
    if (binding.shader != rrbypass::Vrrt)
        return false;
    auto *device = list->get_device();
    auto *state = rrbindings::State(device);
    if (!state || !state->tracker.Healthy())
        return false;
    auto native = fg::NativeObject<ID3D12GraphicsCommandList>(
        (ID3D12GraphicsCommandList *)list->get_native());
    if (!native || !AlbedoInspector().SafeCopies(native.Get()) ||
        !AlbedoInspector().StateCoverage(native.Get()))
        return false;
    const auto restore =
        ComputeState().Capture(native.Get(), (ID3D12PipelineState *)binding.pipeline);
    if (!restore.valid || !restore.complete || !restore.heap_restore_valid ||
        UINT64(restore.root.Get()) != state->tracker.CurrentLayout(list->get_native()))
        return false;
    rrvrrt::Inputs input;
    std::array<rrbindings::Value, 2> values;
    for (unsigned i = 0; i < 2; ++i) {
        auto loc = state->tracker.Find(list->get_native(), {false, i});
        if (!loc.known || loc.parameter >= restore.args.size() ||
            restore.args[loc.parameter].kind != rrcompute::Kind::Table ||
            restore.args[loc.parameter].address != loc.table)
            return false;
        auto slot = rrbindings::Resolve(device, {loc.table}, loc.offset);
        if (!slot.heap)
            return false;
        auto &v = values[i];
        v = state->tracker.Get(slot);
        if (!v.known || !v.resource)
            return false;
        input.resources[i] = fg::NativeObject<ID3D12Resource>((ID3D12Resource *)v.resource);
        if (!input.resources[i])
            return false;
        auto desc = input.resources[i]->GetDesc();
        if (!rrsparsecopy::SameDevice((ID3D12Device *)device->get_native(),
                                      input.resources[i].Get()))
            return false;
        if (i == 0) {
            if (v.buffer || v.view_type != unsigned(reshade::api::resource_view_type::texture_2d) ||
                v.first_level || v.levels != 1 || v.first_layer || v.layers > 1 ||
                v.view_format != DXGI_FORMAT_R32_UINT || desc.Format != DXGI_FORMAT_R32_UINT ||
                desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels != 1 ||
                desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 || desc.Width != v.width ||
                desc.Height != v.height || !rrvrrt::Dimensions(desc.Width, desc.Height))
                return false;
            input.w = UINT(desc.Width);
            input.h = desc.Height;
        } else {
            if (!v.buffer || v.view_format != unsigned(reshade::api::format::r32_typeless) ||
                desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || v.byte_offset % 4 ||
                v.byte_size < 32 || v.byte_size % 4 || v.byte_size / 4 > UINT_MAX ||
                v.byte_offset > desc.Width || v.byte_size > desc.Width - v.byte_offset)
                return false;
            input.budget_offset = v.byte_offset;
            input.budget_bytes = v.byte_size;
        }
        D3D12_RESOURCE_STATES observed{};
        if (SparseLayouts().Get(native.Get(), input.resources[i].Get(), observed) &&
            !rrsparsecopy::Readable(observed))
            return false;
    }
    if (z != 1 || x != (input.w + 15) / 16 || y != (input.h + 15) / 16)
        return false;
    for (unsigned i = 0; i < 2; ++i) {
        const unsigned reg = i ? 3 : 1;
        const auto value = SamplingConstant(device, list->get_native(), reg);
        if (!rrinputs::NativeConstantMatches(state->tracker, list->get_native(), reg, value,
                                             restore))
            return false;
        input.resources[i + 2] = fg::NativeObject<ID3D12Resource>((ID3D12Resource *)value.resource);
        if (!input.resources[i + 2])
            return false;
        const auto desc = input.resources[i + 2]->GetDesc();
        const UINT64 required = i ? 1392 : 32;
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || value.byte_offset % 256 ||
            value.byte_offset > desc.Width || required > desc.Width - value.byte_offset ||
            value.byte_size < required ||
            !rrsparsecopy::SameDevice((ID3D12Device *)device->get_native(),
                                      input.resources[i + 2].Get()))
            return false;
        input.cbv[i] = input.resources[i + 2]->GetGPUVirtualAddress() + value.byte_offset;
        D3D12_RESOURCE_STATES observed{};
        constexpr auto allowed = D3D12_RESOURCE_STATE_GENERIC_READ;
        if (SparseLayouts().Get(native.Get(), input.resources[i + 2].Get(), observed) &&
            (!(observed & D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) ||
             (observed & ~allowed)))
            return false;
    }
    if (!LiveRR().NativeSampling(native.Get(), input, restore))
        return false;
    return true;
}
