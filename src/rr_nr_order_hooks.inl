#pragma once
// NR offload Gate 1: feeds the ordering probe (rr_nr_order.h) from the game's recorded and
// submitted work. Diagnostic only: off unless NrOrderProbe=1 in ReShade.ini. Every entry point
// returns at once while it is off, and none of them changes what the game records.
#include "rr_nr_order.h"
#include <algorithm>
static rrnrorder::Probe &NrOrder() {
    static auto *p = []() {
        auto *v = new rrnrorder::Probe;
        v->logger = &WriteLog;
        return v;
    }();
    return *p;
}
// Lists are named by their native object: the queue and barrier hooks see native lists,
// ReShade's events may see a wrapper.
static std::mutex nr_ids_mutex;
static std::unordered_map<uint64_t, uint64_t> nr_ids;
static uint64_t NrListId(void *list) {
    if (!list)
        return 0;
    {
        std::lock_guard lock(nr_ids_mutex);
        auto at = nr_ids.find(uint64_t(list));
        if (at != nr_ids.end())
            return at->second;
    }
    auto native = fg::NativeObject<ID3D12GraphicsCommandList>(static_cast<IUnknown *>(
        static_cast<ID3D12CommandList *>(list)));
    const uint64_t id = native ? uint64_t(native.Get()) : uint64_t(list);
    std::lock_guard lock(nr_ids_mutex);
    if (nr_ids.size() >= 16384)
        nr_ids.clear();
    nr_ids[uint64_t(list)] = id;
    return id;
}
static void NrForgetList(void *list) {
    std::lock_guard lock(nr_ids_mutex);
    nr_ids.erase(uint64_t(list));
}
// Heap slots holding a view of a watched output, kept current from descriptor writes and
// copies; rebuilt from the binding tracker whenever the watched outputs change.
static std::mutex nr_slots_mutex;
static std::vector<rrnrorder::HeapSlot> nr_slots;
static std::array<uint64_t, 4> nr_slots_for{};
static bool NrSameSlot(const rrnrorder::HeapSlot &a, rrbindings::Slot b) {
    return a.heap == b.heap && a.offset == b.offset;
}
static void NrDescriptor(rrbindings::Slot slot, const rrbindings::Value &value) {
    auto &probe = NrOrder();
    if (!probe.Armed())
        return;
    const bool watched = value.known && !value.buffer && probe.MaybeWatched(value.resource);
    std::lock_guard lock(nr_slots_mutex);
    auto at = std::find_if(nr_slots.begin(), nr_slots.end(),
                           [&](const auto &s) { return NrSameSlot(s, slot); });
    if (at != nr_slots.end())
        nr_slots.erase(at);
    if (watched && nr_slots.size() < 4096)
        nr_slots.push_back({slot.heap, slot.offset, value.resource});
}
static std::vector<rrnrorder::HeapSlot> NrSlots(rrbindings::Tracker &descriptors) {
    const auto watched = NrOrder().WatchedResources();
    std::lock_guard lock(nr_slots_mutex);
    if (watched != nr_slots_for) {
        nr_slots.clear();
        for (auto r : watched)
            if (r)
                for (auto s : descriptors.SlotsOf(r, r, 4096 - nr_slots.size()))
                    nr_slots.push_back({s.heap, s.offset, r});
        nr_slots_for = watched;
    }
    return nr_slots;
}
// Before a draw or dispatch: does a bound range hold a view of a watched output?
static void NrReads(reshade::api::command_list *list, uint64_t id, bool graphics) {
    auto *device = list->get_device();
    auto *state = rrbindings::State(device);
    if (!state || !state->tracker.Healthy())
        return;
    auto slots = NrSlots(state->tracker);
    if (slots.empty())
        return;
    auto &bindings = graphics ? state->graphics : state->tracker;
    std::vector<rrnrorder::RangeSlot> ranges;
    for (const auto &r : bindings.BoundViews(list->get_native())) {
        const auto start = rrbindings::Resolve(device, {r.table}, r.binding);
        if (start.heap)
            ranges.push_back({start.heap, start.offset, r.count});
    }
    const auto hit = rrnrorder::RangeUse(ranges, slots);
    if (hit.use != rrnrorder::Use::Count)
        NrOrder().Note(id, hit.resource, hit.use);
}
static void NrCommand(reshade::api::command_list *list, rrnrorder::Op op, bool graphics = false,
                      bool reads = false) {
    auto &probe = NrOrder();
    if (!probe.Armed() || rrinternal::active ||
        list->get_device()->get_api() != reshade::api::device_api::d3d12)
        return;
    try {
        const auto id = NrListId(reinterpret_cast<void *>(list->get_native()));
        if (reads)
            NrReads(list, id, graphics);
        probe.Command(id, op);
    } catch (...) {
    }
}
static void NrResource(reshade::api::command_list *list, reshade::api::resource resource,
                       rrnrorder::Use use) {
    auto &probe = NrOrder();
    if (!probe.MaybeWatched(resource.handle) || rrinternal::active)
        return;
    try {
        probe.Note(NrListId(reinterpret_cast<void *>(list->get_native())), resource.handle, use);
    } catch (...) {
    }
}
static void NrView(reshade::api::command_list *list, reshade::api::resource_view view,
                   rrnrorder::Use use) {
    if (!NrOrder().Armed() || !view.handle)
        return;
    try {
        NrResource(list, list->get_device()->get_resource_from_view(view), use);
    } catch (...) {
    }
}
static void NrBarriers(ID3D12GraphicsCommandList *cl, UINT n, const D3D12_RESOURCE_BARRIER *b) {
    auto &probe = NrOrder();
    if (!probe.Armed() || !b || n > 4096)
        return;
    try {
        uint64_t id = 0;
        for (UINT i = 0; i < n; ++i) {
            ID3D12Resource *r = b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION
                                    ? b[i].Transition.pResource
                                : b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_UAV ? b[i].UAV.pResource
                                                                               : nullptr;
            if (!probe.MaybeWatched(uint64_t(r)))
                continue;
            if (!id)
                id = NrListId(cl);
            if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
                probe.Note(id, uint64_t(r), rrnrorder::Use::Barrier,
                           uint32_t(b[i].Transition.StateBefore),
                           uint32_t(b[i].Transition.StateAfter));
            else
                probe.Note(id, uint64_t(r), rrnrorder::Use::Barrier,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    } catch (...) {
    }
}
static void NrSubmit(ID3D12CommandQueue *q, UINT n, ID3D12CommandList *const *lists) {
    auto &probe = NrOrder();
    if (!probe.Armed() || !q || !n || !lists || n > 4096)
        return;
    try {
        std::vector<uint64_t> ids(n);
        for (UINT i = 0; i < n; ++i)
            ids[i] = NrListId(lists[i]);
        probe.Submit(uint64_t(q), uint8_t(q->GetDesc().Type), n, ids.data());
    } catch (...) {
    }
}
static void NrProduce(ID3D12GraphicsCommandList *cl, const NVSDK_NGX_Parameter *p, bool replaced) {
    auto &probe = NrOrder();
    if (!probe.Armed())
        return;
    try {
        auto *output = static_cast<ID3D12Resource *>(rrtrace::ReadInputs(p).output);
        if (!output)
            return;
        auto native = fg::NativeObject<ID3D12Resource>(output);
        const auto call = rrtrace::Probe::CurrentCall();
        probe.Produce(NrListId(cl), uint64_t(output), uint64_t(native.Get()), call.frame,
                      call.viewport, replaced);
    } catch (...) {
    }
}
static void NrFrameGen(ID3D12GraphicsCommandList *cl) {
    if (NrOrder().Armed())
        NrOrder().FrameGen(NrListId(cl));
}
static void NrReset(ID3D12GraphicsCommandList *cl) {
    if (NrOrder().Armed())
        NrOrder().Reset(NrListId(cl));
}
static bool NrCopy(reshade::api::command_list *l, reshade::api::resource s,
                   reshade::api::resource d) {
    NrResource(l, s, rrnrorder::Use::CopySource);
    NrResource(l, d, rrnrorder::Use::CopyDest);
    NrCommand(l, rrnrorder::Op::Copy);
    return false;
}
static bool NrCopyResource(reshade::api::command_list *l, reshade::api::resource s,
                           reshade::api::resource d) {
    return NrCopy(l, s, d);
}
static bool NrCopyBuffer(reshade::api::command_list *l, reshade::api::resource s, uint64_t,
                         reshade::api::resource d, uint64_t, uint64_t) {
    return NrCopy(l, s, d);
}
static bool NrCopyBufferToTexture(reshade::api::command_list *l, reshade::api::resource s,
                                  uint64_t, uint32_t, uint32_t, reshade::api::resource d, uint32_t,
                                  const reshade::api::subresource_box *) {
    return NrCopy(l, s, d);
}
static bool NrCopyTexture(reshade::api::command_list *l, reshade::api::resource s, uint32_t,
                          const reshade::api::subresource_box *, reshade::api::resource d,
                          uint32_t, const reshade::api::subresource_box *,
                          reshade::api::filter_mode) {
    return NrCopy(l, s, d);
}
static bool NrCopyTextureToBuffer(reshade::api::command_list *l, reshade::api::resource s,
                                  uint32_t, const reshade::api::subresource_box *,
                                  reshade::api::resource d, uint64_t, uint32_t, uint32_t) {
    return NrCopy(l, s, d);
}
static bool NrResolve(reshade::api::command_list *l, reshade::api::resource s, uint32_t,
                      const reshade::api::subresource_box *, reshade::api::resource d, uint32_t,
                      uint32_t, uint32_t, uint32_t, reshade::api::format) {
    NrResource(l, s, rrnrorder::Use::CopySource);
    NrResource(l, d, rrnrorder::Use::CopyDest);
    NrCommand(l, rrnrorder::Op::Resolve);
    return false;
}
static bool NrClearRtv(reshade::api::command_list *l, reshade::api::resource_view v,
                       const float[4], uint32_t, const reshade::api::rect *) {
    NrView(l, v, rrnrorder::Use::Clear);
    NrCommand(l, rrnrorder::Op::Clear);
    return false;
}
static bool NrClearDsv(reshade::api::command_list *l, reshade::api::resource_view v, const float *,
                       const uint8_t *, uint32_t, const reshade::api::rect *) {
    NrView(l, v, rrnrorder::Use::Clear);
    NrCommand(l, rrnrorder::Op::Clear);
    return false;
}
static bool NrClearUavUint(reshade::api::command_list *l, reshade::api::resource_view v,
                           const uint32_t[4], uint32_t, const reshade::api::rect *) {
    NrView(l, v, rrnrorder::Use::Clear);
    NrCommand(l, rrnrorder::Op::Clear);
    return false;
}
static bool NrClearUavFloat(reshade::api::command_list *l, reshade::api::resource_view v,
                            const float[4], uint32_t, const reshade::api::rect *) {
    NrView(l, v, rrnrorder::Use::Clear);
    NrCommand(l, rrnrorder::Op::Clear);
    return false;
}
static bool NrMesh(reshade::api::command_list *l, uint32_t, uint32_t, uint32_t) {
    NrCommand(l, rrnrorder::Op::Draw, true, true);
    return false;
}
static bool NrRays(reshade::api::command_list *l, reshade::api::resource, uint64_t, uint64_t,
                   reshade::api::resource, uint64_t, uint64_t, uint64_t, reshade::api::resource,
                   uint64_t, uint64_t, uint64_t, reshade::api::resource, uint64_t, uint64_t,
                   uint64_t, uint32_t, uint32_t, uint32_t) {
    NrCommand(l, rrnrorder::Op::Dispatch, false, true);
    return false;
}
static void RegisterNrOrderEvents() {
    using E = reshade::addon_event;
    reshade::register_event<E::copy_resource>(&NrCopyResource);
    reshade::register_event<E::copy_buffer_region>(&NrCopyBuffer);
    reshade::register_event<E::copy_buffer_to_texture>(&NrCopyBufferToTexture);
    reshade::register_event<E::copy_texture_region>(&NrCopyTexture);
    reshade::register_event<E::copy_texture_to_buffer>(&NrCopyTextureToBuffer);
    reshade::register_event<E::resolve_texture_region>(&NrResolve);
    reshade::register_event<E::clear_render_target_view>(&NrClearRtv);
    reshade::register_event<E::clear_depth_stencil_view>(&NrClearDsv);
    reshade::register_event<E::clear_unordered_access_view_uint>(&NrClearUavUint);
    reshade::register_event<E::clear_unordered_access_view_float>(&NrClearUavFloat);
    reshade::register_event<E::dispatch_mesh>(&NrMesh);
    reshade::register_event<E::dispatch_rays>(&NrRays);
}
