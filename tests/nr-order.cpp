// Gate 1 ordering probe: verdicts from recorded and submitted work. CPU only.
#include "../src/rr_nr_order.h"
#include <cstdio>
#include <cstdlib>
#include <string>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::printf("FAILED line %d: %s\n", __LINE__, #x);                                     \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

using namespace rrnrorder;
static constexpr uint64_t L = 0x100, C = 0x200, F = 0x300, Q = 0x10, Q2 = 0x20, O = 0xA0,
                          ON = 0xA1, Other = 0xB0;
static std::vector<std::string> lines;
static void Log(const char *s) {
    lines.emplace_back(s);
}
struct Armed : Probe {
    Armed() {
        keep_closed = true;
        logger = &Log;
        Arm(true);
    }
};
static void Submit(Probe &p, uint64_t queue, std::initializer_list<uint64_t> ids) {
    std::vector<uint64_t> v(ids);
    p.Submit(queue, 0, uint32_t(v.size()), v.data());
}
static Ticket Only(Probe &p) {
    p.Present();
    p.Present();
    auto closed = p.Closed();
    CHECK(closed.size() == 1);
    return closed[0];
}

static void Disarmed() {
    Probe p;
    p.keep_closed = true;
    p.Produce(L, O, ON, 1, 0, true);
    p.Command(L, Op::Draw);
    Submit(p, Q, {L});
    p.Present();
    p.Present();
    CHECK(p.Closed().empty());
    CHECK(!p.MaybeWatched(O));
}
static void WorkAfterProducer() {
    Armed p;
    p.Command(L, Op::Dispatch); // before the producer: not counted
    p.Produce(L, O, ON, 7, 0, true);
    p.Command(L, Op::Draw);
    p.Command(L, Op::Dispatch);
    Submit(p, Q, {L});
    auto t = Only(p);
    CHECK(t.verdict == Verdict::SameListUnproven);
    CHECK(t.ops_after[size_t(Op::Draw)] == 1 && t.ops_after[size_t(Op::Dispatch)] == 1);
    CHECK(t.frame == 7 && t.replaced);
    CHECK(!BoundaryExists(t.verdict));
}
static void ReadInSameList() {
    Armed p;
    p.Produce(L, O, ON, 1, 0, false);
    p.Note(L, ON, Use::Barrier, 8, 0x40);
    p.Note(L, O, Use::Read);
    p.Command(L, Op::Draw);
    Submit(p, Q, {L});
    auto t = Only(p);
    CHECK(t.verdict == Verdict::SameList);
    CHECK(t.first_after.use == Use::Barrier && t.first_after.before == 8 &&
          t.first_after.after == 0x40);
    CHECK(t.uses_after[size_t(Use::Read)] == 1);
}
static void MaybeReadIsNotProof() {
    Armed p;
    p.Produce(L, O, ON, 1, 0, false);
    p.Note(L, O, Use::MaybeRead);
    p.Command(L, Op::Draw);
    Submit(p, Q, {L});
    CHECK(Only(p).verdict == Verdict::SameListUnproven);
}
static void TouchBeforeProducerIgnored() {
    Armed p;
    // The output is watched from an earlier producer; this list uses it, then produces.
    p.Produce(F, O, ON, 0, 0, false);
    Submit(p, Q, {F});
    p.Present();
    p.Present();
    p.Note(L, O, Use::Read);
    p.Command(L, Op::Draw);
    p.Produce(L, O, ON, 1, 0, false);
    Submit(p, Q, {L});
    p.Present();
    p.Present();
    auto closed = p.Closed();
    CHECK(closed.size() == 2);
    CHECK(closed[1].verdict == Verdict::NoConsumer);
    CHECK(!closed[1].touched_after);
}
static void BoundaryLaterInBatch() {
    Armed p;
    // C is recorded on another thread before L's producer; it is submitted after L.
    p.Produce(F, O, ON, 0, 0, false); // watch O
    p.Present();
    p.Present();
    p.Command(C, Op::Draw);
    p.Note(C, O, Use::Barrier, 8, 0x80);
    p.Note(C, O, Use::Read);
    p.Command(C, Op::Draw);
    p.Command(L, Op::Draw);
    p.Produce(L, O, ON, 2, 0, true);
    p.Note(L, ON, Use::Barrier, 8, 0x40); // a trailing transition is no command
    // An earlier list in the batch that touches O is no consumer: it runs before L.
    p.Note(Other, O, Use::Read);
    p.Submit(Q, 0, 3, std::array<uint64_t, 3>{Other, L, C}.data());
    p.Present();
    p.Present();
    auto closed = p.Closed();
    CHECK(closed.size() == 2);
    auto t = closed[1];
    CHECK(t.verdict == Verdict::ListBoundary && BoundaryExists(t.verdict));
    CHECK(t.submit.index == 1 && t.submit.count == 3);
    CHECK(t.consumer.seen && t.consumer.list == C && t.consumer.index == 2 &&
          t.consumer.batch == t.submit.batch && t.consumer.use == Use::Barrier);
}
static void BoundaryOtherQueue() {
    Armed p;
    p.Produce(L, O, ON, 3, 0, true);
    Submit(p, Q, {L});
    p.Note(C, O, Use::Read);
    p.Command(C, Op::Dispatch);
    p.Submit(Q2, 2, 1, &C);
    auto t = Only(p);
    CHECK(t.verdict == Verdict::ListBoundary);
    CHECK(t.consumer.queue == Q2 && t.consumer.queue_type == 2 &&
          t.consumer.batch == t.submit.batch + 1);
    CHECK(lines.back().find("compute") != std::string::npos);
}
static void Abandoned() {
    Armed p;
    p.Produce(L, O, ON, 1, 0, true);
    p.Reset(L);
    auto closed = p.Closed();
    CHECK(closed.size() == 1 && closed[0].verdict == Verdict::Abandoned);
}
static void Unsubmitted() {
    Armed p;
    p.Produce(L, O, ON, 1, 0, true);
    for (int i = 0; i < 3; ++i)
        p.Present();
    CHECK(p.Closed().empty());
    p.Present();
    auto closed = p.Closed();
    CHECK(closed.size() == 1 && closed[0].verdict == Verdict::Unsubmitted);
}
static void FrameGeneration() {
    {
        Armed p;
        p.Produce(L, O, ON, 1, 0, true);
        p.FrameGen(L);
        Submit(p, Q, {L});
        auto t = Only(p);
        CHECK(t.fg_in_list && t.verdict == Verdict::NoConsumer);
    }
    {
        Armed p;
        p.Produce(L, O, ON, 1, 0, true);
        Submit(p, Q, {L});
        p.FrameGen(F);
        p.Present();
        Submit(p, Q, {F});
        p.Present();
        auto closed = p.Closed();
        CHECK(closed.size() == 1);
        CHECK(!closed[0].fg_in_list && closed[0].fg.seen && closed[0].fg.list == F &&
              closed[0].fg.presents == 1);
    }
}
static void UnwatchedIgnored() {
    Armed p;
    p.Produce(L, O, ON, 1, 0, true);
    p.Note(L, Other, Use::Read);
    Submit(p, Q, {L});
    auto t = Only(p);
    CHECK(!t.touched_after && t.verdict == Verdict::NoConsumer);
}
static void TicketLimit() {
    Armed p;
    for (unsigned i = 0; i < Probe::MaxTickets + 1; ++i)
        p.Produce(L + i, O, ON, i, 0, true);
    auto closed = p.Closed();
    CHECK(closed.size() == 1 && closed[0].verdict == Verdict::Unsubmitted && closed[0].frame == 0);
}
static void Summary() {
    lines.clear();
    Armed p;
    p.detailed = 1;
    p.summary = 2;
    for (uint32_t i = 0; i < 2; ++i) {
        p.Produce(L, O, ON, i, 0, true);
        Submit(p, Q, {L});
        p.Present();
        p.Present();
    }
    CHECK(lines.size() == 2);
    CHECK(lines[0].rfind("NR order #1 ", 0) == 0);
    CHECK(lines[1].find("no-consumer=2") != std::string::npos);
}
static void Ranges() {
    std::vector<HeapSlot> slots{{1, 500, O}};
    CHECK(RangeUse({{1, 490, 16}}, slots).use == Use::Read);
    CHECK(RangeUse({{1, 490, 16}}, slots).resource == O);
    CHECK(RangeUse({{1, 0, 1000000}}, slots).use == Use::MaybeRead);
    CHECK(RangeUse({{2, 490, 16}}, slots).use == Use::Count);
    CHECK(RangeUse({{1, 501, 16}, {1, 400, 100}}, slots).use == Use::Count);
    CHECK(RangeUse({{1, 0, 1000000}, {1, 500, 1}}, slots).use == Use::Read);
    CHECK(RangeUse({{1, 0xFFFFFF00u, 0xFFFFFFFFu}}, {{1, 0xFFFFFFF0u, ON}}).use ==
          Use::MaybeRead);
    std::vector<HeapSlot> two{{1, 10, Other}, {1, 500, O}};
    CHECK(RangeUse({{1, 500, 1}}, two).resource == O);
}

