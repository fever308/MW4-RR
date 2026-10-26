// Included inside Controller, after rr_live_transparency.inl. The controller
// side of the transparency albedo blend: the per-slot coverage capture ticket,
// the blend recorded in AfterSR and its submission-time seal. Coverage never
// rejects a frame and never decides frame acceptance; every failure here only
// turns the blend off (blend_failed) and never reaches RR's Failure().
private:
bool blend_enabled = true;
bool blend_failed = false;
rrblend::Reason blend_unavailable = rrblend::Reason::None;
ULONGLONG blend_retry = 0;
rrblend::Counters blend_counters;
rrblend::Tuning blend_tuning;
rrblend::Pipeline blend_pipeline;
ULONGLONG blend_report = 0, blend_logged_at = 0; // the last 10 s check; the last line
bool blend_off_logged{}; // "off" is logged once per switch-off, not every window
unsigned blend_logged_state = 0; // the last line's window state (CoverageReportLocked)
uint64_t blend_mark = 0;         // blend_counters.covered at the last check
ULONGLONG blend_last_ran = 0;
unsigned blend_logged = 0; // one-time log lines: 1 Oit, 2 Refraction, 4 typed, 8 memory

void BlendFailed(const char *what) noexcept {
    blend_counters.last = rrblend::Reason::Failed;
    if (blend_failed)
        return;
    blend_failed = true;
    try {
        char line[256];
        snprintf(line, sizeof(line),
                 "Transparency albedo blend recording failed: %s; disabled until toggled",
                 what ? what : "unknown");
        Log(line);
    } catch (...) {
    }
}
rrblend::Reason CoverageRefused(rrblend::Reason why) {
    if (blend_enabled)
        blend_counters.last = why;
    return why;
}
// The coverage ticket keeps the slot like the glass tickets do: the slot-retire
// check in PollLocked reads rrtrans::Ticket::Live(), which includes this mirror.
void CoverageLive(Slot &s, bool live) {
    s.coverage.live = live;
    s.transparency.coverage_live = live;
}
void CoverageReset(Slot &s, ID3D12GraphicsCommandList *cl) {
    if (s.coverage.capture.Get() != cl)
        return;
    CoverageLive(s, false);
    // A list reset before its submission never ran its captures: the target
    // holds nothing of this frame.
    if (!s.coverage.submitted) {
        s.coverage.cleared = false;
        s.coverage.capture.Reset();
        s.coverage.captures_oit = s.coverage.captures_refr = s.coverage.captures_refr_alpha = 0;
    }
}
// The pool reservation that coverage on every slot needs, beyond what the
// slots already hold: count * (required - the largest coverage a slot holds).
static bool CoveragePoolExtra(size_t count, UINT64 required, UINT64 largest, UINT64 &extra) {
    extra = 0;
    if (required <= largest)
        return true;
    const UINT64 each = required - largest;
    if (count && each > UINT64_MAX / count)
        return false;
    extra = UINT64(count) * each;
    return true;
}
// Reserve the whole pool with coverage on every slot before the first
// coverage allocation, as for the other optional guides. Otherwise coverage
// takes headroom the mandatory pool reservations count on, and those call
// Failure(). Then Prepare. Memory: waiting, retried after 1 s; other
// failures throw.
rrblend::Reason PrepareCoverage(Slot &s, const D3D12_RESOURCE_DESC &scene, ULONGLONG now) {
    auto &g = s.gpu.coverage;
    if (g.Ready())
        return rrblend::Reason::None;
    if (blend_retry && now < blend_retry)
        return rrblend::Reason::Memory;
    const UINT64 required = rrblend::Gpu::RequiredBytes(pipelines.device.Get(), scene);
    Require(required != 0, "coverage allocation size unavailable");
    UINT64 largest{}, extra{};
    for (const auto &slot : slots)
        largest = (std::max)(largest, slot.gpu.coverage.bytes);
    const bool ready = CoveragePoolExtra(slots.size(), required, largest, extra) &&
                       ReservePool(slots.size(), nullptr, nullptr, s.gpu.w, s.gpu.h, extra) &&
                       g.Prepare(s.gpu, pipelines, budget, scene);
    if (ready) {
        blend_retry = 0;
        return rrblend::Reason::None;
    }
    if (!(blend_logged & 8)) {
        blend_logged |= 8;
        char line[160];
        snprintf(line, sizeof(line), "Transparency albedo blend waiting for memory (%llu MiB)",
                 static_cast<unsigned long long>((extra ? extra : required) >> 20));
        Log(line);
    }
    blend_retry = now + 1000;
    return rrblend::Reason::Memory;
}
void CoverageReconfigured() {
    // The slots' coverage resources went with GpuFrame::Release; the pipeline
    // belongs to the retired device.
    blend_pipeline = {};
    blend_unavailable = rrblend::Reason::None;
    blend_retry = 0;
}
void CoverageReportLocked(ULONGLONG now) {
    if (!blend_report) {
        blend_report = blend_logged_at = now;
        return;
    }
    if (now - blend_report < 10000)
        return;
    blend_report = now;
    // Checked every 10 s while RR runs. A line is logged when the window's state (on, failed,
    // unavailable, and whether any transparency was blended) differs from the last line's, or
    // as a 5-minute summary; it counts everything since the last line. A blend that is off
    // says so once.
    if (blend_enabled)
        blend_off_logged = false;
    auto &k = blend_counters;
    if (!enabled || failed || retired || (blend_off_logged && !blend_enabled)) {
        k = {};
        blend_mark = 0;
        blend_logged_state = 0;
        blend_logged_at = now;
        return;
    }
    const char *state = "on";
    unsigned window = 1;
    if (!blend_enabled) {
        state = "off";
        window = 2;
    } else if (blend_failed) {
        state = "failed";
        window = 3;
        k.last = rrblend::Reason::Failed;
    } else if (blend_unavailable != rrblend::Reason::None) {
        state = "unavailable";
        window = 4;
        k.last = blend_unavailable;
    }
    if (k.covered != blend_mark)
        window |= 8;
    blend_mark = k.covered;
    if (blend_enabled && window == blend_logged_state && now - blend_logged_at < 300000)
        return;
    char line[400];
    if (!blend_enabled) {
        snprintf(line, sizeof(line), "Transparency albedo blend: off");
        blend_off_logged = true;
    } else {
        const int n = rrblend::Report(k, line, sizeof(line), (now - blend_logged_at) / 1000);
        if (n > 0 && size_t(n) < sizeof(line))
            snprintf(line + n, sizeof(line) - size_t(n), " changed=%llu state=%s",
                     static_cast<unsigned long long>(k.covered), state);
    }
    Log(line);
    k = {};
    blend_mark = 0;
    blend_logged_state = window;
    blend_logged_at = now;
}
// The stats of this slot's previous blend, once per recorded blend. The slot
// keeps its guide size until it is released, so that blend covered w x h too.
void CoverageHarvest(Slot &s, ULONGLONG now) {
    const auto stats = s.gpu.coverage.Harvest();
    if (!stats.ran)
        return;
    auto &k = blend_counters;
    ++k.ran;
    k.pixels += UINT64(s.gpu.w) * s.gpu.h;
    k.eligible += stats.eligible;
    k.covered += stats.covered;
    k.opacity64 += stats.opacity64;
    blend_last_ran = now;
}
// AfterSR, on the SR list after GateBegin (guides in NON_PIXEL_SHADER_RESOURCE)
// and before evaluate. w x h is the render size, which BeforeSR proved equals
// the slot's guides. Records the blend only when this frame's captures are
// complete; nothing here may reach AfterSR's catch.
void CoverageBlend(Slot &s, ID3D12GraphicsCommandList *cl, UINT w, UINT h) noexcept {
    try {
        const auto now = GetTickCount64();
        CoverageReportLocked(now);
        CoverageHarvest(s, now);
        const auto &t = s.coverage;
        auto &g = s.gpu.coverage;
        if (!blend_enabled || blend_failed || blend_unavailable != rrblend::Reason::None ||
            !blend_pipeline.Ready() || !g.Ready() || (!t.captures_oit && !t.captures_refr) ||
            !t.Live() || !t.cleared || t.submitted || !cl)
            return;
        if (!rrblend::Extent(g.desc, w, h)) {
            CoverageRefused(rrblend::Reason::Extent);
            return;
        }
        g.WriteDescriptors(pipelines, s.gpu.guides);
        g.WriteControl(rrblend::Make(blend_tuning, w, h)); // Admitted 0 until Seal
        rrinternal::Scope copies;
        rrcompute::Tracker::Internal state;
        g.Record(cl, blend_pipeline, s.gpu.guides, w, h);
        s.blend_used = true;
    } catch (const std::exception &e) {
        BlendFailed(e.what());
    } catch (...) {
        BlendFailed("unknown exception");
    }
}
// Before, in the slot's seal block: the blend reads coverage only when its
// captures ran after the prepass and before SR in this submission. Control
// word 0 is written for every sealed slot (other passes gate on it); this
// slot's previous use has completed, so no dispatch is reading it.
void SealCoverage(Slot &s, int pre, int capture, int consumer, bool baseline) noexcept {
    const bool admitted =
        s.blend_used && rrblend::Admit(baseline, pre, capture, consumer, s.coverage.submitted);
    s.gpu.coverage.Seal(admitted);
    if (!s.blend_used)
        return;
    ++blend_counters.frames;
    if (admitted)
        ++blend_counters.admitted;
    else
        CoverageRefused(rrblend::Reason::Order);
}

