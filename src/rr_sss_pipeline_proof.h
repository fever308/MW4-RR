#pragma once
#include "feed_fg_identity.h"
#include <d3d12.h>
#include <MinHook.h>
#include "rr_startup_timing.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <type_traits>

namespace rrsss {
struct NativeDepthStencilState {
    D3D12_DEPTH_STENCIL_DESC2 desc{};
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool state_known{}, format_known{};
};
inline D3D12_DEPTH_STENCILOP_DESC1 NormalizeStencilFace(const D3D12_DEPTH_STENCILOP_DESC &face,
                                                        UINT8 read, UINT8 write) noexcept {
    return {face.StencilFailOp,
            face.StencilDepthFailOp,
            face.StencilPassOp,
            face.StencilFunc,
            read,
            write};
}
inline D3D12_DEPTH_STENCIL_DESC2 NormalizeDepthStencil(const D3D12_DEPTH_STENCIL_DESC &d) noexcept {
    D3D12_DEPTH_STENCIL_DESC2 out{};
    out.DepthEnable = d.DepthEnable;
    out.DepthWriteMask = d.DepthWriteMask;
    out.DepthFunc = d.DepthFunc;
    out.StencilEnable = d.StencilEnable;
    out.FrontFace = NormalizeStencilFace(d.FrontFace, d.StencilReadMask, d.StencilWriteMask);
    out.BackFace = NormalizeStencilFace(d.BackFace, d.StencilReadMask, d.StencilWriteMask);
    return out;
}
inline D3D12_DEPTH_STENCIL_DESC2
NormalizeDepthStencil(const D3D12_DEPTH_STENCIL_DESC1 &d) noexcept {
    D3D12_DEPTH_STENCIL_DESC2 out{};
    out.DepthEnable = d.DepthEnable;
    out.DepthWriteMask = d.DepthWriteMask;
    out.DepthFunc = d.DepthFunc;
    out.StencilEnable = d.StencilEnable;
    out.FrontFace = NormalizeStencilFace(d.FrontFace, d.StencilReadMask, d.StencilWriteMask);
    out.BackFace = NormalizeStencilFace(d.BackFace, d.StencilReadMask, d.StencilWriteMask);
    out.DepthBoundsTestEnable = d.DepthBoundsTestEnable;
    return out;
}
inline D3D12_DEPTH_STENCIL_DESC2
NormalizeDepthStencil(const D3D12_DEPTH_STENCIL_DESC2 &d) noexcept {
    return d;
}
inline bool DepthStencilEquivalent(const NativeDepthStencilState &a,
                                   const NativeDepthStencilState &b) noexcept {
    if (!a.state_known || !b.state_known || !a.format_known || !b.format_known ||
        a.format != b.format)
        return false;
    const auto &x = a.desc;
    const auto &y = b.desc;
    if (x.DepthEnable != y.DepthEnable || x.DepthWriteMask != y.DepthWriteMask ||
        x.DepthFunc != y.DepthFunc || x.StencilEnable != y.StencilEnable ||
        x.DepthBoundsTestEnable || y.DepthBoundsTestEnable)
        return false;
    auto face = [](const D3D12_DEPTH_STENCILOP_DESC1 &x, const D3D12_DEPTH_STENCILOP_DESC1 &y) {
        return x.StencilFailOp == y.StencilFailOp && x.StencilDepthFailOp == y.StencilDepthFailOp &&
               x.StencilPassOp == y.StencilPassOp && x.StencilFunc == y.StencilFunc &&
               x.StencilReadMask == y.StencilReadMask && x.StencilWriteMask == y.StencilWriteMask;
    };
    return face(x.FrontFace, y.FrontFace) && face(x.BackFace, y.BackFace);
}

// Observation is independent of admission. Creation-time shader/input payloads
// are bounded and hashed synchronously; no borrowed pointer is retained.
struct NativePipelineObservation {
    struct Shader {
        SIZE_T bytes{};
        bool code{};
        uint64_t hash{};
    };
    std::array<Shader, 8> shaders{}; // VS PS DS HS GS CS AS MS
    uint64_t root_identity{}, input_hash{};
    bool input_valid{};
    uint64_t seen{}, duplicates{};
    SIZE_T bytes{}, problem_offset{};
    UINT problem_type = ~0u;
    bool description_known{}, stream{}, complete{}, malformed{}, unknown{}, shader_malformed{};
    bool sample_known{}, raster_known{}, blend_known{}, root_present{};
    DXGI_SAMPLE_DESC sample{};
    UINT sample_mask{}, node_mask{}, flags{}, topology{}, strip_cut{};
    UINT so_entries{}, so_strides{}, so_raster{}, input_elements{}, view_count{}, view_flags{},
        rt_count{};
    bool so_entries_present{}, so_strides_present{}, input_present{}, view_locations_present{},
        cached_present{};
    SIZE_T cached_bytes{};
    NativeDepthStencilState depth;
    D3D12_RASTERIZER_DESC2 raster{};
    UINT raster_version{};
    INT legacy_depth_bias{};
    BOOL multisample{}, antialiased_line{};
    D3D12_BLEND_DESC blend{};
    std::array<DXGI_FORMAT, 8> rt_formats{};
    static unsigned Family(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE kind) noexcept {
        if (kind == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1 ||
            kind == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2)
            return D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL;
        if (kind == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1 ||
            kind == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2)
            return D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER;
        return unsigned(kind);
    }
    bool Known(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE kind) const noexcept {
        const auto f = Family(kind);
        return f < 64 && (seen & (1ull << f)) != 0;
    }
};

#pragma warning(push)
#pragma warning(                                                                                   \
    disable : 4324) // Native stream elements deliberately include pointer-alignment padding.
// A prerequisite only: other PSO/OM/shader compatibility remains the caller's
// responsibility. Proof belongs to the COM object, never a recycled address.
class PipelineProof {
  public:
    enum class Status : UINT {
        Unobserved,
        Proven,
        Description,
        Shader,
        StreamOutput,
        SampleQuality,
        NodeMask,
        ViewInstancing,
        ComputeStage,
        AmplificationStage,
        MeshStage,
        MalformedStage,
        DepthBounds
    };

