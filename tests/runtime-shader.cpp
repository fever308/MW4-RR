#include "../src/rr_sss_runtime_shader.h"
#include <fstream>
#include <iostream>
#include <cassert>
#include <iterator>
#include <thread>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL: " #x " (line " << __LINE__ << ")\n"; std::exit(1); } } while (0)
static std::vector<unsigned char> Read(const wchar_t *path) {
    std::ifstream f(path, std::ios::binary);
    CHECK(f);
    return {std::istreambuf_iterator<char>(f), {}};
}
static std::string Hex(const void *data, size_t size) {
    std::array<unsigned char, 32> d{};
    if (!rrsss::Sha256(data, size, d))
        return "unavailable";
    std::string s;
    char b[3];
    for (auto c : d) { std::snprintf(b, 3, "%02x", c); s += b; }
    return s;
}
// Synthetic disassembly with the shape of the composite's declarations. No
// game code is involved.
static std::string Synthetic(unsigned bytes, bool codec, const char *debug = "0123",
                             const char *instruction = "fmul fast float %1, 2.0") {
    std::string t = "; shader debug name: " + std::string(debug) + ".pdb\n; shader hash: " + debug +
        "\n; cbuffer scene\n;   [" + std::to_string(bytes) + " x i8] (type annotation not present)\n"
        "; cbuffer stable\n;   [144 x i8] (type annotation not present)\n"
        "%hostlayout.scene = type { %hostlayout.struct.GfxCodeSceneConsts }\n"
        "%hostlayout.struct.GfxCodeSceneConsts = type { i32, %struct.GfxWind, i32" +
        std::string(codec ? ", %struct.GfxVelocityCodec" : "") + " }\n"
        "%struct.GfxWind = type { <2 x float>, %struct.GfxWindInner }\n"
        "%struct.GfxWindInner = type { i32 }\n";
    if (codec)
        t += "%struct.GfxVelocityCodec = type { <2 x float> }\n";
    t += "%struct.Used = type { i32 }\n\ndefine void @ps_main() {\n  %1 = load %struct.Used\n  %2 = " +
         std::string(instruction) + "\n  ret void\n}\n"
         "!38 = !{i32 0, %hostlayout.scene* undef, !\"\", i32 0, i32 3, i32 1, i32 " +
         std::to_string(bytes) + ", null}\n"
         "!39 = !{i32 1, %hostlayout.stable* undef, !\"\", i32 0, i32 2, i32 1, i32 144, null}\n";
    return t;
}
static void SyntheticStructure() {
    std::string a, b, c;
    CHECK(rrsss::StructuralText(Synthetic(12672, true), a));
    CHECK(rrsss::StructuralText(Synthetic(12416, false, "4567"), b));
    CHECK(a == b);
    CHECK(a.find("%struct.Used") != std::string::npos);
    CHECK(a.find("GfxVelocityCodec") == std::string::npos && a.find("GfxWind") == std::string::npos);
    CHECK(a.find("144") != std::string::npos);
    CHECK(rrsss::StructuralText(Synthetic(12112, false), c) && c == a);
    // A changed instruction or a too small scene buffer is never equivalent.
    CHECK(rrsss::StructuralText(Synthetic(12416, false, "4567", "fmul fast float %1, 3.0"), c) && c != a);
    CHECK(!rrsss::StructuralText(Synthetic(12096, false), c));
    CHECK(!rrsss::StructuralText(Synthetic(12420, false), c));
    auto text = Synthetic(12416, false);
    CHECK(!rrsss::StructuralText(text + "; shader hash: 89ab\n", c));
    CHECK(!rrsss::StructuralText(text + "!40 = !{i32 0, %hostlayout.scene* undef, !\"\", i32 0, i32 3, i32 1, i32 12416, null}\n", c));
    auto other = text;
    CHECK(rrsss::ReplaceShaderOnce(other, "i32 144, null", "i32 160, null"));
    CHECK(rrsss::StructuralText(other, c) && c != a);
    other = text;
    CHECK(rrsss::ReplaceShaderOnce(other, "i32 3, i32 1, i32 12416", "i32 4, i32 1, i32 12416"));
    CHECK(!rrsss::StructuralText(other, c));
    other = text;
    CHECK(rrsss::ReplaceShaderOnce(other, "%hostlayout.struct.GfxCodeSceneConsts = type", "%hostlayout.struct.Other = type"));
    CHECK(!rrsss::StructuralText(other, c));
    // Unrelated and truncated containers are not candidates.
    std::vector<unsigned char> blob(64);
    CHECK(!rrsss::StructuralCandidate(blob.data(), blob.size()));
    std::memcpy(blob.data(), "DXBC", 4);
    const uint32_t size = 64, parts = 40;
    std::memcpy(blob.data() + 24, &size, 4);
    std::memcpy(blob.data() + 28, &parts, 4);
    CHECK(!rrsss::StructuralCandidate(blob.data(), blob.size()));
    rrsss::RecognizedShaders known;
    for (const auto &v : rrsss::Variants)
        CHECK(known.Has(v.hash, v.bytes) && known.HasHash(v.hash));
    CHECK(!known.Has(rrsss::Variants[0].hash, rrsss::Variants[1].bytes));
    CHECK(!known.HasHash(0x1234));
    known.Add(0x1234, 100);
    CHECK(known.Has(0x1234, 100) && known.HasHash(0x1234) && !known.Has(0x1234, 101));
    const auto &first = rrsss::Variants[0];
    CHECK(rrsss::CompositeShader(first.hash, first.hash, first.bytes));
    CHECK(!rrsss::CompositeShader(rrsss::Variants[1].hash, first.hash, first.bytes));
    CHECK(!rrsss::CompositeShader(first.hash, first.hash, first.bytes + 4));
}
struct Built {
    std::vector<unsigned char> code;
    std::string input, output;
};
// Disassembles, transforms, assembles and validates without the shipped pins,
// so that a new build's fingerprints can be measured.
static bool Measure(HMODULE module, const std::vector<unsigned char> &native, Built &b) {
    rrsss::ShaderTools dxc;
    if (!dxc.Create(module) || !dxc.Disassemble(native.data(), native.size(), b.input))
        return false;
    auto text = b.input;
    if (!rrsss::TransformRuntimeShader(text))
        return false;
    return rrsss::AssembleShader(dxc, text, b.code) &&
           dxc.Disassemble(b.code.data(), b.code.size(), b.output);
}
static std::string Body(const std::string &text) {
    std::string t = text;
    t.erase(std::remove(t.begin(), t.end(), '\r'), t.end());
    const auto at = t.find("\ndefine void @ps_main()");
    const auto end = at == std::string::npos ? at : t.find("\n}\n", at);
    if (end == std::string::npos)
        return {};
    // Reassembly renumbers metadata nodes and annotates each dx.op call with a
    // comment, so attachments such as !tbaa are compared without their ids and
    // comments are dropped. Instructions and values stay exact.
    static const std::regex id(R"(!\d+\b)"), comment(R"([ \t]+;[^\n]*)");
    return std::regex_replace(std::regex_replace(t.substr(at, end - at), comment, ""), id, "!N");
}
static int Fingerprint(const wchar_t *shader, const wchar_t *dll) {
    auto native = Read(shader);
    auto module = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    CHECK(module);
    Built b;
    std::string in, out;
    std::cout << "input bytes=" << native.size() << " fnv=" << std::hex << rrsss::ShaderHash(native.data(), native.size())
              << std::dec << " sha256=" << Hex(native.data(), native.size())
              << " candidate=" << rrsss::StructuralCandidate(native.data(), native.size()) << '\n';
    if (!Measure(module, native, b) || !rrsss::StructuralText(b.input, in) || !rrsss::StructuralText(b.output, out)) {
        std::cout << "measurement failed\n";
        return 1;
    }
    std::cout << "input structure=" << Hex(in.data(), in.size()) << "\noutput bytes=" << b.code.size()
              << " sha256=" << Hex(b.code.data(), b.code.size()) << "\noutput structure=" << Hex(out.data(), out.size()) << '\n';
    return 0;
}
// Rebuilds a captured composite with an edited disassembly, in memory only.
static std::vector<unsigned char> Rebuild(HMODULE module, const std::vector<unsigned char> &native,
                                          bool (*edit)(std::string &)) {
    rrsss::ShaderTools dxc;
    std::string text;
    std::vector<unsigned char> code;
    CHECK(dxc.Create(module) && dxc.Disassemble(native.data(), native.size(), text));
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    CHECK(edit(text));
    CHECK(rrsss::AssembleShader(dxc, text, code));
    return code;
}
static bool OneCodec(std::string &t) {
    // A build that declares one of the two unread GfxVelocityCodec members.
    const std::string codec = ", %struct.GfxVelocityCodec";
    const auto at = t.rfind(codec);
    if (at == std::string::npos)
        return false;
    t.erase(at, codec.size());
    return rrsss::ReplaceShaderOnce(t, "i32 1, i32 12672, null}", "i32 1, i32 12544, null}");
}
static bool ChangedInstruction(std::string &t) {
    // Turns the function body's first multiply into an add.
    const auto at = t.find(" = fmul fast float ", t.find("\ndefine void @ps_main()"));
    if (at == std::string::npos)
        return false;
    t.replace(at + 3, 4, "fadd");
    return true;
}
int wmain(int argc, wchar_t **argv) {
    std::vector<unsigned char> out{1}; const char *status = "";
    CHECK(!rrsss::BuildRuntimeShader(nullptr, 0, nullptr, out, status) && out.empty());
    SyntheticStructure();
    if (argc == 4 && std::wstring(argv[1]) == L"--fingerprint")
        return Fingerprint(argv[2], argv[3]);
    if (argc != 5) { std::cout << "PASS: invalid shader and synthetic structure checks; private fixtures not supplied\n"; return 0; }
    auto supported = Read(argv[1]), expected = Read(argv[2]), variant = Read(argv[3]);
    auto module = LoadLibraryExW(argv[4], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    CHECK(module);
    const auto *a_pin = rrsss::KnownVariant(supported.data(), supported.size());
    const auto *b_pin = rrsss::KnownVariant(variant.data(), variant.size());
    CHECK(a_pin == &rrsss::Variants[0] && b_pin == &rrsss::Variants[1]);
    std::vector<unsigned char> a, b;
    if (!rrsss::BuildRuntimeShader(supported.data(), supported.size(), module, a, status)) {
        std::cerr << "FAIL: supported build not transformed: " << status << '\n'; return 1;
    }
    CHECK(a == expected);
    if (!rrsss::BuildRuntimeShader(variant.data(), variant.size(), module, b, status)) {
        std::cerr << "FAIL: player build not transformed: " << status << '\n'; return 1;
    }
    CHECK(b.size() == b_pin->output_bytes && a != b);
    // Each guide is its own input's instructions plus only the guide output.
    for (auto *p : {&supported, &variant}) {
        Built m;
        CHECK(Measure(module, *p, m));
        auto transformed = m.input;
        CHECK(rrsss::TransformRuntimeShader(transformed));
        CHECK(!Body(m.output).empty() && Body(m.output) == Body(transformed));
        const auto original = Body(m.input), guide = Body(m.output);
        CHECK(guide.size() > original.size());
        CHECK(m.code == (p == &supported ? a : b));
        std::string in, gen;
        CHECK(rrsss::StructuralText(m.input, in) && rrsss::StructuralText(m.output, gen));
        // The guide leaves the input's own scene data declaration in place.
        CHECK(m.output.find(p == &supported ? "i32 12672, null}" : "i32 12416, null}") != std::string::npos);
    }
    // Mixed concurrent requests: one guide per input, built once each.
    rrsss::RecognizedShaders registry;
    rrsss::RuntimeShaderCache cache(&registry);
    std::shared_ptr<const std::vector<unsigned char>> got[6];
    std::vector<std::thread> threads;
    for (int i = 0; i < 6; ++i)
        threads.emplace_back([&, i] {
            const char *s = "";
            auto &input = i % 2 ? variant : supported;
            got[i] = cache.Get(input.data(), input.size(), module, s);
        });
    for (auto &t : threads) t.join();
    for (int i = 0; i < 6; ++i) {
        CHECK(got[i] && *got[i] == (i % 2 ? b : a));
        CHECK(got[i] == got[i % 2]);
    }
    // A failure stays with its input.
    rrsss::RuntimeShaderCache isolated;
    CHECK(!isolated.Get(supported.data(), supported.size(), nullptr, status));
    CHECK(std::string(status) == "shader-compiler-unavailable");
    CHECK(isolated.Get(variant.data(), variant.size(), module, status) && std::string(status) == "ready");
    CHECK(!isolated.Get(supported.data(), supported.size(), module, status));
    // An unseen build that differs only in the unread scene tail is accepted
    // structurally, prepared, and recognized for SSS and transparency draws.
    auto unseen = Rebuild(module, supported, OneCodec);
    const auto unseen_hash = rrsss::ShaderHash(unseen.data(), unseen.size());
    CHECK(!rrsss::KnownVariant(unseen.data(), unseen.size()));
    CHECK(rrsss::StructuralCandidate(unseen.data(), unseen.size()));
    CHECK(!registry.HasHash(unseen_hash));
    auto c = cache.Get(unseen.data(), unseen.size(), module, status);
    if (!c) { std::cerr << "FAIL: unseen equivalent build refused: " << status << '\n'; return 1; }
    CHECK(registry.Has(unseen_hash, unseen.size()) && registry.HasHash(unseen_hash));
    CHECK(*c != a && *c != b);
    {
        Built m, original;
        CHECK(Measure(module, unseen, m) && Measure(module, supported, original));
        CHECK(Body(m.output) == Body(original.output) && m.code == *c);
        CHECK(m.output.find("i32 12544, null}") != std::string::npos);
    }
    // A changed instruction, an altered container or a recompiled unrelated
    // shader with the same signatures is refused.
    auto changed = Rebuild(module, supported, ChangedInstruction);
    CHECK(rrsss::StructuralCandidate(changed.data(), changed.size()));
    CHECK(!cache.Get(changed.data(), changed.size(), module, status));
    CHECK(std::string(status) == "unsupported-subsurface-scattering-structure");
    CHECK(!registry.HasHash(rrsss::ShaderHash(changed.data(), changed.size())));
    for (auto *p : {&supported, &variant}) {
        // Flipped bits inside the DXIL bitcode. Some bits decode to the same
        // program; such a copy must get the same guide, every other one is refused.
        std::string original, decoded;
        rrsss::ShaderTools dxc;
        CHECK(dxc.Create(module) && dxc.Disassemble(p->data(), p->size(), decoded) &&
              rrsss::StructuralText(decoded, original));
        unsigned refused = 0, refused_cached = 0;
        for (unsigned i = 1; i < 16; ++i) {
            auto altered = *p;
            altered[2048 + (altered.size() - 2048) * i / 16] ^= 0x10;
            std::string normalized;
            const bool same = dxc.Disassemble(altered.data(), altered.size(), decoded) &&
                              rrsss::StructuralText(decoded, normalized) && normalized == original;
            const bool built = rrsss::BuildRuntimeShader(altered.data(), altered.size(), module, out, status);
            CHECK(built == same && (!built || out == (p == &supported ? a : b)) && (built || out.empty()));
            if (!same && refused_cached++ < 2) {
                CHECK(!cache.Get(altered.data(), altered.size(), module, status));
                CHECK(!registry.Has(rrsss::ShaderHash(altered.data(), altered.size()), altered.size()));
            }
            refused += !same;
        }
        CHECK(refused >= 3);
        // The bitcode's trailing zero padding does not change the program: it
        // loses the exact pin but is still the same shader structurally.
        auto padded = *p;
        CHECK(padded.back() == 0);
        padded.back() ^= 1;
        CHECK(!rrsss::KnownVariant(padded.data(), padded.size()));
        auto same = cache.Get(padded.data(), padded.size(), module, status);
        CHECK(same && *same == (p == &supported ? a : b));
        auto truncated = *p;
        truncated.resize(truncated.size() - 4);
        CHECK(!cache.Get(truncated.data(), truncated.size(), module, status));
    }
    out = {1};
    CHECK(!rrsss::BuildRuntimeShader(supported.data(), supported.size(), nullptr, out, status) && out.empty());
    FreeLibrary(module);
    std::cout << "PASS: both captured builds produce their pinned guides; mixed concurrent cache reuse, per-input failure isolation, "
                 "structural acceptance of an equivalent build and refusal of altered builds verified; no GPU work\n";
}
