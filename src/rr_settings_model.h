#pragma once
#include <cstdint>
namespace rrsettings {
struct Preferences {
    bool rr = true;
    bool sss = true;
    bool glass = true;
    bool blend = true;
    bool hit = true;
    bool weapon_depth = true; // the weapon kept in front with its own depth (rr_weapon_depth.h)
    bool water = true;        // water responsivity mask (rr_water.h), preset F only
    bool replace_sr = true;   // RR takes the place of the game's SR when it can
    unsigned preset = 6;
    bool tolerate_isolated = true; // count isolated bad guide pixels, keep the frame
};
// NVIDIA's development RR DLL binds Ctrl+Alt+F9/F11, so modified presses must not toggle guides.
inline constexpr bool HotkeyToggles(bool pressed, bool was_down, bool ctrl_or_alt, bool focused) {
    return pressed && !was_down && !ctrl_or_alt && focused;
}
inline constexpr const char *QualityLabel(int quality) {
    switch (quality) {
    case 0:
        return "Performance";
    case 1:
        return "Balanced";
    case 2:
        return "Quality";
    case 3:
        return "Ultra Performance";
    case 4:
        return "Ultra Quality";
    case 5:
        return "Native / DLAA";
    default:
        return "Not observed yet";
    }
}
} // namespace rrsettings
