#include "../src/rr_presets.h"
constexpr bool lifecycle() {
    rrpreset::Selection p;
    if (p.desired != 6 || p.has_effective || p.Pending())
        return false;
    if (p.Request(3) || p.desired != 6)
        return false;
    p.Created();
    if (p.effective != 6 || p.Pending())
        return false;
    if (!p.Request(4) || !p.Pending())
        return false;
    if (!p.Request(5) || p.desired != 5 || p.effective != 6)
        return false;
    if (!p.Request(6) || p.Pending())
        return false;
    p.Request(0);
    p.Retired();
    if (!p.Pending() || p.has_effective)
        return false;
    p.Created();
    return p.effective == 0 && !p.Pending();
}
// A request that lands while RR is being created waits for the next RR.
constexpr bool request_during_creation() {
    rrpreset::Selection p;
    p.Request(4);
    const unsigned created = p.desired;
    p.Request(5); // during the creation call
    p.Created(created);
    return p.effective == 4 && p.desired == 5 && p.Pending();
}
static_assert(request_during_creation());
static_assert(lifecycle());
static_assert(!rrpreset::CanRetire(true, false, false, false, false, false, false, false, true));
static_assert(!rrpreset::CanRetire(false, true, false, false, false, false, false, false, true));
static_assert(!rrpreset::CanRetire(false, false, true, false, false, false, false, false, true));
static_assert(!rrpreset::CanRetire(false, false, false, true, false, false, false, false, true));
static_assert(!rrpreset::CanRetire(false, false, false, false, true, false, false, false, true));
static_assert(!rrpreset::CanRetire(false, false, false, false, false, true, false, false, true));
static_assert(!rrpreset::CanRetire(false, false, false, false, false, false, true, false, true));
static_assert(!rrpreset::CanRetire(false, false, false, false, false, false, false, true, true));
static_assert(!rrpreset::CanRetire(false, false, false, false, false, false, false, false, false));
static_assert(rrpreset::CanRetire(false, false, false, false, false, false, false, false, true));
