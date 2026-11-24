#include "lod_check.hh"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

namespace lod_check
{
namespace
{
/// The half-float bits of a small non-negative integer, exact for every value the pyramid holds.
std::uint16_t half_of(int value)
{
    if (value == 0)
        return 0;
    auto exponent = 0;
    while ((2 << exponent) <= value)
        ++exponent;
    auto const mantissa = (value - (1 << exponent)) << (10 - exponent);
    return std::uint16_t(((exponent + 15) << 10) | mantissa);
}

/// What a curve says about the blend between mips m and m + 1, averaged over every m the sweep covers fully.
///
/// A sample reads m plus the weight of mip m + 1.
/// That weight equals the level's fraction only for a linear blend, and drivers sharpen it once anisotropy is on.
/// Where the curve crosses m + 0.5 is the blend's center under any symmetric blend, so the shift is read there.
struct summary
{
    float shift = 0; ///< m + 0.5 minus the level at the center: positive reaches the coarser mip early, so blurrier
    float width = 0; ///< the level range from 10% to 90% of the blend: 0.8 for a linear one
    bool valid = false;
};

/// The level at which `curve` first rises through `value`, linearly interpolated between steps.
std::optional<float> crossing(float const* curve, float value)
{
    for (auto i = 1; i < sweep_steps; ++i)
    {
        auto const a = curve[i - 1];
        auto const b = curve[i];
        if (a < value && b >= value)
            return sweep_level(i - 1) + (value - a) / (b - a) / float(steps_per_level);
    }
    return std::nullopt;
}

summary summarize(float const* curve)
{
    // The last level is left out: a driver that blends late would not finish its blend inside the sweep.
    auto shift = 0.0f, width = 0.0f;
    auto shifts = 0, widths = 0;
    for (auto m = 0; m < sweep_levels - 1; ++m)
    {
        if (auto const c = crossing(curve, float(m) + 0.5f))
        {
            shift += (float(m) + 0.5f) - *c;
            ++shifts;
        }
        auto const lo = crossing(curve, float(m) + 0.1f);
        auto const hi = crossing(curve, float(m) + 0.9f);
        if (lo && hi)
        {
            width += *hi - *lo;
            ++widths;
        }
    }
    if (shifts == 0 || widths == 0)
        return {};
    return {.shift = shift / float(shifts), .width = width / float(widths), .valid = true};
}

struct gpu_group
{
    std::string name;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    bool software = false;
    std::vector<gpu_run const*> runs;
};

bool same_gpu(gpu_run const& a, gpu_run const& b)
{
    if (a.has_pairing_key && b.has_pairing_key)
        return a.pairing_key == b.pairing_key;
    return a.vendor_id == b.vendor_id && a.device_id == b.device_id && a.adapter == b.adapter;
}

std::vector<gpu_group> group_by_gpu(std::vector<gpu_run> const& runs)
{
    auto groups = std::vector<gpu_group>();
    for (auto const& run : runs)
    {
        if (run.adapter.empty())
            continue;
        auto it
            = std::find_if(groups.begin(), groups.end(), [&](gpu_group const& g) { return same_gpu(*g.runs[0], run); });
        if (it == groups.end())
        {
            groups.push_back(
                {.name = run.adapter, .vendor_id = run.vendor_id, .device_id = run.device_id, .software = run.software});
            it = groups.end() - 1;
        }
        it->runs.push_back(&run);
    }
    return groups;
}

std::string format_cell(summary const& s)
{
    if (!s.valid)
        return std::format("{:<14}", "n/a");
    return std::format("{:+.3f}  {:.2f}  ", s.shift, s.width);
}

/// One table: per anisotropy, each run's shift and width on path `p`, and each later run's shift minus the first's.
void format_table(std::string& out, std::vector<gpu_run const*> const& runs, path p)
{
    out += "  aniso";
    for (auto const* run : runs)
        out += std::format("  {:<14}", run->api);
    for (auto r = 1u; r < runs.size(); ++r)
        out += std::format("  {:<14}", std::format("{} - {}", runs[r]->api, runs[0]->api));
    out += '\n';

    for (auto a = 0; a < anisotropy_count; ++a)
    {
        out += std::format("  {:>4}x", anisotropies[a]);
        auto first = summary();
        for (auto r = 0u; r < runs.size(); ++r)
        {
            auto const s = summarize(&runs[r]->samples[sample_index(a, p, 0)]);
            if (r == 0)
                first = s;
            out += "  " + format_cell(s);
        }
        for (auto r = 1u; r < runs.size(); ++r)
        {
            auto const s = summarize(&runs[r]->samples[sample_index(a, p, 0)]);
            out += s.valid && first.valid ? std::format("  {:+.3f}", s.shift - first.shift) : "  n/a";
        }
        out += '\n';
    }
}

constexpr auto path_tolerance = 0.01f;

/// The anisotropies at which path `p` shifts differently from the square-footprint path, on any run.
std::vector<int> differing_rows(std::vector<gpu_run const*> const& runs, path p)
{
    auto rows = std::vector<int>();
    for (auto a = 0; a < anisotropy_count; ++a)
        for (auto const* run : runs)
        {
            auto const s = summarize(&run->samples[sample_index(a, p, 0)]);
            auto const ref = summarize(&run->samples[sample_index(a, path_square, 0)]);
            if (s.valid && ref.valid && std::abs(s.shift - ref.shift) > path_tolerance)
            {
                rows.push_back(a);
                break;
            }
        }
    return rows;
}

/// The summary at the anisotropy where |shift| is largest, and the narrowest width over all of them.
std::pair<summary, float> extremes(gpu_run const& run)
{
    auto largest = summary();
    auto narrowest = 1.0f;
    for (auto a = 0; a < anisotropy_count; ++a)
    {
        auto const s = summarize(&run.samples[sample_index(a, path_square, 0)]);
        if (!s.valid)
            continue;
        if (!largest.valid || std::abs(s.shift) > std::abs(largest.shift))
            largest = s;
        narrowest = std::min(narrowest, s.width);
    }
    return {largest, narrowest};
}

void format_verdicts(std::string& out, std::vector<gpu_run const*> const& runs)
{
    constexpr auto tolerance = 0.02f;
    for (auto const* run : runs)
    {
        auto const [largest, narrowest] = extremes(*run);
        auto const where = std::abs(largest.shift) < tolerance
                             ? std::string("centers each blend where the specification does")
                             : std::format("reads up to {:.3f} levels {} than the specification", std::abs(largest.shift),
                                           largest.shift > 0 ? "coarser (blurrier)" : "finer (sharper)");
        auto const shape = narrowest > 0.75f
                             ? std::string("blends linearly")
                             : std::format("sharpens the blend with anisotropy, down to {:.2f} levels wide", narrowest);
        out += std::format("  {} {}, and {}.\n", run->api, where, shape);
    }
    for (auto r = 1u; r < runs.size(); ++r)
    {
        auto worst = 0.0f;
        for (auto a = 0; a < anisotropy_count; ++a)
        {
            auto const s = summarize(&runs[r]->samples[sample_index(a, path_square, 0)]);
            auto const ref = summarize(&runs[0]->samples[sample_index(a, path_square, 0)]);
            if (s.valid && ref.valid && std::abs(s.shift - ref.shift) > std::abs(worst))
                worst = s.shift - ref.shift;
        }
        if (std::abs(worst) < tolerance)
            out += std::format("  {} and {} agree within {:.2f} levels.\n", runs[0]->api, runs[r]->api, tolerance);
        else
            out += std::format("  {} reads up to {:.3f} levels {} than {} on this GPU.\n", runs[r]->api,
                               std::abs(worst), worst > 0 ? "coarser" : "finer", runs[0]->api);
    }
}

/// The max anisotropy the run behaved as if it applied on footprint path `p`, read from its shift against the square
/// footprint: the specification divides the major axis by min(ratio, anisotropy), so each level of extra shift halves
/// the anisotropy actually used.
std::optional<float> applied_anisotropy(gpu_run const& run, int anisotropy_index, path p)
{
    auto const s = summarize(&run.samples[sample_index(anisotropy_index, p, 0)]);
    auto const ref = summarize(&run.samples[sample_index(anisotropy_index, path_square, 0)]);
    if (!s.valid || !ref.valid)
        return std::nullopt;
    auto const specified = std::min(footprint_ratios[p], float(anisotropies[anisotropy_index]));
    return specified * std::exp2(-(s.shift - ref.shift));
}

/// "2x" for an anisotropy within 0.05 of a whole number, "2.6x" otherwise.
std::string format_anisotropy(float e)
{
    auto const whole = std::round(e);
    return std::abs(e - whole) < 0.05f ? std::format("{}x", int(whole)) : std::format("{:.1f}x", e);
}

/// Per run, the max anisotropy settings it applies as another value, read from the 16:1 footprint,
/// which every setting up to 16x limits.
void format_applied(std::string& out, std::vector<gpu_run const*> const& runs)
{
    for (auto const* run : runs)
    {
        auto changed = std::string();
        for (auto a = 0; a < anisotropy_count; ++a)
        {
            auto const e = applied_anisotropy(*run, a, path_steep);
            if (e && std::abs(*e - float(anisotropies[a])) / float(anisotropies[a]) > 0.02f)
                changed += std::format("{}{}x as {}", changed.empty() ? "" : ", ", anisotropies[a], format_anisotropy(*e));
        }
        if (changed.empty())
            out += std::format("  {} applies every max anisotropy as set.\n", run->api);
        else
            out += std::format("  {} applies a max anisotropy of {}.\n", run->api, changed);
    }
}
} // namespace

std::vector<std::uint16_t> pyramid_mip(int mip)
{
    auto const size = mip_size(mip);
    return std::vector<std::uint16_t>(std::size_t(size) * std::size_t(size), half_of(mip));
}

std::string format_report(std::vector<gpu_run> const& runs)
{
    auto out = std::format("lod-check {}\nOS: {}\n", LOD_CHECK_VERSION, os_description());

    for (auto const& run : runs)
        if (run.adapter.empty())
            out += std::format("{}: not available ({})\n", run.api, run.error);

    for (auto const& group : group_by_gpu(runs))
    {
        out += std::format("\nGPU: {} ({:04x}:{:04x}){}\n", group.name, group.vendor_id, group.device_id,
                           group.software ? ", software" : "");
        auto measured = std::vector<gpu_run const*>();
        for (auto const* run : group.runs)
        {
            if (run->error.empty())
            {
                out += std::format("  {:<7} driver {}\n", run->api, run->driver);
                measured.push_back(run);
            }
            else
                out += std::format("  {:<7} failed: {}\n", run->api, run->error);
        }
        if (measured.empty())
            continue;

        out += "\n  Shift of each mip blend from where the specification centers it, in levels (+ is blurrier),\n";
        out += "  and the blend's width from 10% to 90% (a linear blend is 0.80). Explicit gradients, square "
               "footprint.\n\n";
        format_table(out, measured, path_square);

        for (auto const p : {path_level, path_wide, path_steep})
        {
            auto const name = p == path_level  ? "An explicit level"
                            : p == path_wide   ? "A 4:1 footprint"
                                               : "A 16:1 footprint";
            auto const rows = differing_rows(measured, p);
            if (rows.empty())
            {
                out += std::format("\n  {} shifts the same, within {:.2f} levels.", name, path_tolerance);
                continue;
            }
            out += std::format("\n  {} shifts the same, except at", name);
            for (auto i = 0u; i < rows.size(); ++i)
            {
                out += std::format("{} {}x:", i == 0 ? "" : ";", anisotropies[rows[i]]);
                for (auto r = 0u; r < measured.size(); ++r)
                {
                    auto const s = summarize(&measured[r]->samples[sample_index(rows[i], p, 0)]);
                    out += std::format("{} {} {}", r == 0 ? "" : ",", measured[r]->api,
                                       s.valid ? std::format("{:+.3f}", s.shift) : std::string("n/a"));
                    if (p != path_level)
                        if (auto const e = applied_anisotropy(*measured[r], rows[i], p))
                            out += std::format(" (as if {})", format_anisotropy(*e));
                }
            }
            out += '.';
        }
        out += "\n\n";
        format_verdicts(out, measured);
        format_applied(out, measured);
    }
    return out;
}

std::string format_curves(std::vector<gpu_run> const& runs)
{
    auto out = std::string("api,gpu,anisotropy,path,level,sample\n");
    constexpr char const* path_names[] = {"level", "square", "wide", "steep"};
    for (auto const& run : runs)
    {
        if (!run.error.empty() || run.adapter.empty())
            continue;
        for (auto a = 0; a < anisotropy_count; ++a)
            for (auto p = 0; p < path_count; ++p)
                for (auto i = 0; i < sweep_steps; ++i)
                    out += std::format("{},\"{}\",{},{},{:.6f},{:.6f}\n", run.api, run.adapter, anisotropies[a],
                                       path_names[p], sweep_level(i), run.samples[sample_index(a, p, i)]);
    }
    return out;
}
} // namespace lod_check
