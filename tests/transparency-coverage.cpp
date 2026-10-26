#define NOMINMAX
#include "../src/rr_transparency_coverage.h"
#include "../src/rr_live_gpu.h"
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// CPU-only COM doubles. No device, adapter, queue, or GPU is created. The
// doubles log every device and command-list call, so the tests compare exact
// call sequences.
using Microsoft::WRL::ComPtr;
using namespace rrblend;

static std::vector<std::string> events;
static std::map<const void *, std::string> names;
static std::string Format(const char *format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}
static const char *Name(const void *object) {
    if (!object)
        return "null";
    const auto it = names.find(object);
    return it == names.end() ? "unnamed" : it->second.c_str();
}
static std::string Transition(const char *name, D3D12_RESOURCE_STATES before,
                              D3D12_RESOURCE_STATES after) {
    return Format("transition %s %#x->%#x sub=%#x flags=0", name, unsigned(before), unsigned(after),
                  D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
}

// Objects are owned by the test, which holds the first reference; Release
// never deletes, so refs == 1 means no ComPtr holds the object any more.
template <class I> struct Com : I {
    ULONG refs = 1;
    explicit Com(const char *name) {
        names[static_cast<I *>(this)] = name;
    }
    Com(const Com &) = delete;
    Com &operator=(const Com &) = delete;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(I)) {
            *out = static_cast<I *>(this);
            ++refs;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        return --refs;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override {
        return S_OK;
    }
};
template <class I> struct Child : Com<I> {
    using Com<I>::Com;
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void **out) override {
        *out = nullptr;
        return E_NOTIMPL;
    }
};

