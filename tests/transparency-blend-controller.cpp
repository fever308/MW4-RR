#define NOMINMAX
#include "../src/rr_live.h"
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Controller side of the transparency albedo blend (src/rr_live_coverage.inl):
// capture tickets, the AfterSR blend, the submission seal, the pool
// reservation and the status row. Plain assert: never built with NDEBUG.
// CPU-only COM doubles, as in tests/transparency-coverage.cpp, plus live
// allocation: the fake device creates committed resources and answers the
// adapter and architecture queries that rrlive::Budget asks the fake DXGI
// below. No device, adapter, queue or GPU is created.
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
static ID3D12Device *child_device{}; // what every child's GetDevice returns
template <class I> struct Child : Com<I> {
    using Com<I>::Com;
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID id, void **out) override {
        if (child_device)
            return child_device->QueryInterface(id, out);
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
    std::deque<FakeHeap> heaps;
    std::deque<FakeRoot> roots;
    std::deque<FakePso> pipelines;
    std::vector<unsigned char> root_blob;
    const void *cs{};
    size_t cs_size{};
    std::deque<FakeResource> created;
    UINT64 created_bytes{};
    unsigned root_fail = 0; // CreateRootSignature fails while nonzero
    explicit FakeDevice(const char *name = "device") : Com(name) {}
    // Allocation sizes in 64 KiB pages, by texel size and samples.
    static UINT64 Size(const D3D12_RESOURCE_DESC &d) {
        auto round = [](UINT64 n) { return (n + 65535) & ~65535ull; };
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
            return round(d.Width);
        const UINT64 texel = d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8
                             : d.Format == DXGI_FORMAT_R16_FLOAT        ? 2
                                                                        : 4;
        return round(d.Width * d.Height * texel * (d.SampleDesc.Count ? d.SampleDesc.Count : 1));
    }
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
            static_cast<D3D12_FEATURE_DATA_ARCHITECTURE *>(data)->UMA = FALSE;
            return S_OK;
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
        if (root_fail) {
            --root_fail;
            *out = nullptr;
            return E_FAIL;
        }
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
        return {Size(*d), 65536};
    }
    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(UINT,
                                                                    D3D12_HEAP_TYPE) override {
        Unexpected("GetCustomHeapProperties");
        return {};
    }
    HRESULT STDMETHODCALLTYPE CreateCommittedResource(
        const D3D12_HEAP_PROPERTIES *heap, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *d,
        D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID id, void **out) override {
        events.push_back(Format("create-committed type=%u dim=%u", unsigned(heap->Type),
                                unsigned(d->Dimension)));
        created_bytes += Size(*d);
        auto &r = created.emplace_back("committed", *d, 0x100000 * (created.size() + 1));
        return r.QueryInterface(id, out);
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
    // Budget looks the adapter up by LUID: the fake DXGI's adapter.
    LUID STDMETHODCALLTYPE GetAdapterLuid() override {
        events.push_back("adapter-luid");
        return {0x1234, 0};
    }
};

struct FakeList final : Child<ID3D12GraphicsCommandList> {
    explicit FakeList(const char *name = "list") : Child(name) {}
    void Unexpected(const char *what) {
        events.push_back(Format("unexpected list %s", what));
    }
    D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override {
        return type;
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

struct FakeFence final : Child<ID3D12Fence> {
    UINT64 completed{};
    FakeFence() : Child("fence") {}
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override {
        return completed;
    }
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64, HANDLE) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Signal(UINT64) override {
        return E_NOTIMPL;
    }
};
struct FakeQueue final : Child<ID3D12CommandQueue> {
    std::vector<UINT64> signals;
    FakeQueue() : Child("queue") {}
    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource *, UINT,
                                              const D3D12_TILED_RESOURCE_COORDINATE *,
                                              const D3D12_TILE_REGION_SIZE *, ID3D12Heap *, UINT,
                                              const D3D12_TILE_RANGE_FLAGS *, const UINT *,
                                              const UINT *, D3D12_TILE_MAPPING_FLAGS) override {}
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource *,
                                            const D3D12_TILED_RESOURCE_COORDINATE *,
                                            ID3D12Resource *,
                                            const D3D12_TILED_RESOURCE_COORDINATE *,
                                            const D3D12_TILE_REGION_SIZE *,
                                            D3D12_TILE_MAPPING_FLAGS) override {}
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT, ID3D12CommandList *const *) override {}
    void STDMETHODCALLTYPE SetMarker(UINT, const void *, UINT) override {}
    void STDMETHODCALLTYPE BeginEvent(UINT, const void *, UINT) override {}
    void STDMETHODCALLTYPE EndEvent() override {}
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence *, UINT64 value) override {
        signals.push_back(value);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence *, UINT64) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64 *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64 *, UINT64 *) override {
        return E_NOTIMPL;
    }
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override {
        return {D3D12_COMMAND_LIST_TYPE_DIRECT};
    }
};

