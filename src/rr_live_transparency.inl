// Included inside Controller. The caller proves native OIT target and the
// original completed preparation transition or a current original RT layout;
// binding a target alone never proves a safe source state.
private:
rrtrans::CompositionLatch transparency_composition;
// Why used frames were not admitted: the frame's first composition outcome and bounded window
// totals, logged as one line at most every 10 s while frames fail (TransparencyAdmissionLocked).
rrtrans::CompositionNote transparency_note;
rrtrans::AdmissionCounters transparency_counters;
ULONGLONG transparency_window{}, transparency_logged_at{};
bool transparency_failing{}; // the last logged window had a frame that was not admitted
bool transparency_producer_suspended{};
// A suspension ends after a pause instead of lasting until the guide is toggled: one frame that
// was not admitted (or found no GPU memory) no longer drops the guide for the session. Repeated
// non-admission backs off (5, 10, 20, 40, 60 s) until a producer frame is admitted again.
ULONGLONG transparency_resume_at{};
unsigned transparency_suspensions{}, transparency_backoff{};
// why: nullptr for a frame not admitted (misses and note then say why), else the reason. Memory
// waits 1 s; anything else backs off from 5 s.
void SuspendTransparencyProducer(const char *why, bool memory = false, uint32_t misses = 0,
                                 rrtrans::Note note = rrtrans::Note::None) {
    ULONGLONG pause = 1000;
    if (!memory) {
        pause = (std::min)(5000ull << (std::min)(transparency_backoff, 4u), 60000ull);
        ++transparency_backoff;
    }
    transparency_producer_suspended = true;
    transparency_resume_at = GetTickCount64() + pause;
    if (++transparency_suspensions > 3)
        return;
    char reasons[256], line[384];
    snprintf(line, sizeof(line), "Transparency guide paused (%s%s%s); retrying in %llus%s",
             why ? why : "frame not admitted", why || !misses ? "" : ": ",
             why || !misses ? "" : rrtrans::MissSummary(misses, note, reasons, sizeof(reasons)),
             pause / 1000, transparency_suspensions == 3 ? " (further pauses not logged)" : "");
    Log(line);
}
// Counts one used frame at its seal. A line is logged for a 10 s window in which a frame was not
// admitted, at most once a minute while the same failures continue, and once when frames are
// admitted again; it counts everything since the last line.
void TransparencyAdmissionLocked(bool accepted, uint32_t misses, rrtrans::Note note,
                                 ULONGLONG now) {
    if (!transparency_window)
        transparency_window = transparency_logged_at = now;
    transparency_counters.Frame(accepted, misses, note);
    if (now - transparency_window < 10000)
        return;
    transparency_window = now;
    const auto &k = transparency_counters;
    const bool failing = k.Refused() != 0;
    if (!failing && !transparency_failing) {
        transparency_counters = {};
        transparency_logged_at = now;
        return;
    }
    if (failing && transparency_failing && now - transparency_logged_at < 60000)
        return;
    char line[640];
    if (rrtrans::AdmissionReport(k, (now - transparency_logged_at) / 1000, line, sizeof(line)) > 0)
        Log(line);
    transparency_failing = failing;
    transparency_counters = {};
    transparency_logged_at = now;
}
void TransparencyNote(rrtrans::Note note) {
    transparency_note.Set(camera_frame, note);
}
// A pipeline with an OIT composition shader (pinned or structurally matched) exists.
static bool CompositionRecognized() {
    return rrblend::Recognized().WasSeen(rrblend::Kind::Oit);
}
void ResumeTransparencyProducer() {
    if (transparency_producer_suspended && GetTickCount64() >= transparency_resume_at)
        transparency_producer_suspended = false;
}