static void FgTickets() {
    Armed p;
    p.Command(F, Op::Draw);
    p.FrameGen(F);
    p.Command(F, Op::Draw);
    p.Command(F, Op::Copy);
    p.Present();
    Submit(p, Q, {L, F});
    auto fg = p.ClosedFg();
    CHECK(fg.size() == 1 && fg[0].submit.seen && fg[0].submit.index == 1);
    CHECK(fg[0].Commands() == 2 && fg[0].ops_after[size_t(Op::Copy)] == 1);
    CHECK(fg[0].submit.presents == 1);
    CHECK(lines.back().find("NR order FG #1") != std::string::npos);
    p.FrameGen(C);
    p.Reset(C);
    CHECK(p.ClosedFg().size() == 2 && p.ClosedFg()[1].abandoned);
    p.FrameGen(L);
    for (int i = 0; i < 5; ++i)
        p.Present();
    CHECK(p.ClosedFg().size() == 3 && !p.ClosedFg()[2].submit.seen);
}

int main() {
    Disarmed();
    WorkAfterProducer();
    ReadInSameList();
    MaybeReadIsNotProof();
    TouchBeforeProducerIgnored();
    BoundaryLaterInBatch();
    BoundaryOtherQueue();
    Abandoned();
    Unsubmitted();
    FrameGeneration();
    UnwatchedIgnored();
    TicketLimit();
    Summary();
    Ranges();
    FgTickets();
    std::puts("NR ordering probe tests passed");
}
