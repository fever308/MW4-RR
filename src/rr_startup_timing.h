#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>

// Startup timing: lock-free counters per stage, a few correlated records and
// rate-limited summaries. Recording never logs and never waits; only Summary,
// Milestone and the preparation records log, from callbacks that already log.
namespace rrtiming {
enum class Stage : unsigned {
    PipelineCallback,   // the addon's whole init_pipeline handler
    SssInitialize,      // SSS recognition and alternate creation in it
    CoverageInitialize, // transparency coverage recognition in it
    PipelineHash,       // shader hashing, compute bypass and bookkeeping in it
    NativeCreate,       // driver pipeline creation inside the proof hooks
    LibraryLoad,        // pipelines loaded from a pipeline library
    ProofHook,          // the proof hooks' own inspection around those
    AlternateCreate,    // the addon's own create_pipeline calls
    CompilerLoad,       // loading the companion shader compiler
    Preparation,        // one input's complete preparation
    SlotWait,           // waiting for another thread preparing the same input
    CompilerIdentity,   // hashing the compiler for the cache key
    CacheLoad,
    CacheValidate,
    CacheStore,
    Disassemble,
    Structure,
    Transform,
    Assemble,           // assembly and validation of the transformed shader
    OutputCheck,
    Count
};
inline constexpr const char *Names[] = {
    "pipeline-callback", "sss-init",      "coverage-init",     "pipeline-other",
    "native-create",     "library-load",  "proof-hook",        "alternate-create",
    "compiler-load",     "preparation",   "slot-wait",         "compiler-identity",
    "cache-load",        "cache-validate", "cache-store",      "disassembly",
    "structure",         "transform",     "assembly",          "output-check"};
static_assert(std::size(Names) == unsigned(Stage::Count));
using Clock = std::chrono::steady_clock;
inline uint64_t Now() noexcept {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        Clock::now().time_since_epoch())
                        .count());
}
inline double Ms(uint64_t ns) noexcept { return double(ns) / 1e6; }
// Per-stage totals for one preparation, so its end record can break it down.
struct Local {
    std::array<uint64_t, unsigned(Stage::Count)> ns{};
};
struct Counter {
    std::atomic<uint64_t> count{}, total{}, max{};
};
class Timing {
  public:
    // Operations longer than this are kept for the next summary.
    static constexpr uint64_t SlowNs = 250'000'000;
    static constexpr unsigned SlowCapacity = 16, SummaryCap = 96;
    std::atomic<void (*)(const char *)> logger{};
    std::array<Counter, unsigned(Stage::Count)> stages{};
    std::atomic<uint64_t> pipelines{}, pixel_shaders{}, candidates{}, in_flight_native{},
        peak_native{}, preparations_started{}, preparations_finished{}, cache_hits{},
        cache_misses{}, cache_invalid{}, cache_stores{}, cache_store_failures{},
        cache_load_failures{}, cache_unavailable{};
    uint64_t start = Now();

