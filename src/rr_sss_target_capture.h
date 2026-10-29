#pragma once
// Private diagnostic only. Copies one exact game-supplied shader; never changes or executes it.
#include <Windows.h>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>
namespace rrssscapture {
inline constexpr size_t TargetBytes = 56352;
inline constexpr uint64_t TargetHash = 0xc6c858a6310ca6a2ull;
enum class State { Waiting, Queued, Writing, Saved, Failed };
class Capture {
    const size_t expected_bytes;
    const uint64_t expected_hash;
    std::atomic<State> state{State::Waiting};
    std::mutex mutex;
    std::vector<unsigned char> bytes;
    std::filesystem::path output;
    std::atomic<DWORD> error{};
public:
    explicit Capture(size_t size = TargetBytes, uint64_t hash = TargetHash)
        : expected_bytes(size), expected_hash(hash) {}
    State Status() const noexcept { return state.load(); }
    DWORD Error() const noexcept { return error.load(); }
    std::filesystem::path Output() { std::lock_guard lock(mutex); return output; }
    bool Observe(const void *data, size_t size) noexcept {
        if (Status() != State::Waiting || !data || size != expected_bytes || !size || size > 65536)
            return false;
        auto p = static_cast<const unsigned char *>(data);
        uint64_t hash = 14695981039346656037ull;
        for (size_t i = 0; i < size; ++i) hash = (hash ^ p[i]) * 1099511628211ull;
        if (hash != expected_hash) return false;
        try {
            std::lock_guard lock(mutex);
            if (Status() != State::Waiting) return false;
            try {
                bytes.assign(p, p + size); // Own bytes before the pipeline callback returns.
                state = State::Queued;
                return true;
            } catch (...) {
                // Publish failure under the same lock as successful capture: another
                // pipeline callback must not have its successful state overwritten.
                error = ERROR_NOT_ENOUGH_MEMORY;
                state = State::Failed;
                return false;
            }
        } catch (...) { return false; }
    }
    bool Save(const std::filesystem::path &directory) noexcept {
        if (Status() != State::Queued) return false;
        HANDLE file = INVALID_HANDLE_VALUE;
        bool created = false;
        std::filesystem::path path;
        DWORD failure = ERROR_GEN_FAILURE;
        try {
            std::vector<unsigned char> owned;
            {
                std::lock_guard lock(mutex);
                if (Status() != State::Queued) return false;
                owned.swap(bytes);
                state = State::Writing;
            }
            path = directory / L"subsurface-scattering-variant.cso";
            { std::lock_guard lock(mutex); output = path; }
            // A unique per-run directory is required: never overwrite an older capture.
            if (!CreateDirectoryW(directory.c_str(), nullptr)) {
                failure = GetLastError();
            } else {
                file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file == INVALID_HANDLE_VALUE) failure = GetLastError();
                else {
                    created = true;
                    DWORD written{};
                    if (!WriteFile(file, owned.data(), DWORD(owned.size()), &written, nullptr))
                        failure = GetLastError();
                    else if (written != owned.size()) failure = ERROR_WRITE_FAULT;
                    else if (!FlushFileBuffers(file)) failure = GetLastError();
                    else failure = ERROR_SUCCESS;
                    if (!CloseHandle(file) && failure == ERROR_SUCCESS) failure = GetLastError();
                    file = INVALID_HANDLE_VALUE;
                }
            }
        } catch (...) { failure = ERROR_NOT_ENOUGH_MEMORY; }
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        if (failure && created) DeleteFileW(path.c_str());
        error = failure;
        state = failure ? State::Failed : State::Saved;
        return !failure;
    }
    void Fail(DWORD code) noexcept {
        std::lock_guard lock(mutex);
        if (Status() == State::Queued) { bytes.clear(); error = code; state = State::Failed; }
    }
};
inline Capture &Session() { static auto *s = new Capture; return *s; }
inline const char *Label(State s) {
    switch (s) {
    case State::Waiting: return "Waiting for the target shader (load the same noisy scene)";
    case State::Queued: return "Target found; saving capture";
    case State::Writing: return "Saving capture";
    case State::Saved: return "SAVED - ready to send with ReShade.log";
    default: return "FAILED - send ReShade.log for the error";
    }
}
inline std::string Utf8(const std::filesystem::path &path) {
    const auto u = path.u8string();
    return std::string(reinterpret_cast<const char *>(u.data()), u.size());
}
inline void Poll(void (*logger)(const char *)) noexcept {
    try {
        auto &s = Session();
        if (s.Status() == State::Queued) {
            wchar_t exe[32768]{};
            const DWORD n = GetModuleFileNameW(nullptr, exe, 32768);
            if (!n || n >= 32768) s.Fail(ERROR_BAD_PATHNAME);
            else {
                FILETIME time{}; GetSystemTimeAsFileTime(&time);
                const uint64_t stamp = (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
                auto folder = std::filesystem::path(exe).parent_path() /
                    (L"MW4-RR-DIAGNOSTIC-2-" + std::to_wstring(stamp) + L"-" + std::to_wstring(GetCurrentProcessId()));
                s.Save(folder);
            }
        }
        static std::atomic<bool> reported{};
        const auto status = s.Status();
        if ((status == State::Saved || status == State::Failed) && !reported.exchange(true) && logger) {
            const auto line = std::string("SSS-DIAG target-capture=") +
                (status == State::Saved ? "saved" : "failed") + " error=" + std::to_string(s.Error()) +
                " file=" + Utf8(s.Output()) + "; private diagnostic, no automatic upload";
            logger(line.c_str());
        }
    } catch (...) { Session().Fail(ERROR_GEN_FAILURE); }
}
} // namespace rrssscapture