  private:
    template <class T> struct alignas(void *) Part {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
        T value;
    };
    enum Method { Graphics, Stream, Library, LoadGraphics, LoadStream, Count };
    struct HookSlot {
        void *target{};
        std::atomic<void *> original{};
    };
    static inline std::array<std::array<HookSlot, 8>, Count> hooks{};
    static inline std::mutex install_mutex;
    static inline unsigned char cookie;
    static inline constexpr GUID proof_id = {
        0x9938bb60, 0x1179, 0x4368, {0xb7, 0x57, 0x84, 0x03, 0xa9, 0x88, 0x1d, 0x66}};
    struct Marker {
        uintptr_t owner;
        UINT version;
        UINT accepted;
        Status status;
        NativeDepthStencilState depth;
        NativePipelineObservation observation;
    };
    struct Error {
        DWORD value = GetLastError();
        ~Error() {
            SetLastError(value);
        }
    };
    template <class T>
    static bool Take(const unsigned char *data, size_t remaining, T &value, size_t &size) noexcept {
        size = sizeof(Part<T>);
        if (remaining < size)
            return false;
        memcpy(&value, data + offsetof(Part<T>, value), sizeof(value));
        return true;
    }
    static void Stamp(HRESULT result, void **object, bool proven, Status status,
                      const NativeDepthStencilState &depth,
                      const NativePipelineObservation &observation) noexcept {
        if (FAILED(result) || !object || !*object)
            return;
        Error error;
        try {
            auto p = fg::NativeObject<ID3D12PipelineState>(static_cast<IUnknown *>(*object));
            if (!p)
                return;
            const Marker marker{reinterpret_cast<uintptr_t>(&cookie),
                                5,
                                proven ? 1u : 0u,
                                status,
                                depth,
                                observation};
            // Failure leaves no new proof. Clear an earlier marker if an interned object
            // was returned by a path we cannot certify.
            if (FAILED(p->SetPrivateData(proof_id, sizeof(marker), &marker)))
                p->SetPrivateData(proof_id, 0, nullptr);
        } catch (...) {
        }
    }
    // Times the driver's own creation apart from this hook's inspection, which
    // began at inspect. Counters only: nothing here logs or waits.
    template <class F> static HRESULT Native(rrtiming::Stage stage, uint64_t inspect, F &&f) {
        auto &t = rrtiming::Global();
        const auto begin = rrtiming::Now();
        t.Add(rrtiming::Stage::ProofHook, begin - inspect);
        t.NativeBegin();
        const auto hr = f();
        t.NativeEnd();
        t.Add(stage, rrtiming::Now() - begin);
        return hr;
    }
    template <Method M, size_t N, class O, class... A> static HRESULT Forward(O *object, A... a) {
        using Fn = HRESULT(STDMETHODCALLTYPE *)(O *, A...);
        return reinterpret_cast<Fn>(hooks[M][N].original.load(std::memory_order_acquire))(object,
                                                                                          a...);
    }
    template <size_t N>
    static HRESULT STDMETHODCALLTYPE OnGraphics(ID3D12Device *d,
                                                const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                                                REFIID iid, void **out) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC local{};
        if (desc && ApplyQuality(d, *desc, local))
            desc = &local;
        Status status;
        NativeDepthStencilState depth;
        const auto inspect = rrtiming::Now();
        const bool proof = LegacyProven(desc, &status, &depth);
        const auto observation = ObserveLegacy(desc);
        const auto hr = Native(rrtiming::Stage::NativeCreate, inspect, [&] {
            return Forward<Graphics, N, ID3D12Device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *,
                           REFIID, void **>(d, desc, iid, out);
        });
        Stamp(hr, out, proof, status, depth, observation);
        return hr;
    }
    template <size_t N>
    static HRESULT STDMETHODCALLTYPE OnStream(ID3D12Device2 *d,
                                              const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
                                              REFIID iid, void **out) {
        Status status;
        NativeDepthStencilState depth;
        const auto inspect = rrtiming::Now();
        const bool proof = StreamProven(desc, &status, &depth);
        const auto observation = ObserveStream(desc);
        const auto hr = Native(rrtiming::Stage::NativeCreate, inspect, [&] {
            return Forward<Stream, N, ID3D12Device2, const D3D12_PIPELINE_STATE_STREAM_DESC *,
                           REFIID, void **>(d, desc, iid, out);
        });
        Stamp(hr, out, proof, status, depth, observation);
        return hr;
    }
    template <size_t N>
    static HRESULT STDMETHODCALLTYPE OnLibrary(ID3D12Device1 *d, const void *blob, SIZE_T size,
                                               REFIID iid, void **out) {
        const auto hr = Forward<Library, N, ID3D12Device1, const void *, SIZE_T, REFIID, void **>(
            d, blob, size, iid, out);
        if (SUCCEEDED(hr) && out && *out) {
            Error error;
            try {
                AttachLibrary(static_cast<IUnknown *>(*out));
            } catch (...) {
            }
        }
        return hr;
    }
    template <size_t N>
    static HRESULT STDMETHODCALLTYPE OnLoadGraphics(ID3D12PipelineLibrary *library, LPCWSTR name,
                                                    const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                                                    REFIID iid, void **out) {
        Status status;
        NativeDepthStencilState depth;
        const auto inspect = rrtiming::Now();
        const bool proof = LegacyProven(desc, &status, &depth);
        const auto observation = ObserveLegacy(desc);
        const auto hr = Native(rrtiming::Stage::LibraryLoad, inspect, [&] {
            return Forward<LoadGraphics, N, ID3D12PipelineLibrary, LPCWSTR,
                           const D3D12_GRAPHICS_PIPELINE_STATE_DESC *, REFIID, void **>(
                library, name, desc, iid, out);
        });
        Stamp(hr, out, proof, status, depth, observation);
        return hr;
    }
    template <size_t N>
    static HRESULT STDMETHODCALLTYPE OnLoadStream(ID3D12PipelineLibrary1 *library, LPCWSTR name,
                                                  const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
                                                  REFIID iid, void **out) {
        Status status;
        NativeDepthStencilState depth;
        const auto inspect = rrtiming::Now();
        const bool proof = StreamProven(desc, &status, &depth);
        const auto observation = ObserveStream(desc);
        const auto hr = Native(rrtiming::Stage::LibraryLoad, inspect, [&] {
            return Forward<LoadStream, N, ID3D12PipelineLibrary1, LPCWSTR,
                           const D3D12_PIPELINE_STATE_STREAM_DESC *, REFIID, void **>(
                library, name, desc, iid, out);
        });
        Stamp(hr, out, proof, status, depth, observation);
        return hr;
    }
    template <Method M, size_t... N> static auto Addresses(std::index_sequence<N...>) {
        if constexpr (M == Graphics)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnGraphics<N>)...};
        if constexpr (M == Stream)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnStream<N>)...};
        if constexpr (M == Library)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnLibrary<N>)...};
        if constexpr (M == LoadGraphics)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnLoadGraphics<N>)...};
        if constexpr (M == LoadStream)
            return std::array<void *, 8>{reinterpret_cast<void *>(&OnLoadStream<N>)...};
    }
    template <Method M> static bool Hook(void *target) {
        if (!target)
            return false;
        auto &family = hooks[M];
        for (auto &s : family)
            if (s.target == target)
                return s.original.load() != nullptr;
        for (size_t k = 0; k < family.size(); ++k)
            if (!family[k].target) {
                void *original{};
                if (MH_CreateHook(target, Addresses<M>(std::make_index_sequence<8>{})[k],
                                  &original) != MH_OK)
                    return false;
                family[k].target = target;
                family[k].original.store(original, std::memory_order_release);
                if (MH_EnableHook(target) != MH_OK) {
                    MH_RemoveHook(target);
                    family[k].original = nullptr;
                    return false;
                }
                return true;
            }
        return false;
    }
    static void AttachLibrary(IUnknown *object) {
        auto library = fg::NativeObject<ID3D12PipelineLibrary>(object);
        if (!library)
            return;
        std::lock_guard lock(install_mutex);
        Hook<LoadGraphics>((*reinterpret_cast<void ***>(library.Get()))[9]);
        Microsoft::WRL::ComPtr<ID3D12PipelineLibrary1> next;
        if (SUCCEEDED(library.As(&next)))
            Hook<LoadStream>((*reinterpret_cast<void ***>(next.Get()))[13]);
    }

  public:
    template <class T>
    static void ObserveRaster(NativePipelineObservation &o, const T &d) noexcept {
        o.raster_known = true;
        auto &r = o.raster;
        r.FillMode = d.FillMode;
        r.CullMode = d.CullMode;
        r.FrontCounterClockwise = d.FrontCounterClockwise;
        r.DepthBias = FLOAT(d.DepthBias);
        r.DepthBiasClamp = d.DepthBiasClamp;
        r.SlopeScaledDepthBias = d.SlopeScaledDepthBias;
        r.DepthClipEnable = d.DepthClipEnable;
        r.ForcedSampleCount = d.ForcedSampleCount;
        r.ConservativeRaster = d.ConservativeRaster;
        if constexpr (std::is_same_v<T, D3D12_RASTERIZER_DESC2>) {
            o.raster_version = 2;
            r.LineRasterizationMode = d.LineRasterizationMode;
        } else {
            o.raster_version = std::is_same_v<T, D3D12_RASTERIZER_DESC1> ? 1u : 0u;
            o.multisample = d.MultisampleEnable;
            o.antialiased_line = d.AntialiasedLineEnable;
            if constexpr (std::is_same_v<T, D3D12_RASTERIZER_DESC>)
                o.legacy_depth_bias = d.DepthBias;
        }
    }
    static uint64_t ByteHash(const void *data, size_t bytes) noexcept {
        uint64_t h = 14695981039346656037ull;
        const auto *p = static_cast<const unsigned char *>(data);
        for (size_t i = 0; i < bytes; ++i)
            h = (h ^ p[i]) * 1099511628211ull;
        return h;
    }
    static void ObserveInput(NativePipelineObservation &o,
                             const D3D12_INPUT_LAYOUT_DESC &d) noexcept {
        o.input_elements = d.NumElements;
        o.input_present = d.pInputElementDescs != nullptr;
        o.input_valid = d.NumElements <= 32 && (!d.NumElements || d.pInputElementDescs);
        o.input_hash = 14695981039346656037ull;
        if (!o.input_valid)
            return;
        auto mix = [&](const void *p, size_t n) {
            const auto *b = static_cast<const unsigned char *>(p);
            for (size_t i = 0; i < n; ++i)
                o.input_hash = (o.input_hash ^ b[i]) * 1099511628211ull;
        };
        for (UINT i = 0; i < d.NumElements; ++i) {
            const auto &e = d.pInputElementDescs[i];
            if (!e.SemanticName) {
                o.input_valid = false;
                return;
            }
            size_t n = 0;
            while (n < 64 && e.SemanticName[n])
                ++n;
            if (n == 64) {
                o.input_valid = false;
                return;
            }
            mix(e.SemanticName, n + 1);
            const UINT fields[] = {
                e.SemanticIndex,     UINT(e.Format),         e.InputSlot,
                e.AlignedByteOffset, UINT(e.InputSlotClass), e.InstanceDataStepRate};
            mix(fields, sizeof(fields));
        }
    }
    static bool SupportedSamples(DXGI_SAMPLE_DESC d) noexcept {
        return d.Quality == 0 || (d.Count == 4 && d.Quality == D3D12_STANDARD_MULTISAMPLE_PATTERN);
    }
    static void ObserveShader(NativePipelineObservation &o, unsigned stage,
                              const D3D12_SHADER_BYTECODE &s) noexcept {
        o.shaders[stage] = {s.BytecodeLength, s.pShaderBytecode != nullptr, 0};
        if (s.pShaderBytecode && s.BytecodeLength && s.BytecodeLength <= 4 * 1024 * 1024)
            o.shaders[stage].hash = ByteHash(s.pShaderBytecode, s.BytecodeLength);
        if (bool(s.pShaderBytecode) != bool(s.BytecodeLength))
            o.shader_malformed = o.malformed = true;
    }
    static NativePipelineObservation
    ObserveLegacy(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *d) noexcept {
        NativePipelineObservation o;
        if (!d) {
            o.malformed = true;
            return o;
        }
        o.description_known = o.complete = true;
        o.bytes = sizeof(*d);
        const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE known[] = {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS};
        for (auto k : known)
            o.seen |= 1ull << NativePipelineObservation::Family(k);
        o.root_present = d->pRootSignature != nullptr;
        o.root_identity = reinterpret_cast<uintptr_t>(d->pRootSignature);
        ObserveShader(o, 0, d->VS);
        ObserveShader(o, 1, d->PS);
        ObserveShader(o, 2, d->DS);
        ObserveShader(o, 3, d->HS);
        ObserveShader(o, 4, d->GS);
        o.sample_known = o.blend_known = true;
        o.sample = d->SampleDesc;
        o.sample_mask = d->SampleMask;
        o.blend = d->BlendState;
        ObserveRaster(o, d->RasterizerState);
        o.depth = {NormalizeDepthStencil(d->DepthStencilState), d->DSVFormat, true, true};
        o.so_entries = d->StreamOutput.NumEntries;
        o.so_strides = d->StreamOutput.NumStrides;
        o.so_raster = d->StreamOutput.RasterizedStream;
        o.so_entries_present = d->StreamOutput.pSODeclaration != nullptr;
        o.so_strides_present = d->StreamOutput.pBufferStrides != nullptr;
        ObserveInput(o, d->InputLayout);
        o.topology = unsigned(d->PrimitiveTopologyType);
        o.strip_cut = unsigned(d->IBStripCutValue);
        o.rt_count = d->NumRenderTargets;
        for (UINT i = 0; i < 8; ++i)
            o.rt_formats[i] = d->RTVFormats[i];
        o.node_mask = d->NodeMask;
        o.flags = unsigned(d->Flags);
        o.cached_present = d->CachedPSO.pCachedBlob != nullptr;
        o.cached_bytes = d->CachedPSO.CachedBlobSizeInBytes;
        return o;
    }
    static NativePipelineObservation
    ObserveStream(const D3D12_PIPELINE_STATE_STREAM_DESC *d) noexcept {
        NativePipelineObservation o;
        o.stream = true;
        if (!d || !d->pPipelineStateSubobjectStream || !d->SizeInBytes || d->SizeInBytes > 65536 ||
            reinterpret_cast<uintptr_t>(d->pPipelineStateSubobjectStream) % alignof(void *)) {
            o.malformed = true;
            return o;
        }
        o.description_known = true;
        o.bytes = d->SizeInBytes;
        const auto *data = static_cast<const unsigned char *>(d->pPipelineStateSubobjectStream);
        size_t left = d->SizeInBytes;
        auto problem = [&](UINT kind) {
            if (o.problem_type == ~0u) {
                o.problem_type = kind;
                o.problem_offset = d->SizeInBytes - left;
            }
        };
        while (left) {
            if (left < sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE)) {
                o.malformed = true;
                problem(~0u);
                return o;
            }
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE kind;
            memcpy(&kind, data, sizeof(kind));
            const auto family = NativePipelineObservation::Family(kind);
            size_t size = 0;
            const bool duplicate = family < 64 && (o.seen & (1ull << family));
            if (duplicate) {
                o.duplicates |= 1ull << family;
                o.malformed = true;
                problem(unsigned(kind));
            }
// Decode every structurally known element even if proof has already failed.
// Never guess unknown element sizes, and never dereference payload pointers.
#define RR_SSS_OBSERVE(K, T, BODY)                                                                 \
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_##K: {                                                \
        T v{};                                                                                     \
        if (!Take(data, left, v, size)) {                                                          \
            o.malformed = true;                                                                    \
            problem(unsigned(kind));                                                               \
            return o;                                                                              \
        }                                                                                          \
        if (!duplicate) {                                                                          \
            BODY;                                                                                  \
        }                                                                                          \
        break;                                                                                     \
    }
            switch (kind) {
                RR_SSS_OBSERVE(ROOT_SIGNATURE, ID3D12RootSignature *, o.root_present = v != nullptr;
                               o.root_identity = reinterpret_cast<uintptr_t>(v))
                RR_SSS_OBSERVE(VS, D3D12_SHADER_BYTECODE, ObserveShader(o, 0, v))
                RR_SSS_OBSERVE(PS, D3D12_SHADER_BYTECODE, ObserveShader(o, 1, v))
                RR_SSS_OBSERVE(DS, D3D12_SHADER_BYTECODE, ObserveShader(o, 2, v))
                RR_SSS_OBSERVE(HS, D3D12_SHADER_BYTECODE, ObserveShader(o, 3, v))
                RR_SSS_OBSERVE(GS, D3D12_SHADER_BYTECODE, ObserveShader(o, 4, v)) RR_SSS_OBSERVE(
                    CS, D3D12_SHADER_BYTECODE,
                    ObserveShader(
                        o, 5, v)) RR_SSS_OBSERVE(AS, D3D12_SHADER_BYTECODE,
                                                 ObserveShader(
                                                     o, 6,
                                                     v)) RR_SSS_OBSERVE(MS, D3D12_SHADER_BYTECODE,
                                                                        ObserveShader(o, 7, v))
                    RR_SSS_OBSERVE(STREAM_OUTPUT, D3D12_STREAM_OUTPUT_DESC,
                                   o.so_entries = v.NumEntries;
                                   o.so_strides = v.NumStrides; o.so_raster = v.RasterizedStream;
                                   o.so_entries_present = v.pSODeclaration != nullptr;
                                   o.so_strides_present =
                                       v.pBufferStrides !=
                                       nullptr) RR_SSS_OBSERVE(BLEND, D3D12_BLEND_DESC, o.blend = v;
                                                               o.blend_known = true)
                        RR_SSS_OBSERVE(SAMPLE_MASK, UINT, o.sample_mask = v) RR_SSS_OBSERVE(
                            RASTERIZER, D3D12_RASTERIZER_DESC,
                            ObserveRaster(o, v)) RR_SSS_OBSERVE(RASTERIZER1, D3D12_RASTERIZER_DESC1,
                                                                ObserveRaster(o, v))
                            RR_SSS_OBSERVE(
                                RASTERIZER2, D3D12_RASTERIZER_DESC2,
                                ObserveRaster(
                                    o, v)) RR_SSS_OBSERVE(DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC,
                                                          o.depth.desc = NormalizeDepthStencil(v);
                                                          o.depth.state_known = true)
                                RR_SSS_OBSERVE(DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1,
                                               o.depth.desc = NormalizeDepthStencil(v);
                                               o.depth.state_known =
                                                   true) RR_SSS_OBSERVE(DEPTH_STENCIL2,
                                                                        D3D12_DEPTH_STENCIL_DESC2,
                                                                        o.depth.desc = v;
                                                                        o.depth.state_known = true)
                                    RR_SSS_OBSERVE(DEPTH_STENCIL_FORMAT, DXGI_FORMAT,
                                                   o.depth.format = v;
                                                   o.depth.format_known =
                                                       true) RR_SSS_OBSERVE(INPUT_LAYOUT,
                                                                            D3D12_INPUT_LAYOUT_DESC,
                                                                            ObserveInput(o, v))
                                        RR_SSS_OBSERVE(
                                            IB_STRIP_CUT_VALUE, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE,
                                            o.strip_cut = unsigned(
                                                v)) RR_SSS_OBSERVE(PRIMITIVE_TOPOLOGY,
                                                                   D3D12_PRIMITIVE_TOPOLOGY_TYPE,
                                                                   o.topology = unsigned(v))
                                            RR_SSS_OBSERVE(
                                                RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY,
                                                o.rt_count = v.NumRenderTargets;
                                                for (UINT i = 0; i < 8; ++i) o.rt_formats[i] =
                                                    v.RTFormats[i]) RR_SSS_OBSERVE(SAMPLE_DESC,
                                                                                   DXGI_SAMPLE_DESC,
                                                                                   o.sample = v;
                                                                                   o.sample_known =
                                                                                       true)
                                                RR_SSS_OBSERVE(NODE_MASK, UINT, o.node_mask = v)
                                                    RR_SSS_OBSERVE(
                                                        CACHED_PSO, D3D12_CACHED_PIPELINE_STATE,
                                                        o.cached_present = v.pCachedBlob != nullptr;
                                                        o.cached_bytes = v.CachedBlobSizeInBytes)
                                                        RR_SSS_OBSERVE(FLAGS,
                                                                       D3D12_PIPELINE_STATE_FLAGS,
                                                                       o.flags = unsigned(v))
                                                            RR_SSS_OBSERVE(
                                                                VIEW_INSTANCING,
                                                                D3D12_VIEW_INSTANCING_DESC,
                                                                o.view_count = v.ViewInstanceCount;
                                                                o.view_flags = unsigned(v.Flags);
                                                                o.view_locations_present =
                                                                    v.pViewInstanceLocations !=
                                                                    nullptr) default
                    : o.unknown = true;
                problem(unsigned(kind));
                return o;
            }