Slot *TransparencySlot(ID3D12Resource *source, bool allow_late_anchor = false) {
    if (!source || !initialized || camera_frame == UINT_MAX || camera_view == UINT_MAX ||
        !latest.readable || !latest.basis_valid || !latest.projection_valid ||
        GetTickCount64() - camera_time > 1000 ||
        (selected_view != UINT_MAX && selected_view != camera_view))
        return nullptr;
    Slot *found{};
    bool conflict = false;
    for (auto &s : slots)
        if (s.busy && s.frame == camera_frame && s.viewport == camera_view) {
            conflict = true;
            const auto &t = s.transparency;
            // A prebound producer guide may acquire only its first exact-source anchor
            // after AfterSR. The snapshot and recorded commands remain immutable.
            const bool late = allow_late_anchor && s.recorded && s.transparency_used && !s.sealed &&
                              !s.pre_submitted && t.producer_copy && t.written && t.prep_live &&
                              !t.prep_submitted && !t.anchor_submitted && t.source.Get() == source;
            if ((s.pre_live || s.awaiting_prepass) && (!s.recorded || late) && !s.poisoned &&
                s.generation == generation) {
                if (found)
                    return nullptr;
                found = &s;
            }
        }
    if (found)
        return found;
    if (conflict)
        return nullptr;
    // Match a retired warmed slot to the observed source, independently of any
    // display resolution. An earlier CPU producer can precede its prepass.
    for (auto &s : slots)
        if (!s.busy && s.gpu.rr_output &&
            s.gpu.transparency.Matches(source->GetDesc(), s.gpu.w, s.gpu.h)) {
            s.busy = true;
            s.awaiting_prepass = true;
            s.frame = camera_frame;
            s.viewport = camera_view;
            s.generation = generation;
            return &s;
        }
    return nullptr;
}
bool TransparencyBudget(const D3D12_RESOURCE_DESC &source, UINT w, UINT h) {
    const UINT64 glass = rrtrans::RequiredBytes(pipelines.device.Get(), source, w, h);
    if (!glass)
        return false;
    UINT64 base{}, depth{}, sss{}, water{};
    for (const auto &s : slots) {
        UINT64 b{}, d{};
        if (!SlotReservationParts(pipelines.device.Get(), s.gpu, budget.limit, b, d) ||
            b < s.gpu.sss.bytes || b - s.gpu.sss.bytes < s.gpu.transparency.bytes ||
            b - s.gpu.sss.bytes - s.gpu.transparency.bytes < s.gpu.water.bytes)
            return false;
        base = (std::max)(base, b - s.gpu.sss.bytes - s.gpu.transparency.bytes - s.gpu.water.bytes);
        depth = (std::max)(depth, d);
        sss = (std::max)(sss, s.gpu.sss.bytes);
        water = (std::max)(water, s.gpu.water.bytes);
    }
    const UINT64 kept = persistent.bytes;
    // Protect mandatory depth conversion and enabled SSS even while some slots
    // have not yet needed those resources. Exact device allocation sizes only.
    auto depth_desc = TextureDesc(w, h, DXGI_FORMAT_R32_FLOAT);
    depth_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    auto depth_buffer = rrprivate::Buffer(((UINT64(w) * 4 + 255) & ~255ull) * h);
    UINT64 reserve{};
    for (const auto &d : {depth_desc, depth_buffer}) {
        const auto n = pipelines.device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
        if (!n || n == UINT64_MAX || n > budget.limit - reserve)
            return false;
        reserve += n;
    }
    depth = (std::max)(depth, reserve);
    // Reserve SSS even while its toggle is off. Otherwise warmed glass
    // slots could consume the ceiling and prevent SSS being enabled later.
    {
        const auto n = rrsss::Gpu::RequiredBytes(pipelines.device.Get(), w, h, source.SampleDesc);
        if (!n)
            return false;
        sss = (std::max)(sss, n);
    }
    uint64_t required{};
    // Water storage (rr_water.h) takes the otherwise unused hit term.
    if (!rrtrans::RequiredSlots(slots.size(), base, depth, sss, glass, water, kept, required))
        return false;
    if (required > budget.limit) {
        if (!budget.Reserve(pipelines.device.Get(), required))
            return false;
    }
    return rrtrans::FitsSlots(slots.size(), base, depth, sss, glass, water, kept, budget.limit);
}

