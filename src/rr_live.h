#pragma once
#include "rr_live_gpu.h"
#include "rr_sss_coverage_gpu.h"
#include "rr_live_policy.h"
#include "rr_guide_health.h"
#include "rr_water.h"
#include "rr_weapon_depth.h"
#include "rr_fg_depth.h"
#include "rr_status.h"
#include "rr_compute_state.h"
#include "rr_sparse_copy.h"
#include "rr_sss_producer.h"
#include "rr_sss_mask.h"
#include "rr_inputs.h"
#include "rr_internal.h"
#include "rr_lock.h"
#include "rr_ngx_api.h"
#include "rr_ngx_log.h"
#include "rr_trace.h"
#include "rr_sr_eligibility.h"
#include "rr_transparency_shaders.h"
#include <atomic>
#include <mutex>
#include <deque>
#include <initializer_list>
#include <vector>
namespace rrlive {
// Private resources remain retained until native recordings retire and every
// submitted producer and consumer completes on the tracked fence.
class Controller {
    friend struct ControllerCpuTest;
    struct Observed {
        D3D12_RESOURCE_STATES state{};
        bool known{}, complete{};
    };
    struct Slot {
        rrsss::Producer sss;
        bool sss_used{}, awaiting_prepass{}, hit_used{}, water_used{};
        rrtrans::Ticket transparency;
        bool transparency_used{}, transparency_accepted{};
        bool responsivity_used{}; // RR was given the water responsivity mask
        GpuFrame gpu;
        bool busy{};
        // This frame's coverage captures, read by CoverageBlend in AfterSR and reset
        // only with the slot.
        rrblend::Ticket coverage;
        bool blend_used{};
        ComPtr<ID3D12GraphicsCommandList> pre, sr;
        bool pre_live{}, sr_live{}, pending_sr{}, recorded{}, sealed{}, inference_ok{}, reported{},
            poisoned{};
        UINT frame = UINT_MAX, viewport = UINT_MAX, generation{}, serial{};
        bool raw_corrected{}, native_sampling{}, pre_submitted{}, gate_recorded{}, replaced_sr{};
        UINT64 lighting_device{}, lighting_revision{};
        UINT64 fence_value{};
        rrtrace::CameraInfo camera;
        std::array<float, 16> view{}, projection{};
        // Weapon depth for Frame Generation (rr_fg_depth.h): AfterSR wrote gpu.fg_depth for
        // this frame (fg_prepared), or why not.
        bool fg_prepared{};
        rrfgdepth::Reason fg_prepare{rrfgdepth::Reason::NotRecorded};
        // The list Frame Generation's depth was tagged on, when not the SR list: its copy reads
        // gpu.fg_depth, so the slot waits for it (fg_list_live until submitted or reset, then
        // the slot's fence covers that submission).
        ComPtr<ID3D12GraphicsCommandList> fg_list;
        bool fg_list_live{};
        // The content check (FgProbe) was recorded on fg_list for this frame, and submitted.
        bool fg_probe{}, fg_probe_submitted{};
        std::array<Observed, 4> states{};
        rreval::Options options;
    };
    struct Call {
        bool locked{};
        std::vector<bool> touched;
    };
    static inline thread_local std::vector<Call> submissions;
    rrlock::Recursive mutex;
    // An NGX call into the add-ons beneath RR is running with the lock released (Foreign).
    bool foreign_call{};
    // Objects a timed-out initialization left to the GPU: kept for the process.
    std::vector<ComPtr<IUnknown>> stranded;
    std::deque<Slot> slots{6};
    Pipelines pipelines;
    Budget budget;
    GpuFrame persistent;
    ComPtr<ID3D12Resource> history, zero;
    ComPtr<ID3D12CommandAllocator> init_allocator;
    ComPtr<ID3D12GraphicsCommandList> init_list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandQueue> last_queue;
    UINT64 signaled{};
    bool enabled{}, failed{}, initialized{}, retired{};
    // A new RR allocation found no GPU memory: native frames until memory_retry_after (ms), and
    // one log line per wait.
    ULONGLONG memory_retry_after{};
    bool memory_logged{};
    bool MemoryWait() {
        if (!memory_retry_after)
            return false;
        if (GetTickCount64() < memory_retry_after)
            return true;
        memory_retry_after = 0;
        return false;
    }
    void MemoryDeferred(const char *what) {
        memory_retry_after = GetTickCount64() + 1000;
        if (memory_logged)
            return;
        memory_logged = true;
        const std::string line = std::string(what) + " waiting for GPU memory; native frames "
                                                     "until it fits, retried every second";
        Log(line.c_str());
    }
    bool replace_sr = true;      // skip the game's SR when RR can take its place (ReplaceNativeSR)
    unsigned replace_refusals{}; // replacement frames RR refused in a row
    bool replace_paused{};       // after 3 refusals, until the setting changes
    UINT generation{}, camera_frame = UINT_MAX, camera_view = UINT_MAX, selected_view = UINT_MAX,
                       next_serial{};
    rrtrace::CameraInfo latest;
    UINT64 camera_time{};
    rrstatus::Telemetry status;
    rrpreset::Selection preset;
    bool layout_pending{};
    rreval::Options observed_options;
    HistoryPolicy policy;
    rrhealth::Tracker health;
    bool tolerate_isolated = true;
    // Weapon depth's depth copy, shared by every slot, created on first use and
    // released by Reconfigure. It sits in the persistent ledger, so pool projections count it
    // once, and the reserve grows by exactly its size (test_reserve, given back on release).
    struct DevTest {
        ComPtr<ID3D12Resource> texture;
        bool active{};     // the latest evaluation used it
        bool blocked{};    // no GPU memory: retried once its setting changes
        unsigned changes{}; // the setting's change count when it was blocked
    };
    // weapon_test: the remapped depth copy RR reads while weapon depth is on.
    DevTest weapon_test;
    UINT64 test_reserve{};
    // Weapon depth (rrweapondepth): weapon_linear is whether the current RR feature was
    // created to read the weapon depth copy; weapon_hold says why weapon depth stays off until
    // the setting next changes. The hold outlives Reconfigure, so weapon depth that cannot
    // run restarts RR once, never over and over.
    using WeaponHold = rrweapondepth::Hold;
    bool weapon_linear{};
    WeaponHold weapon_hold{};
    unsigned weapon_hold_changes{};
    rrweapondepth::Mapping weapon_logged{}; // The mapping last written to the log.
    // Weapon depth for Frame Generation (rr_fg_depth.h). fg_paused: Streamline refused the
    // replacement RefusalLimit times in a row; cleared when the setting is off. Textures live
    // in the slots (GpuFrame::fg_depth); their reserve growth is part of test_reserve.
    bool fg_paused{}, fg_memory_logged{}, fg_supplied_logged{};
    unsigned fg_refusals{}, fg_diagnostics{}, fg_changes{}, fg_pairs{};
    UINT64 fg_memory_retry_at{}, fg_summary_at{};
    struct FgSignature {
        UINT lifecycle{}, state{}, extent_w{}, extent_h{}, width{}, height{}, format{}, inverted{},
            motion{}, jittered{}, offset_x{}, offset_y{}, render_w{}, render_h{};
        bool same_list{}, same_depth{}, slot{}, submitted{};
        // The tagged depth's own description, and the frame delta of the RR slot whose SR depth
        // it is (NoMatch: none of the slots').
        UINT fg_width{}, fg_height{}, fg_format{}, fg_flags{};
        int match{NoMatch};
        static constexpr int NoMatch = INT_MIN;
        bool operator==(const FgSignature &) const = default;
    } fg_logged{};
    // The content check (rrfgdepth::Compare; FgWeaponDepthObserve=1 only): a readback of the game's
    // Frame Generation depth plane (first half) and this frame's SR depth plane (second half, from
    // GpuFrame::depth_buffer), both copied on Frame Generation's list at its tag. armed: recorded,
    // waiting for the slot, which waits for that list, to retire; ready: its GPU work is done.
    struct FgProbe {
        ComPtr<ID3D12Resource> readback;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 half{}, armed_at{};
        UINT frame{};
        bool armed{}, ready{};
    } fg_probe;
    unsigned fg_probes{};
    UINT64 fg_probe_at{};
    bool fg_probe_stuck{}; // never completed: no more checks this session (the readback stays)
    // Water responsivity mask (rr_water.h).
    rrwater::Settings water;
    rrwater::Status water_status;
    bool water_failed{};
    UINT64 water_retry_after{};
    bool transparency_enabled{}, transparency_failed{};
    rrtrans::Pipeline transparency_pipeline;
    ComPtr<ID3D12Resource> transparency_source;
    bool sss_enabled{}, sss_failed{};
    UINT64 sss_retry_after{};
    // An error turns RR or an optional guide off for a pause, not for the session: 5 s,
    // doubling for repeats up to 60 s, back to 5 s after 5 minutes without one. RR itself
    // restarts through Reconfigure, which waits for its outstanding GPU work and never retires
    // a poisoned slot, so RR stays off when that work cannot be accounted for. fatal: GPU
    // objects were stranded or NGX refused to release them, and RR stays off.
    struct Pause {
        UINT64 at{}, lifted{};
        unsigned repeats{};
        UINT64 Schedule(UINT64 now) {
            if (now - lifted > 300000)
                repeats = 0;
            const UINT64 delay = (std::min)(5000ull << (std::min)(repeats, 4u), 60000ull);
            at = now + delay;
            return delay;
        }
    };
    Pause rr_pause, sss_pause, transparency_pause, water_pause, blend_pause;
    bool fatal{};
    // Latest guide an admitted SR submission resolves, for SSS-masked lighting.
    rrsssmask::Record sss_mask;
    ComPtr<ID3D12Resource> sss_mask_guide;
    // Subsurface Scattering coverage experiment (rr_sss_coverage.h): the history ring, the
    // record sealed with the latest admitted guide, and the panel's counts. The ring is in the
    // persistent ledger; coverage_reserve is the budget growth it added, given back by
    // Reconfigure with test_reserve.
    CoverageHistory coverage_history;
    rrssscov::Record coverage_record;
    bool coverage_failed{};
    UINT64 coverage_retry_after{};
    rrssscov::Totals coverage_window, coverage_last;
    UINT64 coverage_window_start{};
    UINT64 coverage_key_skips{}; // surface keys skipped because the entry was still in flight
    rrssscov::FrameReason coverage_reason = rrssscov::FrameReason::NoRecord;
    bool hit_enabled{};
    unsigned hit_reports{};
    HMODULE runtime{};
    UINT64 runtime_epoch{};
    ComPtr<ID3D12Device> public_device;
    // A creation attempt's runtime, logged by Poll after it releases the mutex: module and DXGI
    // queries never run under it. The probe pins the NGX core module.
    struct RuntimeReport {
        HMODULE core{};
        ComPtr<ID3D12Device> device;
    } runtime_report;
    rreval::Api api;
    NVSDK_NGX_Parameter *parameters{};
    NVSDK_NGX_Handle *feature{};
    rreval::Options key;
    rreval::Api (*resolve)(HMODULE) = &rreval::Api::Load;
    // A call into NGX, where an add-on beneath RR (DLSS5 NR) may wait for another thread's NGX
    // call, Frame Generation's, which records barriers RR observes under this lock. The call runs
    // with every hold released. The slot being recorded stays live (sr_live), so no
    // reconfiguration, shutdown or slot reuse retires what the call uses, and foreign_call
    // refuses another SR call meanwhile.
    template <class F> auto Foreign(F &&f) -> decltype(f()) {
        struct Flag {
            bool &v;
            ~Flag() {
                v = false;
            }
        } flag{foreign_call = true};
        return rrlock::Released(mutex, f);
    }
    void Log(const char *s) {
        if (logger)
            logger(s);
    }

    // Bytes that the pool projections count once, not per slot: the persistent allocations and
    // every slot's Frame Generation depth texture (SlotReservationParts leaves it out).
    UINT64 Kept() const {
        UINT64 total = persistent.bytes;
        for (const auto &s : slots)
            total += s.gpu.fg_bytes;
        return total;
    }
    // Whether a shared texture (weapon depth's) is used in this evaluation; creates it on
    // first use. The reserve first grows by the texture's size on top of everything it
    // already holds, so the texture never takes room the pool or the SSS and transparency
    // guides counted on (Reconfigure gives the growth back). This needs free VRAM for the
    // whole unused reserve, so it can stay off on a tight card; a failure only keeps it off
    // (RR keeps running) until its setting changes.
    bool UseTest(DevTest &t, bool wanted, unsigned changes, UINT w, UINT h, DXGI_FORMAT format,
                 const char *name) {
        t.active = false;
        if (!wanted)
            return false;
        if (t.blocked && t.changes == changes)
            return false;
        t.blocked = false;
        if (!t.texture) {
            const auto d = TextureDesc(w, h, format);
            const UINT64 n = pipelines.device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
            const UINT64 old = budget.limit;
            bool ok = n && n != UINT64_MAX && old <= UINT64_MAX - n &&
                      budget.Reserve(pipelines.device.Get(), old + n);
            if (ok) {
                try {
                    // Weapon depth is the depth RR hands DLSS: announced like RR's own depth.
                    t.texture = persistent.Allocate(pipelines, budget, d, D3D12_HEAP_TYPE_DEFAULT,
                                                    nullptr, true);
                    test_reserve += budget.limit - old;
                } catch (const std::exception &) {
                    budget.limit = old;
                    ok = false;
                }
            }
            if (!ok) {
                t.blocked = true;
                t.changes = changes;
                char line[160];
                snprintf(line, sizeof(line),
                         "%s: not enough GPU memory; off until its setting changes", name);
                Log(line);
                return false;
            }
        }
        const auto d = t.texture->GetDesc();
        if (d.Width != w || d.Height != h || d.Format != format)
            return false;
        return t.active = true;
    }

