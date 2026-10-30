// Structural recognition of the transparency composites
// (src/rr_transparency_classifier.h, src/rr_transparency_shaders.h).
//
// Without arguments only synthetic inputs are used: made-up containers and
// disassembly text, no game code. With privately supplied files,
//   transparency-classifier.exe <a36d.cso> <b723.cso> <5a24.cso> <534109.cso> <dxcompiler.dll>
// it also checks the shipped fingerprints and rules on the captured builds and
// on in-memory rebuilds of them. --fingerprint <shader> <dll> prints the
// values used to pin a profile. The private files are never distributed.
#include "../src/rr_transparency_coverage_pipeline.h"
#include "memory-cap.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <vector>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::cerr << "FAIL: " #x " (line " << __LINE__ << ")\n";                               \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

using namespace rrblend;
namespace api = reshade::api;

static std::string Hex(const void *data, size_t size) {
    std::array<unsigned char, 32> d{};
    if (!rrsss::Sha256(data, size, d))
        return "unavailable";
    std::string s;
    char b[3];
    for (auto c : d) {
        std::snprintf(b, 3, "%02x", c);
        s += b;
    }
    return s;
}

// A synthetic DXBC container with the given parts (fourcc, payload).
static std::vector<unsigned char>
Container(const std::vector<std::pair<uint32_t, std::string>> &parts) {
    std::vector<unsigned char> b(32 + 4 * parts.size());
    std::memcpy(b.data(), "DXBC", 4);
    const uint32_t n = uint32_t(parts.size());
    std::memcpy(b.data() + 28, &n, 4);
    for (size_t i = 0; i < parts.size(); ++i) {
        const uint32_t at = uint32_t(b.size()), size = uint32_t(parts[i].second.size());
        std::memcpy(b.data() + 32 + 4 * i, &at, 4);
        b.resize(b.size() + 8 + size);
        std::memcpy(b.data() + at, &parts[i].first, 4);
        std::memcpy(b.data() + at + 4, &size, 4);
        std::memcpy(b.data() + at + 8, parts[i].second.data(), size);
    }
    const uint32_t total = uint32_t(b.size());
    std::memcpy(b.data() + 24, &total, 4);
    return b;
}
static uint64_t Fnv(const std::string &s) {
    return rrsss::ShaderHash(s.data(), s.size());
}

static void TestRegistry() {
    RecognizedTargets r;
    // Every pinned build is known without registration, with its kind.
    for (const auto &t : Targets) {
        CHECK(r.Find(t.hash, t.bytes) == &t && r.FindHash(t.hash) == &t);
        CHECK(r.IsOit(t.hash) == (t.kind == Kind::Oit));
    }
    CHECK(!r.Find(Targets[0].hash, Targets[0].bytes + 4) && !r.FindHash(0x1234));
    CHECK(!r.WasSeen(Kind::Oit) && !r.WasSeen(Kind::Refraction));
    // A structural build is found by hash and size, and by hash at a draw.
    const auto *a = r.Add(0x1234, 11600, Kind::Oit);
    CHECK(a && a->hash == 0x1234 && a->bytes == 11600 && a->kind == Kind::Oit);
    CHECK(r.Find(0x1234, 11600) == a && !r.Find(0x1234, 11604) && r.FindHash(0x1234) == a);
    CHECK(r.IsOit(0x1234) && r.Added() == 1);
    // Adding again returns the same entry; another kind for the same identity never.
    CHECK(r.Add(0x1234, 11600, Kind::Oit) == a && !r.Add(0x1234, 11600, Kind::Refraction));
    CHECK(r.Add(Targets[0].hash, Targets[0].bytes, Targets[0].kind) == &Targets[0]);
    CHECK(r.Added() == 1);
    const auto *refr = r.Add(0x5678, 4700, Kind::Refraction);
    CHECK(refr && !r.IsOit(0x5678) && r.FindHash(0x5678) == refr);
    // Entries never move, and a full registry refuses instead of evicting.
    for (uint64_t h = 1; r.Added() < 16; ++h)
        CHECK(r.Add(0x9000 + h, 100, Kind::Oit));
    CHECK(!r.Add(0xffff, 100, Kind::Oit) && r.Find(0x1234, 11600) == a);
    r.Seen(Kind::Oit);
    CHECK(r.WasSeen(Kind::Oit) && !r.WasSeen(Kind::Refraction));
}

