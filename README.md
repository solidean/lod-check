# lod-check

Where does your GPU blend between mip levels?
lod-check measures it through every graphics API your machine has — D3D12 and Vulkan on Windows, Vulkan on Linux, Metal on macOS — and compares each against the specification.

We wrote it after finding that on NVIDIA, the same anisotropic sampler reads up to a quarter mip level blurrier on Vulkan than on D3D12.
That is driver behaviour, so it may differ on your GPU, your driver or your OS, and it may change.
This tool lets you check.

## Run it

Download the build for your OS from [Releases](https://github.com/solidean/lod-check/releases), unpack it and run `lod-check`.
It takes about a second and needs no installation.

The builds are not code-signed yet:
- **Windows** SmartScreen may warn about an unknown publisher; *More info* → *Run anyway*.
- **macOS** Gatekeeper refuses unsigned downloads; right-click → *Open*, or `xattr -d com.apple.quarantine lod-check`.

Every release is built by this repository's [GitHub Actions workflow](.github/workflows/build.yml) from the tagged commit.
To check a download, compare it against the release's `SHA256SUMS`, or verify where it was built with the [GitHub CLI](https://cli.github.com/):

```
gh attestation verify lod-check-windows-x64.zip --repo solidean/lod-check
```

Or build it yourself, below; it is a few hundred lines per API.

## What it prints

For each GPU and API, one row per sampler anisotropy:

```
  aniso  D3D12           Vulkan          Vulkan - D3D12
     1x  +0.000  0.80    +0.000  0.80    +0.000
     8x  +0.000  0.40    +0.247  0.40    +0.247
```

- **Shift** (the first number) is where the blend between two mips is centred, relative to where the specification puts it, in mip levels.
  `+0.25` means the sampler reaches the coarser mip a quarter level early, so the texture looks blurrier.
- **Width** (the second) is the level range the blend takes from 10% to 90%.
  A linear blend, as the specifications describe, is 0.80; many drivers sharpen it once anisotropy is on.
- The last column is each API's shift minus the first API's, on the same GPU.

Below the table, lod-check says whether an explicit level and a 4:1 footprint shift the same way, then sums it up in plain words.

## How it measures

A 256 × 256 texture has a mip pyramid whose mip *m* holds the value *m* everywhere.
A trilinear sample therefore reads *m* plus the weight the filter gave mip *m* + 1, and that weight is what is measured.
One compute dispatch sweeps the requested level from 0 to 7 in steps of 1/64, for eight samplers (anisotropy 1 to 16) and three ways of asking for the level:
an explicit level, explicit gradients of a square footprint, and explicit gradients of a 4:1 footprint.

The shift is read where the sample crosses *m* + 0.5, which is the centre of any symmetric blend, so a sharpened blend does not distort it.
The shaders are in [shaders/](shaders/), the analysis in [src/analysis.cc](src/analysis.cc).

## Privacy

**lod-check never connects to the network.**
It has no networking code; [src/](src/) is short enough to confirm that.
It prints its report and writes the same text to `lod-check-results.md` in the current directory, and does nothing else.
The report holds your OS version, each GPU's name, PCI vendor and device id, the driver version and the measurements — no hostname, user name or serial number.

## Share your results

We would love to see results from other GPUs, drivers and operating systems, especially where they disagree with ours.
If you want to share yours, paste `lod-check-results.md` into a new post in [Discussions → Results](https://github.com/solidean/lod-check/discussions/categories/results).
Sharing is entirely up to you, and the file is plain text you can read first.

## Options

```
--out <file>     where to write the report (default: lod-check-results.md)
--no-file        print the report only
--curves <file>  also write every measured curve as CSV, for plotting
--software       include software rasterizers (WARP, lavapipe, llvmpipe)
```

## Build

CMake 3.21 or newer and a C++20 compiler.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

- **Windows** needs the Windows SDK, which comes with Visual Studio.
- **Linux** needs nothing beyond the compiler: the Vulkan loader is opened at run time, and the headers are fetched when no Vulkan SDK is installed.
- **macOS** needs Xcode's command-line tools.

The Vulkan shader is committed as SPIR-V; `uv run tools/compile-spirv.py` rebuilds it from [shaders/probe.comp](shaders/probe.comp).

## In a renderer

lod-check is the minimal version.
The same measurement, with images of the difference, lives in [shaped-core](https://github.com/solidean/shaped-core) as `lod-probe`, beside the sampler correction it motivated.
shaped-core is our open-source, MIT-licensed foundation for rendering and geometry.

## License

MIT, see [LICENSE](LICENSE).
