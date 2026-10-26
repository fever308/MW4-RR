#pragma once
#include "rr_live_gpu.h"
#include "rr_live_policy.h"
#include "rr_guide_health.h"
#include "rr_water.h"
#include "rr_weapon_depth.h"
#include "rr_status.h"
#include "rr_compute_state.h"
#include "rr_sparse_copy.h"
#include "rr_sss_producer.h"
#include "rr_sss_mask.h"
#include "rr_inputs.h"
#include "rr_internal.h"
#include "rr_ngx_api.h"
#include "rr_ngx_log.h"
#include "rr_trace.h"
#include "rr_sr_eligibility.h"
#include "rr_sl_route.h"
#include <atomic>
#include <mutex>
#include <deque>
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
        std::array<Observed, 4> states{};
        rreval::Options options;
    };
    struct Call {
        bool locked{};
        std::vector<bool> touched;
    };
    static inline thread_local std::vector<Call> submissions;
    std::recursive_mutex mutex;
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
    bool enabled{}, failed{}, initialized{}, history_queued{}, retired{};
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
    struct Trail {
        struct Entry {
            ID3D12GraphicsCommandList *list{};
            D3D12_RESOURCE_STATES before{}, after{};
            UINT subresource{};
            D3D12_RESOURCE_BARRIER_FLAGS flags{};
        };
        struct Seen {
            std::array<Entry, 6> entries{};
            unsigned count{};
        };
        std::array<ID3D12Resource *, 4> watch{}; // opaque: compared, never dereferenced
        std::array<Seen, 4> seen{};
        ID3D12GraphicsCommandList *sr_list{};
        unsigned logged{};
    } trail;
    UINT generation{}, camera_frame = UINT_MAX, camera_view = UINT_MAX, selected_view = UINT_MAX,
                       next_serial{};
    rrtrace::CameraInfo latest;
    // The game's Streamline constants for the latest frame, for RR's own DLSS-D viewport.
    sl::Constants sl_constants{};
    UINT sl_constants_frame = UINT_MAX, sl_constants_view = UINT_MAX;
    unsigned sl_route_failures{}; // in a row; 3 send replacement back to RR's direct call
    bool sl_route_logged{}, sl_vram_logged{};
    bool sl_last_routed{}; // the latest RR evaluate went through Streamline
    bool sl_history_lost{};       // an evaluate threw: the next one resets whichever route it takes
    UINT sl_used_view = UINT_MAX; // RR's Streamline viewport, until RR retires
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
    // Latest guide an admitted SR submission resolves, for SSS-masked lighting.
    rrsssmask::Record sss_mask;
    ComPtr<ID3D12Resource> sss_mask_guide;
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
    void Log(const char *s) {
        if (logger)
            logger(s);
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
    void Failure(const char *why) {
        failed = true;
        policy.Reject();
        Log(why);
    }
    // A failed retirement call logs its NGX result; a missing export has none.
    bool RetirementFailed(const char *what, bool exported, NVSDK_NGX_Result result) {
        char line[160];
        if (exported)
            snprintf(line, sizeof(line), "%s (%s); allocations retained", what,
                     rrngxlog::Code(result).text);
        else
            snprintf(line, sizeof(line), "%s (export missing); allocations retained", what);
        Failure(line);
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
        // RR's Streamline DLSS-D feature goes with RR's own, once the GPU is done with both.
        if (sl_used_view != UINT_MAX) {
            const auto r = rrtrace::Probe::SlFreeRR(sl_used_view);
            if (r != sl::Result::eOk) {
                char line[96];
                snprintf(line, sizeof(line), "RR's Streamline DLSS-D release failed (result %d)",
                         int(r));
                Log(line);
            }
            sl_used_view = UINT_MAX;
            sl_last_routed = false;
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
        CoverageReconfigured();
        initialized = history_queued = false;
        signaled = 0;
        runtime = nullptr;
        runtime_epoch = 0;
        public_device.Reset();
        api = {};
        selected_view = UINT_MAX;
        key = {};
        if (++generation == 0) {
            Failure("RR feature generation overflow");
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
                !s.pending_sr && Complete(s, done)) {
                auto cache = std::move(s.gpu);
                cache.Retire();
                s = {};
                s.gpu = std::move(cache);
                s.busy = false;
            }
        }
        const UINT64 now = GetTickCount64();
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
    void ToggleSss() {
        std::lock_guard lock(mutex);
        SetSssEnabled(!sss_enabled);
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
            const UINT64 kept = persistent.bytes;
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
    // Whether RR's replacement evaluate can go through the game's Streamline for this SR call.
    bool SlRoutable(const rrtrace::SlCall &scope) const {
        return sl_route_failures < 3 && scope.token && rrtrace::Probe::SlRRReady() &&
               sl_constants_frame == scope.frame && sl_constants_view == scope.viewport;
    }
    void SetReplaceSR(bool value) {
        std::lock_guard lock(mutex);
        if (replace_sr == value)
            return;
        replace_sr = value;
        replace_paused = false;
        replace_refusals = 0;
        sl_route_failures = 0;
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
            Failure("RR enable generation overflow");
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
        // At least the version RR was built with, so the copy reads only the game's own fields;
        // the copy then says that version, so Streamline reads no further either.
        if (c.structType == sl::Constants::s_structType && c.structVersion >= sl::kStructVersion2) {
            sl_constants = c;
            sl_constants.next = nullptr;
            sl_constants.structVersion = sl::kStructVersion2;
            sl_constants_frame = frame;
            sl_constants_view = viewport;
        }
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
        if (!enabled || failed || retired)
            return false;
        bool dispatched = false;
        try {
            PollLocked();
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
                if (fresh)
                    memory_logged = false;
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
    bool RawWasCorrected(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(mutex);
        for (auto &s : slots)
            if (RawSlot(s, cl) && s.raw_corrected)
                return true;
        return false;
    }
    bool RawSpatialPassthrough(ID3D12GraphicsCommandList *cl) {
        std::lock_guard lock(mutex);
        return RawWasCorrected(cl);
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
        if (trail.watch[0])
            for (UINT i = 0; i < n; ++i)
                if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
                    TrailLocked(cl, b[i]);
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
        LogTrailLocked(*s, native.Get(), proof, eligible);
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
    // The game's transitions of the SR inputs and output since the previous SR call, on any
    // list, for the first few SR calls: they show the state each input really has at the call.
    void TrailLocked(ID3D12GraphicsCommandList *cl, const D3D12_RESOURCE_BARRIER &b) {
        const auto &t = b.Transition;
        for (unsigned k = 0; k < 4; ++k)
            if (trail.watch[k] && t.pResource == trail.watch[k]) {
                auto &r = trail.seen[k];
                r.entries[r.count % r.entries.size()] = {cl, t.StateBefore, t.StateAfter,
                                                         t.Subresource, b.Flags};
                ++r.count;
            }
    }
    void LogTrailLocked(const Slot &s, ID3D12GraphicsCommandList *sr_list,
                        const rrsr::StateProof &proof, bool eligible) {
        bool same = true;
        for (unsigned k = 0; k < 4; ++k)
            same &= trail.watch[k] == s.gpu.sr[k].Get();
        if (same && trail.logged < 3) {
            ++trail.logged;
            static constexpr const char *names[] = {"color", "depth", "motion", "output"};
            char line[2048] = {};
            size_t at = 0;
            const auto append = [&](const char *format, auto... values) {
                if (at < sizeof(line)) {
                    const int n = snprintf(line + at, sizeof(line) - at, format, values...);
                    at = n < 0 ? sizeof(line) : at + size_t(n);
                }
            };
            append("SR state trail %u (%s):", trail.logged, eligible ? "eligible" : "not eligible");
            for (unsigned k = 0; k < 4; ++k) {
                const auto &r = trail.seen[k];
                append(" %s %s 0x%x, %u seen:", names[k],
                       proof.known[k] ? "at SR" : "at SR unknown", unsigned(proof.states[k]),
                       r.count);
                const unsigned n =
                    r.count < r.entries.size() ? r.count : unsigned(r.entries.size());
                for (unsigned e = r.count - n; e < r.count; ++e) {
                    const auto &v = r.entries[e % r.entries.size()];
                    append(" [%s 0x%x->0x%x%s%s]",
                           v.list == sr_list         ? "srList"
                           : v.list == trail.sr_list ? "lastSrList"
                                                     : "other",
                           unsigned(v.before), unsigned(v.after),
                           v.subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ? "" : " sub",
                           v.flags == D3D12_RESOURCE_BARRIER_FLAG_NONE ? "" : " split");
                }
                append(";");
            }
            Log(line);
        }
        if (trail.logged >= 3) {
            trail.watch = {};
            return;
        }
        trail.seen = {};
        trail.sr_list = sr_list;
        for (unsigned k = 0; k < 4; ++k)
            trail.watch[k] = s.gpu.sr[k].Get();
    }
    bool AfterSR(ID3D12GraphicsCommandList *public_cl, const NVSDK_NGX_Parameter *p,
                 rrtrace::SlCall scope, NVSDK_NGX_Result result, HMODULE mod, UINT64 epoch,
                 bool safe, bool replacement = false) {

        std::lock_guard lock(mutex);
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
            if (!pipelines.announced && fg::SameNativeObject(pub.Get(), pipelines.device.Get())) {
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
                if (sr_allocates)
                    memory_logged = false;
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
                rreval::CreateParameters(*parameters, o, preset.desired);
                Require(rrpreset::Requested(*parameters, preset.desired),
                        "live RR preset parameter readback failed");
                auto created = api.create(public_cl, NVSDK_NGX_Feature_RayReconstruction,
                                          parameters, &feature);
                char line[200];
                rrngxlog::CapabilityLine(api, line, sizeof(line));
                Log(line);
                rrngxlog::Require(NVSDK_NGX_SUCCEED(created) && feature,
                                  "live RR feature creation failed",
                                  rrngxlog::Returned(created, feature));
                preset.Created();
                weapon_linear = weapon_on;
                rrngxlog::DescribeCreation(o, preset.desired, line, sizeof(line));
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
            s->gpu.GateBegin(cl.Get(), pipelines, history.Get());
            if (s->sss_used) {
                s->gpu.sss.Resolve(cl.Get(), pipelines, s->gpu.header.Get(), s->gpu.stats.Get(),
                                   history.Get());
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
                // Replacing SR, RR goes through the game's Streamline DLSS-D like a game with
                // built-in RR, so add-ons beneath RR that need the game's DLSS call still see one.
                bool routed = false;
                sl::DLSSDOptions sl_options;
                // Only with an add-on hooked beneath RR: otherwise the direct call serves.
                // Off in this test: with its output announced, RR's direct call should serve.
                constexpr bool through_streamline = false;
                if (through_streamline && replacement && rrtrace::Probe::ForeignBeneath() &&
                    SlRoutable(scope) &&
                    rrslroute::Options(evaluated, preset.desired, s->view.data(), sl_options)) {
                    rrslroute::Tags tags;
                    rrslroute::Build(evaluated, resources, tags);
                    // Test build: DLSS5 NR associates its guides with the game's own depth, so
                    // RR's Streamline call is given that depth rather than RR's copy of it.
                    if (s->gpu.use_depth || weapon_on) {
                        tags.Replace(s->gpu.sr[1].Get(), sl::kBufferTypeDepth, states[1], o.depth_x,
                                     o.depth_y, o.width, o.height);
                        if (!sl_route_logged)
                            Log("Test: RR's Streamline call is tagged with the game's own depth");
                    }
                    auto constants = sl_constants;
                    // RR's two features keep separate histories: switching between them resets.
                    constants.reset = reset || !sl_last_routed || sl_history_lost
                                          ? sl::Boolean::eTrue
                                          : sl::Boolean::eFalse;
                    routed = true;
                    const auto viewport = rrslroute::Viewport(scope.viewport);
                    sl_used_view = viewport;
                    // The list the game handed Streamline, which Streamline expects back.
                    const auto r = rrtrace::Probe::SlEvaluateRR(
                        *scope.token, viewport, constants, sl_options, tags.tags.data(), tags.count,
                        scope.command ? scope.command
                                      : reinterpret_cast<sl::CommandBuffer *>(public_cl));
                    // A refused Streamline call falls back to the direct call for this frame, so
                    // it never retires RR.
                    // Out of VRAM is a warning Streamline returns after recording the evaluate.
                    if (r == sl::Result::eWarnOutOfVRAM && !sl_vram_logged) {
                        sl_vram_logged = true;
                        Log("RR through Streamline DLSS-D: recorded with an out-of-VRAM warning");
                    }
                    if (r != sl::Result::eOk && r != sl::Result::eWarnOutOfVRAM) {
                        routed = false;
                        char line[160];
                        snprintf(line, sizeof(line),
                                 "RR through Streamline DLSS-D failed (result %d)%s", int(r),
                                 ++sl_route_failures == 3 ? "; RR evaluates directly from now on"
                                                          : "");
                        Log(line);
                    } else {
                        evaluated_result = NVSDK_NGX_Result_Success;
                        sl_route_failures = 0;
                        if (!sl_route_logged) {
                            sl_route_logged = true;
                            char line[160];
                            snprintf(line, sizeof(line),
                                     "RR evaluates through Streamline DLSS-D on viewport 0x%x "
                                     "(%u tags)",
                                     viewport, tags.count);
                            Log(line);
                        }
                    }
                }
                if (!routed) {
                    if (sl_last_routed || sl_history_lost)
                        parameters->Set(NVSDK_NGX_Parameter_Reset, 1);
                    evaluated_result = api.evaluate(public_cl, feature, parameters, nullptr);
                }
                sl_last_routed = routed;
                sl_history_lost = false;
            } catch (...) {
                sl_history_lost = true;
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
            glass_anchor(pre), cover(pre);
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
            if (rrsss::Present(pre[k]) || rrsss::Present(producer[k]) ||
                rrsss::Present(consumer[k]) || rrsss::Present(glass_pre[k]) ||
                rrsss::Present(glass_anchor[k]) || rrsss::Present(cover[k])) {
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
            if (!history_queued) {
                rrinternal::Scope guard;
                ID3D12CommandList *setup = init_list.Get();
                {
                    q->ExecuteCommandLists(1, &setup);
                }
                history_queued = true;
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
                s.transparency_accepted =
                    s.transparency_used && s.transparency.written && s.transparency.anchored &&
                    (s.transparency.producer_copy
                         ? rrtrans::AdmitProducer(a, glass_pre[k], glass_anchor[k], b,
                                                  s.transparency.prep_submitted,
                                                  s.transparency.anchor_submitted, baseline)
                     : s.transparency.at_draw
                         ? rrtrans::AdmitAtDraw(a, glass_pre[k], glass_anchor[k], b,
                                                s.transparency.prep_submitted,
                                                s.transparency.anchor_submitted, baseline)
                         : rrtrans::Admit(a, glass_pre[k], glass_anchor[k], b,
                                          s.transparency.prep_submitted,
                                          s.transparency.anchor_submitted, baseline));
                if (s.transparency_used && s.transparency.producer_copy &&
                    !s.transparency_accepted && !transparency_producer_suspended)
                    SuspendTransparencyProducer(nullptr);
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
        for (const auto &s : slots)
            if (s.poisoned || s.pre_live || s.sr_live || s.sss.live || s.transparency.Live() ||
                s.coverage.Live() || s.pending_sr) {
                Log("RR shutdown deferred: an RR command recording remains replayable; "
                    "reset/dispose required");
                return false;
            }
        rrprivate::FencePoint point{fence, signaled};
        if (!point.Wait(30000)) {
            Failure("RR shutdown deferred: live GPU inference unfinished");
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