static void TestContainerParts() {
    const auto c = Container({{rrsss::SFI0, "feat"}, {rrsss::ISG1, "input"}, {rrsss::OSG1, "out"}});
    const rrsss::PartPin pins[] = {{rrsss::ISG1, Fnv("input")}, {rrsss::OSG1, Fnv("out")}};
    CHECK(rrsss::ContainerParts(c.data(), c.size(), pins, 2));
    const rrsss::PartPin other[] = {{rrsss::ISG1, Fnv("input")}, {rrsss::OSG1, Fnv("OUT")}};
    CHECK(!rrsss::ContainerParts(c.data(), c.size(), other, 2));
    const rrsss::PartPin missing[] = {{rrsss::PSV0, Fnv("x")}};
    CHECK(!rrsss::ContainerParts(c.data(), c.size(), missing, 1));
    // A duplicated pinned part, a wrong total size and truncation are refused.
    const auto twice = Container({{rrsss::ISG1, "input"}, {rrsss::ISG1, "input"}});
    CHECK(!rrsss::ContainerParts(twice.data(), twice.size(), pins, 1));
    auto bad = c;
    bad[24] ^= 1;
    CHECK(!rrsss::ContainerParts(bad.data(), bad.size(), pins, 2));
    CHECK(!rrsss::ContainerParts(c.data(), c.size() - 1, pins, 2));
    CHECK(!rrsss::ContainerParts(nullptr, 0, pins, 2) && !rrsss::ContainerParts(c.data(), c.size(), pins, 0));
    // The SSS composite's filter is the same check on its own pins.
    CHECK(!rrsss::StructuralCandidate(c.data(), c.size()));
}

// Synthetic disassembly with the composite declarations' shape. No game code.
static std::string Text(unsigned bytes, bool codec, const char *debug = "0123",
                        const char *instruction = "fmul fast float %1, 2.0",
                        const char *binding = "i32 0, i32 5, i32 1", const char *output = "SV_Target") {
    std::string t = "; shader debug name: " + std::string(debug) + ".pdb\n; shader hash: " + debug +
                    "\n; cbuffer scene\n;   [" + std::to_string(bytes) +
                    " x i8] (type annotation not present)\n"
                    "%hostlayout.scene = type { %hostlayout.struct.GfxCodeSceneConsts }\n"
                    "%hostlayout.struct.GfxCodeSceneConsts = type { i32, %struct.GfxWind, i32" +
                    std::string(codec ? ", %struct.GfxVelocityCodec, %struct.GfxVelocityCodec" : "") +
                    " }\n%struct.GfxWind = type { <2 x float> }\n";
    if (codec)
        t += "%struct.GfxVelocityCodec = type { <2 x float> }\n";
    t += "\ndefine void @ps_main() {\n  %1 = call float @dx.op.loadInput\n  %2 = " +
         std::string(instruction) + "\n  ret void\n}\n!10 = !{i32 0, %\"class.Texture2DMS\"* undef, !\"\", " +
         binding + ", i32 3, i32 9, null}\n"
         "!14 = !{i32 0, %hostlayout.scene* undef, !\"\", i32 0, i32 3, i32 1, i32 " +
         std::to_string(bytes) + ", null}\n!26 = !{i32 0, !\"" + output +
         "\", i8 9, i8 16, !27, i8 0, i32 1, i8 4, i32 0, i8 0, null}\n";
    return t;
}
static std::string Normalized(const std::string &text, size_t read) {
    std::string n;
    CHECK(rrsss::StructuralText(text, n, read));
    return Hex(n.data(), n.size());
}

