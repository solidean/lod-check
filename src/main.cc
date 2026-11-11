// lod-check: where each GPU blends between mip levels, through every graphics API this machine has.
//
// It renders nothing on screen and touches no network.
// The report goes to the terminal and to a local markdown file, and sharing it is up to whoever ran it.

#include "lod_check.hh"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>

namespace
{
constexpr char const* share_url = "https://github.com/solidean/lod-check/discussions/categories/results";

constexpr char const* usage = R"(usage: lod-check [options]

Measures where each GPU's trilinear filter blends between mip levels, with and without anisotropy,
through D3D12 and Vulkan on Windows, Vulkan on Linux and Metal on macOS.

  --out <file>     where to write the report (default: lod-check-results.md)
  --no-file        print the report only
  --curves <file>  also write every measured curve as CSV
  --software       include software rasterizers (WARP, lavapipe, llvmpipe)
  --help           show this text
  --version        show the version

lod-check never connects to the network.
)";

bool write_file(std::string const& path, std::string const& text)
{
    auto file = std::ofstream(path, std::ios::binary);
    file << text;
    return bool(file);
}

/// The report as a markdown file that pastes into a GitHub discussion unchanged.
std::string as_markdown(std::string const& report)
{
    return std::format("### lod-check results\n\n```\n{}```\n", report);
}
} // namespace

int main(int argc, char** argv)
{
    auto opts = lod_check::options();
    auto out_path = std::string("lod-check-results.md");
    auto curves_path = std::string();
    for (auto i = 1; i < argc; ++i)
    {
        auto const arg = std::string(argv[i]);
        if (arg == "--help" || arg == "-h")
        {
            std::fputs(usage, stdout);
            return 0;
        }
        if (arg == "--version")
        {
            std::puts(LOD_CHECK_VERSION);
            return 0;
        }
        if (arg == "--software")
            opts.software = true;
        else if (arg == "--no-file")
            out_path.clear();
        else if ((arg == "--out" || arg == "--curves") && i + 1 < argc)
            (arg == "--out" ? out_path : curves_path) = argv[++i];
        else
        {
            std::fprintf(stderr, "unknown argument: %s\n\n%s", argv[i], usage);
            return 2;
        }
    }

    std::puts("Measuring every GPU through every graphics API; this takes a few seconds.\n");

    auto runs = std::vector<lod_check::gpu_run>();
    auto append = [&](std::vector<lod_check::gpu_run> more)
    {
        for (auto& run : more)
            runs.push_back(std::move(run));
    };
#if LOD_CHECK_D3D12
    append(lod_check::run_d3d12(opts));
#endif
#if LOD_CHECK_VULKAN
    append(lod_check::run_vulkan(opts));
#endif
#if LOD_CHECK_METAL
    append(lod_check::run_metal(opts));
#endif

    auto const report = lod_check::format_report(runs);
    std::fputs(report.c_str(), stdout);

    if (!out_path.empty())
    {
        if (write_file(out_path, as_markdown(report)))
            std::printf("\nWrote %s. Nothing was sent anywhere.\n"
                        "To share these results, paste that file at %s\n",
                        out_path.c_str(), share_url);
        else
            std::fprintf(stderr, "\ncould not write %s\n", out_path.c_str());
    }
    if (!curves_path.empty() && !write_file(curves_path, lod_check::format_curves(runs)))
        std::fprintf(stderr, "could not write %s\n", curves_path.c_str());

    auto const any_measured = std::any_of(
        runs.begin(), runs.end(), [](lod_check::gpu_run const& r) { return r.error.empty() && !r.adapter.empty(); });
    lod_check::wait_if_own_console();
    return any_measured ? 0 : 1;
}
