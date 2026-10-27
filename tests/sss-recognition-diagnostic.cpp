#if __has_include("../src/rr_sss_diagnostic.h")
#include "../src/rr_sss_diagnostic.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include <fstream>
#include <iterator>
using namespace rrsssdiag;
namespace api = reshade::api;
static std::vector<std::string> lines;
static void Log(const char *s) {
    lines.emplace_back(s);
}
static bool Has(const char *s) {
    for (auto &l : lines)
        if (l.find(s) != std::string::npos)
            return true;
    return false;
}
int wmain(int argc, wchar_t **argv) {
    // Missing/malformed pixel descriptions must be counted, never dereferenced or called
    // recognized.
    Tracker d;
    d.logger = Log;
    d.Observe(1, 2, 0, nullptr, 3, nullptr);
    assert(Has("description=missing"));
    const unsigned char code[] = {'a'};
    api::shader_desc ps{code, sizeof(code)};
    api::format rt = api::format::r11g11b10_float;
    unsigned samples = 4;
    api::pipeline_subobject objects[] = {
        {api::pipeline_subobject_type::pixel_shader, 1, &ps},
        {api::pipeline_subobject_type::render_target_formats, 1, &rt},
        {api::pipeline_subobject_type::sample_count, 1, &samples}};
    d.Observe(1, 2, 3, objects, 4, nullptr);
    assert(Has("hash=af63dc4c8601ec8c")); // independently known FNV-1a of a
    assert(Has("recognition=size-mismatch"));
    assert(Has("candidate=1"));
    assert(Has("reflection=compiler-unavailable"));
    auto n = lines.size();
    d.Draw(4, false);
    d.Draw(4, false);
    assert(lines.size() == n + 1 && Has("first-draw"));
    // Reused handles must describe their new shader; the old record cannot leak across destroy.
    d.Forget(4);
    d.Draw(4, false);
    ps.code = nullptr;
    d.Observe(1, 2, 3, objects, 4, nullptr);
    assert(Has("description=missing-code"));
    std::vector<unsigned char> fixtureBytes(56388, 0);
    ps = {fixtureBytes.data(), fixtureBytes.size()};
    d.Observe(1, 2, 3, objects, 5, nullptr);
    assert(Has("recognition=hash-mismatch"));
    // A signature change with identical bytecode must remain visible in the inventory.
    ps = {code, sizeof(code)};
    samples = 1;
    d.Observe(1, 2, 3, objects, 6, nullptr);
    assert(Has("samples=1"));
    d.Draw(6, true, true);
    assert(Has("first-indirect-unknown"));
    // Bound output and memory, report overflow explicitly instead of pretending the capture is
    // complete.
    for (unsigned i = 0; i < Tracker::Limit + 2; ++i)
        d.Observe(1, 2, 3, objects, 100 + i, nullptr);
    assert(Has("inventory-limit"));
    n = lines.size();
    d.Summary(1000);
    d.Summary(11000);
    assert(lines.size() > n);
    assert(Has("untrackedDraws=1"));
    d.Summary(601000);
    assert(!d.Active());
    n = lines.size();
    d.Draw(6, false);
    d.Observe(1, 2, 3, objects, 7, nullptr);
    d.Summary(701000);
    assert(lines.size() == n); // automatic cutoff is real, not just a status label
    // CPU-only real DXIL fixture: optional private input is never packaged with the diagnostic.
    if (argc == 3) {
        std::ifstream f(argv[1], std::ios::binary);
        assert(f);
        std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(f), {}};
        HMODULE module = LoadLibraryExW(
            argv[2], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        assert(module);
        Tracker real;
        real.logger = Log;
        lines.clear();
        ps = {bytes.data(), bytes.size()};
        samples = 4;
        real.Observe(1, 2, 3, objects, 88, module);
        for (auto &line : lines)
            fprintf(stderr, "%s\n", line.c_str());
        assert(Has("recognition=exact") && Has("reflection=ok") && Has("system=10"));
        assert(Has("reg=0 space=0") && Has("dxilHash="));
        real.Draw(88, false);
        assert(Has("first-draw"));
        FreeLibrary(module);
        puts("PASS: real game-supplied DXIL recognized and reflected on CPU; no game code changed "
             "or saved");
    }
    puts("PASS: recognition diagnostics, malformed input, draw deduplication, handle reuse, "
         "bounded inventory and automatic cutoff");
}
#else
#include <cstdio>
int main() {
    puts("FAIL: shader recognition diagnostic is missing");
    return 1;
}
#endif
