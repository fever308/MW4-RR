#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <vector>
#include <string>
#include <regex>
#include <memory>
#include <mutex>
#include <atomic>
#include <cctype>
#include <cstring>
#include <iterator>
#include <map>
#pragma comment(lib, "bcrypt.lib")

namespace rrsss {
// Only transformation rules and fingerprints are shipped. The input comes
// from the game's pipeline creation callback; generated code stays in memory.
inline bool ShaderDigest(const void *data, size_t size, const char *expected) noexcept {
    if (!data || !size || size > 4 * 1024 * 1024)
        return false;
    std::array<unsigned char, 32> digest{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                   static_cast<PUCHAR>(const_cast<void *>(data)), ULONG(size),
                   digest.data(), ULONG(digest.size())) < 0)
        return false;
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < digest.size(); ++i)
        if (hex[digest[i] >> 4] != expected[2*i] || hex[digest[i] & 15] != expected[2*i+1])
            return false;
    return true;
}
inline bool ReplaceShaderOnce(std::string &text, const std::string &from,
                               const std::string &to) {
    const auto at = text.find(from);
    if (at == std::string::npos || text.find(from, at + from.size()) != std::string::npos)
        return false;
    text.replace(at, from.size(), to);
    return true;
}
inline bool Sha256(const void *data, size_t size, std::array<unsigned char, 32> &digest) noexcept {
    return data && size && size <= 4 * 1024 * 1024 &&
           BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                      static_cast<PUCHAR>(const_cast<void *>(data)), ULONG(size), digest.data(),
                      ULONG(digest.size())) >= 0;
}
inline uint64_t ShaderHash(const void *data, size_t size) noexcept {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i)
        h = (h ^ static_cast<const unsigned char *>(data)[i]) * 1099511628211ull;
    return h;
}
// Captured builds of the lighting composite that carries the SSS guide and
// produces the transparency layer. Each pins its native input and the
// validated guide generated from it. The builds differ only in the scene
// constant buffer's declared tail, which the shader never reads.
struct ShaderVariant {
    size_t bytes;
    uint64_t hash;
    const char *sha256;
    size_t output_bytes;
    const char *output_sha256;
};
inline constexpr ShaderVariant Variants[] = {
    {56388, 0x9ee7cea57f70d741ull,
     "3632196273fba431d92b8ae659cb9dac09444c43c6244205ba4b4fefab02e591", 63128,
     "bfa44f35ca4c29500ff5f629ba3504c86eb30d7ea38c92067cb8a788780aac8b"},
    {56352, 0xc6c858a6310ca6a2ull,
     "7fb2e21d444b0ece2629b4caa97743c2acfdc09d1ae463dd9e362c8b1b5e7173", 63056,
     "73291469f0018187c5028edf1c3012118753083b071294e37a57f7119a7810d8"},
};
inline const ShaderVariant *KnownVariant(uint64_t hash, size_t bytes) noexcept {
    for (const auto &v : Variants)
        if (v.hash == hash && v.bytes == bytes)
            return &v;
    return nullptr;
}
inline const ShaderVariant *KnownVariant(const void *data, size_t size) noexcept {
    if (!data || !size || size > 4 * 1024 * 1024)
        return nullptr;
    for (const auto &v : Variants)
        if (v.bytes == size && ShaderDigest(data, size, v.sha256))
            return &v;
    return nullptr;
}
// Another build is accepted only when its disassembly, normalized by
// StructuralText, matches the captured builds': the same instructions,
// bindings, signatures and metadata. Only fingerprints are shipped.
inline constexpr const char *InputStructure =
    "83e3d50d5cffcd501eb51873a68c8bd30f8157d780ab919bf8b911978777df7f";
inline constexpr const char *OutputStructure =
    "15ab971c5d61c21c30b4d1b7c4e5653fc1fbfa145e1cf2fca2749eda11e5065a";
