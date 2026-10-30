#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
namespace rrfgdepth {
// Weapon depth for Frame Generation (experimental, off by default; the saved setting is
// rrsettings::Preferences::fg_weapon_depth). DLSS Frame Generation gets a depth in which the
// weapon slice is remapped as RR's weapon depth remaps it (rrweapondepth::Hardware): world
// depth is the game's own, bit for bit, and the weapon and hands keep their own depth order in
// front of the near plane (reverse-Z depth above 1). Whether Frame Generation reads that as
// intended is what the experiment tests: nothing here assumes it does.
//
// The path, from the MW4 RR trace of 2026-10-17 (mw4-rr-trace.log, Streamline 2.12):
//   1. slSetTagForFrame(frame N): color-in, motion, color-out (eValidUntilEvaluate), then the
//      depth alone (eValidUntilEvaluate): Super Resolution's inputs.
//   2. slEvaluateFeature(DLSS, frame N) on the game's SR command list: RR's AfterSR records here.
//   3. slSetTagForFrame(frame N): HUD-less color, depth, motion, all eOnlyValidNow, extent 0x0
//      (the whole resource): Frame Generation's inputs. Same depth resource as step 1.
//   4. slSetTagForFrame(frame N): back buffer and UI (eValidUntilPresent).
//   5. At present, sl.dlss_g evaluates NGX Frame Generation on its own command list.
// Streamline keeps one tag per buffer type and viewport, so step 3 replaces step 1's depth
// tag: Super Resolution has already been evaluated with step 1's depth for frame N, and the
// next frame tags SR's depth again before its evaluation. RR is NGX-private and reads no
// Streamline tag. Only a call like step 3 (depth and HUD-less color together, eOnlyValidNow)
// is ever changed; every other call, and the slSetTag legacy entry (no frame), pass unchanged.
//
// With eOnlyValidNow Streamline copies the tagged resource on the tagging command list when it
// is tagged. The replacement is written on the SR list by RR's AfterSR (step 2), into a texture
// owned by that frame's RR slot. The game's depth is only read. Step 3 is either on the SR
// list itself (the replacement is written earlier on the same list), or, as the first session
// with this build showed (2026-10-30, every frame), on another list: then only once the SR list
// has been submitted, so the GPU runs RR's write before Streamline's copy (same queue in order;
// another queue waits on RR's fence, Controller::Before). The slot then also tracks the
// tagging list and is not reused until that list has been submitted and its GPU work is done.

// The setting as applied (rr_settings.inl ApplyEnabled). Read once per frame by AfterSR,
// so a change applies from the next frame RR records; the frame's tag follows that frame.
inline std::atomic<bool> enabled{};
// FgWeaponDepthObserve=1 in ReShade.ini (no panel control): with the setting off, still match
// each Frame Generation depth tag to RR's frame and log its metadata (Controller::FgDiagnose),
// replacing nothing. For establishing the path before trying the replacement.
inline std::atomic<bool> observe{};
// Bumped on every change of the setting (rr_settings.inl ApplyEnabled): clears a pause and the
// reason counts at the next frame (Controller::FgSettingChanged).
inline std::atomic<unsigned> changes{};

// Why a frame's Frame Generation depth is the game's own (Reason::Supplied: the replacement).
enum class Reason : unsigned {
    Supplied,    // the replacement was tagged and Streamline returned success
    Disabled,    // the setting is off
    NoFgTags,    // no Frame Generation depth tag in the last 2 s (Frame Generation off?)
    RrOff,       // RR is off, failed or restarting: it tracks the frame and owns the texture
    NoFrame,     // RR recorded no SR for this tag's frame and viewport
    NotRecorded, // RR's SR recording for this frame did not write the replacement
    Lifecycle,   // the depth tag is not eOnlyValidNow: Streamline would read it later
    Order,       // tagged on another list while this frame's SR list was not yet submitted
    Depth,       // the tagged depth is not the depth this frame's SR used
    Submitted,   // the frame's SR list was already submitted
    Format,      // the depth is neither D32_FLOAT_S8X24_UINT nor R32_FLOAT
    Projection,  // the frame's projection is not the game's reverse-Z (rrweapondepth::Map)
    Memory,      // no GPU memory for the texture; retried after a second
    Rejected,    // Streamline refused the replacement; the game's tags were re-sent
    Paused,      // Streamline refused it 3 frames in a row: off until the setting changes
    Stale,       // supplied before, but not in the last 2 s (no frame has matched since)
    Count
};
inline const char *Label(Reason r) {
    switch (r) {
    case Reason::Supplied:
        return "replacement supplied";
    case Reason::Disabled:
        return "off";
    case Reason::NoFgTags:
        return "waiting: no Frame Generation depth tag (is Frame Generation on?)";
    case Reason::RrOff:
        return "waiting: Ray Reconstruction is not running";
    case Reason::NoFrame:
        return "unavailable: RR did not handle this frame";
    case Reason::NotRecorded:
        return "unavailable: not prepared for this frame";
    case Reason::Lifecycle:
        return "unavailable: the game tags depth for Frame Generation in an unexpected way";
    case Reason::Order:
        return "unavailable: Frame Generation's depth was tagged before RR's frame was submitted";
    case Reason::Depth:
        return "unavailable: Frame Generation is given another depth than RR";
    case Reason::Submitted:
        return "unavailable: the frame was already submitted";
    case Reason::Format:
        return "unavailable: unsupported depth format";
    case Reason::Projection:
        return "unavailable: the game's projection is not supported";
    case Reason::Memory:
        return "waiting for GPU memory";
    case Reason::Rejected:
        return "unavailable: Streamline refused the replacement";
    case Reason::Paused:
        return "off: Streamline refused it 3 frames in a row (turn it off and on to retry)";
    case Reason::Stale:
        return "waiting: no replacement supplied in the last 2 s";
    default:
        return "unknown";
    }
}
inline const char *Key(Reason r) {
    static const char *keys[] = {"supplied", "disabled",     "no_fg_tags", "rr_off",
                                 "no_frame", "not_recorded", "lifecycle",  "order",
                                 "depth",    "submitted",    "format",     "projection",
                                 "memory",   "rejected",     "paused",     "stale"};
    static_assert(sizeof(keys) / sizeof(*keys) == unsigned(Reason::Count));
    return unsigned(r) < unsigned(Reason::Count) ? keys[unsigned(r)] : "unknown";
}

// Streamline buffer types (sl_consts.h kBufferType*): depth 0, HUD-less color 2.
inline constexpr uint32_t DepthTag = 0, HudlessTag = 2;
// Streamline's eOnlyValidNow.
inline constexpr uint32_t OnlyValidNow = 0;
inline constexpr unsigned MaxTags = 32;
// A slSetTagForFrame call's Frame Generation depth: the index of its depth tag when the call
// also tags HUD-less color (only Frame Generation reads it) and has exactly one depth tag;
// count otherwise.
inline unsigned FgDepthIndex(const uint32_t *types, unsigned count) {
    if (!types || count > MaxTags)
        return count;
    unsigned depth = count, depths = 0;
    bool hudless = false;
    for (unsigned i = 0; i < count; ++i) {
        if (types[i] == DepthTag) {
            depth = i;
            ++depths;
        }
        hudless |= types[i] == HudlessTag;
    }
    return hudless && depths == 1 ? depth : count;
}

// What a tag call finds for its frame (Controller::FgTag). Checked in this order, so a
// reason names the first thing that is missing.
struct Match {
    // same_list: tagged on this frame's SR list. other_list: on another known list.
    // unsubmitted: the SR list has not been submitted yet.
    bool enabled{}, paused{}, lifecycle_now{}, rr_running{}, slot{}, prepared{}, same_list{},
        other_list{}, unsubmitted{}, same_depth{};
    Reason prepare{}; // why AfterSR did not prepare it (when !prepared)
};
inline constexpr Reason Decide(const Match &m) {
    if (!m.enabled)
        return Reason::Disabled;
    if (m.paused)
        return Reason::Paused;
    if (!m.lifecycle_now)
        return Reason::Lifecycle;
    if (!m.rr_running)
        return Reason::RrOff;
    if (!m.slot)
        return Reason::NoFrame;
    if (!m.prepared)
        return m.prepare == Reason::Supplied ? Reason::NotRecorded : m.prepare;
    // On the SR list: before it is submitted (the write precedes the tag on that list). On
    // another list: only after the SR list is submitted, so the write runs first on the GPU.
    if (m.same_list) {
        if (!m.unsubmitted)
            return Reason::Submitted;
    } else if (!m.other_list || m.unsubmitted)
        return Reason::Order;
    if (!m.same_depth)
        return Reason::Depth;
    return Reason::Supplied;
}
// At AfterSR: whether to write the replacement for this frame. fg_seen: a Frame Generation
// depth tag within the last 2 s. Supplied means prepare it.
inline constexpr Reason Prepare(bool enabled, bool paused, bool fg_seen, bool format_ok,
                                bool mapped, bool memory_wait) {
    return !enabled      ? Reason::Disabled
           : paused      ? Reason::Paused
           : !fg_seen    ? Reason::NoFgTags
           : !format_ok  ? Reason::Format
           : !mapped     ? Reason::Projection
           : memory_wait ? Reason::Memory
                         : Reason::Supplied;
}
// Streamline refusals in a row before the replacement pauses until the setting changes.
inline constexpr unsigned RefusalLimit = 3;

// The panel's status and the log's 10 s summary. Written by the tagging thread and AfterSR
// (under the controller's lock), read lock-free by the panel.
struct Status {
    std::atomic<unsigned> last{unsigned(Reason::Disabled)};
    std::atomic<uint64_t> fg_seen_at{};  // GetTickCount64 of the last FG depth tag
    std::atomic<uint64_t> supplied_at{}; // ... of the last supplied replacement
    std::atomic<unsigned> counts[unsigned(Reason::Count)]{};
    void Note(Reason r) {
        last.store(unsigned(r), std::memory_order_relaxed);
        counts[unsigned(r)].fetch_add(1, std::memory_order_relaxed);
    }
    void Reset() {
        for (auto &c : counts)
            c.store(0, std::memory_order_relaxed);
    }
    bool FgSeen(uint64_t now) const {
        const auto at = fg_seen_at.load(std::memory_order_relaxed);
        return at && now - at <= 2000;
    }
    // "reason=count ..." for the reasons seen since the last call; empty when none.
    void Drain(char *out, size_t size) {
        if (!size)
            return;
        out[0] = 0;
        size_t used = 0;
        for (unsigned i = 0; i < unsigned(Reason::Count); ++i) {
            const unsigned n = counts[i].exchange(0, std::memory_order_relaxed);
            if (!n || used >= size)
                continue;
            const int w =
                snprintf(out + used, size - used, "%s%s=%u", used ? " " : "", Key(Reason(i)), n);
            if (w < 0)
                break;
            used += size_t(w) < size - used ? size_t(w) : size - used - 1;
        }
    }
};
inline Status status;
} // namespace rrfgdepth