#undef RR_SSS_OBSERVE
            o.seen |= 1ull << family;
            data += size;
            left -= size;
        }
        o.complete = true;
        return o;
    }
    static std::string DescribeObservation(const NativePipelineObservation &o) {
        std::ostringstream s;
        s << "nativeObservation{descriptionKnown=" << o.description_known << " stream=" << o.stream
          << " complete=" << o.complete << " malformed=" << o.malformed << " unknown=" << o.unknown
          << " bytes=" << o.bytes << " seen=0x" << std::hex << o.seen << " duplicates=0x"
          << o.duplicates << std::dec << " problem=" << o.problem_type << '@' << o.problem_offset;
        s << " sampleKnown=" << o.sample_known << " count=" << o.sample.Count
          << " quality=" << o.sample.Quality << "(0x" << std::hex << o.sample.Quality << std::dec
          << ") maskKnown=" << o.Known(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK)
          << " mask=" << o.sample_mask << "(0x" << std::hex << o.sample_mask << std::dec
          << ") topologyKnown=" << o.Known(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY)
          << " topology=" << o.topology
          << " stripCutKnown=" << o.Known(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE)
          << " stripCut=" << o.strip_cut
          << " nodeKnown=" << o.Known(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK)
          << " node=" << o.node_mask
          << " flagsKnown=" << o.Known(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS)
          << " flags=" << o.flags
          << " rootKnown=" << o.Known(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE)
          << " root=" << o.root_present;
        const auto &d = o.depth.desc;
        s << " depthKnown=" << o.depth.state_known << " dsvKnown=" << o.depth.format_known
          << " dsv=" << unsigned(o.depth.format) << " depth=" << d.DepthEnable << '/'
          << unsigned(d.DepthWriteMask) << '/' << unsigned(d.DepthFunc)
          << " bounds=" << d.DepthBoundsTestEnable << " stencil=" << d.StencilEnable;
        auto face = [&](const char *name, const D3D12_DEPTH_STENCILOP_DESC1 &f) {
            s << ' ' << name << "{func=" << unsigned(f.StencilFunc)
              << " fail=" << unsigned(f.StencilFailOp)
              << " depthFail=" << unsigned(f.StencilDepthFailOp)
              << " pass=" << unsigned(f.StencilPassOp) << " read=" << unsigned(f.StencilReadMask)
              << " write=" << unsigned(f.StencilWriteMask) << '}';
        };
        face("front", d.FrontFace);
        face("back", d.BackFace);
        const auto &r = o.raster;
        s << " rasterKnown=" << o.raster_known << " rasterVersion=" << o.raster_version
          << " fill=" << unsigned(r.FillMode) << " cull=" << unsigned(r.CullMode)
          << " frontCCW=" << r.FrontCounterClockwise << " depthBias=" << r.DepthBias
          << " legacyIntBias=" << o.legacy_depth_bias << " biasClamp=" << r.DepthBiasClamp
          << " slopeBias=" << r.SlopeScaledDepthBias << " depthClip=" << r.DepthClipEnable
          << " multisample=" << o.multisample << " aaLine=" << o.antialiased_line
          << " lineMode=" << unsigned(r.LineRasterizationMode)
          << " forcedSamples=" << r.ForcedSampleCount
          << " conservative=" << unsigned(r.ConservativeRaster);
        s << " shaderMalformed=" << o.shader_malformed
          << " stages(VS,PS,DS,HS,GS,CS,AS,MS;known/code/bytes)=";
        const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE stages[] = {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS};
        for (unsigned i = 0; i < 8; ++i) {
            const auto &v = o.shaders[i];
            s << o.Known(stages[i]) << '/' << v.code << '/' << v.bytes << ',';
        }
        s << " soKnown=" << o.Known(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT)
          << " SO=" << o.so_entries << '/' << o.so_strides << '/' << o.so_raster
          << " soPointers=" << o.so_entries_present << '/' << o.so_strides_present
          << " inputKnown=" << o.Known(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT)
          << " input=" << o.input_elements << '/' << o.input_present
          << " viewsKnown=" << o.Known(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING)
          << " views=" << o.view_count << '/' << o.view_flags << '/' << o.view_locations_present
          << " cachedKnown=" << o.Known(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO)
          << " cached=" << o.cached_present << '/' << o.cached_bytes
          << " rtKnown=" << o.Known(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS)
          << " RTs=" << o.rt_count << ':';
        for (auto f : o.rt_formats)
            s << unsigned(f) << ',';
        s << " blendKnown=" << o.blend_known << " alphaCoverage=" << o.blend.AlphaToCoverageEnable
          << " independentBlend=" << o.blend.IndependentBlendEnable
          << " blendRT(enable,logic,src,dst,op,srcA,dstA,opA,logicOp,write)=";
        for (const auto &b : o.blend.RenderTarget)
            s << '[' << b.BlendEnable << ',' << b.LogicOpEnable << ',' << unsigned(b.SrcBlend)
              << ',' << unsigned(b.DestBlend) << ',' << unsigned(b.BlendOp) << ','
              << unsigned(b.SrcBlendAlpha) << ',' << unsigned(b.DestBlendAlpha) << ','
              << unsigned(b.BlendOpAlpha) << ',' << unsigned(b.LogicOp) << ','
              << unsigned(b.RenderTargetWriteMask) << ']';
        s << '}';
        return s.str();
    }
    static bool Result(Status value, Status *status) noexcept {
        if (status)
            *status = value;
        return value == Status::Proven;
    }
    static bool LegacyProven(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                             Status *status = nullptr,
                             NativeDepthStencilState *depth = nullptr) noexcept {
        if (depth)
            *depth = {};
        if (!desc)
            return Result(Status::Description, status);
        if (!desc->PS.pShaderBytecode || !desc->PS.BytecodeLength)
            return Result((!desc->PS.pShaderBytecode && !desc->PS.BytecodeLength)
                              ? Status::Shader
                              : Status::MalformedStage,
                          status);
        if (depth)
            *depth = {NormalizeDepthStencil(desc->DepthStencilState), desc->DSVFormat, true, true};
        if (desc->StreamOutput.NumEntries || desc->StreamOutput.NumStrides ||
            desc->StreamOutput.RasterizedStream)
            return Result(Status::StreamOutput, status);
        if (!SupportedSamples(desc->SampleDesc))
            return Result(Status::SampleQuality, status);
        if (desc->NodeMask > 1)
            return Result(Status::NodeMask, status);
        return Result(Status::Proven, status);
    }
    static bool StreamProven(const D3D12_PIPELINE_STATE_STREAM_DESC *desc, Status *status = nullptr,
                             NativeDepthStencilState *depth = nullptr) noexcept {
        if (depth)
            *depth = {};
        Result(Status::Description, status);
        if (!desc || !desc->pPipelineStateSubobjectStream || !desc->SizeInBytes ||
            desc->SizeInBytes > 65536 ||
            reinterpret_cast<uintptr_t>(desc->pPipelineStateSubobjectStream) % alignof(void *))
            return false;
        const auto *data = static_cast<const unsigned char *>(desc->pPipelineStateSubobjectStream);
        size_t left = desc->SizeInBytes;
        uint64_t seen = 0;
        bool pixel = false;
        while (left) {
            if (left < sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE))
                return false;
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE kind;
            memcpy(&kind, data, sizeof(kind));
            unsigned family = unsigned(kind);
            if (family >= 64)
                return false;
            if (kind == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1 ||
                kind == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2)
                family = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL;
            if (kind == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1 ||
                kind == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2)
                family = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER;
            if (seen & (1ull << family))
                return false;
            seen |= 1ull << family;
            size_t size = 0;
#define RR_SSS_PART(K, T)                                                                          \
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_##K: {                                                \
        T value{};                                                                                 \
        if (!Take(data, left, value, size))                                                        \
            return false;                                                                          \
        break;                                                                                     \
    }
            switch (kind) {
                RR_SSS_PART(ROOT_SIGNATURE, ID3D12RootSignature *)
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: {
                D3D12_SHADER_BYTECODE value{};
                if (!Take(data, left, value, size))
                    return false;
                if (bool(value.pShaderBytecode) != bool(value.BytecodeLength))
                    return Result(Status::MalformedStage, status);
                pixel = value.pShaderBytecode && value.BytecodeLength;
                break;
            }
                RR_SSS_PART(VS, D3D12_SHADER_BYTECODE)
                RR_SSS_PART(DS, D3D12_SHADER_BYTECODE)
                RR_SSS_PART(HS, D3D12_SHADER_BYTECODE)
                RR_SSS_PART(GS, D3D12_SHADER_BYTECODE)
            // This proof supports graphics Draw paths only, not compute or mesh paths.
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: {
                D3D12_SHADER_BYTECODE value{};
                if (!Take(data, left, value, size))
                    return false;
                if (bool(value.pShaderBytecode) != bool(value.BytecodeLength))
                    return Result(Status::MalformedStage, status);
                if (value.pShaderBytecode)
                    return Result(kind == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS
                                      ? Status::ComputeStage
                                  : kind == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS
                                      ? Status::AmplificationStage
                                      : Status::MeshStage,
                                  status);
                break;
            }
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: {
                D3D12_STREAM_OUTPUT_DESC value{};
                if (!Take(data, left, value, size))
                    return false;
                if (value.NumEntries || value.NumStrides || value.RasterizedStream)
                    return Result(Status::StreamOutput, status);
                break;
            }
                RR_SSS_PART(BLEND, D3D12_BLEND_DESC)
                RR_SSS_PART(SAMPLE_MASK, UINT)
                RR_SSS_PART(RASTERIZER, D3D12_RASTERIZER_DESC)
            RR_SSS_PART(RASTERIZER1, D3D12_RASTERIZER_DESC1)
            RR_SSS_PART(RASTERIZER2, D3D12_RASTERIZER_DESC2)
#define RR_SSS_DEPTH(K, T)                                                                         \
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_##K: {                                                \
        T value{};                                                                                 \
        if (!Take(data, left, value, size))                                                        \
            return false;                                                                          \
        const auto normalized = NormalizeDepthStencil(value);                                      \
        if (normalized.DepthBoundsTestEnable)                                                      \
            return Result(Status::DepthBounds, status);                                            \
        if (depth) {                                                                               \
            depth->desc = normalized;                                                              \
            depth->state_known = true;                                                             \
        }                                                                                          \
        break;                                                                                     \
    }
                RR_SSS_DEPTH(DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC) RR_SSS_DEPTH(
                    DEPTH_STENCIL1,
                    D3D12_DEPTH_STENCIL_DESC1) RR_SSS_DEPTH(DEPTH_STENCIL2,
                                                            D3D12_DEPTH_STENCIL_DESC2)