struct FakeResource final : Child<ID3D12Resource> {
    D3D12_RESOURCE_DESC desc{};
    D3D12_GPU_VIRTUAL_ADDRESS address{};
    alignas(16) unsigned char memory[256]{};
    unsigned unmaps{};
    FakeResource(const char *name, const D3D12_RESOURCE_DESC &d, D3D12_GPU_VIRTUAL_ADDRESS a = 0)
        : Child(name), desc(d), address(a) {}
    HRESULT STDMETHODCALLTYPE Map(UINT subresource, const D3D12_RANGE *read, void **out) override {
        events.push_back(read ? Format("map %s sub=%u read=%zu-%zu", Name(this), subresource,
                                       read->Begin, read->End)
                              : Format("map %s sub=%u read=all", Name(this), subresource));
        *out = memory;
        return S_OK;
    }
    void STDMETHODCALLTYPE Unmap(UINT subresource, const D3D12_RANGE *written) override {
        ++unmaps;
        events.push_back(written ? Format("unmap %s sub=%u written=%zu-%zu", Name(this),
                                          subresource, written->Begin, written->End)
                                 : Format("unmap %s sub=%u written=all", Name(this), subresource));
    }
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override {
        return desc;
    }
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override {
        return address;
    }
    HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT, const D3D12_BOX *, const void *, UINT,
                                                 UINT) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(void *, UINT, UINT, UINT,
                                                  const D3D12_BOX *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES *,
                                                D3D12_HEAP_FLAGS *) override {
        return E_NOTIMPL;
    }
};
struct FakeHeap final : Child<ID3D12DescriptorHeap> {
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    SIZE_T cpu{};
    UINT64 gpu{};
    FakeHeap(const char *name, const D3D12_DESCRIPTOR_HEAP_DESC &d, SIZE_T c, UINT64 g)
        : Child(name), desc(d), cpu(c), gpu(g) {}
    D3D12_DESCRIPTOR_HEAP_DESC STDMETHODCALLTYPE GetDesc() override {
        return desc;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetCPUDescriptorHandleForHeapStart() override {
        return {cpu};
    }
    D3D12_GPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetGPUDescriptorHandleForHeapStart() override {
        return {gpu};
    }
};
struct FakeRoot final : Child<ID3D12RootSignature> {
    using Child::Child;
};
struct FakePso final : Child<ID3D12PipelineState> {
    using Child::Child;
    HRESULT STDMETHODCALLTYPE GetCachedBlob(ID3DBlob **) override {
        return E_NOTIMPL;
    }
};

struct FakeDevice final : Com<ID3D12Device> {
    BOOL typed = TRUE;
    UINT support2 = D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
    HRESULT options_result = S_OK, format_result = S_OK, heap_result = S_OK;
    unsigned heap_fail_at = ~0u, heaps_created = 0;
    UINT64 texture_size = 8ull << 20, buffer_size = 64ull << 10;
    std::deque<FakeHeap> heaps;
    std::deque<FakeRoot> roots;
    std::deque<FakePso> pipelines;
    std::vector<unsigned char> root_blob;
    const void *cs{};
    size_t cs_size{};
    explicit FakeDevice(const char *name = "device") : Com(name) {}
    void Unexpected(const char *what) {
        events.push_back(Format("unexpected device %s", what));
    }
    UINT STDMETHODCALLTYPE GetNodeCount() override {
        Unexpected("GetNodeCount");
        return 1;
    }
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC *, REFIID,
                                                 void **) override {
        Unexpected("CreateCommandQueue");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE, REFIID,
                                                     void **) override {
        Unexpected("CreateCommandAllocator");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC *, REFIID, void **) override {
        Unexpected("CreateGraphicsPipelineState");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC *d,
                                                         REFIID id, void **out) override {
        events.push_back(Format("create-compute-pipeline root=%s cs=%zu", Name(d->pRootSignature),
                                d->CS.BytecodeLength));
        cs = d->CS.pShaderBytecode;
        cs_size = d->CS.BytecodeLength;
        auto &p = pipelines.emplace_back("blend-pso");
        return p.QueryInterface(id, out);
    }
    HRESULT STDMETHODCALLTYPE CreateCommandList(UINT, D3D12_COMMAND_LIST_TYPE,
                                                ID3D12CommandAllocator *, ID3D12PipelineState *,
                                                REFIID, void **) override {
        Unexpected("CreateCommandList");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE feature, void *data,
                                                  UINT size) override {
        switch (feature) {
        case D3D12_FEATURE_D3D12_OPTIONS:
            assert(size == sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS));
            events.push_back("check-options");
            static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS *>(data)->TypedUAVLoadAdditionalFormats =
                typed;
            return options_result;
        case D3D12_FEATURE_FORMAT_SUPPORT: {
            assert(size == sizeof(D3D12_FEATURE_DATA_FORMAT_SUPPORT));
            auto *d = static_cast<D3D12_FEATURE_DATA_FORMAT_SUPPORT *>(data);
            events.push_back(Format("check-format %u", unsigned(d->Format)));
            d->Support1 = D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
            d->Support2 = D3D12_FORMAT_SUPPORT2(support2);
            return format_result;
        }
        case D3D12_FEATURE_ARCHITECTURE:
            events.push_back("check-architecture");
            return E_FAIL; // the budget's VRAM query never succeeds here
        default:
            events.push_back(Format("check-feature %u", unsigned(feature)));
            return E_NOTIMPL;
        }
    }
    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC *d, REFIID id,
                                                   void **out) override {
        events.push_back(Format("create-heap type=%u n=%u flags=%u mask=%u", unsigned(d->Type),
                                d->NumDescriptors, unsigned(d->Flags), d->NodeMask));
        if (heaps_created++ == heap_fail_at) {
            *out = nullptr;
            return heap_result;
        }
        const bool rtv = d->Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        const auto n = SIZE_T(heaps.size() + 1);
        auto &h = heaps.emplace_back(rtv ? "rtv-heap" : "heap", *d, n * 0x10000,
                                     d->Flags ? n * 0x10000000 : 0);
        return h.QueryInterface(id, out);
    }
    UINT STDMETHODCALLTYPE
    GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE type) override {
        return type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ? 32 : 16;
    }
    HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT mask, const void *blob, SIZE_T bytes,
                                                  REFIID id, void **out) override {
        events.push_back(Format("create-root-signature mask=%u", mask));
        root_blob.assign(static_cast<const unsigned char *>(blob),
                         static_cast<const unsigned char *>(blob) + bytes);
        auto &r = roots.emplace_back("blend-root");
        return r.QueryInterface(id, out);
    }
    void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC *,
                                                    D3D12_CPU_DESCRIPTOR_HANDLE) override {
        Unexpected("CreateConstantBufferView");
    }
    void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource *r,
                                                    const D3D12_SHADER_RESOURCE_VIEW_DESC *d,
                                                    D3D12_CPU_DESCRIPTOR_HANDLE at) override {
        std::string view = "desc=null";
        if (d && d->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D)
            view =
                Format("fmt=%u dim=%u map=%#x most=%u mips=%u plane=%u clamp=%g",
                       unsigned(d->Format), unsigned(d->ViewDimension), d->Shader4ComponentMapping,
                       d->Texture2D.MostDetailedMip, d->Texture2D.MipLevels,
                       d->Texture2D.PlaneSlice, double(d->Texture2D.ResourceMinLODClamp));
        else if (d)
            view = Format("fmt=%u dim=%u map=%#x", unsigned(d->Format), unsigned(d->ViewDimension),
                          d->Shader4ComponentMapping);
        events.push_back(Format("srv %s %s at=%#zx", Name(r), view.c_str(), at.ptr));
    }
    void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource *r, ID3D12Resource *counter,
                                                     const D3D12_UNORDERED_ACCESS_VIEW_DESC *d,
                                                     D3D12_CPU_DESCRIPTOR_HANDLE at) override {
        events.push_back(d ? Format("uav %s counter=%s fmt=%u dim=%u mip=%u plane=%u at=%#zx",
                                    Name(r), Name(counter), unsigned(d->Format),
                                    unsigned(d->ViewDimension), d->Texture2D.MipSlice,
                                    d->Texture2D.PlaneSlice, at.ptr)
                           : Format("uav %s desc=null at=%#zx", Name(r), at.ptr));
    }
    void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource *r,
                                                  const D3D12_RENDER_TARGET_VIEW_DESC *d,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE at) override {
        events.push_back(d ? Format("rtv %s fmt=%u dim=%u at=%#zx", Name(r), unsigned(d->Format),
                                    unsigned(d->ViewDimension), at.ptr)
                           : Format("rtv %s desc=null at=%#zx", Name(r), at.ptr));
    }
    void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource *,
                                                  const D3D12_DEPTH_STENCIL_VIEW_DESC *,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE) override {
        Unexpected("CreateDepthStencilView");
    }
    void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC *,
                                         D3D12_CPU_DESCRIPTOR_HANDLE) override {
        Unexpected("CreateSampler");
    }
    void STDMETHODCALLTYPE CopyDescriptors(UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, const UINT *,
                                           UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, const UINT *,
                                           D3D12_DESCRIPTOR_HEAP_TYPE) override {
        Unexpected("CopyDescriptors");
    }
    void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT, D3D12_CPU_DESCRIPTOR_HANDLE,
                                                 D3D12_CPU_DESCRIPTOR_HANDLE,
                                                 D3D12_DESCRIPTOR_HEAP_TYPE) override {
        Unexpected("CopyDescriptorsSimple");
    }
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE
    GetResourceAllocationInfo(UINT, UINT n, const D3D12_RESOURCE_DESC *d) override {
        assert(n == 1);
        events.push_back(
            Format("allocation-info dim=%u fmt=%u", unsigned(d->Dimension), unsigned(d->Format)));
        return {d->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? buffer_size : texture_size,
                65536};
    }
    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(UINT,
                                                                    D3D12_HEAP_TYPE) override {
        Unexpected("GetCustomHeapProperties");
        return {};
    }
    HRESULT STDMETHODCALLTYPE CreateCommittedResource(const D3D12_HEAP_PROPERTIES *,
                                                      D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
                                                      D3D12_RESOURCE_STATES,
                                                      const D3D12_CLEAR_VALUE *, REFIID,
                                                      void **) override {
        Unexpected("CreateCommittedResource");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC *, REFIID, void **) override {
        Unexpected("CreateHeap");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap *, UINT64,
                                                   const D3D12_RESOURCE_DESC *,
                                                   D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *,
                                                   REFIID, void **) override {
        Unexpected("CreatePlacedResource");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC *,
                                                     D3D12_RESOURCE_STATES,
                                                     const D3D12_CLEAR_VALUE *, REFIID,
                                                     void **) override {
        Unexpected("CreateReservedResource");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateSharedHandle(ID3D12DeviceChild *, const SECURITY_ATTRIBUTES *,
                                                 DWORD, LPCWSTR, HANDLE *) override {
        Unexpected("CreateSharedHandle");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE, REFIID, void **) override {
        Unexpected("OpenSharedHandle");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(LPCWSTR, DWORD, HANDLE *) override {
        Unexpected("OpenSharedHandleByName");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE MakeResident(UINT, ID3D12Pageable *const *) override {
        Unexpected("MakeResident");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Evict(UINT, ID3D12Pageable *const *) override {
        Unexpected("Evict");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateFence(UINT64, D3D12_FENCE_FLAGS, REFIID, void **) override {
        Unexpected("CreateFence");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override {
        Unexpected("GetDeviceRemovedReason");
        return E_NOTIMPL;
    }
    void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC *, UINT, UINT, UINT64,
                                                 D3D12_PLACED_SUBRESOURCE_FOOTPRINT *, UINT *,
                                                 UINT64 *, UINT64 *) override {
        Unexpected("GetCopyableFootprints");
    }
    HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC *, REFIID,
                                              void **) override {
        Unexpected("CreateQueryHeap");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL) override {
        Unexpected("SetStablePowerState");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC *,
                                                     ID3D12RootSignature *, REFIID,
                                                     void **) override {
        Unexpected("CreateCommandSignature");
        return E_NOTIMPL;
    }
    void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource *, UINT *, D3D12_PACKED_MIP_INFO *,
                                             D3D12_TILE_SHAPE *, UINT *, UINT,
                                             D3D12_SUBRESOURCE_TILING *) override {
        Unexpected("GetResourceTiling");
    }
    // Budget::Reserve looks the adapter up by LUID; no adapter has this one.
    LUID STDMETHODCALLTYPE GetAdapterLuid() override {
        events.push_back("adapter-luid");
        return {0xffffffffu, 0x7fffffff};
    }
};

struct FakeList final : Child<ID3D12GraphicsCommandList> {
    explicit FakeList(const char *name = "list") : Child(name) {}
    void Unexpected(const char *what) {
        events.push_back(Format("unexpected list %s", what));
    }
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override {
        Unexpected("GetType");
        return D3D12_COMMAND_LIST_TYPE_DIRECT;
    }
    HRESULT STDMETHODCALLTYPE Close() override {
        Unexpected("Close");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator *, ID3D12PipelineState *) override {
        Unexpected("Reset");
        return E_NOTIMPL;
    }
    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState *) override {
        Unexpected("ClearState");
    }
    void STDMETHODCALLTYPE DrawInstanced(UINT, UINT, UINT, UINT) override {
        Unexpected("DrawInstanced");
    }
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT, UINT, UINT, INT, UINT) override {
        Unexpected("DrawIndexedInstanced");
    }
    void STDMETHODCALLTYPE Dispatch(UINT x, UINT y, UINT z) override {
        events.push_back(Format("dispatch %u %u %u", x, y, z));
    }
    void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource *dst, UINT64 dst_offset,
                                            ID3D12Resource *src, UINT64 src_offset,
                                            UINT64 bytes) override {
        events.push_back(Format("copy %s+%llu <- %s+%llu n=%llu", Name(dst),
                                (unsigned long long)dst_offset, Name(src),
                                (unsigned long long)src_offset, (unsigned long long)bytes));
    }
    void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *, UINT, UINT, UINT,
                                             const D3D12_TEXTURE_COPY_LOCATION *,
                                             const D3D12_BOX *) override {
        Unexpected("CopyTextureRegion");
    }
    void STDMETHODCALLTYPE CopyResource(ID3D12Resource *, ID3D12Resource *) override {
        Unexpected("CopyResource");
    }
    void STDMETHODCALLTYPE CopyTiles(ID3D12Resource *, const D3D12_TILED_RESOURCE_COORDINATE *,
                                     const D3D12_TILE_REGION_SIZE *, ID3D12Resource *, UINT64,
                                     D3D12_TILE_COPY_FLAGS) override {
        Unexpected("CopyTiles");
    }
    void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource *, UINT, ID3D12Resource *, UINT,
                                              DXGI_FORMAT) override {
        Unexpected("ResolveSubresource");
    }
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY) override {
        Unexpected("IASetPrimitiveTopology");
    }
    void STDMETHODCALLTYPE RSSetViewports(UINT, const D3D12_VIEWPORT *) override {
        Unexpected("RSSetViewports");
    }
    void STDMETHODCALLTYPE RSSetScissorRects(UINT, const D3D12_RECT *) override {
        Unexpected("RSSetScissorRects");
    }
    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT[4]) override {
        Unexpected("OMSetBlendFactor");
    }
    void STDMETHODCALLTYPE OMSetStencilRef(UINT) override {
        Unexpected("OMSetStencilRef");
    }
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState *p) override {
        events.push_back(Format("pipeline-state %s", Name(p)));
    }
    void STDMETHODCALLTYPE ResourceBarrier(UINT n, const D3D12_RESOURCE_BARRIER *b) override {
        for (UINT i = 0; i < n; ++i) {
            if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
                events.push_back(Format(
                    "transition %s %#x->%#x sub=%#x flags=%u", Name(b[i].Transition.pResource),
                    unsigned(b[i].Transition.StateBefore), unsigned(b[i].Transition.StateAfter),
                    b[i].Transition.Subresource, unsigned(b[i].Flags)));
            else if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_UAV)
                events.push_back(Format("uav-barrier %s flags=%u", Name(b[i].UAV.pResource),
                                        unsigned(b[i].Flags)));
            else
                Unexpected("aliasing barrier");
        }
    }
    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList *) override {
        Unexpected("ExecuteBundle");
    }
    void STDMETHODCALLTYPE SetDescriptorHeaps(UINT n, ID3D12DescriptorHeap *const *h) override {
        std::string list = Format("descriptor-heaps n=%u", n);
        for (UINT i = 0; i < n; ++i)
            list += Format(" %s", Name(h[i]));
        events.push_back(list);
    }
    void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature *r) override {
        events.push_back(Format("compute-root %s", Name(r)));
    }
    void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature *) override {
        Unexpected("SetGraphicsRootSignature");
    }
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT i,
                                                         D3D12_GPU_DESCRIPTOR_HANDLE h) override {
        events.push_back(Format("root-table %u %#llx", i, (unsigned long long)h.ptr));
    }
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT,
                                                          D3D12_GPU_DESCRIPTOR_HANDLE) override {
        Unexpected("SetGraphicsRootDescriptorTable");
    }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT, UINT, UINT) override {
        Unexpected("SetComputeRoot32BitConstant");
    }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT, UINT, UINT) override {
        Unexpected("SetGraphicsRoot32BitConstant");
    }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT, UINT, const void *, UINT) override {
        Unexpected("SetComputeRoot32BitConstants");
    }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT, UINT, const void *, UINT) override {
        Unexpected("SetGraphicsRoot32BitConstants");
    }
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT i,
                                                            D3D12_GPU_VIRTUAL_ADDRESS a) override {
        events.push_back(Format("root-cbv %u %#llx", i, (unsigned long long)a));
    }
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT,
                                                             D3D12_GPU_VIRTUAL_ADDRESS) override {
        Unexpected("SetGraphicsRootConstantBufferView");
    }
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT,
                                                            D3D12_GPU_VIRTUAL_ADDRESS) override {
        Unexpected("SetComputeRootShaderResourceView");
    }
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT,
                                                             D3D12_GPU_VIRTUAL_ADDRESS) override {
        Unexpected("SetGraphicsRootShaderResourceView");
    }
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT i,
                                                             D3D12_GPU_VIRTUAL_ADDRESS a) override {
        events.push_back(Format("root-uav %u %#llx", i, (unsigned long long)a));
    }
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT,
                                                              D3D12_GPU_VIRTUAL_ADDRESS) override {
        Unexpected("SetGraphicsRootUnorderedAccessView");
    }
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *) override {
        Unexpected("IASetIndexBuffer");
    }
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT, UINT,
                                              const D3D12_VERTEX_BUFFER_VIEW *) override {
        Unexpected("IASetVertexBuffers");
    }
    void STDMETHODCALLTYPE SOSetTargets(UINT, UINT,
                                        const D3D12_STREAM_OUTPUT_BUFFER_VIEW *) override {
        Unexpected("SOSetTargets");
    }
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT, const D3D12_CPU_DESCRIPTOR_HANDLE *, BOOL,
                                              const D3D12_CPU_DESCRIPTOR_HANDLE *) override {
        Unexpected("OMSetRenderTargets");
    }
    void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS,
                                                 FLOAT, UINT8, UINT, const D3D12_RECT *) override {
        Unexpected("ClearDepthStencilView");
    }
    void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE view,
                                                 const FLOAT rgba[4], UINT n,
                                                 const D3D12_RECT *rects) override {
        events.push_back(Format("clear-rtv %#zx rgba=%g,%g,%g,%g rects=%u %s", view.ptr,
                                double(rgba[0]), double(rgba[1]), double(rgba[2]), double(rgba[3]),
                                n, rects ? "list" : "null"));
    }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE,
                                                        D3D12_CPU_DESCRIPTOR_HANDLE,
                                                        ID3D12Resource *, const UINT[4], UINT,
                                                        const D3D12_RECT *) override {
        Unexpected("ClearUnorderedAccessViewUint");
    }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE,
                                                         D3D12_CPU_DESCRIPTOR_HANDLE,
                                                         ID3D12Resource *, const FLOAT[4], UINT,
                                                         const D3D12_RECT *) override {
        Unexpected("ClearUnorderedAccessViewFloat");
    }
    void STDMETHODCALLTYPE DiscardResource(ID3D12Resource *,
                                           const D3D12_DISCARD_REGION *) override {
        Unexpected("DiscardResource");
    }
    void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap *, D3D12_QUERY_TYPE, UINT) override {
        Unexpected("BeginQuery");
    }
    void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap *, D3D12_QUERY_TYPE, UINT) override {
        Unexpected("EndQuery");
    }
    void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap *, D3D12_QUERY_TYPE, UINT, UINT,
                                            ID3D12Resource *, UINT64) override {
        Unexpected("ResolveQueryData");
    }
    void STDMETHODCALLTYPE SetPredication(ID3D12Resource *, UINT64, D3D12_PREDICATION_OP) override {
        Unexpected("SetPredication");
    }
    void STDMETHODCALLTYPE SetMarker(UINT, const void *, UINT) override {
        Unexpected("SetMarker");
    }
    void STDMETHODCALLTYPE BeginEvent(UINT, const void *, UINT) override {
        Unexpected("BeginEvent");
    }
    void STDMETHODCALLTYPE EndEvent() override {
        Unexpected("EndEvent");
    }
    void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature *, UINT, ID3D12Resource *, UINT64,
                                           ID3D12Resource *, UINT64) override {
        Unexpected("ExecuteIndirect");
    }
};