// A fake DXGI: defining CreateDXGIFactory1 here keeps the import from being
// linked, so rrlive::Budget sees this adapter's LOCAL budget and usage.
static UINT64 dxgi_budget = 64ull << 30, dxgi_other = 0; // budget, and usage by others
static FakeDevice *dxgi_device{};                        // its created bytes are our usage
static unsigned dxgi_queries{};
static UINT64 Usage() {
    return dxgi_other + (dxgi_device ? dxgi_device->created_bytes : 0);
}
struct FakeAdapter final : IDXGIAdapter3 {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDXGIAdapter3) ||
            id == __uuidof(IDXGIAdapter) || id == __uuidof(IDXGIAdapter1) ||
            id == __uuidof(IDXGIAdapter2)) {
            *out = static_cast<IDXGIAdapter3 *>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return 2;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        return 1;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void **) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT, IDXGIOutput **) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID, LARGE_INTEGER *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1 *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetDesc2(DXGI_ADAPTER_DESC2 *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE
    RegisterHardwareContentProtectionTeardownStatusEvent(HANDLE, DWORD *) override {
        return E_NOTIMPL;
    }
    void STDMETHODCALLTYPE UnregisterHardwareContentProtectionTeardownStatus(DWORD) override {}
    HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(UINT, DXGI_MEMORY_SEGMENT_GROUP group,
                                                   DXGI_QUERY_VIDEO_MEMORY_INFO *info) override {
        ++dxgi_queries;
        *info = {};
        const bool local = group == DXGI_MEMORY_SEGMENT_GROUP_LOCAL;
        info->Budget = local ? dxgi_budget : (64ull << 30);
        info->CurrentUsage = local ? Usage() : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVideoMemoryReservation(UINT, DXGI_MEMORY_SEGMENT_GROUP,
                                                        UINT64) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE RegisterVideoMemoryBudgetChangeNotificationEvent(HANDLE,
                                                                               DWORD *) override {
        return E_NOTIMPL;
    }
    void STDMETHODCALLTYPE UnregisterVideoMemoryBudgetChangeNotification(DWORD) override {}
};
static FakeAdapter fake_adapter;
struct FakeFactory final : IDXGIFactory4 {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDXGIFactory4) ||
            id == __uuidof(IDXGIFactory1) || id == __uuidof(IDXGIFactory)) {
            *out = static_cast<IDXGIFactory4 *>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return 2;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        return 1;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void **) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT, IDXGIAdapter **) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND, UINT) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown *, DXGI_SWAP_CHAIN_DESC *,
                                              IDXGISwapChain **) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE, IDXGIAdapter **) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT, IDXGIAdapter1 **) override {
        return E_NOTIMPL;
    }
    BOOL STDMETHODCALLTYPE IsCurrent() override {
        return TRUE;
    }
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override {
        return FALSE;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown *, HWND,
                                                     const DXGI_SWAP_CHAIN_DESC1 *,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *,
                                                     IDXGIOutput *, IDXGISwapChain1 **) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown *, IUnknown *,
                                                           const DXGI_SWAP_CHAIN_DESC1 *,
                                                           IDXGIOutput *,
                                                           IDXGISwapChain1 **) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE, LUID *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND, UINT, DWORD *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE, DWORD *) override {
        return E_NOTIMPL;
    }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD) override {}
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND, UINT, DWORD *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE, DWORD *) override {
        return E_NOTIMPL;
    }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD) override {}
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown *,
                                                            const DXGI_SWAP_CHAIN_DESC1 *,
                                                            IDXGIOutput *,
                                                            IDXGISwapChain1 **) override {
        return E_NOTIMPL;
    }
    UINT STDMETHODCALLTYPE GetCreationFlags() override {
        return 0;
    }
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID luid, REFIID id, void **out) override {
        if (luid.LowPart == 0x1234 && luid.HighPart == 0)
            return fake_adapter.QueryInterface(id, out);
        *out = nullptr;
        return DXGI_ERROR_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID, void **out) override {
        *out = nullptr;
        return E_NOTIMPL;
    }
};
static FakeFactory fake_factory;
extern "C" HRESULT __stdcall CreateDXGIFactory1(REFIID id, void **factory) {
    return fake_factory.QueryInterface(id, factory);
}

