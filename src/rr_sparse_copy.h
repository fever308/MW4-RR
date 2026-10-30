#pragma once
#include "feed_fg_identity.h"
#include "rr_resolution_limits.h"
#include "rr_sparse_budget.h"
#include <dxgi1_4.h>
#include "rr_vrrt_data.h"
#include "rr_compute_state.h"
#include "../build/rr_vrrt_native.generated.h"
#include "rr_lighting_source.h"
#include "rr_sss_mask.h"
#include "rr_edge_fill.h"
#include <array>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <atomic>
namespace rrsparsecopy {
using Microsoft::WRL::ComPtr;
inline D3D12_RESOURCE_BARRIER Transition(ID3D12Resource *r, D3D12_RESOURCE_STATES a,
                                         D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER v{};
    v.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    v.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    return v;
}
// Opaque identities only. Holding COM references for every observed transition
// would retain the game's entire transient texture allocator.
class Layouts {
    inline static constexpr GUID IdentityId = {
        0x028c9157, 0x4821, 0x490b, {0xa4, 0xc8, 0x59, 0x34, 0x72, 0x1a, 0x17, 0x01}};
    inline static std::atomic<UINT64> nextIdentity{};
    struct State {
        D3D12_RESOURCE_STATES layout{};
        UINT64 identity{};
        bool aliased{}, named{}; // named: by a barrier naming this resource
        bool untracked{};        // a split or subresource transition: state not known
    };
    struct Recording {
        std::unordered_map<ID3D12Resource *, State> states;
        bool capped{};
        bool lossy{}; // some barriers on this list were not recorded (Status only)
    };
    std::mutex mutex;
    std::unordered_map<ID3D12GraphicsCommandList *, Recording> lists;
    bool overflowed{}; // a list was not recorded because 256 were already tracked
    static UINT64 Identity(ID3D12Resource *r, bool assign) {
        UINT64 value{};
        UINT size = sizeof(value);
        if (SUCCEEDED(r->GetPrivateData(IdentityId, &size, &value)) && size == sizeof(value) &&
            value)
            return value;
        if (!assign)
            return 0;
        auto previous = nextIdentity.load();
        do {
            if (previous == UINT64_MAX)
                return 0;
        } while (!nextIdentity.compare_exchange_weak(previous, previous + 1));
        value = previous + 1;
        if (FAILED(r->SetPrivateData(IdentityId, sizeof(value), &value)))
            return 0;
        return value;
    }

