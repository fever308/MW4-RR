#pragma once
#include "rr_gpu_common.h"
#include "rr_sss_layout.h"
#include "../build/sss-owned-shader/rr_sss_deinterleave.generated.h"
namespace rrsss {
using Microsoft::WRL::ComPtr;
inline void Transition(ID3D12GraphicsCommandList *cl, ID3D12Resource *resource,
                       D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    cl->ResourceBarrier(1, &b);
}
struct ResolvePipeline {
    ComPtr<ID3D12RootSignature> root, coverage_root;
    ComPtr<ID3D12PipelineState> pipeline, coverage_pipeline;
    // coverage: the Subsurface Scattering coverage resolve (rrssscov), the same layout plus
    // parameter 5, a u3 table.
    template <class Pipelines> void Prepare(Pipelines &p, bool coverage = false) {
        auto &r = coverage ? coverage_root : root;
        auto &pso = coverage ? coverage_pipeline : pipeline;
        if (pso)
            return;
        D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                           {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0},
                                           {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 3, 0, 0}};
        D3D12_ROOT_PARAMETER args[6]{};
        args[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        for (unsigned i = 0; i < 2; ++i) {
            args[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            args[1 + i].DescriptorTable = {1, &ranges[i]};
            args[3 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            args[3 + i].Descriptor.ShaderRegister = 1 + i;
        }
        args[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        args[5].DescriptorTable = {1, &ranges[2]};
        D3D12_ROOT_SIGNATURE_DESC desc{coverage ? 6u : 5u, args};
        p.Root(desc, r);
        if (coverage)
            p.Pipeline(r.Get(), RRSSSDeinterleaveCoverage, sizeof(RRSSSDeinterleaveCoverage), pso);
        else
            p.Pipeline(r.Get(), RRSSSDeinterleave, sizeof(RRSSSDeinterleave), pso);
    }
};
struct Gpu {
    ComPtr<ID3D12Resource> target, guide;
    ComPtr<ID3D12DescriptorHeap> rtv, heap;
    UINT stride{}, width{}, height{};
    UINT64 bytes{};
    bool ready{};
    DXGI_SAMPLE_DESC samples{};
    static bool SupportedSamples(DXGI_SAMPLE_DESC d) {
        return d.Count == 4 && (d.Quality == 0 || d.Quality == 0xffffffffu);
    }
    static bool ReuseSamples(bool allocated, DXGI_SAMPLE_DESC cached, DXGI_SAMPLE_DESC requested) {
        return SupportedSamples(requested) && (!allocated || (cached.Count == requested.Count &&
                                                              cached.Quality == requested.Quality));
    }
    bool MatchesSamples(DXGI_SAMPLE_DESC requested) const {
        return ReuseSamples(bool(target), samples, requested);
    }
    static bool ReuseLayout(bool allocated, UINT cached_w, UINT cached_h, DXGI_SAMPLE_DESC cached,
                            UINT w, UINT h, DXGI_SAMPLE_DESC requested) {
        return Layout{w, h}.Valid() && ReuseSamples(allocated, cached, requested) &&
               (!allocated || (cached_w == w && cached_h == h));
    }
    bool MatchesLayout(UINT w, UINT h, DXGI_SAMPLE_DESC requested) const {
        return ReuseLayout(bool(target), width, height, samples, w, h, requested);
    }
    static D3D12_RESOURCE_DESC TargetDesc(UINT w, UINT h, DXGI_SAMPLE_DESC sample_desc = {4, 0}) {
        const Layout layout{w, h};
        rrprivate::Require(layout.Valid(), "SSS exact 2x parity layout invalid");
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = layout.ProducerWidth();
        d.Height = layout.ProducerHeight();
        d.DepthOrArraySize = d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R16_FLOAT;
        d.SampleDesc = sample_desc;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        return d;
    }
    static D3D12_RESOURCE_DESC GuideDesc(UINT w, UINT h) {
        auto d = TargetDesc(w, h);
        d.Width = w;
        d.Height = h;
        d.SampleDesc = {1, 0};
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return d;
    }
    static UINT64 RequiredBytes(ID3D12Device *device, UINT w, UINT h,
                                DXGI_SAMPLE_DESC sample_desc = {4, 0}) {
        if (!device || !Layout{w, h}.Valid() || !SupportedSamples(sample_desc))
            return UINT64_MAX;
        const D3D12_RESOURCE_DESC descriptions[] = {TargetDesc(w, h, sample_desc), GuideDesc(w, h)};
        UINT64 total = 0;
        for (const auto &d : descriptions) {
            const auto n = device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
            if (!n || n == UINT64_MAX || total > UINT64_MAX - n)
                return UINT64_MAX;
            total += n;
        }
        return total;
    }
    template <class Frame, class Pipelines, class Budget>
    void Prepare(Frame &frame, Pipelines &p, Budget &budget,
                 DXGI_SAMPLE_DESC sample_desc = {4, 0}) {
        using rrprivate::Check;
        using rrprivate::Require;
        Require(MatchesLayout(frame.w, frame.h, sample_desc),
                "SSS layout or sample pattern mismatch with retained allocation");
        if (ready)
            return;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{DXGI_FORMAT_R16_FLOAT};
        Check(
            p.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)),
            "SSS R16 support query failed");
        const UINT needs =
            D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET |
            D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD | D3D12_FORMAT_SUPPORT1_SHADER_LOAD;
        Require((support.Support1 & needs) == needs &&
                    (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE),
                "SSS R16 target/load/store support unavailable");
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality{
            DXGI_FORMAT_R16_FLOAT, 4, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
        Check(p.device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality,
                                            sizeof(quality)),
              "SSS MSAA support query failed");
        Require(quality.NumQualityLevels > 0, "SSS four-sample target unavailable");
        p.sss.Prepare(p);
        auto desc = TargetDesc(frame.w, frame.h, sample_desc);
        if (!target) {
            target = frame.Allocate(p, budget, desc);
            width = frame.w;
            height = frame.h;
            samples = sample_desc;
            bytes += p.device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        }
        desc = GuideDesc(frame.w, frame.h);
        if (!guide) {
            guide = frame.Allocate(p, budget, desc);
            bytes += p.device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        }
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
        if (!rtv)
            rrprivate::CheckAllocation(p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv)),
                  "SSS RTV heap allocation failed");
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R16_FLOAT;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        p.device->CreateRenderTargetView(target.Get(), &view,
                                         rtv->GetCPUDescriptorHandleForHeapStart());
        // The third descriptor is the coverage resolve's u3 (Resolve), written per frame.
        hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
        if (!heap)
            rrprivate::CheckAllocation(p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)),
                  "SSS resolve heap allocation failed");
        stride = p.stride;
        const auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        p.device->CreateShaderResourceView(target.Get(), &srv, cpu);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        p.device->CreateUnorderedAccessView(guide.Get(), nullptr, &uav, {cpu.ptr + stride});
        ready = true;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Target() const {
        return rtv->GetCPUDescriptorHandleForHeapStart();
    }
    void Begin(ID3D12GraphicsCommandList *cl) {
        Transition(cl, target.Get(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float sentinel[] = {-65504, -65504, -65504, -65504};
        cl->ClearRenderTargetView(Target(), sentinel, 0, nullptr);
    }
    void End(ID3D12GraphicsCommandList *cl) {
        Transition(cl, target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_COMMON);
    }
    // coverage: a w x h R8_UINT texture in COMMON for the coverage resolve, given exactly
    // when the producer recorded with the marker initializer (Producer::marked); it is left
    // in COMMON. The caller prepares p.sss with coverage before recording.
    template <class Pipelines>
    void Resolve(ID3D12GraphicsCommandList *cl, Pipelines &p, ID3D12Resource *header,
                 ID3D12Resource *stats, ID3D12Resource *history,
                 ID3D12Resource *coverage = nullptr) {
        if (coverage) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.Format = DXGI_FORMAT_R8_UINT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            p.device->CreateUnorderedAccessView(
                coverage, nullptr, &uav,
                {heap->GetCPUDescriptorHandleForHeapStart().ptr + 2 * stride});
            Transition(cl, coverage, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        Transition(cl, target.Get(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(cl, guide.Get(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap *heaps[] = {heap.Get()};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetPipelineState(coverage ? p.sss.coverage_pipeline.Get() : p.sss.pipeline.Get());
        cl->SetComputeRootSignature(coverage ? p.sss.coverage_root.Get() : p.sss.root.Get());
        cl->SetComputeRootConstantBufferView(0, header->GetGPUVirtualAddress());
        const auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
        cl->SetComputeRootDescriptorTable(1, gpu);
        cl->SetComputeRootDescriptorTable(2, {gpu.ptr + stride});
        cl->SetComputeRootUnorderedAccessView(3, stats->GetGPUVirtualAddress());
        cl->SetComputeRootUnorderedAccessView(4, history->GetGPUVirtualAddress());
        if (coverage)
            cl->SetComputeRootDescriptorTable(5, {gpu.ptr + 2 * stride});
        const auto groups = Layout{width, height}.Dispatch();
        cl->Dispatch(groups.ThreadGroupCountX, groups.ThreadGroupCountY, groups.ThreadGroupCountZ);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        cl->ResourceBarrier(1, &barrier);
        Transition(cl, target.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COMMON);
        Transition(cl, guide.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (coverage)
            Transition(cl, coverage, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COMMON);
    }
    void Finish(ID3D12GraphicsCommandList *cl) {
        Transition(cl, guide.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COMMON);
    }
};
} // namespace rrsss
