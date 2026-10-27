#pragma once
// NR offload, Gate 1: GPU ordering probe. Diagnostic only, off unless NrOrderProbe=1.
//
// For each SR/RR result written into the game's DLSS output, it records which GPU work the
// game recorded after it in the same command list, which later submission first touches that
// output, and where frame generation is submitted. A second-GPU pass needs a submission
// boundary between the producer and the first consumer: if the game records the consumer in
// the producer's own list, there is none without command-list interception, which the design
// rules out.
//
// It never changes what the game records or submits. Command lists and queues are opaque ids
// (native pointers) so the model runs on the CPU without a device.
#include <array>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace rrnrorder {
// GPU work recorded in a list. Barriers are not commands: they are recorded as touches.
enum class Op : uint8_t { Draw, Dispatch, Indirect, Copy, Clear, Resolve, Count };
// How a recorded command or barrier involves a watched output.
enum class Use : uint8_t {
    Barrier,      // a transition naming the output
    Read,         // a bound descriptor range of at most ExplicitRange entries holds its view
    MaybeRead,    // a larger (bindless) bound range holds its view: a read is possible
    RenderTarget, // bound as a render target
    CopySource,
    CopyDest,
    Clear,
    Count
};
inline constexpr uint32_t ExplicitRange = 256;
inline const char *Name(Use u) {
    static const char *names[] = {"barrier", "read",   "maybe-read", "render-target",
                                  "copy-src", "copy-dst", "clear"};
    return unsigned(u) < unsigned(Use::Count) ? names[unsigned(u)] : "?";
}
enum class Verdict : uint8_t {
    Pending,
    Abandoned,        // the producer's list was reset or destroyed before submission
    Unsubmitted,      // never submitted within the ticket's window
    SameList,         // the game uses the output later in the producer's own list
    SameListUnproven, // GPU work follows in the producer's list; no proof it skips the output
    ListBoundary,     // the producer's list ends with it; the first use is a later list
    NoConsumer,       // the producer's list ends with it; no later use seen in the window
    Count
};
inline const char *Name(Verdict v) {
    static const char *names[] = {"pending",    "abandoned",     "unsubmitted",
                                  "same-list",  "same-list-unproven", "list-boundary",
                                  "no-consumer"};
    return unsigned(v) < unsigned(Verdict::Count) ? names[unsigned(v)] : "?";
}
// Whether a second-GPU round trip can be placed between producer and consumer at a
// submission boundary, from one ticket's verdict.
inline bool BoundaryExists(Verdict v) {
    return v == Verdict::ListBoundary;
}
struct Touch {
    uint64_t resource{};
    Use use{};
    uint32_t event{}; // per-list event number: orders touches, commands and producers
    uint32_t before{}, after{};
};
struct Submission {
    bool seen{};
    uint64_t list{}, queue{};
    uint8_t queue_type{}; // D3D12_COMMAND_LIST_TYPE of the queue
    uint64_t batch{};     // global ExecuteCommandLists number
    uint32_t index{}, count{};
    Use use{};      // consumer only: the first use in that list
    uint32_t presents{}; // presents between the producer's submission and this one
};
struct Ticket {
    uint32_t id{};
    uint32_t frame{}, viewport{};
    bool replaced{}; // RR took the place of the game's SR
    uint64_t list{};
    std::array<uint64_t, 2> resource{}; // public and native identity of the output
    uint32_t mark{};                    // the list's event number at the producer
    std::array<uint32_t, size_t(Op::Count)> ops_at_mark{};
    // Filled when the producer's list is submitted.
    std::array<uint32_t, size_t(Op::Count)> ops_after{};
    std::array<uint32_t, size_t(Use::Count)> uses_after{};
    Touch first_after{}; // the first touch of the output after the producer, in the list
    bool touched_after{};
    uint32_t commands_before_first_use{}; // GPU commands between producer and first_after
    uint32_t touch_overflow{};
    bool fg_in_list{}; // frame generation evaluated in the producer's list, after it
    Submission submit, consumer, fg;
    uint32_t presents{}; // since submission
    Verdict verdict = Verdict::Pending;
    uint32_t Commands() const {
        uint32_t n = 0;
        for (auto v : ops_after)
            n += v;
        return n;
    }
};
inline Verdict Judge(const Ticket &t) {
    if (!t.submit.seen)
        return t.verdict == Verdict::Abandoned ? Verdict::Abandoned : Verdict::Unsubmitted;
    for (unsigned u = 0; u < unsigned(Use::Count); ++u)
        if (u != unsigned(Use::Barrier) && u != unsigned(Use::MaybeRead) && t.uses_after[u])
            return Verdict::SameList;
    if (t.Commands())
        return Verdict::SameListUnproven;
    return t.consumer.seen ? Verdict::ListBoundary : Verdict::NoConsumer;
}