static std::vector<std::string> logs;
static bool Logged(const char *text) {
    for (const auto &line : logs)
        if (line.find(text) != std::string::npos)
            return true;
    return false;
}
static bool Recorded(const char *event) {
    return std::find(events.begin(), events.end(), event) != events.end();
}
static D3D12_RESOURCE_DESC Texture(UINT64 w, UINT h, DXGI_FORMAT format, UINT samples = 1,
                                   D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc = {samples, 0};
    d.Flags = flags;
    return d;
}
// The native MSAA scene target the OIT resolve renders into: half the render size.
static D3D12_RESOURCE_DESC Scene(UINT64 w = 960, UINT h = 540) {
    return Texture(w, h, DXGI_FORMAT_R11G11B10_FLOAT, 4, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
}
static bool OwnsLock(std::recursive_mutex &m) {
    bool locked = false;
    std::thread([&] {
        locked = m.try_lock();
        if (locked)
            m.unlock();
    }).join();
    return !locked;
}

namespace rrlive {
// Friend of Controller (rr_live.h): drives its private state directly.
struct ControllerCpuTest {
    using Slot = Controller::Slot;
    // A live controller on a fake device: RR on, a fresh camera and one warm
    // slot whose guides are 1920x1080 (w x h), as after its first frame.
    struct Rig {
        FakeDevice device;
        FakeFence fence;
        FakeQueue queue;
        FakeList list{"oit-list"}, other{"other-list"}, pre{"pre-list"}, sr{"sr-list"};
        FakeResource scene;
        FakeResource output{"rr-output", TextureDesc(3840, 2160, DXGI_FORMAT_R16G16B16A16_FLOAT)};
        std::deque<FakeResource> guides;
        Controller c;
        explicit Rig(UINT w = 1920, UINT h = 1080) : scene("scene", Scene(w / 2, h / 2)) {
            child_device = &device;
            dxgi_device = &device;
            dxgi_budget = 64ull << 30;
            dxgi_other = 0;
            events.clear();
            logs.clear();
            c.logger = [](const char *line) { logs.emplace_back(line); };
            c.lighting_valid = [](UINT64, UINT64) { return true; };
            c.SetEnabled(true);
            c.initialized = true;
            c.pipelines.device = &device;
            c.pipelines.stride = 32;
            c.fence = &fence;
            Camera();
            auto &f = c.slots[0].gpu;
            f.w = w;
            f.h = h;
            f.rr_output = &output;
            f.bytes = FakeDevice::Size(output.desc);
            c.budget.bytes = f.bytes;
            device.created_bytes = f.bytes;
            const char *names[] = {"normal-roughness", "diffuse", "specular", "status"};
            for (unsigned i = 0; i < 4; ++i) {
                guides.emplace_back(names[i],
                                    i < 3 ? Texture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, 1,
                                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
                                          : Texture(w, h, DXGI_FORMAT_R32_UINT));
                f.guides[i] = &guides.back();
            }
        }
        ~Rig() {
            // GpuFrame never unmaps on destruction; the controller goes first.
            child_device = nullptr;
            dxgi_device = nullptr;
        }
        void Camera(UINT frame = 7) {
            c.camera_frame = frame;
            c.camera_view = 0;
            c.latest.readable = c.latest.basis_valid = c.latest.projection_valid = true;
            c.camera_time = GetTickCount64();
        }
        Controller::CoverageToken Capture(FakeList &cl, DXGI_SAMPLE_DESC sample = {4, 0}) {
            return c.CoverageCapture(&cl, &scene, sample);
        }
        Slot &S(size_t i = 0) {
            return c.slots[i];
        }
    };
    static void PoolExtra() {
        UINT64 e = 1;
        assert(Controller::CoveragePoolExtra(6, 100, 0, e) && e == 600);
        assert(Controller::CoveragePoolExtra(6, 100, 40, e) && e == 360);
        assert(Controller::CoveragePoolExtra(6, 100, 100, e) && e == 0);
        assert(Controller::CoveragePoolExtra(6, 100, 200, e) && e == 0);
        assert(Controller::CoveragePoolExtra(0, 100, 0, e) && e == 0);
        assert(Controller::CoveragePoolExtra(1, UINT64_MAX, 0, e) && e == UINT64_MAX);
        assert(!Controller::CoveragePoolExtra(2, UINT64_MAX, 0, e) && e == 0);
        assert(!Controller::CoveragePoolExtra(3, UINT64_MAX / 2, 0, e) && e == 0);
        assert(Controller::CoveragePoolExtra(2, UINT64_MAX / 2, 0, e) && e == UINT64_MAX - 1);
    }

    static void State() {
        using rrstatus::State;
        Controller c; // no device, no driver
        assert(c.TransparencyBlendEnabled());
        assert(c.TransparencyBlendState(true, false) == State::ParentOff); // RR off
        c.SetEnabled(true);
        assert(c.TransparencyBlendState(true, false) == State::Waiting);
        assert(c.TransparencyBlendState(false, false) == State::Unavailable);
        assert(c.TransparencyBlendState(true, true) == State::Unavailable);
        c.blend_unavailable = rrblend::Reason::Typed;
        assert(c.TransparencyBlendState(true, false) == State::Unavailable);
        c.blend_unavailable = rrblend::Reason::None;
        c.blend_last_ran = GetTickCount64();
        assert(c.TransparencyBlendState(true, false) == State::Active);
        c.blend_last_ran = GetTickCount64() - 1500;
        assert(c.TransparencyBlendState(true, false) == State::Waiting);
        c.blend_failed = true;
        assert(c.TransparencyBlendState(false, true) == State::Error);
        c.failed = true;
        assert(c.TransparencyBlendState(true, false) == State::ParentOff);
        c.failed = false;
        c.retired = true;
        assert(c.TransparencyBlendState(true, false) == State::ParentOff);
        c.SetTransparencyBlendEnabled(false);
        assert(!c.TransparencyBlendEnabled() && !c.blend_failed);
        assert(c.TransparencyBlendState(true, false) == State::Off);

        // Toggling restarts history, clears a failure and the memory retry.
        Controller t;
        t.SetEnabled(true);
        assert(t.policy.Next(1, t.generation, false));
        assert(!t.policy.Next(2, t.generation, false));
        t.SetTransparencyBlendEnabled(true); // unchanged: no restart
        assert(!t.policy.Next(3, t.generation, false));
        t.blend_failed = true;
        t.blend_retry = 5;
        t.SetTransparencyBlendEnabled(false);
        assert(!t.blend_failed && !t.blend_retry);
        assert(t.policy.Next(4, t.generation, false));
        t.blend_failed = true;
        t.SetTransparencyBlendEnabled(true);
        assert(!t.blend_failed && t.policy.Next(5, t.generation, false));

        // Tuning is clamped.
        rrblend::Tuning tuning;
        tuning.diffuse = 7.f;
        tuning.roughness = 3.f;
        t.SetTransparencyBlendTuning(tuning);
        assert(t.blend_tuning.diffuse == 1.f && t.blend_tuning.roughness == 1.f);
    }

    static void Capture() {
        Rig r;
        auto &c = r.c;
        auto &s = r.S();
        auto t = r.Capture(r.list);
        assert(t && t.first && t.slot == &s && t.cl == &r.list && t.gpu == &s.gpu.coverage);
        assert(t.rtv.ptr == s.gpu.coverage.rtv->GetCPUDescriptorHandleForHeapStart().ptr);
        assert(OwnsLock(c.mutex)); // the token holds the controller lock
        assert(s.busy && s.awaiting_prepass && s.frame == 7 && s.generation == c.generation);
        assert(s.coverage.capture.Get() == &r.list && s.coverage.Live() && s.transparency.Live());
        assert(s.gpu.coverage.Ready() && c.blend_pipeline.Ready());
        assert(s.gpu.coverage.desc.Width == 960 && s.gpu.coverage.desc.SampleDesc.Count == 4);
        c.CoverageCaptured(std::move(t), rrblend::Kind::Oit);
        assert(!t && !OwnsLock(c.mutex));
        assert(s.coverage.captures_oit == 1 && !s.coverage.captures_refr && s.coverage.cleared);
        assert(c.blend_counters.captures_oit == 1);
        assert(Logged("Transparency albedo blend: first OIT resolve captured (960x540, x4)"));

        // Later captures on the same list do not clear again.
        auto t2 = r.Capture(r.list);
        assert(t2 && !t2.first && t2.slot == &s);
        c.CoverageCaptured(std::move(t2), rrblend::Kind::Refraction, true);
        assert(s.coverage.captures_refr == 1 && s.coverage.captures_refr_alpha == 1);
        auto t3 = r.Capture(r.list);
        c.CoverageCaptured(std::move(t3), rrblend::Kind::Refraction, false);
        assert(s.coverage.captures_refr == 2 && s.coverage.captures_refr_alpha == 1);
        assert(c.blend_counters.captures_refr == 2);

        // Refusals change nothing and record nothing.
        const auto before = events.size();
        assert(!r.Capture(r.other) && c.blend_counters.last == rrblend::Reason::Split);
        assert(!r.Capture(r.list, {4, 16}) && c.blend_counters.last == rrblend::Reason::NoPipeline);
        assert(!r.Capture(r.list, {1, 0}) && c.blend_counters.last == rrblend::Reason::NoPipeline);
        r.list.type = D3D12_COMMAND_LIST_TYPE_BUNDLE;
        assert(!r.Capture(r.list) && c.blend_counters.last == rrblend::Reason::Extent);
        r.list.type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        s.coverage.submitted = true;
        assert(!r.Capture(r.list) && c.blend_counters.last == rrblend::Reason::Order);
        s.coverage.submitted = false;
        s.recorded = true;
        assert(!r.Capture(r.list)); // after AfterSR, the slot takes no captures
        s.recorded = false;
        assert(events.size() == before);
        assert(s.coverage.captures_oit == 1 && s.coverage.captures_refr == 2);
        assert(!OwnsLock(c.mutex));

        // The claimed slot stays while its capture list is live, then retires.
        c.Poll();
        assert(s.busy);
        c.Reset(&r.other);
        assert(s.busy && s.coverage.Live());
        c.Reset(&r.list);
        assert(!s.busy && !s.coverage.capture && !s.coverage.Live() && !s.transparency.Live());
        assert(!s.coverage.captures_oit && !s.coverage.cleared && !s.blend_used);
        assert(s.gpu.coverage.Ready()); // the resources stay with the slot's frame

        // Retirement waits for a live capture and for an unfenced submission.
        auto t4 = r.Capture(r.list);
        c.CoverageCaptured(std::move(t4), rrblend::Kind::Oit);
        c.QueueReconfigure();
        assert(!c.Reconfigure() && c.initialized);
        c.Reset(&r.list);
        s.coverage.submitted = true; // submitted, but no slot fence yet
        assert(!s.fence_value && !c.Reconfigure() && c.initialized);
        s.coverage.submitted = false;
        assert(c.Reconfigure() && !c.initialized);
        assert(!c.blend_pipeline.Ready() && !c.slots[0].gpu.coverage.Ready());
        assert(c.slots[0].gpu.coverage.bytes == 0 && c.budget.bytes == 0);
    }

    static void Refusals() {
        {
            Rig r;
            r.c.SetTransparencyBlendEnabled(false);
            assert(!r.Capture(r.list) && !r.S().busy && events.empty());
            assert(r.c.blend_counters.last == rrblend::Reason::None);
            r.c.SetTransparencyBlendEnabled(true);
            r.c.SetEnabled(false);
            assert(!r.Capture(r.list) && !r.S().busy && events.empty());
            r.c.SetEnabled(true);
            r.c.camera_time = GetTickCount64() - 5000; // stale camera
            assert(!r.Capture(r.list) && r.c.blend_counters.last == rrblend::Reason::NoSlot);
            r.Camera();
            r.c.QueueReconfigure();
            assert(!r.Capture(r.list));
            r.c.layout_pending = false;
            assert(r.Capture(r.list));
        }
        {
            Rig r;
            r.S().gpu.w = 1918; // the scene is not half these guides
            assert(!r.Capture(r.list) && !r.S().busy && events.empty());
        }
        {
            // Typed UAV loads of the guide format are missing: unavailable, never failed.
            Rig r;
            r.device.typed = FALSE;
            assert(!r.Capture(r.list) && r.c.blend_unavailable == rrblend::Reason::Typed);
            assert(!r.c.blend_failed && r.device.created.empty());
            assert(Logged("Transparency albedo blend unavailable: typed UAV load"));
            assert(r.c.TransparencyBlendState(true, false) == rrstatus::State::Unavailable);
            assert(!r.Capture(r.list));
            r.c.CoverageReconfigured();
            assert(r.c.blend_unavailable == rrblend::Reason::None);
        }
        {
            // A pipeline failure turns only the blend off, until toggled.
            Rig r;
            r.device.root_fail = 1;
            bool threw = false;
            try {
                assert(!r.Capture(r.list));
            } catch (...) {
                threw = true;
            }
            assert(!threw && r.c.blend_failed && !r.c.failed && r.c.Enabled());
            assert(Logged("Transparency albedo blend recording failed: "));
            assert(r.c.TransparencyBlendState(true, false) == rrstatus::State::Error);
            const auto lines = logs.size();
            assert(!r.Capture(r.list) && logs.size() == lines); // logged once
            r.c.SetTransparencyBlendEnabled(false);
            r.c.SetTransparencyBlendEnabled(true);
            assert(r.Capture(r.list));
        }
        {
            // A failure after the caller touched the list.
            Rig r;
            auto t = r.Capture(r.list);
            r.c.CoverageFailed(std::move(t), "replay refused");
            assert(!t && !OwnsLock(r.c.mutex) && r.c.blend_failed && !r.c.failed);
            assert(Logged("recording failed: replay refused; disabled until toggled"));
            assert(r.S().coverage.Live() && !r.S().coverage.captures_oit);
            assert(!r.Capture(r.list));
        }
    }

    // Coverage reserves the whole pool first: a later mandatory pool
    // reservation (a slot's first prepass, rr_live.h Prepass) must still fit
    // the ceiling when the GPU has no free memory left.
    static void PoolReservation() {
        const UINT w = 3840, h = 2160;
        Rig r(w, h);
        auto &c = r.c;
        auto &b = c.budget;
        const UINT64 prepass = GpuFrame::RequiredPrepassBytes(&r.device, w, h);
        const UINT64 out = FakeDevice::Size(r.output.desc);
        const UINT64 C = rrblend::Gpu::RequiredBytes(&r.device, Scene(w / 2, h / 2));
        assert(prepass && C);
        // Three warm slots hold their prepass storage (slot 0 also its output).
        for (size_t i = 0; i < 3; ++i) {
            auto &f = c.slots[i].gpu;
            f.w = w;
            f.h = h;
            f.bytes += prepass;
            b.bytes += prepass;
            r.device.created_bytes += prepass;
        }
        assert(c.slots[0].gpu.bytes == prepass + out);
        assert(c.ReservePool(c.slots.size(), nullptr, nullptr, w, h));
        const UINT64 limit0 = b.limit;
        assert(limit0 == 6 * (prepass + out));

        // Not enough room for coverage on every slot: wait, allocate nothing.
        dxgi_budget = Usage() + (limit0 - b.bytes) + 5 * C;
        auto t = r.Capture(r.list);
        assert(!t && c.blend_counters.last == rrblend::Reason::Memory && c.blend_retry);
        assert(r.device.created.empty() && b.limit == limit0 && !r.S().gpu.coverage.target);
        assert(Logged("Transparency albedo blend waiting for memory"));
        dxgi_queries = 0;
        assert(!r.Capture(r.list) && dxgi_queries == 0); // retried only after 1 s
        assert(c.PrepareCoverage(r.S(), Scene(w / 2, h / 2), GetTickCount64()) ==
               rrblend::Reason::Memory);
        assert(dxgi_queries == 0 && !c.blend_failed);

        // Room for all six: the ceiling grows by six coverages before the first.
        dxgi_budget = Usage() + (limit0 - b.bytes) + 6 * C;
        c.blend_retry = 1;
        t = r.Capture(r.list);
        assert(t && r.S().gpu.coverage.bytes == C && !c.blend_retry);
        assert(b.limit == limit0 + 6 * C && b.bytes == 3 * prepass + out + C);
        c.CoverageCaptured(std::move(t), rrblend::Kind::Oit);

        // The game takes every free byte. Slot 3's first prepass reserves the
        // pool again under Require (Failure() on false): it still fits.
        dxgi_budget = Usage();
        dxgi_queries = 0;
        assert(c.ReservePool(c.slots.size(), nullptr, nullptr, w, h));
        assert(dxgi_queries == 0 && b.limit == limit0 + 6 * C);

        // Coverage on another slot needs only its own memory, no new reservation.
        dxgi_budget = Usage() + C;
        assert(c.PrepareCoverage(c.slots[1], Scene(w / 2, h / 2), GetTickCount64()) ==
               rrblend::Reason::None);
        assert(c.slots[1].gpu.coverage.bytes == C && b.limit == limit0 + 6 * C);
        dxgi_budget = Usage();
        dxgi_queries = 0;
        assert(c.ReservePool(c.slots.size(), nullptr, nullptr, w, h) && dxgi_queries == 0);
        for (size_t i = 0; i < 2; ++i) {
            // Fake resources are owned by the device; forget their mappings.
            c.slots[i].gpu.coverage.control_map = nullptr;
            c.slots[i].gpu.coverage.readback_map = nullptr;
        }
    }

    static void Blend() {
        Rig r;
        auto &c = r.c;
        auto &s = r.S();
        // No capture this frame: nothing is recorded.
        c.CoverageBlend(s, &r.sr, 1920, 1080);
        assert(!s.blend_used && events.empty());
        auto t = r.Capture(r.list);
        c.CoverageCaptured(std::move(t), rrblend::Kind::Oit);
        auto &g = s.gpu.coverage;
        rrblend::Tuning tuning;
        tuning.diffuse = 0.25f;
        c.SetTransparencyBlendTuning(tuning);

        // The target holds this frame's coverage only once a capture cleared it.
        events.clear();
        s.coverage.cleared = false;
        c.CoverageBlend(s, &r.sr, 1920, 1080);
        assert(!s.blend_used && events.empty());
        s.coverage.cleared = true;
        // A different render size than the capture's: no blend.
        c.CoverageBlend(s, &r.sr, 1918, 1080);
        assert(!s.blend_used && events.empty() && c.blend_counters.last == rrblend::Reason::Extent);
        c.CoverageBlend(s, &r.sr, 1920, 1080);
        assert(s.blend_used && !c.blend_failed);
        assert(Recorded("dispatch 240 135 1") && g.Described());
        const auto *k = reinterpret_cast<const rrblend::Control *>(g.control_map);
        assert(k->admitted == 0 && k->width == 1920 && k->height == 1080 && k->diffuse[0] == 0.25f);

        // The submission-time seal: word 0 only, frame acceptance untouched.
        c.SealCoverage(s, 0, 1, 2, true);
        assert(g.control_map[0] == 1 && c.blend_counters.admitted == 1 &&
               c.blend_counters.frames == 1);
        c.SealCoverage(s, 0, 1, 2, false);
        assert(g.control_map[0] == 0 && c.blend_counters.admitted == 1);
        c.SealCoverage(s, 0, 3, 2, true);
        assert(g.control_map[0] == 0 && c.blend_counters.last == rrblend::Reason::Order);
        c.SealCoverage(s, 2, 1, 3, true);
        assert(g.control_map[0] == 0);
        c.SealCoverage(s, 0, -1, 2, true);
        assert(g.control_map[0] == 0);
        s.coverage.submitted = true;
        c.SealCoverage(s, 0, 1, 2, true);
        assert(g.control_map[0] == 0 && c.blend_counters.frames == 6);
        s.coverage.submitted = false;
        s.blend_used = false;
        g.control_map[0] = 1;
        c.SealCoverage(s, 0, 1, 2, true);
        assert(g.control_map[0] == 0 && c.blend_counters.frames == 6); // not blended: 0

        // The GPU copied the stats back: harvested once, before the next Record.
        const UINT stats[] = {1, 1000, 500, 64 * 250};
        memcpy(g.readback_map, stats, sizeof(stats));
        c.blend_counters = {};
        s.coverage.submitted = true; // no blend this time
        c.CoverageBlend(s, &r.sr, 1920, 1080);
        const auto &n = c.blend_counters;
        assert(n.ran == 1 && n.pixels == 1920ull * 1080 && n.eligible == 1000 && n.covered == 500 &&
               n.opacity64 == 64 * 250);
        assert(c.TransparencyBlendState(true, false) == rrstatus::State::Active);
        c.CoverageBlend(s, &r.sr, 1920, 1080);
        assert(n.ran == 1 && n.pixels == 1920ull * 1080);
        // A recorded blend whose list never ran reads ran == 0: nothing counted.
        s.coverage.submitted = false;
        c.CoverageBlend(s, &r.sr, 1920, 1080);
        c.CoverageBlend(s, &r.sr, 1920, 1080);
        assert(n.ran == 1 && n.eligible == 1000);

        // A blend failure stays inside CoverageBlend: RR keeps running.
        FakeResource stranger("stranger", r.guides[1].desc);
        s.gpu.guides[1] = &stranger;
        s.blend_used = false;
        events.clear();
        bool threw = false;
        try {
            c.CoverageBlend(s, &r.sr, 1920, 1080);
        } catch (...) {
            threw = true;
        }
        assert(!threw && c.blend_failed && !c.failed && !s.blend_used && events.empty());
        assert(Logged("recording failed: coverage blend guides changed before retirement"));
        c.CoverageBlend(s, &r.sr, 1920, 1080);
        assert(events.empty());
        s.gpu.guides[1] = &r.guides[1];
        g.control_map = nullptr;
        g.readback_map = nullptr;
    }

    // Before(): the capture list's index, the slot fence and the seal.
    static void Submission(int order) {
        Rig r;
        auto &c = r.c;
        auto &s = r.S();
        UINT header[64]{};
        auto t = r.Capture(r.list);
        c.CoverageCaptured(std::move(t), rrblend::Kind::Oit);
        // order 1: the capture list was submitted before the prepass and SR.
        if (order == 1) {
            ID3D12CommandList *early[] = {&r.list};
            c.Before(&r.queue, 1, early);
            c.After(&r.queue);
            assert(s.coverage.submitted && s.fence_value == 1);
        }
        s.pre = &r.pre;
        s.pre_live = true;
        s.awaiting_prepass = false;
        s.sr = &r.sr;
        s.sr_live = true;
        s.gate_recorded = s.inference_ok = s.raw_corrected = true;
        s.gpu.header_map = header;
        c.CoverageBlend(s, &r.sr, 1920, 1080);
        s.recorded = true;
        assert(s.blend_used == (order != 1));
        ID3D12CommandList *in_order[] = {&r.pre, &r.list, &r.sr};
        ID3D12CommandList *late[] = {&r.pre, &r.sr, &r.list};
        ID3D12CommandList *without[] = {&r.pre, &r.sr};
        c.Before(&r.queue, order == 1 ? 2 : 3, order == 2 ? late : order == 1 ? without : in_order);
        c.After(&r.queue);
        assert(s.sealed && header[6] == 1); // coverage never rejects the frame
        assert(s.gpu.coverage.control_map[0] == (order == 0 ? 1u : 0u));
        assert(s.coverage.submitted && s.fence_value == r.queue.signals.back());
        assert(c.blend_counters.admitted == (order == 0 ? 1u : 0u));
        assert(!OwnsLock(c.mutex));
        s.gpu.header_map = nullptr;
        s.gpu.coverage.control_map = nullptr;
        s.gpu.coverage.readback_map = nullptr;
    }

    // Shutdown waits while a capture list can still be submitted.
    static void Shutdown() {
        Rig r;
        auto t = r.Capture(r.list);
        r.c.CoverageCaptured(std::move(t), rrblend::Kind::Oit);
        r.c.runtime = reinterpret_cast<HMODULE>(1);
        assert(!r.c.BeforeShutdown(reinterpret_cast<HMODULE>(1), nullptr));
        assert(Logged("RR shutdown deferred: an RR command recording remains replayable"));
        r.c.Reset(&r.list);
        assert(r.c.BeforeShutdown(reinterpret_cast<HMODULE>(1), nullptr));
        r.S().gpu.coverage.control_map = nullptr;
        r.S().gpu.coverage.readback_map = nullptr;
    }

    // A capture list reset before its submission ran nothing: its captures and
    // its clear are dropped. A submitted list's captures ran and are kept.
    static void ResetUnsubmitted() {
        Rig r;
        auto &s = r.S();
        s.pre_live = true; // the slot stays in use across the reset
        auto t = r.Capture(r.list);
        r.c.CoverageCaptured(std::move(t), rrblend::Kind::Refraction, true);
        assert(s.coverage.cleared && s.coverage.captures_refr == 1);
        r.c.Reset(&r.list);
        assert(s.busy && !s.coverage.Live() && !s.transparency.Live());
        assert(!s.coverage.cleared && !s.coverage.captures_oit && !s.coverage.captures_refr &&
               !s.coverage.captures_refr_alpha && !s.coverage.capture);
        // The list is recorded again: its first capture clears again.
        t = r.Capture(r.list);
        assert(t && t.first);
        r.c.CoverageCaptured(std::move(t), rrblend::Kind::Oit);
        s.coverage.submitted = true;
        r.c.Reset(&r.list);
        assert(!s.coverage.Live() && s.coverage.cleared && s.coverage.captures_oit == 1);
        s.gpu.coverage.control_map = nullptr;
        s.gpu.coverage.readback_map = nullptr;
    }

    // While RR runs, each 10 s check logs a state change (on, failed, unavailable, blending
    // or not), or a 5-minute summary; a line counts everything since the last one.
    static void Report() {
        Controller c;
        c.logger = [](const char *line) { logs.emplace_back(line); };
        logs.clear();
        c.SetEnabled(true);
        c.CoverageReportLocked(100000);
        c.blend_counters.captures_oit = 3;
        c.blend_counters.covered = 1234;
        c.CoverageReportLocked(105000);
        assert(logs.empty() && c.blend_counters.captures_oit == 3);
        c.CoverageReportLocked(110000);
        assert(logs.size() == 1 &&
               Logged("Transparency albedo blend (10s): frames=0 captures=3 oit=3 refr=0") &&
               Logged("lastReject=none changed=1234 state=on"));
        assert(c.blend_counters.captures_oit == 0 && c.blend_counters.covered == 0);
        // Nothing transparent seen is a change: the line comes, all zero.
        logs.clear();
        c.CoverageReportLocked(120000);
        assert(logs.size() == 1 &&
               Logged("frames=0 captures=0 oit=0 refr=0 admitted=0 eligible=0.0% covered=0.0%") &&
               Logged("lastReject=none changed=0 state=on"));
        // The same state again waits, and keeps counting.
        c.blend_counters.frames = 5;
        c.blend_counters.last = rrblend::Reason::NoSlot;
        c.CoverageReportLocked(130000);
        assert(logs.size() == 1 && c.blend_counters.frames == 5);
        // A failed blend logs at once, with the frames since the last line, then waits.
        logs.clear();
        c.blend_failed = true;
        c.CoverageReportLocked(140000);
        c.CoverageReportLocked(150000);
        assert(logs.size() == 1 && Logged("blend (20s): frames=5 ") &&
               Logged("lastReject=failed changed=0 state=failed"));
        // So does an unavailable one, and the same state comes again as the summary.
        logs.clear();
        c.blend_failed = false;
        c.blend_unavailable = rrblend::Reason::Typed;
        c.CoverageReportLocked(160000);
        assert(logs.size() == 1 && Logged("lastReject=typed-load changed=0 state=unavailable"));
        for (ULONGLONG t = 170000; t < 460000; t += 10000)
            c.CoverageReportLocked(t);
        assert(logs.size() == 1);
        c.CoverageReportLocked(460000);
        assert(logs.size() == 2 && logs[1].find("blend (300s): ") != std::string::npos);
        c.blend_unavailable = rrblend::Reason::None;
        // Blend off: one short line, not repeated while it stays off; counters reset.
        logs.clear();
        c.blend_counters.frames = 1;
        c.SetTransparencyBlendEnabled(false);
        c.CoverageReportLocked(470000);
        assert(logs.size() == 1 && logs[0] == "Transparency albedo blend: off");
        assert(c.blend_counters.frames == 0);
        c.CoverageReportLocked(475000);
        c.CoverageReportLocked(485000);
        assert(logs.size() == 1);
        // RR off, failed or retired: no line.
        c.SetTransparencyBlendEnabled(true);
        c.SetEnabled(false);
        c.CoverageReportLocked(495000);
        c.SetEnabled(true);
        c.failed = true;
        c.CoverageReportLocked(505000);
        c.failed = false;
        c.retired = true;
        c.CoverageReportLocked(515000);
        assert(logs.size() == 1);
    }

    // The render glue's signature refusal names the reason in the 10 s line,
    // and only while the blend is on, as the controller's own refusals do.
    static void SignatureRefusal() {
        Controller c;
        c.CoverageSignatureRefused();
        assert(c.blend_counters.last == rrblend::Reason::Signature);
        assert(!c.blend_failed && c.blend_unavailable == rrblend::Reason::None);
        c.blend_counters = {};
        c.SetTransparencyBlendEnabled(false);
        c.CoverageSignatureRefused();
        assert(c.blend_counters.last == rrblend::Reason::None);
    }

    static void Run() {
        PoolExtra();
        State();
        Capture();
        Refusals();
        PoolReservation();
        Blend();
        for (int order : {0, 1, 2})
            Submission(order);
        Shutdown();
        ResetUnsubmitted();
        Report();
        SignatureRefusal();
    }
};
} // namespace rrlive

int main() {
    rrlive::ControllerCpuTest::Run();
    printf("transparency-blend-controller: all tests passed\n");
    return 0;
}
