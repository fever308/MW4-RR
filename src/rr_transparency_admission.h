#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include "rr_transparency_policy.h"
// Why the transparency guide did not admit a frame it was used for. Pure
// policy and counters: no D3D12, ReShade or NGX calls. Each failed condition is
// its own bit, so one frame can report several; Misses() == 0 exactly when the
// route's admission rule (rr_transparency_policy.h) accepts.
namespace rrtrans {
enum Miss : uint32_t {
    MissBaseline = 1u << 0,    // RR rejected its own frame (baseline)
    MissUnwritten = 1u << 1,   // no snapshot was written for the frame
    MissUnanchored = 1u << 2,  // no recognized composition anchored the snapshot
    MissPrepass = 1u << 3,     // the RR prepass list was not in the SR submission
    MissProducer = 1u << 4,    // the producer/preparation list was not in the SR submission
    MissAnchor = 1u << 5,      // the composition (anchor) list was not in the SR submission
    MissDuplicate = 1u << 6,   // a list appeared twice in the SR submission
    MissOrder = 1u << 7,       // every list was present, in an order the rule refuses
    MissEarlier = 1u << 8,     // the producer or anchor list was submitted in an earlier batch
};
inline constexpr unsigned MissCount = 9;
inline const char *MissLabel(unsigned bit) {
    static constexpr const char *names[MissCount] = {
        "baseline-rejected",   "snapshot-unwritten",     "no-composition-anchor",
        "prepass-not-in-batch", "producer-not-in-batch", "composition-not-in-batch",
        "duplicate-list",      "list-order",             "submitted-in-earlier-batch"};
    return bit < MissCount ? names[bit] : "unknown";
}
// Which admission rule the slot's ticket uses (rr_live.h seal block).
enum class Route : uint8_t { Producer, AtDraw, Prepared };
inline bool RouteAdmits(Route route, int prepass, int preparation, int anchor, int consumer,
                        bool preparation_submitted, bool anchor_submitted, bool baseline) {
    switch (route) {
    case Route::Producer:
        return AdmitProducer(prepass, preparation, anchor, consumer, preparation_submitted,
                             anchor_submitted, baseline);
    case Route::AtDraw:
        return AdmitAtDraw(prepass, preparation, anchor, consumer, preparation_submitted,
                           anchor_submitted, baseline);
    default:
        return Admit(prepass, preparation, anchor, consumer, preparation_submitted,
                     anchor_submitted, baseline);
    }
}
// Indices come from rrsss::RecordingIndex: -1 absent, -2 duplicate.
inline uint32_t Misses(Route route, int prepass, int preparation, int anchor, int consumer,
                       bool preparation_submitted, bool anchor_submitted, bool baseline,
                       bool written, bool anchored) {
    uint32_t m = 0;
    if (!baseline)
        m |= MissBaseline;
    if (!written)
        m |= MissUnwritten;
    if (!anchored)
        m |= MissUnanchored;
    if (preparation_submitted || anchor_submitted)
        m |= MissEarlier;
    const int lists[] = {prepass, preparation, anchor, consumer};
    const uint32_t absent[] = {MissPrepass, MissProducer, MissAnchor, 0};
    bool present = true;
    for (unsigned i = 0; i < 4; ++i) {
        if (lists[i] == -2)
            m |= MissDuplicate;
        else if (lists[i] < 0)
            m |= absent[i];
        present = present && lists[i] >= 0;
    }
    // The order bit names only what the presence bits do not already explain.
    if (present && !RouteAdmits(route, prepass, preparation, anchor, consumer, false, false, true))
        m |= MissOrder;
    // A consumer that is absent never reaches the seal; keep the invariant anyway.
    if (!m && !(written && anchored &&
                RouteAdmits(route, prepass, preparation, anchor, consumer, preparation_submitted,
                            anchor_submitted, baseline)))
        m |= MissOrder;
    return m;
}

// What the frame's first composition callback found. Only the first one per
// frame claims the composition latch, so it is the one that matters.
enum class Note : uint8_t {
    None,          // no composition callback for the frame
    Unrecognized,  // no composition shader has been recognized in this process
    Anchored,      // the composition anchored a snapshot
    Context,       // the composition's command list was unsafe or uncovered
    Target,        // its render target or pipeline was not proven
    View,          // its render target view was not the supported layout
    Source,        // its render target resource was unavailable
    Shape,         // the target's size or device does not match RR
    NoSlot,        // no RR frame slot matched the composition
    Suspended,     // the producer route was paused
    Mismatch,      // the snapshot was for another source or command list
    ProducerOpen,  // the producer had not yet copied on this command list
    NotRenderTarget, // the target's state was not proven RENDER_TARGET
    Reused,        // the slot was already recorded, anchored or submitted
    Budget,        // GPU memory for the snapshot was not available
    Failed,        // the snapshot copy threw; the guide is off
    Count
};
inline const char *Label(Note note) {
    static constexpr const char *names[] = {
        "no-composition-draw", "composition-shader-unrecognized", "anchored",
        "composition-list-unsafe", "composition-target-unproven", "composition-view-unsupported",
        "composition-source-unavailable", "composition-shape-mismatch", "no-frame-slot",
        "producer-paused", "source-or-list-mismatch", "producer-copy-pending",
        "composition-not-render-target", "slot-already-used", "no-gpu-memory", "copy-failed"};
    static_assert(sizeof(names) / sizeof(names[0]) == size_t(Note::Count));
    return unsigned(note) < unsigned(Note::Count) ? names[unsigned(note)] : "unknown";
}
// The composition outcome of recent camera frames, keyed by frame, so that a
// later frame's composition recorded before an earlier frame's seal cannot
// relabel the earlier one.
struct CompositionNote {
    static constexpr unsigned Frames = 8; // more than the frames RR keeps in flight
    struct Entry {
        uint32_t frame{UINT32_MAX};
        Note note{Note::None};
    } entries[Frames]{};
    void Set(uint32_t current, Note value) {
        if (current == UINT32_MAX)
            return;
        auto &e = entries[current % Frames];
        // The latch keeps the first composition per frame; a later one never overwrites it.
        if (e.frame == current)
            return;
        e = {current, value};
    }
    // recognized: a composition shader identity is registered in this process.
    Note For(uint32_t slot_frame, bool recognized) const {
        const auto &e = entries[slot_frame % Frames];
        if (slot_frame != UINT32_MAX && e.frame == slot_frame)
            return e.note;
        return recognized ? Note::None : Note::Unrecognized;
    }
};

// Bounded window totals, logged as one rate-limited line.
struct AdmissionCounters {
    uint64_t used{}, admitted{};
    uint64_t misses[MissCount]{};
    uint64_t notes[size_t(Note::Count)]{};
    void Frame(bool accepted, uint32_t misses_bits, Note note) {
        ++used;
        if (accepted) {
            ++admitted;
            return;
        }
        for (unsigned i = 0; i < MissCount; ++i)
            if (misses_bits & (1u << i))
                ++misses[i];
        if (misses_bits & MissUnanchored)
            ++notes[size_t(note) < size_t(Note::Count) ? size_t(note) : 0];
    }
    uint64_t Refused() const {
        return used - admitted;
    }
};
// One line: "name=count" for every non-zero entry, in a fixed order so that
// lines compare across windows.
inline int AdmissionReport(const AdmissionCounters &k, uint64_t seconds, char *buf, size_t n) {
    if (!buf || !n)
        return -1;
    int at = snprintf(buf, n, "Transparency guide (%llus): used=%llu admitted=%llu notAdmitted=%llu",
                      static_cast<unsigned long long>(seconds),
                      static_cast<unsigned long long>(k.used),
                      static_cast<unsigned long long>(k.admitted),
                      static_cast<unsigned long long>(k.Refused()));
    auto append = [&](const char *prefix, const char *name, uint64_t count) {
        if (at < 0 || size_t(at) >= n)
            return;
        at += snprintf(buf + at, n - size_t(at), "%s%s=%llu", prefix, name,
                       static_cast<unsigned long long>(count));
    };
    bool first = true;
    for (unsigned i = 0; i < MissCount; ++i)
        if (k.misses[i]) {
            append(first ? " failed: " : " ", MissLabel(i), k.misses[i]);
            first = false;
        }
    first = true;
    for (size_t i = 0; i < size_t(Note::Count); ++i)
        if (k.notes[i]) {
            append(first ? " anchor: " : " ", Label(Note(i)), k.notes[i]);
            first = false;
        }
    return at;
}
// The labels of a frame's misses, for the pause line. Returns buf.
inline const char *MissSummary(uint32_t misses_bits, Note note, char *buf, size_t n) {
    if (!buf || !n)
        return "";
    buf[0] = 0;
    int at = 0;
    for (unsigned i = 0; i < MissCount && at >= 0 && size_t(at) < n; ++i)
        if (misses_bits & (1u << i)) {
            at += snprintf(buf + at, n - size_t(at), "%s%s", at ? ", " : "", MissLabel(i));
            if ((1u << i) == MissUnanchored && at >= 0 && size_t(at) < n)
                at += snprintf(buf + at, n - size_t(at), " (%s)", Label(note));
        }
    return buf;
}
} // namespace rrtrans
