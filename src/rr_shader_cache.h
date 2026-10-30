#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "rr_startup_timing.h"
#pragma comment(lib, "bcrypt.lib")

// A per-user cache of shader bytecode this PC prepared itself, under
// %LOCALAPPDATA%\MW4-RR\ShaderCache\v1. Nothing is shipped, uploaded or read
// from anywhere else. Only bytecode is stored, never graphics objects.
//
// An entry is named by a key over everything that decides its content: the
// cache format, the shader role, the transformation version and the shipped
// structure fingerprints, the compiler's SHA-256, and the native input's
// SHA-256 and size. A file is used only when its header repeats that key and
// input, its size is exact and its payload hashes to the recorded SHA-256.
// That proves only that the file is intact: the caller must still accept the
// bytecode by the role's own output checks before using it. Only fully
// accepted results are stored, so failures are never cached.
namespace rrshadercache {
using Digest = std::array<unsigned char, 32>;
inline constexpr uint32_t FormatVersion = 1;
inline constexpr size_t HeaderBytes = 128, MaxPayload = 4 * 1024 * 1024;
inline constexpr size_t MaxEntries = 32, MaxTotalBytes = 32 * 1024 * 1024;
inline constexpr char Magic[4] = {'R', 'R', 'S', 'C'};

class Hasher {
    BCRYPT_HASH_HANDLE handle{};
    bool ok{};

  public:
    Hasher() noexcept {
        ok = BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &handle, nullptr, 0, nullptr, 0, 0) >= 0;
    }
    ~Hasher() {
        if (handle)
            BCryptDestroyHash(handle);
    }
    Hasher(const Hasher &) = delete;
    Hasher &operator=(const Hasher &) = delete;
    Hasher &Add(const void *data, size_t size) noexcept {
        for (auto *p = static_cast<const unsigned char *>(data); ok && size;) {
            const ULONG n = ULONG((std::min)(size, size_t(1) << 30));
            ok = BCryptHashData(handle, const_cast<PUCHAR>(p), n, 0) >= 0;
            p += n;
            size -= n;
        }
        return *this;
    }
    Hasher &Add(const std::string &s) noexcept { return Add(s.data(), s.size()); }
    bool Finish(Digest &out) noexcept {
        ok = ok && BCryptFinishHash(handle, out.data(), ULONG(out.size()), 0) >= 0;
        return ok;
    }
};
inline bool Sha256(const void *data, size_t size, Digest &out) noexcept {
    Hasher h;
    return h.Add(data, size).Finish(out);
}
inline std::string Hex(const unsigned char *data, size_t size) {
    constexpr char hex[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < size; ++i)
        s += hex[data[i] >> 4], s += hex[data[i] & 15];
    return s;
}
inline std::string Hex(const Digest &d) { return Hex(d.data(), d.size()); }

// Everything that decides an entry's content.
struct Identity {
    std::string role;      // the shader role, e.g. "sss-guide"
    std::string transform; // transformation version and the fingerprints it is pinned by
    std::string compiler;  // SHA-256 and size of the compiler that prepared it
    Digest input{};
    uint64_t input_size{};
};
inline bool Key(const Identity &id, Digest &out) {
    if (id.role.empty() || id.transform.empty() || id.compiler.empty() || !id.input_size)
        return false;
    const auto text = "mw4-rr shader cache\nformat=" + std::to_string(FormatVersion) +
                      "\nrole=" + id.role + "\ntransform=" + id.transform +
                      "\ncompiler=" + id.compiler + "\ninput=" + Hex(id.input) + ":" +
                      std::to_string(id.input_size) + "\n";
    return Sha256(text.data(), text.size(), out);
}
// The SHA-256 and size of a loaded module's file, e.g. the companion compiler.
inline bool ModuleIdentity(HMODULE module, std::string &out) {
    out.clear();
    if (!module)
        return false;
    std::wstring path(32768, L'\0');
    const auto n = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
    if (!n || n >= path.size())
        return false;
    path.resize(n);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    Hasher h;
    std::vector<unsigned char> buffer(1 << 20);
    uint64_t total = 0;
    bool ok = true;
    for (DWORD got = 0; ok; total += got) {
        ok = ReadFile(f, buffer.data(), DWORD(buffer.size()), &got, nullptr) != FALSE;
        if (!ok || !got)
            break;
        h.Add(buffer.data(), got);
    }
    CloseHandle(f);
    Digest d{};
    if (!ok || !total || !h.Finish(d))
        return false;
    out = Hex(d) + ":" + std::to_string(total);
    return true;
}
#pragma pack(push, 1)
struct Header {
    char magic[4];
    uint32_t format;
    uint32_t header_bytes;
    uint32_t reserved;
    unsigned char key[32];
    unsigned char input[32];
    uint64_t input_size;
    uint64_t output_size;
    unsigned char output[32];
};
#pragma pack(pop)
static_assert(sizeof(Header) == HeaderBytes);