  public:
    void Reset(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(mutex);
        lists.erase(cl);
    }
    // cl's states are unknown from here, as after Reset, and Status never calls a resource
    // on cl untouched until cl is reset.
    void Lose(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(mutex);
        LoseLocked(cl);
    }
    void Observe(ID3D12GraphicsCommandList *cl, UINT n, const D3D12_RESOURCE_BARRIER *b) {
        std::lock_guard lock(mutex);
        if (!cl || n > 4096 || (n && !b)) {
            LoseLocked(cl);
            return;
        }
        if (!lists.contains(cl) && lists.size() >= 256) {
            overflowed = true;
            return;
        }
        auto &l = lists[cl];
        if (l.capped)
            return;
        for (UINT i = 0; i < n; ++i) {
            // Aliasing barriers synchronize overlapping memory usage; they do not
            // transition either resource object's state. Keep those states, but
            // separately withhold snapshot eligibility until a new transition.
            // State-only callers must independently prove current native use.
            // https://microsoft.github.io/DirectX-Specs/d3d/ResourceHeaps.html
            if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
                const auto &a = b[i].Aliasing;
                if (!a.pResourceBefore || !a.pResourceAfter)
                    for (auto &[resource, state] : l.states)
                        state.aliased = true;
                for (auto *resource : {a.pResourceBefore, a.pResourceAfter}) {
                    auto at = l.states.find(resource);
                    if (at != l.states.end())
                        at->second.aliased = at->second.named = true;
                }
            }
            if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                const auto &t = b[i].Transition;
                if (!t.pResource)
                    continue;
                if (b[i].Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE ||
                    t.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
                    // Kept as named but untracked, so Seen never calls it untouched.
                    if (l.states.size() >= 4096 && !l.states.contains(t.pResource)) {
                        l.states.clear();
                        l.capped = true;
                        return;
                    }
                    const auto id = Identity(t.pResource, true);
                    if (id)
                        l.states[t.pResource] = {{}, id, false, false, true};
                    else {
                        l.states.erase(t.pResource);
                        l.lossy = true;
                    }
                    continue;
                }
                if (l.states.size() >= 4096 && !l.states.contains(t.pResource)) {
                    l.states.clear();
                    l.capped = true;
                    return;
                }
                const auto id = Identity(t.pResource, true);
                if (id)
                    l.states[t.pResource] = {t.StateAfter, id};
                else {
                    l.states.erase(t.pResource);
                    l.lossy = true;
                }
            }
        }
    }
  private:
    void LoseLocked(ID3D12GraphicsCommandList *cl) {
        if (!cl)
            return;
        lists.erase(cl);
        if (lists.size() >= 256) {
            overflowed = true;
            return;
        }
        lists[cl].lossy = true;
    }
    const State *Find(ID3D12GraphicsCommandList *cl, ID3D12Resource *r) { // mutex held
        auto l = lists.find(cl);
        if (!r || l == lists.end() || l->second.capped)
            return nullptr;
        auto at = l->second.states.find(r);
        if (at == l->second.states.end() || at->second.untracked ||
            Identity(r, false) != at->second.identity)
            return nullptr;
        return &at->second;
    }
    bool Lookup(ID3D12GraphicsCommandList *cl, ID3D12Resource *r,
                D3D12_RESOURCE_STATES &value, bool require_unaliased) {
        std::lock_guard lock(mutex);
        const auto *at = Find(cl, r);
        if (!at || (require_unaliased && at->aliased))
            return false;
        value = at->layout;
        return true;
    }
  public:
    bool Get(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES &value) {
        return Lookup(cl, r, value, false);
    }
    // Additional conservative eligibility, not a guarantee of initialized contents.
    // The caller still needs its producer and command-order checks.
    bool GetUnaliased(ID3D12GraphicsCommandList *cl, ID3D12Resource *r,
                      D3D12_RESOURCE_STATES &value) {
        return Lookup(cl, r, value, true);
    }
    // Get, also saying which aliasing barrier on cl, if any, came after r's last
    // transition: Any for one with a null side (it covers every resource), Named
    // for one naming r.
    enum class Alias { None, Any, Named };
    bool Get(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES &value,
             Alias &alias) {
        std::lock_guard lock(mutex);
        const auto *at = Find(cl, r);
        if (!at)
            return false;
        value = at->layout;
        alias = at->named ? Alias::Named : at->aliased ? Alias::Any : Alias::None;
        return true;
    }
    // Known: its last transition on cl is tracked (Get). Untouched: cl's barriers were all
    // recorded and none named r since cl's reset, so r keeps the state it entered cl with.
    // Untracked: anything else (split or subresource transition, capped or unrecorded list,
    // a new resource at the same address).
    enum class Seen { Known, Untouched, Untracked };
    Seen Status(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES &value) {
        std::lock_guard lock(mutex);
        if (!cl || !r)
            return Seen::Untracked;
        auto l = lists.find(cl);
        if (l == lists.end())
            return overflowed ? Seen::Untracked : Seen::Untouched;
        if (l->second.capped || l->second.lossy)
            return Seen::Untracked;
        auto at = l->second.states.find(r);
        if (at == l->second.states.end())
            return Seen::Untouched;
        if (at->second.untracked || Identity(r, false) != at->second.identity)
            return Seen::Untracked;
        value = at->second.layout;
        return Seen::Known;
    }
    // Fixture simulates allocation lifetime rollover at the same opaque address.
    static void RetireIdentityForTest(ID3D12Resource *r) {
        r->SetPrivateData(IdentityId, 0, nullptr);
    }
};
struct Pair {
    std::array<ComPtr<ID3D12Resource>, 2> r;
    std::array<D3D12_RESOURCE_STATES, 2> state{};
};
inline bool Shape(const D3D12_RESOURCE_DESC &a, const D3D12_RESOURCE_DESC &b) {
    auto valid = [](const auto &d) {
        return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
               rrresolution::Texture(d.Width, d.Height) && d.MipLevels == 1 &&
               d.DepthOrArraySize == 1 && d.SampleDesc.Count == 1;
    };
    return valid(a) && valid(b) && a.Width == b.Width && a.Height == b.Height &&
           (a.Format == DXGI_FORMAT_R32G32B32A32_FLOAT ||
            a.Format == DXGI_FORMAT_R32G32B32A32_UINT) &&
           (b.Format == DXGI_FORMAT_R32_FLOAT || b.Format == DXGI_FORMAT_R32_UINT);
}
inline bool Shape(const Pair &p) {
    return p.r[0] && p.r[1] && p.r[0] != p.r[1] && Shape(p.r[0]->GetDesc(), p.r[1]->GetDesc());
}
inline bool SameShape(ID3D12Resource *a, ID3D12Resource *b) {
    auto x = a->GetDesc(), y = b->GetDesc();
    return x.Width == y.Width && x.Height == y.Height && x.Format == y.Format &&
           x.DepthOrArraySize == y.DepthOrArraySize && x.MipLevels == y.MipLevels &&
           x.SampleDesc.Count == y.SampleDesc.Count;
}
inline bool Readable(D3D12_RESOURCE_STATES s) {
    return (s & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) &&
           !(s & ~(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE));
}
inline bool SameDevice(ID3D12Device *d, ID3D12Resource *r) {
    ComPtr<ID3D12Device> actual;
    return r && SUCCEEDED(r->GetDevice(IID_PPV_ARGS(&actual))) &&
           fg::SameNativeObject(actual.Get(), d);
}
// Current-frame surface inputs of the edge-aware no-history fill (rr_edge_fill.h): the
// prepass PackedSurface the native spatial dispatch binds at t13, in its tracked readable
// state, and the slot's converted guide normals and Status, left COMMON by the conversion.
struct EdgeInputs {
    ComPtr<ID3D12Resource> surface, normal, status;
    D3D12_RESOURCE_STATES surface_state{};
};
// Each is one sample per lighting pixel in the format the fill shader reads.
inline bool EdgeShape(const D3D12_RESOURCE_DESC &surface, const D3D12_RESOURCE_DESC &normal,
                      const D3D12_RESOURCE_DESC &status, const D3D12_RESOURCE_DESC &lighting) {
    auto one = [&](const D3D12_RESOURCE_DESC &d, DXGI_FORMAT format) {
        return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == lighting.Width &&
               d.Height == lighting.Height && d.DepthOrArraySize == 1 && d.MipLevels == 1 &&
               d.SampleDesc.Count == 1 && d.Format == format;
    };
    return one(surface, DXGI_FORMAT_R32G32B32A32_UINT) &&
           one(normal, DXGI_FORMAT_R16G16B16A16_FLOAT) && one(status, DXGI_FORMAT_R32_UINT);
}
// Private shader variants: lobe * Variants + bits, bit 0 the edge-aware fill. A masked root
// is params 0-4, then the edge table (t3..t7) when bit 0 is set.
inline constexpr unsigned EdgeBit = 1, Variants = 4;
inline constexpr unsigned VariantIndex(unsigned lobe, unsigned bits) { return lobe * Variants + bits; }
inline constexpr unsigned EdgeTableParameter = 5;
inline constexpr unsigned PrivateDescriptors = 10; // 0-4 base, 5-9 edge
class Pool {
#ifdef RR_SPARSE_RETIREMENT_TEST
    friend struct RetirementTest;
#endif
    struct Slot {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12Fence> fence;
        Pair privatePair, source, history, destination;
        ComPtr<ID3D12PipelineState> scale;
        ComPtr<ID3D12Resource> rates;
        ComPtr<ID3D12RootSignature> privateRoot;
        // privateRoot plus a t2 table for the SSS guide, for SSS-masked sources, by
        // variant bits (VariantIndex).
        std::array<ComPtr<ID3D12RootSignature>, Variants> privateMaskRoots;
        // Indexed by VariantIndex(rrlighting::Source, bits).
        std::array<ComPtr<ID3D12PipelineState>, rrlighting::Count * Variants> privateScale;
        unsigned privateLobe{}, privateBits{};
        ComPtr<ID3D12DescriptorHeap> privateHeap;
        // SSS guide an SSS-masked preparation reads, retained like the inputs.
        ComPtr<ID3D12Resource> mask;
        // The edge fill's surface inputs, retained like the mask.
        EdgeInputs edge;
        ID3D12GraphicsCommandList *command{};
        UINT64 serial{}, signaled{}, bytes{};
        bool live{}, consumed{}, pending{}, poisoned{};
    };
    std::mutex mutex;
    std::array<Slot, 8> slots;
    std::unordered_map<unsigned, std::vector<unsigned>> calls;
    unsigned next{};
    UINT64 allocated{};
    bool Complete(const Slot &s) const {
        if (s.poisoned || s.pending)
            return false;
        if (!s.serial)
            return true;
        auto v = s.fence->GetCompletedValue();
        return v != UINT64_MAX && v >= s.signaled && s.signaled == s.serial;
    }
    void RetireCompleted() {
        for (auto &s : slots)
            if (!s.live && Complete(s)) {
                s.source = {};
                s.history = {};
                s.destination = {};
                s.scale.Reset();
                s.rates.Reset();
                s.mask.Reset();
                s.edge = {};
                s.command = nullptr;
            }
    }
    static void Copy(ID3D12GraphicsCommandList *cl, ID3D12Resource *to, D3D12_RESOURCE_STATES ts,
                     ID3D12Resource *from, D3D12_RESOURCE_STATES fs) {
        std::array<D3D12_RESOURCE_BARRIER, 2> before = {
            Transition(from, fs, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(to, ts, D3D12_RESOURCE_STATE_COPY_DEST)};
        cl->ResourceBarrier(2, before.data());
        cl->CopyResource(to, from);
        std::array<D3D12_RESOURCE_BARRIER, 2> after = {
            Transition(to, D3D12_RESOURCE_STATE_COPY_DEST, ts),
            Transition(from, D3D12_RESOURCE_STATE_COPY_SOURCE, fs)};
        cl->ResourceBarrier(2, after.data());
    }
    // The edge fill's private reads: the raw snapshot (COMMON after its copy into the
    // output) and the converted normals and Status (COMMON after the conversion). The
    // PackedSurface keeps the game's readable state.
    static std::array<D3D12_RESOURCE_BARRIER, 4> EdgeBarriers(const Slot &s, bool read) {
        constexpr auto common = D3D12_RESOURCE_STATE_COMMON,
                       srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        ID3D12Resource *r[] = {s.privatePair.r[0].Get(), s.privatePair.r[1].Get(),
                               s.edge.normal.Get(), s.edge.status.Get()};
        std::array<D3D12_RESOURCE_BARRIER, 4> b;
        for (unsigned i = 0; i < 4; ++i)
            b[i] = read ? Transition(r[i], common, srv) : Transition(r[i], srv, common);
        return b;
    }
    static D3D12_SHADER_BYTECODE PrivateScaleCode(bool packed_float, unsigned lobe,
                                                  unsigned bits = 0) {
        if (lobe == rrlighting::SssHistory2 && (bits & EdgeBit))
            return packed_float ? D3D12_SHADER_BYTECODE{RRVrrtScaleSssHistory2EdgeFloat,
                                                        sizeof(RRVrrtScaleSssHistory2EdgeFloat)}
                                : D3D12_SHADER_BYTECODE{RRVrrtScaleSssHistory2Edge,
                                                        sizeof(RRVrrtScaleSssHistory2Edge)};
        if (lobe == rrlighting::SssHistory2)
            return packed_float ? D3D12_SHADER_BYTECODE{RRVrrtScaleSssHistory2Float,
                                                        sizeof(RRVrrtScaleSssHistory2Float)}
                                : D3D12_SHADER_BYTECODE{RRVrrtScaleSssHistory2,
                                                        sizeof(RRVrrtScaleSssHistory2)};
        return packed_float ? D3D12_SHADER_BYTECODE{RRVrrtScaleFloat, sizeof(RRVrrtScaleFloat)}
                            : D3D12_SHADER_BYTECODE{RRVrrtScale, sizeof(RRVrrtScale)};
    }
    // mask: a proven SSS guide for an SSS-masked lobe; without one it prepares raw.
    // edge: proven surface inputs for the edge-aware fill of an SSS-masked lobe; without
    // them, or if that variant cannot be created, it uses the baseline group mean.
    static bool PreparePrivateScale(Slot &s, const Pair &input, const Pair &output,
                                    unsigned lobe = 0, ID3D12Resource *mask = nullptr,
                                    const EdgeInputs *edge = nullptr) {
        if (lobe >= rrlighting::Count || (rrlighting::SssMasked(lobe) && !mask))
            lobe = 0;
        unsigned bits = edge && rrlighting::SssMasked(lobe) ? EdgeBit : 0;
        // The mask root is the same layout plus parameter 4, a t2 table, then the edge
        // table (parameter 5, t3..t7) for the edge variant.
        auto makeRoot = [&](bool masked, ComPtr<ID3D12RootSignature> &result,
                            unsigned variant = 0) {
            if (result)
                return true;
            D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
                                               {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 0},
                                               {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 0, 0},
                                               {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 3, 0, 0}};
            D3D12_ROOT_PARAMETER p[6]{};
            for (unsigned i = 0; i < 2; ++i) {
                p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                p[i].DescriptorTable = {1, &ranges[i]};
                p[i + 2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                p[i + 2].Descriptor.ShaderRegister = i ? 3 : 1;
            }
            p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p[4].DescriptorTable = {1, &ranges[2]};
            p[EdgeTableParameter].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p[EdgeTableParameter].DescriptorTable = {1, &ranges[3]};
            D3D12_ROOT_SIGNATURE_DESC rd{!masked ? 4u : (variant & EdgeBit) ? 6u : 5u, p};
            ComPtr<ID3DBlob> code, error;
            ComPtr<ID3D12RootSignature> root;
            if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &code,
                                                   &error)) ||
                FAILED(s.device->CreateRootSignature(0, code->GetBufferPointer(),
                                                     code->GetBufferSize(), IID_PPV_ARGS(&root))))
                return false;
            result = root;
            return true;
        };
        if (!makeRoot(false, s.privateRoot))
            return false;
        if (bits && !makeRoot(true, s.privateMaskRoots[bits], bits))
            bits = 0;
        if (rrlighting::SssMasked(lobe) && !makeRoot(true, s.privateMaskRoots[0]))
            lobe = 0;
        const bool packed_float = output.r[0]->GetDesc().Format == DXGI_FORMAT_R32G32B32A32_FLOAT;
        auto create = [&](unsigned variant, unsigned variant_bits) {
            auto &pipeline = s.privateScale[VariantIndex(variant, variant_bits)];
            if (pipeline)
                return true;
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = rrlighting::SssMasked(variant)
                                    ? s.privateMaskRoots[variant_bits].Get()
                                    : s.privateRoot.Get();
            pd.CS = PrivateScaleCode(packed_float, variant, variant_bits);
            return SUCCEEDED(s.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pipeline)));
        };
        // An edge variant that cannot be created falls back to the baseline fill, and a
        // lobe variant that cannot be created to raw preparation.
        if (!rrlighting::SssMasked(lobe) || (bits && !create(lobe, bits)))
            bits = 0;
        if (!create(lobe, bits))
            lobe = bits = 0;
        if (!create(lobe, bits))
            return false;
        s.privateLobe = lobe;
        s.privateBits = bits;
        if (!s.privateHeap) {
            D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, PrivateDescriptors,
                                          D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
            if (FAILED(s.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.privateHeap))))
                return false;
        }
        // A slot is selected only after both recording retirement and GPU completion.
        // Its descriptors stay immutable from Apply through every submission.
        auto cpu = s.privateHeap->GetCPUDescriptorHandleForHeapStart();
        auto stride =
            s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (unsigned i = 0; i < 2; ++i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = input.r[i]->GetDesc().Format;
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Texture2D.MipLevels = 1;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            s.device->CreateShaderResourceView(input.r[i].Get(), &v, {cpu.ptr + i * stride});
            D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
            u.Format = output.r[i]->GetDesc().Format;
            u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            s.device->CreateUnorderedAccessView(output.r[i].Get(), nullptr, &u,
                                                {cpu.ptr + (2 + i) * stride});
        }
        if (rrlighting::SssMasked(lobe)) {
            D3D12_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = DXGI_FORMAT_R16_FLOAT;
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Texture2D.MipLevels = 1;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            s.device->CreateShaderResourceView(mask, &v, {cpu.ptr + 4 * stride});
        }
        if (bits & EdgeBit) {
            // t3/t4 the raw snapshot the output is copied from, t5-t7 the surface inputs.
            ID3D12Resource *views[] = {s.privatePair.r[0].Get(), s.privatePair.r[1].Get(),
                                       edge->surface.Get(), edge->normal.Get(),
                                       edge->status.Get()};
            for (unsigned i = 0; i < 5; ++i) {
                D3D12_SHADER_RESOURCE_VIEW_DESC v{};
                v.Format = views[i]->GetDesc().Format;
                v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                v.Texture2D.MipLevels = 1;
                v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                s.device->CreateShaderResourceView(views[i], &v, {cpu.ptr + (5 + i) * stride});
            }
        }
        return true;
    }

  public:
    void (*failure)(const char *){};
    void (*logger)(const char *){};
    uint64_t abandoned{}; // snapshots no first spatial used, logged at 1-3 and powers of two
    void Fail(const char *why) {
        if (failure)
            failure(why);
    }
    bool Snapshot(ID3D12GraphicsCommandList *cl, const Pair &source, const Pair &history) {
        std::lock_guard lock(mutex);
        if (!cl || cl->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || !Shape(source) ||
            !Shape(history))
            return false;
        ComPtr<ID3D12Device> d;
        if (FAILED(cl->GetDevice(IID_PPV_ARGS(&d))))
            return false;
        d = fg::NativeObject<ID3D12Device>(d.Get());
        if (!d)
            return false;
        for (unsigned i = 0; i < 2; ++i) {
            if (!SameDevice(d.Get(), source.r[i].Get()) ||
                !SameDevice(d.Get(), history.r[i].Get()) ||
                !SameShape(source.r[i].Get(), history.r[i].Get()) || source.r[i] == history.r[i] ||
                !Readable(source.state[i]))
                return false;
        }
        for (auto &s : slots)
            if (s.live && s.command == cl && !s.consumed)
                return false;
        Slot *slot = nullptr;
        for (auto &s : slots)
            if (!s.live && Complete(s)) {
                if (s.device == d && s.privatePair.r[0] &&
                    SameShape(s.privatePair.r[0].Get(), source.r[0].Get()) &&
                    SameShape(s.privatePair.r[1].Get(), source.r[1].Get())) {
                    slot = &s;
                    break;
                }
                if (!slot)
                    slot = &s;
            }
        if (!slot)
            return false;
        auto &s = *slot;
        if (!s.privatePair.r[0] || s.device != d ||
            !SameShape(s.privatePair.r[0].Get(), source.r[0].Get()) ||
            !SameShape(s.privatePair.r[1].Get(), source.r[1].Get())) {
            Pair replacement;
            UINT64 bytes{};
            std::array<D3D12_RESOURCE_DESC, 2> descriptions;
            for (unsigned i = 0; i < 2; ++i) {
                auto &desc = descriptions[i];
                desc = source.r[i]->GetDesc();
                desc.Flags = D3D12_RESOURCE_FLAG_NONE;
                desc.Alignment = 0;
                auto a = d->GetResourceAllocationInfo(0, 1, &desc);
                if (!a.SizeInBytes || a.SizeInBytes == UINT64_MAX ||
                    a.SizeInBytes > UINT64_MAX - bytes)
                    return false;
                bytes += a.SizeInBytes;
            }
            // Query only for a new/replacement allocation, never matching-slot reuse.
            // The local usage already includes game resources and other private pools.
            ComPtr<IDXGIFactory4> factory;
            ComPtr<IDXGIAdapter3> adapter;
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            UINT64 replacement_total{};
            const auto target_luid = d->GetAdapterLuid(),
                       previous_luid = s.device ? s.device->GetAdapterLuid() : LUID{};
            const bool same_adapter = s.device && target_luid.LowPart == previous_luid.LowPart &&
                                      target_luid.HighPart == previous_luid.HighPart;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
                FAILED(factory->EnumAdapterByLuid(d->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) ||
                FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) ||
                !rrsparsebudget::Replacement(allocated, s.bytes, bytes, same_adapter, info.Budget,
                                             info.CurrentUsage, replacement_total))
                return false;
            // This slot is reset/disposed AND complete. Release its previous private
            // textures before replacement, so transient allocation also stays bounded.
            allocated -= s.bytes;
            s = {};
            ComPtr<ID3D12Fence> fence;
            if (FAILED(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
                return false;
            for (unsigned i = 0; i < 2; ++i) {
                D3D12_HEAP_PROPERTIES hp{};
                hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                if (FAILED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &descriptions[i],
                                                      D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                      IID_PPV_ARGS(&replacement.r[i]))))
                    return false;
            }
            allocated = replacement_total;
            s.device = d;
            s.privatePair = std::move(replacement);
            s.bytes = bytes;
            s.fence = fence;
        }
        s.source = source;
        s.history = history;
        s.destination = {};
        s.scale.Reset();
        s.mask.Reset();
        s.edge = {};
        s.command = cl;
        s.live = true;
        s.consumed = false;
        for (unsigned i = 0; i < 2; ++i)
            Copy(cl, s.privatePair.r[i].Get(), D3D12_RESOURCE_STATE_COMMON, source.r[i].Get(),
                 source.state[i]);
        return true;
    }
    bool Pending(ID3D12GraphicsCommandList *cl, const Pair &input) {
        std::lock_guard lock(mutex);
        for (auto &s : slots)
            if (s.live && s.command == cl && !s.consumed)
                return s.history.r == input.r;
        return false;
    }
    // A spatial proof skipped after this list's snapshot: the snapshot is left unused, so Before()
    // fences it like an applied one. live stays set, so Complete() cannot reuse the slot before the
    // recorded copy has run.
    void Abandon(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(mutex);
        for (auto &s : slots)
            if (s.live && s.command == cl && !s.consumed)
                s.consumed = true;
    }
    bool Apply(ID3D12GraphicsCommandList *cl, const Pair &input, const Pair &output,
               ID3D12PipelineState *scale, ID3D12PipelineState *restore, UINT x, UINT y, UINT z,
               const rrvrrt::Data *rates = nullptr, const rrvrrt::CBVs *cbvs = nullptr,
               const rrcompute::Snapshot *privateRestore = nullptr, unsigned lobe = 0,
               ID3D12Resource *mask = nullptr, bool *masked = nullptr,
               const EdgeInputs *edge = nullptr, bool *edged = nullptr) {
        std::lock_guard lock(mutex);
        if (masked)
            *masked = false;
        if (edged)
            *edged = false;
        if (!Shape(input) || !Shape(output) || !scale || !restore || z != 1)
            return false;
        Slot *slot = nullptr;
        for (auto &s : slots)
            if (s.live && s.command == cl && !s.consumed && s.history.r == input.r) {
                slot = &s;
                break;
            }
        if (!slot)
            return false;
        auto &s = *slot;
        const auto desc = output.r[0]->GetDesc();
        if (x != (desc.Width + 15) / 16 || y != (desc.Height + 15) / 16)
            return false;
        for (unsigned i = 0; i < 2; ++i) {
            if (!Readable(input.state[i]) ||
                output.state[i] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS ||
                !SameDevice(s.device.Get(), output.r[i].Get()) ||
                !SameShape(s.privatePair.r[i].Get(), output.r[i].Get()))
                return false;
            for (auto &h : s.history.r)
                if (output.r[i] == h)
                    return false;
            for (auto &r : s.source.r)
                if (output.r[i] == r)
                    return false;
        }
        if (privateRestore &&
            (!rates || cbvs || !privateRestore->valid || !privateRestore->complete ||
             !privateRestore->heap_restore_valid || privateRestore->command != cl ||
             privateRestore->pipeline.Get() != restore || !privateRestore->root))
            return false;
        if (privateRestore)
            for (unsigned i = 0; i < 2; ++i) {
                const bool packedFloat =
                    output.r[0]->GetDesc().Format == DXGI_FORMAT_R32G32B32A32_FLOAT;
                const auto format = i ? (packedFloat ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32_UINT)
                                      : (packedFloat ? DXGI_FORMAT_R32G32B32A32_FLOAT
                                                     : DXGI_FORMAT_R32G32B32A32_UINT);
                if (input.r[i]->GetDesc().Format != format ||
                    output.r[i]->GetDesc().Format != format)
                    return false;
            }
        if (rates) {
            if ((!privateRestore && (!cbvs || !cbvs->valid)) || !rates->buffer ||
                rates->w != desc.Width || rates->h != desc.Height ||
                !SameDevice(s.device.Get(), rates->buffer.Get()))
                return false;
            const auto buffer = rates->buffer->GetDesc();
            if (buffer.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
                !rrvrrt::Dimensions(rates->w, rates->h) ||
                buffer.Width < rrvrrt::Bytes(rates->w, rates->h) ||
                !rates->buffer->GetGPUVirtualAddress())
                return false;
        }
        // An SSS-masked lobe reads the guide through the private root only, at the
        // lighting extent; otherwise it prepares raw lighting.
        if (!privateRestore || !mask || !SameDevice(s.device.Get(), mask) ||
            !rrsssmask::Shape(mask->GetDesc(), desc))
            mask = nullptr;
        // The edge fill needs the mask path and every surface input at the lighting extent.
        if (!mask || !edge || !edge->surface || !edge->normal || !edge->status ||
            !Readable(edge->surface_state) || !SameDevice(s.device.Get(), edge->surface.Get()) ||
            !SameDevice(s.device.Get(), edge->normal.Get()) ||
            !SameDevice(s.device.Get(), edge->status.Get()) ||
            !EdgeShape(edge->surface->GetDesc(), edge->normal->GetDesc(),
                       edge->status->GetDesc(), desc))
            edge = nullptr;
        if (privateRestore && !PreparePrivateScale(s, input, output, lobe, mask, edge))
            return false;
        const bool useMask = privateRestore && rrlighting::SssMasked(s.privateLobe);
        const bool useEdge = useMask && (s.privateBits & EdgeBit);
        // All proofs and references acquired before the first output mutation.
        s.destination = output;
        s.scale = scale;
        s.rates = rates ? rates->buffer : nullptr;
        s.mask = useMask ? mask : nullptr;
        s.edge = useEdge ? *edge : EdgeInputs{};
        s.consumed = true;
        for (unsigned i = 0; i < 2; ++i)
            Copy(cl, output.r[i].Get(), output.state[i], s.privatePair.r[i].Get(),
                 D3D12_RESOURCE_STATE_COMMON);
        if (privateRestore) {
            auto gpu = s.privateHeap->GetGPUDescriptorHandleForHeapStart();
            auto stride =
                s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            ID3D12DescriptorHeap *heaps[] = {s.privateHeap.Get()};
            cl->SetDescriptorHeaps(1, heaps);
            cl->SetComputeRootSignature(useMask ? s.privateMaskRoots[s.privateBits].Get()
                                                : s.privateRoot.Get());
            cl->SetPipelineState(
                s.privateScale[VariantIndex(s.privateLobe, useMask ? s.privateBits : 0)].Get());
            cl->SetComputeRootDescriptorTable(0, gpu);
            cl->SetComputeRootDescriptorTable(1, {gpu.ptr + 2 * stride});
            if (useMask) {
                cl->SetComputeRootDescriptorTable(4, {gpu.ptr + 4 * stride});
                auto read = Transition(s.mask.Get(), D3D12_RESOURCE_STATE_COMMON,
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                cl->ResourceBarrier(1, &read);
            }
            if (useEdge) {
                cl->SetComputeRootDescriptorTable(EdgeTableParameter, {gpu.ptr + 5 * stride});
                cl->ResourceBarrier(4, EdgeBarriers(s, true).data());
            }
        } else
            cl->SetPipelineState(scale);
        if (rates) {
            auto before = Transition(s.rates.Get(), D3D12_RESOURCE_STATE_COMMON,
                                     D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            cl->ResourceBarrier(1, &before);
            const UINT strip_rows = rrvrrt::Rows(rates->w);
            for (UINT row = 0; row < rates->h; row += strip_rows) {
                const auto va =
                    s.rates->GetGPUVirtualAddress() + UINT64(row / strip_rows) * rrvrrt::ChunkBytes;
                cl->SetComputeRootConstantBufferView(privateRestore ? 2 : cbvs->parameters[0], va);
                cl->SetComputeRootConstantBufferView(privateRestore ? 3 : cbvs->parameters[1],
                                                     va + rrvrrt::WindowBytes);
                cl->Dispatch(x, (strip_rows + 15) / 16, 1);
            }
            if (!privateRestore)
                for (unsigned i = 0; i < 2; ++i)
                    cl->SetComputeRootConstantBufferView(cbvs->parameters[i], cbvs->addresses[i]);
            std::swap(before.Transition.StateBefore, before.Transition.StateAfter);
            cl->ResourceBarrier(1, &before);
        } else
            cl->Dispatch(x, y, z);
        if (useMask) {
            auto done = Transition(s.mask.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                   D3D12_RESOURCE_STATE_COMMON);
            cl->ResourceBarrier(1, &done);
        }
        if (useEdge)
            cl->ResourceBarrier(4, EdgeBarriers(s, false).data());
        if (masked)
            *masked = useMask;
        if (edged)
            *edged = useEdge;
        if (privateRestore)
            privateRestore->Restore(cl, true);
        else
            cl->SetPipelineState(restore);
        return true;
    }
    // Retirement requires BOTH recording retirement and observed GPU completion.
    // Keep owned allocations cached; release only the completed native inputs.
    void Poll() {
        std::lock_guard lock(mutex);
        RetireCompleted();
    }
    void Reset(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(mutex);
        for (auto &s : slots)
            if (s.command == cl)
                s.live = false;
        RetireCompleted();
    }
    unsigned Before(ID3D12CommandQueue *q, UINT count, ID3D12CommandList *const *lists) {
        std::lock_guard lock(mutex);
        if (!q || count > 1024 || (count && !lists)) {
            Fail("RR invalid native submission");
            return 0;
        }
        std::vector<unsigned> matched;
        for (unsigned k = 0; k < slots.size(); ++k) {
            auto &s = slots[k];
            if (!s.live)
                continue;
            unsigned occurrences = 0;
            for (unsigned i = 0; i < count; ++i)
                occurrences += lists[i] == s.command;
            if (!occurrences)
                continue;
            // A snapshot no first spatial used (its proof was skipped, or RR or its checks were
            // off by then) is abandoned here, and fenced like an applied one.
            if (!s.consumed) {
                s.consumed = true;
                const auto n = ++abandoned;
                if (logger && (n <= 3 || !(n & (n - 1)))) {
                    char line[160];
                    snprintf(line, sizeof(line),
                             "RR lighting snapshot submitted without its first spatial pass; "
                             "frame skipped (%llu so far)",
                             (unsigned long long)n);
                    logger(line);
                }
            }
            ComPtr<ID3D12Device> d;
            const bool valid = occurrences == 1 && !s.pending && !s.poisoned &&
                               q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT &&
                               SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&d))) &&
                               fg::SameNativeObject(d.Get(), s.device.Get()) &&
                               (!s.queue || s.queue.Get() == q);
            if (!valid) {
                s.poisoned = true;
                Fail("RR unmatched first spatial, ambiguous queue/replay; resources retained and "
                     "RR blocked");
                continue;
            }
            matched.push_back(k);
        }
        if (matched.empty())
            return 0;
        unsigned tag{};
        for (unsigned i = 0; i < 65535; ++i) {
            next = next % 65535 + 1;
            if (!calls.contains(next)) {
                tag = next;
                break;
            }
        }
        if (!tag) {
            for (auto k : matched)
                slots[k].poisoned = true;
            Fail("RR submission token capacity");
            return 0;
        }
        for (auto k : matched) {
            auto &s = slots[k];
            s.pending = true;
            s.queue = q;
            ++s.serial;
        }
        calls.emplace(tag, std::move(matched));
        return tag;
    }
    void After(ID3D12CommandQueue *q, unsigned tag) {
        std::lock_guard lock(mutex);
        auto at = calls.find(tag);
        if (!tag || at == calls.end())
            return;
        for (auto k : at->second) {
            auto &s = slots[k];
            if (s.queue.Get() != q || FAILED(q->Signal(s.fence.Get(), s.serial))) {
                s.poisoned = true;
                Fail("RR native GPU fence failed; resources retained and RR blocked");
            } else
                s.signaled = s.serial;
            s.pending = false;
        }
        calls.erase(at);
    }
    unsigned Live() {
        std::lock_guard lock(mutex);
        unsigned n = 0;
        for (auto &s : slots)
            n += s.live;
        return n;
    }
    UINT64 Bytes() {
        std::lock_guard lock(mutex);
        return allocated;
    }
};
} // namespace rrsparsecopy
