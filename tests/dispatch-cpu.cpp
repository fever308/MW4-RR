// Exercise the actual production dispatch callback without a device or GPU.
#include "../src/mw4_rr.cpp"
#include <cassert>
#include <cstdlib>

// Narrow ReShade ABI doubles. api_object has three virtual methods, followed by
// device::get_api or device_object::get_device. Unexpected calls abort.
struct ApiDouble {
    void **vtable;
    void *methods[4];
    uint64_t native{};
    ApiDouble *device{};
    unsigned native_reads{};
    explicit ApiDouble(bool is_device) : vtable(methods) {
        methods[0] = reinterpret_cast<void *>(&Native);
        methods[1] = reinterpret_cast<void *>(&Private);
        methods[2] = reinterpret_cast<void *>(&Trap);
        methods[3] = is_device ? reinterpret_cast<void *>(&Api)
                               : reinterpret_cast<void *>(&Device);
    }
    static void Trap() { std::abort(); }
    static uint64_t Native(ApiDouble *s) { ++s->native_reads; return s->native; }
    static void Private(ApiDouble *, const uint8_t *, uint64_t *out) { *out = 0; }
    static reshade::api::device_api Api(ApiDouble *) { return reshade::api::device_api::d3d12; }
    static reshade::api::device *Device(ApiDouble *s) {
        return reinterpret_cast<reshade::api::device *>(s->device);
    }
};
// ID3D12GraphicsCommandList ABI: IUnknown at 0..2, SetPipelineState at 25.
// All other methods trap, so dispatch must leave GPU work to the native fallback.
struct NativeDouble {
    void **vtable;
    void *methods[64];
    unsigned queries{}, refs = 1, restores{};
    bool valid{};
    reshade::api::command_list *list{};
    uint64_t restored{};
    NativeDouble() : vtable(methods) {
        for (auto &method : methods)
            method = reinterpret_cast<void *>(&ApiDouble::Trap);
        methods[0] = reinterpret_cast<void *>(&Query);
        methods[1] = reinterpret_cast<void *>(&AddRef);
        methods[2] = reinterpret_cast<void *>(&Release);
        methods[25] = reinterpret_cast<void *>(&Pipeline);
    }
    static HRESULT STDMETHODCALLTYPE Query(NativeDouble *s, REFIID id, void **out) {
        ++s->queries;
        *out = nullptr;
        if (!s->valid || (id != __uuidof(IUnknown) && id != __uuidof(ID3D12GraphicsCommandList)))
            return E_NOINTERFACE;
        *out = s;
        ++s->refs;
        return S_OK;
    }
    static ULONG STDMETHODCALLTYPE AddRef(NativeDouble *s) { return ++s->refs; }
    static ULONG STDMETHODCALLTYPE Release(NativeDouble *s) { return --s->refs; }
    static void STDMETHODCALLTYPE Pipeline(NativeDouble *s, ID3D12PipelineState *pipeline) {
        ++s->restores;
        s->restored = reinterpret_cast<uint64_t>(pipeline);
        OnBind(s->list, reshade::api::pipeline_stage::all_compute, {s->restored});
    }
};
int main() {
    ApiDouble device(true), list(false);
    NativeDouble native;
    list.device = &device;
    list.native = reinterpret_cast<uint64_t>(&native);
    auto *cl = reinterpret_cast<reshade::api::command_list *>(&list);
    LiveRR().SetEnabled(false);
    ShaderDispatches().Bind(list.native, 123, 0x123456789abcdef0ull, true);
    assert(!OnDispatch(cl, 8, 8, 1));
    fprintf(stderr, "Unrelated dispatch with RR off: native queries=%u, list reads=%u\n",
            native.queries, list.native_reads);
    assert(native.queries == 0 && "unrelated dispatch must skip native/controller work");
    assert(list.native_reads == 1 && "dispatch must share one binding lookup");
    // The early exit must not swallow native-lighting targets or weaken identity checks.
    for (auto hash : {rrbypass::Temporal, rrbypass::Spatial, rrbypass::Vrrt}) {
        native.queries = 0;
        ShaderDispatches().Bind(list.native, 123, hash, true);
        assert(!OnDispatch(cl, 8, 8, 1));
        assert(native.queries != 0 && native.refs == 1);
    }
    // A successful native restore re-enters the real OnBind callback. The shader
    // identity remains canonical and missing live-frame state still falls back.
    native.valid = true;
    native.list = cl;
    LiveRR().SetEnabled(true);
    for (auto hash : {rrbypass::Temporal, rrbypass::Spatial, rrbypass::Vrrt}) {
        shader_hashes[123] = {hash, true};
        ShaderDispatches().Bind(list.native, 123, hash, true);
        native.restores = 0;
        assert(!OnDispatch(cl, 8, 8, 1));
        const auto binding = ShaderDispatches().Get(list.native);
        assert(native.restores == 1 && native.restored == 123 && native.refs == 1);
        assert(binding.pipeline == 123 && binding.shader == hash);
    }
    native.valid = false;
    // Unknown-specular candidate discovery still runs when enabled, ahead of the early exit.
    LiveRR().SetEnabled(true);
    LiveRR().SetSpecularHitEnabled(true);
    native.queries = 0;
    ShaderDispatches().Bind(list.native, 123, 0x123456789abcdef0ull, true);
    assert(!OnDispatch(cl, 8, 8, 1));
    assert(native.queries != 0 && native.refs == 1);
    LiveRR().SetEnabled(false);
    ShaderDispatches().Reset(list.native);
    native.queries = list.native_reads = 0;
    assert(!OnDispatch(cl, 8, 8, 1));
    assert(native.queries == 0 && list.native_reads == 1);
    {
        rrinternal::Scope internal;
        list.native_reads = 0;
        assert(!OnDispatch(cl, 8, 8, 1) && list.native_reads == 0);
    }
    puts("PASS: production dispatch skips unrelated work and preserves target/identity handling");
}
