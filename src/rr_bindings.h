#pragma once
#include <reshade_api_device.hpp>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <limits>
#include <memory>

namespace rrbindings {
using namespace reshade::api;
struct Slot {
    uint64_t heap{};
    uint32_t offset{};
    bool operator==(const Slot &) const = default;
};
struct SlotHash {
    size_t operator()(const Slot &s) const {
        return std::hash<uint64_t>{}(s.heap) ^ (size_t(s.offset) * 0x9e3779b9u);
    }
};
// Descriptions are copied at descriptor creation/update time. Never retain a
// resource pointer for deferred dereferencing, or infer GPU execution from this.
struct Value {
    bool known{};
    uint64_t resource{}, width{};
    uint32_t height{}, format{}, dimension{}, view_format{}, view_type{};
    uint32_t first_level{}, levels{}, first_layer{}, layers{};
    bool buffer{};
    uint64_t byte_offset{}, byte_size{};
};
struct Register {
    bool uav{};
    uint32_t index{};
};
struct Location {
    bool known{};
    uint64_t table{};
    uint32_t offset{}, parameter{};
    // The descriptor_range_flags of the range the register was found in, when the layout declared
    // them (descriptor_table_with_flags or push ranges with flags).
    uint32_t range_flags{};
    bool flags_known{};
};
struct DescriptorWatch {
    std::shared_ptr<std::atomic<bool>> stable, health;
    bool Valid() const {
        return stable && health && stable->load() && health->load();
    }
};
class Tracker {
    // A layout range; flags only when it arrived with flags (a plain descriptor_table has none).
    struct Range : descriptor_range {
        uint32_t flags{};
        bool flags_known{};
    };
    struct Param {
        std::vector<Range> ranges;
        bool root{};
    };
    struct State {
        uint64_t layout{};
        std::array<uint64_t, 64> tables{};
        std::array<Value, 64> constants{};
    };
    mutable std::mutex mutex;
    std::shared_ptr<std::atomic<bool>> health = std::make_shared<std::atomic<bool>>(true);
    std::atomic<bool> &healthy = *health;
    std::vector<std::pair<Slot, std::weak_ptr<std::atomic<bool>>>> watches;
    size_t descriptor_limit;
    const shader_stage visibility;
    std::unordered_map<Slot, Value, SlotHash> descriptors;
    std::unordered_map<uint64_t, std::vector<Param>> layouts;
    std::unordered_map<uint64_t, State> lists;
    void PutLocked(Slot s, const Value &v) {
        for (auto &watch : watches)
            if (watch.first == s)
                if (auto valid = watch.second.lock())
                    *valid = false;
        if (!s.heap) {
            healthy = false;
            return;
        }
        auto i = descriptors.find(s);
        if (!v.known) {
            if (i != descriptors.end())
                descriptors.erase(i);
            return;
        }
        if (i != descriptors.end())
            i->second = v;
        else if (descriptors.size() < descriptor_limit)
            descriptors.emplace(s, v);
        else
            healthy = false;
    }