    void Add(Stage s, uint64_t ns, Local *local = nullptr) noexcept {
        auto &c = stages[unsigned(s)];
        c.count.fetch_add(1, std::memory_order_relaxed);
        c.total.fetch_add(ns, std::memory_order_relaxed);
        for (auto m = c.max.load(std::memory_order_relaxed);
             ns > m && !c.max.compare_exchange_weak(m, ns, std::memory_order_relaxed);)
            ;
        if (local)
            local->ns[unsigned(s)] += ns;
        if (ns >= SlowNs) {
            const auto i = slow_next.fetch_add(1, std::memory_order_relaxed);
            if (i < SlowCapacity) {
                slow[i].stage.store(unsigned(s), std::memory_order_relaxed);
                slow[i].ns.store(ns, std::memory_order_relaxed);
                slow[i].at.store(Now() - start, std::memory_order_release);
            }
        }
    }
    void Log(const char *text) noexcept {
        if (auto *l = logger.load(std::memory_order_acquire))
            try {
                l(text);
            } catch (...) {
            }
    }
    // Once per name: when a runtime stage first happened, after addon load.
    void Milestone(std::atomic<bool> &once, const char *name) noexcept {
        if (once.exchange(true, std::memory_order_relaxed))
            return;
        char line[160];
        std::snprintf(line, sizeof(line), "RR startup: %s at %.1f s after addon load", name,
                      double(Now() - start) / 1e9);
        Log(line);
    }
    // Rate-limited: at most every interval, only when something changed, and
    // at most SummaryCap lines a session. Cheap to call per frame.
    void Summary(bool force = false) noexcept {
        const auto now = Now();
        auto last = last_summary.load(std::memory_order_relaxed);
        const uint64_t interval = summaries.load(std::memory_order_relaxed) < 48
                                      ? 5'000'000'000ull
                                      : 60'000'000'000ull;
        if (!force && now - last < interval)
            return;
        if (!last_summary.compare_exchange_strong(last, now, std::memory_order_relaxed))
            return;
        const auto seen = pipelines.load(std::memory_order_relaxed) +
                          stages[unsigned(Stage::NativeCreate)].count.load(std::memory_order_relaxed) +
                          preparations_finished.load(std::memory_order_relaxed);
        if (!force && seen == last_seen.exchange(seen, std::memory_order_relaxed))
            return;
        if (summaries.fetch_add(1, std::memory_order_relaxed) >= SummaryCap)
            return;
        try {
            std::string text;
            Format(text, now);
            Log(text.c_str());
            FlushSlow();
        } catch (...) {
        }
    }
    void Format(std::string &text, uint64_t now) const {
        auto stage = [&](Stage s) {
            const auto &c = stages[unsigned(s)];
            char b[96];
            std::snprintf(b, sizeof(b), " %s=%.0fms/%llu(max %.0fms)", Names[unsigned(s)],
                          Ms(c.total.load(std::memory_order_relaxed)),
                          (unsigned long long)c.count.load(std::memory_order_relaxed),
                          Ms(c.max.load(std::memory_order_relaxed)));
            return std::string(b);
        };
        char head[320];
        const auto started = preparations_started.load(std::memory_order_relaxed),
                   finished = preparations_finished.load(std::memory_order_relaxed);
        std::snprintf(
            head, sizeof(head),
            "RR startup timing at %.1f s: pipelines=%llu pixelShaders=%llu candidates=%llu "
            "nativeInProgress=%llu(peak %llu) preparations=%llu/%llu unfinished=%llu "
            "cache hit=%llu miss=%llu invalid=%llu stored=%llu storeFail=%llu loadFail=%llu "
            "unavailable=%llu;",
            double(now - start) / 1e9, (unsigned long long)pipelines.load(),
            (unsigned long long)pixel_shaders.load(), (unsigned long long)candidates.load(),
            (unsigned long long)in_flight_native.load(), (unsigned long long)peak_native.load(),
            (unsigned long long)finished, (unsigned long long)started,
            (unsigned long long)(started - (std::min)(started, finished)),
            (unsigned long long)cache_hits.load(), (unsigned long long)cache_misses.load(),
            (unsigned long long)cache_invalid.load(), (unsigned long long)cache_stores.load(),
            (unsigned long long)cache_store_failures.load(),
            (unsigned long long)cache_load_failures.load(),
            (unsigned long long)cache_unavailable.load());
        text = head;
        for (unsigned i = 0; i < unsigned(Stage::Count); ++i)
            if (stages[i].count.load(std::memory_order_relaxed))
                text += stage(Stage(i));
    }
    void FlushSlow() noexcept {
        const auto n = (std::min)(slow_next.load(std::memory_order_relaxed), SlowCapacity);
        for (auto i = slow_logged.load(std::memory_order_relaxed); i < n; ++i) {
            const auto at = slow[i].at.load(std::memory_order_acquire);
            if (!at)
                return; // still being written; reported next time
            if (!slow_logged.compare_exchange_strong(i, i + 1, std::memory_order_relaxed))
                return;
            char line[160];
            std::snprintf(line, sizeof(line), "RR startup: slow %s took %.0f ms, ended at %.1f s",
                          Names[slow[i].stage.load(std::memory_order_relaxed)],
                          Ms(slow[i].ns.load(std::memory_order_relaxed)), double(at) / 1e9);
            Log(line);
        }
    }
    void NativeBegin() noexcept {
        const auto n = in_flight_native.fetch_add(1, std::memory_order_relaxed) + 1;
        for (auto p = peak_native.load(std::memory_order_relaxed);
             n > p && !peak_native.compare_exchange_weak(p, n, std::memory_order_relaxed);)
            ;
    }
    void NativeEnd() noexcept { in_flight_native.fetch_sub(1, std::memory_order_relaxed); }

  private:
    struct Slow {
        std::atomic<unsigned> stage{};
        std::atomic<uint64_t> ns{}, at{};
    };
    std::array<Slow, SlowCapacity> slow{};
    std::atomic<unsigned> slow_next{}, slow_logged{};
    std::atomic<uint64_t> last_summary{}, last_seen{};
    std::atomic<unsigned> summaries{};
};
inline Timing &Global() {
    static Timing timing;
    return timing;
}
// Times one stage into the global counters and, if given, a preparation's own.
class Scope {
    Stage stage;
    Local *local;
    Timing *timing;
    uint64_t begin = Now();

  public:
    explicit Scope(Stage s, Local *l = nullptr, Timing *t = nullptr) noexcept
        : stage(s), local(l), timing(t ? t : &Global()) {}
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
    uint64_t Elapsed() const noexcept { return Now() - begin; }
    ~Scope() { timing->Add(stage, Now() - begin, local); }
};
} // namespace rrtiming