public:
bool TransparencyBlendEnabled() {
    std::lock_guard lock(mutex);
    return blend_enabled;
}
void SetTransparencyBlendEnabled(bool value) {
    std::lock_guard lock(mutex);
    if (blend_enabled != value) {
        blend_enabled = value;
        blend_failed = false;
        blend_retry = 0;
        policy.Reject();
    }
}
void SetTransparencyBlendTuning(const rrblend::Tuning &tuning) {
    std::lock_guard lock(mutex);
    blend_tuning = rrblend::Clamp(tuning);
}
// signature_hooked: the command signature hook is attached on every device
// (rrsig::Attached() and the registry's SignatureHooked()). oit_refused: the
// coverage registry refused every OIT PSO and accepted none (OitUnavailable).
rrstatus::State TransparencyBlendState(bool signature_hooked, bool oit_refused) {
    std::lock_guard lock(mutex);
    const auto now = GetTickCount64();
    return rrblend::StatusOf(
        blend_enabled, blend_failed,
        blend_unavailable != rrblend::Reason::None || !signature_hooked || oit_refused, enabled,
        failed, retired, blend_last_ran && now - blend_last_ran <= 1000);
}
void CoverageReport() {
    std::lock_guard lock(mutex);
    CoverageReportLocked(GetTickCount64());
}
// A granted capture. It holds the controller lock until CoverageCaptured or
// CoverageFailed, so the slot and its coverage resources cannot change while
// the caller records: BeginCapture(cl, first) on *gpu, the variant PSO and the
// owned RTV (rtv), the replay, the native state restore, EndCapture(cl).
struct CoverageToken {
    std::unique_lock<rrlock::Recursive> lock;
    Slot *slot{};
    ID3D12GraphicsCommandList *cl{};
    rrblend::Gpu *gpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    bool first{}; // no capture has cleared the target on this list yet
    explicit operator bool() const {
        return slot && lock.owns_lock();
    }
};
// Every check runs here, before the caller touches cl. scene: the native
// render target the capture mirrors (its size and samples); sample: the
// variant PSO's sample description (registry Entry::sample).
CoverageToken CoverageCapture(ID3D12GraphicsCommandList *cl, ID3D12Resource *scene,
                              DXGI_SAMPLE_DESC sample) {
    CoverageToken token{std::unique_lock(mutex)};
    if (!blend_enabled || blend_failed || blend_unavailable != rrblend::Reason::None || !enabled ||
        failed || retired || NeedsReconfigure() || !cl || !scene) {
        if (blend_enabled && !blend_failed)
            CoverageRefused(blend_unavailable != rrblend::Reason::None ? blend_unavailable
                                                                       : rrblend::Reason::Disabled);
        return {};
    }
    const auto desc = scene->GetDesc();
    if (cl->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || !rrblend::Gpu::Shape(desc) ||
        !rrsparsecopy::SameDevice(pipelines.device.Get(), scene)) {
        CoverageRefused(rrblend::Reason::Extent);
        return {};
    }
    auto *s = TransparencySlot(scene);
    if (!s) {
        CoverageRefused(rrblend::Reason::NoSlot);
        return {};
    }
    auto &t = s->coverage;
    if (t.capture && t.capture.Get() != cl) {
        CoverageRefused(rrblend::Reason::Split);
        return {};
    }
    if (t.submitted || s->recorded) {
        CoverageRefused(rrblend::Reason::Order);
        return {};
    }
    if (!s->gpu.w || !s->gpu.h || !s->gpu.coverage.Matches(desc) ||
        !rrblend::Extent(rrblend::CoverageDesc(desc), s->gpu.w, s->gpu.h)) {
        CoverageRefused(rrblend::Reason::Extent);
        return {};
    }
    if (sample.Count != desc.SampleDesc.Count || sample.Quality != desc.SampleDesc.Quality) {
        CoverageRefused(rrblend::Reason::NoPipeline);
        return {};
    }
    const auto now = GetTickCount64();
    try {
        if (!blend_pipeline.Ready() && blend_retry && now < blend_retry) {
            CoverageRefused(rrblend::Reason::Memory);
            return {};
        }
        if (!blend_pipeline.Prepare(pipelines)) {
            blend_unavailable = blend_pipeline.unavailable;
            if (!(blend_logged & 4)) {
                blend_logged |= 4;
                Log("Transparency albedo blend unavailable: typed UAV load of "
                    "R16G16B16A16_FLOAT unsupported");
            }
            CoverageRefused(blend_unavailable);
            return {};
        }
        const auto prepared = PrepareCoverage(*s, desc, now);
        if (prepared != rrblend::Reason::None) {
            CoverageRefused(prepared);
            return {};
        }
    } catch (const rrprivate::AllocationDeferred &) {
        // The blend pipeline's creation was out of memory: retry after 1 s.
        blend_retry = now + 1000;
        CoverageRefused(rrblend::Reason::Memory);
        return {};
    } catch (const std::exception &e) {
        BlendFailed(e.what());
        return {};
    } catch (...) {
        BlendFailed("unknown exception");
        return {};
    }
    // The caller records into the target from here on: the list now holds it.
    t.capture = cl;
    CoverageLive(*s, true);
    token.slot = s;
    token.cl = cl;
    token.gpu = &s->gpu.coverage;
    token.rtv = s->gpu.coverage.rtv->GetCPUDescriptorHandleForHeapStart();
    token.first = !t.cleared;
    return token;
}
// The caller recorded the whole capture sequence. alpha: a Refraction variant
// that also accumulates alpha (registry Entry::refraction_alpha).
void CoverageCaptured(CoverageToken &&token, rrblend::Kind kind, bool alpha = false) {
    const auto held = std::move(token);
    if (!held)
        return;
    auto &t = held.slot->coverage;
    t.cleared = true;
    if (kind == rrblend::Kind::Oit) {
        ++t.captures_oit;
        ++blend_counters.captures_oit;
    } else {
        ++t.captures_refr;
        ++blend_counters.captures_refr;
        if (alpha)
            ++t.captures_refr_alpha;
    }
    const unsigned bit = kind == rrblend::Kind::Oit ? 1 : 2;
    if (!(blend_logged & bit)) {
        blend_logged |= bit;
        char line[192];
        snprintf(line, sizeof(line), "Transparency albedo blend: first %s captured (%llux%u, x%u)",
                 kind == rrblend::Kind::Oit ? "OIT resolve" : "refraction draw",
                 static_cast<unsigned long long>(held.gpu->desc.Width), held.gpu->desc.Height,
                 held.gpu->desc.SampleDesc.Count);
        Log(line);
    }
}
// A capture sequence failed after the caller touched cl. The ticket stays live
// (the list references the target); the blend is off until toggled.
void CoverageFailed(CoverageToken &&token, const char *what) {
    const auto held = std::move(token);
    if (held)
        BlendFailed(what);
}
// The render glue refused an OIT resolve whose command signature is not
// Replayable (unknown, partial root constants or unsupported): no capture.
void CoverageSignatureRefused() {
    std::lock_guard lock(mutex);
    CoverageRefused(rrblend::Reason::Signature);
}