  public:
    ~Tracker() {
        healthy = false;
    }
    explicit Tracker(size_t limit = 1048576, shader_stage stage = shader_stage::compute)
        : descriptor_limit(limit), visibility(stage) {}
    bool Healthy() const {
        return healthy.load();
    }
    void Fail() noexcept {
        healthy = false;
    }
    void Put(Slot s, const Value &value) {
        std::lock_guard lock(mutex);
        if (healthy)
            PutLocked(s, value);
    }
    Value Get(Slot s) const {
        std::lock_guard lock(mutex);
        if (!healthy)
            return {};
        auto i = descriptors.find(s);
        return i == descriptors.end() ? Value{} : i->second;
    }
    DescriptorWatch Watch(Slot s, Value &value) {
        std::lock_guard lock(mutex);
        value = {};
        if (!healthy || watches.size() >= 16)
            return {};
        auto i = descriptors.find(s);
        if (i == descriptors.end() || !i->second.known)
            return {};
        DescriptorWatch result{std::make_shared<std::atomic<bool>>(true), health};
        watches.emplace_back(s, result.stable);
        value = i->second;
        return result;
    }
    void Copy(Slot source, Slot dest, uint32_t count) {
        std::lock_guard lock(mutex);
        if (!healthy)
            return;
        if (!source.heap || !dest.heap || count > 4096 ||
            uint64_t(source.offset) + count > uint64_t(UINT32_MAX) + 1 ||
            uint64_t(dest.offset) + count > uint64_t(UINT32_MAX) + 1) {
            healthy = false;
            return;
        }
        std::vector<Value> values(count);
        for (uint32_t i = 0; i < count; ++i) {
            auto it = descriptors.find({source.heap, source.offset + i});
            if (it != descriptors.end())
                values[i] = it->second;
        }
        for (uint32_t i = 0; i < count && healthy; ++i)
            PutLocked({dest.heap, dest.offset + i}, values[i]);
    }
    void Layout(uint64_t layout, uint32_t count, const pipeline_layout_param *params) {
        std::lock_guard lock(mutex);
        if (!healthy)
            return;
        if (!layout || count > 64 || (count && !params) ||
            (!layouts.contains(layout) && layouts.size() >= 4096)) {
            healthy = false;
            return;
        }
        std::vector<Param> copy(count);
        size_t total = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const auto &p = params[i];
            uint32_t n = 0;
            if (p.type == pipeline_layout_param_type::descriptor_table)
                n = p.descriptor_table.count;
            else if (p.type == pipeline_layout_param_type::descriptor_table_with_flags)
                n = p.descriptor_table_with_flags.count;
            else if (p.type == pipeline_layout_param_type::push_descriptors) {
                copy[i].root = true;
                copy[i].ranges.push_back(Range{p.push_descriptors});
                continue;
            } else if (p.type ==
                       pipeline_layout_param_type::push_descriptors_with_ranges_and_flags) {
                copy[i].root = true;
                n = p.descriptor_table_with_flags.count;
            } else
                continue;
            if (n > 256 || total + n > 256 ||
                (n && ((p.type == pipeline_layout_param_type::descriptor_table &&
                        !p.descriptor_table.ranges) ||
                       ((p.type == pipeline_layout_param_type::descriptor_table_with_flags ||
                         copy[i].root) &&
                        !p.descriptor_table_with_flags.ranges)))) {
                healthy = false;
                return;
            }
            total += n;
            for (uint32_t j = 0; j < n; ++j)
                if (p.type == pipeline_layout_param_type::descriptor_table)
                    copy[i].ranges.push_back(Range{p.descriptor_table.ranges[j]});
                else {
                    const auto &r = p.descriptor_table_with_flags.ranges[j];
                    copy[i].ranges.push_back(
                        Range{static_cast<const descriptor_range &>(r), uint32_t(r.flags), true});
                }
        }
        layouts[layout] = std::move(copy);
    }
    void DestroyLayout(uint64_t layout) {
        std::lock_guard lock(mutex);
        layouts.erase(layout);
        for (auto &[id, s] : lists)
            if (s.layout == layout)
                s = {};
    }
    void Bind(uint64_t list, shader_stage stages, uint64_t layout, uint32_t first, uint32_t count,
              const descriptor_table *tables) {
        if (uint32_t(stages & visibility) == 0)
            return;
        std::lock_guard lock(mutex);
        if (!healthy)
            return;
        if (first > 64 || count > 64 - first || (count && !tables) ||
            (!lists.contains(list) && lists.size() >= 8192)) {
            healthy = false;
            return;
        }
        auto &s = lists[list];
        if (s.layout != layout) {
            s = {};
            s.layout = layout;
        }
        for (uint32_t i = 0; i < count; ++i)
            s.tables[first + i] = tables[i].handle;
    }
    void Reset(uint64_t list) {
        std::lock_guard lock(mutex);
        lists.erase(list);
    }
    void Push(uint64_t list, shader_stage stages, uint64_t layout, uint32_t parameter,
              const descriptor_table_update &update) {
        if (uint32_t(stages & visibility) == 0)
            return;
        std::lock_guard lock(mutex);
        if (!healthy)
            return;
        if (parameter >= 64 || (!lists.contains(list) && lists.size() >= 8192)) {
            healthy = false;
            return;
        }
        auto &s = lists[list];
        if (s.layout != layout) {
            s = {};
            s.layout = layout;
        }
        s.constants[parameter] = {};
        if (update.type != descriptor_type::constant_buffer || update.count != 1 ||
            update.binding != 0 || update.array_offset != 0 || !update.descriptors)
            return;
        const auto &b = *static_cast<const buffer_range *>(update.descriptors);
        if (!b.buffer.handle || !b.size)
            return;
        auto &v = s.constants[parameter];
        v.known = true;
        v.buffer = true;
        v.resource = b.buffer.handle;
        v.byte_offset = b.offset;
        v.byte_size = b.size;
    }
    Value Constant(uint64_t list, uint32_t reg) const {
        std::lock_guard lock(mutex);
        if (!healthy)
            return {};
        auto s = lists.find(list);
        if (s == lists.end())
            return {};
        auto layout = layouts.find(s->second.layout);
        if (layout == layouts.end())
            return {};
        Value result;
        unsigned matches = 0;
        for (uint32_t p = 0; p < layout->second.size(); ++p)
            for (const auto &r : layout->second[p].ranges) {
                if (r.type != descriptor_type::constant_buffer || r.dx_register_space ||
                    uint32_t(r.visibility & visibility) == 0 || reg < r.dx_register_index ||
                    uint64_t(reg) - r.dx_register_index >= r.count)
                    continue;
                if (++matches > 1)
                    return {};
                if (layout->second[p].root && r.count == 1)
                    result = s->second.constants[p];
            }
        return result;
    }
    Location FindConstant(uint64_t list, uint32_t reg) const {
        return FindImpl(list, {false, reg}, true);
    }
    Location FindRootConstant(uint64_t list, uint32_t reg) const {
        return FindImpl(list, {false, reg}, true, true);
    }
    Location FindRootResource(uint64_t list, Register reg) const {
        return FindImpl(list, reg, false, true);
    }
    uint64_t CurrentLayout(uint64_t list) const {
        std::lock_guard lock(mutex);
        auto s = lists.find(list);
        return healthy && s != lists.end() ? s->second.layout : 0;
    }
    Location Find(uint64_t list, Register reg) const {
        return FindImpl(list, reg, false);
    }
    // NR ordering probe (diagnostic): the SRV/UAV ranges of the list's current layout that
    // have a table bound, and the heap slots holding a view of either resource.
    struct BoundRange {
        uint64_t table{};
        uint32_t binding{}, count{};
    };
    std::vector<BoundRange> BoundViews(uint64_t list) const {
        std::vector<BoundRange> result;
        std::lock_guard lock(mutex);
        if (!healthy)
            return result;
        auto s = lists.find(list);
        if (s == lists.end())
            return result;
        auto layout = layouts.find(s->second.layout);
        if (layout == layouts.end())
            return result;
        for (uint32_t param = 0; param < layout->second.size(); ++param) {
            if (layout->second[param].root || !s->second.tables[param])
                continue;
            for (const auto &r : layout->second[param].ranges)
                if ((r.type == descriptor_type::shader_resource_view ||
                     r.type == descriptor_type::texture_shader_resource_view ||
                     r.type == descriptor_type::unordered_access_view ||
                     r.type == descriptor_type::texture_unordered_access_view) &&
                    uint32_t(r.visibility & visibility) != 0 && r.count)
                    result.push_back({s->second.tables[param], r.binding, r.count});
        }
        return result;
    }
    std::vector<Slot> SlotsOf(uint64_t a, uint64_t b, size_t limit) const {
        std::vector<Slot> result;
        std::lock_guard lock(mutex);
        for (const auto &[slot, value] : descriptors)
            if (value.known && !value.buffer && value.resource &&
                (value.resource == a || value.resource == b)) {
                if (result.size() >= limit)
                    break;
                result.push_back(slot);
            }
        return result;
    }