class Probe {
  public:
    static constexpr unsigned MaxTouches = 16, MaxTickets = 8, Window = 2 /* presents */;
    void (*logger)(const char *){};
    // Detailed lines for the first `detailed` tickets, then a summary every `summary` tickets.
    unsigned detailed = 120, summary = 600;

    bool Armed() const {
        return armed;
    }
    void Arm(bool on) {
        std::lock_guard lock(mutex);
        armed = on;
        if (!on) {
            lists.clear();
            tickets.clear();
            watched = {};
        }
    }
    bool Watched(uint64_t resource) const {
        if (!resource)
            return false;
        for (auto r : watched)
            if (r == resource)
                return true;
        return false;
    }
    // Lock-free precheck for hot callers; Touch rechecks under the lock.
    bool MaybeWatched(uint64_t resource) const {
        return armed && Watched(resource);
    }
    std::array<uint64_t, 4> WatchedResources() const {
        std::lock_guard lock(mutex);
        return watched;
    }
    // A list may be watched for descriptor reads only while it carries an unsubmitted producer
    // or any output is watched.
    bool AnyWatched() const {
        for (auto r : watched)
            if (r)
                return true;
        return false;
    }

    void Produce(uint64_t list, uint64_t public_resource, uint64_t native_resource, uint32_t frame,
                 uint32_t viewport, bool replaced) {
        if (!armed || !list || !(public_resource || native_resource))
            return;
        std::lock_guard lock(mutex);
        Watch(public_resource);
        Watch(native_resource);
        auto &rec = Record(list);
        Ticket t;
        t.id = ++next_id;
        t.frame = frame;
        t.viewport = viewport;
        t.replaced = replaced;
        t.list = list;
        t.resource = {public_resource, native_resource};
        t.mark = ++rec.events;
        t.ops_at_mark = rec.ops;
        if (tickets.size() >= MaxTickets) {
            // The oldest ticket never closed: count it unsubmitted rather than grow.
            Close(tickets.front());
            tickets.erase(tickets.begin());
        }
        tickets.push_back(t);
    }
    void Command(uint64_t list, Op op) {
        if (!armed || !list || op >= Op::Count)
            return;
        std::lock_guard lock(mutex);
        auto &rec = Record(list);
        ++rec.events;
        ++rec.ops[size_t(op)];
    }
    void Note(uint64_t list, uint64_t resource, Use use, uint32_t before = 0,
             uint32_t after = 0) {
        if (!armed || !list || use >= Use::Count)
            return;
        std::lock_guard lock(mutex);
        if (!Watched(resource))
            return;
        auto &rec = Record(list);
        Touch t{resource, use, ++rec.events, before, after};
        if (rec.touches.size() < MaxTouches)
            rec.touches.push_back(t);
        else
            ++rec.overflow;
    }
    void FrameGen(uint64_t list) {
        if (!armed || !list)
            return;
        std::lock_guard lock(mutex);
        auto &rec = Record(list);
        rec.fg_event = ++rec.events;
    }
    void Reset(uint64_t list) {
        if (!armed)
            return;
        std::lock_guard lock(mutex);
        for (auto &t : tickets)
            if (t.list == list && !t.submit.seen && t.verdict == Verdict::Pending)
                t.verdict = Verdict::Abandoned;
        lists.erase(list);
        Sweep();
    }
    void Submit(uint64_t queue, uint8_t queue_type, uint32_t n, const uint64_t *ids) {
        if (!armed || !n || !ids)
            return;
        std::lock_guard lock(mutex);
        const uint64_t batch = ++batches;
        for (uint32_t i = 0; i < n; ++i) {
            auto at = lists.find(ids[i]);
            const ListRecord *rec = at == lists.end() ? nullptr : &at->second;
            Submission here{true, ids[i], queue, queue_type, batch, i, n};
            for (auto &t : tickets) {
                if (t.verdict != Verdict::Pending)
                    continue;
                if (!t.submit.seen) {
                    if (t.list == ids[i] && rec)
                        Submitted(t, *rec, here);
                    continue;
                }
                if (!rec)
                    continue;
                here.presents = t.presents;
                if (!t.consumer.seen)
                    for (const auto &touch : rec->touches)
                        if (touch.resource == t.resource[0] || touch.resource == t.resource[1]) {
                            t.consumer = here;
                            t.consumer.use = touch.use;
                            break;
                        }
                if (!t.fg.seen && !t.fg_in_list && rec->fg_event)
                    t.fg = here;
            }
        }
        Sweep();
    }
    void Present() {
        if (!armed)
            return;
        std::lock_guard lock(mutex);
        ++presents;
        for (auto &t : tickets)
            if (t.verdict == Verdict::Pending) {
                ++t.presents;
                if (t.submit.seen ? t.presents >= Window : t.presents >= Window + 2)
                    t.verdict = t.submit.seen ? Judge(t) : Verdict::Unsubmitted;
            }
        Sweep();
    }

