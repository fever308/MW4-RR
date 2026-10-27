// Included inside Controller. deque growth preserves every live Slot address.
private:
bool ReservePool(size_t count, const rreval::Options *pending = nullptr,
                 const D3D12_RESOURCE_DESC *native_depth = nullptr, UINT guide_width = 0,
                 UINT guide_height = 0, UINT64 extra_persistent = 0, UINT64 pending_water = 0,
                 UINT64 depth_floor = 0) {
    UINT64 base{}, depth{}, sss{}, glass{}, output{}, water{};
    auto allocation = [&](const D3D12_RESOURCE_DESC &d) {
        return pipelines.device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
    };
    for (const auto &s : slots) {
        UINT64 b{}, d{};
        if (!SlotReservationParts(pipelines.device.Get(), s.gpu, budget.limit, b, d) ||
            b < s.gpu.sss.bytes || b - s.gpu.sss.bytes < s.gpu.transparency.bytes ||
            b - s.gpu.sss.bytes - s.gpu.transparency.bytes < s.gpu.water.bytes)
            return false;
        b -= s.gpu.sss.bytes + s.gpu.transparency.bytes + s.gpu.water.bytes;
        if (s.gpu.rr_output) {
            const auto n = allocation(s.gpu.rr_output->GetDesc());
            if (!n || n == UINT64_MAX || n > b)
                return false;
            b -= n;
            output = (std::max)(output, n);
        }
        base = (std::max)(base, b);
        depth = (std::max)(depth, d);
        sss = (std::max)(sss, s.gpu.sss.bytes);
        glass = (std::max)(glass, s.gpu.transparency.bytes);
        water = (std::max)(water, s.gpu.water.bytes);
    }
    water = (std::max)(water, pending_water);
    depth = (std::max)(depth, depth_floor);
    if (guide_width || guide_height) {
        const auto bytes =
            GpuFrame::RequiredPrepassBytes(pipelines.device.Get(), guide_width, guide_height);
        if (!bytes)
            return false;
        base = (std::max)(base, bytes);
    }
    if (pending) {
        const auto n = allocation(
            TextureDesc(pending->out_width, pending->out_height, DXGI_FORMAT_R16G16B16A16_FLOAT));
        if (!n || n == UINT64_MAX)
            return false;
        output = (std::max)(output, n);
        if (native_depth && native_depth->Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) {
            UINT64 bytes{};
            pipelines.device->GetCopyableFootprints(native_depth, 0, 1, 0, nullptr, nullptr,
                                                    nullptr, &bytes);
            auto depth_desc =
                TextureDesc(UINT(native_depth->Width), native_depth->Height, DXGI_FORMAT_R32_FLOAT);
            depth_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
            const auto texture = allocation(depth_desc);
            const auto buffer = bytes ? allocation(rrprivate::Buffer(bytes)) : 0;
            if (!texture || !buffer || texture == UINT64_MAX || buffer == UINT64_MAX ||
                texture > UINT64_MAX - buffer)
                return false;
            depth = (std::max)(depth, texture + buffer);
        }
    }
    UINT64 required{};
    if (extra_persistent > UINT64_MAX - persistent.bytes || output > UINT64_MAX - base ||
        // Water storage (rr_water.h) takes the otherwise unused hit term.
        !rrtrans::RequiredSlots(count, base + output, depth, sss, glass, water,
                                persistent.bytes + extra_persistent, required))
        return false;
    return budget.Reserve(pipelines.device.Get(), required);
}
// Slots prepared ahead of use: the frames a normal queue keeps in flight. Each slot is a set
// of guide, output and depth textures (about 200 MB at 4K Quality, 400 MB at 4K DLAA), so
// preparing the whole pool up front cost players gigabytes RR never used. Slots past this
// prepare when a deeper queue first needs them.
static constexpr size_t ReserveAhead = 3;
size_t Prepared(bool output) const {
    size_t n{};
    for (const auto &v : slots)
        n += output ? v.gpu.rr_output != nullptr : v.gpu.heap != nullptr;
    return n;
}
// The pool's other slots up to ReserveAhead, prepared while GPU memory allows (after `self`
// first prepares), so the usual frame queue later finds them ready rather than competing with
// other add-ons' memory (DLSS5 NR's, for one), which would send those frames back to native
// SR. A shortage or a failure stops it quietly: the rest prepare when first used.
void ReserveSlots(const Slot &self, const std::array<ComPtr<ID3D12Resource>, 4> &textures,
                  const std::array<ComPtr<ID3D12Resource>, 2> &cb,
                  const std::array<UINT64, 2> &offsets) {
    for (auto &v : slots) {
        if (Prepared(false) >= ReserveAhead)
            return;
        if (&v == &self || v.busy || v.gpu.heap)
            continue;
        try {
            v.gpu.Prepare(pipelines, budget, textures, cb, offsets);
            v.gpu.Retire(); // keeps no game resource until the slot is used
        } catch (const std::exception &) {
            return;
        }
    }
}
// The same for the slots' output and depth copy, after `self` first allocates its own.
void ReserveOutputs(const Slot &self, const rreval::Options &o,
                    const std::array<ComPtr<ID3D12Resource>, 4> &sr_inputs) {
    for (auto &v : slots) {
        if (Prepared(true) >= ReserveAhead)
            return;
        if (&v == &self || v.busy || !v.gpu.heap || v.gpu.rr_output)
            continue;
        try {
            v.gpu.PrepareSR(pipelines, budget, o, sr_inputs);
            v.gpu.sr = {};
        } catch (const std::exception &) {
            return;
        }
    }
}
Slot *GrowPool() {
    if (!initialized || slots.size() >= UINT_MAX || !ReservePool(slots.size() + 1))
        return nullptr;
    slots.emplace_back();
    return &slots.back();
}

public:
