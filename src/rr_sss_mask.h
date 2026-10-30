#pragma once
#include <d3d12.h>
// SSS-masked lighting sources (rrlighting::SssMasked) take the native lobe only
// where an earlier frame's SSS guide is nonzero. The guide is handed off when
// its SR recording, which leaves it COMMON, is submitted and admitted.
namespace rrsssmask {
// An older guide trails moving SSS edges further; past this, use raw lighting.
inline constexpr unsigned MaxAge = 3;
// The guide an admitted SR submission resolves, by frame and extent.
struct Record {
    bool valid{};
    unsigned frame{}, generation{}, width{}, height{};
};
// Usable by a later frame of the same feature generation and guide extent.
inline constexpr bool Usable(const Record &r, unsigned frame, unsigned generation, unsigned width,
                             unsigned height) {
    const unsigned age = frame - r.frame;
    return r.valid && r.generation == generation && r.width == width && r.height == height &&
           age != 0 && age <= MaxAge;
}
// One R16_FLOAT guide sample per lighting pixel.
inline constexpr bool Shape(const D3D12_RESOURCE_DESC &guide, const D3D12_RESOURCE_DESC &lighting) {
    return guide.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
           guide.Width == lighting.Width && guide.Height == lighting.Height &&
           guide.DepthOrArraySize == 1 && guide.MipLevels == 1 && guide.SampleDesc.Count == 1 &&
           guide.Format == DXGI_FORMAT_R16_FLOAT;
}
// Latest outcome of an SSS-masked first spatial preparation, for the overlay.
enum class State : unsigned {
    Idle,
    Active,
    NoGuide,
    Unsupported,
    // Subsurface Scattering coverage experiment (rr_sss_coverage.h) selected.
    CoverageActive,
    CoverageWaiting
};
inline constexpr const char *Label(State s) {
    switch (s) {
    case State::Active:
        return "active";
    case State::CoverageActive:
        return "active, selected by reprojected Subsurface Scattering coverage";
    case State::CoverageWaiting:
        return "waiting for usable Subsurface Scattering coverage (raw lighting)";
    case State::NoGuide:
        return "waiting for the Subsurface Scattering guide (raw lighting)";
    case State::Unsupported:
        return "unavailable on this path (raw lighting)";
    default:
        return "not running";
    }
}
} // namespace rrsssmask