    template <class Prepare>
    bool PrepareSssAllocation(UINT64 now, bool ready, Prepare prepare) {
        // Preparation records no GPU commands. Retain partial allocations and
        // retry them after budget pressure subsides; ready slots remain usable.
        if (!ready && now < sss_retry_after)
            return false;
        try {
            prepare();
            if (!ready)
                sss_retry_after = 0;
            return true;
        } catch (const rrprivate::AllocationDeferred &) {
            if (!sss_retry_after)
                Log("Subsurface Scattering guide waiting for GPU memory; allocation will retry");
            sss_retry_after = now + 1000;
            return false;
        }
    }
    // The mandatory depth copy for a w x h input, as the SSS and glass
    // reservations protect it.
    UINT64 DepthCopyBytes(UINT w, UINT h) const {
        if (!pipelines.device)
            return 0;
        auto texture = TextureDesc(w, h, DXGI_FORMAT_R32_FLOAT);
        texture.Flags = D3D12_RESOURCE_FLAG_NONE;
        const auto buffer = rrprivate::Buffer(((UINT64(w) * 4 + 255) & ~255ull) * h);
        UINT64 total{};
        for (const auto &d : {texture, buffer}) {
            const auto n = pipelines.device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
            if (!n || n == UINT64_MAX || total > UINT64_MAX - n)
                return 0;
            total += n;
        }
        return total;
    }
    // Water storage is optional and all or nothing: a slot runs the water mask
    // only while every prepared slot holds the storage, so RR never sees them
    // switch on and off from frame to frame. Like the SSS storage it waits for
    // a mandatory SR allocation, then reserves the whole pool with water
    // storage on every slot plus the prepass and depth copy the SSS and glass
    // reservations protect, so no later mandatory reservation needs more memory
    // because of it. A slot used for the first time later gets its storage at
    // that first prepass. A budget or memory shortage leaves the water mask
    // waiting and retries a second later; any other failure turns them off for
    // the session instead of turning RR off.
    bool PrepareWater(GpuFrame &frame, UINT64 now) {
        bool complete = frame.water.ready, warm = frame.rr_output != nullptr;
        for (const auto &s : slots) {
            complete = complete && (!s.gpu.heap || s.gpu.water.ready);
            warm = warm || s.gpu.rr_output != nullptr;
        }
        if (complete)
            return true;
        if (!warm || now < water_retry_after)
            return false;
        try {
            const auto need = GpuFrame::WaterBytes(pipelines.device.Get(), frame.w, frame.h);
            const auto depth = need ? DepthCopyBytes(frame.w, frame.h) : 0;
            bool ready = depth && ReservePool(slots.size(), nullptr, nullptr, frame.w, frame.h,
                                              0, need, depth);
            for (auto &s : slots)
                if (ready && s.gpu.heap && !s.gpu.water.ready)
                    ready = s.gpu.PrepareWater(pipelines, budget);
            if (ready) {
                water_retry_after = 0;
                return true;
            }
            if (!water_retry_after)
                Log("Water mask waiting for GPU budget; allocation will retry");
        } catch (const rrprivate::AllocationDeferred &) {
            if (!water_retry_after)
                Log("Water mask waiting for GPU memory; allocation will retry");
        } catch (const std::exception &e) {
            water_failed = true;
            const std::string line = std::string("Water mask unavailable: ") + e.what();
            Log(line.c_str());
            return false;
        }
        water_retry_after = now + 1000;
        return false;
    }
    // done is the fence's completed value, which PollLocked reads once for every slot.
    bool Complete(const Slot &s, UINT64 done) const {
        if (s.poisoned)
            return false;
        if (!s.fence_value)
            return !s.sealed;
        return done != UINT64_MAX && done >= s.fence_value;
    }
    bool Complete(const Slot &s) const {
        return Complete(s, (s.poisoned || !s.fence_value) ? 0 : fence->GetCompletedValue());
    }
    void Failure(const char *why, bool permanent = false) {
        const bool first = !failed;
        failed = true;
        fatal |= permanent;
        // A poisoned slot never retires: its GPU work is unaccounted for.
        for (const auto &s : slots)
            fatal |= s.poisoned;
        policy.Reject();
        if (!first || fatal || retired) {
            Log(why);
            return;
        }
        char line[320];
        snprintf(line, sizeof(line), "%s; RR restarts in %llu s", why,
                 (unsigned long long)(rr_pause.Schedule(GetTickCount64()) / 1000));
        Log(line);
    }
    // At a native rendering boundary: restart RR once its pause is over and every slot retires.
    bool Recover() {
        if (!failed)
            return true;
        const UINT64 now = GetTickCount64();
        if (fatal || retired || !rr_pause.at || now < rr_pause.at)
            return false;
        layout_pending = true;
        if (!Reconfigure() || fatal)
            return false;
        failed = false;
        rr_pause.at = 0;
        rr_pause.lifted = now;
        ++rr_pause.repeats;
        Log("RR restarted after an error");
        return true;
    }
    // In PollLocked: an optional guide turned off by an error comes back after its pause.
    void Resume(bool &off, Pause &p, UINT64 now, const char *what) {
        if (!off)
            return;
        if (!p.at) {
            p.Schedule(now);
            return;
        }
        if (now < p.at)
            return;
        off = false;
        p.at = 0;
        p.lifted = now;
        ++p.repeats;
        char line[128];
        snprintf(line, sizeof(line), "%s: retrying after an error", what);
        Log(line);
    }
    // A failed retirement call logs its NGX result; a missing export has none.
    bool RetirementFailed(const char *what, bool exported, NVSDK_NGX_Result result) {
        char line[160];
        if (exported)
            snprintf(line, sizeof(line), "%s (%s); allocations retained", what,
                     rrngxlog::Code(result).text);
        else
            snprintf(line, sizeof(line), "%s (export missing); allocations retained", what);
        Failure(line, true);
        return false;
    }
    void Initialize(ID3D12Device *d) {
        if (initialized) {
            Require(fg::SameNativeObject(pipelines.device.Get(), d),
                    "live device generation was not retired before device change");
            return;
        }
        const UINT64 before = persistent.bytes;
        try {
            InitializeLocked(d);
        } catch (...) {
            // Under GPU memory pressure, for one: back to uninitialized, so a later frame
            // initializes from the start.
            history.Reset();
            zero.Reset();
            persistent.Unwind(budget, before);
            init_list.Reset();
            init_allocator.Reset();
            fence.Reset();
            pipelines = {};
            throw;
        }
    }
    void InitializeLocked(ID3D12Device *d) {
        pipelines.Initialize(d);
        auto bd = rrprivate::Buffer(256);
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        history = persistent.Allocate(pipelines, budget, bd);
        zero =
            persistent.Allocate(pipelines, budget, rrprivate::Buffer(256), D3D12_HEAP_TYPE_UPLOAD);
        void *m{};
        Check(zero->Map(0, nullptr, &m), "live history init map failed");
        memset(m, 0, 256);
        zero->Unmap(0, nullptr);
        Check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        IID_PPV_ARGS(&init_allocator)),
              "live init allocator failed");
        Check(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, init_allocator.Get(), nullptr,
                                   IID_PPV_ARGS(&init_list)),
              "live init list failed");
        Change(init_list.Get(), history.Get(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_COPY_DEST);
        init_list->CopyBufferRegion(history.Get(), 0, zero.Get(), 0, 256);
        Change(init_list.Get(), history.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_COMMON);
        Check(init_list->Close(), "live init close failed");
        Check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
              "live timeline fence failed");
        // Clear the history on a private queue and wait for it here, not from inside the
        // game's ExecuteCommandLists: a nested submission there can reach a compute queue, and
        // another add-on's submit hook (DLSS5 NR's) loses track of the outer submission.
        ComPtr<ID3D12CommandQueue> setup_queue;
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(d->CreateCommandQueue(&qd, IID_PPV_ARGS(&setup_queue)), "live init queue failed");
        ComPtr<ID3D12Fence> setup_fence;
        Check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&setup_fence)),
              "live init fence failed");
        {
            rrinternal::Scope guard;
            ID3D12CommandList *setup = init_list.Get();
            setup_queue->ExecuteCommandLists(1, &setup);
        }
        Check(setup_queue->Signal(setup_fence.Get(), 1), "live init signal failed");
        if (!rrprivate::FencePoint{setup_fence, 1}.Wait(5000)) {
            // The GPU may still run the clear: what it uses is never released, and RR stays off.
            for (IUnknown *object : std::initializer_list<IUnknown *>{
                     history.Get(), zero.Get(), init_list.Get(), init_allocator.Get(),
                     setup_queue.Get(), setup_fence.Get()})
                stranded.emplace_back(object);
            failed = true;
            fatal = true;
            throw std::runtime_error("live init unfinished; RR off");
        }
        initialized = true;
    }
    bool NeedsReconfigure() const {
        return layout_pending || (preset.has_effective && preset.desired != preset.effective) ||
               (feature && weapon_linear != WeaponDepthWanted());
    }
    // The weapon depth setting, unless held off since its last change. RR's depth
    // type is fixed at creation, so a new feature takes it and a change restarts RR.
    bool WeaponDepthWanted() const {
        return rrweapondepth::enabled.load(std::memory_order_relaxed) &&
               (weapon_hold == WeaponHold::None ||
                weapon_hold_changes != rrweapondepth::changes.load(std::memory_order_relaxed));
    }
    // outcome (Refused): why NGX refused, an rrngxlog::Code text, "null handle" or "threw".
    void HoldWeaponDepth(WeaponHold why, unsigned changes, const char *outcome = nullptr) {
        // Frames already in flight on the same RR repeat the same hold: log it once.
        if (weapon_hold == why && weapon_hold_changes == changes)
            return;
        weapon_hold = why;
        weapon_hold_changes = changes;
        // UseTest already logged a memory failure.
        if (why == WeaponHold::Projection)
            Log("Weapon depth: the game's projection is not supported; off until "
                "the setting changes, RR runs without it");
        else if (why == WeaponHold::Refused) {
            char line[200];
            snprintf(line, sizeof(line),
                     "Weapon depth: RR failed with the weapon depth copy%s%s%s; off until the "
                     "setting changes, RR runs without it",
                     outcome ? " (" : "", outcome ? outcome : "", outcome ? ")" : "");
            Log(line);
        }
    }
    void QueueReconfigure() {
        layout_pending = true;
        policy.Reject();
        status.Fallback(generation, GetTickCount64());
    }
    // Called only at native rendering boundaries, never by panel methods. Poll
    // retirement first: a reset recording can still have outstanding submissions.
    bool Reconfigure() {
        if (!NeedsReconfigure())
            return true;
        if (foreign_call)
            return false;
        for (const auto &s : slots)
            if (s.fg_list_live) // Frame Generation's copy of a slot texture is not submitted yet
                return false;
        for (const auto &s : slots)
            if (!rrpreset::CanRetire(
                    s.poisoned, s.pre_live, s.sr_live, s.sss.live,
                    s.transparency.Live() || s.coverage.Live(), s.pending_sr,
                    s.recorded && !s.sealed && s.sr_live,
                    (s.pre_submitted || s.sss.submitted || s.transparency.prep_submitted ||
                     s.transparency.anchor_submitted || s.coverage.submitted) &&
                        !s.fence_value,
                    Complete(s)))
                return false;
        if (fence && signaled) {
            const auto done = fence->GetCompletedValue();
            if (done == UINT64_MAX || done < signaled)
                return false;
        }
        rrinternal::Scope copies;
        rrcompute::Tracker::Internal state;
        if (feature) {
            const bool exported = api.release != nullptr;
            const auto r = exported ? api.release(feature) : NVSDK_NGX_Result_Fail;
            if (!exported || NVSDK_NGX_FAILED(r))
                return RetirementFailed("RR feature retirement failed", exported, r);
            feature = nullptr;
            preset.Retired();
        }
        if (parameters) {
            const bool exported = api.destroy != nullptr;
            const auto r = exported ? api.destroy(parameters) : NVSDK_NGX_Result_Fail;
            if (!exported || NVSDK_NGX_FAILED(r))
                return RetirementFailed("RR parameter retirement failed", exported, r);
            parameters = nullptr;
        }
        for (auto &s : slots) {
            s.gpu.Release(budget);
            s = {};
        }
        water_retry_after = 0;
        sss_mask = {};
        sss_mask_guide.Reset();
        coverage_record = {};
        // Its allocations are in the persistent ledger released below; its budget growth is
        // in test_reserve.
        coverage_history.Release();
        coverage_retry_after = 0;
        history.Reset();
        zero.Reset();
        persistent.Release(budget);
        // The weapon depth texture was in the persistent ledger; give back the reserve it added
        // (test_reserve, saturating), never below what is still allocated.
        weapon_test = {};
        weapon_linear = false;
        weapon_logged = {};
        budget.limit = (std::max)(
            {Budget::Minimum, budget.bytes, budget.limit - (std::min)(test_reserve, budget.limit)});
        test_reserve = 0;
        init_list.Reset();
        init_allocator.Reset();
        fence.Reset();
        last_queue.Reset();
        pipelines = {};
        transparency_pipeline = {};
        transparency_source.Reset();
        transparency_composition = {};
        transparency_note = {};
        transparency_counters = {};
        transparency_window = transparency_logged_at = 0;
        transparency_failing = false;
        CoverageReconfigured();
        initialized = false;
        signaled = 0;
        runtime = nullptr;
        runtime_epoch = 0;
        public_device.Reset();
        api = {};
        selected_view = UINT_MAX;
        key = {};
        if (++generation == 0) {
            Failure("RR feature generation overflow", true);
            return false;
        }
        status.Reset(generation);
        policy.Reject();
        preset.Retired();
        layout_pending = false;
        return true;
    }
    bool Key(const rreval::Options &o, HMODULE mod, UINT64 epoch, ID3D12Device *d, UINT viewport) {
        if (!feature)
            return true;
        return runtime == mod && runtime_epoch == epoch && public_device.Get() == d &&
               selected_view == viewport && rreval::SameCreation(key, o);
    }
    Slot *Match(rrtrace::SlCall call) {
        if (!call.valid || call.feature != sl::kFeatureDLSS)
            return nullptr;
        Slot *result{};
        for (auto &s : slots)
            if (s.pre_live && s.frame == call.frame && s.viewport == call.viewport && !s.recorded) {
                if (result)
                    return nullptr;
                result = &s;
            }
        return result;
    }
    void PollLocked() {
        // One read for both checks of every slot, so a slot never retires a frame it did not
        // report. Only a signaled slot has a fence value, and only with a fence.
        const UINT64 done = fence ? fence->GetCompletedValue() : 0;
        for (auto &s : slots) {
            if (s.recorded && s.sealed && !s.reported && Complete(s, done)) {
                auto v = s.gate_recorded ? s.gpu.ReadStats() : std::array<UINT, 4>{1, 0, 0, 0};
                s.reported = true;
                // After reported, as the readers below: a failed map is then reported once.
                const auto finite = s.gate_recorded ? s.gpu.ReadFinite() : std::array<UINT, 2>{};
                const bool sss = s.sss_used && s.sss.admission.accepted;
                const bool glass = s.transparency_used && s.transparency_accepted;
                CoverageCounters(s);
                // A frame whose gate never ran never reached RR: no guide counts for it.
                health.Frame(v, {finite[0], finite[1], sss, glass, s.gate_recorded && s.hit_used,
                                 s.gate_recorded && s.responsivity_used});
                status.Completed(s.serial, s.generation, GetTickCount64(), v[0] == 0 && v[1] != 0,
                                 sss, glass, s.replaced_sr, s.hit_used);
                if (v[0] || !v[1]) {
                    policy.Reject();
                }
            }
            if (s.busy && !s.pre_live && !s.sr_live && !s.sss.live && !s.transparency.Live() &&
                !s.fg_list_live &&
                !s.pending_sr && Complete(s, done)) {
                // The content check's copies ran in a submission this slot's fence covers.
                if (s.fg_probe && s.fg_probe_submitted && fg_probe.armed) {
                    fg_probe.armed = false;
                    fg_probe.ready = true;
                }
                auto cache = std::move(s.gpu);
                cache.Retire();
                // Weapon depth for Frame Generation turned off, or Frame Generation tags no
                // depth any more: its textures go as slots retire.
                if (!rrfgdepth::enabled.load(std::memory_order_relaxed) ||
                    !rrfgdepth::status.FgSeen(GetTickCount64()))
                    ReleaseFgTexture(cache);
                s = {};
                s.gpu = std::move(cache);
                s.busy = false;
            }
        }
        // Idle slots keep their texture between frames: they go too once it is unused. An idle
        // slot's last frame has completed (it went idle above).
        if (!rrfgdepth::enabled.load(std::memory_order_relaxed) ||
            !rrfgdepth::status.FgSeen(GetTickCount64()))
            for (auto &s : slots)
                if (!s.busy && s.gpu.fg_depth)
                    ReleaseFgTexture(s.gpu);
        const UINT64 now = GetTickCount64();
        Resume(sss_failed, sss_pause, now, "Subsurface Scattering guide");
        Resume(transparency_failed, transparency_pause, now, "Transparency guide");
        Resume(water_failed, water_pause, now, "Water responsivity mask");
        Resume(blend_failed, blend_pause, now, "Transparency albedo blend");
        std::string report;
        if (health.Report(now, report))
            Log(report.c_str());
    }
    // A submitted SR recording that resolves its guide hands it to later
    // SSS-masked lighting if admitted. A rejected one writes zeros, so a record
    // of that same guide is dropped. A discarded recording never gets here.
    void SealSssMask(const Slot &s, bool accepted) {
        if (accepted) {
            sss_mask = {true, s.frame, s.generation, s.gpu.w, s.gpu.h};
            sss_mask_guide = s.gpu.sss.guide;
        } else if (sss_mask_guide && sss_mask_guide == s.gpu.sss.guide) {
            sss_mask = {};
            sss_mask_guide.Reset();
        }
        SealSssCoverage(s, accepted);
    }
    // The coverage record follows the guide: replaced by an admitted guide whose frame
    // recorded both its surface key and its coverage, dropped with a rejected one. An
    // admitted frame without coverage keeps the older record: Check still limits its age,
    // generation and extent, and with MaxAge < Ring its entry is not rewritten while usable.
    void SealSssCoverage(const Slot &s, bool accepted) {
        if (!accepted) {
            if (coverage_record.valid && coverage_record.frame == s.frame)
                coverage_record = {};
            return;
        }
        const auto &e = coverage_history.entries[rrssscov::Entry(s.frame)];
        if (!coverage_history.Matches(s.gpu.w, s.gpu.h) || !s.sss.marked ||
            e.key_frame != s.frame || e.coverage_frame != s.frame || e.generation != s.generation) {
            // A record from before a camera cut never outlives the cut.
            if (s.camera.reset != 0)
                coverage_record = {};
            return;
        }
        coverage_record = {true, s.frame, s.generation, s.gpu.w, s.gpu.h, CoverageCamera(s)};
    }
    static rrssscov::Camera CoverageCamera(const Slot &s) {
        rrssscov::Camera c;
        c.view = s.view;
        c.projection = s.projection;
        c.frame = s.frame;
        c.viewport = s.viewport;
        c.valid = s.camera.readable && s.camera.basis_valid && s.camera.projection_valid;
        c.reset = s.camera.reset != 0;
        return c;
    }
    // Allocates the coverage history once it is wanted and a mandatory RR slot exists; a
    // budget or memory shortage retries a second later, any other failure turns the
    // experiment off for the session. Never fails RR.
    bool PrepareSssCoverage(UINT w, UINT h) {
        if (coverage_history.ready)
            return coverage_history.Matches(w, h);
        const UINT64 now = GetTickCount64();
        if (coverage_failed || now < coverage_retry_after || !pipelines.device)
            return false;
        const UINT64 n = CoverageHistory::Bytes(pipelines.device.Get(), w, h);
        const UINT64 old = budget.limit;
        const UINT64 before = persistent.bytes;
        try {
            if (!n || old > UINT64_MAX - n || !budget.Reserve(pipelines.device.Get(), old + n))
                throw rrprivate::AllocationDeferred();
            test_reserve += budget.limit - old;
            coverage_history.Prepare(pipelines, budget, persistent, w, h);
            coverage_retry_after = 0;
            Log("Subsurface Scattering coverage history allocated (experiment)");
            return true;
        } catch (const rrprivate::AllocationDeferred &) {
            CoverageUnwind(before, old);
            if (!coverage_retry_after)
                Log("Subsurface Scattering coverage history waiting for GPU memory; retried "
                    "every second");
            coverage_retry_after = now + 1000;
        } catch (const std::exception &e) {
            CoverageUnwind(before, old);
            coverage_failed = true;
            const std::string line =
                std::string("Subsurface Scattering coverage experiment unavailable: ") + e.what();
            Log(line.c_str());
        }
        return false;
    }
    // A failed preparation: nothing references its resources yet, so they and the budget
    // growth are given back.
    void CoverageUnwind(UINT64 before, UINT64 old_limit) {
        coverage_history.Release();
        persistent.Unwind(budget, before);
        if (budget.limit > old_limit) {
            test_reserve -= (std::min)(test_reserve, budget.limit - old_limit);
            budget.limit = (std::max)(old_limit, budget.bytes);
        }
    }
    // Right after a prepass: this frame's surface key. Skipped (the frame then has no
    // record) while an earlier frame using the same descriptor set still has GPU work,
    // since those descriptors would be rewritten under it. With more sets than slots this
    // only happens if slots hold frames Sets or more apart.
    void CoverageKey(Slot &s, ID3D12GraphicsCommandList *cl) {
        if (!rrssscov::Wanted() || !sss_enabled || sss_failed || !PrepareSssCoverage(s.gpu.w, s.gpu.h))
            return;
        for (const auto &o : slots)
            if (&o != &s && o.busy && o.frame != UINT_MAX &&
                CoverageHistory::Set(o.frame) == CoverageHistory::Set(s.frame)) {
                // Shown in the panel; logged at 1, 2, 4, 8, ...
                if (!(++coverage_key_skips & (coverage_key_skips - 1)))
                    Log("Subsurface Scattering coverage: surface key skipped, its descriptors "
                        "are still in flight (raw lighting this frame)");
                return;
            }
        coverage_history.Key(cl, pipelines, s.gpu.sources[3].Get(), s.frame, s.generation);
    }
    // The Subsurface Scattering pass of slot s records coverage (marker initializer).
    bool CoverageMarking(const Slot &s, const rrsss::StencilSetup &stencil) const {
        return rrssscov::Wanted() && coverage_history.Matches(s.gpu.w, s.gpu.h) &&
               stencil.required && stencil.marker;
    }
    // The coverage texture this slot's resolve writes, or null for the baseline resolve.
    ID3D12Resource *CoverageTarget(const Slot &s) {
        auto &e = coverage_history.entries[rrssscov::Entry(s.frame)];
        if (!s.sss.marked)
            return nullptr;
        // A marked producer gives 65504 where the baseline gives 0, so its resolve takes the
        // coverage variant even when no record can be kept (the stamps below). Marking needs
        // a matching ring and Reconfigure retires every slot before releasing it, so this
        // test does not fail for a marked recording.
        if (!coverage_history.Matches(s.gpu.w, s.gpu.h))
            return nullptr;
        if (e.key_frame == s.frame && e.generation == s.generation)
            e.coverage_frame = s.frame;
        return e.coverage.Get();
    }
    void CoverageCounters(const Slot &s) {
        auto &e = coverage_history.entries[rrssscov::Entry(s.frame)];
        if (!e.counted || e.key_frame != s.frame || !e.map)
            return;
        e.counted = false;
        uint32_t v[rrssscov::Counters];
        memcpy(v, e.map, sizeof(v));
        const UINT64 now = GetTickCount64();
        if (!coverage_window_start)
            coverage_window_start = now;
        coverage_window.Add(v, e.age);
        if (now - coverage_window_start >= 1000) {
            coverage_last = coverage_window;
            coverage_window = {};
            coverage_window_start = now;
        }
    }

    // Weapon depth for Frame Generation (rr_fg_depth.h). Gives back a slot's texture and the
    // reserve it added, never below what is still allocated. Only for a slot whose GPU work
    // completed, or in AfterSR before the frame records it.
    void ReleaseFgTexture(GpuFrame &g) {
        if (!g.fg_depth)
            return;
        const UINT64 growth = g.fg_growth;
        g.ReleaseFg(budget);
        test_reserve -= (std::min)(test_reserve, growth);
        budget.limit = (std::max)(
            {Budget::Minimum, budget.bytes, budget.limit - (std::min)(growth, budget.limit)});
    }
    // The slot's texture at w x h, created on first use. Like UseTest, the reserve grows by
    // its size first, so it never takes room RR's pool counted on. False: no GPU memory (a
    // second's wait; RR keeps running).
    bool FgTexture(Slot &s, UINT w, UINT h, UINT64 now) {
        auto &g = s.gpu;
        if (g.fg_depth) {
            const auto d = g.fg_depth->GetDesc();
            if (d.Width == w && d.Height == h)
                return true;
            ReleaseFgTexture(g); // this slot's last use completed before it was reused
        }
        auto d = TextureDesc(w, h, DXGI_FORMAT_R32_FLOAT);
        d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        const UINT64 n = pipelines.device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
        const UINT64 old = budget.limit;
        bool ok = n && n != UINT64_MAX && old <= UINT64_MAX - n &&
                  budget.Reserve(pipelines.device.Get(), old + n);
        if (ok) {
            const UINT64 before = g.bytes;
            try {
                // Announced like RR's depth copy: DLSS5 NR registers resources it may see.
                g.fg_depth =
                    g.Allocate(pipelines, budget, d, D3D12_HEAP_TYPE_DEFAULT, nullptr, true);
                g.fg_bytes = g.bytes - before;
                g.fg_growth = budget.limit - old;
                test_reserve += g.fg_growth;
            } catch (const std::exception &) {
                g.fg_depth.Reset();
                g.Unwind(budget, before);
                budget.limit = old;
                ok = false;
            }
        }
        if (ok) {
            fg_memory_logged = false;
            return true;
        }
        fg_memory_retry_at = now + 1000;
        if (!fg_memory_logged) {
            fg_memory_logged = true;
            Log("Weapon depth for Frame Generation: not enough GPU memory; the game's depth is "
                "used, retried every second");
        }
        return false;
    }
    // Once per change of the setting, under the lock: a pause, the refusal count and the
    // reason counts start over, so the first summary after a change is about the new setting.
    void FgSettingChanged() {
        const unsigned changes = rrfgdepth::changes.load(std::memory_order_relaxed);
        if (changes == fg_changes)
            return;
        fg_changes = changes;
        fg_paused = false;
        fg_refusals = 0;
        fg_supplied_logged = false;
        rrfgdepth::status.Reset();
    }
    // In AfterSR, on the frame's SR list after RR's depth source is a shader resource and
    // before RR's own passes: writes this frame's replacement (GpuFrame::FgWeaponDepth). The
    // setting is read once here, so the frame's tag follows this decision. A failure here
    // only leaves the frame's Frame Generation depth the game's own.
    void PrepareFgDepth(Slot &s, ID3D12GraphicsCommandList *cl) {
        s.fg_prepared = false;
        FgSettingChanged();
        const bool on = rrfgdepth::enabled.load(std::memory_order_relaxed);
        const UINT64 now = GetTickCount64();
        try {
            const auto native = s.gpu.sr[1]->GetDesc();
            const bool format_ok = native.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT
                                       ? s.gpu.use_depth && s.gpu.depth
                                       : native.Format == DXGI_FORMAT_R32_FLOAT && !s.gpu.use_depth;
            rrweapondepth::Mapping map;
            const bool mapped = rrweapondepth::Map(s.projection, map);
            auto why = rrfgdepth::Prepare(on, fg_paused, rrfgdepth::status.FgSeen(now), format_ok,
                                          mapped, now < fg_memory_retry_at);
            if (why == rrfgdepth::Reason::Supplied &&
                (native.Width > UINT_MAX || !FgTexture(s, UINT(native.Width), native.Height, now)))
                why = rrfgdepth::Reason::Memory;
            s.fg_prepare = why;
            if (why != rrfgdepth::Reason::Supplied)
                return;
            s.gpu.FgWeaponDepth(cl, pipelines, map);
            s.fg_prepared = true;
        } catch (const std::exception &e) {
            s.fg_prepare = rrfgdepth::Reason::NotRecorded;
            std::string line = std::string("Weapon depth for Frame Generation: ") + e.what();
            Log(line.c_str());
        }
    }
    // One diagnostic line when a Frame Generation depth tag's metadata changes, at most 24 a
    // session, and a 10 s summary of the reasons. Metadata only: no resource is read.
    void FgDiagnose(const rrtrace::FgTag &t, const Slot *s, bool same_list, bool same_depth,
                    ID3D12Resource *depth, const D3D12_RESOURCE_DESC &tagged, rrfgdepth::Reason why,
                    UINT64 now) {
        FgSignature v{t.lifecycle, t.state, t.extent_width, t.extent_height};
        v.fg_width = UINT(tagged.Width);
        v.fg_height = tagged.Height;
        v.fg_format = UINT(tagged.Format);
        v.fg_flags = UINT(tagged.Flags);
        for (auto &o : slots)
            if (depth && o.recorded && o.gpu.sr[1].Get() == depth) {
                v.match = int(INT64(o.frame) - INT64(t.frame));
                break;
            }
        v.same_list = same_list;
        v.same_depth = same_depth;
        v.slot = s != nullptr;
        v.submitted = s && s->sealed;
        if (s && same_depth) {
            const auto d = s->gpu.sr[1]->GetDesc();
            v.width = UINT(d.Width);
            v.height = d.Height;
            v.format = UINT(d.Format);
            v.offset_x = s->options.depth_x;
            v.offset_y = s->options.depth_y;
            v.render_w = s->options.width;
            v.render_h = s->options.height;
        }
        if (s) {
            v.inverted = s->camera.depth_inverted;
            v.motion = s->camera.camera_motion;
            v.jittered = s->camera.motion_jittered;
        }
        char delta[16] = "none";
        if (v.match != FgSignature::NoMatch)
            snprintf(delta, sizeof(delta), "%+d", v.match);
        if (!(v == fg_logged) && fg_diagnostics < 24) {
            fg_logged = v;
            ++fg_diagnostics;
            char line[640];
            snprintf(line, sizeof(line),
                     "FG depth tag: frame=%u view=%u lifecycle=%u extent=%ux%u state=0x%x "
                     "rrFrame=%u sameList=%u srSubmitted=%u sameDepth=%u depth=%ux%u format=%u "
                     "srSubrect=%u,%u+%ux%u camera{depthInverted=%u cameraMotionIncluded=%u "
                     "mvJittered=%u near=%g far=%g} tagged=%ux%u format=%u flags=0x%x "
                     "srDepthOfFrame=%s -> %s",
                     t.frame, t.viewport, t.lifecycle, t.extent_width, t.extent_height, t.state,
                     unsigned(v.slot), unsigned(same_list), unsigned(v.submitted),
                     unsigned(same_depth), v.width, v.height, v.format, v.offset_x, v.offset_y,
                     v.render_w, v.render_h, v.inverted, v.motion, v.jittered,
                     s ? s->camera.near_plane : 0.f, s ? s->camera.far_plane : 0.f, v.fg_width,
                     v.fg_height, v.fg_format, v.fg_flags, delta, rrfgdepth::Key(why));
            Log(line);
        }
        // The identities themselves for the first frames whose depth is not SR's: whether the
        // game alternates depth textures, or tags a texture RR never saw.
        if (s && !same_depth && fg_pairs < 16) {
            ++fg_pairs;
            char line[384];
            int n = snprintf(line, sizeof(line),
                             "FG depth pair: frame=%u tagged=%p srDepth=%p slots{", t.frame,
                             static_cast<void *>(depth), static_cast<void *>(s->gpu.sr[1].Get()));
            for (auto &o : slots)
                if (o.recorded && n > 0 && size_t(n) < sizeof(line))
                    n += snprintf(line + n, sizeof(line) - n, " %u:%p", o.frame,
                                  static_cast<void *>(o.gpu.sr[1].Get()));
            if (n > 0 && size_t(n) < sizeof(line))
                snprintf(line + n, sizeof(line) - n, " }");
            Log(line);
        }
        if (!fg_summary_at)
            fg_summary_at = now;
        if (now - fg_summary_at >= 10000) {
            fg_summary_at = now;
            char counts[384];
            rrfgdepth::status.Drain(counts, sizeof(counts));
            if (counts[0]) {
                char line[448];
                snprintf(line, sizeof(line), "Weapon depth for Frame Generation (10s): %s", counts);
                Log(line);
            }
        }
    }
    // The content check's commands, prepared under the lock by FgProbeArm and recorded by
    // FgProbeRecord outside it.
    struct FgProbeRecording {
        ComPtr<ID3D12Resource> readback, source;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 half{}, bytes{};
    };
    // Under the lock: whether this replacement frame also gets the content check (at most
    // ProbeLimit a session, ProbeInterval apart, one in flight). The caller marks the slot.
    bool FgProbeArm(const rrtrace::FgTag &t, const Slot &s, const D3D12_RESOURCE_DESC &tagged,
                    UINT64 now, FgProbeRecording &r) {
        if (fg_probe.armed || fg_probe.ready || fg_probe_stuck ||
            fg_probes >= rrfgdepth::ProbeLimit ||
            (fg_probe_at && now - fg_probe_at < rrfgdepth::ProbeInterval) || !s.gpu.use_depth ||
            !s.gpu.depth_buffer || !pipelines.device || tagged.MipLevels != 1 ||
            tagged.DepthOrArraySize != 1)
            return false;
        fg_probe_at = now;
        ++fg_probes;
        FgProbe p;
        p.fp = s.gpu.depth_fp;
        r.bytes = s.gpu.depth_buffer->GetDesc().Width;
        p.half = (r.bytes + 511) & ~UINT64(511);
        p.frame = t.frame;
        p.armed_at = now;
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
        const auto desc = rrprivate::Buffer(2 * p.half);
        if (FAILED(pipelines.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                             D3D12_RESOURCE_STATE_COPY_DEST,
                                                             nullptr, IID_PPV_ARGS(&p.readback)))) {
            Log("Weapon depth for Frame Generation: no memory for the depth content check");
            return false;
        }
        p.armed = true;
        fg_probe = p;
        r.readback = p.readback;
        r.source = s.gpu.depth_buffer;
        r.fp = p.fp;
        r.half = p.half;
        return true;
    }
    // Outside the lock, with RR's hooks quiet: copies only, so no binding the game set is
    // changed, and the game's depth returns to the state its tag declares.
    static void FgProbeRecord(ID3D12GraphicsCommandList *list, ID3D12Resource *depth,
                              D3D12_RESOURCE_STATES declared, const FgProbeRecording &r) {
        Change(list, depth, declared, D3D12_RESOURCE_STATE_COPY_SOURCE, 0);
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = r.readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = r.fp;
        to.PlacedFootprint.Offset = 0;
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = depth;
        from.SubresourceIndex = 0; // the depth plane
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        Change(list, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, declared, 0);
        // RR's copy of SR's depth plane is a buffer in COMMON: promoted to a copy source.
        list->CopyBufferRegion(r.readback.Get(), r.half, r.source.Get(), 0, r.bytes);
    }
    // A completed content check, read and logged outside the lock.
    void FgProbeCollect(UINT64 now) {
        FgProbe p;
        {
            std::lock_guard lock(mutex);
            if (fg_probe.ready) {
                p = std::move(fg_probe);
                fg_probe = {};
            } else if (fg_probe.armed && !fg_probe_stuck && now - fg_probe.armed_at > 10000) {
                fg_probe_stuck = true;
                Log("Weapon depth for Frame Generation: the depth content check did not complete; "
                    "no further checks this session");
            }
        }
        if (!p.readback)
            return;
        const auto &f = p.fp.Footprint;
        void *data{};
        const D3D12_RANGE range{0, SIZE_T(2 * p.half)};
        if (FAILED(p.readback->Map(0, &range, &data)) || !data) {
            Log("Weapon depth for Frame Generation: the depth content check could not be read");
            return;
        }
        const auto *base = static_cast<const unsigned char *>(data);
        const auto c = rrfgdepth::Compare(reinterpret_cast<const float *>(base),
                                          reinterpret_cast<const float *>(base + p.half), f.Width,
                                          f.Height, f.RowPitch / 4, rrweapondepth::Slice);
        const D3D12_RANGE none{};
        p.readback->Unmap(0, &none);
        const char *verdict = c.world_equal != c.world
                                  ? "the world differs: not this frame's SR depth"
                              : !c.weapon ? "a copy of SR's depth (no weapon in this frame)"
                              : c.weapon_equal == c.weapon
                                  ? "a plain copy of SR's depth, weapon included"
                                  : "the world matches and the weapon differs: the game changes "
                                    "the weapon in Frame Generation's depth";
        char line[512];
        snprintf(line, sizeof(line),
                 "FG depth content (frame %u): world %llu px, %llu equal, max diff %g; weapon %llu "
                 "px, %llu equal, SR [%g, %g] FG [%g, %g]; FG above 1: %llu px -> %s",
                 p.frame, (unsigned long long)c.world, (unsigned long long)c.world_equal,
                 c.world_max_diff, (unsigned long long)c.weapon, (unsigned long long)c.weapon_equal,
                 c.weapon_sr_min, c.weapon_sr_max, c.weapon_fg_min, c.weapon_fg_max,
                 (unsigned long long)c.fg_above_one, verdict);
        Log(line);
    }