static void TestStructureVerdict() {
    const auto pin = Normalized(Text(12672, true), 7712);
    Profile p{Kind::Oit, "oit", 0x1, {}, pin.c_str(), 7712};
    // The 12672- and 12416-byte builds, and one declaring only the bytes read, all match.
    CHECK(!StructureVerdict(p, Text(12672, true)));
    CHECK(!StructureVerdict(p, Text(12416, false, "4567")));
    CHECK(!StructureVerdict(p, Text(7712, false, "89ab")));
    // A scene buffer smaller than the bytes the instructions read is not normalizable.
    CHECK(!strcmp(StructureVerdict(p, Text(7696, false)), "scene-constants-not-normalizable"));
    CHECK(!strcmp(StructureVerdict(p, Text(12420, false)), "scene-constants-not-normalizable"));
    // The SSS read bound is not reused: the same text is only normalizable above 12112.
    std::string n;
    CHECK(!rrsss::StructuralText(Text(7712, false), n));
    // A changed instruction, resource binding or output is a different shader.
    CHECK(!strcmp(StructureVerdict(p, Text(12416, false, "4567", "fadd fast float %1, 2.0")),
                  "structure-mismatch"));
    CHECK(!strcmp(StructureVerdict(p, Text(12416, false, "4567", "fmul fast float %1, 2.0",
                                           "i32 0, i32 6, i32 1")),
                  "structure-mismatch"));
    CHECK(!strcmp(StructureVerdict(p, Text(12416, false, "4567", "fmul fast float %1, 2.0",
                                           "i32 0, i32 5, i32 1", "SV_Depth")),
                  "structure-mismatch"));
    // Extra metadata or a second scene buffer is never normalized away.
    CHECK(!strcmp(StructureVerdict(p, Text(12416, false) + "!99 = !{i32 7}\n"),
                  "structure-mismatch"));
    CHECK(StructureVerdict(p, Text(12416, false) +
                                  "!40 = !{i32 1, %hostlayout.scene* undef, !\"\", i32 0, i32 3, "
                                  "i32 1, i32 12416, null}\n"));
    // A profile without a fingerprint accepts no structure.
    Profile none = p;
    none.structure = "";
    CHECK(!strcmp(StructureVerdict(none, Text(12672, true)), "structure-not-pinned"));
    // The shipped profiles: one per kind, each pinned to a shipped exact target.
    for (const auto &profile : Profiles)
        CHECK(FindTarget(profile.canonical, profile.kind == Kind::Oit ? 11856 : 4812) &&
              profile.read_bytes % 16 == 0 && profile.read_bytes < 12416);
}

