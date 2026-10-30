#define NOMINMAX
#include "../src/rr_live.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

// CPU-only COM double. No device, adapter, queue, or GPU is created.
struct CpuFence final : ID3D12Fence {
    ULONG refs = 1;
    UINT64 completed{};
    unsigned queries{}, waits{}, signals{};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **) override {
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto n = --refs;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void **) override {
        return E_NOTIMPL;
    }
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override {
        ++queries;
        return completed;
    }
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64, HANDLE) override {
        ++waits;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Signal(UINT64) override {
        ++signals;
        return E_NOTIMPL;
    }
};
// CPU-only resource double: its reference count is observed; Map hands out mapped, when set,
// and records the range read.
struct CpuResource final : ID3D12Resource {
    ULONG refs = 1;
    void *mapped{};
    D3D12_RANGE read{};
    D3D12_RESOURCE_DESC description{};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        *out = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(ID3D12Resource))
            return E_NOINTERFACE;
        *out = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto n = --refs;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void **) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Map(UINT, const D3D12_RANGE *range, void **data) override {
        if (!mapped)
            return E_NOTIMPL;
        read = range ? *range : D3D12_RANGE{};
        *data = mapped;
        return S_OK;
    }
    void STDMETHODCALLTYPE Unmap(UINT, const D3D12_RANGE *) override {}
    unsigned descs{}; // GetDesc calls
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override {
        ++descs;
        return description;
    }
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override {
        return 0;
    }
    HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT, const D3D12_BOX *, const void *, UINT,
                                                 UINT) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(void *, UINT, UINT, UINT,
                                                  const D3D12_BOX *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES *,
                                                D3D12_HEAP_FLAGS *) override {
        return E_NOTIMPL;
    }
};
// Narrow ABI doubles for native FG recording and submission. All unused methods trap;
// no D3D12 device, queue or command list is created. As in release-hooks-cpu.cpp,
// calls go through the SDK interfaces and use their real COM vtable layout.
struct FgCpuObject {
    void **vtable{};
    const IID *iid{};
    ULONG refs = 1;
    FgCpuObject *device{};
    D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    UINT barriers{}, bad_barriers{}, signals{}, waits{};
    UINT64 signaled{}, waited{};
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    void *table[64]{};
    explicit FgCpuObject(const IID &id) : vtable(table), iid(&id) {
        for (auto &entry : table)
            entry = reinterpret_cast<void *>(&Trap);
        table[0] = reinterpret_cast<void *>(&Query);
        table[1] = reinterpret_cast<void *>(&AddRef);
        table[2] = reinterpret_cast<void *>(&Release);
        table[7] = reinterpret_cast<void *>(&GetDevice);
        if (id == __uuidof(ID3D12GraphicsCommandList)) {
            table[8] = reinterpret_cast<void *>(&GetType);
            table[26] = reinterpret_cast<void *>(&Barrier);
        } else if (id == __uuidof(ID3D12CommandQueue)) {
            table[14] = reinterpret_cast<void *>(&Signal);
            table[15] = reinterpret_cast<void *>(&Wait);
            table[18] = reinterpret_cast<void *>(&QueueDesc);
        }
    }
    template <class T> T *As() { return reinterpret_cast<T *>(this); }
    static void Trap() { std::abort(); }
    static HRESULT STDMETHODCALLTYPE Query(FgCpuObject *s, REFIID id, void **out) {
        *out = nullptr;
        if (id != __uuidof(IUnknown) && id != *s->iid)
            return E_NOINTERFACE;
        *out = s;
        ++s->refs;
        return S_OK;
    }
    static ULONG STDMETHODCALLTYPE AddRef(FgCpuObject *s) { return ++s->refs; }
    static ULONG STDMETHODCALLTYPE Release(FgCpuObject *s) { return --s->refs; }
    static HRESULT STDMETHODCALLTYPE GetDevice(FgCpuObject *s, REFIID id, void **out) {
        return Query(s->device, id, out);
    }
    static D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType(FgCpuObject *s) { return s->type; }
    static void STDMETHODCALLTYPE Barrier(FgCpuObject *s, UINT n,
                                           const D3D12_RESOURCE_BARRIER *barriers) {
        for (UINT i = 0; i < n; ++i) {
            assert(barriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION);
            const auto &b = barriers[i].Transition;
            ++s->barriers;
            s->bad_barriers += b.StateBefore != s->state;
            s->state = b.StateAfter;
        }
    }
    static HRESULT STDMETHODCALLTYPE Signal(FgCpuObject *s, ID3D12Fence *, UINT64 value) {
        ++s->signals;
        s->signaled = value;
        return S_OK;
    }
    static HRESULT STDMETHODCALLTYPE Wait(FgCpuObject *s, ID3D12Fence *, UINT64 value) {
        ++s->waits;
        s->waited = value;
        return S_OK;
    }
    // The x64 COM method returns this struct through its second (hidden) argument.
    static D3D12_COMMAND_QUEUE_DESC *STDMETHODCALLTYPE
    QueueDesc(FgCpuObject *s, D3D12_COMMAND_QUEUE_DESC *out) {
        *out = {s->type};
        return out;
    }
};
static unsigned releases{}, destroys{};
static NVSDK_NGX_Result NVSDK_CONV ReleaseFeature(NVSDK_NGX_Handle *) {
    ++releases;
    return NVSDK_NGX_Result_Success;
}
static NVSDK_NGX_Result NVSDK_CONV DestroyParameters(NVSDK_NGX_Parameter *) {
    ++destroys;
    return NVSDK_NGX_Result_Success;
}
static std::string logged;
static void Logged(const char *s) {
    logged += s;
    logged += '\n';
}
static NVSDK_NGX_Result NVSDK_CONV FeatureNotFound(NVSDK_NGX_Handle *) {
    return NVSDK_NGX_Result(0xBAD00004u);
}
static NVSDK_NGX_Result NVSDK_CONV PlatformError(NVSDK_NGX_Parameter *) {
    return NVSDK_NGX_Result(0xBAD00002u);
}

