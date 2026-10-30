#pragma once
#include "rr_transparency_policy.h"
#include "rr_transparency_admission.h"
#include "rr_gpu_common.h"
#include "feed_fg_identity.h"
namespace rrtrans {
using Microsoft::WRL::ComPtr;
using rrprivate::Check;
using rrprivate::Require;
inline bool Shape(const D3D12_RESOURCE_DESC &d, UINT w, UINT h) {
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && Extent(d.Width, d.Height, w, h) &&
           w <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION && h <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
           d.DepthOrArraySize == 1 && d.MipLevels == 1 && d.SampleDesc.Count == 4 &&
           (d.SampleDesc.Quality == 0 ||
            d.SampleDesc.Quality == D3D12_STANDARD_MULTISAMPLE_PATTERN) &&
           d.Format == DXGI_FORMAT_R11G11B10_FLOAT &&
           !(d.Flags &
             (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE |
              D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS));
}
inline D3D12_RESOURCE_DESC SnapshotDesc(D3D12_RESOURCE_DESC d) {
    d.Alignment = 0;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return d;
}
inline D3D12_RESOURCE_DESC GuideDesc(UINT w, UINT h) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return d;
}
inline UINT64 RequiredBytes(ID3D12Device *device, const D3D12_RESOURCE_DESC &d, UINT w, UINT h) {
    if (!device || !Shape(d, w, h))
        return 0;
    UINT64 total{};
    for (const auto &desc : {SnapshotDesc(d), GuideDesc(w, h)}) {
        const auto n = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        if (!n || n == UINT64_MAX || total > UINT64_MAX - n)
            return 0;
        total += n;
    }
    return total;
}
inline void Change(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES from,
                   D3D12_RESOURCE_STATES to) {
    if (from == to)
        return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    cl->ResourceBarrier(1, &b);
}
struct Pipeline {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root, gated_root;
    ComPtr<ID3D12PipelineState> state, gated_state;
    UINT stride{};
    void Initialize(ID3D12Device *d) {
        if (state && gated_state) {
            Require(fg::SameNativeObject(d, device.Get()), "glass guide pipeline device changed");
            return;
        }
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{DXGI_FORMAT_R11G11B10_FLOAT};
        Check(d->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)),
              "glass format support query failed");
        Require(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE,
                "glass R11 typed UAV store unavailable");
        for (unsigned gated = 0; gated < 2; ++gated) {
            D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                               {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0}};
            D3D12_ROOT_PARAMETER p[3]{};
            for (UINT i = 0; i < 2; ++i) {
                p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                p[i].DescriptorTable = {1, &ranges[i]};
            }
            p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            p[2].Descriptor = {0, 0};
            D3D12_ROOT_SIGNATURE_DESC desc{gated ? 3u : 2u, p};
            ComPtr<ID3DBlob> signature, shader, error;
            Check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature,
                                              &error),
                  "glass root serialize failed");
            auto &selected_root = gated ? gated_root : root;
            auto &selected_state = gated ? gated_state : state;
            Check(d->CreateRootSignature(0, signature->GetBufferPointer(),
                                         signature->GetBufferSize(), IID_PPV_ARGS(&selected_root)),
                  "glass root creation failed");
            // Rejected submission: never load stale/unwritten producer texels. The
            // private textures always retain valid COMMON/read/UAV transitions, even
            // when producer and consumer execute in the rejected order.
            const char *code =
                gated ? "cbuffer C:register(b0){uint4 h0;uint4 h1;}Texture2DMS<float3,4> "
                        "src:register(t0);RWTexture2D<float3> "
                        "dst:register(u0);[numthreads(8,8,1)]void main(uint3 "
                        "id:SV_DispatchThreadID){uint "
                        "w,h;dst.GetDimensions(w,h);if(id.x>=w||id.y>=h)return;[branch]if(h1.z==0){"
                        "dst[id.xy]=float3(0,0,0);return;}uint "
                        "sample=(id.x&1)|((id.y&1)<<1);dst[id.xy]=src.Load(int2(id.xy/2),sample);}"
                      : "Texture2DMS<float3,4> src:register(t0);RWTexture2D<float3> "
                        "dst:register(u0);[numthreads(8,8,1)]void main(uint3 "
                        "id:SV_DispatchThreadID){uint "
                        "w,h;dst.GetDimensions(w,h);if(id.x>=w||id.y>=h)return;uint "
                        "sample=(id.x&1)|((id.y&1)<<1);dst[id.xy]=src.Load(int2(id.xy/2),sample);}";
            Check(D3DCompile(code, strlen(code), nullptr, nullptr, nullptr, "main", "cs_5_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &error),
                  "glass unpack shader compile failed");
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = selected_root.Get();
            pd.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
            Check(d->CreateComputePipelineState(&pd, IID_PPV_ARGS(&selected_state)),
                  "glass unpack pipeline failed");
        }
        device = d;
        stride = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
};
struct Gpu {
    ComPtr<ID3D12Resource> snapshot, guide;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT w{}, h{};
    UINT64 bytes{};
    bool Matches(const D3D12_RESOURCE_DESC &d, UINT rw, UINT rh) const {
        return Shape(d, rw, rh) &&
               (!snapshot ||
                (w == rw && h == rh && snapshot->GetDesc().SampleDesc.Count == d.SampleDesc.Count &&
                 snapshot->GetDesc().SampleDesc.Quality == d.SampleDesc.Quality));
    }
    template <class Owner, class Pipelines, class Budget>
    void Prepare(Owner &owner, Pipelines &p, Budget &budget, const D3D12_RESOURCE_DESC &d, UINT rw,
                 UINT rh) {
        Require(Matches(d, rw, rh), "glass guide sample layout or extent changed");
        if (snapshot) {
            Require(guide && heap, "glass guide allocation incomplete");
            return;
        }
        const auto before = owner.bytes;
        // All or nothing, so a later try allocates again instead of finding half a guide.
        try {
            snapshot = owner.Allocate(p, budget, SnapshotDesc(d));
            guide = owner.Allocate(p, budget, GuideDesc(rw, rh));
            D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                          D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
            Check(p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)),
                  "glass immutable heap failed");
        } catch (...) {
            snapshot.Reset();
            guide.Reset();
            heap.Reset();
            budget.bytes -= owner.bytes - before;
            owner.bytes = before;
            throw;
        }
        bytes = owner.bytes - before;
        w = rw;
        h = rh;
        const auto stride =
            p.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R11G11B10_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        p.device->CreateShaderResourceView(snapshot.Get(), &srv, cpu);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R11G11B10_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        p.device->CreateUnorderedAccessView(guide.Get(), nullptr, &uav, {cpu.ptr + stride});
    }
    void Copy(ID3D12GraphicsCommandList *cl, ID3D12Resource *source,
              D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET) {
        Require(
            source && snapshot && Matches(source->GetDesc(), w, h) &&
                (state == D3D12_RESOURCE_STATE_RENDER_TARGET || ProducerReadState(unsigned(state))),
            "glass snapshot copy source/state invalid");
        Change(cl, source, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Change(cl, snapshot.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyResource(snapshot.Get(), source);
        Change(cl, snapshot.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Change(cl, source, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    }
    void Resolve(ID3D12GraphicsCommandList *cl, Pipeline &p, ID3D12Resource *header = nullptr) {
        Require(snapshot && guide && heap, "glass snapshot unavailable");
        Change(cl, snapshot.Get(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Change(cl, guide.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap *heaps[] = {heap.Get()};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(header ? p.gated_root.Get() : p.root.Get());
        cl->SetPipelineState(header ? p.gated_state.Get() : p.state.Get());
        if (header)
            cl->SetComputeRootConstantBufferView(2, header->GetGPUVirtualAddress());
        auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
        cl->SetComputeRootDescriptorTable(0, gpu);
        cl->SetComputeRootDescriptorTable(1, {gpu.ptr + p.stride});
        cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        Change(cl, guide.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Change(cl, snapshot.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COMMON);
    }
    void Finish(ID3D12GraphicsCommandList *cl) {
        Change(cl, guide.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COMMON);
    }
};
struct Ticket {
    bool producer_copy{}, producer_pending{}, producer_ready{};
    UINT64 producer_recording{};
    D3D12_RESOURCE_STATES producer_state{};
    ComPtr<ID3D12Resource> source;
    ComPtr<ID3D12GraphicsCommandList> preparation, anchor;
    bool prep_live{}, anchor_live{}, prep_submitted{}, anchor_submitted{}, written{}, anchored{},
        at_draw{};
    // Mirrors the slot's coverage capture ticket (rrblend::Ticket::live), so the
    // controller's slot-retire check keeps a slot whose capture list is live.
    bool coverage_live{};
    bool Live() const {
        return prep_live || anchor_live || coverage_live;
    }
};
} // namespace rrtrans
