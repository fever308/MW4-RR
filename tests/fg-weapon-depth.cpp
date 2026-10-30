#include "../src/rr_trace.h"
#include "../src/rr_weapon_depth.h"
#include <cassert>
#include <cstdio>
#include <cstring>

// Weapon depth for Frame Generation (src/rr_fg_depth.h): which Streamline tag call is Frame
// Generation's, the per-frame decision, and that a replacement changes only a copy.
using rrfgdepth::Reason;

static void Classification() {
    using rrfgdepth::FgDepthIndex;
    // The game's calls for one frame (mw4-rr-trace.log, 2026-10-17).
    const uint32_t sr_inputs[] = {3, 1, 4}; // color-in, motion, color-out
    const uint32_t sr_depth[] = {0};        // SR's depth, alone
    const uint32_t fg_inputs[] = {2, 0, 1}; // HUD-less, depth, motion
    const uint32_t present[] = {53, 23};    // back buffer, UI
    assert(FgDepthIndex(sr_inputs, 3) == 3);
    assert(FgDepthIndex(sr_depth, 1) == 1);
    assert(FgDepthIndex(fg_inputs, 3) == 1);
    assert(FgDepthIndex(present, 2) == 2);
    // Two depth tags in one call: no single identity.
    const uint32_t twice[] = {2, 0, 0};
    assert(FgDepthIndex(twice, 3) == 3);
    assert(FgDepthIndex(nullptr, 0) == 0);
    uint32_t many[rrfgdepth::MaxTags + 1]{};
    many[0] = 2;
    assert(FgDepthIndex(many, rrfgdepth::MaxTags + 1) == rrfgdepth::MaxTags + 1);
}

static rrfgdepth::Match Ready() {
    rrfgdepth::Match m;
    m.enabled = m.lifecycle_now = m.rr_running = m.slot = m.prepared = m.same_list = m.unsubmitted =
        m.same_depth = true;
    return m;
}
static void Decisions() {
    using rrfgdepth::Decide;
    assert(Decide(Ready()) == Reason::Supplied);
    auto m = Ready();
    m.enabled = false;
    assert(Decide(m) == Reason::Disabled);
    m = Ready();
    m.paused = true;
    assert(Decide(m) == Reason::Paused);
    m = Ready();
    m.lifecycle_now = false;
    assert(Decide(m) == Reason::Lifecycle);
    m = Ready();
    m.rr_running = false;
    assert(Decide(m) == Reason::RrOff);
    m = Ready();
    m.slot = false;
    assert(Decide(m) == Reason::NoFrame);
    // Not prepared: AfterSR's reason, or "not recorded" when AfterSR never decided.
    m = Ready();
    m.prepared = false;
    m.prepare = Reason::Memory;
    assert(Decide(m) == Reason::Memory);
    m.prepare = Reason::Supplied;
    assert(Decide(m) == Reason::NotRecorded);
    m = Ready();
    m.same_list = false;
    assert(Decide(m) == Reason::List);
    m = Ready();
    m.unsubmitted = false;
    assert(Decide(m) == Reason::Submitted);
    m = Ready();
    m.same_depth = false;
    assert(Decide(m) == Reason::Depth);

    using rrfgdepth::Prepare;
    assert(Prepare(true, false, true, true, true, false) == Reason::Supplied);
    assert(Prepare(false, false, true, true, true, false) == Reason::Disabled);
    assert(Prepare(true, true, true, true, true, false) == Reason::Paused);
    assert(Prepare(true, false, false, true, true, false) == Reason::NoFgTags);
    assert(Prepare(true, false, true, false, true, false) == Reason::Format);
    assert(Prepare(true, false, true, true, false, false) == Reason::Projection);
    assert(Prepare(true, false, true, true, true, true) == Reason::Memory);
}