public:
#define RRTRANS_PRODUCER_CONTROLLER 1
// The verified native draw owns a strong reference until both recording
// retirement and its submission fence. No later source identity lookup is used.
bool TransparencyProducerMark(ID3D12GraphicsCommandList *cl, ID3D12Resource *source,
                              UINT64 recording) {
    std::lock_guard lock(mutex);
    ResumeTransparencyProducer();
    if (!enabled || !transparency_enabled || transparency_failed ||
        transparency_producer_suspended || failed || retired || NeedsReconfigure() || !cl ||
        !source || !recording || cl->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        transparency_composition.frame == camera_frame)
        return false;
    const auto d = source->GetDesc();
    if (d.Width > UINT_MAX / 2 || d.Height > UINT_MAX / 2 ||
        !rrtrans::Shape(d, UINT(d.Width * 2), d.Height * 2) ||
        !rrsparsecopy::SameDevice(pipelines.device.Get(), source))
        return false;
    auto *s = TransparencySlot(source);
    if (!s)
        return false;
    auto &t = s->transparency;
    if (t.preparation || t.anchor || !s->gpu.transparency.Matches(d, s->gpu.w, s->gpu.h))
        return false;
    t.source = source;
    t.preparation = cl;
    t.prep_live = true;
    t.producer_copy = true;
    t.producer_pending = true;
    t.producer_recording = recording;
    return true;
}
void TransparencyProducerUnknown(ID3D12GraphicsCommandList *cl) {
    std::lock_guard lock(mutex);
    for (auto &s : slots) {
        auto &t = s.transparency;
        if (t.producer_copy && t.preparation.Get() == cl && !t.written) {
            t.producer_pending = false;
            t.producer_ready = false;
        }
    }
}

private:
void TransparencyProducerBarriersLocked(ID3D12GraphicsCommandList *cl, UINT n,
                                        const D3D12_RESOURCE_BARRIER *b) {
    for (auto &s : slots) {
        auto &t = s.transparency;
        if (t.preparation.Get() != cl)
            continue;
        // Readiness belongs only to the immediately observed original batch. If
        // Inspector skipped its callback (e.g. predication), a later seam is too late.
        if (t.producer_ready) {
            t.producer_ready = false;
            continue;
        }
        if (!t.producer_pending || !t.prep_live)
            continue;
        if (n > 4096 || (n && !b)) {
            t.producer_pending = false;
            continue;
        }
        for (UINT i = 0; i < n; ++i) {
            const auto &v = b[i];
            if (v.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
                t.producer_pending = false;
                break;
            }
            if (v.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
                v.Transition.pResource != t.source.Get())
                continue;
            t.producer_pending = false;
            const auto &x = v.Transition;
            t.producer_ready =
                v.Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE &&
                (x.Subresource == 0 || x.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) &&
                x.StateBefore == D3D12_RESOURCE_STATE_RENDER_TARGET &&
                rrtrans::ProducerReadState(unsigned(x.StateAfter));
            t.producer_state = x.StateAfter;
            // Inspector forwards the individual transition and invokes the copy before
            // any later barrier in this batch, including a global alias.
            break;
        }
    }
}