// Stand-in for rrlive::Pipelines: its Root/Pipeline helpers, logged.
struct FakePipelines {
    ComPtr<ID3D12Device> device;
    std::deque<FakeRoot> roots;
    std::deque<FakePso> pipelines;
    struct Parameter {
        D3D12_ROOT_PARAMETER_TYPE type;
        D3D12_SHADER_VISIBILITY visibility;
        UINT reg, space;
        std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
    };
    std::vector<Parameter> parameters;
    D3D12_ROOT_SIGNATURE_FLAGS flags{};
    UINT samplers = ~0u;
    const void *code{};
    size_t code_size{};
    ID3D12RootSignature *pipeline_root{};
    bool fail_pipeline{};
    explicit FakePipelines(ID3D12Device *d) : device(d) {}
    void Root(const D3D12_ROOT_SIGNATURE_DESC &rd, ComPtr<ID3D12RootSignature> &r) {
        events.push_back("root-signature");
        parameters.clear();
        for (UINT i = 0; i < rd.NumParameters; ++i) {
            const auto &p = rd.pParameters[i];
            Parameter q{p.ParameterType, p.ShaderVisibility, 0, 0, {}};
            if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
                q.ranges.assign(p.DescriptorTable.pDescriptorRanges,
                                p.DescriptorTable.pDescriptorRanges +
                                    p.DescriptorTable.NumDescriptorRanges);
            else {
                q.reg = p.Descriptor.ShaderRegister;
                q.space = p.Descriptor.RegisterSpace;
            }
            parameters.push_back(q);
        }
        flags = rd.Flags;
        samplers = rd.NumStaticSamplers;
        r = &roots.emplace_back("blend-root");
    }
    void Pipeline(ID3D12RootSignature *r, const void *c, size_t n, ComPtr<ID3D12PipelineState> &p) {
        events.push_back("compute-pipeline");
        if (fail_pipeline)
            throw std::runtime_error("live pipeline creation failed");
        pipeline_root = r;
        code = c;
        code_size = n;
        p = &pipelines.emplace_back("blend-pso");
    }
};

// Stand-in for the GpuFrame owner: its budget-checked Allocate, logged.
struct FakeBudget {
    UINT64 limit = rrlive::Budget::Minimum, bytes{};
    bool grant = true;            // whether DXGI would have room for the growth
    std::vector<UINT64> reserves; // every Reserve request
    bool Reserve(ID3D12Device *device, UINT64 required) {
        assert(device);
        events.push_back(Format("reserve %llu", (unsigned long long)required));
        reserves.push_back(required);
        if (required <= limit)
            return true;
        if (!grant)
            return false;
        limit = required;
        return true;
    }
};
struct FakeOwner {
    struct Call {
        D3D12_RESOURCE_DESC desc;
        D3D12_HEAP_TYPE type;
        bool has_clear;
        D3D12_CLEAR_VALUE clear;
    };
    UINT64 bytes{};
    unsigned defer_at = ~0u, fail_at = ~0u, attempts = 0;
    std::vector<Call> calls;
    std::deque<FakeResource> resources;
    static constexpr UINT64 TargetBytes = 8ull << 20, BufferBytes = 64ull << 10;
    FakeResource *Find(const char *name) {
        for (auto &r : resources)
            if (Name(&r) == std::string(name))
                return &r;
        return nullptr;
    }
    template <class P, class B>
    ComPtr<ID3D12Resource> Allocate(P &, B &budget, D3D12_RESOURCE_DESC d,
                                    D3D12_HEAP_TYPE type = D3D12_HEAP_TYPE_DEFAULT,
                                    const D3D12_CLEAR_VALUE *clear = nullptr) {
        calls.push_back({d, type, clear != nullptr, clear ? *clear : D3D12_CLEAR_VALUE{}});
        const auto attempt = attempts++;
        if (attempt == defer_at)
            throw rrprivate::AllocationDeferred();
        if (attempt == fail_at)
            throw std::runtime_error("live resource allocation failed");
        const bool texture = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        const char *name = texture                            ? "coverage"
                           : type == D3D12_HEAP_TYPE_UPLOAD   ? "control"
                           : type == D3D12_HEAP_TYPE_READBACK ? "readback"
                                                              : "stats";
        const D3D12_GPU_VIRTUAL_ADDRESS address = texture                            ? 0x1000000
                                                  : type == D3D12_HEAP_TYPE_UPLOAD   ? 0x2000000
                                                  : type == D3D12_HEAP_TYPE_READBACK ? 0x4000000
                                                                                     : 0x3000000;
        auto &r = resources.emplace_back(name, d, address);
        memset(r.memory, 0xcd, sizeof(r.memory));
        const auto size = texture ? TargetBytes : BufferBytes;
        bytes += size;
        budget.bytes += size;
        return &r;
    }
};