  private:
    Location FindImpl(uint64_t list, Register reg, bool constant, bool root_only = false) const {
        std::lock_guard lock(mutex);
        if (!healthy)
            return {};
        auto s = lists.find(list);
        if (s == lists.end())
            return {};
        auto layout = layouts.find(s->second.layout);
        if (layout == layouts.end())
            return {};
        Location result;
        unsigned matches = 0;
        for (uint32_t param = 0; param < layout->second.size(); ++param) {
            for (const auto &r : layout->second[param].ranges) {
                const bool compatible =
                    constant  ? r.type == descriptor_type::constant_buffer
                    : reg.uav ? (r.type == descriptor_type::unordered_access_view ||
                                 r.type == descriptor_type::texture_unordered_access_view ||
                                 r.type == descriptor_type::buffer_unordered_access_view)
                              : (r.type == descriptor_type::shader_resource_view ||
                                 r.type == descriptor_type::texture_shader_resource_view ||
                                 r.type == descriptor_type::buffer_shader_resource_view);
                if (!compatible || r.dx_register_space != 0 ||
                    uint32_t(r.visibility & visibility) == 0 || reg.index < r.dx_register_index)
                    continue;
                const uint64_t relative = uint64_t(reg.index) - r.dx_register_index;
                if (relative >= r.count || relative + r.binding > UINT32_MAX)
                    continue;
                if (++matches > 1)
                    return {}; // Count matching ranges, even unsupported/unbound representations.
                if (root_only) {
                    if (layout->second[param].root && r.count == 1)
                        result = {true, 0, 0, param, r.flags, r.flags_known};
                } else if (!layout->second[param].root && s->second.tables[param]) {
                    const auto offset = uint32_t(relative + r.binding);
                    result = {true, s->second.tables[param], offset, param, r.flags, r.flags_known};
                }
            }
        }
        return result;
    }
};
} // namespace rrbindings