static void StatusAndLabels() {
    for (unsigned i = 0; i < unsigned(Reason::Count); ++i) {
        assert(std::strcmp(rrfgdepth::Key(Reason(i)), "unknown") != 0);
        assert(std::strcmp(rrfgdepth::Label(Reason(i)), "unknown") != 0);
    }
    rrfgdepth::Status s;
    assert(!s.FgSeen(5000));
    s.fg_seen_at = 1000;
    assert(s.FgSeen(3000) && !s.FgSeen(3001));
    s.Note(Reason::Supplied);
    s.Note(Reason::Supplied);
    s.Note(Reason::Depth);
    char line[128];
    s.Drain(line, sizeof(line));
    assert(std::strcmp(line, "supplied=2 depth=1") == 0);
    s.Drain(line, sizeof(line));
    assert(line[0] == 0);
    // A change of the setting starts the counts over.
    s.Note(Reason::Disabled);
    s.Reset();
    s.Drain(line, sizeof(line));
    assert(line[0] == 0);
    // A small buffer truncates, never overflows.
    for (unsigned i = 0; i < unsigned(Reason::Count); ++i)
        s.Note(Reason(i));
    char tiny[8];
    s.Drain(tiny, sizeof(tiny));
    assert(std::strlen(tiny) < sizeof(tiny));
}

// The tag hook reads only public fields and replaces the depth tag in a copy.
static void Tags() {
    sl::Resource hudless(sl::ResourceType::eTex2d, reinterpret_cast<void *>(0x10), 4);
    sl::Resource depth(sl::ResourceType::eTex2d, reinterpret_cast<void *>(0x20), 0x80);
    sl::Resource motion(sl::ResourceType::eTex2d, reinterpret_cast<void *>(0x30), 0x40);
    sl::ResourceTag tags[] = {{&hudless, sl::kBufferTypeHUDLessColor, sl::eOnlyValidNow},
                              {&depth, sl::kBufferTypeDepth, sl::eOnlyValidNow},
                              {&motion, sl::kBufferTypeMotionVectors, sl::eOnlyValidNow}};
    rrtrace::FgTag t;
    assert(rrtrace::ReadFgTag(tags, 3, t));
    assert(t.index == 1 && t.native == depth.native && t.state == 0x80 &&
           t.lifecycle == rrfgdepth::OnlyValidNow && !t.extent_width && !t.extent_height);
    sl::ResourceTag copy[rrfgdepth::MaxTags];
    sl::Resource replacement(sl::ResourceType::eTex2d, nullptr);
    assert(rrtrace::CopyTags(tags, 3, t.index, copy, replacement));
    assert(replacement.native == depth.native && replacement.state == depth.state);
    replacement.native = reinterpret_cast<void *>(0x99);
    copy[t.index].resource = &replacement;
    // The game's tags and its resource description are untouched.
    assert(tags[1].resource == &depth && depth.native == reinterpret_cast<void *>(0x20));
    assert(copy[0].resource == &hudless && copy[2].resource == &motion &&
           copy[1].type == sl::kBufferTypeDepth && copy[1].lifecycle == sl::eOnlyValidNow);
    // SR's depth alone, a missing resource or an unknown struct are not Frame Generation's.
    assert(!rrtrace::ReadFgTag(&tags[1], 1, t));
    sl::ResourceTag none[] = {{&hudless, sl::kBufferTypeHUDLessColor, sl::eOnlyValidNow},
                              {nullptr, sl::kBufferTypeDepth, sl::eOnlyValidNow}};
    assert(!rrtrace::ReadFgTag(none, 2, t));
    assert(!rrtrace::ReadFgTag(nullptr, 3, t));
}

// The replacement is the weapon depth RR gets: world depth unchanged, the weapon slice in
// front of the near plane (the shader is rrweapondepth::Hardware's twin).
static void Remap() {
    std::array<float, 16> game{};
    game[0] = 0.75f;
    game[5] = 1.3333f;
    game[11] = 1;
    game[14] = 1;
    rrweapondepth::Mapping m;
    assert(rrweapondepth::Map(game, m) && m.offset == 0);
    for (float d : {0.0f, 1e-6f, 0.25f, rrweapondepth::Slice})
        assert(rrweapondepth::Hardware(d, m) == d);
    assert(rrweapondepth::Hardware(1, m) > 1);
}

int main() {
    Classification();
    Decisions();
    StatusAndLabels();
    Tags();
    Remap();
    std::puts("fg-weapon-depth: passed");
    return 0;
}
