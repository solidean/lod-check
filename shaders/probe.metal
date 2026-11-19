// lod-check's probe for Metal, the same shader as probe.hlsl, compiled at run time.
//
// The pyramid's mip m holds m, so a sample reads m plus the weight the filter gave mip m + 1.
// Thread x is a step of the level sweep, y the sampling path, z the sampler.

#include <metal_stdlib>
using namespace metal;

constant float anisotropies[8] = {1, 2, 3, 4, 6, 8, 12, 16};

kernel void probe(texture2d<float> pyramid [[texture(0)]],
                  // Trilinear, repeating, no bias; anisotropy 1, 2, 3, 4, 6, 8, 12, 16.
                  array<sampler, 8> samplers [[sampler(0)]],
                  device float* samples [[buffer(0)]],
                  uint3 id [[thread_position_in_grid]])
{
    const uint steps = 7 * 64 + 1;
    if (id.x >= steps)
        return;

    const float lod = float(id.x) / 64.0;
    const float2 uv = float2(0.375, 0.625);
    // One texel of the requested level, in uv: the footprint every path asks for.
    const float extent = exp2(lod) / 256.0;
    const sampler s = samplers[id.z];

    float result;
    if (id.y == 0)
        result = pyramid.sample(s, uv, level(lod)).x;
    else if (id.y == 1)
        result = pyramid.sample(s, uv, gradient2d(float2(extent, 0), float2(0, extent))).x;
    else
    {
        // A 4:1 or 16:1 footprint whose level is still `lod`: the specification divides the major axis by
        // min(ratio, anisotropy).
        const float ratio = id.y == 2 ? 4.0 : 16.0;
        const float major = extent * min(ratio, anisotropies[id.z]);
        result = pyramid.sample(s, uv, gradient2d(float2(major, 0), float2(0, major / ratio))).x;
    }
    samples[(id.z * 4 + id.y) * steps + id.x] = result;
}
