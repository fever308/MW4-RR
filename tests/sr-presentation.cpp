#include "../src/rr_sr_eligibility.h"
#include "../src/rr_foreign_hook.h"
#include "../src/rr_sl_route.h"
#include "../src/rr_spatial_fallback.h"
#include "../src/rr_status.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <string>
int main() {
    rrstatus::Telemetry telemetry;
    rrstatus::Flags flags;
    flags.rr = true;
    telemetry.Reset(1);
    telemetry.Completed(1, 1, 100, true, false, false, true);
    assert(telemetry.Read(flags, 100).route == rrstatus::Route::Replacement);
    telemetry.Completed(2, 1, 101, false, false, false, true);
    assert(telemetry.Read(flags, 101).route == rrstatus::Route::SpatialFallback);
    telemetry.Completed(3, 1, 102, true, false, false, false);
    assert(telemetry.Read(flags, 102).route == rrstatus::Route::NativeSR);
    assert(telemetry.Read(flags, 2000).route == rrstatus::Route::Unknown);
    assert(telemetry.Read(flags, 102).hit == rrstatus::State::Off);
    flags.hit = true;
    assert(telemetry.Read(flags, 102).hit == rrstatus::State::Waiting);
    telemetry.Completed(4, 1, 103, true, false, false, true, true);
    assert(telemetry.Read(flags, 103).hit == rrstatus::State::Active);
    telemetry.Completed(5, 1, 104, true, false, false, true, false);
    assert(telemetry.Read(flags, 104).hit == rrstatus::State::Waiting);
    flags.rr = false;
    assert(telemetry.Read(flags, 104).hit == rrstatus::State::ParentOff);
    flags.rr = true;

    rrsr::StateProof p;
    p.known = {true, true, true, true};
    p.states = {D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    p.resources = {1, 2, 3, 4};
    assert(rrsr::Eligible(p));
    // An input this list never transitioned takes the DLSS contract state; the output must be seen.
    for (unsigned i = 0; i < 4; ++i) {
        auto q = p;
        q.known[i] = false;
        assert(rrsr::Eligible(q) == (i < 3));
    }
    // What the game hands RR (17:45 and 17:54 captures): colour pixel-shader read only, depth
    // and motion never transitioned on the SR list. RR's evaluate transitions colour itself.
    auto q = p;
    q.known = {true, false, false, true};
    q.states[0] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    assert(rrsr::Eligible(q));
    q.states[0] = D3D12_RESOURCE_STATE_COMMON;
    assert(!rrsr::Eligible(q));
    q.states[0] = D3D12_RESOURCE_STATE_COPY_SOURCE;
    assert(!rrsr::Eligible(q));
    q.states[0] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE;
    assert(!rrsr::Eligible(q));
    q = p;
    q.states[1] = D3D12_RESOURCE_STATE_DEPTH_READ;
    assert(!rrsr::Eligible(q));
    q = p;
    q.resources[2] = 0;
    q.known[2] = false;
    assert(!rrsr::Eligible(q));
    assert(rrsr::ReadOnly(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) &&
           rrsr::ReadOnly(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) &&
           !rrsr::ReadOnly(D3D12_RESOURCE_STATE_COMMON) &&
           !rrsr::ReadOnly(D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    assert(std::string(rrsr::SkipText(rrsr::Ineligible)) == "state" &&
           std::string(rrsr::SkipText(rrsr::Count)) == "unknown");
    // Jumps a hook beneath RR's starts NGX evaluation with (rrforeign::Beneath).
    {
        const uint8_t rel[16] = {0xE9, 0x10, 0x00, 0x00, 0x00};
        assert(rrforeign::JumpTarget(rel, 0x1000) == 0x1015);
        const uint8_t back[16] = {0xE9, 0xF0, 0xFF, 0xFF, 0xFF};
        assert(rrforeign::JumpTarget(back, 0x1000) == 0xFF5);
        const uint8_t indirect[16] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
        assert(rrforeign::JumpTarget(indirect, 0x2000) == 0x2006);
        const uint8_t absolute[16] = {0x48, 0xB8, 0x88, 0x77, 0x66, 0x55,
                                      0x44, 0x33, 0x22, 0x11, 0xFF, 0xE0};
        assert(rrforeign::JumpTarget(absolute, 0) == 0x1122334455667788ull);
        const uint8_t prologue[16] = {0x48, 0x89, 0x5C, 0x24, 0x08}; // mov [rsp+8], rbx
        assert(rrforeign::JumpTarget(prologue, 0x1000) == 0);
        assert(std::string(rrsr::SkipText(rrsr::Foreign)) == "otherHook");
    }
    q = p;
    q.states[0] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    assert(!rrsr::Eligible(q));
    q = p;
    q.states[3] = D3D12_RESOURCE_STATE_COMMON;
    assert(!rrsr::Eligible(q));
    q = p;
    q.resources[3] = q.resources[0];
    assert(!rrsr::Eligible(q));
    q = p;
    q.states[0] =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    assert(rrsr::Eligible(q));
    auto a = rrspatial::Coordinate(0, 2, 4);
    assert(a.first == 0 && a.second == 1 && a.fraction == 0);
    a = rrspatial::Coordinate(1, 2, 4);
    assert(a.first == 0 && a.second == 1 && std::abs(a.fraction - .25f) < .00001f);
    a = rrspatial::Coordinate(3, 2, 4);
    assert(a.first == 1 && a.second == 1 && a.fraction == 0);
    for (auto in : {1u, 640u, 1088u, 1920u, 3440u})
        for (auto out : {1u, 720u, 1080u, 1440u, 2160u, 5120u})
            for (unsigned x = 0; x < out; ++x) {
                auto c = rrspatial::Coordinate(x, in, out);
                assert(c.first < in && c.second < in && c.fraction >= 0 && c.fraction <= 1);
            }
    assert(rrspatial::UseFallback(true, true, false));
    assert(rrspatial::UseFallback(true, false, true));
    assert(!rrspatial::UseFallback(false, true, true));
    assert(!rrspatial::UseFallback(true, false, false));
    // RR's Streamline route: its own viewport, the game's quality mode, and its tags.
    static_assert(rrslroute::Viewport(0) == 0x52520000u);
    static_assert(rrslroute::Viewport(0x12345) == 0x52522345u);
    assert(rrslroute::Mode(0) == sl::DLSSMode::eMaxPerformance);
    assert(rrslroute::Mode(2) == sl::DLSSMode::eMaxQuality);
    assert(rrslroute::Mode(5) == sl::DLSSMode::eDLAA);
    assert(rrslroute::Mode(9) == sl::DLSSMode::eOff);
    {
        const float m[16] = {1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 4, 0, 3, 5, 7, 1};
        float inv[16];
        assert(rrslroute::Invert(m, inv));
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                float v = 0;
                for (int k = 0; k < 4; ++k)
                    v += m[r * 4 + k] * inv[k * 4 + c];
                assert(std::fabs(v - (r == c ? 1.f : 0.f)) < 1e-5f);
            }
        const float singular[16] = {};
        assert(!rrslroute::Invert(singular, inv));
        rreval::Options o;
        o.quality = 1;
        o.out_width = 3840;
        o.out_height = 2160;
        o.width = 1920;
        o.height = 1080;
        o.color_x = 8;
        o.color_y = 4;
        o.depth_x = o.depth_y = o.motion_x = o.motion_y = 0;
        o.flags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
        sl::DLSSDOptions options;
        assert(rrslroute::Options(o, 4, m, options));
        assert(options.mode == sl::DLSSMode::eBalanced && options.outputWidth == 3840);
        assert(options.colorBuffersHDR == sl::Boolean::eTrue);
        assert(options.balancedPreset == sl::DLSSDPreset(4));
        assert(!rrslroute::Options(o, 4, singular, options));
        o.quality = 7;
        assert(!rrslroute::Options(o, 4, m, options));
        o.quality = 1;
        auto fake = [](uintptr_t v) { return reinterpret_cast<ID3D12Resource *>(v); };
        rreval::Resources r{};
        r.color = fake(0x10);
        r.depth = fake(0x20);
        r.motion = fake(0x30);
        r.output = fake(0x40);
        r.normal = fake(0x50);
        rrslroute::Tags tags;
        rrslroute::Build(o, r, tags);
        assert(tags.count == 5);
        assert(tags.tags[0].type == sl::kBufferTypeScalingInputColor);
        assert(tags.tags[0].extent.left == 8 && tags.tags[0].extent.top == 4);
        assert(tags.tags[0].extent.width == 1920 && tags.tags[0].resource == &tags.resources[0]);
        assert(tags.tags[1].type == sl::kBufferTypeScalingOutputColor);
        assert(tags.resources[1].state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        assert(tags.tags[1].extent.width == 3840);
        assert(tags.resources[2].state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        assert(tags.tags[4].type == sl::kBufferTypeNormalRoughness);
        for (unsigned i = 0; i < tags.count; ++i)
            assert(tags.tags[i].lifecycle == sl::eValidUntilEvaluate);
    }
}
