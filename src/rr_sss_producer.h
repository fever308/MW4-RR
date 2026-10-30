#pragma once
#include "rr_sss_policy.h"
#include "rr_sss_graphics_state.h"
#include "rr_sss_stencil.h"
#include "rr_sss_layout.h"
#include <stdexcept>

namespace rrsss {
struct ProducerDraw {
    bool indexed{};
    UINT count{}, instances{}, first{};
    INT base_vertex{};
    UINT first_instance{};
    template <class Command> void Issue(Command *cl) const {
        if (indexed)
            cl->DrawIndexedInstanced(count, instances, first, base_vertex, first_instance);
        else
            cl->DrawInstanced(count, instances, first, first_instance);
    }
};
inline bool SupportedState(const GraphicsOutputProof &proof, bool depth_bound,
                           const StencilSetup &stencil = {}) {
    return proof.Available() &&
           !(depth_bound ? (!stencil.required ||
                            !proof.StencilReady(stencil.read_mask, stencil.replay_geometry))
                         : stencil.required);
}
inline bool SupportedArea(const GraphicsOutputProof &proof, bool depth_bound, UINT width,
                          UINT height, const StencilSetup &stencil = {}) {
    const Layout layout{width, height};
    if (!layout.Valid() || !SupportedState(proof, depth_bound, stencil))
        return false;
    const auto &v = proof.viewports[0];
    const auto &r = proof.scissors[0];
    return v.TopLeftX == 0 && v.TopLeftY == 0 && v.Width == float(layout.ProducerWidth()) &&
           v.Height == float(layout.ProducerHeight()) && v.MinDepth == 0 && v.MaxDepth == 1 &&
           r.left == 0 && r.top == 0 && r.right == LONG(layout.ProducerWidth()) &&
           r.bottom == LONG(layout.ProducerHeight());
}
inline bool GeometryState(const GraphicsOutputSnapshot &s, const StencilSetup &stencil = {}) {
    return s.valid && s.pipeline && s.outputs && s.outputs->valid && s.outputs->count == 1 &&
           s.OriginalTarget().ptr && s.generation == s.proof.generation &&
           (!stencil.required || stencil.pipeline) &&
           SupportedState(s.proof, s.outputs->depth_bound, stencil);
}
inline bool Geometry(const GraphicsOutputSnapshot &s, UINT width, UINT height,
                     const StencilSetup &stencil = {}) {
    return GeometryState(s, stencil) &&
           SupportedArea(s.proof, s.outputs->depth_bound, width, height, stencil);
}
struct Producer {
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> variant;
    StencilSetup stencil;
    GraphicsOutputSnapshot original;
    bool live{}, submitted{}, written{};
    // mark: record Subsurface Scattering coverage with the marker initializer
    // (rrssscov::Marker); marked: this recording did, so its resolve must use the coverage
    // resolve, which gives the guide 0 for the marker as the baseline does for 0.
    bool mark{}, marked{};
    Admission admission;
    template <class Gpu>
    DrawResult Record(ID3D12GraphicsCommandList *cl, const ProducerDraw &draw, Gpu &gpu) {
        bool begun = false;
        marked = false;
        return DrawOnce(
            [&] {
                if (bool(original.DepthTarget().ptr) != stencil.required ||
                    (stencil.required && !stencil.pipeline))
                    throw std::runtime_error("SSS stencil initializer unavailable");
            },
            [&] {
                gpu.Begin(cl);
                begun = true;
                const auto depth = original.DepthTarget();
                if (stencil.required) {
                    auto target = gpu.Target();
                    marked = mark && stencil.marker;
                    cl->SetPipelineState(marked ? stencil.marker.Get() : stencil.pipeline.Get());
                    cl->OMSetRenderTargets(1, &target, FALSE, &depth);
                    if (stencil.replay_geometry)
                        draw.Issue(cl);
                    else
                        cl->DrawInstanced(3, 1, 0, 0);
                }
                cl->SetPipelineState(variant.Get());
                D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {original.OriginalTarget(), gpu.Target()};
                cl->OMSetRenderTargets(2, rtvs, FALSE, depth.ptr ? &depth : nullptr);
            },
            [&] { draw.Issue(cl); },
            [&] {
                original.Restore(cl);
                if (begun)
                    gpu.End(cl);
            });
    }
};
} // namespace rrsss
