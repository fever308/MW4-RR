// Synthetic tests of the local shader cache and the startup timing. No game
// shader, compiler or GPU is involved: preparation and acceptance are fakes,
// and every file lives in a fresh directory under %TEMP%.
#include "../src/rr_sss_runtime_shader.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <thread>
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL: " #x " (line " << __LINE__ << ")\n"; std::exit(1); } } while (0)
using rrshadercache::Digest;
using rrshadercache::DiskCache;
using rrshadercache::Identity;
using rrshadercache::Outcome;

static std::wstring TempDir(const wchar_t *name) {
    wchar_t temp[MAX_PATH + 1]{};
    CHECK(GetTempPathW(MAX_PATH, temp));
    std::wstring dir = std::wstring(temp) + L"mw4-rr-cache-test-" +
                       std::to_wstring(GetCurrentProcessId()) + L"-" + name;
    return dir;
}
static void RemoveTree(const std::wstring &dir) {
    WIN32_FIND_DATAW d;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &d);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = d.cFileName;
            if (n == L"." || n == L"..")
                continue;
            if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveTree(dir + L"\\" + n);
            else
                DeleteFileW((dir + L"\\" + n).c_str());
        } while (FindNextFileW(f, &d));
        FindClose(f);
    }
    RemoveDirectoryW(dir.c_str());
}
static std::vector<std::wstring> Files(const std::wstring &dir, const wchar_t *pattern) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW d;
    HANDLE f = FindFirstFileW((dir + L"\\" + pattern).c_str(), &d);
    if (f == INVALID_HANDLE_VALUE)
        return out;
    do
        out.push_back(dir + L"\\" + d.cFileName);
    while (FindNextFileW(f, &d));
    FindClose(f);
    return out;
}
static std::vector<unsigned char> ReadBytes(const std::wstring &path) {
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    CHECK(f);
    return {std::istreambuf_iterator<char>(f), {}};
}
static void WriteBytes(const std::wstring &path, const std::vector<unsigned char> &b) {
    std::ofstream f(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    CHECK(f);
    f.write(reinterpret_cast<const char *>(b.data()), std::streamsize(b.size()));
    CHECK(f.good());
}
// A DXBC-shaped payload: magic and total size, then a pattern.
static std::vector<unsigned char> Shader(size_t size, unsigned char seed) {
    std::vector<unsigned char> b(size);
    for (size_t i = 0; i < size; ++i)
        b[i] = static_cast<unsigned char>(seed + i * 7);
    std::memcpy(b.data(), "DXBC", 4);
    const uint32_t n = uint32_t(size);
    std::memcpy(b.data() + 24, &n, 4);
    return b;
}
static Identity Id(unsigned char input, const char *compiler = "compiler-a:1",
                   const char *transform = "t1") {
    Identity id;
    id.role = "test-role";
    id.transform = transform;
    id.compiler = compiler;
    id.input.fill(input);
    id.input_size = 1000 + input;
    return id;
}
struct Fake {
    std::atomic<int> builds{}, validations{};
    std::vector<unsigned char> result;
    bool build_ok = true, accept = true;
    const char *reject = "rejected-by-role";
    bool Run(DiskCache *cache, const Identity &id, std::vector<unsigned char> &out, Outcome &o,
             rrtiming::Timing &t, const char **status_out = nullptr) {
        const char *status = "";
        const bool ok = rrshadercache::Prepare(
            cache, &id, out, status, o,
            [&](std::vector<unsigned char> &code, const char *&why) {
                ++builds;
                if (!build_ok) {
                    why = "shader-compiler-unavailable";
                    return false;
                }
                code = result;
                why = "ready";
                return true;
            },
            [&](const std::vector<unsigned char> &code, const char *&why) {
                ++validations;
                why = accept && code == result ? "ready" : reject;
                return accept && code == result;
            },
            [](const char *why) { return rrsss::CachedRejectionTransient(why); }, nullptr, &t);
        if (status_out)
            *status_out = status;
        return ok;
    }
};
static std::wstring EntryPath(const DiskCache &cache, const Identity &id) {
    Digest key{};
    CHECK(rrshadercache::Key(id, key));
    const auto h = rrshadercache::Hex(key);
    return cache.Directory() + L"\\" + std::wstring(h.begin(), h.end()) + L".rrsc";
}

static void Keys() {
    Digest a{}, b{};
    CHECK(rrshadercache::Key(Id(1), a) && rrshadercache::Key(Id(1), b) && a == b);
    for (const auto &other : {Id(2), Id(1, "compiler-b:1"), Id(1, "compiler-a:1", "t2")}) {
        CHECK(rrshadercache::Key(other, b) && a != b);
    }
    auto role = Id(1);
    role.role = "other-role";
    CHECK(rrshadercache::Key(role, b) && a != b);
    auto size = Id(1);
    size.input_size += 1;
    CHECK(rrshadercache::Key(size, b) && a != b);
    // Without a compiler identity there is no key, so nothing is cached.
    CHECK(!rrshadercache::Key(Id(1, ""), b));
}
static void HitMissAndInvalidation() {
    const auto dir = TempDir(L"hit");
    RemoveTree(dir);
    rrtiming::Timing t;
    Fake fake;
    fake.result = Shader(4096, 3);
    std::vector<unsigned char> out;
    {
        DiskCache cache(dir);
        Outcome o;
        CHECK(fake.Run(&cache, Id(1), out, o, t) && out == fake.result);
        CHECK(!std::strcmp(o.cache, "miss") && !std::strcmp(o.store, "stored"));
        CHECK(fake.builds == 1 && fake.validations == 0);
        CHECK(Files(dir, L"*.rrsc").size() == 1 && Files(dir, L"*.tmp").empty());
    }
    {
        // A new instance, as in the next session: the stored result is accepted again.
        DiskCache cache(dir);
        Outcome o;
        CHECK(fake.Run(&cache, Id(1), out, o, t) && out == fake.result);
        CHECK(!std::strcmp(o.cache, "hit") && fake.builds == 1 && fake.validations == 1);
        // Another compiler, transformation or input is another entry.
        for (const auto &id : {Id(1, "compiler-b:1"), Id(1, "compiler-a:1", "t2"), Id(2)}) {
            Outcome m;
            CHECK(fake.Run(&cache, id, out, m, t) && !std::strcmp(m.cache, "miss"));
        }
        CHECK(fake.builds == 4 && Files(dir, L"*.rrsc").size() == 4);
    }
    CHECK(t.cache_hits == 1 && t.cache_misses == 4 && t.cache_stores == 4);
    RemoveTree(dir);
}
static void CorruptTruncatedAndMiskeyed() {
    const auto dir = TempDir(L"corrupt");
    RemoveTree(dir);
    rrtiming::Timing t;
    Fake fake;
    fake.result = Shader(2048, 9);
    DiskCache cache(dir);
    std::vector<unsigned char> out;
    Outcome o;
    CHECK(fake.Run(&cache, Id(1), out, o, t));
    const auto path = EntryPath(cache, Id(1));
    const auto good = ReadBytes(path);
    CHECK(good.size() == rrshadercache::HeaderBytes + fake.result.size());
    struct Case {
        const char *why;
        std::vector<unsigned char> bytes;
    };
    std::vector<Case> cases;
    auto flipped = good;
    flipped[rrshadercache::HeaderBytes + 100] ^= 1;
    cases.push_back({"corrupt", flipped});
    cases.push_back({"truncated", {good.begin(), good.end() - 1}});
    cases.push_back({"truncated", {good.begin(), good.begin() + 40}});
    auto extended = good;
    extended.push_back(0);
    cases.push_back({"truncated", extended});
    auto format = good;
    format[4] ^= 0x80;
    cases.push_back({"incompatible", format});
    auto magic = good;
    magic[0] = 'X';
    cases.push_back({"incompatible", magic});
    auto input = good;
    input[48] ^= 1; // inside the recorded input digest
    cases.push_back({"wrong-input", input});
    for (const auto &c : cases) {
        WriteBytes(path, c.bytes);
        Outcome bad;
        const int before = fake.builds;
        CHECK(fake.Run(&cache, Id(1), out, bad, t) && out == fake.result);
        if (std::strcmp(bad.cache, "invalid") || std::strcmp(bad.invalid, c.why)) {
            std::cerr << "expected invalid:" << c.why << ", got " << bad.cache << ":"
                      << (bad.invalid ? bad.invalid : "") << '\n';
            CHECK(false);
        }
        // Rebuilt and stored again: the next request is a hit.
        CHECK(fake.builds == before + 1 && ReadBytes(path) == good);
        Outcome again;
        CHECK(fake.Run(&cache, Id(1), out, again, t) && !std::strcmp(again.cache, "hit"));
    }
    // An intact entry under another entry's name.
    Outcome other;
    CHECK(fake.Run(&cache, Id(2), out, other, t));
    WriteBytes(EntryPath(cache, Id(2)), good);
    Outcome wrong;
    CHECK(fake.Run(&cache, Id(2), out, wrong, t) && !std::strcmp(wrong.invalid, "wrong-key"));
    // Intact bytes that are not a shader container.
    Fake plain;
    plain.result = std::vector<unsigned char>(300, 0x41);
    Outcome first, second;
    CHECK(plain.Run(&cache, Id(3), out, first, t) && !std::strcmp(first.store, "stored"));
    CHECK(plain.Run(&cache, Id(3), out, second, t) && !std::strcmp(second.invalid, "not-a-shader"));
    CHECK(plain.builds == 2);
    RemoveTree(dir);
}
static void AcceptanceIsRequired() {
    const auto dir = TempDir(L"accept");
    RemoveTree(dir);
    rrtiming::Timing t;
    Fake fake;
    fake.result = Shader(1024, 5);
    DiskCache cache(dir);
    std::vector<unsigned char> out;
    Outcome o;
    CHECK(fake.Run(&cache, Id(1), out, o, t));
    // An intact file the role refuses is never used: it is removed and rebuilt.
    fake.accept = false;
    Outcome refused;
    CHECK(fake.Run(&cache, Id(1), out, refused, t) && out == fake.result);
    CHECK(!std::strcmp(refused.cache, "rejected") && !std::strcmp(refused.invalid, "rejected-by-role"));
    CHECK(fake.builds == 2 && !std::strcmp(refused.store, "stored"));
    // A replaced payload with a matching checksum is still refused by the role.
    fake.accept = true;
    const auto forged = Shader(1024, 77);
    const char *why = "";
    CHECK(cache.Put([&] { Digest k{}; CHECK(rrshadercache::Key(Id(1), k)); return k; }(), Id(1), forged, why));
    Outcome checked;
    CHECK(fake.Run(&cache, Id(1), out, checked, t) && out == fake.result);
    CHECK(!std::strcmp(checked.cache, "rejected") && fake.builds == 3);
    // A refusal for a transient reason keeps the entry for a later session.
    CHECK(cache.Put([&] { Digest k{}; CHECK(rrshadercache::Key(Id(1), k)); return k; }(), Id(1), forged, why));
    fake.reject = "shader-compiler-unavailable";
    fake.build_ok = false;
    Outcome transient;
    const char *status = "";
    CHECK(!fake.Run(&cache, Id(1), out, transient, t, &status) && out.empty());
    CHECK(!std::strcmp(transient.cache, "rejected") && !std::strcmp(status, "shader-compiler-unavailable"));
    CHECK(ReadBytes(EntryPath(cache, Id(1))).size() == rrshadercache::HeaderBytes + forged.size());
    RemoveTree(dir);
}
static void FailuresAreNotCached() {
    const auto dir = TempDir(L"fail");
    RemoveTree(dir);
    rrtiming::Timing t;
    Fake fake;
    fake.result = Shader(512, 1);
    fake.build_ok = false;
    DiskCache cache(dir);
    std::vector<unsigned char> out;
    for (int i = 0; i < 2; ++i) {
        Outcome o;
        CHECK(!fake.Run(&cache, Id(1), out, o, t) && !std::strcmp(o.store, "not-stored"));
    }
    CHECK(fake.builds == 2 && Files(dir, L"*.rrsc").empty());
    RemoveTree(dir);
}
static void WriteFailureFallsBack() {
    // The cache directory's path is taken by a file: nothing can be stored,
    // and preparation still succeeds every time.
    const auto file = TempDir(L"blocked");
    RemoveTree(file);
    DeleteFileW(file.c_str());
    WriteBytes(file, {1, 2, 3});
    rrtiming::Timing t;
    Fake fake;
    fake.result = Shader(512, 2);
    DiskCache cache(file + L"\\cache");
    std::vector<unsigned char> out;
    for (int i = 0; i < 2; ++i) {
        Outcome o;
        CHECK(fake.Run(&cache, Id(1), out, o, t) && out == fake.result);
        CHECK(!std::strcmp(o.cache, "miss") && !std::strcmp(o.store, "directory-unavailable"));
    }
    CHECK(t.cache_store_failures == 2 && t.cache_stores == 0);
    DeleteFileW(file.c_str());
    // No profile directory at all: no cache, normal preparation.
    DiskCache none(L"");
    Outcome o;
    CHECK(fake.Run(&none, Id(1), out, o, t) && out == fake.result);
    CHECK(!std::strcmp(o.cache, "load-failed") && t.cache_load_failures == 1);
    // No compiler identity: no key and no cache.
    Outcome unkeyed;
    CHECK(fake.Run(&none, Id(1, ""), out, unkeyed, t) && !std::strcmp(unkeyed.cache, "unavailable"));
}
static void Concurrent() {
    const auto dir = TempDir(L"concurrent");
    RemoveTree(dir);
    rrtiming::Timing t;
    Fake fake;
    fake.result = Shader(64 * 1024, 4);
    // Two instances stand for two processes sharing one directory.
    DiskCache a(dir), b(dir);
    std::vector<std::thread> threads;
    std::atomic<int> good{};
    for (int i = 0; i < 16; ++i)
        threads.emplace_back([&, i] {
            std::vector<unsigned char> out;
            Outcome o;
            for (int k = 0; k < 8; ++k)
                if (fake.Run(i % 2 ? &a : &b, Id(static_cast<unsigned char>(1 + i % 3)), out, o, t) &&
                    out == fake.result)
                    ++good;
        });
    for (auto &th : threads)
        th.join();
    CHECK(good == 16 * 8);
    CHECK(Files(dir, L"*.tmp").empty() && Files(dir, L"*.rrsc").size() == 3);
    for (unsigned char i = 1; i <= 3; ++i) {
        DiskCache fresh(dir);
        std::vector<unsigned char> out;
        Outcome o;
        CHECK(fake.Run(&fresh, Id(i), out, o, t) && !std::strcmp(o.cache, "hit"));
    }
    RemoveTree(dir);
}
static void Bounded() {
    const auto dir = TempDir(L"bounded");
    RemoveTree(dir);
    rrtiming::Timing t;
    DiskCache cache(dir);
    std::vector<unsigned char> out;
    for (unsigned i = 0; i < rrshadercache::MaxEntries + 8; ++i) {
        Fake fake;
        fake.result = Shader(1024, static_cast<unsigned char>(i));
        Outcome o;
        CHECK(fake.Run(&cache, Id(static_cast<unsigned char>(i + 1)), out, o, t));
        Sleep(2); // distinct write times
    }
    CHECK(Files(dir, L"*.rrsc").size() == rrshadercache::MaxEntries);
    // The newest entry survives.
    CHECK(GetFileAttributesW(EntryPath(cache, Id(static_cast<unsigned char>(rrshadercache::MaxEntries + 8))).c_str()) !=
          INVALID_FILE_ATTRIBUTES);
    // Too large an entry is never written.
    const char *why = "";
    Digest key{};
    CHECK(rrshadercache::Key(Id(200), key));
    CHECK(!cache.Put(key, Id(200), std::vector<unsigned char>(rrshadercache::MaxPayload + 1), why));
    // An interrupted write's leftover is removed once it is old; a fresh one is kept.
    const auto stale = dir + L"\\" + std::wstring(64, L'a') + L".1.1.1.tmp";
    const auto fresh = dir + L"\\" + std::wstring(64, L'b') + L".1.1.1.tmp";
    WriteBytes(stale, {1});
    WriteBytes(fresh, {1});
    HANDLE h = CreateFileW(stale.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    FILETIME old{};
    GetSystemTimeAsFileTime(&old);
    ULARGE_INTEGER v{{old.dwLowDateTime, old.dwHighDateTime}};
    v.QuadPart -= 2ull * 36'000'000'000ull;
    old.dwLowDateTime = v.LowPart;
    old.dwHighDateTime = v.HighPart;
    CHECK(SetFileTime(h, nullptr, nullptr, &old));
    CloseHandle(h);
    // An unrelated file in the directory is never touched.
    const auto unrelated = dir + L"\\notes.txt";
    WriteBytes(unrelated, {1});
    Fake fake;
    fake.result = Shader(1024, 99);
    Outcome o;
    CHECK(fake.Run(&cache, Id(250), out, o, t));
    CHECK(GetFileAttributesW(stale.c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW(fresh.c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW(unrelated.c_str()) != INVALID_FILE_ATTRIBUTES);
    RemoveTree(dir);
}
static void RuntimeCacheFiltersUnrelated() {
    // An unrelated shader is filtered before hashing: no slot, no record and no file.
    const auto dir = TempDir(L"runtime");
    RemoveTree(dir);
    rrtiming::Timing t;
    std::vector<std::string> lines;
    static std::vector<std::string> *sink;
    sink = &lines;
    t.logger = [](const char *s) { sink->push_back(s); };
    DiskCache disk(dir);
    rrsss::RecognizedShaders registry;
    rrsss::RuntimeShaderCache cache(&registry, &disk, &t);
    const auto unrelated = Shader(4096, 8);
    const char *status = "";
    bool attempted = true;
    const char *outcome = "";
    CHECK(!cache.Get(unrelated.data(), unrelated.size(), nullptr, status, &attempted, &outcome));
    CHECK(!attempted && !std::strcmp(status, "unsupported-subsurface-scattering-shader"));
    CHECK(!std::strcmp(outcome, "not-used") && lines.empty() && t.preparations_started == 0);
    CHECK(GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(rrsss::TransformIdentity().find(rrsss::InputStructure) != std::string::npos);
    CHECK(rrsss::CachedRejectionTransient("shader-compiler-unavailable") &&
          !rrsss::CachedRejectionTransient("cached-shader-output-mismatch"));
}
static void DeclaredScene() {
    size_t bytes = 0;
    const std::string line =
        "!38 = !{i32 0, %hostlayout.scene* undef, !\"\", i32 0, i32 3, i32 1, i32 12544, null}";
    CHECK(rrsss::DeclaredSceneBytes("x\n" + line + "\ny\n", bytes) && bytes == 12544);
    CHECK(rrsss::DeclaredSceneBytes(line, bytes) && bytes == 12544);
    // A use of the type elsewhere is not a declaration; two declarations are refused.
    CHECK(rrsss::DeclaredSceneBytes("%x = load %hostlayout.scene* undef\n" + line, bytes));
    CHECK(!rrsss::DeclaredSceneBytes(line + "\n" + line, bytes));
    CHECK(!rrsss::DeclaredSceneBytes("no resource\n", bytes));
}
static void Timing() {
    rrtiming::Timing t;
    static std::vector<std::string> lines;
    t.logger = [](const char *s) { lines.push_back(s); };
    t.Add(rrtiming::Stage::NativeCreate, 3'000'000);
    t.Add(rrtiming::Stage::NativeCreate, 1'000'000);
    CHECK(t.stages[unsigned(rrtiming::Stage::NativeCreate)].count == 2);
    CHECK(t.stages[unsigned(rrtiming::Stage::NativeCreate)].total == 4'000'000);
    CHECK(t.stages[unsigned(rrtiming::Stage::NativeCreate)].max == 3'000'000);
    rrtiming::Local local;
    t.Add(rrtiming::Stage::Transform, rrtiming::Timing::SlowNs + 1, &local);
    CHECK(local.ns[unsigned(rrtiming::Stage::Transform)] == rrtiming::Timing::SlowNs + 1);
    t.pipelines = 5;
    t.Summary();
    CHECK(lines.size() == 2);
    CHECK(lines[0].find("pipelines=5") != std::string::npos &&
          lines[0].find("native-create=4ms/2(max 3ms)") != std::string::npos);
    CHECK(lines[1].find("slow transform took 250 ms") != std::string::npos);
    // Rate-limited and only when something changed; the slow record is logged once.
    t.Summary();
    CHECK(lines.size() == 2);
    t.Summary(true);
    CHECK(lines.size() == 3);
    std::atomic<bool> once{};
    t.Milestone(once, "first device");
    t.Milestone(once, "first device");
    CHECK(lines.size() == 4 && lines[3].find("RR startup: first device at") == 0);
    for (int i = 0; i < 200; ++i)
        t.Summary(true);
    CHECK(lines.size() == 2 + rrtiming::Timing::SummaryCap); // plus the slow record and the milestone
    t.NativeBegin();
    t.NativeBegin();
    t.NativeEnd();
    CHECK(t.in_flight_native == 1 && t.peak_native == 2);
    {
        rrtiming::Scope s(rrtiming::Stage::CacheLoad, &local, &t);
    }
    CHECK(t.stages[unsigned(rrtiming::Stage::CacheLoad)].count == 1);
}
int wmain() {
    Keys();
    HitMissAndInvalidation();
    CorruptTruncatedAndMiskeyed();
    AcceptanceIsRequired();
    FailuresAreNotCached();
    WriteFailureFallsBack();
    Concurrent();
    Bounded();
    RuntimeCacheFiltersUnrelated();
    DeclaredScene();
    Timing();
    std::cout << "PASS: synthetic shader cache (hit, miss, invalidation, corruption, truncation, wrong key, "
                 "acceptance required, failures not cached, write failure, concurrency, size bound) and "
                 "startup timing; no game shader, compiler or GPU used\n";
    return 0;
}
