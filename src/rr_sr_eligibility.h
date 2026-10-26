#pragma once
#include <d3d12.h>
#include <array>
#include <atomic>
#include <cstdint>
namespace rrsr {
// With an add-on hooked beneath RR (DLSS5 NR), replace SR and announce RR's output and depth to
// other add-ons. On for the hang test: RR's history clear no longer submits from inside the
// game's ExecuteCommandLists, where NR's submit hook lost track of the outer submission.
inline constexpr bool ReplaceUnderForeignHook = true;
struct StateProof {
    std::array<bool, 4> known{};
    std::array<D3D12_RESOURCE_STATES, 4> states{};
    std::array<uintptr_t, 4> resources{};
};
// A shader-read state only. Pixel-shader read alone is accepted: RR's evaluate transitions each
// input from its state to non-pixel-shader read and back (Controller::AfterSR).
inline bool ReadOnly(D3D12_RESOURCE_STATES state) {
    constexpr auto reads =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    return state && !(state & ~reads);
}
// An input this list never transitioned keeps the state DLSS requires of its caller,
// non-pixel-shader read, which the native SR route already assumes after every SR call.
// The output must be seen in its unordered access state.
inline bool Eligible(const StateProof &p) {
    for (unsigned i = 0; i < 4; ++i) {
        if (!p.resources[i])
            return false;
        for (unsigned j = 0; j < i; ++j)
            if (p.resources[i] == p.resources[j])
                return false;
        if (i < 3 && p.known[i] && !ReadOnly(p.states[i]))
            return false;
    }
    return p.known[3] && p.states[3] == D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
}
// Why the latest SR evaluate kept native SR, for the health line.
enum Skip : unsigned {
    None,
    Off,         // replacement setting off
    Disabled,    // RR off
    Failed,      // RR failed or retired
    Reconfigure, // RR is being recreated
    Unsafe,      // a copy or state observer could not cover the list
    Scope,       // no RR frame waiting for this SR call
    List,        // SR recorded on another list than the one RR prepared
    Ineligible,  // input or output state (Eligible)
    Rejected,    // RR's own checks refused the frame (logged by AfterSR)
    Callback,    // the game passed a progress callback
    Creation,    // the SR feature's creation was not seen
    Reset,       // the Reset parameter cannot be written
    Untracked,   // an input's transition on the SR list was split, per plane or unrecorded
    Foreign,     // another add-on hooked DLSS evaluation beneath RR and needs the SR call
    Memory,      // a new RR allocation is waiting for GPU memory (not a refusal)
    Count
};
inline const char *SkipText(unsigned why) {
    static constexpr const char *text[] = {"none",        "setting",   "rrOff",     "failed",
                                           "reconfigure", "unsafe",    "noFrame",   "list",
                                           "state",       "rrChecks",  "callback",  "creation",
                                           "reset",       "untracked", "otherHook", "gpuMemory"};
    return why < Count ? text[why] : "unknown";
}
// Lock-free: written on the evaluate thread, read by the health line.
struct Counters {
    std::atomic<unsigned> replaced{}, native{}, last{};
};
inline Counters counters;
inline void Note(unsigned why) {
    counters.last.store(why, std::memory_order_relaxed);
}
} // namespace rrsr