#include "rr_live_pool.inl"
  public:
    explicit Controller(rreval::Api (*loader)(HMODULE) = &rreval::Api::Load) : resolve(loader) {}
#include "rr_live_transparency.inl"
#include "rr_live_coverage.inl"

    bool SssEnabled() {
        std::lock_guard lock(mutex);
        return sss_enabled;
    }
    // Isolated unusable guide pixels are counted instead of rejecting the frame.
    void SetTolerateIsolated(bool value) {
        std::lock_guard lock(mutex);
        tolerate_isolated = value;
    }
    // Weapon depth for Frame Generation (rr_fg_depth.h), from the Streamline tag hook with no
    // other lock held: true hands Frame Generation this frame's replacement in `out` (the
    // game's depth tag description, copied). Native identities are read before the lock.
    bool FgTag(const rrtrace::FgTag &t, sl::Resource &out) {
        auto &status = rrfgdepth::status;
        const UINT64 now = GetTickCount64();
        status.fg_seen_at.store(now, std::memory_order_relaxed);
        rrfgdepth::Match m;
        m.enabled = rrfgdepth::enabled.load(std::memory_order_relaxed);
        // Off and not observing: nothing is counted (no summary would report it).
        if (!m.enabled && !rrfgdepth::observe.load(std::memory_order_relaxed))
            return false;
        auto list =
            fg::NativeObject<ID3D12GraphicsCommandList>(reinterpret_cast<IUnknown *>(t.command));
        auto depth = fg::NativeObject<ID3D12Resource>(reinterpret_cast<IUnknown *>(t.native));
        D3D12_RESOURCE_DESC tagged{};
        if (depth)
            tagged = depth->GetDesc();
        const auto type = list ? list->GetType() : D3D12_COMMAND_LIST_TYPE_COPY;
        FgProbeCollect(now);
        std::unique_lock lock(mutex);
        FgSettingChanged();
        m.paused = fg_paused;
        m.lifecycle_now = t.lifecycle == rrfgdepth::OnlyValidNow;
        m.rr_running = enabled && !failed && !retired;
        Slot *s{};
        for (auto &v : slots)
            if (v.recorded && v.frame == t.frame && v.viewport == t.viewport &&
                t.frame != UINT_MAX) {
                if (s) {
                    s = nullptr; // two slots for one frame: no identity
                    break;
                }
                s = &v;
            }
        m.slot = s != nullptr;
        bool same_list = false, same_depth = false;
        if (s) {
            m.prepared = s->fg_prepared;
            m.prepare = s->fg_prepare;
            same_list = list && s->sr_live && list.Get() == s->sr.Get();
            same_depth = depth && s->gpu.sr[1] && depth.Get() == s->gpu.sr[1].Get();
            m.same_list = same_list;
            // A second tag for this frame on yet another list while the first is open would
            // stop the slot tracking the first: the game's depth for that one.
            m.other_list =
                list && !same_list && !(s->fg_list_live && s->fg_list.Get() != list.Get());
            m.list_ok =
                type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE;
            m.unsubmitted = !s->sealed;
            m.same_depth = same_depth;
            if (depth && s->gpu.sr[1] && !same_depth) {
                const auto sr = s->gpu.sr[1]->GetDesc();
                m.game_copy = tagged.Dimension == sr.Dimension && tagged.Width == sr.Width &&
                              tagged.Height == sr.Height && tagged.Format == sr.Format &&
                              tagged.MipLevels == sr.MipLevels &&
                              tagged.DepthOrArraySize == sr.DepthOrArraySize;
            }
        }
        const auto why = rrfgdepth::Decide(m);
        FgDiagnose(t, s, same_list, same_depth || m.game_copy, depth.Get(), tagged, why, now);
        if (why != rrfgdepth::Reason::Supplied) {
            status.Note(why);
            return false;
        }
        const auto d = s->gpu.fg_depth->GetDesc();
        out.native = s->gpu.fg_depth.Get();
        out.memory = nullptr;
        out.view = nullptr;
        out.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        out.width = UINT(d.Width);
        out.height = d.Height;
        out.nativeFormat = DXGI_FORMAT_R32_FLOAT;
        out.mipLevels = 1;
        out.arrayLayers = 1;
        out.gpuVirtualAddress = 0;
        if (same_list)
            return true;
        // Another list: the SR list ended with the texture a shader resource and has been
        // submitted, so it is back in COMMON (simultaneous-access); make it the state the tag
        // declares before Streamline's copy. The slot now waits for this list. With the game's
        // own copy, a few frames also get the content check (FgProbe): what that copy holds.
        FgProbeRecording probe;
        const bool probing = m.game_copy && rrfgdepth::observe.load(std::memory_order_relaxed) &&
                             FgProbeArm(t, *s, tagged, now, probe);
        s->fg_list = list;
        s->fg_list_live = true;
        if (probing) { // a second tag on the same list keeps the check it armed
            s->fg_probe = true;
            s->fg_probe_submitted = false;
        }
        ComPtr<ID3D12Resource> texture = s->gpu.fg_depth;
        lock.unlock();
        // Recorded on the native list with RR's own hooks quiet, outside the lock: a barrier
        // and copies, so no binding the game set is changed.
        rrinternal::Scope copy_guard;
        rrcompute::Tracker::Internal state_guard;
        Change(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (probing)
            FgProbeRecord(list.Get(), depth.Get(), D3D12_RESOURCE_STATES(t.state), probe);
        return true;
    }
    // Streamline's result for a replacement FgTag supplied. A refusal already re-sent the
    // game's tags; RefusalLimit in a row pause the replacement until the setting is off.
    void FgResult(const rrtrace::FgTag &t, sl::Result r) {
        auto &status = rrfgdepth::status;
        std::lock_guard lock(mutex);
        if (r == sl::Result::eOk) {
            status.Note(rrfgdepth::Reason::Supplied);
            status.supplied_at.store(GetTickCount64(), std::memory_order_relaxed);
            fg_refusals = 0;
            if (!fg_supplied_logged) {
                fg_supplied_logged = true;
                char line[256];
                snprintf(line, sizeof(line),
                         "Weapon depth for Frame Generation: replacement supplied from frame %u "
                         "(Streamline accepted the tag; whether Frame Generation uses it is not "
                         "observed)",
                         t.frame);
                Log(line);
            }
            return;
        }
        status.Note(rrfgdepth::Reason::Rejected);
        char line[160];
        snprintf(line, sizeof(line),
                 "Weapon depth for Frame Generation: Streamline refused the replacement (%d) on "
                 "frame %u; the game's depth was tagged instead",
                 int(r), t.frame);
        Log(line);
        if (++fg_refusals >= rrfgdepth::RefusalLimit && !fg_paused) {
            fg_paused = true;
            Log("Weapon depth for Frame Generation: off after 3 refusals in a row, until the "
                "setting is turned off and on");
        }
    }
    // The panel's line: the last reason, Supplied only while replacements were supplied within
    // the last 2 s.
    rrfgdepth::Reason FgState() const {
        auto &status = rrfgdepth::status;
        if (!rrfgdepth::enabled.load(std::memory_order_relaxed))
            return rrfgdepth::Reason::Disabled;
        const UINT64 now = GetTickCount64();
        if (!status.FgSeen(now))
            return rrfgdepth::Reason::NoFgTags;
        const auto last = rrfgdepth::Reason(status.last.load(std::memory_order_relaxed));
        if (last == rrfgdepth::Reason::Supplied &&
            now - status.supplied_at.load(std::memory_order_relaxed) > 2000)
            return rrfgdepth::Reason::Stale;
        return last;
    }
    // Weapon depth state for the panel. Restarting: RR is being recreated with the
    // depth type the setting asks for (the change takes effect once in-flight frames retire).
    enum class WeaponState { Off, Active, Waiting, Restarting, NoMemory, Unsupported, Refused };
    WeaponState WeaponDepthState() {
        std::lock_guard lock(mutex);
        const bool running = enabled && !failed && !retired && feature;
        if (!rrweapondepth::enabled.load(std::memory_order_relaxed))
            return running && weapon_linear ? WeaponState::Restarting : WeaponState::Off;
        if (!WeaponDepthWanted())
            return weapon_hold == WeaponHold::Memory       ? WeaponState::NoMemory
                   : weapon_hold == WeaponHold::Projection ? WeaponState::Unsupported
                                                           : WeaponState::Refused;
        if (!running)
            return WeaponState::Waiting;
        if (!weapon_linear)
            return WeaponState::Restarting;
        return weapon_test.active ? WeaponState::Active : WeaponState::Waiting;
    }
    bool Running() {
        std::lock_guard lock(mutex);
        return enabled && !failed && !retired && feature;
    }
    // Water responsivity mask (rr_water.h). Nothing is allocated or sent to RR until it
    // is wanted; then each slot keeps its water storage until the next rebuild
    // (Reconfigure), like the SSS storage. It applies from the first prepass that has
    // that storage. A nonzero colour subrect origin drops it, as it drops the SSS and
    // glass guides.
    void SetWater(const rrwater::Settings &value) {
        std::lock_guard lock(mutex);
        water = value;
        water.strength = rrwater::Clamp(value.strength);
        if (!WaterWanted())
            water_retry_after = 0;
    }
    // The water classification is wanted this frame: the preset RR runs, or the one it
    // is being recreated with.
    bool WaterWanted() const {
        return rrwater::Wanted(water,
                               (preset.has_effective ? preset.effective : preset.desired) == 6);
    }
    // What the latest evaluation sent, limited to what would still be sent now.
    rrwater::Status WaterStatus() {
        std::lock_guard lock(mutex);
        auto v = water_status;
        v.wanted = WaterWanted();
        v.unavailable = water_failed;
        const bool live = v.wanted && enabled && !failed && !retired && !water_failed;
        v.waiting_memory = live && water_retry_after != 0;
        v.ready = v.ready && live;
        v.responsivity = v.responsivity && v.ready &&
                         rrwater::UseResponsivity(water, true, preset.has_effective,
                                                  preset.effective);
        return v;
    }
    void SetSssEnabled(bool value) {
        std::lock_guard lock(mutex);
        if (sss_enabled != value) {
            sss_enabled = value;
            sss_mask = {};
            sss_mask_guide.Reset();
            coverage_record = {};
            policy.Reject();
        }
    }
    // The SSS guide an SSS-masked first spatial pass on cl's frame reads, or null
    // for raw lighting; age is how many frames older it is. A guide is resolved
    // after its own frame's first spatial pass, so this is an earlier frame's,
    // submitted before cl; that recording leaves it COMMON.
    ComPtr<ID3D12Resource> SssMask(ID3D12GraphicsCommandList *cl, unsigned *age = nullptr) {
        std::lock_guard lock(mutex);
        if (!sss_enabled || sss_failed || !sss_mask_guide)
            return {};
        const Slot *match{};
        for (const auto &s : slots)
            if (RawSlot(s, cl) && s.native_sampling) {
                if (match)
                    return {};
                match = &s;
            }
        if (!match ||
            !rrsssmask::Usable(sss_mask, match->frame, generation, match->gpu.w, match->gpu.h))
            return {};
        if (age)
            *age = match->frame - sss_mask.frame;
        return sss_mask_guide;
    }
    // This frame's surface for the edge-aware no-history fill (rr_edge_fill.h), from the
    // slot SssMask() matches on cl: its prepass PackedSurface and the guide conversion's
    // normals and Status, recorded earlier on cl and left COMMON. False without exactly
    // one such slot with converted guides.
    bool EdgeSurface(ID3D12GraphicsCommandList *cl, ComPtr<ID3D12Resource> &surface,
                     ComPtr<ID3D12Resource> &normal, ComPtr<ID3D12Resource> &status) {
        std::lock_guard lock(mutex);
        const Slot *match{};
        for (const auto &s : slots)
            if (RawSlot(s, cl) && s.native_sampling) {
                if (match)
                    return false;
                match = &s;
            }
        if (!match || !match->gpu.sources[3] || !match->gpu.guides[0] || !match->gpu.guides[3])
            return false;
        surface = match->gpu.sources[3];
        normal = match->gpu.guides[0];
        status = match->gpu.guides[3];
        return true;
    }
    void ToggleSss() {
        std::lock_guard lock(mutex);
        SetSssEnabled(!sss_enabled);
    }
    // The coverage selection's inputs for the SSS-masked first spatial pass on cl's frame
    // (rr_sss_coverage.h). reason says why there are none; apply: the selection is used
    // (else only counted beside the baseline).
    bool SssCoverage(ID3D12GraphicsCommandList *cl, bool apply, rrsparsecopy::CoverageInputs &out,
                     rrssscov::FrameReason &reason) {
        std::lock_guard lock(mutex);
        out = {};
        // Every return leaves its reason for the panel.
        reason = coverage_reason = rrssscov::FrameReason::NoStorage;
        if (!sss_enabled || sss_failed || !coverage_history.ready)
            return false;
        const Slot *match{};
        for (const auto &s : slots)
            if (RawSlot(s, cl) && s.native_sampling) {
                if (match)
                    return false;
                match = &s;
            }
        if (!match)
            return false;
        auto &current = coverage_history.entries[rrssscov::Entry(match->frame)];
        if (current.key_frame != match->frame || current.generation != generation)
            return false;
        const auto camera = CoverageCamera(*match);
        reason = rrssscov::Check(coverage_record, match->frame, generation, match->gpu.w,
                                 match->gpu.h, match->viewport, camera.reset);
        coverage_reason = reason;
        if (reason == rrssscov::FrameReason::Reset)
            coverage_record = {}; // a cut: the record is from before it
        if (reason != rrssscov::FrameReason::Ready)
            return false;
        const auto &previous = coverage_history.entries[rrssscov::Entry(coverage_record.frame)];
        if (previous.key_frame != coverage_record.frame ||
            previous.coverage_frame != coverage_record.frame ||
            previous.generation != coverage_record.generation) {
            reason = coverage_reason = rrssscov::FrameReason::NoStorage;
            return false;
        }
        const unsigned age = match->frame - coverage_record.frame;
        if (!rrssscov::Make(camera, coverage_record.camera, match->gpu.w, match->gpu.h, age, apply,
                            out.constants)) {
            reason = coverage_reason = rrssscov::FrameReason::Camera;
            return false;
        }
        out.current = current.key;
        out.previous = previous.key;
        out.coverage = previous.coverage;
        out.counters = current.counters;
        out.readback = current.readback;
        current.age = age;
        return true;
    }
    // The pass recorded the selection: its counters are read once the frame completes.
    void SssCoverageRecorded(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(mutex);
        for (const auto &s : slots)
            if (RawSlot(s, cl) && s.native_sampling) {
                auto &e = coverage_history.entries[rrssscov::Entry(s.frame)];
                if (e.key_frame == s.frame)
                    e.counted = true;
            }
    }
    struct SssCoverageState {
        bool allocated{}, failed{}, waiting{};
        rrssscov::FrameReason reason{};
        rrssscov::Totals totals;
        UINT64 key_skips{};
    };
    SssCoverageState SssCoverageReport() {
        std::lock_guard lock(mutex);
        return {coverage_history.ready, coverage_failed, coverage_retry_after != 0,
                coverage_reason, coverage_last, coverage_key_skips};
    }
    bool SpecularHitEnabled() {
        std::lock_guard lock(mutex);
        return hit_enabled;
    }
    void SetSpecularHitEnabled(bool value) {
        std::lock_guard lock(mutex);
        if (hit_enabled != value) {
            hit_enabled = value;
            policy.Reject();
        }
    }
    bool PrepassSurface(ID3D12GraphicsCommandList *cl, ID3D12Resource *surface) {
        std::lock_guard lock(mutex);
        for (auto &s : slots)
            if (RawSlot(s, cl) && s.gpu.sources[3].Get() == surface)
                return true;
        return false;
    }
    // The native distance blur runs after the prepass on the same recording and
    // reads that prepass's PackedSurface. Only a copy is recorded; the caller has
    // proven the dispatch, tables, views and the source's tracked state.
    bool SpecularDistance(ID3D12GraphicsCommandList *cl, ID3D12Resource *surface,
                          ID3D12Resource *source, D3D12_RESOURCE_STATES source_state) {
        std::lock_guard lock(mutex);
        if (!enabled || !hit_enabled || failed || retired || !cl || !surface || !source)
            return false;
        Slot *match{};
        for (auto &s : slots)
            if (RawSlot(s, cl) && s.gpu.sources[3].Get() == surface) {
                if (match)
                    return false;
                match = &s;
            }
        if (!match)
            return false;
        try {
            rrinternal::Scope copy_guard;
            rrcompute::Tracker::Internal state_guard;
            const bool copied =
                match->gpu.CopySpecularDistance(cl, pipelines, source, source_state);
            if (hit_reports < 2) {
                ++hit_reports;
                const auto d = source->GetDesc();
                char line[192];
                snprintf(line, sizeof(line),
                         "Specular hit distance %s: source=%llux%u format=%u state=0x%x "
                         "guides=%ux%u",
                         copied ? "copied" : "rejected", d.Width, d.Height, unsigned(d.Format),
                         unsigned(source_state), match->gpu.w, match->gpu.h);
                Log(line);
            }
            return copied;
        } catch (const std::exception &e) {
            Failure(e.what());
            return false;
        }
    }

    bool OwnedSssDraw(ID3D12GraphicsCommandList *cl, ID3D12PipelineState *variant,
                      const rrsss::GraphicsOutputSnapshot &original,
                      const rrsss::ProducerDraw &draw, const rrsss::StencilSetup &stencil = {}) {
        std::lock_guard lock(mutex);
        if (!enabled || !sss_enabled || sss_failed || failed || retired || NeedsReconfigure())
            return false;
        if (!cl || !variant || original.command != cl || !rrsss::GeometryState(original, stencil) ||
            draw.count != 3 || draw.instances != 1) {
            return false;
        }
        // Native SR and the SSS producer can be recorded on different CPU threads.
        // AfterSR takes this same mutex and freezes guide selection at recorded=true.
        // pending_sr alone is not the cutoff; GPU submission order is checked separately.
        Slot *match{};
        for (auto &s : slots)
            if ((s.pre_live || s.awaiting_prepass) && !s.recorded && s.frame == camera_frame &&
                s.viewport == camera_view && s.generation == generation && !s.poisoned) {
                if (match) {
                    return false;
                }
                match = &s;
            }
        bool early = false;
        if (!match) {
            // CPU recording order need not match GPU execution order. Reserve only a
            // free warmed slot; the later prepass adopts this exact frame/view ticket.
            bool conflict = false;
            for (const auto &slot : slots)
                if (slot.busy && slot.frame == camera_frame && slot.viewport == camera_view)
                    conflict = true;
            if (!conflict && initialized && camera_frame != UINT_MAX && camera_view != UINT_MAX &&
                latest.readable && latest.basis_valid && latest.projection_valid &&
                GetTickCount64() - camera_time <= 1000 &&
                (selected_view == UINT_MAX || selected_view == camera_view)) {
                for (auto &slot : slots)
                    if (!slot.busy && slot.gpu.rr_output &&
                        rrsss::Geometry(original, slot.gpu.w, slot.gpu.h, stencil) &&
                        slot.gpu.sss.MatchesLayout(slot.gpu.w, slot.gpu.h, stencil.samples)) {
                        match = &slot;
                        early = true;
                        break;
                    }
            }
        }
        if (!match) {
            return false;
        }
        auto &s = *match;
        if (s.sss.command) {
            s.sss.written = false;
            return false;
        }
        if (!rrsss::Geometry(original, s.gpu.w, s.gpu.h, stencil) ||
            !s.gpu.sss.MatchesLayout(s.gpu.w, s.gpu.h, stencil.samples)) {
            return false;
        }
        if (!s.gpu.sss.ready) {
            // Observe a complete mandatory SR allocation before reserving optional MRTs.
            if (!s.gpu.rr_output) {
                return false;
            }
            UINT64 base = 0, depth = 0, water_bytes = 0;
            for (const auto &slot : slots) {
                UINT64 b{}, d{};
                if (!SlotReservationParts(pipelines.device.Get(), slot.gpu, budget.limit, b, d) ||
                    b < slot.gpu.sss.bytes || b - slot.gpu.sss.bytes < slot.gpu.water.bytes)
                    return false;
                base = (std::max)(base, b - slot.gpu.sss.bytes - slot.gpu.water.bytes);
                depth = (std::max)(depth, d);
                water_bytes = (std::max)(water_bytes, slot.gpu.water.bytes);
            }
            const UINT64 kept = Kept();
            auto texture = TextureDesc(s.gpu.w, s.gpu.h, DXGI_FORMAT_R32_FLOAT);
            texture.Flags = D3D12_RESOURCE_FLAG_NONE;
            auto buffer = rrprivate::Buffer(((UINT64(s.gpu.w) * 4 + 255) & ~255ull) * s.gpu.h);
            const auto texture_bytes =
                           pipelines.device->GetResourceAllocationInfo(0, 1, &texture).SizeInBytes,
                       buffer_bytes =
                           pipelines.device->GetResourceAllocationInfo(0, 1, &buffer).SizeInBytes;
            if (!texture_bytes || !buffer_bytes || texture_bytes > budget.limit ||
                buffer_bytes > budget.limit - texture_bytes)
                return false;
            depth = (std::max)(depth, texture_bytes + buffer_bytes);
            UINT64 required{};
            const auto optional_sss = rrsss::Gpu::RequiredBytes(pipelines.device.Get(), s.gpu.w,
                                                                s.gpu.h, stencil.samples);
            // Water storage (rr_water.h) takes the otherwise unused hit term.
            if (!rrtrans::RequiredSlots(slots.size(), base, depth, optional_sss, 0, water_bytes,
                                        kept, required) ||
                !budget.Reserve(pipelines.device.Get(), required) ||
                !rrsss::FitsOwnedBudget(base, depth, kept, optional_sss, water_bytes, budget.limit,
                                        slots.size())) {
                return false;
            }
        }
        bool issued = false;
        try {
            ComPtr<ID3D12Device> d;
            Check(cl->GetDevice(IID_PPV_ARGS(&d)), "SSS producer device missing");
            Require(fg::SameNativeObject(d.Get(), pipelines.device.Get()),
                    "SSS producer device mismatch");
            if (!PrepareSssAllocation(GetTickCount64(), s.gpu.sss.ready, [&] {
                    s.gpu.sss.Prepare(s.gpu, pipelines, budget, stencil.samples);
                }))
                return false;
            if (early) {
                s.busy = true;
                s.awaiting_prepass = true;
                s.frame = camera_frame;
                s.viewport = camera_view;
                s.generation = generation;
            }
            s.sss.command = cl;
            s.sss.variant = variant;
            s.sss.original = original;
            s.sss.stencil = stencil;
            s.sss.mark = CoverageMarking(s, stencil);
            s.sss.live = true;
            rrinternal::Scope copies;
            rrcompute::Tracker::Internal state;
            rrsss::DrawResult result;
            result = s.sss.Record(cl, draw, s.gpu.sss);
            issued = result.issued;
            s.sss.written = result.issued && !result.failed;
            if (result.failed) {
                sss_failed = true;
                policy.Reject();
                Log("RR SSS draw failure contained; optional guide disabled, issued draw not "
                    "repeated");
            }
            return result.issued;
        } catch (const std::exception &e) {
            sss_failed = true;
            s.sss.written = false;
            policy.Reject();
            try {
                Log(e.what());
            } catch (...) {
            }
            return issued;
        } catch (...) {
            sss_failed = true;
            s.sss.written = false;
            policy.Reject();
            return issued;
        }
    }
    void (*logger)(const char *){};
    bool (*lighting_valid)(UINT64, UINT64){};
  private:
    bool LightingValid(const Slot &s) const {
        return s.raw_corrected && lighting_valid && lighting_valid(s.lighting_device, s.lighting_revision);
    }
  public:

    rrstatus::Snapshot Status() {
        std::lock_guard lock(mutex);
        return status.Read({enabled, sss_enabled, transparency_enabled, failed, retired, sss_failed,
                            transparency_failed, hit_enabled},
                           GetTickCount64());
    }
    bool Enabled() {
        std::lock_guard lock(mutex);
        return enabled;
    }
    // Native SR history is reset when its evaluations resume (rrtrace::Probe).
    // Changing the setting also clears a pause after refused replacement frames.
    void SetReplaceSR(bool value) {
        std::lock_guard lock(mutex);
        if (replace_sr == value)
            return;
        replace_sr = value;
        replace_paused = false;
        replace_refusals = 0;
    }
    bool ReplaceSRPaused() {
        std::lock_guard lock(mutex);
        return replace_sr && replace_paused;
    }
    void SetEnabled(bool value) {
        std::lock_guard lock(mutex);
        if (enabled == value)
            return;
        enabled = value;
        if (++generation == 0) {
            Failure("RR enable generation overflow", true);
            return;
        }
        status.Reset(generation);
        policy.Reject();
    }
    void Toggle() {
        std::lock_guard lock(mutex);
        SetEnabled(!enabled);
    }
    bool RequestPreset(unsigned value) {
        std::lock_guard lock(mutex);
        return preset.Request(value);
    }
    rrpreset::Status PresetStatus() {
        std::lock_guard lock(mutex);
        return {preset.desired, preset.effective, preset.has_effective,
                preset.Pending() || layout_pending};
    }
    rreval::Options Options() {
        std::lock_guard lock(mutex);
        return observed_options;
    }
    void Camera(UINT frame, UINT viewport, const sl::Constants &c) {
        std::lock_guard lock(mutex);
        latest = rrtrace::ReadCamera(c);
        camera_frame = frame;
        camera_view = viewport;
        camera_time = GetTickCount64();
    }

    void Poll() {
        RuntimeReport report;
        {
            std::lock_guard lock(mutex);
            try {
                PollLocked();
            } catch (const std::exception &e) {
                Failure(e.what());
            }
            report = std::exchange(runtime_report, {});
        }
        if (report.core)
            try {
                Log(rrngxlog::RuntimeLine(report.core, report.device.Get()).c_str());
            } catch (...) {
                // A line that cannot be built is dropped: nothing may escape into ReShade.
            }
    }
    // All native identity/binding/view/constant/context proofs are required at
    // the call site. Prepare completes before dispatch; once native dispatch is
    // issued it MUST be suppressed at the wrapper even if later conversion fails.
    bool Prepass(ID3D12GraphicsCommandList *cl,
                 const std::array<ComPtr<ID3D12Resource>, 4> &textures,
                 const std::array<ComPtr<ID3D12Resource>, 2> &cb,
                 const std::array<UINT64, 2> &offsets, const rrcompute::Snapshot &restore, UINT x,
                 UINT y, UINT z) {
        std::lock_guard lock(mutex);
        if (!enabled || retired)
            return false;
        bool dispatched = false;
        try {
            PollLocked();
            if (!Recover())
                return false;
            if (!restore.valid || !restore.heap_restore_valid || restore.command != cl ||
                camera_frame == UINT_MAX || camera_view == UINT_MAX || !latest.readable ||
                !latest.basis_valid || !latest.projection_valid ||
                GetTickCount64() - camera_time > 1000) {
                return false;
            }
            if (textures[3]) {
                const auto incoming = textures[3]->GetDesc();
                for (const auto &slot : slots)
                    if (slot.gpu.heap &&
                        (incoming.Width != slot.gpu.w || incoming.Height != slot.gpu.h)) {
                        QueueReconfigure();
                        break;
                    }
            }
            if (selected_view != UINT_MAX && selected_view != camera_view)
                QueueReconfigure();
            ComPtr<ID3D12Device> incoming_device;
            Check(cl->GetDevice(IID_PPV_ARGS(&incoming_device)), "live prepass device missing");
            if (initialized && !fg::SameNativeObject(pipelines.device.Get(), incoming_device.Get()))
                QueueReconfigure();
            if (!Reconfigure())
                return false;
            for (auto &s : slots)
                if (s.pre_live &&
                    (s.pre.Get() == cl || (s.frame == camera_frame && s.viewport == camera_view))) {
                    return false;
                }
            if (selected_view != UINT_MAX && selected_view != camera_view) {
                return false;
            }
            Slot *free{};
            for (auto &s : slots)
                if (s.busy && s.awaiting_prepass && s.frame == camera_frame &&
                    s.viewport == camera_view) {
                    if (free || s.generation != generation || s.poisoned || s.recorded ||
                        s.pending_sr || s.sss.command.Get() == cl ||
                        s.transparency.preparation.Get() == cl ||
                        s.transparency.anchor.Get() == cl) {
                        return false;
                    }
                    free = &s;
                }
            if (!free)
                for (auto &s : slots)
                    if (!s.busy) {
                        free = &s;
                        break;
                    }
            if (!free)
                free = GrowPool();
            if (!free) {
                status.Fallback(generation, GetTickCount64());
                policy.Reject();
                return false;
            }
            ComPtr<ID3D12Device> d;
            Check(cl->GetDevice(IID_PPV_ARGS(&d)), "live prepass device missing");
            const auto guide_extent = textures[3]->GetDesc();
            Require(guide_extent.Width <= UINT_MAX, "live prepass guide extent unsupported");
            auto &s = *free;
            // Only a new slot (or a new RR) allocates. Under GPU memory pressure (another
            // add-on's VRAM, for one) Initialize and Prepare leave nothing behind, this frame
            // stays native and a later one retries.
            const bool fresh = !initialized || !s.gpu.heap;
            if (fresh && MemoryWait()) {
                status.Fallback(generation, GetTickCount64());
                policy.Reject();
                return false;
            }
            try {
                Initialize(d.Get());
                if (!s.gpu.heap && !ReservePool(slots.size(), nullptr, nullptr,
                                                UINT(guide_extent.Width), guide_extent.Height))
                    throw rrprivate::AllocationDeferred();
                s.gpu.Prepare(pipelines, budget, textures, cb, offsets);
                if (fresh) {
                    memory_logged = false;
                    ReserveSlots(s, textures, cb, offsets);
                }
            } catch (const rrprivate::AllocationDeferred &) {
                MemoryDeferred("RR frame slot");
                status.Fallback(generation, GetTickCount64());
                policy.Reject();
                return false;
            }
            s.busy = true;
            // A null classification view unless a water test wants it this frame.
            // The guide mode follows it, so every water test waits and fails together.
            s.gpu.WaterTarget(pipelines, WaterWanted() && !water_failed &&
                                             PrepareWater(s.gpu, GetTickCount64()));
            s.pre = cl;
            s.pre_live = true;
            s.awaiting_prepass = false;
            s.frame = camera_frame;
            s.viewport = camera_view;
            s.generation = generation;
            s.camera = latest;
            s.view = rreval::WorldToView(latest.position, latest.right, latest.up, latest.forward);
            std::copy_n(latest.projection, 16, s.projection.begin());
            rrinternal::Scope copy_guard;
            rrcompute::Tracker::Internal state_guard;
            cl->Dispatch(x, y, z);
            dispatched = true;
            s.gpu.Convert(cl, pipelines);
            CoverageKey(s, cl);
            restore.Restore(cl, true);
            return true;
        } catch (const std::exception &e) {
            Failure(e.what());
            if (dispatched) {
                rrinternal::Scope a;
                rrcompute::Tracker::Internal b;
                restore.Restore(cl, true);
            }
            return dispatched;
        }
    }

    void RawCorrected(ID3D12GraphicsCommandList *cl, UINT64 device, UINT64 revision) {
        std::lock_guard lock(mutex);
        for (auto &s : slots)
            if (enabled && !failed && !retired && s.generation == generation && s.pre_live &&
                s.pre.Get() == cl && !s.recorded) {
                s.raw_corrected = true;
                s.lighting_device = device;
                s.lighting_revision = revision;
            }
    }
    bool RawSlot(const Slot &s, ID3D12GraphicsCommandList *cl) const {
        return enabled && !failed && !retired && s.generation == generation && s.pre_live &&
               s.pre.Get() == cl && !s.pending_sr && !s.recorded && !s.poisoned;
    }
    bool CanRaw(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(mutex);
        for (auto &s : slots)
            if (RawSlot(s, cl))
                return true;
        return false;
    }
    bool NativeSampling(ID3D12GraphicsCommandList *cl, const rrvrrt::Inputs &inputs,
                        const rrcompute::Snapshot &restore) {
        std::lock_guard lock(mutex);
        if (!restore.valid || !restore.complete || !restore.heap_restore_valid ||
            restore.command != cl)
            return false;
        Slot *match{};
        for (auto &s : slots)
            if (RawSlot(s, cl)) {
                if (match || s.native_sampling) {
                    Failure("RR ambiguous/repeated native VRRT in one guide recording; raw path "
                            "blocked");
                    return false;
                }
                match = &s;
            }
        if (!match)
            return false;
        if (!match->gpu.probability && MemoryWait())
            return false;
        try {
            rrinternal::Scope copies;
            rrcompute::Tracker::Internal state;
            match->gpu.Probability(cl, pipelines, budget, inputs);
            restore.Restore(cl, true);
            match->native_sampling = true;
            return true;
        } catch (const rrprivate::AllocationDeferred &) {
            MemoryDeferred("RR sampling buffer"); // Allocated before anything is recorded.
            return false;
        } catch (const std::exception &e) {
            Failure(e.what());
            return false;
        }
    }
    rrvrrt::Data SamplingData(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(mutex);
        rrvrrt::Data result;
        for (auto &s : slots)
            if (RawSlot(s, cl) && s.native_sampling) {
                if (result.buffer)
                    return {};
                result = {s.gpu.probability, s.gpu.w, s.gpu.h};
            }
        return result;
    }
    bool SamplingReady(ID3D12GraphicsCommandList *cl) {
        return bool(SamplingData(cl).buffer);
    }
    // Only while the correction still holds: a proof skipped later in the recording (rrbypass::Skip)
    // leaves the native filters running for the frame RR will reject.
    bool RawSpatialPassthrough(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(mutex);
        for (auto &s : slots)
            if (RawSlot(s, cl) && LightingValid(s))
                return true;
        return false;
    }
    void BeforeSR(ID3D12GraphicsCommandList *public_cl, const NVSDK_NGX_Parameter *p,
                  rrtrace::SlCall scope, bool safe) {
        std::lock_guard lock(mutex);
        auto actual = rrinputs::ReadEvaluation(p);
        auto creation = rrtrace::Probe::CurrentCreation();
        if (creation.valid) {
            actual.out_width = creation.out_width;
            actual.out_height = creation.out_height;
            actual.flags = creation.flags;
            actual.quality = creation.quality;
        }
        if (rreval::Valid(actual))
            observed_options = actual;
        if (failed || retired)
            return;
        if (feature && rreval::Valid(actual) && !rreval::SameCreation(key, actual)) {
            QueueReconfigure();
            return;
        }
        auto *s = Match(scope);
        if (!s) {
            return;
        }
        try {
            // A refused call must not leave an earlier call's list and states to match.
            s->pending_sr = false;
            s->states = {};
            Require(safe && LightingValid(*s),
                    "RR SR rejected: lighting route or pipeline revision unproven");
            auto cl = fg::NativeObject<ID3D12GraphicsCommandList>(public_cl);
            Require(bool(cl), "live native SR list missing");
            s->options = actual;
            Require(rreval::Valid(s->options) && s->options.width == s->gpu.w &&
                        s->options.height == s->gpu.h,
                    "live SR dimensions/options missing or mismatched");
            auto input = rrtrace::ReadInputs(p);
            void *raw[] = {input.color, input.depth, input.motion, input.output};
            for (unsigned i = 0; i < 4; ++i) {
                s->gpu.sr[i] = fg::NativeObject<ID3D12Resource>((ID3D12Resource *)raw[i]);
                Require(bool(s->gpu.sr[i]) &&
                            rrsparsecopy::SameDevice(pipelines.device.Get(), s->gpu.sr[i].Get()),
                        "live SR resource identity mismatch");
            }
            s->sr = cl;
            s->sr_live = true;
            s->pending_sr = true;
            s->states = {};
        } catch (const std::exception &e) {
            policy.Reject();
            Log(e.what());
        }
    }
    void Barriers(ID3D12GraphicsCommandList *cl, UINT n, const D3D12_RESOURCE_BARRIER *b) {
        std::lock_guard lock(mutex);
        TransparencyProducerBarriersLocked(cl, n, b);
        if (n > 4096 || (n && !b))
            return;
        for (auto &s : slots)
            if (s.pending_sr && s.sr.Get() == cl)
                for (UINT i = 0; i < n; ++i)
                    if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                        auto &t = b[i].Transition;
                        for (unsigned k = 0; k < 4; ++k)
                            if (t.pResource == s.gpu.sr[k].Get() &&
                                (t.Subresource == 0 ||
                                 t.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES))
                                s.states[k] = {t.StateAfter, true,
                                               b[i].Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE};
                    }
    }
    bool TryReplaceSR(ID3D12GraphicsCommandList *public_cl, const NVSDK_NGX_Parameter *p,
                      rrtrace::SlCall scope, HMODULE mod, UINT64 epoch, bool safe,
                      rrsparsecopy::Layouts &layouts) {
        std::lock_guard lock(mutex);
        // Each refusal names itself for the health line (rrsr::counters).
        const auto skip = [this](unsigned why) {
            rrsr::Note(why);
            if (why != rrsr::Rejected)
                replace_refusals = 0; // an SR call between refusals: not in a row
            return false;
        };
        if (!replace_sr || replace_paused)
            return skip(rrsr::Off);
        if (foreign_call)
            return skip(rrsr::Scope);
        if (!enabled)
            return skip(rrsr::Disabled);
        if (failed || retired)
            return skip(rrsr::Failed);
        if (NeedsReconfigure())
            return skip(rrsr::Reconfigure);
        if (!safe)
            return skip(rrsr::Unsafe);
        if (!scope.valid)
            return skip(rrsr::Scope);
        auto native = fg::NativeObject<ID3D12GraphicsCommandList>(public_cl);
        auto *s = Match(scope);
        if (!s || !s->pending_sr)
            return skip(rrsr::Scope);
        if (!native || s->sr.Get() != native.Get())
            return skip(rrsr::List);
        if (!rrsr::ReplaceUnderForeignHook && rrtrace::Probe::ForeignBeneath())
            return skip(rrsr::Foreign);
        rrsr::StateProof proof;
        bool untracked = false;
        for (unsigned i = 0; i < 4; ++i) {
            proof.resources[i] = reinterpret_cast<uintptr_t>(s->gpu.sr[i].Get());
            const auto seen = layouts.Status(native.Get(), s->gpu.sr[i].Get(), proof.states[i]);
            proof.known[i] = seen == rrsparsecopy::Layouts::Seen::Known;
            untracked |= seen == rrsparsecopy::Layouts::Seen::Untracked;
        }
        // Only an input no barrier on this list named may take the contract state: one whose
        // transition was split, per subresource or unrecorded has no state RR can start from.
        const bool eligible = !untracked && rrsr::Eligible(proof);
        if (untracked)
            return skip(rrsr::Untracked);
        if (!eligible)
            return skip(rrsr::Ineligible);
        // An input the list never named stays unknown: AfterSR takes its contract state.
        for (unsigned i = 0; i < 4; ++i)
            if (proof.known[i])
                s->states[i] = {proof.states[i], true, true};
        if (!AfterSR(public_cl, p, scope, NVSDK_NGX_Result_Success, mod, epoch, safe, true)) {
            // Waiting for GPU memory is no refusal: native SR upscales the frame meanwhile.
            if (memory_retry_after)
                return skip(rrsr::Memory);
            // A refused frame loses RR (native SR runs, AfterSR does not run again), so a
            // check that keeps refusing pauses replacement until the setting changes.
            if (++replace_refusals == 3) {
                replace_paused = true;
                Log("SR replacement off: RR refused 3 replacement frames in a row (reason "
                    "above); the game's SR runs until the setting is turned off and on");
            }
            return skip(rrsr::Rejected);
        }
        replace_refusals = 0;
        rrsr::Note(rrsr::None);
        return true;
    }
    bool AfterSR(ID3D12GraphicsCommandList *public_cl, const NVSDK_NGX_Parameter *p,
                 rrtrace::SlCall scope, NVSDK_NGX_Result result, HMODULE mod, UINT64 epoch,
                 bool safe, bool replacement = false) {

        std::lock_guard lock(mutex);
        if (foreign_call)
            return false;
        Slot *s{};
        for (auto &v : slots)
            if (v.pending_sr && v.frame == scope.frame && v.viewport == scope.viewport) {
                if (s) {
                    return false;
                }
                s = &v;
            }
        if (!s) {
            return false;
        }
        s->pending_sr = false;
        if (failed || retired)
            return false;
        try {
            auto cl = fg::NativeObject<ID3D12GraphicsCommandList>(public_cl);
            Require(LightingValid(*s) && safe &&
                        NVSDK_NGX_SUCCEED(result) && cl.Get() == s->sr.Get() && scope.valid &&
                        scope.feature == sl::kFeatureDLSS && mod,
                    "live original SR failed or scope changed");
            ComPtr<ID3D12Device> pub;
            Check(public_cl->GetDevice(IID_PPV_ARGS(&pub)), "live public device missing");
            if (rrsr::ReplaceUnderForeignHook && !pipelines.announced &&
                fg::SameNativeObject(pub.Get(), pipelines.device.Get())) {
                pipelines.announced = pub;
                Log(pub.Get() != fg::NativeObject<ID3D12Device>(pub.Get()).Get()
                        ? "RR's output and depth are created through ReShade's device, announced "
                          "to other add-ons"
                        : "RR's output and depth are created on the native device: other add-ons "
                          "are not told of them");
            }
            if (!Key(s->options, mod, epoch, pub.Get(), scope.viewport)) {
                QueueReconfigure();
                return false;
            }
            auto input = rrtrace::ReadInputs(p);
            void *raw[] = {input.color, input.depth, input.motion, input.output};
            for (unsigned i = 0; i < 4; ++i)
                Require(fg::SameNativeObject((ID3D12Resource *)raw[i], s->gpu.sr[i].Get()),
                        "live SR input identity changed");
            auto &o = s->options;
            const UINT xs[] = {o.color_x, o.depth_x, o.motion_x},
                       ys[] = {o.color_y, o.depth_y, o.motion_y};
            for (unsigned i = 0; i < 4; ++i) {
                auto d = s->gpu.sr[i]->GetDesc();
                Require(d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                            d.DepthOrArraySize == 1 && d.MipLevels == 1 && d.SampleDesc.Count == 1,
                        "live SR unsupported resource shape");
                if (i < 3)
                    Require(xs[i] <= d.Width && o.width <= d.Width - xs[i] && ys[i] <= d.Height &&
                                o.height <= d.Height - ys[i],
                            "live SR subrect exceeds resource");
                else
                    Require(d.Width == o.out_width && d.Height == o.out_height &&
                                (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
                                (d.Format == DXGI_FORMAT_R11G11B10_FLOAT ||
                                 d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                                 d.Format == DXGI_FORMAT_R32G32B32A32_FLOAT) &&
                                rrtrace::ReadUnsigned(
                                    p, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0) == 0 &&
                                rrtrace::ReadUnsigned(
                                    p, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0) == 0,
                            "live output shape/format/subrect unsupported");
            }
            const auto color_format = s->gpu.sr[0]->GetDesc().Format,
                       motion_format = s->gpu.sr[2]->GetDesc().Format;
            Require((color_format == DXGI_FORMAT_R11G11B10_FLOAT ||
                     color_format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                     color_format == DXGI_FORMAT_R32G32B32A32_FLOAT) &&
                        (motion_format == DXGI_FORMAT_R16G16_FLOAT ||
                         motion_format == DXGI_FORMAT_R32G32_FLOAT),
                    "live color/motion format unsupported");
            Require(!s->states[3].known ||
                        (s->states[3].complete &&
                         s->states[3].state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                    "live output observed state contradicts successful SR UAV postcondition");
            auto depth_format = s->gpu.sr[1]->GetDesc().Format;
            Require(depth_format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
                        depth_format == DXGI_FORMAT_R32_FLOAT,
                    "live depth format unsupported");
            std::array<D3D12_RESOURCE_STATES, 3> states;
            for (unsigned i = 0; i < 3; ++i) {
                auto &v = s->states[i];
                Require(!v.known || (v.complete && rrsr::ReadOnly(v.state)),
                        "live post-SR observed input state conflicts");
                states[i] = v.known ? v.state : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            }
            auto sr_inputs = s->gpu.sr;
            const auto native_depth = sr_inputs[1]->GetDesc();
            if (s->gpu.depth) {
                const auto cached = s->gpu.depth->GetDesc();
                if (native_depth.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT &&
                    (cached.Width != native_depth.Width || cached.Height != native_depth.Height)) {
                    QueueReconfigure();
                    return false;
                }
            }
            const bool sr_allocates =
                !s->gpu.rr_output ||
                (native_depth.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT && !s->gpu.depth);
            if (sr_allocates && MemoryWait()) {
                policy.Reject();
                return false;
            }
            try {
                if (sr_allocates && !ReservePool(slots.size(), &o, &native_depth))
                    throw rrprivate::AllocationDeferred();
                s->gpu.PrepareSR(pipelines, budget, o, sr_inputs);
                if (sr_allocates) {
                    memory_logged = false;
                    ReserveOutputs(*s, o, sr_inputs);
                }
            } catch (const rrprivate::AllocationDeferred &) {
                MemoryDeferred("RR output");
                policy.Reject();
                return false;
            }
            Require(next_serial < UINT_MAX, "live serial overflow");
            s->serial = ++next_serial;
            const unsigned availability = !sss_enabled               ? 0
                                          : sss_failed               ? 1
                                          : !s->sss.command          ? 2
                                          : !s->sss.live             ? 3
                                          : !s->sss.written          ? 4
                                          : (o.color_x || o.color_y) ? 5
                                                                     : 6;
            s->sss_used = availability == 6;

            // Prebind only an already-written owned producer snapshot; first composition
            // may be recorded later on the CPU. Before seals exact original queue order,
            // and GPU Resolve writes zero without reading the snapshot on rejection.
            const auto &t = s->transparency;
            s->transparency_used =
                transparency_enabled && !transparency_failed &&
                (!t.producer_copy || !transparency_producer_suspended) && t.written &&
                t.prep_live && (t.producer_copy || (t.anchored && t.anchor_live)) && !o.color_x &&
                !o.color_y && s->gpu.transparency.guide &&
                s->gpu.transparency.Matches(t.source->GetDesc(), o.width, o.height) &&
                color_format == DXGI_FORMAT_R11G11B10_FLOAT;

            // An optional per-evaluation hint may be NULL without invalidating native
            // radiance/history. Explicit toggles and real frame rejection still Reject.
            s->hit_used = hit_enabled && s->gpu.hit_ready && s->gpu.specular_hit &&
                          !o.color_x && !o.color_y;
            // The water mask reads the classification this slot's prepass wrote.
            s->water_used = WaterWanted() && !water_failed && s->gpu.water_written &&
                            !o.color_x && !o.color_y;

            auto projection = s->projection;
            // Weapon depth: RR's depth type is fixed when RR is created, so a frame follows
            // the feature's type and a new feature takes the setting's (rrweapondepth::Choose).
            // The weapon slice is remapped with the projection RR is given.
            const auto weapon_changes = rrweapondepth::changes.load(std::memory_order_relaxed);
            const bool weapon_wanted = WeaponDepthWanted();
            rrweapondepth::Mapping weapon_map;
            const bool weapon_mapped =
                rrweapondepth::Wants(feature, weapon_linear, weapon_wanted) &&
                rrweapondepth::Map(projection, weapon_map);
            const bool weapon_texture =
                UseTest(weapon_test, weapon_mapped, weapon_changes, s->gpu.w, s->gpu.h,
                        DXGI_FORMAT_R32_FLOAT, "Weapon depth");
            const auto weapon_frame =
                rrweapondepth::Choose(feature, weapon_linear, weapon_wanted, weapon_mapped,
                                      weapon_texture, weapon_test.blocked);
            bool weapon_on = weapon_frame.linear;
            if (weapon_frame.hold != WeaponHold::None)
                HoldWeaponDepth(weapon_frame.hold, weapon_changes);
            if (weapon_frame.restart) {
                QueueReconfigure();
                return false;
            }

            const bool reset =
                policy.Next(s->frame, s->generation, o.reset == 1 || s->camera.reset == 1);
            if (reset)
                health.Reset(policy.LastReason());
            s->gpu.Header(s->serial, reset, replacement, o.color_x, o.color_y, tolerate_isolated);
            rrinternal::Scope copy_guard;
            rrcompute::Tracker::Internal state_guard;
            s->recorded = true;
            if (!feature) {
                runtime = mod;
                runtime_epoch = epoch;
                public_device = pub;
                // Logged by the next Poll whether or not this attempt creates RR.
                runtime_report = {mod, pub};
                selected_view = scope.viewport;
                key = o;
                api = resolve(mod);
                Require(api.Valid(), "live NGX exports missing");
                const auto allocated = api.allocate(&parameters);
                rrngxlog::Require(NVSDK_NGX_SUCCEED(allocated) && parameters,
                                  "live NGX parameter allocation failed",
                                  rrngxlog::Returned(allocated, parameters));
                // The preset RR is created with: a request during the creation call below waits
                // for the next RR.
                const auto created_preset = preset.desired;
                rreval::CreateParameters(*parameters, o, created_preset);
                Require(rrpreset::Requested(*parameters, created_preset),
                        "live RR preset parameter readback failed");
                NVSDK_NGX_Handle *created_feature{};
                auto created = Foreign([&] {
                    return api.create(public_cl, NVSDK_NGX_Feature_RayReconstruction, parameters,
                                      &created_feature);
                });
                feature = created_feature;
                char line[200];
                rrngxlog::CapabilityLine(api, line, sizeof(line));
                Log(line);
                rrngxlog::Require(NVSDK_NGX_SUCCEED(created) && feature,
                                  "live RR feature creation failed",
                                  rrngxlog::Returned(created, feature));
                preset.Created(created_preset);
                weapon_linear = weapon_on;
                rrngxlog::DescribeCreation(o, created_preset, line, sizeof(line));
                Log(line);
                if (weapon_on)
                    Log("Weapon depth: on (RR reads the weapon-remapped depth copy)");
            }
            // Water responsivity mask, written by WaterPass below.
            const bool water_responsivity = rrwater::UseResponsivity(
                water, s->water_used, preset.has_effective, preset.effective);
            if (s->water_used)
                s->gpu.WaterHeader(rrwater::Clamp(water.strength),
                                   rrwater::Flags(water_responsivity));
            water_status = {};
            water_status.ready = s->water_used;
            water_status.responsivity = water_responsivity;
            rreval::Resources resources{s->gpu.sr[0].Get(),
                                        weapon_on          ? weapon_test.texture.Get()
                                        : s->gpu.use_depth ? s->gpu.depth.Get()
                                                           : s->gpu.sr[1].Get(),
                                        s->gpu.sr[2].Get(),
                                        s->gpu.rr_output.Get(),
                                        s->gpu.guides[0].Get(),
                                        s->gpu.guides[1].Get(),
                                        s->gpu.guides[2].Get(),
                                        s->sss_used ? s->gpu.sss.guide.Get() : nullptr,
                                        s->transparency_used ? s->gpu.transparency.guide.Get()
                                                             : nullptr,
                                        water_responsivity ? s->gpu.water.responsivity.Get()
                                                           : nullptr,
                                        s->hit_used ? s->gpu.specular_hit.Get() : nullptr};
            s->responsivity_used = resources.responsivity != nullptr;
            // The weapon depth copy is read at its own origin.
            auto evaluated = o;
            if (weapon_on)
                evaluated.depth_x = evaluated.depth_y = 0;
            if (weapon_on && (weapon_map.scale != weapon_logged.scale ||
                              weapon_map.offset != weapon_logged.offset ||
                              weapon_map.sky != weapon_logged.sky)) {
                weapon_logged = weapon_map;
                char line[224];
                snprintf(line, sizeof(line),
                         "Weapon depth: world depth %.7g / (depth + %.7g), sky %.7g, weapon "
                         "slice in front of %.7g",
                         weapon_map.scale, weapon_map.offset, weapon_map.sky,
                         weapon_map.scale / (1 + weapon_map.offset));
                Log(line);
            }
            rreval::EvaluateParameters(*parameters, evaluated, resources, s->view.data(),
                                       projection.data());
            parameters->Set(NVSDK_NGX_Parameter_Reset, reset ? 1 : 0);
            s->gpu.Depth(cl.Get(), states[1]);
            for (unsigned i = 0; i < 3; ++i)
                if (i != 1 || !s->gpu.use_depth)
                    Change(cl.Get(), s->gpu.sr[i].Get(), states[i],
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 0);
            PrepareFgDepth(*s, cl.Get());
            s->gpu.GateBegin(cl.Get(), pipelines, history.Get());
            if (s->sss_used) {
                s->gpu.sss.Resolve(cl.Get(), pipelines, s->gpu.header.Get(), s->gpu.stats.Get(),
                                   history.Get(), CoverageTarget(*s));
            }
            CoverageBlend(*s, cl.Get(), o.width, o.height);
            if (s->transparency_used)
                s->gpu.transparency.Resolve(cl.Get(), transparency_pipeline, s->gpu.header.Get());
            if (s->water_used)
                s->gpu.WaterPass(cl.Get(), pipelines);
            if (s->hit_used)
                Change(cl.Get(), s->gpu.specular_hit.Get(), D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (weapon_on)
                s->gpu.WeaponDepth(cl.Get(), pipelines, weapon_test.texture.Get(), o.depth_x,
                                   o.depth_y, weapon_map);

            NVSDK_NGX_Result evaluated_result = NVSDK_NGX_Result_FAIL_PlatformError;
            bool threw = false; // then evaluated_result is the sentinel above, not NGX's
            try {
                evaluated_result =
                    Foreign([&] { return api.evaluate(public_cl, feature, parameters, nullptr); });
            } catch (...) {
                threw = true;
                Log("RR evaluation threw; restoring resources for native SR");
            }
            s->inference_ok = NVSDK_NGX_SUCCEED(evaluated_result);
            if (s->hit_used)
                Change(cl.Get(), s->gpu.specular_hit.Get(),
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            if (weapon_test.active)
                s->gpu.TestFinish(cl.Get(), weapon_test.texture.Get());
            if (s->water_used)
                s->gpu.WaterFinish(cl.Get());
            if (s->transparency_used)
                s->gpu.transparency.Finish(cl.Get());
            if (s->sss_used)
                s->gpu.sss.Finish(cl.Get());
            s->gpu.GateEnd(cl.Get(), pipelines, history.Get(), s->inference_ok);
            s->gate_recorded = true;
            s->replaced_sr = replacement && s->inference_ok;
            for (unsigned i = 0; i < 3; ++i)
                if (i != 1 || !s->gpu.use_depth)
                    Change(cl.Get(), s->gpu.sr[i].Get(),
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, states[i], 0);
            s->recorded = true;
            if (!s->inference_ok) {
                const rrngxlog::Code code(evaluated_result);
                const char *outcome = threw ? "threw" : code.text;
                // A linear-depth RR that fails: restart it with hardware depth rather than
                // turning RR off.
                if (weapon_on) {
                    HoldWeaponDepth(WeaponHold::Refused, weapon_changes, outcome);
                    QueueReconfigure();
                } else {
                    char line[128];
                    snprintf(line, sizeof(line), "RR inference failed (%s); native SR retained",
                             outcome);
                    Failure(line);
                }
            }
            return s->inference_ok;
        } catch (const std::exception &e) {
            Failure(e.what());
            return false;
        }
    }
    void Reset(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(mutex);
        for (auto &s : slots) {
            if (s.pre.Get() == cl)
                s.pre_live = false;
            if (s.sss.command.Get() == cl)
                s.sss.live = false;
            if (s.transparency.preparation.Get() == cl)
                s.transparency.prep_live = false;
            if (s.transparency.anchor.Get() == cl)
                s.transparency.anchor_live = false;
            CoverageReset(s, cl);
            if (s.sr.Get() == cl) {
                s.sr_live = false;
                s.pending_sr = false;
            }
            if (s.fg_list.Get() == cl) {
                // Reset before submission: the check's copies never run.
                if (s.fg_probe && !s.fg_probe_submitted) {
                    s.fg_probe = false;
                    fg_probe = {};
                }
                s.fg_list_live = false;
            }
            if (!s.recorded && (s.pre.Get() == cl || s.sr.Get() == cl))
                policy.Reject();
        }
        PollLocked();
    }
    void Before(ID3D12CommandQueue *q, UINT n, ID3D12CommandList *const *lists) {
        Call call;
        submissions.push_back(std::move(call));
        if (n && !lists)
            return;
        mutex.lock();
        submissions.back().locked = true;
        submissions.back().touched.resize(slots.size());
        std::vector<int> pre(slots.size(), -1), producer(pre), consumer(pre), glass_pre(pre),
            glass_anchor(pre), cover(pre), fg_copy(pre);
        bool any = false;
        auto index = [&](ID3D12CommandList *c) { return rrsss::RecordingIndex(n, lists, c); };
        for (unsigned k = 0; k < slots.size(); ++k) {
            const auto &s = slots[k];
            pre[k] = s.pre_live ? index(s.pre.Get()) : -1;
            producer[k] = s.sss.live ? index(s.sss.command.Get()) : -1;
            consumer[k] = s.sr_live ? index(s.sr.Get()) : -1;
            glass_pre[k] = s.transparency.prep_live ? index(s.transparency.preparation.Get()) : -1;
            glass_anchor[k] = s.transparency.anchor_live ? index(s.transparency.anchor.Get()) : -1;
            cover[k] = s.coverage.Live() ? index(s.coverage.capture.Get()) : -1;
            fg_copy[k] = s.fg_list_live ? index(s.fg_list.Get()) : -1;
            if (rrsss::Present(pre[k]) || rrsss::Present(producer[k]) ||
                rrsss::Present(consumer[k]) || rrsss::Present(glass_pre[k]) ||
                rrsss::Present(glass_anchor[k]) || rrsss::Present(cover[k]) ||
                rrsss::Present(fg_copy[k])) {
                submissions.back().touched[k] = true;
                any = true;
            }
        }
        if (!any)
            return;
        bool queue_ready = false;
        try {
            ComPtr<ID3D12Device> d;
            Check(q->GetDevice(IID_PPV_ARGS(&d)), "live queue device missing");
            Require(fg::SameNativeObject(d.Get(), pipelines.device.Get()),
                    "live queue identity mismatch");
            if (last_queue && last_queue.Get() != q && signaled) {
                const auto wait_result = q->Wait(fence.Get(), signaled);
                Check(wait_result, "live cross-queue GPU wait failed");
            }
            last_queue = q;
            queue_ready = q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT;
        } catch (const std::exception &e) {
            for (unsigned k = 0; k < slots.size(); ++k)
                if (submissions.back().touched[k])
                    slots[k].poisoned = true;
            Failure(e.what());
        }
        for (unsigned k = 0; k < slots.size(); ++k) {
            if (!submissions.back().touched[k])
                continue;
            auto &s = slots[k];
            const int a = pre[k], c = producer[k], b = consumer[k];
            if (s.recorded && !s.sealed && rrsss::Present(b)) {
                s.sealed = true;
                const bool baseline = queue_ready && a >= 0 && b >= a && !s.pre_submitted &&
                                      !failed && s.gate_recorded && s.inference_ok &&
                                      LightingValid(s);
                const bool sss_accept =
                    s.sss_used && s.sss.written &&
                    rrsss::Admit(a, c, b, s.pre_submitted, s.sss.submitted, baseline);
                s.sss.admission.Seal(b, sss_accept);
                s.gpu.header_map[7] = sss_accept ? 1u : 0u;
                if (s.sss_used)
                    SealSssMask(s, sss_accept);
                const auto route = s.transparency.producer_copy ? rrtrans::Route::Producer
                                   : s.transparency.at_draw     ? rrtrans::Route::AtDraw
                                                                : rrtrans::Route::Prepared;
                s.transparency_accepted =
                    s.transparency_used && s.transparency.written && s.transparency.anchored &&
                    rrtrans::RouteAdmits(route, a, glass_pre[k], glass_anchor[k], b,
                                         s.transparency.prep_submitted,
                                         s.transparency.anchor_submitted, baseline);
                if (s.transparency_used) {
                    const auto misses = s.transparency_accepted
                                            ? 0u
                                            : rrtrans::Misses(route, a, glass_pre[k],
                                                              glass_anchor[k], b,
                                                              s.transparency.prep_submitted,
                                                              s.transparency.anchor_submitted,
                                                              baseline, s.transparency.written,
                                                              s.transparency.anchored);
                    const auto note = transparency_note.For(s.frame, CompositionRecognized());
                    TransparencyAdmissionLocked(s.transparency_accepted, misses, note,
                                                GetTickCount64());
                    if (s.transparency.producer_copy) {
                        if (s.transparency_accepted)
                            transparency_backoff = 0;
                        else if (!transparency_producer_suspended)
                            SuspendTransparencyProducer(nullptr, false, misses, note);
                    }
                }
                // Never part of acceptance: an unadmitted blend only reads no coverage.
                SealCoverage(s, a, cover[k], b, baseline);
                const bool accepted = baseline && (!s.sss_used || sss_accept) &&
                                      (!s.transparency_used || s.transparency_accepted);
                s.gpu.Accept(accepted);

                if (!accepted)
                    policy.Reject();
            } else if (s.recorded && s.sealed && rrsss::Present(b)) {
                policy.Reject();
                Log("RR replayed SR recording; immutable acceptance retained; GPU serial gate "
                    "suppresses output and requires reset");
            }
            if (rrsss::Present(a))
                s.pre_submitted = true;
            if (rrsss::Present(c))
                s.sss.submitted = true;
            if (rrsss::Present(glass_pre[k]))
                s.transparency.prep_submitted = true;
            if (rrsss::Present(glass_anchor[k]))
                s.transparency.anchor_submitted = true;
            if (rrsss::Present(cover[k]))
                s.coverage.submitted = true;
            if (rrsss::Present(fg_copy[k])) {
                s.fg_list_live = false; // the fence signaled after this call covers it
                s.fg_probe_submitted = s.fg_probe;
            }
        }
    }
    void After(ID3D12CommandQueue *q) {
        if (submissions.empty())
            return;
        auto call = submissions.back();
        submissions.pop_back();
        if (!call.locked)
            return;
        {
            std::lock_guard lock(mutex);
            bool any = false;
            for (bool v : call.touched)
                any |= v;
            if (any) {
                const auto value = ++signaled;
                const auto signal_result = q->Signal(fence.Get(), value);
                if (FAILED(signal_result)) {
                    for (size_t k = 0; k < call.touched.size(); ++k)
                        if (call.touched[k])
                            slots[k].poisoned = true;
                    Failure("RR native completion fence failed; allocations retained");
                } else
                    for (size_t k = 0; k < call.touched.size(); ++k)
                        if (call.touched[k]) {
                            slots[k].fence_value = value;
                            slots[k].reported = false;
                        }
            }
        }
        mutex.unlock();
    }
    bool BeforeShutdown(HMODULE mod, ID3D12Device *d) {
        std::lock_guard lock(mutex);
        if (runtime != mod || (d && !fg::SameNativeObject(public_device.Get(), d)))
            return true;
        enabled = false;
        if (foreign_call) {
            Log("RR shutdown deferred: an RR evaluation is in NGX");
            return false;
        }
        for (const auto &s : slots)
            if (s.poisoned || s.pre_live || s.sr_live || s.sss.live || s.transparency.Live() ||
                s.coverage.Live() || s.pending_sr) {
                Log("RR shutdown deferred: an RR command recording remains replayable; "
                    "reset/dispose required");
                return false;
            }
        rrprivate::FencePoint point{fence, signaled};
        if (!point.Wait(30000)) {
            Failure("RR shutdown deferred: live GPU inference unfinished", true);
            return false;
        }
        return true;
    }
    void AfterShutdown(HMODULE mod, ID3D12Device *d, bool success) {
        std::lock_guard lock(mutex);
        if (success && runtime == mod && (!d || fg::SameNativeObject(public_device.Get(), d))) {
            feature = nullptr;
            parameters = nullptr;
            preset.has_effective = false;
            preset.draining = false;
            retired = true;
            enabled = false;
            Log("RR NGX shutdown invalidated private handles; live mode retired");
        }
    }
};
} // namespace rrlive