// The classifier without a compiler: unrelated shaders are filtered before any
// full hash, a candidate's missing compiler is retried and then given up, and
// the verdict is kept per distinct shader.
static void TestClassifierSynthetic() {
    const auto candidate = Container({{rrsss::ISG1, "sample-index"}, {rrsss::OSG1, "two-targets"}});
    const Profile profile{Kind::Oit,
                          "oit",
                          0x1,
                          {{rrsss::SFI0, Fnv("feat")},
                           {rrsss::ISG1, Fnv("sample-index")},
                           {rrsss::OSG1, Fnv("two-targets")},
                           {rrsss::PSV0, Fnv("runtime")}},
                          "00",
                          16};
    const auto full = Container({{rrsss::SFI0, "feat"},
                                 {rrsss::ISG1, "sample-index"},
                                 {rrsss::OSG1, "two-targets"},
                                 {rrsss::PSV0, "runtime"}});
    RecognizedTargets registry;
    Classifier c(&registry, std::span<const Profile>(&profile, 1));
    std::vector<std::string> lines;
    static std::vector<std::string> *sink;
    sink = &lines;
    c.logger = [](const char *line) { sink->emplace_back(line); };
    CHECK(!CandidateProfile(candidate.data(), candidate.size(), std::span<const Profile>(&profile, 1)));
    auto v = c.Classify(candidate.data(), candidate.size(), nullptr);
    CHECK(!v.target && !strcmp(v.status, "unrelated") && !v.profile);
    CHECK(c.Stats().unrelated == 1 && c.Stats().candidates == 0 && lines.empty());
    for (unsigned i = 0; i < Classifier::Attempts; ++i) {
        v = c.Classify(full.data(), full.size(), nullptr);
        CHECK(!v.target && !strcmp(v.status, "shader-compiler-unavailable"));
        CHECK(v.decided == (i + 1 == Classifier::Attempts));
    }
    const auto s = c.Stats();
    CHECK(s.candidates == 1 && s.refused == 1 && s.retried == Classifier::Attempts - 1);
    CHECK(lines.size() == 1 && lines[0].find("refused as oit build") != std::string::npos &&
          lines[0].find("shader-compiler-unavailable") != std::string::npos);
    // Given up: no further attempt, no further line.
    v = c.Classify(full.data(), full.size(), nullptr);
    CHECK(!v.decided && !strcmp(v.status, "shader-compiler-unavailable") && lines.size() == 1);
    CHECK(c.Stats().candidates == 1 && registry.Added() == 0);
    // The pinned builds are found by their exact identity without any profile.
    std::vector<unsigned char> blob(64);
    CHECK(!strcmp(c.Classify(blob.data(), blob.size(), nullptr).status, "unrelated"));
    CHECK(!strcmp(c.Classify(nullptr, 0, nullptr).status, "invalid-bytecode"));
    // Through pipeline subobjects: only the pixel shader is classified.
    api::shader_desc ps{full.data(), full.size()}, vs{candidate.data(), candidate.size()};
    api::pipeline_subobject objects[] = {{api::pipeline_subobject_type::vertex_shader, 1, &vs},
                                         {api::pipeline_subobject_type::pixel_shader, 1, &ps}};
    uint64_t hash = 7;
    CHECK(!ClassifiedTarget(2, objects, hash, c) && hash == 7);
    CHECK(!ClassifiedTarget(0, objects, hash, c) && !ClassifiedTarget(2, nullptr, hash, c));
}

static std::vector<unsigned char> Read(const wchar_t *path) {
    std::ifstream f(path, std::ios::binary);
    CHECK(f);
    return {std::istreambuf_iterator<char>(f), {}};
}
static std::string Disassemble(HMODULE module, const std::vector<unsigned char> &code) {
    rrsss::ShaderTools dxc;
    std::string text;
    CHECK(dxc.Create(module) && dxc.Disassemble(code.data(), code.size(), text));
    return text;
}
static int Fingerprint(const wchar_t *shader, const wchar_t *dll) {
    const auto code = Read(shader);
    auto module = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    CHECK(module);
    std::cout << "bytes=" << code.size() << " fnv=" << std::hex << rrsss::ShaderHash(code.data(), code.size())
              << std::dec << " sha256=" << Hex(code.data(), code.size()) << '\n';
    const auto text = Disassemble(module, code);
    for (const auto &p : Profiles) {
        std::string n;
        std::cout << p.name << ": parts=" << rrsss::ContainerParts(code.data(), code.size(), p.parts, 4)
                  << " structure="
                  << (rrsss::StructuralText(text, n, p.read_bytes) ? Hex(n.data(), n.size()) : "unavailable")
                  << '\n';
    }
    return 0;
}
// Rebuilds a captured composite with an edited disassembly, in memory only.
static std::vector<unsigned char> Rebuild(HMODULE module, const std::vector<unsigned char> &native,
                                          bool (*edit)(std::string &)) {
    rrsss::ShaderTools dxc;
    std::string text = Disassemble(module, native);
    std::vector<unsigned char> code;
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    CHECK(dxc.Create(module) && edit(text));
    CHECK(rrsss::AssembleShader(dxc, text, code));
    return code;
}
// The tester's build: both unread GfxVelocityCodec members gone, 12416 bytes.
static bool NoCodecs(std::string &t) {
    const std::string codecs = ", %struct.GfxVelocityCodec, %struct.GfxVelocityCodec";
    const auto at = t.find(codecs);
    if (at == std::string::npos)
        return false;
    t.erase(at, codecs.size());
    const auto def = t.find("\n%struct.GfxVelocityCodec = type ");
    if (def == std::string::npos)
        return false;
    t.erase(def, t.find('\n', def + 1) - def);
    return rrsss::ReplaceShaderOnce(t, "i32 1, i32 12672, null}", "i32 1, i32 12416, null}") &&
           rrsss::ReplaceShaderOnce(t, "[12672 x i8]", "[12416 x i8]");
}
static bool ChangedInstruction(std::string &t) {
    const auto at = t.find(" = fmul fast float ", t.find("\ndefine void @"));
    if (at == std::string::npos)
        return false;
    t.replace(at + 3, 4, "fadd");
    return true;
}

