#pragma once
// Temporary CPU-only observer. Never patches a shader, submits GPU work or retains a game object.
#include <reshade.hpp>
#include "rr_sss_runtime_shader.h"
#include "rr_sss_target_capture.h"
#include <d3d12shader.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <iomanip>
namespace rrsssdiag {
inline constexpr uint64_t ExpectedHash = 0x9ee7cea57f70d741ull;
inline constexpr size_t ExpectedBytes = 56388;
inline uint64_t Hash(const void *data, size_t size) {
    auto p = static_cast<const unsigned char *>(data);
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
inline std::string Hex(uint64_t v) {
    std::ostringstream s;
    s << std::hex << std::setw(16) << std::setfill('0') << v;
    return s.str();
}
class Tracker {
    struct Entry {
        uint64_t hash{};
        bool candidate{}, drawn{}, indirect{}, unknown{};
    };
    std::mutex mutex;
    std::unordered_map<uint64_t, Entry> entries;
    std::unordered_set<uint64_t> reflected;
    std::atomic<unsigned> callbacks{}, pixels{}, exact{}, candidates{}, draws{}, untracked{},
        busy{}, overflow{}, errors{};
    unsigned emitted{};
    uint64_t last_summary{};
    std::atomic<unsigned> reflection_count{}, outcomes{}, unknown_indirect{}, malformed{};
    std::atomic<bool> stopped{};
    uint64_t started{};
    void Emit(const std::string &s) noexcept {
        try {
            if (logger)
                logger(s.c_str());
        } catch (...) {
        }
    }
    void Reflect(const reshade::api::shader_desc &ps, uint64_t pipeline, HMODULE compiler) {
        const auto shaderHash = Hash(ps.code, ps.code_size);
        bool full = false, firstOverflow = false;
        {
            std::lock_guard lock(mutex);
            if (reflected.contains(shaderHash))
                return;
            full = reflected.size() >= 256;
            if (!full)
                reflected.insert(shaderHash);
            else
                firstOverflow = reflection_count.fetch_add(1) == 0;
        }
        if (full) {
            if (firstOverflow)
                Emit("SSS-DIAG reflection-limit=256 unique candidates; remaining shader identities "
                     "still recorded");
            return;
        }
        auto prefix = "SSS-DIAG pipeline=" + Hex(pipeline) + " ";
        auto create = compiler ? reinterpret_cast<DxcCreateInstanceProc>(
                                     GetProcAddress(compiler, "DxcCreateInstance"))
                               : nullptr;
        if (!create) {
            Emit(prefix + "reflection=compiler-unavailable");
            return;
        }
        using Microsoft::WRL::ComPtr;
        ComPtr<IDxcLibrary> library;
        ComPtr<IDxcBlobEncoding> source;
        ComPtr<IDxcContainerReflection> container;
        if (FAILED(create(CLSID_DxcLibrary, IID_PPV_ARGS(&library))) ||
            FAILED(create(CLSID_DxcContainerReflection, IID_PPV_ARGS(&container))) ||
            FAILED(library->CreateBlobWithEncodingFromPinned(ps.code, UINT32(ps.code_size), 0,
                                                             &source)) ||
            FAILED(container->Load(source.Get()))) {
            Emit(prefix + "reflection=container-failed");
            return;
        }
        UINT32 part{};
        if (FAILED(container->FindFirstPartKind(0x4c495844, &part))) {
            Emit(prefix + "reflection=no-dxil");
            return;
        }
        ComPtr<IDxcBlob> dxil;
        if (SUCCEEDED(container->GetPartContent(part, &dxil)) && dxil)
            Emit(prefix + "dxilHash=" + Hex(Hash(dxil->GetBufferPointer(), dxil->GetBufferSize())) +
                 " dxilBytes=" + std::to_string(dxil->GetBufferSize()));
        ComPtr<ID3D12ShaderReflection> reflection;
        D3D12_SHADER_DESC desc{};
        if (FAILED(container->GetPartReflection(part, IID_PPV_ARGS(&reflection))) ||
            FAILED(reflection->GetDesc(&desc))) {
            Emit(prefix + "reflection=shader-failed");
            return;
        }
        std::ostringstream s;
        s << prefix << "reflection=ok version=" << desc.Version
          << " instructions=" << desc.InstructionCount << " inputs=" << desc.InputParameters
          << " outputs=" << desc.OutputParameters << " resources=" << desc.BoundResources
          << " bindings=";
        for (UINT i = 0; i < (std::min)(desc.BoundResources, 128u); ++i) {
            D3D12_SHADER_INPUT_BIND_DESC b{};
            if (SUCCEEDED(reflection->GetResourceBindingDesc(i, &b)))
                s << "[type=" << b.Type << " reg=" << b.BindPoint << " space=" << b.Space
                  << " count=" << b.BindCount << " dim=" << b.Dimension << "]";
        }
        Emit(s.str());
        for (bool output : {false, true}) {
            std::ostringstream sig;
            sig << prefix << (output ? "outputSignature=" : "inputSignature=");
            const auto count = output ? desc.OutputParameters : desc.InputParameters;
            for (UINT i = 0; i < (std::min)(count, 32u); ++i) {
                D3D12_SIGNATURE_PARAMETER_DESC p{};
                auto hr = output ? reflection->GetOutputParameterDesc(i, &p)
                                 : reflection->GetInputParameterDesc(i, &p);
                if (SUCCEEDED(hr))
                    sig << "["
                        << std::string(
                               p.SemanticName ? p.SemanticName : "?", 0,
                               (std::min)(p.SemanticName ? strlen(p.SemanticName) : size_t(1),
                                          size_t(64)))
                        << p.SemanticIndex << " reg=" << p.Register
                        << " system=" << p.SystemValueType << " mask=" << unsigned(p.Mask) << "]";
            }
            Emit(sig.str());
        }
    }

  public:
    static constexpr unsigned Limit = 8192;
    void (*logger)(const char *){};
    bool Active() const noexcept {
        return !stopped.load(std::memory_order_relaxed);
    }
    void Observe(uint64_t device, uint64_t layout, unsigned n,
                 const reshade::api::pipeline_subobject *objects, uint64_t pipeline,
                 HMODULE compiler) noexcept {
        if (!Active())
            return;
        ++callbacks;
        try {
            auto prefix = "SSS-DIAG pipeline=" + Hex(pipeline) + " device=" + Hex(device) +
                          " layout=" + Hex(layout) + " ";
            if (!objects || !n || n > 64) {
                if (malformed.fetch_add(1) < 16)
                    Emit(prefix + "description=missing count=" + std::to_string(n));
                return;
            }
            const reshade::api::shader_desc *ps = nullptr;
            unsigned rt_count = ~0u, rt_format = ~0u, samples = ~0u, depth = ~0u, stencil = ~0u,
                     topology = ~0u;
            unsigned ps_count = 0;
            for (unsigned i = 0; i < n; ++i) {
                auto &o = objects[i];
                using T = reshade::api::pipeline_subobject_type;
                if (o.type == T::pixel_shader) {
                    ++ps_count;
                    if (o.count == 1 && o.data)
                        ps = static_cast<const reshade::api::shader_desc *>(o.data);
                }
                if (!o.data || !o.count)
                    continue;
                if (o.type == T::render_target_formats) {
                    rt_count = o.count;
                    rt_format = unsigned(*static_cast<const reshade::api::format *>(o.data));
                }
                if (o.type == T::sample_count && o.count == 1)
                    samples = *static_cast<const unsigned *>(o.data);
                if (o.type == T::primitive_topology && o.count == 1)
                    topology =
                        unsigned(*static_cast<const reshade::api::primitive_topology *>(o.data));
                if (o.type == T::depth_stencil_state && o.count == 1) {
                    auto &d = *static_cast<const reshade::api::depth_stencil_desc *>(o.data);
                    depth = d.depth_enable;
                    stencil = d.stencil_enable;
                }
            }
            if (!ps_count)
                return;
            if (ps_count == 1 && ps)
                rrssscapture::Session().Observe(ps->code, ps->code_size);
            bool full = false, firstOverflow = false;
            {
                std::lock_guard lock(mutex);
                full = emitted >= Limit;
                if (full)
                    firstOverflow = overflow.fetch_add(1) == 0;
                else
                    ++emitted;
            }
            if (full) {
                if (firstOverflow)
                    Emit("SSS-DIAG inventory-limit=8192; capture incomplete");
                return;
            }
            if (ps_count != 1 || !ps || !ps->code || !ps->code_size ||
                ps->code_size > 4 * 1024 * 1024) {
                Emit(prefix + "description=missing-code-or-invalid-size");
                return;
            }
            ++pixels;
            auto hash = Hash(ps->code, ps->code_size);
            const bool matched = ps->code_size == ExpectedBytes && hash == ExpectedHash;
            // Broad discovery only: NONE of these heuristics admits a replacement shader.
            const bool candidate =
                matched || ps->code_size == ExpectedBytes ||
                (rt_count == 1 && rt_format == unsigned(reshade::api::format::r11g11b10_float));
            exact += matched;
            candidates += candidate;
            {
                std::lock_guard lock(mutex);
                entries[pipeline] = {hash, candidate, false, false};
            }
            std::ostringstream s;
            s << prefix << "hash=" << Hex(hash) << " bytes=" << ps->code_size << " recognition="
              << (matched                          ? "exact"
                  : ps->code_size != ExpectedBytes ? "size-mismatch"
                                                   : "hash-mismatch")
              << " candidate=" << candidate << " rtCount=" << rt_count << " rt0=" << rt_format
              << " samples=" << samples << " depth=" << depth << " stencil=" << stencil
              << " topology=" << topology;
            Emit(s.str());
            if (candidate)
                Reflect(*ps, pipeline, compiler);
        } catch (...) {
            ++errors;
        }
    }
    void Outcome(const char *text) noexcept {
        try {
            if (Active() && outcomes.fetch_add(1) < 64)
                Emit(std::string("SSS-DIAG preparation: ") + text);
        } catch (...) {
            ++errors;
        }
    }
    void Forget(uint64_t pipeline) noexcept {
        try {
            std::lock_guard lock(mutex);
            entries.erase(pipeline);
        } catch (...) {
            ++errors;
        }
    }
    void Draw(uint64_t pipeline, bool indirect, bool unknown = false) noexcept {
        if (!Active())
            return;
        if (unknown)
            ++unknown_indirect;
        else
            ++draws;
        try {
            uint64_t hash{};
            bool candidate{};
            {
                std::unique_lock lock(mutex, std::try_to_lock);
                if (!lock.owns_lock()) {
                    ++busy;
                    return;
                }
                auto it = entries.find(pipeline);
                if (it == entries.end()) {
                    ++untracked;
                    return;
                }
                bool &seen = unknown    ? it->second.unknown
                             : indirect ? it->second.indirect
                                        : it->second.drawn;
                if (seen)
                    return;
                seen = true;
                hash = it->second.hash;
                candidate = it->second.candidate;
            }
            Emit(std::string(unknown ? "SSS-DIAG first-indirect-unknown pipeline="
                                     : "SSS-DIAG first-draw pipeline=") +
                 Hex(pipeline) + " hash=" + Hex(hash) + " candidate=" + std::to_string(candidate) +
                 " kind=" +
                 (unknown    ? "unknown-signature"
                  : indirect ? "indirect"
                             : "direct"));
        } catch (...) {
            ++errors;
        }
    }
    void Summary(uint64_t now, bool rr = true, bool sss = true) noexcept {
        // Called from the existing runtime callback. No controller or game-object access.
        try {
            std::string line;
            {
                std::unique_lock lock(mutex, std::try_to_lock);
                if (!lock.owns_lock() || !Active())
                    return;
                if (!started)
                    started = now;
                if (last_summary && now - last_summary < 10000)
                    return;
                last_summary = now;
                const bool done = now - started >= 600000;
                std::ostringstream s;
                s << "SSS-DIAG summary callbacks=" << callbacks.load()
                  << " pixelPipelines=" << pixels.load() << " exact=" << exact.load()
                  << " candidates=" << candidates.load() << " draws=" << draws.load()
                  << " unknownIndirect=" << unknown_indirect.load()
                  << " untrackedDraws=" << untracked.load()
                  << " malformedDescriptions=" << malformed.load() << " rrEnabled=" << rr
                  << " sssEnabled=" << sss << " busyDraws=" << busy.load()
                  << " overflow=" << overflow.load() << " errors=" << errors.load()
                  << " recording=" << (done ? "finished" : "on");
                line = s.str();
                if (done)
                    stopped = true;
            }
            Emit(line);
        } catch (...) {
            ++errors;
        }
    }
};
inline Tracker &Session() {
    static auto *p = new Tracker;
    return *p;
}
} // namespace rrsssdiag
