# lod-check

On NVIDIA, the same anisotropic sampler reads up to a quarter mip level blurrier on Vulkan than on D3D12.
lod-check measures where your GPU blends between mip levels, on every graphics API it has (D3D12 and Vulkan on Windows, Vulkan on Linux, Metal on macOS), and compares that to the spec.

## Run

Grab a build from [Releases](https://github.com/solidean/lod-check/releases), unzip, run `lod-check`.
It takes a second or two.

The binaries are not signed, so Windows SmartScreen will warn (*More info* → *Run anyway*) and macOS needs right-click → *Open*.
Each release is built by [CI](.github/workflows/build.yml) from the tagged commit; check a download against `SHA256SUMS` or with `gh attestation verify <zip> --repo solidean/lod-check`.

```
  aniso  D3D12           Vulkan          Vulkan - D3D12
     1x  +0.000  0.80    +0.000  0.80    +0.000
     8x  +0.000  0.40    +0.247  0.40    +0.247
```

The first number per API is the shift in mip levels from where the spec centers the blend (positive is blurrier).
The second is the blend's width from 10% to 90%; a linear blend is 0.80.

The report is also written to `lod-check-results.md`.
If you want to share it, post it in [Discussions → Results](https://github.com/solidean/lod-check/discussions/categories/results).
The tool has no network code; the report contains the OS version, GPU names and ids, driver versions and the measurements.

Options: `--out <file>`, `--no-file`, `--curves <file>` (every curve as CSV), `--software` (include WARP, lavapipe, llvmpipe).

## How it works

Mip *m* of a 256 × 256 texture holds the value *m*, so a trilinear sample returns *m* plus the weight of mip *m* + 1.
One compute dispatch sweeps the requested level in 1/64 steps for anisotropy 1 to 16, using an explicit level and explicit gradients for a square and a 4:1 footprint.
The shift is read where the sample crosses *m* + 0.5.
See [shaders/](shaders/) and [src/analysis.cc](src/analysis.cc).

## Build

CMake 3.21+ and a C++20 compiler; on Windows the Windows SDK, on macOS the Xcode command-line tools.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The Vulkan loader is opened at run time, so Linux needs no SDK.
`uv run tools/compile-spirv.py` regenerates the committed SPIR-V from [shaders/probe.comp](shaders/probe.comp).

The same measurement, with images, is `lod-probe` in [shaped-core](https://github.com/solidean/shaped-core), next to the sampler fix.

MIT licensed.