static int Private(wchar_t **argv) {
    const auto oit = Read(argv[1]), b723 = Read(argv[2]), r5a24 = Read(argv[3]), other = Read(argv[4]);
    auto module = LoadLibraryExW(argv[5], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    CHECK(module);
    const auto &oit_profile = Profiles[0], &refr_profile = Profiles[1];
    CHECK(oit_profile.kind == Kind::Oit && refr_profile.kind == Kind::Refraction);
    // The captured builds are the pinned targets, and their containers pass their profile's filter.
    CHECK(FindTarget(rrsss::ShaderHash(oit.data(), oit.size()), oit.size())->kind == Kind::Oit);
    CHECK(FindTarget(rrsss::ShaderHash(b723.data(), b723.size()), b723.size())->kind == Kind::Refraction);
    CHECK(FindTarget(rrsss::ShaderHash(r5a24.data(), r5a24.size()), r5a24.size())->kind == Kind::Refraction);
    CHECK(CandidateProfile(oit.data(), oit.size()) == &oit_profile);
    CHECK(CandidateProfile(b723.data(), b723.size()) == &refr_profile);
    CHECK(CandidateProfile(r5a24.data(), r5a24.size()) == &refr_profile);
    // The fingerprints hold on the captured builds; the 12672- and 12416-byte refraction
    // builds normalize to the same structure, which pins the rule on actual evidence.
    CHECK(!StructureVerdict(oit_profile, Disassemble(module, oit)));
    CHECK(!StructureVerdict(refr_profile, Disassemble(module, b723)));
    CHECK(!StructureVerdict(refr_profile, Disassemble(module, r5a24)));
    // Another OIT resolve on this PC (534109, other resources) is not this composite: its
    // runtime-info part differs, and so does its structure.
    CHECK(!CandidateProfile(other.data(), other.size()));
    CHECK(!strcmp(StructureVerdict(oit_profile, Disassemble(module, other)), "structure-mismatch"));
    // The OIT resolve rebuilt as the 12416-byte build is recognized structurally, once,
    // by concurrent pipeline creations, and every later check finds the same identity.
    const auto tester = Rebuild(module, oit, NoCodecs);
    const auto tester_hash = rrsss::ShaderHash(tester.data(), tester.size());
    CHECK(!FindTarget(tester_hash, tester.size()) && CandidateProfile(tester.data(), tester.size()));
    RecognizedTargets registry;
    Classifier c(&registry);
    std::vector<std::thread> threads;
    Classifier::Verdict verdicts[8];
    for (int i = 0; i < 8; ++i)
        threads.emplace_back([&, i] { verdicts[i] = c.Classify(tester.data(), tester.size(), module); });
    for (auto &t : threads)
        t.join();
    unsigned decided = 0;
    for (const auto &v : verdicts) {
        CHECK(v.target && v.target == verdicts[0].target && v.target->kind == Kind::Oit);
        CHECK(v.target->hash == tester_hash && v.target->bytes == tester.size());
        decided += v.decided;
    }
    CHECK(decided == 1 && c.Stats().candidates == 1 && c.Stats().accepted == 1);
    CHECK(registry.IsOit(tester_hash) && registry.Find(tester_hash, tester.size()) == verdicts[0].target);
    // The draw-time checks (guide anchor, both coverage paths) read the shared registry.
    api::shader_desc ps{tester.data(), tester.size()};
    api::pipeline_subobject objects[] = {{api::pipeline_subobject_type::pixel_shader, 1, &ps}};
    uint64_t hash = 0;
    CHECK(ClassifiedTarget(1, objects, hash, c) == verdicts[0].target && hash == tester_hash);
    CHECK(c.Stats().candidates == 1); // reused, not classified again
    // The refraction composite rebuilt with one codec removed is recognized as refraction.
    const auto refr = Rebuild(module, b723, [](std::string &t) {
        const std::string codec = ", %struct.GfxVelocityCodec";
        const auto at = t.rfind(codec);
        if (at == std::string::npos)
            return false;
        t.erase(at, codec.size());
        return rrsss::ReplaceShaderOnce(t, "i32 1, i32 12672, null}", "i32 1, i32 12544, null}");
    });
    auto v = c.Classify(refr.data(), refr.size(), module);
    CHECK(v.target && v.target->kind == Kind::Refraction && !registry.IsOit(v.target->hash));
    // A changed instruction is refused with its reason, and stays refused.
    const auto changed = Rebuild(module, oit, ChangedInstruction);
    v = c.Classify(changed.data(), changed.size(), module);
    CHECK(!v.target && v.decided && !strcmp(v.status, "structure-mismatch"));
    v = c.Classify(changed.data(), changed.size(), module);
    CHECK(!v.target && !v.decided && !registry.Find(rrsss::ShaderHash(changed.data(), changed.size()),
                                                     changed.size()));
    // Flipped bits in the bitcode: the same program again, or refused; never another identity.
    unsigned refused = 0;
    for (unsigned i = 1; i < 8; ++i) {
        auto altered = oit;
        altered[2048 + (altered.size() - 2048) * i / 8] ^= 0x10;
        rrsss::ShaderTools dxc;
        std::string text, normalized, original;
        CHECK(dxc.Create(module) && rrsss::StructuralText(Disassemble(module, oit), original, 7712));
        const bool same = dxc.Disassemble(altered.data(), altered.size(), text) &&
                          rrsss::StructuralText(text, normalized, 7712) && normalized == original;
        v = c.Classify(altered.data(), altered.size(), module);
        CHECK(!!v.target == same);
        refused += !same;
    }
    CHECK(refused >= 3);
    auto truncated = tester;
    truncated.resize(truncated.size() - 4);
    CHECK(!c.Classify(truncated.data(), truncated.size(), module).target);
    FreeLibrary(module);
    std::cout << "PASS: pinned and structural transparency composites verified on the private "
                 "captured builds; no GPU work\n";
    return 0;
}

int wmain(int argc, wchar_t **argv) {
    if (!CapTestMemory())
        return 1;
    TestRegistry();
    TestContainerParts();
    TestStructureVerdict();
    TestClassifierSynthetic();
    if (argc == 4 && std::wstring(argv[1]) == L"--fingerprint")
        return Fingerprint(argv[2], argv[3]);
    if (argc != 6) {
        std::cout << "PASS: synthetic transparency composite checks; private fixtures not supplied\n";
        return 0;
    }
    return Private(argv);
}