#undef RR_SSS_DEPTH
                    RR_SSS_PART(INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC) RR_SSS_PART(
                        IB_STRIP_CUT_VALUE, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE)
                        RR_SSS_PART(PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE) RR_SSS_PART(
                            RENDER_TARGET_FORMATS,
                            D3D12_RT_FORMAT_ARRAY) case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: {
                DXGI_FORMAT value{};
                if (!Take(data, left, value, size))
                    return false;
                if (depth) {
                    depth->format = value;
                    depth->format_known = true;
                }
                break;
            }
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: {
                DXGI_SAMPLE_DESC value{};
                if (!Take(data, left, value, size))
                    return false;
                if (!SupportedSamples(value))
                    return Result(Status::SampleQuality, status);
                break;
            }
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: {
                UINT value{};
                if (!Take(data, left, value, size))
                    return false;
                if (value > 1)
                    return Result(Status::NodeMask, status);
                break;
            }
                RR_SSS_PART(CACHED_PSO, D3D12_CACHED_PIPELINE_STATE)
                RR_SSS_PART(FLAGS, D3D12_PIPELINE_STATE_FLAGS)
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
                D3D12_VIEW_INSTANCING_DESC value{};
                if (!Take(data, left, value, size))
                    return false;
                if (value.ViewInstanceCount || value.Flags != D3D12_VIEW_INSTANCING_FLAG_NONE)
                    return Result(Status::ViewInstancing, status);
                break;
            }
            default:
                return false;
            }