    // Test access: verdict totals so far.
    std::array<uint64_t, size_t(Verdict::Count)> Totals() const {
        std::lock_guard lock(mutex);
        return totals;
    }
    std::vector<Ticket> Closed() const {
        std::lock_guard lock(mutex);
        return closed;
    }
    bool keep_closed = false; // tests keep closed tickets

  private:
    struct ListRecord {
        uint32_t events{};
        std::array<uint32_t, size_t(Op::Count)> ops{};
        std::vector<Touch> touches;
        uint32_t overflow{};
        uint32_t fg_event{};
    };
    mutable std::mutex mutex;
    bool armed{};
    std::unordered_map<uint64_t, ListRecord> lists;
    std::vector<Ticket> tickets, closed;
    std::array<uint64_t, 4> watched{};
    unsigned watch_next{};
    uint32_t next_id{};
    uint64_t batches{}, presents{}, logged{};
    std::array<uint64_t, size_t(Verdict::Count)> totals{};

    ListRecord &Record(uint64_t list) {
        if (lists.size() >= 8192 && !lists.contains(list))
            lists.clear(); // a leak of list ids must not grow without bound
        return lists[list];
    }
    void Watch(uint64_t resource) {
        if (!resource || Watched(resource))
            return;
        watched[watch_next++ % watched.size()] = resource;
    }
    static bool Same(const Ticket &t, uint64_t r) {
        return r && (r == t.resource[0] || r == t.resource[1]);
    }
    static void Submitted(Ticket &t, const ListRecord &rec, const Submission &s) {
        t.submit = s;
        for (size_t i = 0; i < t.ops_after.size(); ++i)
            t.ops_after[i] = rec.ops[i] - t.ops_at_mark[i];
        t.touch_overflow = rec.overflow;
        for (const auto &touch : rec.touches)
            if (touch.event > t.mark && Same(t, touch.resource)) {
                ++t.uses_after[size_t(touch.use)];
                if (!t.touched_after) {
                    t.touched_after = true;
                    t.first_after = touch;
                }
            }
        t.fg_in_list = rec.fg_event > t.mark;
        if (t.fg_in_list)
            t.fg = s;
    }
    void Sweep() {
        for (size_t i = 0; i < tickets.size();)
            if (tickets[i].verdict != Verdict::Pending) {
                Close(tickets[i]);
                tickets.erase(tickets.begin() + i);
            } else
                ++i;
    }
    void Close(Ticket &t) {
        if (t.verdict == Verdict::Pending)
            t.verdict = t.submit.seen ? Judge(t) : Verdict::Unsubmitted;
        ++totals[size_t(t.verdict)];
        if (keep_closed)
            closed.push_back(t);
        if (!logger)
            return;
        ++logged;
        if (logged <= detailed)
            LogTicket(t);
        if (summary && logged % summary == 0)
            LogSummary();
    }
    static const char *QueueName(uint8_t type) {
        return type == 0 ? "direct" : type == 2 ? "compute" : type == 3 ? "copy" : "other";
    }
    void LogTicket(const Ticket &t) const {
        char line[640];
        int n = snprintf(
            line, sizeof(line),
            "NR order #%u frame=%u view=%u %s list=%llx verdict=%s | after producer in list: "
            "draw=%u dispatch=%u indirect=%u copy=%u clear=%u resolve=%u",
            t.id, t.frame, t.viewport, t.replaced ? "rr-replaced" : "native-sr",
            (unsigned long long)t.list, Name(t.verdict), t.ops_after[0], t.ops_after[1],
            t.ops_after[2], t.ops_after[3], t.ops_after[4], t.ops_after[5]);
        if (t.touched_after && n > 0 && size_t(n) < sizeof(line))
            n += snprintf(line + n, sizeof(line) - n, "; output uses %u/%u/%u/%u/%u/%u/%u first=%s",
                          t.uses_after[0], t.uses_after[1], t.uses_after[2], t.uses_after[3],
                          t.uses_after[4], t.uses_after[5], t.uses_after[6],
                          Name(t.first_after.use));
        if (t.touched_after && t.first_after.use == Use::Barrier && n > 0 &&
            size_t(n) < sizeof(line))
            n += snprintf(line + n, sizeof(line) - n, " 0x%x->0x%x", t.first_after.before,
                          t.first_after.after);
        if (t.touch_overflow && n > 0 && size_t(n) < sizeof(line))
            n += snprintf(line + n, sizeof(line) - n, " (touch overflow %u)", t.touch_overflow);
        auto sub = [&](const char *name, const Submission &s) {
            if (n <= 0 || size_t(n) >= sizeof(line))
                return;
            if (!s.seen)
                n += snprintf(line + n, sizeof(line) - n, " | %s: none", name);
            else
                n += snprintf(line + n, sizeof(line) - n,
                              " | %s: q=%llx(%s) batch=%llu idx=%u/%u list=%llx presents=%u%s%s",
                              name, (unsigned long long)s.queue, QueueName(s.queue_type),
                              (unsigned long long)s.batch, s.index, s.count,
                              (unsigned long long)s.list, s.presents,
                              &s == &t.consumer ? " use=" : "",
                              &s == &t.consumer ? Name(s.use) : "");
        };
        sub("submit", t.submit);
        sub("consumer", t.consumer);
        if (t.fg_in_list && n > 0 && size_t(n) < sizeof(line))
            n += snprintf(line + n, sizeof(line) - n, " | fg: in producer list after it");
        else
            sub("fg", t.fg);
        logger(line);
    }
    void LogSummary() const {
        char line[320];
        snprintf(line, sizeof(line),
                 "NR order summary: %llu tickets: same-list=%llu same-list-unproven=%llu "
                 "list-boundary=%llu no-consumer=%llu abandoned=%llu unsubmitted=%llu",
                 (unsigned long long)logged,
                 (unsigned long long)totals[size_t(Verdict::SameList)],
                 (unsigned long long)totals[size_t(Verdict::SameListUnproven)],
                 (unsigned long long)totals[size_t(Verdict::ListBoundary)],
                 (unsigned long long)totals[size_t(Verdict::NoConsumer)],
                 (unsigned long long)totals[size_t(Verdict::Abandoned)],
                 (unsigned long long)totals[size_t(Verdict::Unsubmitted)]);
        logger(line);
    }
};

// Descriptor reads: does a bound range hold a view of a watched output? Ranges are resolved to
// heap slots by the caller; `slots` are the heap slots currently holding the output's views.
struct RangeSlot {
    uint64_t heap{};
    uint32_t start{}, count{};
};
struct HeapSlot {
    uint64_t heap{};
    uint32_t offset{};
    uint64_t resource{}; // the output whose view the slot holds
};
struct RangeHit {
    Use use = Use::Count; // Read, MaybeRead, or Count when no range holds the output
    uint64_t resource{};
};
inline RangeHit RangeUse(const std::vector<RangeSlot> &ranges, const std::vector<HeapSlot> &slots) {
    RangeHit found;
    for (const auto &r : ranges)
        for (const auto &s : slots)
            if (s.heap == r.heap && s.offset >= r.start && uint64_t(s.offset) - r.start < r.count) {
                if (r.count <= ExplicitRange)
                    return {Use::Read, s.resource};
                found = {Use::MaybeRead, s.resource};
            }
    return found;
}
} // namespace rrnrorder
