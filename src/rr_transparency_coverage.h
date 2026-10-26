#pragma once
#include "rr_gpu_common.h"
#include "rr_sss_gpu.h"
#include "rr_transparency_blend_policy.h"
#include "../build/rr_transparency_blend.generated.h"
// GPU side of the transparency albedo blend: the per-slot capture ticket, the
// per-device blend pipeline and the per-slot coverage resources with their
// recording sequences. Every owned resource is COMMON at rest, and every
// recording sequence is noexcept; the caller runs all checks before the first.
namespace rrblend {
using Microsoft::WRL::ComPtr;

// Per slot: this frame's coverage captures. All of them are on one list.
struct Ticket {
    ComPtr<ID3D12GraphicsCommandList> capture; // the list holding the captures
    bool live{};                               // cleared when that list is Reset
    bool submitted{};                          // that list appeared in an ExecuteCommandLists
    uint32_t captures_oit{}, captures_refr{};
    uint32_t captures_refr_alpha{}; // Refraction captures whose variant also wrote alpha
    bool cleared{};                 // the first capture cleared the target on that list
    bool Live() const {
        return live;
    }
    void Clear() {
        *this = {};
    }
};

// One per device. Nothing touches the device before the first Prepare, as
// for rrlive::Pipelines::ProbabilityPipeline.
struct Pipeline {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> state;
    // Typed: the device cannot load and store the R16G16B16A16_FLOAT guides
    // through typed UAVs. Decided once per device; the blend never records.
    Reason unavailable{Reason::None};
    bool Ready() const {
        return bool(state);
    }
    // Typed UAV load and store of the guide format (R16G16B16A16_FLOAT). The SSS
    // guide's R16_FLOAT is in the set TypedUAVLoadAdditionalFormats guarantees.
    static bool TypedLoadSupported(ID3D12Device *d) noexcept {
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        if (!d ||
            FAILED(
                d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) ||
            !options.TypedUAVLoadAdditionalFormats)
            return false;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{DXGI_FORMAT_R16G16B16A16_FLOAT};
        if (FAILED(d->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))))
            return false;
        const UINT needs =
            D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
        return (support.Support2 & needs) == needs;
    }
    // p: rrlive::Pipelines (device, Root, Pipeline). Returns Ready(); false
    // with unavailable == Typed when the device lacks typed UAV loads. Creation
    // failures throw (rrprivate::Check/CheckAllocation).
    template <class Pipelines> bool Prepare(Pipelines &p) {
        rrprivate::Require(p.device && (!device || device.Get() == p.device.Get()),
                           "transparency blend pipeline device changed");
        if (state)
            return true;
        if (unavailable == Reason::Typed)
            return false;
        device = p.device;
        if (!root) {
            // The shader's DXIL declares typed UAV loads of additional formats;
            // without them PSO creation fails, so it is never attempted.
            if (!TypedLoadSupported(device.Get())) {
                unavailable = Reason::Typed;
                return false;
            }
            // [0] b0 control, [1] t0 coverage, t1 status, u0-u2 guides, u4 SSS guide,
            // [2] u3 stats.
            D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
                                               {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 2},
                                               {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 4, 0, 5}};
            D3D12_ROOT_PARAMETER args[3]{};
            args[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            args[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            args[1].DescriptorTable = {3, ranges};
            args[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            args[2].Descriptor.ShaderRegister = 3;
            D3D12_ROOT_SIGNATURE_DESC desc{3, args};
            p.Root(desc, root);
        }
        p.Pipeline(root.Get(), RRTransparencyBlend, sizeof(RRTransparencyBlend), state);
        return true;
    }
};

// The blend shader's Stats words: [0] ran, [4] eligible, [8] covered, [12] opacity*64.
struct PassStats {
    uint32_t ran, eligible, covered, opacity64;
};
static_assert(sizeof(PassStats) == 16);

// Per slot. Caller owns synchronization. Resources and descriptors are
// immutable from recording until BOTH recording retirement and GPU completion
// (the GpuFrame rule). The GPU reads the control buffer when the dispatch
// executes, so WriteControl and Seal must never run while an earlier dispatch
// of this slot may still be in flight; the slot's fence (it is reused only
// after it retired and completed, as for rrtrans::Gpu) guarantees that.
struct Gpu {
    // [0] t0 coverage, [1] t1 status, [2..4] u0-u2 guides, [5] u4 SSS guide
    static constexpr UINT HeapSize = 6;
    ComPtr<ID3D12Resource> target; // R10G10B10A2_UNORM 2DMS transmittance, COMMON at rest
    ComPtr<ID3D12Resource> control, stats, readback;
    ComPtr<ID3D12DescriptorHeap> rtv, heap;
    D3D12_RESOURCE_DESC desc{}; // the target's description, set with target
    UINT64 bytes{};             // this slot's coverage bytes, for reporting only
    UINT stride{};
    UINT *control_map{};
    UINT *readback_map{}; // the GPU copies PassStats here; Record clears those words
    std::array<ID3D12Resource *, 4> described{}; // guides behind heap [1..4] (identity only)
    // The SSS guide behind heap [5] for this frame, or null (a null view;
    // WriteControl then clears SssFlag). Identity only.
    ID3D12Resource *faded{};
    bool ready{}, recorded{};

    // A scene target the coverage target can mirror: 2D with 4 samples in the
    // 0 or standard pattern. Its format and flags do not matter.
    static bool Shape(const D3D12_RESOURCE_DESC &scene) {
        return scene.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && scene.Width &&
               scene.Height && scene.Width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
               scene.Height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
               scene.SampleDesc.Count == 4 &&
               (scene.SampleDesc.Quality == 0 ||
                scene.SampleDesc.Quality == D3D12_STANDARD_MULTISAMPLE_PATTERN);
    }
    static D3D12_RESOURCE_DESC StatsDesc() {
        auto d = rrprivate::Buffer(256);
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return d;
    }
    // The budgeted resources in allocation order: target, control, stats and
    // readback. Descriptor heaps are not budgeted.
    static std::array<D3D12_RESOURCE_DESC, 4> Descs(const D3D12_RESOURCE_DESC &scene) {
        return {CoverageDesc(scene), rrprivate::Buffer(256), StatsDesc(), rrprivate::Buffer(256)};
    }
    template <class Query>
    static UINT64 RequiredBytes(const D3D12_RESOURCE_DESC &scene, Query query) {
        if (!Shape(scene))
            return 0;
        UINT64 total{};
        for (const auto &d : Descs(scene)) {
            const UINT64 n = query(d);
            if (!n || n == UINT64_MAX || total > UINT64_MAX - n)
                return 0;
            total += n;
        }
        return total;
    }
    static UINT64 RequiredBytes(ID3D12Device *device, const D3D12_RESOURCE_DESC &scene) {
        return device ? RequiredBytes(
                            scene,
                            [&](const D3D12_RESOURCE_DESC &d) {
                                return device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
                            })
                      : 0;
    }
    bool Matches(const D3D12_RESOURCE_DESC &scene) const {
        return Shape(scene) &&
               (!target || (desc.Width == scene.Width && desc.Height == scene.Height &&
                            desc.SampleDesc.Count == scene.SampleDesc.Count &&
                            desc.SampleDesc.Quality == scene.SampleDesc.Quality));
    }
    bool Ready() const {
        return ready;
    }
    // Bytes the budgeted resources that are still null need. A zero, unknown
    // or overflowing size is a hard failure.
    UINT64 Missing(ID3D12Device *device, const D3D12_RESOURCE_DESC &scene) const {
        const ID3D12Resource *resources[] = {target.Get(), control.Get(), stats.Get(),
                                             readback.Get()};
        const auto descs = Descs(scene);
        UINT64 total{};
        for (size_t i = 0; i < descs.size(); ++i) {
            if (resources[i])
                continue;
            const UINT64 n = device->GetResourceAllocationInfo(0, 1, &descs[i]).SizeInBytes;
            rrprivate::Require(n && n != UINT64_MAX && total <= UINT64_MAX - n,
                               "coverage allocation size unavailable");
            total += n;
        }
        return total;
    }
    // Resumable: each resource is created only while it is null. Every
    // resource goes through owner.Allocate (rrlive::GpuFrame::Allocate), so it
    // is budget-checked and added to the frame's and the budget's bytes; bytes
    // keeps this slot's share for reporting.
    // Precondition (binding on the caller): the caller must already have
    // reserved the pool with coverage on every slot, e.g. ReservePool with
    // extra_persistent = slot count * (RequiredBytes(device, scene) - the
    // largest coverage bytes a slot already holds). Once allocated, coverage
    // is in GpuFrame::bytes, and the pool projections take the largest slot
    // base times the slot count, so coverage taken from existing headroom
    // instead would make later mandatory pool reservations exceed the
    // ceiling, fail, and disable RR.
    // Prepare itself only rechecks each allocation: it sizes the still-null
    // resources and, when the ledger cannot hold them, asks budget.Reserve to
    // grow the ceiling by that much (only when DXGI has room). When Reserve
    // refuses (nothing is allocated) or Allocate defers on its DXGI check,
    // this returns false, and the caller reports Reason::Memory and retries
    // after 1 s. Other failures throw.
    template <class Owner, class Pipelines, class Budget>
    bool Prepare(Owner &owner, Pipelines &p, Budget &budget, const D3D12_RESOURCE_DESC &scene) {
        using rrprivate::Check;
        using rrprivate::CheckAllocation;
        rrprivate::Require(Matches(scene),
                           "coverage target extent or sample layout changed before retirement");
        if (ready)
            return true;
        const UINT64 needed = Missing(p.device.Get(), scene);
        rrprivate::Require(needed <= UINT64_MAX - budget.bytes, "coverage memory ledger overflow");
        if (needed && (budget.bytes > budget.limit || needed > budget.limit - budget.bytes) &&
            !budget.Reserve(p.device.Get(), budget.bytes + needed))
            return false;
        auto allocate = [&](ComPtr<ID3D12Resource> &r, const D3D12_RESOURCE_DESC &d,
                            D3D12_HEAP_TYPE type, const D3D12_CLEAR_VALUE *clear = nullptr) {
            if (r)
                return;
            const auto before = owner.bytes;
            r = owner.Allocate(p, budget, d, type, clear);
            bytes += owner.bytes - before;
        };
        const auto descs = Descs(scene);
        try {
            if (!target) {
                // T = 1: fully transmissive in every channel and in alpha.
                const D3D12_CLEAR_VALUE transmissive{DXGI_FORMAT_R10G10B10A2_UNORM,
                                                     {{1.f, 1.f, 1.f, 1.f}}};
                allocate(target, descs[0], D3D12_HEAP_TYPE_DEFAULT, &transmissive);
                desc = descs[0];
            }
            allocate(control, descs[1], D3D12_HEAP_TYPE_UPLOAD);
            allocate(stats, descs[2], D3D12_HEAP_TYPE_DEFAULT);
            allocate(readback, descs[3], D3D12_HEAP_TYPE_READBACK);
            D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
            if (!rtv)
                CheckAllocation(p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv)),
                                "coverage RTV heap allocation failed");
            hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, HeapSize,
                  D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
            if (!heap)
                CheckAllocation(p.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)),
                                "coverage blend heap allocation failed");
        } catch (const rrprivate::AllocationDeferred &) {
            return false;
        }
        void *m{};
        if (!control_map) {
            D3D12_RANGE none{};
            Check(control->Map(0, &none, &m), "coverage control map failed");
            memset(m, 0, 256); // includes Control::zero, the stats zero source
            control_map = static_cast<UINT *>(m);
        }
        if (!readback_map) {
            Check(readback->Map(0, nullptr, &m), "coverage readback map failed");
            readback_map = static_cast<UINT *>(m);
        }
        stride = p.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        p.device->CreateRenderTargetView(target.Get(), &view,
                                         rtv->GetCPUDescriptorHandleForHeapStart());
        // t0 is always a real 4-sample SRV of this slot's target: a null or
        // smaller coverage would load 0, which reads as full opacity.
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        p.device->CreateShaderResourceView(target.Get(), &srv,
                                           heap->GetCPUDescriptorHandleForHeapStart());
        ready = true;
        return true;
    }
    // An SSS guide the blend can fade in place: rrsss::Gpu::GuideDesc, the
    // render size of the albedo guides.
    static bool SssUsable(const D3D12_RESOURCE_DESC &d, const D3D12_RESOURCE_DESC &guide) {
        return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.DepthOrArraySize == 1 &&
               d.SampleDesc.Count == 1 && d.Format == DXGI_FORMAT_R16_FLOAT &&
               (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) && d.Width == guide.Width &&
               d.Height == guide.Height;
    }
    // Heap [1] Status SRV and [2..4] guide UAVs, written once per slot after
    // the guides exist (GpuFrame::guides: normal/roughness, diffuse, specular,
    // status). They stay unchanged until the slot is released, so a later call
    // only checks that it names the same guides.
    // Heap [5] is the SSS guide's UAV (u4), rewritten on every call: the guide
    // is used only on frames whose SSS draw was resolved. Safe for the same
    // reason as WriteControl: this slot's previous dispatch has completed. sss
    // is null on other frames; then, or when it is not SssUsable, [5] is a
    // null view and faded is null, so WriteControl clears SssFlag.
    template <class Pipelines>
    void WriteDescriptors(Pipelines &p, const std::array<ComPtr<ID3D12Resource>, 4> &guides,
                          ID3D12Resource *sss = nullptr) {
        using rrprivate::Require;
        Require(ready, "coverage blend resources not prepared");
        std::array<ID3D12Resource *, 4> resources{};
        for (unsigned i = 0; i < 4; ++i)
            resources[i] = guides[i].Get();
        if (Described())
            Require(resources == described, "coverage blend guides changed before retirement");
        else
            DescribeGuides(p, resources);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        faded = sss && SssUsable(sss->GetDesc(), resources[0]->GetDesc()) ? sss : nullptr;
        p.device->CreateUnorderedAccessView(
            faded, nullptr, &uav,
            {heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(5) * stride});
    }
    template <class Pipelines>
    void DescribeGuides(Pipelines &p, const std::array<ID3D12Resource *, 4> &resources) {
        using rrprivate::Require;
        for (unsigned i = 0; i < 4; ++i) {
            Require(resources[i], "coverage blend guide missing");
            const auto d = resources[i]->GetDesc();
            Require(d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.SampleDesc.Count == 1 &&
                        d.Format ==
                            (i == 3 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16G16B16A16_FLOAT) &&
                        (i == 3 || (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)),
                    "coverage blend guide layout unexpected");
        }
        const auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_UINT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        p.device->CreateShaderResourceView(resources[3], &srv, {cpu.ptr + stride});
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        for (unsigned i = 0; i < 3; ++i)
            p.device->CreateUnorderedAccessView(resources[i], nullptr, &uav,
                                                {cpu.ptr + SIZE_T(2 + i) * stride});
        described = resources;
    }
    bool Described() const {
        return described[0] != nullptr;
    }
    // Capture bracket on the capture list; the replay goes in between and
    // renders into the returned RTV. The first capture of a frame clears.
    D3D12_CPU_DESCRIPTOR_HANDLE BeginCapture(ID3D12GraphicsCommandList *cl, bool first) noexcept {
        rrsss::Transition(cl, target.Get(), D3D12_RESOURCE_STATE_COMMON,
                          D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto view = rtv->GetCPUDescriptorHandleForHeapStart();
        if (first) {
            const float transmissive[] = {1.f, 1.f, 1.f, 1.f}; // alpha too
            cl->ClearRenderTargetView(view, transmissive, 0, nullptr);
        }
        return view;
    }
    void EndCapture(ID3D12GraphicsCommandList *cl) noexcept {
        rrsss::Transition(cl, target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_COMMON);
    }
    // The whole control buffer, except that Control::zero, the source Record
    // clears the stats from, stays zero whatever c holds, and SssFlag is
    // cleared unless the last WriteDescriptors wrote a real SSS guide view.
    void WriteControl(const Control &c) noexcept {
        if (!control_map)
            return;
        auto k = c;
        if (!faded)
            k.flags &= ~SssFlag;
        memset(k.zero, 0, sizeof(k.zero));
        memcpy(control_map, &k, sizeof(k));
    }
    // Submission-time gate: word 0 only.
    void Seal(bool admitted) noexcept {
        if (control_map)
            control_map[0] = admitted ? 1u : 0u;
    }
    // GPU address of control word 0 (Admitted), for other passes gated on it.
    D3D12_GPU_VIRTUAL_ADDRESS ControlAddress() const noexcept {
        return control ? control->GetGPUVirtualAddress() : 0;
    }
    // The stats the last Record copied back, once per Record; zero otherwise,
    // and ran == 0 when that Record's list never executed. Call it only once
    // that recording's slot has completed (slot reuse guarantees it), so the
    // values describe an earlier frame of this slot.
    PassStats Harvest() noexcept {
        PassStats s{};
        if (!recorded || !readback_map)
            return s;
        recorded = false;
        memcpy(&s, readback_map, sizeof(s));
        return s;
    }
    // On the SR list, after GateBegin (guides in NON_PIXEL_SHADER_RESOURCE),
    // before NGX evaluate. guides as in WriteDescriptors; guides[3] (status)
    // keeps its state. The SSS guide WriteDescriptors last named, if any, is
    // in NON_PIXEL_SHADER_RESOURCE too (after rrsss::Gpu::Resolve). The caller has prepared
    // everything: this records only.
    void Record(ID3D12GraphicsCommandList *cl, const Pipeline &pipeline,
                const std::array<ComPtr<ID3D12Resource>, 4> &guides, UINT w, UINT h) noexcept {
        using rrsss::Transition;
        constexpr auto common = D3D12_RESOURCE_STATE_COMMON,
                       read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Transition(cl, target.Get(), common, read);
        for (unsigned i = 0; i < 3; ++i)
            Transition(cl, guides[i].Get(), read, uav);
        if (faded)
            Transition(cl, faded, read, uav);
        Transition(cl, stats.Get(), common, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->CopyBufferRegion(stats.Get(), 0, control.Get(), offsetof(Control, zero),
                             sizeof(Control::zero));
        Transition(cl, stats.Get(), D3D12_RESOURCE_STATE_COPY_DEST, uav);
        ID3D12DescriptorHeap *heaps[] = {heap.Get()};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(pipeline.root.Get());
        cl->SetPipelineState(pipeline.state.Get());
        cl->SetComputeRootConstantBufferView(0, control->GetGPUVirtualAddress());
        cl->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
        cl->SetComputeRootUnorderedAccessView(2, stats->GetGPUVirtualAddress());
        cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER order{};
        order.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        cl->ResourceBarrier(1, &order);
        for (unsigned i = 0; i < 3; ++i)
            Transition(cl, guides[i].Get(), uav, read);
        if (faded)
            Transition(cl, faded, uav, read);
        Transition(cl, target.Get(), read, common);
        Transition(cl, stats.Get(), uav, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->CopyBufferRegion(readback.Get(), 0, stats.Get(), 0, sizeof(PassStats));
        Transition(cl, stats.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, common);
        // Clear the readback words now, so a list that never executes leaves
        // ran == 0 for the next Harvest instead of the previous dispatch's
        // stats again. Safe: this slot's previous dispatch has completed (the
        // slot is reused only after its fence), so its copy has already landed.
        if (readback_map)
            memset(readback_map, 0, sizeof(PassStats));
        recorded = true;
    }
    // Unmaps and drops everything. The bytes stay in the owner's ledger:
    // GpuFrame::Release returns them with the frame's.
    void Release() noexcept {
        if (control_map)
            control->Unmap(0, nullptr);
        if (readback_map) {
            // Empty written range: it is tooling-only, and a non-empty one on a
            // READBACK heap trips the debug layer's UNMAP_RANGE_NOT_EMPTY.
            const D3D12_RANGE none{};
            readback->Unmap(0, &none);
        }
        *this = {};
    }
};
} // namespace rrblend