enum class Load { Hit, Miss, Invalid, Error };

class DiskCache {
    std::wstring dir;
    std::mutex mutex; // directory creation and pruning within this process
    bool created{};
    std::atomic<unsigned> sequence{};

    static std::wstring Name(const Digest &key) {
        const auto h = Hex(key);
        return std::wstring(h.begin(), h.end());
    }
    std::wstring Path(const Digest &key) const { return dir + L"\\" + Name(key) + L".rrsc"; }
    bool Ensure() {
        std::lock_guard lock(mutex);
        if (created)
            return true;
        if (dir.empty())
            return false;
        // Each missing level from the first, so that a new user profile works.
        for (size_t at = dir.find(L'\\', 3); ; at = dir.find(L'\\', at + 1)) {
            const auto part = dir.substr(0, at);
            if (!CreateDirectoryW(part.c_str(), nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS)
                return false;
            if (at == std::wstring::npos)
                break;
        }
        const auto attributes = GetFileAttributesW(dir.c_str());
        created = attributes != INVALID_FILE_ATTRIBUTES &&
                  (attributes & FILE_ATTRIBUTE_DIRECTORY);
        return created;
    }

  public:
    explicit DiskCache(std::wstring directory) : dir(std::move(directory)) {
        while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
            dir.pop_back();
    }
    const std::wstring &Directory() const { return dir; }
    static std::wstring DefaultDirectory() {
        std::wstring local(32768, L'\0');
        const auto n = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), DWORD(local.size()));
        if (!n || n >= local.size())
            return {};
        local.resize(n);
        return local + L"\\MW4-RR\\ShaderCache\\v1";
    }
    // Hit fills out with an intact entry for this key and input. Invalid means
    // a file exists but is truncated, corrupt, incompatible or keyed for
    // something else; the caller removes it and prepares the shader again.
    Load Get(const Digest &key, const Identity &id, std::vector<unsigned char> &out,
             const char *&why) {
        out.clear();
        why = "missing";
        if (dir.empty()) {
            why = "no-directory";
            return Load::Error;
        }
        const auto path = Path(key);
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            const auto e = GetLastError();
            if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
                return Load::Miss;
            why = "open-failed";
            return Load::Error;
        }
        LARGE_INTEGER size{};
        std::vector<unsigned char> bytes;
        bool read = GetFileSizeEx(f, &size) != FALSE;
        if (read && size.QuadPart >= LONGLONG(HeaderBytes) &&
            size.QuadPart <= LONGLONG(HeaderBytes + MaxPayload)) {
            bytes.resize(size_t(size.QuadPart));
            DWORD got = 0;
            read = ReadFile(f, bytes.data(), DWORD(bytes.size()), &got, nullptr) && got == bytes.size();
        }
        CloseHandle(f);
        if (!read) {
            why = "read-failed";
            return Load::Error;
        }
        if (bytes.size() < HeaderBytes) {
            why = size.QuadPart < LONGLONG(HeaderBytes) ? "truncated" : "oversized";
            return Load::Invalid;
        }
        Header h;
        std::memcpy(&h, bytes.data(), sizeof(h));
        if (std::memcmp(h.magic, Magic, 4) || h.format != FormatVersion ||
            h.header_bytes != HeaderBytes || h.reserved) {
            why = "incompatible";
            return Load::Invalid;
        }
        if (std::memcmp(h.key, key.data(), 32)) {
            why = "wrong-key";
            return Load::Invalid;
        }
        if (std::memcmp(h.input, id.input.data(), 32) || h.input_size != id.input_size) {
            why = "wrong-input";
            return Load::Invalid;
        }
        if (!h.output_size || h.output_size > MaxPayload ||
            h.output_size != bytes.size() - HeaderBytes) {
            why = "truncated";
            return Load::Invalid;
        }
        Digest d{};
        if (!Sha256(bytes.data() + HeaderBytes, size_t(h.output_size), d) ||
            std::memcmp(d.data(), h.output, 32)) {
            why = "corrupt";
            return Load::Invalid;
        }
        const auto *p = bytes.data() + HeaderBytes;
        uint32_t container = 0;
        if (h.output_size < 32 || std::memcmp(p, "DXBC", 4) ||
            (std::memcpy(&container, p + 24, 4), container != h.output_size)) {
            why = "not-a-shader";
            return Load::Invalid;
        }
        out.assign(p, p + h.output_size);
        why = "hit";
        return Load::Hit;
    }
    // Writes the entry beside its final name, then renames it into place, so
    // a reader sees either no entry or a complete one, also across processes.
    bool Put(const Digest &key, const Identity &id, const std::vector<unsigned char> &code,
             const char *&why) {
        why = "stored";
        if (code.empty() || code.size() > MaxPayload) {
            why = "size";
            return false;
        }
        if (!Ensure()) {
            why = "directory-unavailable";
            return false;
        }
        Header h{};
        std::memcpy(h.magic, Magic, 4);
        h.format = FormatVersion;
        h.header_bytes = HeaderBytes;
        std::memcpy(h.key, key.data(), 32);
        std::memcpy(h.input, id.input.data(), 32);
        h.input_size = id.input_size;
        h.output_size = code.size();
        Digest d{};
        if (!Sha256(code.data(), code.size(), d)) {
            why = "hash-failed";
            return false;
        }
        std::memcpy(h.output, d.data(), 32);
        const auto final_path = Path(key);
        const auto temp = dir + L"\\" + Name(key) + L"." + std::to_wstring(GetCurrentProcessId()) +
                          L"." + std::to_wstring(GetCurrentThreadId()) + L"." +
                          std::to_wstring(sequence.fetch_add(1)) + L".tmp";
        HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            why = "create-failed";
            return false;
        }
        DWORD wrote = 0, wrote2 = 0;
        const bool written =
            WriteFile(f, &h, DWORD(sizeof(h)), &wrote, nullptr) && wrote == sizeof(h) &&
            WriteFile(f, code.data(), DWORD(code.size()), &wrote2, nullptr) &&
            wrote2 == code.size() && FlushFileBuffers(f);
        CloseHandle(f);
        if (!written) {
            DeleteFileW(temp.c_str());
            why = "write-failed";
            return false;
        }
        if (!MoveFileExW(temp.c_str(), final_path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temp.c_str());
            why = "rename-failed";
            return false;
        }
        Prune(final_path);
        return true;
    }
    void Remove(const Digest &key) { DeleteFileW(Path(key).c_str()); }
    // Keeps at most MaxEntries entries and MaxTotalBytes, removing the oldest
    // first, and leftovers of interrupted writes. Only this cache's own file
    // names in its own directory are touched.
    void Prune(const std::wstring &keep) {
        std::lock_guard lock(mutex);
        struct Entry {
            std::wstring path;
            uint64_t bytes, written;
        };
        std::vector<Entry> entries;
        FILETIME now_file{};
        GetSystemTimeAsFileTime(&now_file);
        const uint64_t now = (uint64_t(now_file.dwHighDateTime) << 32) | now_file.dwLowDateTime;
        auto scan = [&](const wchar_t *pattern, auto each) {
            WIN32_FIND_DATAW data;
            HANDLE find = FindFirstFileW((dir + L"\\" + pattern).c_str(), &data);
            if (find == INVALID_HANDLE_VALUE)
                return;
            do
                if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                    wcslen(data.cFileName) >= 64 + 5)
                    each(data);
            while (FindNextFileW(find, &data));
            FindClose(find);
        };
        auto time = [](const FILETIME &t) {
            return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
        };
        scan(L"*.rrsc", [&](const WIN32_FIND_DATAW &d) {
            entries.push_back({dir + L"\\" + d.cFileName,
                               (uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow,
                               time(d.ftLastWriteTime)});
        });
        // A write left behind by a crash; an hour is far longer than any write.
        scan(L"*.tmp", [&](const WIN32_FIND_DATAW &d) {
            if (now > time(d.ftLastWriteTime) + 36'000'000'000ull)
                DeleteFileW((dir + L"\\" + d.cFileName).c_str());
        });
        std::sort(entries.begin(), entries.end(),
                  [](const Entry &a, const Entry &b) { return a.written < b.written; });
        uint64_t total = 0;
        for (const auto &e : entries)
            total += e.bytes;
        size_t count = entries.size();
        for (const auto &e : entries) {
            if (count <= MaxEntries && total <= MaxTotalBytes)
                break;
            if (_wcsicmp(e.path.c_str(), keep.c_str()) == 0)
                continue;
            if (DeleteFileW(e.path.c_str()))
                --count, total -= e.bytes;
        }
    }
};