namespace rrlive {
struct ControllerCpuTest {
    static void DevTests() {
        // An off or blocked shared test texture never touches the device.
        Controller c;
        Controller::DevTest t;
        assert(!c.UseTest(t, false, 0, 8, 8, DXGI_FORMAT_R16_FLOAT, "test") && !t.active);
        t.blocked = true;
        t.changes = 3;
        assert(!c.UseTest(t, true, 3, 8, 8, DXGI_FORMAT_R16_FLOAT, "test") && t.blocked &&
               !t.active);
        // Reconfigure gives back only the reserve the test textures added.
        c.budget.limit = Budget::Minimum + 300;
        c.test_reserve = 200;
        c.weapon_test.blocked = c.weapon_test.active = true;
        c.QueueReconfigure();
        assert(c.Reconfigure());
        assert(c.budget.limit == Budget::Minimum + 100 && !c.test_reserve);
        assert(!c.weapon_test.blocked && !c.weapon_test.active);
        c.budget.limit = Budget::Minimum + 50;
        c.test_reserve = 200;
        c.QueueReconfigure();
        assert(c.Reconfigure() && c.budget.limit == Budget::Minimum);
    }
    // A failed retirement keeps its allocations and logs the NGX result, or the missing export.
    static void RetirementFailure() {
        Controller f;
        f.logger = &Logged;
        f.feature = reinterpret_cast<NVSDK_NGX_Handle *>(1);
        f.parameters = reinterpret_cast<NVSDK_NGX_Parameter *>(2);
        f.api.release = FeatureNotFound;
        f.QueueReconfigure();
        assert(!f.Reconfigure() && f.failed && f.feature && f.parameters);
        assert(logged == "RR feature retirement failed (0xBAD00004 FAIL_FeatureNotFound); "
                         "allocations retained\n");
        logged.clear();
        Controller p;
        p.logger = &Logged;
        p.parameters = reinterpret_cast<NVSDK_NGX_Parameter *>(2);
        p.api.destroy = PlatformError;
        p.QueueReconfigure();
        assert(!p.Reconfigure() && p.failed && p.parameters);
        assert(logged == "RR parameter retirement failed (0xBAD00002 FAIL_PlatformError); "
                         "allocations retained\n");
        logged.clear();
        Controller m;
        m.logger = &Logged;
        m.feature = reinterpret_cast<NVSDK_NGX_Handle *>(1);
        m.QueueReconfigure();
        assert(!m.Reconfigure() && m.failed && m.feature);
        assert(logged == "RR feature retirement failed (export missing); allocations retained\n");
        logged.clear();
    }
    // An error turns RR off for a pause, then RR restarts through Reconfigure; a poisoned slot
    // or a failed retirement keeps it off. Optional guides come back after their own pause.
    static void FailureRecovers() {
        Controller c;
        c.logger = &Logged;
        c.Failure("boom");
        assert(c.failed && !c.fatal && c.rr_pause.at);
        assert(logged == "boom; RR restarts in 5 s\n");
        logged.clear();
        assert(!c.Recover() && c.failed); // the pause is not over
        c.rr_pause.at = 1;
        assert(c.Recover() && !c.failed && !c.layout_pending && c.rr_pause.repeats == 1);
        assert(logged == "RR restarted after an error\n");
        logged.clear();
        c.Failure("again");
        assert(logged == "again; RR restarts in 10 s\n");
        logged.clear();
        Controller p;
        p.logger = &Logged;
        p.slots.emplace_back();
        p.slots.back().poisoned = true;
        p.Failure("lost");
        assert(p.fatal && logged == "lost\n");
        logged.clear();
        p.rr_pause.at = 1;
        assert(!p.Recover() && p.failed);
        Controller g;
        g.logger = &Logged;
        g.sss_failed = true;
        g.Resume(g.sss_failed, g.sss_pause, 100000, "Subsurface Scattering guide");
        assert(g.sss_failed && g.sss_pause.at == 105000);
        g.Resume(g.sss_failed, g.sss_pause, 104999, "Subsurface Scattering guide");
        assert(g.sss_failed && logged.empty());
        g.Resume(g.sss_failed, g.sss_pause, 105000, "Subsurface Scattering guide");
        assert(!g.sss_failed && !g.sss_pause.at && g.sss_pause.repeats == 1);
        assert(logged == "Subsurface Scattering guide: retrying after an error\n");
        logged.clear();
    }
    // Each stats reader maps only its own bytes: the gate (0..15) and the non-finite counts
    // (256, 260), inside the 512-byte readback.
    static void StatsReaders() {
        static_assert(GpuFrame::StatsBytes == 512 && GpuFrame::FiniteOffset == 256);
        UINT words[GpuFrame::StatsBytes / 4]; // the whole readback
        for (UINT i = 0; i < GpuFrame::StatsBytes / 4; ++i)
            words[i] = 100 + i; // word i holds 100 + its byte offset / 4
        GpuFrame g;
        auto *readback = new CpuResource;
        readback->mapped = words;
        g.readback.Attach(readback);
        assert((g.ReadStats() == std::array<UINT, 4>{100, 101, 102, 103}));
        assert(readback->read.Begin == 0 && readback->read.End == 16);
        assert((g.ReadFinite() == std::array<UINT, 2>{164, 165}));
        assert(readback->read.Begin == 256 && readback->read.End == 264);
    }
    // A creation attempt's runtime is logged once, by the next Poll, with the controller mutex
    // released: its module and DXGI queries never run under it.
    static inline Controller *polled{};
    static inline bool unlocked{};
    static void LogUnlocked(const char *s) {
        Logged(s);
        // From another thread: the mutex is recursive, so this one would always get it.
        std::thread([] {
            if (polled->mutex.try_lock()) {
                polled->mutex.unlock();
                unlocked = true;
            }
        }).join();
    }
    static void RuntimeLog() {
        Controller c;
        polled = &c;
        c.logger = &LogUnlocked;
        c.runtime_report = {GetModuleHandleW(L"kernel32.dll"), nullptr};
        c.Poll();
        assert(logged.rfind("RR runtime: NGX core ", 0) == 0 && unlocked);
        assert(std::count(logged.begin(), logged.end(), '\n') == 1);
        c.Poll(); // Drained: the next Poll logs nothing.
        assert(std::count(logged.begin(), logged.end(), '\n') == 1);
        logged.clear();
        polled = nullptr;
    }
    static void WeaponDepth() {
        // Weapon depth restarts RR only when its setting differs from the depth type RR was
        // created with. Linear depth that could not run stays off through Reconfigure until
        // the setting next changes, so it restarts RR once, never over and over.
        using State = Controller::WeaponState;
        auto *const created = reinterpret_cast<NVSDK_NGX_Handle *>(1);
        Controller c;
        rrweapondepth::enabled = true;
        ++rrweapondepth::changes;
        assert(c.WeaponDepthWanted() && !c.NeedsReconfigure()); // The next RR takes it.
        assert(c.WeaponDepthState() == State::Waiting);
        c.enabled = true;
        c.feature = created; // Created with hardware depth.
        assert(c.NeedsReconfigure() && c.WeaponDepthState() == State::Restarting);
        c.weapon_linear = true;
        assert(!c.NeedsReconfigure() && c.WeaponDepthState() == State::Waiting);
        c.weapon_test.active = true;
        assert(c.WeaponDepthState() == State::Active);
        // The refusal names NGX's outcome.
        c.logger = Logged;
        const rrngxlog::Code refused(0xBAD00005u);
        c.HoldWeaponDepth(Controller::WeaponHold::Refused, rrweapondepth::changes, refused.text);
        assert(!c.WeaponDepthWanted() && c.NeedsReconfigure());
        assert(c.WeaponDepthState() == State::Refused);
        const std::string refusal = "Weapon depth: RR failed with the weapon depth copy (0xBAD00005 "
                                    "FAIL_InvalidParameter); off until the setting changes, RR "
                                    "runs without it\n";
        assert(logged == refusal);
        c.HoldWeaponDepth(Controller::WeaponHold::Refused, rrweapondepth::changes, "threw");
        assert(logged == refusal); // Frames still in flight repeat the hold; logged once.
        logged.clear();
        c.logger = nullptr;
        c.feature = nullptr; // Released by Reconfigure in the game; no NGX here.
        c.QueueReconfigure();
        assert(c.Reconfigure());
        assert(!c.weapon_linear && !c.weapon_test.active && !c.WeaponDepthWanted());
        c.feature = created; // Recreated with hardware depth: no second restart.
        assert(!c.NeedsReconfigure() && c.WeaponDepthState() == State::Refused);
        ++rrweapondepth::changes; // Turned off and on again: tried again.
        assert(c.WeaponDepthWanted() && c.NeedsReconfigure());
        c.HoldWeaponDepth(Controller::WeaponHold::Memory, rrweapondepth::changes);
        assert(c.WeaponDepthState() == State::NoMemory && !c.NeedsReconfigure());
        c.HoldWeaponDepth(Controller::WeaponHold::Projection, rrweapondepth::changes);
        assert(c.WeaponDepthState() == State::Unsupported);
        rrweapondepth::enabled = false;
        ++rrweapondepth::changes;
        assert(!c.NeedsReconfigure() && c.WeaponDepthState() == State::Off);
        c.weapon_linear = true; // Turned off while RR has linear depth: back to hardware depth.
        assert(c.NeedsReconfigure() && c.WeaponDepthState() == State::Restarting);
        c.weapon_linear = false;
        c.feature = nullptr;
    }
    // Regression: a second tag on one open recording must not claim COMMON again.
    // Streamline restores the declared NPSR state after each volatile depth copy.
    static void FgRepeatedTags() {
        FgCpuObject list(__uuidof(ID3D12GraphicsCommandList));
        Microsoft::WRL::ComPtr<CpuResource> depth, replacement;
        depth.Attach(new CpuResource);
        replacement.Attach(new CpuResource);
        depth->description = replacement->description =
            TextureDesc(8, 4, DXGI_FORMAT_R32_FLOAT);
        Controller c;
        c.enabled = true;
        auto &s = c.slots[0];
        s.busy = s.recorded = s.sealed = s.fg_prepared = true;
        s.frame = 41;
        s.viewport = 0;
        s.gpu.sr[1] = depth.Get();
        s.gpu.fg_depth = replacement.Get();
        rrfgdepth::enabled = true;
        rrfgdepth::observe = false;
        rrtrace::FgTag tag;
        tag.frame = 41;
        tag.viewport = 0;
        tag.command = list.As<sl::CommandBuffer>();
        tag.native = depth.Get();
        tag.lifecycle = rrfgdepth::OnlyValidNow;
        sl::Resource out(sl::ResourceType::eTex2d, nullptr);
        assert(c.FgTag(tag, out) && out.native == replacement.Get());
        assert(list.barriers == 1 && list.bad_barriers == 0);
        assert(list.state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        assert(c.FgTag(tag, out) && out.native == replacement.Get());
        assert(list.bad_barriers == 0 && "repeat tag must preserve the recording's NPSR state");
        assert(list.barriers == 1);
    }
    // Regression: an FG recording can execute again before Reset. Every execution
    // must advance its completion fence, including on a different queue; Reset
    // retires the recording but must still wait for the final execution to finish.
    static void FgRecordingLifetime() {
        FgCpuObject device(__uuidof(ID3D12Device));
        FgCpuObject list(__uuidof(ID3D12GraphicsCommandList));
        FgCpuObject first(__uuidof(ID3D12CommandQueue)), second(__uuidof(ID3D12CommandQueue));
        first.device = second.device = &device;
        Microsoft::WRL::ComPtr<CpuFence> f;
        f.Attach(new CpuFence);
        Controller c;
        c.pipelines.device = device.As<ID3D12Device>();
        c.fence = f;
        auto &s = c.slots[0];
        s.busy = s.fg_list_live = true;
        s.fg_list = list.As<ID3D12GraphicsCommandList>();
        s.gpu.fg_depth.Attach(new CpuResource);
        // Keep the texture cached while the slot is idle, until reconfiguration.
        rrfgdepth::enabled = true;
        rrfgdepth::status.fg_seen_at = GetTickCount64();
        ID3D12CommandList *lists[] = {list.As<ID3D12CommandList>()};
        c.Before(first.As<ID3D12CommandQueue>(), 1, lists);
        c.After(first.As<ID3D12CommandQueue>());
        assert(first.signaled == 1 && s.fence_value == 1);
        f->completed = 1;
        c.PollLocked();
        assert(s.busy && s.gpu.fg_depth && "a live FG recording still owns its texture");
        c.QueueReconfigure();
        assert(!c.Reconfigure());
        c.Before(second.As<ID3D12CommandQueue>(), 1, lists);
        c.After(second.As<ID3D12CommandQueue>());
        assert(second.waits == 1 && second.waited == 1);
        assert(second.signaled == 2 && s.fence_value == 2);
        c.Reset(list.As<ID3D12GraphicsCommandList>());
        assert(s.busy && !s.fg_list_live && !c.Reconfigure());
        f->completed = 2;
        c.PollLocked();
        assert(!s.busy && s.gpu.fg_depth);
        assert(c.Reconfigure() && !s.gpu.fg_depth);
        // A discarded recording has no GPU submission and must retire on Reset.
        Controller discarded;
        auto &d = discarded.slots[0];
        d.busy = d.fg_list_live = true;
        d.fg_list = list.As<ID3D12GraphicsCommandList>();
        discarded.Reset(list.As<ID3D12GraphicsCommandList>());
        assert(!d.busy && !d.fg_list_live);
    }
    static void Run() {
        FgRepeatedTags();
        FgRecordingLifetime();
        DevTests();
        RetirementFailure();
        FailureRecovers();
        StatsReaders();
        RuntimeLog();
        WeaponDepth();
        // A rejected allocation after preset retirement must wait, then recover.
        // Ready guide slots must remain usable while another slot waits.
        bool deferred = false;
        try { rrprivate::CheckAllocation(E_OUTOFMEMORY, "allocation"); }
        catch (const rrprivate::AllocationDeferred &) { deferred = true; }
        assert(deferred);
        bool removed = false;
        try { rrprivate::CheckAllocation(DXGI_ERROR_DEVICE_REMOVED, "removed"); }
        catch (const rrprivate::AllocationDeferred &) { assert(false); }
        catch (const std::runtime_error &) { removed = true; }
        assert(removed);
        rrprivate::CheckAllocation(S_OK, "successful allocation");
        Controller retry;
        retry.preset.Created();
        assert(retry.RequestPreset(4) && retry.Reconfigure());
        retry.preset.Created();
        assert(retry.PresetStatus().effective == 4);
        unsigned attempts = 0;
        auto reject = [&] { ++attempts; throw rrprivate::AllocationDeferred(); };
        bool escaped = false;
        try {
            assert(!retry.PrepareSssAllocation(1000, false, reject));
        } catch (const rrprivate::AllocationDeferred &) { escaped = true; }
        assert(!escaped && "temporary allocation rejection must not reach the fatal SSS catch");
        assert(!retry.sss_failed && attempts == 1);
        assert(!retry.PrepareSssAllocation(1100, false, reject) && attempts == 1);
        assert(retry.PrepareSssAllocation(1100, true, [&] { ++attempts; }));
        assert(attempts == 2);
        assert(!retry.PrepareSssAllocation(1999, false, reject) && attempts == 2);
        assert(retry.PrepareSssAllocation(2000, false, [&] { ++attempts; }));
        assert(attempts == 3 && !retry.sss_failed);
        bool fatal = false;
        try {
            retry.PrepareSssAllocation(2100, false, [] {
                throw std::runtime_error("invalid resource layout");
            });
        } catch (const std::runtime_error &) { fatal = true; }
        assert(fatal); // Genuine failures retain the existing containment path.
        Controller startup;
        assert(startup.RequestPreset(4) && !startup.NeedsReconfigure());
        assert(!startup.PresetStatus().has_effective && !startup.PresetStatus().pending);
        startup.preset.Retired();
        assert(startup.PresetStatus().pending &&
               !startup.NeedsReconfigure()); // Await creation without blocking producers.
        assert(startup.RequestPreset(0) && startup.Reconfigure());
        // Running water tests: settings are clamped, and the panel only reports
        // a mask the latest evaluation sent that would still be sent now.
        {
            // The default responsivity mask classifies water on preset F only.
            Controller f;
            assert(f.WaterWanted() && f.WaterStatus().wanted);
            assert(f.RequestPreset(5) && !f.WaterWanted() && !f.WaterStatus().wanted);
        }
        Controller water;
        rrwater::Settings wet;
        wet.responsivity = true;
        wet.strength = 3.0f;
        water.SetWater(wet);
        assert(water.water.strength == 1.0f);
        auto wet_status = water.WaterStatus();
        assert(wet_status.wanted && !wet_status.ready && !wet_status.unavailable &&
               !wet_status.responsivity);
        water.water_status.ready = water.water_status.responsivity = true;
        assert(!water.WaterStatus().ready); // RR is off
        water.SetEnabled(true);
        assert(water.WaterStatus().ready && !water.WaterStatus().responsivity); // no preset yet
        water.preset.Created();
        wet_status = water.WaterStatus();
        assert(wet_status.ready && wet_status.responsivity);
        // Water storage waits for a first SR allocation and for the retry
        // window; a shortage then waits again without failing the tests.
        GpuFrame dry;
        assert(!water.PrepareWater(dry, 1000) && water.water_retry_after == 0);
        dry.rr_output.Attach(new CpuResource);
        water.water_retry_after = 2000;
        assert(water.WaterStatus().waiting_memory);
        assert(!water.PrepareWater(dry, 1999) && water.water_retry_after == 2000);
        assert(!water.PrepareWater(dry, 2000) && water.water_retry_after == 3000); // no device
        assert(!water.water_failed && water.WaterStatus().waiting_memory);
        water.SetEnabled(false);
        assert(!water.WaterStatus().waiting_memory); // nothing retries while RR is off
        water.SetEnabled(true);
        rrwater::Settings dry_settings;
        water.SetWater(dry_settings); // the default responsivity mask still wants water on F
        assert(water.water_retry_after == 3000);
        dry_settings.responsivity = false;
        water.SetWater(dry_settings);
        assert(water.water_retry_after == 0);
        water.SetWater(wet);
        water.water_failed = true;
        wet_status = water.WaterStatus();
        assert(wet_status.unavailable && !wet_status.ready);
        Controller c;
        c.SetEnabled(true);
        c.SetSssEnabled(true);
        c.SetTransparencyEnabled(true);
        assert(c.Enabled() && c.SssEnabled() && c.TransparencyEnabled());
        c.preset.Created();
        c.feature = reinterpret_cast<NVSDK_NGX_Handle *>(1);
        c.parameters = reinterpret_cast<NVSDK_NGX_Parameter *>(2);
        c.api.release = ReleaseFeature;
        c.api.destroy = DestroyParameters;
        auto *f = new CpuFence;
        c.fence.Attach(f);
        c.signaled = 9;
        auto &s = c.slots[0];
        s.busy = true;
        s.fence_value = 9;
        assert(!c.RequestPreset(1) && c.PresetStatus().desired == 6);
        assert(c.RequestPreset(4) && c.PresetStatus().pending);
        assert(c.RequestPreset(5) && c.PresetStatus().desired == 5 &&
               c.PresetStatus().effective == 6);
        const auto before_queries = f->queries;
        c.SetEnabled(false);
        c.SetEnabled(true);
        c.SetSssEnabled(false);
        c.SetTransparencyEnabled(false);
        c.Options();
        c.Status();
        c.PresetStatus();
        // The transparency albedo blend row with no driver or device: RR is on and
        // nothing ran yet. Toggling it restarts history.
        assert(std::string(rrstatus::Label(c.TransparencyBlendState(true, false))) ==
               "ON / waiting");
        assert(std::string(rrstatus::Label(c.TransparencyBlendState(false, false))) ==
               "unavailable");
        c.policy.Next(20, c.generation, false);
        assert(!c.policy.Next(21, c.generation, false));
        c.SetTransparencyBlendEnabled(false);
        assert(!c.TransparencyBlendEnabled() &&
               std::string(rrstatus::Label(c.TransparencyBlendState(true, false))) == "OFF");
        assert(c.policy.Next(22, c.generation, false));
        c.SetTransparencyBlendEnabled(true);
        c.SetTransparencyBlendTuning({});
        assert(c.TransparencyBlendEnabled() && c.policy.Next(23, c.generation, false));
        assert(f->queries == before_queries && releases == 0 &&
               destroys == 0); // UI never calls driver.
        bool *held[] = {&s.pre_live,
                        &s.sr_live,
                        &s.sss.live,
                        &s.transparency.prep_live,
                        &s.transparency.anchor_live,
                        &s.coverage.live,
                        &s.pending_sr,
                        &s.poisoned};
        f->completed = 9;
        for (auto flag : held) {
            *flag = true;
            assert(!c.Reconfigure() && releases == 0);
            *flag = false;
        }
        f->completed = 8;
        assert(!c.Reconfigure() && releases == 0);
        f->completed = UINT64_MAX;
        assert(!c.Reconfigure() && releases == 0); // Device removal retains allocations.
        f->completed = 9;
        f->AddRef();
        assert(c.Reconfigure() && releases == 1 && destroys == 1);
        assert(!c.feature && !c.parameters && !c.PresetStatus().has_effective &&
               c.PresetStatus().pending);
        assert(f->waits == 0 && f->signals == 0);
        f->Release();
        assert(c.Reconfigure() &&
               releases == 1); // No duplicate release while awaiting next creation.
        c.preset.Created();
        assert(c.PresetStatus().effective == 5 && !c.PresetStatus().pending);
        // New observed dimensions/mode queue the same safe retirement path.
        c.key.width = 1280;
        c.key.height = 720;
        c.key.out_width = 2560;
        c.key.out_height = 1440;
        c.key.flags = 0;
        c.key.quality = 1;
        auto actual = c.key;
        actual.width = 1720;
        actual.height = 720;
        actual.out_width = 3440;
        assert(!rreval::SameCreation(c.key, actual));
        actual = c.key;
        actual.quality = 2;
        assert(!rreval::SameCreation(c.key, actual));
        actual = c.key;
        actual.jitter_x = 0.25f;
        assert(rreval::SameCreation(c.key, actual));
        c.QueueReconfigure();
        assert(c.PresetStatus().pending && c.Reconfigure());
        assert(!c.initialized && c.selected_view == UINT_MAX && !c.layout_pending);

        // Only an admitted SR submission hands its SSS guide to masked lighting,
        // for the next MaxAge frames of the same generation and extent. A rejected
        // resolve of that guide, an SSS toggle or a rebuild releases it.
        auto *guide = new CpuResource;
        {
            Controller masked;
            masked.SetEnabled(true);
            masked.SetSssEnabled(true);
            auto &producer = masked.slots[1];
            producer.frame = 10;
            producer.generation = masked.generation;
            producer.gpu.sss.guide = guide;
            auto &ms = masked.slots[0];
            ms.generation = masked.generation;
            ms.pre_live = ms.native_sampling = true;
            ms.frame = 11;
            assert(!masked.SssMask(nullptr));
            masked.SealSssMask(producer, false);
            assert(!masked.SssMask(nullptr));
            masked.SealSssMask(producer, true);
            unsigned age{};
            assert(masked.SssMask(nullptr, &age).Get() == guide && age == 1);
            ms.frame = 10 + rrsssmask::MaxAge;
            assert(masked.SssMask(nullptr, &age).Get() == guide && age == rrsssmask::MaxAge);
            ms.frame = 11 + rrsssmask::MaxAge;
            assert(!masked.SssMask(nullptr));
            ms.frame = 10;
            assert(!masked.SssMask(nullptr));
            ms.frame = 11;
            ms.gpu.w = 8;
            assert(!masked.SssMask(nullptr));
            ms.gpu.w = 0;
            ms.pending_sr = true;
            assert(!masked.SssMask(nullptr));
            ms.pending_sr = false;
            ms.native_sampling = false;
            assert(!masked.SssMask(nullptr));
            ms.native_sampling = true;
            assert(masked.SssMask(nullptr));
            masked.SealSssMask(producer, false);
            assert(!masked.SssMask(nullptr) && !masked.sss_mask.valid && !masked.sss_mask_guide);
            masked.SealSssMask(producer, true);
            masked.SetSssEnabled(false);
            assert(!masked.SssMask(nullptr) && !masked.sss_mask_guide);
            masked.SetSssEnabled(true);
            masked.SealSssMask(producer, true);
            assert(masked.SssMask(nullptr));
            masked.SetEnabled(false);
            masked.SetEnabled(true);
            ms.generation = masked.generation;
            assert(!masked.SssMask(nullptr)); // Recorded under the previous generation.
            ms.pre_live = false;
            masked.QueueReconfigure();
            assert(masked.Reconfigure());
            assert(!masked.sss_mask.valid && !masked.sss_mask_guide);
        }
        assert(guide->refs == 1);
        guide->Release();
    }
};
} // namespace rrlive
int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "fg-tags") == 0)
        rrlive::ControllerCpuTest::FgRepeatedTags();
    else if (argc > 1 && std::strcmp(argv[1], "fg-lifetime") == 0)
        rrlive::ControllerCpuTest::FgRecordingLifetime();
    else
        rrlive::ControllerCpuTest::Run();
}