static D3D12_RESOURCE_DESC Scene(UINT64 w = 960, UINT h = 540, UINT quality = 0) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    d.SampleDesc = {4, quality};
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return d;
}
static constexpr UINT64 TotalBytes = FakeOwner::TargetBytes + 3 * FakeOwner::BufferBytes;
static bool Throws(auto &&f) {
    try {
        f();
    } catch (const rrprivate::AllocationDeferred &) {
        return false;
    } catch (const std::runtime_error &) {
        return true;
    }
    return false;
}
static bool SameDesc(const D3D12_RESOURCE_DESC &a, const D3D12_RESOURCE_DESC &b) {
    return !memcmp(&a, &b, sizeof(a));
}
// The size queries Prepare makes for the budgeted resources from `from` on
// (0 target, 1 control, 2 stats, 3 readback) before it allocates any of them.
static std::vector<std::string> SizeQueries(unsigned from = 0) {
    std::vector<std::string> v;
    for (unsigned i = from; i < 4; ++i)
        v.push_back(i ? Format("allocation-info dim=%u fmt=%u", D3D12_RESOURCE_DIMENSION_BUFFER,
                               DXGI_FORMAT_UNKNOWN)
                      : Format("allocation-info dim=%u fmt=%u", D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                               DXGI_FORMAT_R10G10B10A2_UNORM));
    return v;
}
static std::vector<std::string> Join(std::vector<std::string> a,
                                     const std::vector<std::string> &b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// The guide set of a GpuFrame: normal/roughness, diffuse, specular, status.
struct GuideSet {
    std::deque<FakeResource> fakes;
    std::array<ComPtr<ID3D12Resource>, 4> resources;
    explicit GuideSet(UINT w = 1920, UINT h = 1080) {
        const char *labels[] = {"normal", "diffuse", "specular", "status"};
        for (unsigned i = 0; i < 4; ++i) {
            auto &r = fakes.emplace_back(
                labels[i],
                rrlive::TextureDesc(
                    w, h, i == 3 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16G16B16A16_FLOAT));
            resources[i] = &r;
        }
    }
    // An SSS guide as rrsss::Gpu::GuideDesc makes it: R16_FLOAT, render size, UAV.
    FakeResource &Sss(DXGI_FORMAT format = DXGI_FORMAT_R16_FLOAT) {
        const auto d = resources[0]->GetDesc();
        return fakes.emplace_back("sss", rrlive::TextureDesc(UINT(d.Width), d.Height, format));
    }
};

// A prepared slot: device, pipeline helpers, owner and Gpu.
struct Slot {
    FakeDevice device;
    FakePipelines pipelines{&device};
    FakeOwner owner;
    FakeBudget budget;
    Gpu gpu;
    Slot() {
        assert(gpu.Prepare(owner, pipelines, budget, Scene()));
    }
    FakeResource &Get(const char *name) {
        auto *r = owner.Find(name);
        assert(r);
        return *r;
    }
    FakeHeap &Heap(bool rtv) {
        for (auto &h : device.heaps)
            if ((h.desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV) == rtv)
                return h;
        assert(!"heap");
        return device.heaps.front();
    }
};

static void TestTicket() {
    Ticket t;
    assert(!t.Live() && !t.capture && !t.live && !t.submitted && !t.cleared);
    assert(!t.captures_oit && !t.captures_refr && !t.captures_refr_alpha);
    FakeList list("capture-list");
    t.capture = &list;
    assert(list.refs == 2);
    t.live = t.submitted = t.cleared = true;
    t.captures_oit = 2;
    t.captures_refr = 3;
    t.captures_refr_alpha = 1;
    assert(t.Live());
    t.live = false;
    assert(!t.Live()); // only the capture recording decides liveness
    t.live = true;
    t.Clear();
    assert(!t.Live() && !t.capture && !t.live && !t.submitted && !t.cleared);
    assert(!t.captures_oit && !t.captures_refr && !t.captures_refr_alpha);
    assert(list.refs == 1); // the list reference is dropped
}

static void TestPipelineTyped() {
    enum Case { NoTypedFormats, NoLoad, NoStore, OptionsFailed, FormatFailed };
    for (auto c : {NoTypedFormats, NoLoad, NoStore, OptionsFailed, FormatFailed}) {
        FakeDevice device;
        if (c == NoTypedFormats)
            device.typed = FALSE;
        if (c == NoLoad)
            device.support2 = D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
        if (c == NoStore)
            device.support2 = D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD;
        if (c == OptionsFailed)
            device.options_result = E_FAIL;
        if (c == FormatFailed)
            device.format_result = E_FAIL;
        FakePipelines p(&device);
        Pipeline pipeline;
        events.clear();
        assert(!pipeline.Prepare(p));
        assert(pipeline.unavailable == Reason::Typed && !pipeline.Ready());
        assert(!pipeline.root && !pipeline.state);
        // The PSO is never created: its DXIL requires typed UAV loads.
        for (const auto &e : events)
            assert(e.rfind("check-", 0) == 0);
        // Decided once per device: no further queries.
        events.clear();
        assert(!pipeline.Prepare(p) && events.empty());
        assert(pipeline.unavailable == Reason::Typed);
    }
}

static void TestPipelineBuild() {
    FakeDevice device;
    FakePipelines p(&device);
    Pipeline pipeline;
    assert(!pipeline.Ready() && pipeline.unavailable == Reason::None);
    events.clear();
    assert(pipeline.Prepare(p) && pipeline.Ready() && pipeline.unavailable == Reason::None);
    // Typed UAV load support of the guide format is proven before the PSO exists.
    const std::vector<std::string> order = {
        "check-options", Format("check-format %u", DXGI_FORMAT_R16G16B16A16_FLOAT),
        "root-signature", "compute-pipeline"};
    assert(events == order);
    // [0] root CBV b0 (control), [1] table t0-t1, u0-u2, then u4 (SSS guide), [2] root UAV u3
    // (stats).
    assert(p.parameters.size() == 3 && p.flags == D3D12_ROOT_SIGNATURE_FLAG_NONE && !p.samplers);
    for (const auto &q : p.parameters)
        assert(q.visibility == D3D12_SHADER_VISIBILITY_ALL);
    assert(p.parameters[0].type == D3D12_ROOT_PARAMETER_TYPE_CBV && p.parameters[0].reg == 0 &&
           p.parameters[0].space == 0);
    const auto &table = p.parameters[1];
    assert(table.type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE && table.ranges.size() == 3);
    assert(table.ranges[0].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV &&
           table.ranges[0].NumDescriptors == 2 && table.ranges[0].BaseShaderRegister == 0 &&
           table.ranges[0].RegisterSpace == 0 &&
           table.ranges[0].OffsetInDescriptorsFromTableStart == 0);
    assert(table.ranges[1].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV &&
           table.ranges[1].NumDescriptors == 3 && table.ranges[1].BaseShaderRegister == 0 &&
           table.ranges[1].RegisterSpace == 0 &&
           table.ranges[1].OffsetInDescriptorsFromTableStart == 2);
    assert(table.ranges[2].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV &&
           table.ranges[2].NumDescriptors == 1 && table.ranges[2].BaseShaderRegister == 4 &&
           table.ranges[2].RegisterSpace == 0 &&
           table.ranges[2].OffsetInDescriptorsFromTableStart == 5);
    assert(p.parameters[2].type == D3D12_ROOT_PARAMETER_TYPE_UAV && p.parameters[2].reg == 3 &&
           p.parameters[2].space == 0);
    assert(p.code == RRTransparencyBlend && p.code_size == sizeof(RRTransparencyBlend));
    assert(p.pipeline_root == pipeline.root.Get());
    // Built once.
    events.clear();
    assert(pipeline.Prepare(p) && events.empty());
    // Another device never reuses it.
    FakeDevice other("other");
    FakePipelines q(&other);
    events.clear();
    assert(Throws([&] { pipeline.Prepare(q); }) && events.empty());

    // A failed PSO creation propagates; the retry keeps the root signature.
    FakeDevice again;
    FakePipelines r(&again);
    r.fail_pipeline = true;
    Pipeline retried;
    assert(Throws([&] { retried.Prepare(r); }));
    assert(!retried.Ready() && retried.root && retried.unavailable == Reason::None);
    r.fail_pipeline = false;
    events.clear();
    assert(retried.Prepare(r) && retried.Ready());
    assert(events == std::vector<std::string>{"compute-pipeline"});
}

// The real rrlive::Pipelines helpers serialize the root signature the D3D12 way.
static void TestPipelineLive() {
    FakeDevice device;
    rrlive::Pipelines live;
    live.device = &device;
    Pipeline pipeline;
    events.clear();
    assert(pipeline.Prepare(live) && pipeline.Ready());
    const std::vector<std::string> order = {
        "check-options", Format("check-format %u", DXGI_FORMAT_R16G16B16A16_FLOAT),
        "create-root-signature mask=0",
        Format("create-compute-pipeline root=blend-root cs=%zu", sizeof(RRTransparencyBlend))};
    assert(events == order);
    assert(device.cs == RRTransparencyBlend && device.cs_size == sizeof(RRTransparencyBlend));
    ComPtr<ID3D12RootSignatureDeserializer> reader;
    assert(SUCCEEDED(D3D12CreateRootSignatureDeserializer(
        device.root_blob.data(), device.root_blob.size(), __uuidof(ID3D12RootSignatureDeserializer),
        reinterpret_cast<void **>(reader.GetAddressOf()))));
    const auto *d = reader->GetRootSignatureDesc();
    assert(d->NumParameters == 3 && !d->NumStaticSamplers);
    assert(d->pParameters[0].ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV &&
           d->pParameters[0].Descriptor.ShaderRegister == 0);
    const auto &t = d->pParameters[1].DescriptorTable;
    assert(d->pParameters[1].ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE &&
           t.NumDescriptorRanges == 3);
    assert(t.pDescriptorRanges[0].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV &&
           t.pDescriptorRanges[0].NumDescriptors == 2 &&
           t.pDescriptorRanges[0].OffsetInDescriptorsFromTableStart == 0);
    assert(t.pDescriptorRanges[1].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV &&
           t.pDescriptorRanges[1].NumDescriptors == 3 &&
           t.pDescriptorRanges[1].BaseShaderRegister == 0 &&
           t.pDescriptorRanges[1].OffsetInDescriptorsFromTableStart == 2);
    assert(t.pDescriptorRanges[2].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV &&
           t.pDescriptorRanges[2].NumDescriptors == 1 &&
           t.pDescriptorRanges[2].BaseShaderRegister == 4 &&
           t.pDescriptorRanges[2].OffsetInDescriptorsFromTableStart == 5);
    assert(d->pParameters[2].ParameterType == D3D12_ROOT_PARAMETER_TYPE_UAV &&
           d->pParameters[2].Descriptor.ShaderRegister == 3);
    pipeline = {};
    live = {};
}

static void TestShapeAndMatches() {
    const auto scene = Scene();
    assert(Gpu::Shape(scene) && Gpu::Shape(Scene(960, 540, D3D12_STANDARD_MULTISAMPLE_PATTERN)));
    assert(Gpu::Shape(Scene(16384, 16384)));
    for (auto bad :
         {Scene(0, 540), Scene(960, 0), Scene(16385, 540), Scene(960, 16385), Scene(960, 540, 1)}) {
        assert(!Gpu::Shape(bad));
        assert(!Gpu{}.Matches(bad));
    }
    for (UINT count : {1u, 2u, 8u}) {
        auto bad = scene;
        bad.SampleDesc.Count = count;
        assert(!Gpu::Shape(bad));
    }
    for (auto dimension : {D3D12_RESOURCE_DIMENSION_BUFFER, D3D12_RESOURCE_DIMENSION_TEXTURE1D,
                           D3D12_RESOURCE_DIMENSION_TEXTURE3D}) {
        auto bad = scene;
        bad.Dimension = dimension;
        assert(!Gpu::Shape(bad));
    }
    // Before allocation any shape matches; afterwards only the allocated one.
    assert(Gpu{}.Matches(scene) && Gpu{}.Matches(Scene(1280, 720)));
    Slot s;
    assert(s.gpu.Matches(scene));
    assert(SameDesc(s.gpu.desc, CoverageDesc(scene)));
    auto other = scene;
    other.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // the scene's own format is irrelevant
    other.Flags = D3D12_RESOURCE_FLAG_NONE;
    assert(s.gpu.Matches(other));
    assert(!s.gpu.Matches(Scene(961, 540)) && !s.gpu.Matches(Scene(960, 541)));
    assert(!s.gpu.Matches(Scene(960, 540, D3D12_STANDARD_MULTISAMPLE_PATTERN)));
    assert(!s.gpu.Matches(Scene(0, 540)));
}

static void TestRequiredBytes() {
    const auto scene = Scene(1280, 720, D3D12_STANDARD_MULTISAMPLE_PATTERN);
    std::vector<D3D12_RESOURCE_DESC> seen;
    auto query = [&](const D3D12_RESOURCE_DESC &d) {
        seen.push_back(d);
        return d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? UINT64(65536) : UINT64(7 << 20);
    };
    assert(Gpu::RequiredBytes(scene, query) == (7u << 20) + 3 * 65536);
    // Target, control, stats, readback; the descriptor heaps are not counted.
    assert(seen.size() == 4 && SameDesc(seen[0], CoverageDesc(scene)));
    unsigned plain = 0, uav = 0;
    for (size_t i = 1; i < 4; ++i) {
        const auto &d = seen[i];
        assert(d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && d.Width == 256 && d.Height == 1 &&
               d.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR && d.SampleDesc.Count == 1);
        plain += d.Flags == D3D12_RESOURCE_FLAG_NONE;
        uav += d.Flags == D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    assert(plain == 2 && uav == 1);
    // Unknown, failed and overflowing sizes are refused.
    assert(!Gpu::RequiredBytes(scene, [](const D3D12_RESOURCE_DESC &) { return UINT64(0); }));
    assert(!Gpu::RequiredBytes(scene, [](const D3D12_RESOURCE_DESC &) { return UINT64_MAX; }));
    assert(!Gpu::RequiredBytes(scene, [](const D3D12_RESOURCE_DESC &) { return UINT64(1) << 62; }));
    seen.clear();
    assert(!Gpu::RequiredBytes(Scene(960, 540, 1), query) && seen.empty());
    // Device form: GetResourceAllocationInfo of the same four descriptions.
    FakeDevice device;
    events.clear();
    assert(Gpu::RequiredBytes(&device, scene) == device.texture_size + 3 * device.buffer_size);
    assert(events.size() == 4 &&
           events[0] == Format("allocation-info dim=%u fmt=%u", D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                               DXGI_FORMAT_R10G10B10A2_UNORM));
    assert(!Gpu::RequiredBytes(nullptr, scene));
    // One source of truth: on a fresh Gpu it is what Prepare sizes (Missing).
    assert(Gpu::RequiredBytes(&device, scene) == Gpu{}.Missing(&device, scene));
    device.texture_size = 3 << 20;
    assert(Gpu::RequiredBytes(&device, scene) == Gpu{}.Missing(&device, scene) &&
           Gpu{}.Missing(&device, scene) == (3u << 20) + 3 * device.buffer_size);
}

static void TestPrepare() {
    FakeDevice device;
    FakePipelines p(&device);
    FakeOwner owner;
    FakeBudget budget;
    Gpu gpu;
    assert(!gpu.Ready() && !gpu.ControlAddress());
    const auto scene = Scene();
    events.clear();
    assert(gpu.Prepare(owner, p, budget, scene) && gpu.Ready());
    // Every resource through the owner's budget-checked Allocate, COMMON at rest there.
    assert(owner.calls.size() == 4);
    const auto &target = owner.calls[0];
    assert(SameDesc(target.desc, CoverageDesc(scene)) && target.type == D3D12_HEAP_TYPE_DEFAULT);
    assert(target.has_clear && target.clear.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
    for (float c : target.clear.Color)
        assert(c == 1.f);
    const auto &control = owner.calls[1];
    assert(control.type == D3D12_HEAP_TYPE_UPLOAD && !control.has_clear &&
           SameDesc(control.desc, rrprivate::Buffer(256)));
    const auto &stats = owner.calls[2];
    auto stats_desc = rrprivate::Buffer(256);
    stats_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    assert(stats.type == D3D12_HEAP_TYPE_DEFAULT && !stats.has_clear &&
           SameDesc(stats.desc, stats_desc));
    const auto &readback = owner.calls[3];
    assert(readback.type == D3D12_HEAP_TYPE_READBACK && !readback.has_clear &&
           SameDesc(readback.desc, rrprivate::Buffer(256)));
    assert(gpu.target.Get() == owner.Find("coverage") &&
           gpu.control.Get() == owner.Find("control"));
    assert(gpu.stats.Get() == owner.Find("stats") && gpu.readback.Get() == owner.Find("readback"));
    // Own bytes for reporting; the owner and its budget carry the same allocation.
    assert(gpu.bytes == TotalBytes && owner.bytes == TotalBytes && budget.bytes == TotalBytes);
    // The four sizes are queried first; the ledger had room, so nothing was reserved.
    assert(budget.reserves.empty() && budget.limit == rrlive::Budget::Minimum);
    const std::vector<std::string> order = Join(
        SizeQueries(),
        {Format("create-heap type=%u n=1 flags=0 mask=0", D3D12_DESCRIPTOR_HEAP_TYPE_RTV),
         Format("create-heap type=%u n=6 flags=%u mask=0", D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE),
         "map control sub=0 read=0-0", "map readback sub=0 read=all",
         Format("rtv coverage fmt=%u dim=%u at=0x10000", DXGI_FORMAT_R10G10B10A2_UNORM,
                D3D12_RTV_DIMENSION_TEXTURE2DMS),
         Format("srv coverage fmt=%u dim=%u map=%#x at=0x20000", DXGI_FORMAT_R10G10B10A2_UNORM,
                D3D12_SRV_DIMENSION_TEXTURE2DMS, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING)});
    assert(events == order);
    // The control buffer starts zeroed (its zero region clears the stats).
    for (auto b : owner.Find("control")->memory)
        assert(b == 0);
    assert(gpu.ControlAddress() == 0x2000000);
    assert(SameDesc(gpu.desc, CoverageDesc(scene)));
    // Ready: nothing more is created.
    events.clear();
    owner.calls.clear();
    assert(gpu.Prepare(owner, p, budget, scene) && events.empty() && owner.calls.empty());
    // A different extent or sample layout is never prepared over the retained target.
    assert(Throws([&] { gpu.Prepare(owner, p, budget, Scene(961, 540)); }));
    assert(Throws([&] { gpu.Prepare(owner, p, budget, Scene(960, 540, 0xffffffff)); }));
    assert(owner.calls.empty() && events.empty() && gpu.Ready());
    Gpu fresh;
    assert(Throws([&] { fresh.Prepare(owner, p, budget, Scene(960, 540, 3)); }));
    assert(owner.calls.empty() && !fresh.Ready());
}

static void TestPrepareResumes() {
    // Deferred at each allocation: false, partial resources and bytes kept, and the
    // retry allocates only what is missing.
    for (unsigned at = 0; at < 4; ++at) {
        FakeDevice device;
        FakePipelines p(&device);
        FakeOwner owner;
        FakeBudget budget;
        owner.defer_at = at;
        Gpu gpu;
        events.clear();
        assert(!gpu.Prepare(owner, p, budget, Scene()) && !gpu.Ready());
        assert(owner.calls.size() == at + 1 && events == SizeQueries());
        const UINT64 kept = at ? FakeOwner::TargetBytes + (at - 1) * FakeOwner::BufferBytes : 0;
        assert(gpu.bytes == kept && owner.bytes == kept && budget.bytes == kept);
        assert(bool(gpu.target) == (at > 0) && bool(gpu.control) == (at > 1) &&
               bool(gpu.stats) == (at > 2) && !gpu.readback);
        assert(!gpu.ControlAddress() || at > 1);
        owner.calls.clear();
        events.clear();
        assert(gpu.Prepare(owner, p, budget, Scene()) && gpu.Ready());
        assert(owner.calls.size() == 4 - at);
        // Only the still-missing resources are sized again.
        const auto sized = SizeQueries(at);
        assert(events.size() > sized.size() &&
               std::equal(sized.begin(), sized.end(), events.begin()));
        assert(owner.calls[0].type == (at == 0   ? D3D12_HEAP_TYPE_DEFAULT
                                       : at == 1 ? D3D12_HEAP_TYPE_UPLOAD
                                       : at == 2 ? D3D12_HEAP_TYPE_DEFAULT
                                                 : D3D12_HEAP_TYPE_READBACK));
        assert(owner.resources.size() == 4);
        assert(gpu.bytes == TotalBytes && owner.bytes == TotalBytes && budget.bytes == TotalBytes);
    }
    // A descriptor heap out of memory also defers; the retry creates only that heap.
    for (unsigned at = 0; at < 2; ++at) {
        FakeDevice device;
        device.heap_fail_at = at;
        device.heap_result = E_OUTOFMEMORY;
        FakePipelines p(&device);
        FakeOwner owner;
        FakeBudget budget;
        Gpu gpu;
        events.clear();
        assert(!gpu.Prepare(owner, p, budget, Scene()) && !gpu.Ready());
        assert(owner.calls.size() == 4 && gpu.bytes == TotalBytes);
        assert(events.size() == 4 + at + 1);
        events.clear();
        assert(gpu.Prepare(owner, p, budget, Scene()) && gpu.Ready());
        assert(owner.calls.size() == 4 && device.heaps.size() == 2);
        assert(events[0] == (at ? Format("create-heap type=%u n=6 flags=%u mask=0",
                                         D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
                                : Format("create-heap type=%u n=1 flags=0 mask=0",
                                         D3D12_DESCRIPTOR_HEAP_TYPE_RTV)));
    }
    // Any other failure propagates, and the bytes already allocated stay counted.
    {
        FakeDevice device;
        FakePipelines p(&device);
        FakeOwner owner;
        FakeBudget budget;
        owner.fail_at = 1;
        Gpu gpu;
        assert(Throws([&] { gpu.Prepare(owner, p, budget, Scene()); }));
        assert(!gpu.Ready() && gpu.bytes == FakeOwner::TargetBytes &&
               owner.bytes == FakeOwner::TargetBytes);
        FakeDevice heapless;
        heapless.heap_fail_at = 0;
        heapless.heap_result = E_INVALIDARG;
        FakePipelines q(&heapless);
        FakeOwner other;
        Gpu failed;
        assert(Throws([&] { failed.Prepare(other, q, budget, Scene()); }) && !failed.Ready());
    }
}

// The live owner: GpuFrame::Allocate defers when the budget cannot admit the
// allocation, before any resource is created.
static void TestPrepareLiveFrame() {
    FakeDevice device;
    rrlive::Pipelines pipelines;
    pipelines.device = &device;
    rrlive::Budget budget;
    rrlive::GpuFrame frame;
    events.clear();
    assert(!frame.coverage.Prepare(frame, pipelines, budget, Scene()));
    assert(!frame.coverage.Ready() && !frame.coverage.target && !frame.coverage.bytes);
    assert(!frame.bytes && !budget.bytes && budget.limit == rrlive::Budget::Minimum);
    // The ledger had room (no Reserve); GpuFrame::Allocate sized the target again.
    const auto order = Join(
        SizeQueries(), {Format("allocation-info dim=%u fmt=%u", D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                               DXGI_FORMAT_R10G10B10A2_UNORM),
                        "check-architecture"});
    assert(events == order);
    pipelines = {};
}

// The live budget at its ceiling: Budget::Reserve sets limit to exactly what
// the pool needs above the minimum, and coverage is not in that projection.
// Prepare asks the budget to grow by what coverage needs; when DXGI has no
// room (here: no adapter matches), it defers without allocating, instead of
// GpuFrame::Allocate's hard "reserve exceeded" failure.
static void TestPrepareFullLedger() {
    for (const UINT64 headroom : {UINT64(0), UINT64(4096)}) {
        FakeDevice device;
        rrlive::Pipelines pipelines;
        pipelines.device = &device;
        rrlive::Budget budget;
        budget.limit = 2ull << 30;
        budget.bytes = budget.limit - headroom;
        rrlive::GpuFrame frame;
        events.clear();
        bool prepared = true;
        std::string error;
        try {
            prepared = frame.coverage.Prepare(frame, pipelines, budget, Scene());
        } catch (const std::exception &e) {
            error = e.what();
        }
        if (!error.empty())
            printf("full ledger (headroom %llu): Prepare threw \"%s\"\n",
                   (unsigned long long)headroom, error.c_str());
        assert(error.empty() && !prepared);
        // Nothing allocated or counted, and the ceiling did not move.
        assert(!frame.coverage.Ready() && !frame.coverage.target && !frame.coverage.control &&
               !frame.coverage.stats && !frame.coverage.readback && !frame.coverage.bytes);
        assert(!frame.bytes && budget.bytes == budget.limit - headroom &&
               budget.limit == 2ull << 30);
        // Only the four size queries, then Reserve's adapter lookup (when DXGI loads).
        const auto sized = SizeQueries();
        assert(events.size() >= sized.size() &&
               std::equal(sized.begin(), sized.end(), events.begin()));
        for (size_t i = sized.size(); i < events.size(); ++i)
            assert(events[i] == "adapter-luid");
        pipelines = {};
    }
}

// The ledger rule with a recording budget: Reserve is asked for exactly the
// ledger plus the still-missing resources, before anything is allocated.
static void TestPrepareReserve() {
    constexpr UINT64 full = 3ull << 30;
    // Refused: nothing allocated, nothing counted; the retry with room succeeds.
    {
        FakeDevice device;
        FakePipelines p(&device);
        FakeOwner owner;
        FakeBudget budget;
        budget.limit = budget.bytes = full;
        budget.grant = false;
        Gpu gpu;
        events.clear();
        assert(!gpu.Prepare(owner, p, budget, Scene()) && !gpu.Ready());
        assert(owner.calls.empty() && !gpu.bytes && budget.bytes == full && budget.limit == full);
        assert(events == Join(SizeQueries(), {Format("reserve %llu", full + TotalBytes)}));
        budget.grant = true;
        assert(gpu.Prepare(owner, p, budget, Scene()) && gpu.Ready());
        assert(budget.reserves.size() == 2 && budget.reserves[1] == full + TotalBytes);
        assert(budget.limit == full + TotalBytes && budget.bytes == full + TotalBytes);
        assert(owner.calls.size() == 4 && gpu.bytes == TotalBytes);
    }
    // Resumed: only the missing stats and readback are counted.
    {
        FakeDevice device;
        FakePipelines p(&device);
        FakeOwner owner;
        FakeBudget budget;
        owner.defer_at = 2;
        Gpu gpu;
        assert(!gpu.Prepare(owner, p, budget, Scene()) && budget.reserves.empty());
        const UINT64 kept = FakeOwner::TargetBytes + FakeOwner::BufferBytes;
        assert(budget.bytes == kept);
        budget.limit = budget.bytes + FakeOwner::BufferBytes; // room for one buffer only
        budget.grant = false;
        events.clear();
        owner.calls.clear();
        assert(!gpu.Prepare(owner, p, budget, Scene()) && owner.calls.empty());
        assert(events ==
               Join(SizeQueries(2), {Format("reserve %llu", kept + 2 * FakeOwner::BufferBytes)}));
        budget.grant = true;
        assert(gpu.Prepare(owner, p, budget, Scene()) && gpu.Ready() && owner.calls.size() == 2);
        assert(budget.limit == kept + 2 * FakeOwner::BufferBytes && gpu.bytes == TotalBytes);
    }
    // A ledger already above its ceiling also asks; one that fits exactly does not.
    {
        FakeDevice device;
        FakePipelines p(&device);
        FakeOwner owner;
        FakeBudget budget;
        budget.limit = full;
        budget.bytes = full + 1;
        budget.grant = false;
        Gpu gpu;
        assert(!gpu.Prepare(owner, p, budget, Scene()) && owner.calls.empty());
        assert(budget.reserves.size() == 1 && budget.reserves[0] == full + 1 + TotalBytes);
        budget.bytes = full - TotalBytes;
        budget.reserves.clear();
        assert(gpu.Prepare(owner, p, budget, Scene()) && budget.reserves.empty());
        assert(budget.bytes == full && owner.calls.size() == 4);
    }
    // Only descriptor heaps missing: nothing to budget, so no Reserve even above the ceiling.
    {
        FakeDevice device;
        device.heap_fail_at = 0;
        device.heap_result = E_OUTOFMEMORY;
        FakePipelines p(&device);
        FakeOwner owner;
        FakeBudget budget;
        Gpu gpu;
        assert(!gpu.Prepare(owner, p, budget, Scene()) && owner.calls.size() == 4);
        budget.limit = budget.bytes - 1;
        budget.grant = false;
        events.clear();
        assert(gpu.Prepare(owner, p, budget, Scene()) && gpu.Ready());
        assert(budget.reserves.empty() && owner.calls.size() == 4);
        assert(events[0] ==
               Format("create-heap type=%u n=1 flags=0 mask=0", D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
    }
    // Unknown, failed or overflowing sizes are hard failures, before any allocation.
    for (unsigned c = 0; c < 4; ++c) {
        FakeDevice device;
        if (c == 0)
            device.texture_size = 0;
        else if (c == 1)
            device.buffer_size = UINT64_MAX;
        else if (c == 2)
            device.texture_size = device.buffer_size = 1ull << 62;
        FakePipelines p(&device);
        FakeOwner owner;
        FakeBudget budget;
        if (c == 3)
            budget.limit = budget.bytes = UINT64_MAX - 1; // ledger + needed overflows
        Gpu gpu;
        assert(Throws([&] { gpu.Prepare(owner, p, budget, Scene()); }));
        assert(owner.calls.empty() && budget.reserves.empty() && !gpu.bytes && !gpu.Ready());
    }
    // A lone missing resource of unknown size, even on an empty ledger.
    {
        FakeDevice device;
        FakePipelines p(&device);
        FakeOwner owner;
        FakeBudget budget;
        owner.defer_at = 3;
        Gpu gpu;
        assert(!gpu.Prepare(owner, p, budget, Scene()) && gpu.stats && !gpu.readback);
        budget.bytes = 0;
        device.buffer_size = UINT64_MAX;
        owner.calls.clear();
        assert(Throws([&] { gpu.Prepare(owner, p, budget, Scene()); }));
        assert(owner.calls.empty() && budget.reserves.empty() && !gpu.readback);
    }
}

static void TestCapture() {
    Slot s;
    FakeList list;
    const auto rtv = s.Heap(true).cpu;
    events.clear();
    auto handle = s.gpu.BeginCapture(&list, true);
    assert(handle.ptr == rtv);
    std::vector<std::string> expected = {
        Transition("coverage", D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET),
        Format("clear-rtv %#zx rgba=1,1,1,1 rects=0 null", rtv)};
    assert(events == expected);
    events.clear();
    handle = s.gpu.BeginCapture(&list, false);
    assert(handle.ptr == rtv);
    expected.pop_back();
    assert(events == expected);
    events.clear();
    s.gpu.EndCapture(&list);
    expected = {
        Transition("coverage", D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON)};
    assert(events == expected);
    static_assert(noexcept(s.gpu.BeginCapture(&list, true)) && noexcept(s.gpu.EndCapture(&list)));
}

static void TestControl() {
    Slot s;
    auto &memory = s.Get("control").memory;
    auto c = Make(Tuning{}, 1919, 1081);
    c.admitted = 7;
    c.reserved[2] = 9;
    events.clear();
    s.gpu.WriteControl(c);
    // No SSS guide was described, so its flag is cleared; the rest is the caller's.
    assert(c.flags & SssFlag);
    c.flags &= ~SssFlag;
    assert(!memcmp(memory, &c, sizeof(c)));
    // Seal writes word 0 only.
    s.gpu.Seal(true);
    UINT word{};
    memcpy(&word, memory, 4);
    assert(word == 1 && !memcmp(memory + 4, reinterpret_cast<const unsigned char *>(&c) + 4, 252));
    s.gpu.Seal(false);
    memcpy(&word, memory, 4);
    assert(word == 0 && !memcmp(memory + 4, reinterpret_cast<const unsigned char *>(&c) + 4, 252));
    assert(events.empty());
    // Control word 0's GPU address, for passes that gate on the same admission.
    assert(s.gpu.ControlAddress() == s.Get("control").address);
    Gpu unprepared;
    unprepared.WriteControl(c);
    unprepared.Seal(true);
    assert(!unprepared.ControlAddress());
    static_assert(noexcept(s.gpu.WriteControl(c)) && noexcept(s.gpu.Seal(true)));
}

// Control::zero is the source Record clears the stats from: WriteControl keeps
// it zero whatever the caller passes, so the stats never accumulate.
static void TestControlZero() {
    Slot s;
    auto &memory = s.Get("control").memory;
    auto c = Make(Tuning{}, 1920, 1080);
    for (unsigned i = 0; i < 4; ++i)
        c.zero[i] = 0x11111111u * (i + 1);
    c.reserved[3] = 6;
    c.padding[0] = 5;
    s.gpu.WriteControl(c);
    Control mapped;
    memcpy(&mapped, memory, sizeof(mapped));
    for (auto z : mapped.zero)
        assert(z == 0);
    // Every other word is the caller's.
    auto expected = c;
    memset(expected.zero, 0, sizeof(expected.zero));
    expected.flags &= ~SssFlag;
    assert(!memcmp(&mapped, &expected, sizeof(mapped)));
}

// Heap [5]: the u4 view of the SSS guide, or a null view of the same shape.
static std::string SssView(const char *name, SIZE_T heap) {
    return Format("uav %s counter=null fmt=%u dim=%u mip=0 plane=0 at=%#zx", name,
                  DXGI_FORMAT_R16_FLOAT, D3D12_UAV_DIMENSION_TEXTURE2D, heap + 160);
}

static void TestDescriptors() {
    Slot s;
    GuideSet g;
    const auto heap = s.Heap(false).cpu;
    assert(!s.gpu.Described());
    events.clear();
    s.gpu.WriteDescriptors(s.pipelines, g.resources);
    assert(s.gpu.Described());
    const std::vector<std::string> expected = {
        Format("srv status fmt=%u dim=%u map=%#x most=0 mips=1 plane=0 clamp=0 at=%#zx",
               DXGI_FORMAT_R32_UINT, D3D12_SRV_DIMENSION_TEXTURE2D,
               D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, heap + 32),
        Format("uav normal counter=null fmt=%u dim=%u mip=0 plane=0 at=%#zx",
               DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_UAV_DIMENSION_TEXTURE2D, heap + 64),
        Format("uav diffuse counter=null fmt=%u dim=%u mip=0 plane=0 at=%#zx",
               DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_UAV_DIMENSION_TEXTURE2D, heap + 96),
        Format("uav specular counter=null fmt=%u dim=%u mip=0 plane=0 at=%#zx",
               DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_UAV_DIMENSION_TEXTURE2D, heap + 128),
        SssView("null", heap)};
    assert(events == expected && !s.gpu.faded);
    // Immutable until the slot retires: the same guides write only the SSS
    // view again, other guides are refused.
    events.clear();
    s.gpu.WriteDescriptors(s.pipelines, g.resources);
    assert(events == std::vector<std::string>{SssView("null", heap)});
    GuideSet other;
    events.clear();
    assert(Throws([&] { s.gpu.WriteDescriptors(s.pipelines, other.resources); }));
    assert(events.empty() && s.gpu.Described());

    // The SSS guide's view is rewritten on every call; WriteControl keeps
    // SssFlag only while it is a real view.
    auto &memory = s.Get("control").memory;
    auto flags = [&] {
        Control mapped;
        memcpy(&mapped, memory, sizeof(mapped));
        return mapped.flags;
    };
    auto &sss = g.Sss();
    events.clear();
    s.gpu.WriteDescriptors(s.pipelines, g.resources, &sss);
    assert(events == std::vector<std::string>{SssView("sss", heap)} && s.gpu.faded == &sss);
    s.gpu.WriteControl(Make(Tuning{}, 1920, 1080));
    assert(flags() & SssFlag);
    s.gpu.WriteDescriptors(s.pipelines, g.resources);
    s.gpu.WriteControl(Make(Tuning{}, 1920, 1080));
    assert(!(flags() & SssFlag) && !s.gpu.faded);
    // A guide of another format or size, or without UAV access, is not used.
    auto unusable = [&](FakeResource &bad) {
        events.clear();
        s.gpu.WriteDescriptors(s.pipelines, g.resources, &bad);
        s.gpu.WriteControl(Make(Tuning{}, 1920, 1080));
        return events == std::vector<std::string>{SssView("null", heap)} && !s.gpu.faded &&
               !(flags() & SssFlag);
    };
    assert(unusable(g.Sss(DXGI_FORMAT_R32_FLOAT)));
    auto &wide = g.Sss();
    wide.desc.Width = 1921;
    assert(unusable(wide));
    auto &tall = g.Sss();
    tall.desc.Height = 1081;
    assert(unusable(tall));
    auto &fixed = g.Sss();
    fixed.desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    assert(unusable(fixed));
    auto &sampled = g.Sss();
    sampled.desc.SampleDesc.Count = 4;
    assert(unusable(sampled));

    // Every guide is checked before any view is written.
    auto refused = [](auto change) {
        Slot t;
        GuideSet bad;
        change(bad);
        events.clear();
        const bool threw = Throws([&] { t.gpu.WriteDescriptors(t.pipelines, bad.resources); });
        return threw && events.empty() && !t.gpu.Described();
    };
    assert(refused([](GuideSet &b) { b.resources[1].Reset(); }));
    assert(refused([](GuideSet &b) { b.fakes[3].desc.Format = DXGI_FORMAT_R16_UINT; }));
    assert(refused([](GuideSet &b) { b.fakes[2].desc.Format = DXGI_FORMAT_R11G11B10_FLOAT; }));
    assert(refused([](GuideSet &b) { b.fakes[0].desc.Flags = D3D12_RESOURCE_FLAG_NONE; }));
    assert(refused([](GuideSet &b) { b.fakes[1].desc.SampleDesc.Count = 4; }));
    assert(
        refused([](GuideSet &b) { b.fakes[2].desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; }));
    // Not before the slot's resources exist.
    Gpu unprepared;
    FakeDevice device;
    FakePipelines p(&device);
    assert(Throws([&] { unprepared.WriteDescriptors(p, g.resources); }));
}

static void TestRecord() {
    Slot s;
    GuideSet g;
    Pipeline pipeline;
    assert(pipeline.Prepare(s.pipelines));
    s.gpu.WriteDescriptors(s.pipelines, g.resources);
    FakeList list;
    events.clear();
    s.gpu.Record(&list, pipeline, g.resources, 1919, 1081);
    constexpr auto common = D3D12_RESOURCE_STATE_COMMON,
                   read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   dest = D3D12_RESOURCE_STATE_COPY_DEST, source = D3D12_RESOURCE_STATE_COPY_SOURCE;
    const std::vector<std::string> expected = {
        Transition("coverage", common, read),
        Transition("normal", read, uav),
        Transition("diffuse", read, uav),
        Transition("specular", read, uav),
        Transition("stats", common, dest),
        "copy stats+0 <- control+64 n=16",
        Transition("stats", dest, uav),
        "descriptor-heaps n=1 heap",
        "compute-root blend-root",
        "pipeline-state blend-pso",
        "root-cbv 0 0x2000000",
        Format("root-table 1 %#llx", (unsigned long long)s.Heap(false).gpu),
        "root-uav 2 0x3000000",
        "dispatch 240 136 1",
        "uav-barrier null flags=0",
        Transition("normal", uav, read),
        Transition("diffuse", uav, read),
        Transition("specular", uav, read),
        Transition("coverage", read, common),
        Transition("stats", uav, source),
        "copy readback+0 <- stats+0 n=16",
        Transition("stats", source, common)};
    assert(events == expected);
    static_assert(noexcept(s.gpu.Record(&list, pipeline, g.resources, 1, 1)));

    // With an SSS guide it goes to UAV beside the albedo guides and back.
    auto &sss = g.Sss();
    s.gpu.WriteDescriptors(s.pipelines, g.resources, &sss);
    events.clear();
    s.gpu.Record(&list, pipeline, g.resources, 1919, 1081);
    auto with = expected;
    with.insert(with.begin() + 4, Transition("sss", read, uav));
    with.insert(with.begin() + 19, Transition("sss", uav, read));
    assert(with[18] == Transition("specular", uav, read));
    assert(events == with);
}

static void TestHarvest() {
    Slot s;
    auto &memory = s.Get("readback").memory;
    const UINT values[] = {1, 2000, 300, 9600, 77};
    memcpy(memory, values, sizeof(values));
    // Nothing recorded yet.
    auto stats = s.gpu.Harvest();
    assert(!stats.ran && !stats.eligible && !stats.covered && !stats.opacity64);
    GuideSet g;
    Pipeline pipeline;
    assert(pipeline.Prepare(s.pipelines));
    s.gpu.WriteDescriptors(s.pipelines, g.resources);
    FakeList list;
    s.gpu.Record(&list, pipeline, g.resources, 1920, 1080);
    memcpy(memory, values, sizeof(values)); // the recorded copy lands on the GPU
    stats = s.gpu.Harvest();
    assert(stats.ran == 1 && stats.eligible == 2000 && stats.covered == 300 &&
           stats.opacity64 == 9600);
    // Each recording is harvested once.
    stats = s.gpu.Harvest();
    assert(!stats.ran && !stats.eligible && !stats.covered && !stats.opacity64);
    assert(!Gpu{}.Harvest().ran);
    static_assert(sizeof(PassStats) == 16 && noexcept(s.gpu.Harvest()));
}

// A recorded SR list that never executes leaves no stale stats: Record clears
// the readback words, so the next Harvest reports ran == 0 instead of the
// previous completed dispatch again.
static void TestHarvestUnexecuted() {
    Slot s;
    auto &memory = s.Get("readback").memory;
    GuideSet g;
    Pipeline pipeline;
    assert(pipeline.Prepare(s.pipelines));
    s.gpu.WriteDescriptors(s.pipelines, g.resources);
    FakeList list;
    s.gpu.Record(&list, pipeline, g.resources, 1920, 1080);
    const UINT values[] = {1, 2000, 300, 9600};
    memcpy(memory, values, sizeof(values)); // that dispatch completed
    auto stats = s.gpu.Harvest();
    assert(stats.ran == 1 && stats.eligible == 2000 && stats.covered == 300 &&
           stats.opacity64 == 9600);
    // Recorded again; the list is never executed, so no copy lands.
    s.gpu.Record(&list, pipeline, g.resources, 1920, 1080);
    stats = s.gpu.Harvest();
    assert(!stats.ran && !stats.eligible && !stats.covered && !stats.opacity64);
    // Only the 16 stats bytes are cleared.
    for (size_t i = sizeof(PassStats); i < sizeof(memory); ++i)
        assert(memory[i] == 0xcd);
}

static void TestRelease() {
    Slot s;
    auto &control = s.Get("control");
    auto &readback = s.Get("readback");
    events.clear();
    s.gpu.Release();
    assert(control.unmaps == 1 && readback.unmaps == 1);
    // The control buffer as written; the readback with the empty range the debug layer wants.
    const std::vector<std::string> unmapped = {"unmap control sub=0 written=all",
                                               "unmap readback sub=0 written=0-0"};
    assert(events == unmapped);
    assert(!s.gpu.Ready() && !s.gpu.target && !s.gpu.control && !s.gpu.stats && !s.gpu.readback);
    assert(!s.gpu.rtv && !s.gpu.heap && !s.gpu.bytes && !s.gpu.ControlAddress());
    assert(!s.gpu.Described() && s.gpu.Matches(Scene(1280, 720)));
    for (auto &r : s.owner.resources)
        assert(r.refs == 1);
    for (auto &h : s.device.heaps)
        assert(h.refs == 1);
    events.clear();
    Gpu{}.Release();
    assert(events.empty());
}

// GpuFrame::Release releases the coverage resources with the frame.
static void TestFrameRelease() {
    FakeDevice device;
    FakePipelines p(&device);
    FakeOwner owner;
    FakeBudget fake;
    rrlive::Budget budget;
    rrlive::GpuFrame frame;
    assert(frame.coverage.Prepare(owner, p, fake, Scene()));
    events.clear();
    frame.Release(budget);
    assert(owner.Find("control")->unmaps == 1 && owner.Find("readback")->unmaps == 1);
    assert(!frame.coverage.Ready() && !frame.coverage.target);
    for (auto &r : owner.resources)
        assert(r.refs == 1);
}

int main() {
    TestTicket();
    TestPipelineTyped();
    TestPipelineBuild();
    TestPipelineLive();
    TestShapeAndMatches();
    TestRequiredBytes();
    TestPrepare();
    TestPrepareResumes();
    TestPrepareLiveFrame();
    TestPrepareFullLedger();
    TestPrepareReserve();
    TestCapture();
    TestControl();
    TestControlZero();
    TestDescriptors();
    TestRecord();
    TestHarvest();
    TestHarvestUnexecuted();
    TestRelease();
    TestFrameRelease();
    puts("PASS: transparency coverage ticket, typed-load gate, blend pipeline, coverage "
         "resources, resumable preparation, ledger reservation, capture, control, "
         "descriptors, recording, harvest and release");
}