#undef RR_SSS_PART
            data += size;
            left -= size;
        }
        return Result(pixel ? Status::Proven : Status::Shader, status);
    }
    static bool Equivalent(const NativePipelineObservation &a,
                           const NativePipelineObservation &b) noexcept {
        auto valid = [](const auto &o) {
            return o.complete && !o.malformed && !o.unknown && o.root_present && o.root_identity &&
                   o.input_valid && o.sample_known && o.raster_known && o.blend_known &&
                   o.raster_version == 0 && o.rt_count > 0 && o.rt_count <= 4 && !o.so_entries &&
                   !o.so_strides && !o.so_raster && !o.view_count && !o.view_flags &&
                   !o.cached_bytes && !o.cached_present;
        };
        if (!valid(a) || !valid(b) || a.root_identity != b.root_identity ||
            a.input_elements != b.input_elements || a.input_hash != b.input_hash ||
            a.sample.Count != b.sample.Count || a.sample.Quality != b.sample.Quality ||
            a.sample_mask != b.sample_mask || a.topology != b.topology ||
            a.strip_cut != b.strip_cut || a.node_mask != b.node_mask || a.flags != b.flags ||
            a.rt_count != b.rt_count || !DepthStencilEquivalent(a.depth, b.depth))
            return false;
        for (unsigned i = 0; i < 8; ++i)
            if (a.shaders[i].bytes != b.shaders[i].bytes ||
                a.shaders[i].code != b.shaders[i].code || a.shaders[i].hash != b.shaders[i].hash)
                return false;
        const auto &x = a.raster;
        const auto &y = b.raster;
        if (x.FillMode != y.FillMode || x.CullMode != y.CullMode ||
            x.FrontCounterClockwise != y.FrontCounterClockwise ||
            a.legacy_depth_bias != b.legacy_depth_bias || x.DepthBiasClamp != y.DepthBiasClamp ||
            x.SlopeScaledDepthBias != y.SlopeScaledDepthBias ||
            x.DepthClipEnable != y.DepthClipEnable || a.multisample != b.multisample ||
            a.antialiased_line != b.antialiased_line ||
            x.ForcedSampleCount != y.ForcedSampleCount ||
            x.ConservativeRaster != y.ConservativeRaster ||
            a.blend.AlphaToCoverageEnable != b.blend.AlphaToCoverageEnable)
            return false;
        for (UINT i = 0; i < a.rt_count; ++i) {
            if (a.rt_formats[i] != b.rt_formats[i])
                return false;
            const auto &u = a.blend.RenderTarget[a.blend.IndependentBlendEnable ? i : 0];
            const auto &v = b.blend.RenderTarget[b.blend.IndependentBlendEnable ? i : 0];
            if (u.BlendEnable != v.BlendEnable || u.LogicOpEnable != v.LogicOpEnable ||
                u.RenderTargetWriteMask != v.RenderTargetWriteMask)
                return false;
            if (u.BlendEnable &&
                (u.SrcBlend != v.SrcBlend || u.DestBlend != v.DestBlend || u.BlendOp != v.BlendOp ||
                 u.SrcBlendAlpha != v.SrcBlendAlpha || u.DestBlendAlpha != v.DestBlendAlpha ||
                 u.BlendOpAlpha != v.BlendOpAlpha))
                return false;
            if (u.LogicOpEnable && u.LogicOp != v.LogicOp)
                return false;
        }
        return true;
    }
    class QualityScope;

  private:
    static inline thread_local QualityScope *quality_scope{};

  public:
    class QualityScope {
        friend class PipelineProof;
        ID3D12Device *device;
        NativePipelineObservation expected;
        QualityScope *previous{};
        bool active{};

      public:
        bool attempted{}, applied{};
        QualityScope(ID3D12Device *d, const NativePipelineObservation &e)
            : device(d), expected(e), previous(quality_scope) {
            active = !previous;
            quality_scope = this;
        }
        ~QualityScope() {
            if (quality_scope == this)
                quality_scope = previous;
        }
        QualityScope(const QualityScope &) = delete;
        QualityScope &operator=(const QualityScope &) = delete;
    };
    static bool ApplyQuality(ID3D12Device *d, const D3D12_GRAPHICS_PIPELINE_STATE_DESC &source,
                             D3D12_GRAPHICS_PIPELINE_STATE_DESC &copy) noexcept {
        auto *s = quality_scope;
        if (!s || !s->active || s->attempted)
            return false;
        s->attempted = true;
        if (d != s->device || source.SampleDesc.Count != 4 || source.SampleDesc.Quality != 0 ||
            !SupportedSamples(s->expected.sample) || s->expected.sample.Count != 4)
            return false;
        copy = source;
        copy.SampleDesc = s->expected.sample;
        copy.CachedPSO = {};
        if (!Equivalent(s->expected, ObserveLegacy(&copy)))
            return false;
        s->applied = true;
        return true;
    }
    static bool Attach(ID3D12Device *object) noexcept {
        Error error;
        try {
            auto device = fg::NativeObject<ID3D12Device>(object);
            if (!device)
                return false;
            std::lock_guard lock(install_mutex);
            const auto initialized = MH_Initialize();
            if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED)
                return false;
            bool okay = Hook<Graphics>((*reinterpret_cast<void ***>(device.Get()))[10]);
            Microsoft::WRL::ComPtr<ID3D12Device1> d1;
            if (SUCCEEDED(device.As(&d1)))
                okay = Hook<Library>((*reinterpret_cast<void ***>(d1.Get()))[44]) && okay;
            Microsoft::WRL::ComPtr<ID3D12Device2> d2;
            if (SUCCEEDED(device.As(&d2)))
                okay = Hook<Stream>((*reinterpret_cast<void ***>(d2.Get()))[47]) && okay;
            return okay;
        } catch (...) {
            return false;
        }
    }

  private:
    static bool ReadMarker(ID3D12PipelineState *object, Marker &marker) noexcept {
        Error error;
        try {
            auto p = fg::NativeObject<ID3D12PipelineState>(object);
            if (!p)
                return false;
            UINT size = sizeof(marker);
            return SUCCEEDED(p->GetPrivateData(proof_id, &size, &marker)) &&
                   size == sizeof(marker) && marker.owner == reinterpret_cast<uintptr_t>(&cookie) &&
                   marker.version == 5 &&
                   marker.accepted == (marker.status == Status::Proven ? 1u : 0u);
        } catch (...) {
            return false;
        }
    }

  public:
    static bool NativeObservation(ID3D12PipelineState *object,
                                  NativePipelineObservation &out) noexcept {
        out = {};
        Marker marker{};
        if (!ReadMarker(object, marker))
            return false;
        out = marker.observation;
        return true;
    }
    static std::string NativeDescription(ID3D12PipelineState *object) {
        NativePipelineObservation o;
        if (!NativeObservation(object, o))
            return "nativeObservation{unobserved}";
        return DescribeObservation(o);
    }
    static Status NativeStatus(ID3D12PipelineState *object) noexcept {
        Marker marker{};
        return ReadMarker(object, marker) ? marker.status : Status::Unobserved;
    }
    static bool NativeDepthStencil(ID3D12PipelineState *object,
                                   NativeDepthStencilState &out) noexcept {
        out = {};
        Marker marker{};
        if (!ReadMarker(object, marker) || marker.status != Status::Proven ||
            !marker.depth.state_known || !marker.depth.format_known ||
            marker.depth.desc.DepthBoundsTestEnable)
            return false;
        out = marker.depth;
        return true;
    }
    static bool NativePipelineProven(ID3D12PipelineState *object) noexcept {
        return NativeStatus(object) == Status::Proven;
    }
    static const char *NativePipelineStatus(ID3D12PipelineState *object) noexcept {
        switch (NativeStatus(object)) {
        case Status::Proven:
            return "proven";
        case Status::Description:
            return "native-description-unsupported";
        case Status::Shader:
            return "native-pixel-stage-missing";
        case Status::ComputeStage:
            return "native-compute-stage-present";
        case Status::AmplificationStage:
            return "native-amplification-stage-present";
        case Status::MeshStage:
            return "native-mesh-stage-present";
        case Status::MalformedStage:
            return "native-stage-bytecode-malformed";
        case Status::DepthBounds:
            return "native-depth-bounds-enabled";
        case Status::StreamOutput:
            return "native-stream-output-unsupported";
        case Status::SampleQuality:
            return "native-sample-quality-nonzero";
        case Status::NodeMask:
            return "native-node-mask-unsupported";
        case Status::ViewInstancing:
            return "native-view-instancing-unsupported";
        default:
            return "native-proof-unobserved";
        }
    }
};
inline bool NativeDepthStencil(ID3D12PipelineState *pipeline,
                               NativeDepthStencilState &out) noexcept {
    return PipelineProof::NativeDepthStencil(pipeline, out);
}
inline bool NativeDepthStencilEquivalent(ID3D12PipelineState *a, ID3D12PipelineState *b) noexcept {
    NativeDepthStencilState x, y;
    return NativeDepthStencil(a, x) && NativeDepthStencil(b, y) && DepthStencilEquivalent(x, y);
}
inline bool NativePipelineProven(ID3D12PipelineState *pipeline) noexcept {
    return PipelineProof::NativePipelineProven(pipeline);
}
inline const char *NativePipelineStatus(ID3D12PipelineState *pipeline) noexcept {
    return PipelineProof::NativePipelineStatus(pipeline);
}
#pragma warning(pop)
} // namespace rrsss
