#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include "rr_transparency_blend_policy.h"
// The transparency composites this process recognizes: the pinned Targets and
// the builds a structural match accepted at pipeline creation
// (rr_transparency_classifier.h). Every later check that names a composite
// (the guide's composition anchor and both albedo-blend capture paths) asks
// here, so a build accepted at creation is never refused by an exact hash at a
// draw. Reads take no lock: draws check this per call.
namespace rrblend {
class RecognizedTargets {
    static constexpr unsigned Capacity = 16;
    std::mutex mutex;
    std::array<Target, Capacity> added{}; // stable: entries are never moved or removed
    std::atomic<unsigned> count{};
    std::atomic<bool> seen_oit{}, seen_refraction{};

  public:
    // A pipeline with this composite was created. Pins and structural builds alike.
    void Seen(Kind kind) {
        (kind == Kind::Oit ? seen_oit : seen_refraction).store(true, std::memory_order_relaxed);
    }
    bool WasSeen(Kind kind) const {
        return (kind == Kind::Oit ? seen_oit : seen_refraction).load(std::memory_order_relaxed);
    }
    // Returns the stable entry, or nullptr when the registry is full.
    const Target *Add(uint64_t hash, size_t bytes, Kind kind) {
        std::lock_guard lock(mutex);
        if (const auto *t = Find(hash, bytes))
            return t->kind == kind ? t : nullptr;
        const auto n = count.load(std::memory_order_relaxed);
        if (n >= Capacity)
            return nullptr;
        added[n] = {hash, bytes, kind};
        count.store(n + 1, std::memory_order_release);
        return &added[n];
    }
    const Target *Find(uint64_t hash, size_t bytes) const {
        if (const auto *t = FindTarget(hash, bytes))
            return t;
        const auto n = count.load(std::memory_order_acquire);
        for (unsigned i = 0; i < n; ++i)
            if (added[i].hash == hash && added[i].bytes == bytes)
                return &added[i];
        return nullptr;
    }
    // Draw-time lookup by the bound pipeline's pixel shader hash, which holds
    // no size; the creation-time entry (registry or coverage variant) proved it.
    const Target *FindHash(uint64_t hash) const {
        for (const auto &t : Targets)
            if (t.hash == hash)
                return &t;
        const auto n = count.load(std::memory_order_acquire);
        for (unsigned i = 0; i < n; ++i)
            if (added[i].hash == hash)
                return &added[i];
        return nullptr;
    }
    bool IsOit(uint64_t hash) const {
        const auto *t = FindHash(hash);
        return t && t->kind == Kind::Oit;
    }
    unsigned Added() const {
        return count.load(std::memory_order_acquire);
    }
};
inline RecognizedTargets &Recognized() {
    static RecognizedTargets targets;
    return targets;
}
} // namespace rrblend