// One preparation through the disk cache. validate accepts cached bytecode by
// the role's own checks; build prepares it from the native input. Any cache
// failure falls back to build, and only build's successes are stored.
struct Outcome {
    const char *cache = "not-used"; // hit, miss, invalid:<why>, rejected, error:<why>, ...
    const char *store = "not-stored";
    const char *invalid = nullptr;
};
// A rejection that transient(status) calls transient leaves the entry in place.
template <class Build, class Validate, class Transient>
bool Prepare(DiskCache *cache, const Identity *id, std::vector<unsigned char> &out,
             const char *&status, Outcome &outcome, Build &&build, Validate &&validate,
             Transient &&transient, rrtiming::Local *local = nullptr,
             rrtiming::Timing *timing = nullptr) {
    auto &t = timing ? *timing : rrtiming::Global();
    Digest key{};
    const bool keyed = cache && id && Key(*id, key);
    if (cache && !keyed) {
        outcome.cache = "unavailable";
        t.cache_unavailable.fetch_add(1, std::memory_order_relaxed);
    }
    if (keyed) {
        std::vector<unsigned char> cached;
        const char *why = "";
        Load got;
        {
            rrtiming::Scope scope(rrtiming::Stage::CacheLoad, local, &t);
            got = cache->Get(key, *id, cached, why);
        }
        if (got == Load::Hit) {
            bool accepted;
            {
                rrtiming::Scope scope(rrtiming::Stage::CacheValidate, local, &t);
                accepted = validate(cached, status);
            }
            if (accepted) {
                out = std::move(cached);
                outcome.cache = "hit";
                t.cache_hits.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            outcome.cache = "rejected";
            outcome.invalid = status;
            t.cache_invalid.fetch_add(1, std::memory_order_relaxed);
            if (!transient(status))
                cache->Remove(key);
        } else if (got == Load::Invalid) {
            outcome.cache = "invalid";
            outcome.invalid = why;
            t.cache_invalid.fetch_add(1, std::memory_order_relaxed);
            cache->Remove(key);
        } else if (got == Load::Error) {
            outcome.cache = "load-failed";
            outcome.invalid = why;
            t.cache_load_failures.fetch_add(1, std::memory_order_relaxed);
        } else {
            outcome.cache = "miss";
            t.cache_misses.fetch_add(1, std::memory_order_relaxed);
        }
    }
    out.clear();
    if (!build(out, status)) {
        out.clear();
        return false;
    }
    if (keyed) {
        const char *why = "";
        bool stored;
        {
            rrtiming::Scope scope(rrtiming::Stage::CacheStore, local, &t);
            stored = cache->Put(key, *id, out, why);
        }
        outcome.store = why;
        (stored ? t.cache_stores : t.cache_store_failures).fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}
} // namespace rrshadercache
