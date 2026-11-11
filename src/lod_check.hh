#pragma once

// What every backend measures, and what the report makes of it.
//
// The pyramid's mip m holds the value m everywhere, so a trilinear sample reads m plus the weight the GPU gave mip m + 1.
// Sweeping the level in small steps traces that weight as a curve, one per sampler and sampling path.
// The shaders in shaders/ hard-code the same constants; change them together.

#include <cstdint>
#include <string>
#include <vector>

namespace lod_check
{
inline constexpr int pyramid_size = 256;
inline constexpr int pyramid_mips = 9;

/// The sweep runs from level 0 to `sweep_levels` in steps of 1 / `steps_per_level`, endpoints included.
inline constexpr int steps_per_level = 64;
inline constexpr int sweep_levels = 7;
inline constexpr int sweep_steps = sweep_levels * steps_per_level + 1;

/// Threads per group along the sweep, and the groups that cover it.
inline constexpr int group_size = 64;
inline constexpr int sweep_groups = (sweep_steps + group_size - 1) / group_size;

/// One sampler per entry, all trilinear and repeating, with no LOD bias.
inline constexpr int anisotropies[] = {1, 2, 3, 4, 6, 8, 12, 16};
inline constexpr int anisotropy_count = int(sizeof(anisotropies) / sizeof(anisotropies[0]));

/// Where a sample's level comes from.
/// Every path asks for the same level, so a driver that treats them alike draws three identical curves.
enum path : int
{
    path_level = 0,  ///< an explicit level: SampleLevel, textureLod, level()
    path_square = 1, ///< explicit gradients of a square footprint: SampleGrad, textureGrad, gradient2d()
    path_wide = 2,   ///< explicit gradients of a 4:1 footprint, the anisotropic case proper
};
inline constexpr int path_count = 3;

/// The level step `i` asks for.
inline float sweep_level(int i)
{
    return float(i) / float(steps_per_level);
}

/// Index of one sample in a run's `samples`.
inline int sample_index(int anisotropy_index, int p, int step)
{
    return (anisotropy_index * path_count + p) * sweep_steps + step;
}

/// What one graphics API measured on one GPU.
struct gpu_run
{
    std::string api;     ///< "D3D12", "Vulkan" or "Metal"
    std::string adapter; ///< the GPU's name; empty when the API itself was unavailable
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::string driver;
    std::uint64_t pairing_key = 0; ///< the LUID or registry id, only to pair one GPU's runs; never printed
    bool has_pairing_key = false;
    bool software = false;
    std::string error;          ///< why the run failed, or why the API is unavailable; empty on success
    std::vector<float> samples; ///< sweep_steps per (anisotropy, path), indexed by sample_index
};

struct options
{
    bool software = false; ///< include software rasterizers: WARP, lavapipe, llvmpipe
};

/// One run per GPU that API sees, or a single run whose `error` says why the API is missing.
std::vector<gpu_run> run_d3d12(options const& opts);
std::vector<gpu_run> run_vulkan(options const& opts);
std::vector<gpu_run> run_metal(options const& opts);

/// Mip `mip` of the pyramid, as IEEE half floats, row-major and tightly packed.
std::vector<std::uint16_t> pyramid_mip(int mip);
inline int mip_size(int mip)
{
    return pyramid_size >> mip;
}

/// The OS name and version, which the report states beside the drivers.
std::string os_description();

/// The report: the same text goes to the terminal and to the shareable file.
std::string format_report(std::vector<gpu_run> const& runs);

/// Every curve as CSV, for plotting.
std::string format_curves(std::vector<gpu_run> const& runs);

/// On Windows, keeps a console window that lod-check opened itself (a double-click) open until Enter.
void wait_if_own_console();
} // namespace lod_check