public:
bool TransparencyProducerTransition(ID3D12GraphicsCommandList *cl, const D3D12_RESOURCE_BARRIER &b,
                                    bool safe) {
    std::lock_guard lock(mutex);
    if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
        return false;
    for (auto &s : slots) {
        auto &t = s.transparency;
        if (!t.producer_copy || !t.producer_ready || t.preparation.Get() != cl ||
            t.source.Get() != b.Transition.pResource)
            continue;
        t.producer_ready = false;
        if (!safe || !enabled || !transparency_enabled || transparency_failed ||
            transparency_producer_suspended || failed || retired || NeedsReconfigure() ||
            !t.prep_live || s.recorded || s.poisoned || s.generation != generation ||
            b.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE ||
            b.Transition.StateBefore != D3D12_RESOURCE_STATE_RENDER_TARGET ||
            b.Transition.StateAfter != t.producer_state ||
            (b.Transition.Subresource != 0 &&
             b.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES))
            return false;
        const auto d = t.source->GetDesc();
        if (!s.gpu.rr_output || !s.gpu.transparency.Matches(d, s.gpu.w, s.gpu.h))
            return false;
        try {
            if (!s.gpu.transparency.snapshot && !TransparencyBudget(d, s.gpu.w, s.gpu.h)) {
                return false;
            }
            transparency_pipeline.Initialize(pipelines.device.Get());
            s.gpu.transparency.Prepare(s.gpu, pipelines, budget, d, s.gpu.w, s.gpu.h);
            rrinternal::Scope copies;
            rrcompute::Tracker::Internal state;
            s.gpu.transparency.Copy(cl, t.source.Get(), t.producer_state);
            t.written = true;
            return true;
        } catch (const rrprivate::AllocationDeferred &) {
            t.written = false;
            SuspendTransparencyProducer("waiting for GPU memory", true);
            return false;
        } catch (const std::exception &e) {
            t.written = false;
            SuspendTransparencyProducer(e.what());
            return false;
        }
    }
    return false;
}
bool TransparencyEnabled() {
    std::lock_guard lock(mutex);
    return transparency_enabled;
}
void SetTransparencyEnabled(bool value) {
    std::lock_guard lock(mutex);
    if (transparency_enabled != value) {
        transparency_enabled = value;
        if (value)
            transparency_producer_suspended = false;
        policy.Reject();
    }
}
void ToggleTransparency() {
    std::lock_guard lock(mutex);
    SetTransparencyEnabled(!transparency_enabled);
}
void TransparencyUnsupported(rrtrans::Note why) {
    std::lock_guard lock(mutex);
    if (enabled && transparency_enabled && !failed && !retired &&
        transparency_composition.Claim(camera_frame, camera_view))
        TransparencyNote(why);
}
void TransparencyAnchor(ID3D12GraphicsCommandList *cl, ID3D12Resource *source) {
    std::lock_guard lock(mutex);
    if (!enabled || !transparency_enabled || transparency_failed || failed || retired ||
        NeedsReconfigure() || !cl || !source)
        return;
    const auto d = source->GetDesc();
    if (d.Width > UINT_MAX / 2 || d.Height > UINT_MAX / 2 ||
        !rrtrans::Shape(d, UINT(d.Width * 2), d.Height * 2) ||
        !rrsparsecopy::SameDevice(pipelines.device.Get(), source)) {
        return;
    }
    transparency_source = source;
    auto *s = TransparencySlot(source, true);
    if (!s) {
        return;
    }
    auto &t = s->transparency;
    if (t.producer_copy)
        return; // Only the first composition may anchor this earlier snapshot.
    if (t.source && t.source.Get() != source) {
        t.written = false;
        t.anchored = false;
        return;
    }
    if (t.anchor && t.anchor.Get() != cl) {
        t.written = false;
        t.anchored = false;
        return;
    }
    if (t.at_draw)
        return;
    t.source = source;
    t.anchor = cl;
    t.anchor_live = true;
    t.anchored = t.preparation.Get() != cl;
}
// Invoked immediately before the native common OIT composition. The caller
// independently proves RT using original complete barriers, including for an
// indirect command that may contain zero draws. Never infer state from RTVs.
bool TransparencyAtDraw(ID3D12GraphicsCommandList *cl, ID3D12Resource *source,
                        bool render_target_proven) {
    std::lock_guard lock(mutex);
    if (!enabled || !transparency_enabled || transparency_failed || failed || retired ||
        NeedsReconfigure() || !cl || !source)
        return false;
    // Claim before slot/allocation/layout checks. A rejected first composition
    // cannot let a later composition snapshot color already containing glass.
    if (!transparency_composition.Claim(camera_frame, camera_view))
        return false;
    using rrtrans::Note;
    auto note = [&](Note n) {
        TransparencyNote(n);
        return false;
    };
    if (auto *s = TransparencySlot(source, true); s && s->transparency.producer_copy) {
        auto &t = s->transparency;
        if (transparency_producer_suspended)
            return note(Note::Suspended);
        if (t.source.Get() != source)
            return note(Note::Mismatch);
        if (!t.prep_live || t.prep_submitted || t.anchor_submitted || t.anchor)
            return note(Note::Reused);
        if (t.preparation.Get() == cl && !t.written) {
            t.producer_pending = false;
            t.producer_ready = false;
            return note(Note::ProducerOpen);
        }
        t.anchor = cl;
        t.anchor_live = true;
        t.anchored = true;
        TransparencyNote(Note::Anchored);
        return t.written;
    }
    if (!render_target_proven)
        return note(Note::NotRenderTarget);
    if (cl->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return note(Note::Context);
    const auto d = source->GetDesc();
    if (d.Width > UINT_MAX / 2 || d.Height > UINT_MAX / 2 ||
        !rrtrans::Shape(d, UINT(d.Width * 2), d.Height * 2) ||
        !rrsparsecopy::SameDevice(pipelines.device.Get(), source)) {
        return note(Note::Shape);
    }
    auto *s = TransparencySlot(source);
    if (!s) {
        return note(Note::NoSlot);
    }
    auto &t = s->transparency;
    if (t.source && t.source.Get() != source || t.anchor && t.anchor.Get() != cl) {
        t.written = false;
        t.anchored = false;
        return note(Note::Mismatch);
    }
    if (t.at_draw || t.prep_submitted || t.anchor_submitted || t.preparation && !t.prep_live) {
        return note(Note::Reused);
    }
    if (!s->gpu.rr_output || !s->gpu.transparency.Matches(d, s->gpu.w, s->gpu.h)) {
        return note(Note::Shape);
    }
    try {
        if (!s->gpu.transparency.snapshot && !TransparencyBudget(d, s->gpu.w, s->gpu.h)) {
            return note(Note::Budget);
        }
        transparency_pipeline.Initialize(pipelines.device.Get());
        s->gpu.transparency.Prepare(s->gpu, pipelines, budget, d, s->gpu.w, s->gpu.h);
        // Preserve an earlier preparation reference and live flag: its commands
        // still reference this private snapshot until reset and GPU completion.
        t.source = source;
        if (!t.preparation) {
            t.preparation = cl;
            t.prep_live = true;
        }
        t.anchor = cl;
        t.anchor_live = true;
        rrinternal::Scope copies;
        rrcompute::Tracker::Internal state;
        s->gpu.transparency.Copy(cl, source);
        t.written = true;
        t.anchored = true;
        t.at_draw = true;
        transparency_source = source;
        TransparencyNote(Note::Anchored);
        return true;
    } catch (const std::exception &e) {
        transparency_failed = true;
        t.written = false;
        policy.Reject();
        Log(e.what());
        return note(Note::Failed);
    }
}
bool TransparencyPreparation(ID3D12GraphicsCommandList *cl, ID3D12Resource *source) {
    std::lock_guard lock(mutex);
    if (!enabled || !transparency_enabled || transparency_failed || failed || retired ||
        NeedsReconfigure() || !cl || !source || source != transparency_source.Get())
        return false;
    auto *s = TransparencySlot(source);
    if (!s) {
        return false;
    }
    auto &t = s->transparency;
    // CPU recording order may put an old preparation callback after the final
    // composition copy. Do not overwrite or invalidate the completed ticket.
    if (t.at_draw || t.producer_copy)
        return false;
    if (t.preparation) {
        t.written = false;
        return false;
    }
    if (t.source && t.source.Get() != source || t.anchor.Get() == cl) {
        return false;
    }
    if (!s->gpu.rr_output || !s->gpu.transparency.Matches(source->GetDesc(), s->gpu.w, s->gpu.h) ||
        !rrsparsecopy::SameDevice(pipelines.device.Get(), source)) {
        return false;
    }
    try {
        if (!s->gpu.transparency.snapshot &&
            !TransparencyBudget(source->GetDesc(), s->gpu.w, s->gpu.h)) {
            return false;
        }
        transparency_pipeline.Initialize(pipelines.device.Get());
        s->gpu.transparency.Prepare(s->gpu, pipelines, budget, source->GetDesc(), s->gpu.w,
                                    s->gpu.h);
        t.source = source;
        t.preparation = cl;
        t.prep_live = true;
        rrinternal::Scope copies;
        rrcompute::Tracker::Internal state;
        s->gpu.transparency.Copy(cl, source);
        t.written = true;
        return true;
    } catch (const std::exception &e) {
        transparency_failed = true;
        t.written = false;
        policy.Reject();
        Log(e.what());
        return false;
    }
}
#ifdef RR_SSS_CONTROLLER_TEST
struct TransparencyTestState {
    bool busy{}, preparation_live{}, anchor_live{}, used{}, accepted{}, complete{};
    UINT64 bytes{};
    ID3D12Resource *guide{};
    bool at_draw{}, written{};
};
TransparencyTestState TransparencyStateForTest(UINT frame) {
    std::lock_guard lock(mutex);
    for (auto &s : slots)
        if (s.frame == frame)
            return {s.busy,
                    s.transparency.prep_live,
                    s.transparency.anchor_live,
                    s.transparency_used,
                    s.transparency_accepted,
                    Complete(s),
                    s.gpu.transparency.bytes,
                    s.gpu.transparency.guide.Get(),
                    s.transparency.at_draw,
                    s.transparency.written};
    return {};
}
#endif
