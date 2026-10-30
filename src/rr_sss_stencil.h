#pragma once
#include <d3d12.h>
#include <wrl/client.h>
namespace rrsss {
struct StencilSetup {
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    // The same initializer writing rrssscov::Marker instead of 0 (Subsurface Scattering
    // coverage record); null when it could not be created, which only keeps the
    // pass from recording coverage.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> marker;
    UINT read_mask = 255;
    bool required{};
    DXGI_SAMPLE_DESC samples{4, 0};
    bool replay_geometry{};
};
inline D3D12_COMPARISON_FUNC InverseStencil(D3D12_COMPARISON_FUNC op) {
    switch (op) {
    case D3D12_COMPARISON_FUNC_NEVER:
        return D3D12_COMPARISON_FUNC_ALWAYS;
    case D3D12_COMPARISON_FUNC_LESS:
        return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case D3D12_COMPARISON_FUNC_EQUAL:
        return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case D3D12_COMPARISON_FUNC_LESS_EQUAL:
        return D3D12_COMPARISON_FUNC_GREATER;
    case D3D12_COMPARISON_FUNC_GREATER:
        return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case D3D12_COMPARISON_FUNC_NOT_EQUAL:
        return D3D12_COMPARISON_FUNC_EQUAL;
    case D3D12_COMPARISON_FUNC_GREATER_EQUAL:
        return D3D12_COMPARISON_FUNC_LESS;
    case D3D12_COMPARISON_FUNC_ALWAYS:
        return D3D12_COMPARISON_FUNC_NEVER;
    default:
        return D3D12_COMPARISON_FUNC(0);
    }
}
inline bool StencilCompatible(const D3D12_DEPTH_STENCIL_DESC2 &d, DXGI_FORMAT format,
                              bool replay_geometry = false) {
    return !d.DepthEnable && d.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ZERO &&
           !d.DepthBoundsTestEnable && d.StencilEnable &&
           format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT &&
           (replay_geometry || d.FrontFace.StencilFunc == d.BackFace.StencilFunc) &&
           d.FrontFace.StencilReadMask == d.BackFace.StencilReadMask &&
           InverseStencil(d.FrontFace.StencilFunc) != D3D12_COMPARISON_FUNC(0) &&
           InverseStencil(d.BackFace.StencilFunc) != D3D12_COMPARISON_FUNC(0);
}
inline D3D12_DEPTH_STENCIL_DESC InverseStencilState(const D3D12_DEPTH_STENCIL_DESC2 &d) {
    D3D12_DEPTH_STENCIL_DESC out{};
    out.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    out.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    out.StencilEnable = TRUE;
    out.StencilReadMask = d.FrontFace.StencilReadMask;
    out.StencilWriteMask = 0;
    out.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                     InverseStencil(d.FrontFace.StencilFunc)};
    out.BackFace = out.FrontFace;
    out.BackFace.StencilFunc = InverseStencil(d.BackFace.StencilFunc);
    return out;
}
} // namespace rrsss
