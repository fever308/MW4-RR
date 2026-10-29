#if __has_include("../src/rr_sss_target_capture.h")
#include "../src/rr_sss_target_capture.h"
#include <cassert>
#include <fstream>
#include <thread>
#include <iterator>
using namespace rrssscapture;
int main() {
    wchar_t temp[MAX_PATH]{};
    assert(GetTempPathW(MAX_PATH, temp));
    auto root = std::filesystem::path(temp) / (L"mw4-capture-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    assert(std::filesystem::create_directory(root));
    // Reject unrelated bytes, then copy the matching shader before its borrowed memory changes.
    Capture capture(1, 0xaf63dc4c8601ec8cull);
    unsigned char data[] = {'a'};
    assert(!capture.Observe(nullptr, 1));
    assert(!capture.Observe(data, 2));
    unsigned char other[] = {'b'};
    assert(!capture.Observe(other, 1));
    assert(capture.Status() == State::Waiting);
    std::thread a([&]{capture.Observe(data, 1);});
    std::thread b([&]{capture.Observe(data, 1);});
    a.join(); b.join();
    assert(capture.Status() == State::Queued);
    data[0] = 'z';
    assert(capture.Save(root / L"capture"));
    assert(capture.Status() == State::Saved);
    std::ifstream f(capture.Output(), std::ios::binary);
    std::string saved{std::istreambuf_iterator<char>(f), {}};
    assert(saved == "a");
    f.close();
    assert(!capture.Save(root / L"duplicate"));
    assert(!std::filesystem::exists(root / L"duplicate"));
    assert(!capture.Observe(other, 1));
    // Never overwrite an existing capture directory or report a failed write as saved.
    Capture collision(1, 0xaf63dc4c8601ec8cull);
    data[0]='a'; assert(collision.Observe(data, 1));
    assert(!collision.Save(root / L"capture"));
    assert(collision.Status() == State::Failed && collision.Error() != 0);
    std::ifstream unchanged(capture.Output(), std::ios::binary);
    assert(unchanged.get() == 'a'); unchanged.close();
    Capture invalidPath(1, 0xaf63dc4c8601ec8cull);
    assert(invalidPath.Observe(data, 1));
    assert(!invalidPath.Save(root / L"missing-parent" / L"capture"));
    assert(invalidPath.Status() == State::Failed);
    std::filesystem::remove_all(root);
    puts("PASS: exact selection, owned capture, concurrent deduplication, lossless file, no overwrite and explicit write failure");
}
#else
#include <cstdio>
int main() { puts("FAIL: targeted shader capture is missing"); return 1; }
#endif
