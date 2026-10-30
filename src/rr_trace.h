#pragma once
#include <unordered_map>
#include <windows.h>
#include <psapi.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <sl.h>
#include <sl_core_api.h>
#include <sl_dlss.h>
#include <sl_dlss_d.h>
#include <MinHook.h>
#include <array>
#include <algorithm>
#include <type_traits>
#include <iterator>
#include <set>
#include <mutex>
#include <utility>
#include <cstdio>
#include <cstdarg>
#include <cwctype>
#include <cmath>
#include <cstring>
#include <atomic>
#include "rr_trace_model.h"
#include "rr_sr_reset.h"
#include "rr_sr_eligibility.h"
#include "rr_foreign_hook.h"
#include "rr_fg_depth.h"
#include <optional>

namespace rrtrace {
struct Inputs {
    void *color{};
    void *output{};
    void *depth{};
    void *motion{};
    void *diffuse{};
    void *specular{};
    void *normal{};
    void *roughness{};
    void *hit{};
    void *specular_motion{};
    void *world_to_view{};
    void *view_to_clip{};
    unsigned width{}, height{}, denoise{UINT_MAX}, roughness_mode{UINT_MAX};
    bool readable{};
};
inline void *ReadResource(const NVSDK_NGX_Parameter *p, const char *key) {
    ID3D12Resource *r{};
    if (NVSDK_NGX_SUCCEED(p->Get(key, &r)))
        return r;
    void *v{};
    return NVSDK_NGX_SUCCEED(p->Get(key, &v)) ? v : nullptr;
}
inline void *ReadPointer(const NVSDK_NGX_Parameter *p, const char *key) {
    void *v{};
    return NVSDK_NGX_SUCCEED(p->Get(key, &v)) ? v : nullptr;
}
inline unsigned ReadUnsigned(const NVSDK_NGX_Parameter *p, const char *key, unsigned missing) {
    unsigned v{};
    return NVSDK_NGX_SUCCEED(p->Get(key, &v)) ? v : missing;
}
// Only read public parameter getters; never call a graphics method on a pointer
// supplied by a caller. An observer failure must not abort the original call.
inline Inputs ReadInputs(const NVSDK_NGX_Parameter *p) {
    Inputs v{};
    if (!p)
        return v;
    __try {
        v.color = ReadResource(p, "Color");
        v.output = ReadResource(p, "Output");
        v.depth = ReadResource(p, "Depth");
        v.motion = ReadResource(p, "MotionVectors");
        v.diffuse = ReadResource(p, NVSDK_NGX_Parameter_DiffuseAlbedo);
        v.specular = ReadResource(p, NVSDK_NGX_Parameter_SpecularAlbedo);
        v.normal = ReadResource(p, NVSDK_NGX_Parameter_GBuffer_Normals);
        v.roughness = ReadResource(p, NVSDK_NGX_Parameter_GBuffer_Roughness);
        v.hit = ReadResource(p, NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance);
        v.specular_motion = ReadResource(p, NVSDK_NGX_Parameter_GBuffer_SpecularMvec);
        v.world_to_view = ReadPointer(p, NVSDK_NGX_Parameter_DLSS_WORLD_TO_VIEW_MATRIX);
        v.view_to_clip = ReadPointer(p, NVSDK_NGX_Parameter_DLSS_VIEW_TO_CLIP_MATRIX);
        v.width = ReadUnsigned(p, "Width", 0);
        v.height = ReadUnsigned(p, "Height", 0);
        v.denoise = ReadUnsigned(p, NVSDK_NGX_Parameter_DLSS_Denoise_Mode, UINT_MAX);
        v.roughness_mode = ReadUnsigned(p, NVSDK_NGX_Parameter_DLSS_Roughness_Mode, UINT_MAX);
        v.readable = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v = {};
    }
    return v;
}
inline const sl::BaseStructure *ReadLocalTag(const sl::BaseStructure **tags, uint32_t i) {
    __try {
        return tags[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
inline uint32_t FrameIndex(const sl::FrameToken &frame) {
    __try {
        return frame.structType == sl::FrameToken::s_structType && frame.structVersion == 1
                   ? uint32_t(frame)
                   : UINT_MAX;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return UINT_MAX;
    }
}
inline uint32_t ViewIndex(const sl::ViewportHandle &viewport) {
    __try {
        return uint32_t(viewport);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return UINT_MAX;
    }
}
inline uint32_t ReadViewport(const sl::BaseStructure *base) {
    __try {
        for (unsigned n = 0; base && n < 8; ++n, base = base->next)
            if (base->structType == sl::ViewportHandle::s_structType)
                return base->structVersion == 1
                           ? uint32_t(*static_cast<const sl::ViewportHandle *>(base))
                           : UINT_MAX;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return UINT_MAX;
}
struct CreationInfo {
    bool native_sr_skipped{};
    bool valid{};
    unsigned width{}, height{}, out_width{}, out_height{};
    int flags = -1, quality = -1;
};
inline CreationInfo ReadCreation(const NVSDK_NGX_Parameter *p) {
    CreationInfo v;
    if (!p)
        return v;
    __try {
        v.width = ReadUnsigned(p, "Width", 0);
        v.height = ReadUnsigned(p, "Height", 0);
        v.out_width = ReadUnsigned(p, "OutWidth", 0);
        v.out_height = ReadUnsigned(p, "OutHeight", 0);
        int flags{}, quality{};
        const bool found =
            NVSDK_NGX_SUCCEED(p->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags)) &&
            NVSDK_NGX_SUCCEED(p->Get(NVSDK_NGX_Parameter_PerfQualityValue, &quality));
        if (found) {
            v.flags = flags;
            v.quality = quality;
        }
        v.valid = found && v.width && v.height && v.out_width && v.out_height;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return {};
    }
    return v;
}
struct CameraInfo {
    bool readable{}, projection_valid{}, basis_valid{};
    float projection[16]{}, inverse[16]{}, position[3]{}, right[3]{}, up[3]{}, forward[3]{},
        jitter[2]{}, motion_scale[2]{};
    float near_plane{}, far_plane{}, fov{}, aspect{};
    unsigned depth_inverted{}, camera_motion{}, motion3d{}, reset{}, orthographic{},
        motion_dilated{}, motion_jittered{};
};
inline bool CameraNumber(float value) {
    return std::isfinite(value) && std::abs(value) < sl::INVALID_FLOAT;
}
inline CameraInfo ReadCamera(const sl::Constants &c) {
    CameraInfo v{};
    __try {
        if (c.structType != sl::Constants::s_structType || c.structVersion < 1 ||
            c.structVersion > 2)
            return v;
        memcpy(v.projection, &c.cameraViewToClip, sizeof(v.projection));
        memcpy(v.inverse, &c.clipToCameraView, sizeof(v.inverse));
        memcpy(v.position, &c.cameraPos, sizeof(v.position));
        memcpy(v.right, &c.cameraRight, sizeof(v.right));
        memcpy(v.up, &c.cameraUp, sizeof(v.up));
        memcpy(v.forward, &c.cameraFwd, sizeof(v.forward));
        memcpy(v.jitter, &c.jitterOffset, sizeof(v.jitter));
        memcpy(v.motion_scale, &c.mvecScale, sizeof(v.motion_scale));
        v.near_plane = c.cameraNear;
        v.far_plane = c.cameraFar;
        v.fov = c.cameraFOV;
        v.aspect = c.cameraAspectRatio;
        v.depth_inverted = unsigned(c.depthInverted);
        v.camera_motion = unsigned(c.cameraMotionIncluded);
        v.motion3d = unsigned(c.motionVectors3D);
        v.reset = unsigned(c.reset);
        v.orthographic = unsigned(c.orthographicProjection);
        v.motion_dilated = unsigned(c.motionVectorsDilated);
        v.motion_jittered = unsigned(c.motionVectorsJittered);
        v.readable = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return {};
    }
    v.projection_valid = true;
    for (unsigned i = 0; i < 16; ++i)
        if (!CameraNumber(v.projection[i]) || !CameraNumber(v.inverse[i]))
            v.projection_valid = false;
    if (v.projection_valid)
        for (unsigned i = 0; i < 4; ++i)
            for (unsigned j = 0; j < 4; ++j) {
                double sum = 0;
                for (unsigned k = 0; k < 4; ++k)
                    sum += double(v.projection[i * 4 + k]) * v.inverse[k * 4 + j];
                if (std::abs(sum - (i == j ? 1. : 0.)) > 0.03)
                    v.projection_valid = false;
            }
    v.basis_valid = true;
    const float *axes[] = {v.right, v.up, v.forward};
    for (float p : v.position)
        if (!CameraNumber(p))
            v.basis_valid = false;
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 3; ++j) {
            double dot = 0;
            for (unsigned k = 0; k < 3; ++k) {
                if (!CameraNumber(axes[i][k]) || !CameraNumber(axes[j][k]))
                    v.basis_valid = false;
                dot += double(axes[i][k]) * axes[j][k];
            }
            if (!std::isfinite(dot) || std::abs(dot - (i == j ? 1. : 0.)) > 0.03)
                v.basis_valid = false;
        }
    return v;
}
struct SlCall {
    bool valid{};
    unsigned feature{}, frame{UINT_MAX}, viewport{UINT_MAX};
    sl::CommandBuffer *command{};
};
struct LastError {
    DWORD value = GetLastError();
    ~LastError() {
        SetLastError(value);
    }
};
struct OptionInfo {
    bool valid{};
    unsigned mode{}, width{}, height{};
};
template <class T> inline OptionInfo ReadOptions(const T &options) {
    OptionInfo v{};
    __try {
        if (options.structType == T::s_structType && options.structVersion >= 1 &&
            options.structVersion <= 3) {
            v.valid = true;
            v.mode = unsigned(options.mode);
            v.width = options.outputWidth;
            v.height = options.outputHeight;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v = {};
    }
    return v;
}
struct TagInfo {
    bool valid{}, resource_valid{};
    unsigned type{}, life{}, width{}, height{};
    void *native{};
};
inline TagInfo ReadTag(const sl::BaseStructure *base) {
    TagInfo v{};
    __try {
        if (!base || base->structType != sl::ResourceTag::s_structType || base->structVersion != 1)
            return v;
        auto *tag = static_cast<const sl::ResourceTag *>(base);
        v.valid = true;
        v.type = tag->type;
        v.life = unsigned(tag->lifecycle);
        v.width = tag->extent.width;
        v.height = tag->extent.height;
        auto *r = tag->resource;
        if (r && r->structType == sl::Resource::s_structType && r->structVersion == 1) {
            v.resource_valid = true;
            v.native = r->native;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v = {};
    }
    return v;
}
// A slSetTagForFrame call's Frame Generation depth tag (rrfgdepth::FgDepthIndex), read only
// through public fields. index == count: the call has none.
struct FgTag {
    unsigned frame{UINT_MAX}, viewport{UINT_MAX}, index{}, count{};
    uint32_t lifecycle{}, state{}, extent_width{}, extent_height{};
    void *native{};
    sl::CommandBuffer *command{};
};
static_assert(rrfgdepth::DepthTag == sl::kBufferTypeDepth &&
                  rrfgdepth::HudlessTag == sl::kBufferTypeHUDLessColor &&
                  rrfgdepth::OnlyValidNow == uint32_t(sl::eOnlyValidNow),
              "Frame Generation tag constants match Streamline's");
inline bool ReadFgTag(const sl::ResourceTag *tags, uint32_t count, FgTag &v) {
    uint32_t types[rrfgdepth::MaxTags]{};
    v.count = count;
    v.index = count;
    if (!tags || !count || count > rrfgdepth::MaxTags)
        return false;
    __try {
        for (uint32_t i = 0; i < count; ++i) {
            if (tags[i].structType != sl::ResourceTag::s_structType ||
                tags[i].structVersion != 1)
                return false;
            types[i] = tags[i].type;
        }
        const unsigned at = rrfgdepth::FgDepthIndex(types, count);
        if (at == count)
            return false;
        const auto &tag = tags[at];
        auto *r = tag.resource;
        if (!r || r->structType != sl::Resource::s_structType || r->structVersion != 1 ||
            r->type != sl::ResourceType::eTex2d || !r->native)
            return false;
        v.index = at;
        v.lifecycle = uint32_t(tag.lifecycle);
        v.extent_width = tag.extent.width;
        v.extent_height = tag.extent.height;
        v.native = r->native;
        v.state = r->state;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v.index = count;
        return false;
    }
}
// Copies the caller's tags and its depth tag's resource description, so a replacement changes
// only the copy. The copies keep every other field, `next` chains included.
inline bool CopyTags(const sl::ResourceTag *tags, uint32_t count, unsigned index,
                     sl::ResourceTag *out, sl::Resource &resource) {
    __try {
        memcpy(out, tags, sizeof(sl::ResourceTag) * count);
        memcpy(&resource, tags[index].resource, sizeof(sl::Resource));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
inline const char *TagName(unsigned t) {
    switch (t) {
    case 0:
        return "depth";
    case 1:
        return "motion";
    case 2:
        return "HUDless";
    case 3:
        return "color-input";
    case 4:
        return "color-output";
    case 5:
        return "normals";
    case 6:
        return "roughness";
    case 7:
        return "diffuse-albedo";
    case 8:
        return "specular-albedo";
    case 10:
        return "specular-motion";
    case 14:
        return "normal-roughness";
    case 23:
        return "UI";
    case 42:
        return "specular-hit-distance";
    case 53:
        return "backbuffer";
    default:
        return "other";
    }
}
inline const char *SlName(unsigned f) {
    return f == sl::kFeatureDLSS ? "SR" : f == sl::kFeatureDLSS_RR ? "RR" : "other";
}
class Probe {
    friend struct ProbeTestAccess;

  public:
    using Logger = void (*)(const char *);
    using Create = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D12GraphicsCommandList *, NVSDK_NGX_Feature,
                                                  const NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
    using Evaluate = decltype(&NVSDK_NGX_D3D12_EvaluateFeature);
    using EvaluateC = decltype(&NVSDK_NGX_D3D12_EvaluateFeature_C);
    using Release = decltype(&NVSDK_NGX_D3D12_ReleaseFeature);
    using Shutdown = NVSDK_NGX_Result(NVSDK_CONV *)();
    using Shutdown1 = decltype(&NVSDK_NGX_D3D12_Shutdown1);
    struct Slot {
        HMODULE module{};
        Kind family{};
        char name[MAX_PATH]{};
        Create create{};
        Evaluate eval{};
        EvaluateC eval_c{};
        Release release{};
        Shutdown shutdown{};
        Shutdown1 shutdown1{};
        bool shutdown_covered{};
        Handles handles;
        std::unordered_map<uintptr_t, CreationInfo> creation;
    };
    Logger logger{};
    bool (*replace_evaluate)(ID3D12GraphicsCommandList *, const NVSDK_NGX_Parameter *,
                             NVSDK_NGX_Result &){};
    void (*before_evaluate)(ID3D12GraphicsCommandList *, Kind, const NVSDK_NGX_Parameter *){};
    void (*after_evaluate)(ID3D12GraphicsCommandList *, Kind, const NVSDK_NGX_Parameter *,
                           NVSDK_NGX_Result){};
    void (*after_constants)(uint32_t, uint32_t, const sl::Constants &){};
    // Frame Generation depth (rr_fg_depth.h): called for every slSetTagForFrame call that tags
    // Frame Generation's depth, with no lock of the probe held. True: tag `replacement`
    // instead of the game's depth (a copy of its description, to change native and state).
    bool (*fg_depth)(const FgTag &, sl::Resource &replacement){};
    // After a replacement was tagged: Streamline's result for it (eOk: kept).
    void (*fg_result)(const FgTag &, sl::Result){};
    bool (*before_shutdown)(HMODULE, ID3D12Device *){};
    void (*after_shutdown)(HMODULE, ID3D12Device *, bool){};
    static SlCall CurrentCall() {
        return current_call;
    }
    static HMODULE CurrentRuntime() {
        return current_runtime;
    }
    // Another add-on hooked NGX evaluation before RR (rrforeign::Beneath): it sees only the
    // calls RR passes down, so RR keeps the game's SR call. Set once, read lock-free.
    static bool ForeignBeneath() {
        return foreign_beneath.load(std::memory_order_acquire);
    }
    static const char *ForeignOwner() {
        return ForeignBeneath() ? foreign_owner : "";
    }
    static CreationInfo CurrentCreation() {
        return current_creation;
    }
    // Conservative process-wide generation: a shutdown of any observed public
    // runtime retires outstanding recorded work, including a later same-address init.
    static uint64_t RuntimeEpoch() {
        return runtime_epoch.load();
    }
    static bool ShutdownCovered(HMODULE module) {
        auto *self = active;
        if (!self || !module)
            return false;
        std::lock_guard lock(self->mutex);
        for (size_t i = 0; i < self->slot_count; ++i)
            if (self->slots[i].module == module && self->slots[i].shutdown_covered)
                return true;
        return false;
    }
    static inline Probe *active{};
    std::recursive_mutex mutex;
    std::array<Slot, 32> slots{};
    size_t slot_count{};
    bool AttachNgx(const char *name, Kind family, void *create, void *eval, void *eval_c,
                   void *release, HMODULE module = nullptr, void *shutdown = nullptr,
                   void *shutdown1 = nullptr) {
        std::lock_guard lock(mutex);
        if (slot_count == slots.size())
            return false;
        const size_t n = slot_count++;
        auto &s = slots[n];
        s.family = family;
        s.module = module;
        strncpy_s(s.name, name, _TRUNCATE);
        bool installed = false;
        if (create)
            installed =
                Hook(create, CreateHooks()[n], reinterpret_cast<void **>(&s.create)) || installed;
        if (eval && module) {
            char owner[64]{};
            if (rrforeign::Beneath(eval, module, owner, sizeof(owner)) &&
                !foreign_beneath.exchange(true)) {
                strncpy_s(foreign_owner, owner[0] ? owner : "unknown code", _TRUNCATE);
                Log("DLSS evaluation was hooked before RR by %s; RR's output and depth are "
                    "announced to it",
                    foreign_owner);
            }
        }
        if (eval)
            installed = Hook(eval, EvalHooks()[n], reinterpret_cast<void **>(&s.eval)) || installed;
        if (eval_c && eval_c != eval)
            installed =
                Hook(eval_c, EvalCHooks()[n], reinterpret_cast<void **>(&s.eval_c)) || installed;
        if (release)
            installed = Hook(release, ReleaseHooks()[n], reinterpret_cast<void **>(&s.release)) ||
                        installed;
        if (shutdown)
            installed =
                Hook(shutdown, ShutdownHooks()[n], reinterpret_cast<void **>(&s.shutdown)) ||
                installed;
        if (shutdown1)
            installed =
                Hook(shutdown1, Shutdown1Hooks()[n], reinterpret_cast<void **>(&s.shutdown1)) ||
                installed;
        s.shutdown_covered =
            (shutdown || shutdown1) && (!shutdown || s.shutdown) && (!shutdown1 || s.shutdown1);
        return installed;
    }
    void Poll() {
        LastError preserve_error;
        // Discovery can call into Streamline. Never hold the accounting lock
        // across those calls: an intercepted call may already hold its locks.
        std::unique_lock poll_lock(poll_mutex, std::try_to_lock);
        if (!poll_lock.owns_lock())
            return;
        auto now = GetTickCount64();
        if (last_poll && now - last_poll < 1000)
            return;
        last_poll = now;
        if (!ready) {
            auto mh = MH_Initialize();
            if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED)
                return;
            HMODULE self{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                        GET_MODULE_HANDLE_EX_FLAG_PIN,
                                    reinterpret_cast<LPCWSTR>(&CreateHook<0>), &self)) {
                Log("Self pin failed; hooks not installed");
                return;
            }
            active = this;
            ready = true;
            Log("NGX integration initialized.");
        }
        HMODULE modules[2048]{};
        DWORD bytes{};
        if (EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) {
            const size_t count = (std::min)(size_t(bytes / sizeof(HMODULE)), std::size(modules));
            for (size_t i = 0; i < count; ++i) {
                auto module = modules[i];
                if (seen.count(module))
                    continue;
                wchar_t path[MAX_PATH]{};
                if (!GetModuleFileNameW(module, path, MAX_PATH))
                    continue;
                for (auto &c : path)
                    c = static_cast<wchar_t>(towlower(c));
                const wchar_t *leaf = wcsrchr(path, L'\\');
                leaf = leaf ? leaf + 1 : path;
                const bool core =
                    wcscmp(leaf, L"_nvngx.dll") == 0 || wcscmp(leaf, L"nvngx.dll") == 0;
                const bool sr =
                    wcscmp(leaf, L"nvngx_dlss.dll") == 0 || wcsstr(path, L"\\ngx\\models\\dlss\\");
                const bool rr = RrModelPath(path);
                const bool slmod = wcscmp(leaf, L"sl.interposer.dll") == 0;
                if (!core && !sr && !rr && !slmod)
                    continue;
                HMODULE pinned{};
                if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                            GET_MODULE_HANDLE_EX_FLAG_PIN,
                                        reinterpret_cast<LPCWSTR>(module), &pinned))
                    continue;
                seen.insert(module);
                if (slmod) {
                    InstallSl(module);
                    continue;
                }
                // Feature/model DLL exports are private ABI, despite sharing
                // names with the public runtime. Presence is not execution.
                if (!core)
                    continue;
                char utf8[MAX_PATH]{};
                WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr);
                AttachNgx(utf8,
                          rr   ? Kind::RR
                          : sr ? Kind::SR
                               : Kind::Unknown,
                          Address(module, "NVSDK_NGX_D3D12_CreateFeature"),
                          Address(module, "NVSDK_NGX_D3D12_EvaluateFeature"),
                          Address(module, "NVSDK_NGX_D3D12_EvaluateFeature_C"),
                          Address(module, "NVSDK_NGX_D3D12_ReleaseFeature"), module,
                          Address(module, "NVSDK_NGX_D3D12_Shutdown"),
                          Address(module, "NVSDK_NGX_D3D12_Shutdown1"));
            }
        }
        if (get_function)
            ResolveOptions();
    }
    // Used only by the standalone single-threaded harness, never during gameplay.
    void RemoveForTest() {
        for (auto target : targets) {
            MH_DisableHook(target);
            MH_RemoveHook(target);
        }
        targets.clear();
        active = nullptr;
    }

  private:
    static inline thread_local SlCall current_call{};
    static inline thread_local HMODULE current_runtime{};
    static inline thread_local CreationInfo current_creation{};
    static inline std::atomic<uint64_t> runtime_epoch{};
    static inline std::atomic<bool> foreign_beneath{};
    static inline char foreign_owner[64]{};
    std::mutex poll_mutex;
    bool ready{};
    ULONGLONG last_poll{};
    std::set<HMODULE> seen;
    std::set<void *> targets;
    PFun_slEvaluateFeature *sl_eval{};
    PFun_slSetTagForFrame *sl_tags{};
    using LegacyTag = sl::Result(const sl::ViewportHandle &, const sl::ResourceTag *, uint32_t,
                                 sl::CommandBuffer *);
    LegacyTag *legacy_tags{};
    PFun_slSetConstants *constants{};
    PFun_slIsFeatureSupported *support{};
    PFun_slSetFeatureLoaded *set_loaded{};
    PFun_slAllocateResources *allocate{};
    PFun_slGetFeatureFunction *get_function{};
    PFun_slDLSSDSetOptions *rr_options{};
    PFun_slDLSSSetOptions *sr_options{};
    HMODULE sl_module{};
    int rr_resolve = -999, sr_resolve = -999;
    void Log(const char *fmt, ...) const {
        if (!logger)
            return;
        DWORD error = GetLastError();
        char line[2048];
        va_list args;
        va_start(args, fmt);
        vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, args);
        va_end(args);
        logger(line);
        SetLastError(error);
    }
    static void *Address(HMODULE m, const char *name) {
        return reinterpret_cast<void *>(GetProcAddress(m, name));
    }
    bool Hook(void *target, void *replacement, void **original) {
        if (!target || targets.count(target))
            return false;
        HMODULE owner{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(target), &owner)) {
            Log("Hook skipped: cannot pin target module address=%p", target);
            return false;
        }
        auto status = MH_CreateHook(target, replacement, original);
        if (status != MH_OK) {
            Log("Hook create failed target=%p status=%d", target, int(status));
            return false;
        }
        status = MH_EnableHook(target);
        if (status != MH_OK) {
            MH_RemoveHook(target);
            *original = nullptr;
            Log("Hook enable failed status=%d", int(status));
            return false;
        }
        targets.insert(target);
        return true;
    }
    template <size_t N>
    static NVSDK_NGX_Result NVSDK_CONV CreateHook(ID3D12GraphicsCommandList *cl,
                                                  NVSDK_NGX_Feature f, const NVSDK_NGX_Parameter *p,
                                                  NVSDK_NGX_Handle **out) {
        auto *self = active;
        auto &s = self->slots[N];
        CreationInfo creation{};
        {
            LastError preserve_error;
            creation = ReadCreation(p);
        }
        const auto result = s.create(cl, f, p, out);
        const DWORD error = GetLastError();
        {
            std::lock_guard lock(self->mutex);
            const bool ok = NVSDK_NGX_SUCCEED(result);
            s.handles.Created(ok && out ? reinterpret_cast<uintptr_t>(*out) : 0, unsigned(f), ok);
            if (ok && out && *out) {
                const auto id = reinterpret_cast<uintptr_t>(*out);
                s.creation.erase(id);
                if (creation.valid && s.creation.size() < 128)
                    s.creation.emplace(id, creation);
            }
        }
        SetLastError(error);
        return result;
    }
    template <size_t N, bool C>
    static NVSDK_NGX_Result EvalCommon(
        ID3D12GraphicsCommandList *cl, const NVSDK_NGX_Handle *h, const NVSDK_NGX_Parameter *p,
        std::conditional_t<C, PFN_NVSDK_NGX_ProgressCallback_C, PFN_NVSDK_NGX_ProgressCallback>
            cb) {
        auto *self = active;
        auto &s = self->slots[N];
        Kind kind;
        CreationInfo creation{};
        DWORD error = GetLastError();
        {
            std::lock_guard lock(self->mutex);
            kind = s.family == Kind::Unknown ? s.handles.Lookup(reinterpret_cast<uintptr_t>(h))
                                             : s.family;
            auto c = s.creation.find(reinterpret_cast<uintptr_t>(h));
            if (c != s.creation.end())
                creation = c->second;
        }
        struct RuntimeScope {
            HMODULE prior = current_runtime;
            CreationInfo previous = current_creation;
            RuntimeScope(HMODULE m, CreationInfo c) {
                current_runtime = m;
                current_creation = c;
            }
            ~RuntimeScope() {
                current_runtime = prior;
                current_creation = previous;
            }
        } runtime_scope(s.module, creation);
        if (self->before_evaluate)
            try {
                self->before_evaluate(cl, kind, p);
            } catch (...) {
                self->Log("pre-evaluate observer failed; original NGX call preserved");
            }
        std::optional<rrsr::ResetParameter> reset_parameter;
        if (kind == Kind::SR && self->replace_evaluate && (cb || !creation.valid))
            rrsr::Note(cb ? rrsr::Callback : rrsr::Creation);
        if (kind == Kind::SR && !cb && creation.valid && self->replace_evaluate) {
            try {
                rrsr::ResetParameter availability(p);
                NVSDK_NGX_Result replacement_result = NVSDK_NGX_Result_FAIL_PlatformError;
                if (!availability.Ready())
                    rrsr::Note(rrsr::Reset);
                if (availability.Ready() && self->replace_evaluate(cl, p, replacement_result) &&
                    NVSDK_NGX_SUCCEED(replacement_result)) {
                    std::lock_guard lock(self->mutex);
                    auto at = s.creation.find(reinterpret_cast<uintptr_t>(h));
                    if (at != s.creation.end())
                        at->second.native_sr_skipped = true;
                    SetLastError(error);
                    return replacement_result;
                }
            } catch (...) {
                self->Log("RR replacement unavailable; native SR retained");
            }
        }
        if (kind == Kind::SR && creation.native_sr_skipped) {
            try {
                if (!reset_parameter)
                    reset_parameter.emplace(p);
                if (!reset_parameter->Force()) {
                    self->Log("Native SR history reset could not be applied");
                    SetLastError(error);
                    return NVSDK_NGX_Result_FAIL_InvalidParameter;
                }
            } catch (...) {
                SetLastError(error);
                return NVSDK_NGX_Result_FAIL_InvalidParameter;
            }
        }
        SetLastError(error);
        NVSDK_NGX_Result result;
        if constexpr (C)
            result = s.eval_c(cl, h, p, cb);
        else
            result = s.eval(cl, h, p, cb);
        error = GetLastError();
        // The observer sees the caller's reset flag, not our native-SR recovery reset.
        reset_parameter.reset();
        if (kind == Kind::SR && creation.native_sr_skipped && NVSDK_NGX_SUCCEED(result)) {
            std::lock_guard lock(self->mutex);
            auto at = s.creation.find(reinterpret_cast<uintptr_t>(h));
            if (at != s.creation.end())
                at->second.native_sr_skipped = false;
        }
        if (self->after_evaluate)
            try {
                self->after_evaluate(cl, kind, p, result);
            } catch (...) {
                self->Log("post-evaluate observer failed; original NGX result preserved");
            }
        SetLastError(error);
        return result;
    }
    template <size_t N>
    static NVSDK_NGX_Result NVSDK_CONV EvalHook(ID3D12GraphicsCommandList *cl,
                                                const NVSDK_NGX_Handle *h,
                                                const NVSDK_NGX_Parameter *p,
                                                PFN_NVSDK_NGX_ProgressCallback cb) {
        return EvalCommon<N, false>(cl, h, p, cb);
    }
    template <size_t N>
    static NVSDK_NGX_Result NVSDK_CONV EvalCHook(ID3D12GraphicsCommandList *cl,
                                                 const NVSDK_NGX_Handle *h,
                                                 const NVSDK_NGX_Parameter *p,
                                                 PFN_NVSDK_NGX_ProgressCallback_C cb) {
        return EvalCommon<N, true>(cl, h, p, cb);
    }
    template <size_t N> static NVSDK_NGX_Result NVSDK_CONV ReleaseHook(NVSDK_NGX_Handle *h) {
        auto *self = active;
        auto result = self->slots[N].release(h);
        DWORD error = GetLastError();
        {
            std::lock_guard lock(self->mutex);
            self->slots[N].handles.Released(reinterpret_cast<uintptr_t>(h),
                                            NVSDK_NGX_SUCCEED(result));
            if (NVSDK_NGX_SUCCEED(result))
                self->slots[N].creation.erase(reinterpret_cast<uintptr_t>(h));
        }
        SetLastError(error);
        return result;
    }
    template <size_t... N> static auto Creates(std::index_sequence<N...>) {
        return std::array<void *, sizeof...(N)>{reinterpret_cast<void *>(&CreateHook<N>)...};
    }
    template <size_t N, bool Device> static NVSDK_NGX_Result ShutdownCommon(ID3D12Device *device) {
        auto *self = active;
        auto &s = self->slots[N];
        const DWORD incoming = GetLastError();
        ++runtime_epoch;
        bool allowed = true;
        try {
            if (self->before_shutdown)
                allowed = self->before_shutdown(s.module, device);
        } catch (...) {
            allowed = false;
        }
        SetLastError(incoming);
        if (!allowed) {
            self->Log("NGX shutdown deferred because private work has not safely completed");
            return NVSDK_NGX_Result_FAIL_PlatformError;
        }
        NVSDK_NGX_Result result;
        if constexpr (Device)
            result = s.shutdown1(device);
        else
            result = s.shutdown();
        const DWORD error = GetLastError();
        const bool success = NVSDK_NGX_SUCCEED(result);
        if (success) {
            std::lock_guard lock(self->mutex);
            s.handles = Handles{};
            s.creation.clear();
        }
        try {
            if (self->after_shutdown)
                self->after_shutdown(s.module, device, success);
        } catch (...) {
            self->Log("post-shutdown observer failed; original result preserved");
        }
        SetLastError(error);
        return result;
    }
    template <size_t N> static NVSDK_NGX_Result NVSDK_CONV ShutdownHook() {
        return ShutdownCommon<N, false>(nullptr);
    }
    template <size_t N> static NVSDK_NGX_Result NVSDK_CONV Shutdown1Hook(ID3D12Device *device) {
        return ShutdownCommon<N, true>(device);
    }
    template <size_t... N> static auto Shutdowns(std::index_sequence<N...>) {
        return std::array<void *, sizeof...(N)>{reinterpret_cast<void *>(&ShutdownHook<N>)...};
    }
    template <size_t... N> static auto Shutdowns1(std::index_sequence<N...>) {
        return std::array<void *, sizeof...(N)>{reinterpret_cast<void *>(&Shutdown1Hook<N>)...};
    }
    static std::array<void *, 32> ShutdownHooks() {
        return Shutdowns(std::make_index_sequence<32>{});
    }
    static std::array<void *, 32> Shutdown1Hooks() {
        return Shutdowns1(std::make_index_sequence<32>{});
    }
    template <size_t... N> static auto Evals(std::index_sequence<N...>) {
        return std::array<void *, sizeof...(N)>{reinterpret_cast<void *>(&EvalHook<N>)...};
    }
    template <size_t... N> static auto EvalCs(std::index_sequence<N...>) {
        return std::array<void *, sizeof...(N)>{reinterpret_cast<void *>(&EvalCHook<N>)...};
    }
    template <size_t... N> static auto Releases(std::index_sequence<N...>) {
        return std::array<void *, sizeof...(N)>{reinterpret_cast<void *>(&ReleaseHook<N>)...};
    }
    static std::array<void *, 32> CreateHooks() {
        return Creates(std::make_index_sequence<32>{});
    }
    static std::array<void *, 32> EvalHooks() {
        return Evals(std::make_index_sequence<32>{});
    }
    static std::array<void *, 32> EvalCHooks() {
        return EvalCs(std::make_index_sequence<32>{});
    }
    static std::array<void *, 32> ReleaseHooks() {
        return Releases(std::make_index_sequence<32>{});
    }
    static sl::Result SlEval(sl::Feature f, const sl::FrameToken &frame,
                             const sl::BaseStructure **inputs, uint32_t count,
                             sl::CommandBuffer *cl) {
        struct Scope {
            SlCall previous;
            Scope() : previous(current_call) {}
            ~Scope() {
                current_call = previous;
            }
        } scope;
        {
            LastError preserve_error;
            unsigned viewport = UINT_MAX;
            bool found = false, duplicate = false;
            if (inputs && count <= 32)
                for (unsigned i = 0; i < count; ++i) {
                    auto v = ReadViewport(ReadLocalTag(inputs, i));
                    if (v != UINT_MAX) {
                        duplicate |= found;
                        found = true;
                        viewport = v;
                    }
                }
            if (duplicate)
                viewport = UINT_MAX;
            auto index = FrameIndex(frame);
            current_call = {index != UINT_MAX && viewport != UINT_MAX && cl != nullptr, f, index,
                            viewport, cl};
        }
        return active->sl_eval(f, frame, inputs, count, cl);
    }
    // Frame Generation depth (rr_fg_depth.h): only a call that tags Frame Generation's depth
    // may be changed, and then only in a copy; the game's array is never written. A refused
    // replacement re-sends the game's own tags, so Frame Generation keeps its input.
    static sl::Result Tags(const sl::FrameToken &frame, const sl::ViewportHandle &viewport,
                           const sl::ResourceTag *tags, uint32_t count, sl::CommandBuffer *cl) {
        auto *self = active;
        FgTag fg;
        sl::ResourceTag copy[rrfgdepth::MaxTags];
        sl::Resource resource(sl::ResourceType::eTex2d, nullptr);
        bool replaced = false;
        if (self->fg_depth) {
            LastError preserve_error;
            if (ReadFgTag(tags, count, fg)) {
                fg.frame = FrameIndex(frame);
                fg.viewport = ViewIndex(viewport);
                fg.command = cl;
                try {
                    replaced = CopyTags(tags, count, fg.index, copy, resource) &&
                               self->fg_depth(fg, resource);
                } catch (...) {
                    replaced = false;
                }
            }
        }
        if (!replaced)
            return self->sl_tags(frame, viewport, tags, count, cl);
        copy[fg.index].resource = &resource;
        const auto supplied = self->sl_tags(frame, viewport, copy, count, cl);
        const auto result = supplied == sl::Result::eOk
                                ? supplied
                                : self->sl_tags(frame, viewport, tags, count, cl);
        {
            LastError preserve_error;
            try {
                if (self->fg_result)
                    self->fg_result(fg, supplied);
            } catch (...) {
            }
        }
        return result;
    }
    static sl::Result Legacy(const sl::ViewportHandle &viewport, const sl::ResourceTag *tags,
                             uint32_t count, sl::CommandBuffer *cl) {
        return active->legacy_tags(viewport, tags, count, cl);
    }
    static sl::Result Constants(const sl::Constants &c, const sl::FrameToken &frame,
                                const sl::ViewportHandle &viewport) {
        auto *self = active;
        auto result = self->constants(c, frame, viewport);
        LastError preserve_error;
        if (result == sl::Result::eOk && self->after_constants)
            try {
                self->after_constants(FrameIndex(frame), ReadViewport(&viewport), c);
            } catch (...) {
                self->Log("camera observer failed; original constants result preserved");
            }
        return result;
    }
    static sl::Result Support(sl::Feature f, const sl::AdapterInfo &a) {
        return active->support(f, a);
    }
    static sl::Result Loaded(sl::Feature f, bool value) {
        return active->set_loaded(f, value);
    }
    static sl::Result Allocate(sl::CommandBuffer *cl, sl::Feature f, const sl::ViewportHandle &v) {
        return active->allocate(cl, f, v);
    }
    static sl::Result RROptions(const sl::ViewportHandle &v, const sl::DLSSDOptions &o) {
        return active->rr_options(v, o);
    }
    static sl::Result SROptions(const sl::ViewportHandle &v, const sl::DLSSOptions &o) {
        return active->sr_options(v, o);
    }
    void InstallSl(HMODULE m) {
        if (sl_module && sl_module != m) {
            Log("Additional SL interposer skipped; original instance remains observed");
            return;
        }
        sl_module = m;
        auto install = [&](const char *name, void *hook, void **original) {
            bool ok = Hook(Address(m, name), hook, original);
            if (!ok)
                Log("Streamline hook unavailable: %s", name);
        };
        install("slEvaluateFeature", reinterpret_cast<void *>(&SlEval),
                reinterpret_cast<void **>(&sl_eval));
        install("slSetTagForFrame", reinterpret_cast<void *>(&Tags),
                reinterpret_cast<void **>(&sl_tags));
        install("slSetTag", reinterpret_cast<void *>(&Legacy),
                reinterpret_cast<void **>(&legacy_tags));
        install("slSetConstants", reinterpret_cast<void *>(&Constants),
                reinterpret_cast<void **>(&constants));
        install("slIsFeatureSupported", reinterpret_cast<void *>(&Support),
                reinterpret_cast<void **>(&support));
        install("slSetFeatureLoaded", reinterpret_cast<void *>(&Loaded),
                reinterpret_cast<void **>(&set_loaded));
        install("slAllocateResources", reinterpret_cast<void *>(&Allocate),
                reinterpret_cast<void **>(&allocate));
        get_function =
            reinterpret_cast<PFun_slGetFeatureFunction *>(Address(m, "slGetFeatureFunction"));
    }
    void ResolveOptions() {
        if (!rr_options) {
            void *f{};
            auto r = get_function(sl::kFeatureDLSS_RR, "slDLSSDSetOptions", f);
            if (rr_resolve != int(r)) {
                rr_resolve = int(r);
                if (r != sl::Result::eOk)
                    Log("RR options unavailable (%d)", int(r));
            }
            if (r == sl::Result::eOk && f)
                Hook(f, reinterpret_cast<void *>(&RROptions),
                     reinterpret_cast<void **>(&rr_options));
        }
        if (!sr_options) {
            void *f{};
            auto r = get_function(sl::kFeatureDLSS, "slDLSSSetOptions", f);
            if (sr_resolve != int(r)) {
                sr_resolve = int(r);
                if (r != sl::Result::eOk)
                    Log("SR options unavailable (%d)", int(r));
            }
            if (r == sl::Result::eOk && f)
                Hook(f, reinterpret_cast<void *>(&SROptions),
                     reinterpret_cast<void **>(&sr_options));
        }
    }
};
} // namespace rrtrace