// Highest scene constant-buffer byte the shader (and so the guide) reads.
inline constexpr size_t SceneReadBytes = 12112;
// Cheap filter before disassembly: the container's signature, runtime-info
// and feature parts must equal the captured builds'.
inline bool StructuralCandidate(const void *data, size_t size) noexcept {
    if (!data || size < 32 || size > 4 * 1024 * 1024)
        return false;
    const auto *b = static_cast<const unsigned char *>(data);
    auto u32 = [&](size_t at) {
        uint32_t v;
        std::memcpy(&v, b + at, 4);
        return v;
    };
    const uint32_t count = u32(28);
    if (std::memcmp(b, "DXBC", 4) || u32(24) != size || count > 64 || 32 + 4ull * count > size)
        return false;
    struct Part {
        uint32_t fourcc;
        uint64_t hash;
        bool seen;
    } parts[] = {{0x30494653u, 0xa8c7f832281a39c5ull, false},  // SFI0
                 {0x31475349u, 0x95d4f83ffd7546b6ull, false},  // ISG1
                 {0x3147534fu, 0x5116c835e844e585ull, false},  // OSG1
                 {0x30565350u, 0xdc71665911d748feull, false}}; // PSV0
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t at = u32(32 + 4 * i);
        if (at > size - 8 || u32(at + 4) > size - 8 - at)
            return false;
        for (auto &p : parts)
            if (p.fourcc == u32(at)) {
                if (p.seen || ShaderHash(b + at + 8, u32(at + 4)) != p.hash)
                    return false;
                p.seen = true;
            }
    }
    return std::all_of(std::begin(parts), std::end(parts), [](const Part &p) { return p.seen; });
}
// Removes what a rebuild may change without changing behavior: the shader
// hash and debug name, and the scene constant buffer's declared layout and
// size, which must still cover every byte the shader reads. Each other
// declaration, instruction and metadata line is kept verbatim.
inline bool StructuralText(const std::string &text, std::string &out) {
    out.clear();
    std::vector<std::string> lines;
    for (size_t at = 0; at <= text.size();) {
        auto end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        auto line = text.substr(at, end - at);
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
        while (!line.empty() && line.back() == '\0')
            line.pop_back();
        lines.push_back(std::move(line));
        at = end + 1;
    }
    auto once = [&](auto match) -> std::string * {
        std::string *found = nullptr;
        for (auto &l : lines)
            if (match(l)) {
                if (found)
                    return nullptr;
                found = &l;
            }
        return found;
    };
    auto starts = [](const std::string &l, const char *p) { return l.rfind(p, 0) == 0; };
    for (const char *p : {"; shader debug name: ", "; shader hash: "}) {
        if (std::count_if(lines.begin(), lines.end(),
                          [&](const std::string &x) { return starts(x, p); }) > 1)
            return false;
        for (auto &l : lines)
            if (starts(l, p))
                l.clear();
    }
    const std::regex resource(
        R"(^(!\d+ = !\{i32 0, %hostlayout\.scene\* undef, !"", i32 0, i32 3, i32 1, i32 )(\d{1,6})(, null\})$)");
    std::smatch m;
    auto *scene = once([&](const std::string &x) {
        return starts(x, "!") && x.find("%hostlayout.scene* undef") != std::string::npos;
    });
    if (!scene || !std::regex_match(*scene, m, resource))
        return false;
    const auto bytes = std::stoul(m[2].str());
    if (bytes % 16 || bytes < SceneReadBytes || bytes > 65536)
        return false;
    const auto declared = "[" + m[2].str() + " x i8]";
    *scene = m[1].str() + "S" + m[3].str();
    auto *comment = once([&](const std::string &x) {
        return starts(x, ";") && x.find(declared) != std::string::npos;
    });
    if (!comment || !ReplaceShaderOnce(*comment, declared, "[S x i8]"))
        return false;
    auto *layout = once([&](const std::string &x) {
        return starts(x, "%hostlayout.struct.GfxCodeSceneConsts = type ");
    });
    if (!layout)
        return false;
    *layout = "%hostlayout.struct.GfxCodeSceneConsts = type S";
    // Struct types that only the dropped scene layout used go with it.
    auto ident = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '$';
    };
    auto tokens = [&](const std::string &l) {
        std::vector<std::string> found;
        for (auto at = l.find("%struct."); at != std::string::npos; at = l.find("%struct.", at + 1)) {
            auto end = at + 8;
            while (end < l.size() && ident(l[end]))
                ++end;
            found.push_back(l.substr(at, end - at));
        }
        return found;
    };
    std::map<std::string, size_t> uses;
    for (const auto &l : lines)
        for (const auto &t : tokens(l))
            ++uses[t];
    for (bool removed = true; removed;) {
        removed = false;
        for (auto &l : lines) {
            if (!starts(l, "%struct."))
                continue;
            const auto name = tokens(l).front();
            if (l.compare(name.size(), 8, " = type ") || uses[name] != 1)
                continue;
            for (const auto &t : tokens(l))
                --uses[t];
            l.clear();
            removed = true;
        }
    }
    for (const auto &l : lines)
        if (!l.empty())
            out += l + '\n';
    return true;
}
inline bool StructureMatches(const std::string &text, const char *expected) {
    std::string normalized;
    return StructuralText(text, normalized) &&
           ShaderDigest(normalized.data(), normalized.size(), expected);
}
inline bool TransformRuntimeShader(std::string &text) {
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    if (text.find("%sss.") != std::string::npos || text.find("@dx.op.discard") != std::string::npos)
        return false;
    if (!ReplaceShaderOnce(text, "%608 = and i32 %607, 16384", "%608 = and i32 %607, 16384"))
        return false;
    std::string added;
    const std::regex ids(R"(%(61[1-9]|62[0-3])\b)");
    for (int i = 611; i <= 623; ++i) {
        const auto prefix = "\n  %" + std::to_string(i) + " = ";
        const auto at = text.find(prefix);
        if (at == std::string::npos || text.find(prefix, at + prefix.size()) != std::string::npos)
            return false;
        const auto end = text.find('\n', at + 1);
        if (end == std::string::npos)
            return false;
        auto line = text.substr(at + 1, end - at - 1);
        if (i == 623 && line != "  %623 = fmul fast float %622, 2.500000e-01")
            return false;
        added += std::regex_replace(line, ids, "%sss.$1") + '\n';
    }
    const std::string first = "  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 0, float %3584)";
    if (!ReplaceShaderOnce(text, first, added + first) ||
        !ReplaceShaderOnce(text, "  ret void",
            "  call void @dx.op.storeOutput.f32(i32 5, i32 1, i32 0, i8 0, float %sss.623)\n  ret void"))
        return false;
    int next = 0;
    const std::regex metadata(R"((?:^|\n)!(\d+) = )");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), metadata);
         it != std::sregex_iterator(); ++it)
        next = (std::max)(next, std::stoi((*it)[1].str()) + 1);
    if (!next || !ReplaceShaderOnce(text, "!52 = !{!53}", "!52 = !{!53, !" + std::to_string(next) + "}"))
        return false;
    text += "\n!" + std::to_string(next) + " = !{i32 1, !\"SV_Target\", i8 9, i8 16, !" +
        std::to_string(next+1) + ", i8 0, i32 1, i8 1, i32 1, i8 0, !" + std::to_string(next+2) + "}\n!" +
        std::to_string(next+1) + " = !{i32 1}\n!" + std::to_string(next+2) + " = !{i32 3, i32 1}\n";
    return true;
}
struct ShaderTools {
    Microsoft::WRL::ComPtr<IDxcLibrary> library;
    Microsoft::WRL::ComPtr<IDxcCompiler> compiler;
    Microsoft::WRL::ComPtr<IDxcAssembler> assembler;
    Microsoft::WRL::ComPtr<IDxcValidator> validator;
    bool Create(HMODULE module) noexcept {
        auto create = module ? reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module, "DxcCreateInstance")) : nullptr;
        return create && SUCCEEDED(create(CLSID_DxcLibrary, IID_PPV_ARGS(&library))) &&
               SUCCEEDED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) &&
               SUCCEEDED(create(CLSID_DxcAssembler, IID_PPV_ARGS(&assembler))) &&
               SUCCEEDED(create(CLSID_DxcValidator, IID_PPV_ARGS(&validator)));
    }
    bool Disassemble(const void *code, size_t size, std::string &text) {
        Microsoft::WRL::ComPtr<IDxcBlobEncoding> source, disassembly;
        if (!code || !size || size > 4 * 1024 * 1024 ||
            FAILED(library->CreateBlobWithEncodingFromPinned(code, UINT32(size), 0, &source)) ||
            FAILED(compiler->Disassemble(source.Get(), &disassembly)) || !disassembly ||
            disassembly->GetBufferSize() > 4 * 1024 * 1024)
            return false;
        text.assign(static_cast<const char *>(disassembly->GetBufferPointer()), disassembly->GetBufferSize());
        while (!text.empty() && text.back() == '\0') text.pop_back();
        return true;
    }
};
// Assembles and validates a transformed disassembly into a container.
inline bool AssembleShader(ShaderTools &dxc, const std::string &text, std::vector<unsigned char> &out,
                           const char **status = nullptr) {
    using Microsoft::WRL::ComPtr;
    auto fail = [&](const char *why) {
        if (status)
            *status = why;
        return false;
    };
    out.clear();
    ComPtr<IDxcBlobEncoding> modified;
    if (FAILED(dxc.library->CreateBlobWithEncodingOnHeapCopy(text.data(), UINT32(text.size()), CP_UTF8, &modified)))
        return fail("shader-assembly-failed");
    auto succeeded = [](IDxcOperationResult *op) {
        HRESULT result = E_FAIL;
        return op && SUCCEEDED(op->GetStatus(&result)) && SUCCEEDED(result);
    };
    ComPtr<IDxcOperationResult> assembled, validated;
    ComPtr<IDxcBlob> code;
    if (FAILED(dxc.assembler->AssembleToContainer(modified.Get(), &assembled)) || !succeeded(assembled.Get()) ||
        FAILED(assembled->GetResult(&code)) || !code || code->GetBufferSize() > 4 * 1024 * 1024)
        return fail("shader-assembly-failed");
    if (FAILED(dxc.validator->Validate(code.Get(), DxcValidatorFlags_InPlaceEdit, &validated)) || !succeeded(validated.Get()))
        return fail("shader-validation-failed");
    const auto *bytes = static_cast<const unsigned char *>(code->GetBufferPointer());
    out.assign(bytes, bytes + code->GetBufferSize());
    return true;
}
inline bool BuildRuntimeShader(const void *native, size_t size, HMODULE module,
                               std::vector<unsigned char> &out, const char *&status) noexcept {
    out.clear();
    status = "unsupported-subsurface-scattering-shader";
    const auto *known = KnownVariant(native, size);
    if (!known && !StructuralCandidate(native, size))
        return false;
    try {
        using Microsoft::WRL::ComPtr;
        status = "shader-compiler-unavailable";
        ShaderTools dxc;
        if (!dxc.Create(module))
            return false;
        std::string text;
        status = "shader-disassembly-failed";
        if (!dxc.Disassemble(native, size, text))
            return false;
        // Captured builds must match too, so the structure check is proven on them.
        status = "unsupported-subsurface-scattering-structure";
        if (!StructureMatches(text, InputStructure))
            return false;
        status = "shader-transformation-rejected";
        if (!TransformRuntimeShader(text))
            return false;
        std::vector<unsigned char> code;
        if (!AssembleShader(dxc, text, code, &status))
            return false;
        // Pins the complete validated output, including original instructions,
        // bindings and output signatures, to the tested transformation result:
        // byte for byte for a captured build, structurally for another build.
        status = "shader-output-mismatch";
        if (known && (code.size() != known->output_bytes ||
                      !ShaderDigest(code.data(), code.size(), known->output_sha256)))
            return false;
        std::string generated;
        if (!dxc.Disassemble(code.data(), code.size(), generated) ||
            !StructureMatches(generated, OutputStructure))
            return false;
        out = std::move(code);
        status = "ready";
        return true;
    } catch (...) {
        out.clear();
        status = "shader-preparation-exception";
        return false;
    }
}
// Builds accepted at runtime are identified at draws by FNV-1a64 and size,
// like the captured builds. Reads take no lock: draws check this per call.
class RecognizedShaders {
    static constexpr unsigned Capacity = 16;
    std::mutex mutex;
    std::array<std::atomic<uint64_t>, Capacity> hashes{};
    std::array<std::atomic<size_t>, Capacity> sizes{};
    std::atomic<unsigned> count{};
public:
    void Add(uint64_t hash, size_t bytes) {
        std::lock_guard lock(mutex);
        const auto n = count.load(std::memory_order_relaxed);
        if (Has(hash, bytes) || n >= Capacity)
            return;
        hashes[n].store(hash, std::memory_order_relaxed);
        sizes[n].store(bytes, std::memory_order_relaxed);
        count.store(n + 1, std::memory_order_release);
    }
    bool Has(uint64_t hash, size_t bytes) const {
        if (KnownVariant(hash, bytes))
            return true;
        const auto n = count.load(std::memory_order_acquire);
        for (unsigned i = 0; i < n; ++i)
            if (hashes[i].load(std::memory_order_relaxed) == hash &&
                sizes[i].load(std::memory_order_relaxed) == bytes)
                return true;
        return false;
    }
    bool HasHash(uint64_t hash) const {
        for (const auto &v : Variants)
            if (v.hash == hash)
                return true;
        const auto n = count.load(std::memory_order_acquire);
        for (unsigned i = 0; i < n; ++i)
            if (hashes[i].load(std::memory_order_relaxed) == hash)
                return true;
        return false;
    }
};
// One slot per distinct input (SHA-256 and size): an input never receives
// another build's guide, and a failure stays with the input that caused it.
class RuntimeShaderCache {
    struct Slot {
        std::array<unsigned char, 32> digest{};
        size_t size{};
        std::mutex mutex;
        bool attempted{};
        const char *result = "not-prepared";
        std::shared_ptr<const std::vector<unsigned char>> code;
    };
    std::mutex mutex;
    std::vector<std::shared_ptr<Slot>> slots;
    RecognizedShaders *recognized{};
public:
    static constexpr size_t Capacity = 16;
    explicit RuntimeShaderCache(RecognizedShaders *recognized = nullptr) : recognized(recognized) {}
    std::shared_ptr<const std::vector<unsigned char>> Get(const void *native, size_t size,
                                                         HMODULE module, const char *&status) {
        std::array<unsigned char, 32> digest{};
        if ((!KnownVariant(native, size) && !StructuralCandidate(native, size)) ||
            !Sha256(native, size, digest)) {
            status = "unsupported-subsurface-scattering-shader";
            return {};
        }
        std::shared_ptr<Slot> slot;
        {
            std::lock_guard lock(mutex);
            for (const auto &s : slots)
                if (s->size == size && s->digest == digest)
                    slot = s;
            if (!slot) {
                if (slots.size() >= Capacity) {
                    status = "too-many-subsurface-scattering-shaders";
                    return {};
                }
                slot = std::make_shared<Slot>();
                slot->digest = digest;
                slot->size = size;
                slots.push_back(slot);
            }
        }
        std::lock_guard lock(slot->mutex);
        if (!slot->attempted) {
            slot->attempted = true;
            auto prepared = std::make_shared<std::vector<unsigned char>>();
            if (BuildRuntimeShader(native, size, module, *prepared, slot->result)) {
                slot->code = std::move(prepared);
                if (recognized)
                    recognized->Add(ShaderHash(native, size), size);
            }
        }
        status = slot->result;
        return slot->code;
    }
};
inline RecognizedShaders &Recognized() {
    static RecognizedShaders shaders;
    return shaders;
}
// The SSS composite is also the transparency producer. A bound pipeline's
// shader hash (bound) and its native bytecode (observed, bytes) must name the
// same recognized build.
inline bool CompositeShader(uint64_t bound, uint64_t observed, size_t bytes) {
    return observed == bound && Recognized().Has(observed, bytes);
}
inline RuntimeShaderCache &RuntimeShader() {
    static RuntimeShaderCache cache(&Recognized());
    return cache;
}
} // namespace rrsss
