// Why the transparency guide did not admit a frame (src/rr_transparency_admission.h).
// Pure policy: no D3D12, ReShade or NGX objects.
#include "../src/rr_transparency_admission.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

using namespace rrtrans;

// Misses() is empty exactly when the route's own rule admits the frame, for
// every combination of list indices (absent -1, duplicate -2, 0..3) and flags.
static void TestMissesMatchAdmission() {
    unsigned admitted = 0, refused = 0;
    for (auto route : {Route::Producer, Route::AtDraw, Route::Prepared})
        for (int a = -2; a <= 3; ++a)
            for (int p = -2; p <= 3; ++p)
                for (int n = -2; n <= 3; ++n)
                    for (int c = 0; c <= 3; ++c)
                        for (unsigned flags = 0; flags < 32; ++flags) {
                            const bool ps = flags & 1, as = flags & 2, base = flags & 4,
                                       written = flags & 8, anchored = flags & 16;
                            const bool admit =
                                written && anchored && RouteAdmits(route, a, p, n, c, ps, as, base);
                            const auto m = Misses(route, a, p, n, c, ps, as, base, written, anchored);
                            assert((m == 0) == admit);
                            admit ? ++admitted : ++refused;
                            // Each named condition is reported when it holds.
                            assert(!!(m & MissBaseline) == !base);
                            assert(!!(m & MissUnwritten) == !written);
                            assert(!!(m & MissUnanchored) == !anchored);
                            assert(!!(m & MissEarlier) == (ps || as));
                            assert(!!(m & MissPrepass) == (a == -1));
                            assert(!!(m & MissProducer) == (p == -1));
                            assert(!!(m & MissAnchor) == (n == -1));
                            assert(!!(m & MissDuplicate) == (a == -2 || p == -2 || n == -2));
                        }
    assert(admitted > 0 && refused > admitted);
}

static void TestNamedCases() {
    // The tester's frames: the SSS composite produced and copied the snapshot, but no
    // composition anchored it, so the anchor list is absent too.
    auto m = Misses(Route::Producer, 0, 1, -1, 2, false, false, true, true, false);
    assert(m == (MissUnanchored | MissAnchor));
    // Several failures at once are all named.
    m = Misses(Route::Producer, -1, 1, -1, 2, true, false, false, true, false);
    assert(m == (MissBaseline | MissUnanchored | MissEarlier | MissPrepass | MissAnchor));
    // Present, but the composition was recorded before the producer.
    m = Misses(Route::Producer, 0, 2, 1, 3, false, false, true, true, true);
    assert(m == MissOrder);
    // The prepared route needs the anchor right after the preparation.
    assert(Misses(Route::Prepared, 0, 1, 2, 3, false, false, true, true, true) == 0);
    assert(Misses(Route::Prepared, 0, 1, 3, 4, false, false, true, true, true) == MissOrder);
    char text[256];
    MissSummary(MissUnanchored | MissAnchor, Note::Unrecognized, text, sizeof(text));
    assert(!strcmp(text, "no-composition-anchor (composition-shader-unrecognized), "
                         "composition-not-in-batch"));
    MissSummary(MissBaseline, Note::None, text, sizeof(text));
    assert(!strcmp(text, "baseline-rejected"));
    char tiny[8];
    MissSummary(MissUnanchored | MissAnchor, Note::NoSlot, tiny, sizeof(tiny));
    assert(strlen(tiny) == sizeof(tiny) - 1);
    for (unsigned i = 0; i < MissCount; ++i)
        assert(strcmp(MissLabel(i), "unknown"));
    assert(!strcmp(MissLabel(MissCount), "unknown"));
    for (unsigned i = 0; i < unsigned(Note::Count); ++i)
        assert(strcmp(Label(Note(i)), "unknown"));
}

static void TestCompositionNote() {
    CompositionNote n;
    // No composition callback: whether any composition shader is known decides the note.
    assert(n.For(5, false) == Note::Unrecognized && n.For(5, true) == Note::None);
    n.Set(5, Note::NoSlot);
    n.Set(5, Note::Anchored); // a later composition of the same frame never overwrites
    assert(n.For(5, true) == Note::NoSlot && n.For(5, false) == Note::NoSlot);
    // Another frame's note is not this frame's.
    assert(n.For(6, true) == Note::None && n.For(6, false) == Note::Unrecognized);
    n.Set(6, Note::Anchored);
    assert(n.For(6, true) == Note::Anchored);
    n.Set(UINT32_MAX, Note::Failed);
    assert(n.For(6, true) == Note::Anchored && n.For(UINT32_MAX, true) == Note::None);
}

static void TestReport() {
    AdmissionCounters k;
    char line[512];
    AdmissionReport(k, 10, line, sizeof(line));
    assert(!strcmp(line, "Transparency guide (10s): used=0 admitted=0 notAdmitted=0"));
    k.Frame(true, 0, Note::Anchored);
    for (int i = 0; i < 3; ++i)
        k.Frame(false, MissUnanchored | MissAnchor, Note::Unrecognized);
    k.Frame(false, MissBaseline, Note::Anchored); // anchored: only the baseline is named
    k.Frame(false, MissUnanchored, Note::Context);
    assert(k.used == 6 && k.admitted == 1 && k.Refused() == 5);
    AdmissionReport(k, 12, line, sizeof(line));
    assert(!strcmp(line, "Transparency guide (12s): used=6 admitted=1 notAdmitted=5 failed: "
                         "baseline-rejected=1 no-composition-anchor=4 composition-not-in-batch=3 "
                         "anchor: composition-shader-unrecognized=3 composition-list-unsafe=1"));
    char tiny[20];
    const int full = AdmissionReport(k, 12, line, sizeof(line));
    assert(AdmissionReport(k, 12, tiny, sizeof(tiny)) >= int(sizeof(tiny)) - 1 &&
           strlen(tiny) == sizeof(tiny) - 1 && full == int(strlen(line)));
    assert(AdmissionReport(k, 12, nullptr, 0) == -1);
}

int main() {
    TestMissesMatchAdmission();
    TestNamedCases();
    TestCompositionNote();
    TestReport();
    std::puts("PASS: transparency admission diagnostics");
    return 0;
}
