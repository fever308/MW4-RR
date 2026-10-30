#pragma once
// Subsurface Scattering coverage experiment (rr_sss_coverage.h): the GPU history ring.
// Included by rr_live.h after rr_live_gpu.h.
#include "rr_live_gpu.h"
#include "rr_sss_coverage.h"
#include "../build/rr_sss_coverage.generated.h"
namespace rrlive {
// One entry per frame (rrssscov::Entry): the surface key written right after that frame's
// prepass, the coverage its Subsurface Scattering resolve writes, and the lighting pass's
// counters with their readback. Every texture and the counters are COMMON outside the passes
// that use them; the readback stays COPY_DEST. Allocated once, all four entries together,
// while the experiment is wanted, for the lighting extent; released only by Reconfigure.
struct CoverageHistory {
    struct Entry {
        ComPtr<ID3D12Resource> key, coverage, counters, readback;
        const uint32_t *map{};
        // The frame whose key / coverage the entry holds (UINT_MAX: none), and its RR
        // generation. counted: this frame's lighting pass recorded counters into readback.
        UINT key_frame = UINT_MAX, coverage_frame = UINT_MAX, generation{}, age{};
        bool counted{};
    };
    std::array<Entry, rrssscov::Ring> entries;
    // The key pass's descriptors (0: PackedSurface SRV, 1: key UAV). The caller picks a set
    // no slot with GPU work in flight holds (Slot::coverage_set), so a set is never rewritten
    // under the GPU however far behind it runs or however frames are numbered (the entry's
    // textures are ordered by submission; only descriptors need this).
    static constexpr unsigned Sets = 16;
    std::array<ComPtr<ID3D12DescriptorHeap>, Sets> heaps;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    UINT w{}, h{};
    UINT64 bytes{};
    bool ready{};
    static std::array<D3D12_RESOURCE_DESC, 4> Descs(UINT w, UINT h) {
        auto counters = rrprivate::Buffer(rrssscov::CounterBytes);
        counters.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return {TextureDesc(w, h, DXGI_FORMAT_R32_UINT), TextureDesc(w, h, DXGI_FORMAT_R8_UINT),
                counters, rrprivate::Buffer(rrssscov::CounterBytes)};
    }
    // The whole ring's allocation size, 0 when it cannot be expressed.
    static UINT64 Bytes(ID3D12Device *device, UINT w, UINT h) {
        if (!device || !w || !h)
            return 0;
        UINT64 total{};
        for (const auto &d : Descs(w, h)) {
            const auto n = device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
            if (!n || n == UINT64_MAX || n > UINT64_MAX / rrssscov::Ring ||
                total > UINT64_MAX - n * rrssscov::Ring)
                return 0;
            total += n * rrssscov::Ring;
        }
        return total;
    }
    bool Matches(UINT width, UINT height) const {
        return ready && w == width && h == height;
    }
    // ledger: the persistent frame whose allocations Reconfigure releases. Throws as
    // GpuFrame::Allocate does; a partial ring is released by the caller (*this = {}).
    void Prepare(Pipelines &p, Budget &b, GpuFrame &ledger, UINT width, UINT height) {
        Require(!ready, "SSS coverage history already prepared");
        D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                           {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0}};
        D3D12_ROOT_PARAMETER args[4]{};
        args[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        args[0].Constants = {0, 0, 2};
        for (unsigned i = 0; i < 2; ++i) {
            args[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            args[1 + i].DescriptorTable = {1, &ranges[i]};
        }
        args[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        args[3].Descriptor.ShaderRegister = 1;
        D3D12_ROOT_SIGNATURE_DESC desc{4, args};
        p.Root(desc, root);
        p.Pipeline(root.Get(), RRSSSCoverageKey, sizeof(RRSSSCoverageKey), pipeline);
        // The resolve's coverage variant, so recording never creates a pipeline.
        p.sss.Prepare(p, true);
        const auto descs = Descs(width, height);
        const UINT64 before = ledger.bytes;
        for (auto &e : entries) {
            e.key = ledger.Allocate(p, b, descs[0]);
            e.coverage = ledger.Allocate(p, b, descs[1]);
            e.counters = ledger.Allocate(p, b, descs[2]);
            e.readback = ledger.Allocate(p, b, descs[3], D3D12_HEAP_TYPE_READBACK);
            void *m{};
            Check(e.readback->Map(0, nullptr, &m), "SSS coverage readback map failed");
            e.map = static_cast<const uint32_t *>(m);
        }
        for (auto &heap : heaps) {
            D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                          D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
            rrprivate::CheckAllocation(p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)),
                                       "SSS coverage descriptor heap failed");
        }
        bytes = ledger.bytes - before;
        w = width;
        h = height;
        ready = true;
    }
    // Records this frame's surface key from the prepass PackedSurface (UNORDERED_ACCESS, as
    // the prepass leaves it) and zeroes the frame's counters. The caller holds a proven
    // prepass recording; the descriptors of set (< Sets) are written for this recording
    // only, and no frame in flight uses that set.
    bool Key(ID3D12GraphicsCommandList *cl, Pipelines &p, ID3D12Resource *surface, UINT frame,
             UINT generation, unsigned set) {
        if (!ready || !cl || !surface || set >= Sets)
            return false;
        const auto d = surface->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width != w || d.Height != h ||
            d.Format != DXGI_FORMAT_R32G32B32A32_UINT || d.SampleDesc.Count != 1)
            return false;
        auto &e = entries[rrssscov::Entry(frame)];
        auto &heap = heaps[set];
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = d.Format;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipLevels = 1;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        p.device->CreateShaderResourceView(surface, &v, heap->GetCPUDescriptorHandleForHeapStart());
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = DXGI_FORMAT_R32_UINT;
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        p.device->CreateUnorderedAccessView(
            e.key.Get(), nullptr, &u, {heap->GetCPUDescriptorHandleForHeapStart().ptr + p.stride});
        Change(cl, surface, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Change(cl, e.key.Get(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Change(cl, e.counters.Get(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap *bound[] = {heap.Get()};
        cl->SetDescriptorHeaps(1, bound);
        cl->SetComputeRootSignature(root.Get());
        cl->SetPipelineState(pipeline.Get());
        const UINT size[] = {w, h};
        cl->SetComputeRoot32BitConstants(0, 2, size, 0);
        const auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
        cl->SetComputeRootDescriptorTable(1, gpu);
        cl->SetComputeRootDescriptorTable(2, {gpu.ptr + p.stride});
        cl->SetComputeRootUnorderedAccessView(3, e.counters->GetGPUVirtualAddress());
        cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        Change(cl, e.counters.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COMMON);
        Change(cl, e.key.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COMMON);
        Change(cl, surface, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        e.key_frame = frame;
        e.coverage_frame = UINT_MAX;
        e.generation = generation;
        e.counted = false;
        return true;
    }
    void Release() {
        for (auto &e : entries)
            if (e.readback && e.map)
                e.readback->Unmap(0, nullptr);
        *this = {};
    }
};
} // namespace rrlive
