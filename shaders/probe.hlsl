// lod-check's probe for D3D12, compiled at run time as cs_5_0.
//
// The pyramid's mip m holds m, so a sample reads m plus the weight the filter gave mip m + 1.
// Thread x is a step of the level sweep, group y the sampling path, group z the sampler.
// The constants match src/lod_check.hh.

Texture2D<float> pyramid : register(t0);
RWStructuredBuffer<float> samples : register(u0);

// Trilinear, repeating, no bias; anisotropy 1, 2, 3, 4, 6, 8, 12, 16.
SamplerState s0 : register(s0);
SamplerState s1 : register(s1);
SamplerState s2 : register(s2);
SamplerState s3 : register(s3);
SamplerState s4 : register(s4);
SamplerState s5 : register(s5);
SamplerState s6 : register(s6);
SamplerState s7 : register(s7);

static const float anisotropies[8] = {1, 2, 3, 4, 6, 8, 12, 16};

float sample_level(uint s, float2 uv, float level)
{
    switch (s)
    {
    case 0: return pyramid.SampleLevel(s0, uv, level);
    case 1: return pyramid.SampleLevel(s1, uv, level);
    case 2: return pyramid.SampleLevel(s2, uv, level);
    case 3: return pyramid.SampleLevel(s3, uv, level);
    case 4: return pyramid.SampleLevel(s4, uv, level);
    case 5: return pyramid.SampleLevel(s5, uv, level);
    case 6: return pyramid.SampleLevel(s6, uv, level);
    default: return pyramid.SampleLevel(s7, uv, level);
    }
}

float sample_grad(uint s, float2 uv, float2 dx, float2 dy)
{
    switch (s)
    {
    case 0: return pyramid.SampleGrad(s0, uv, dx, dy);
    case 1: return pyramid.SampleGrad(s1, uv, dx, dy);
    case 2: return pyramid.SampleGrad(s2, uv, dx, dy);
    case 3: return pyramid.SampleGrad(s3, uv, dx, dy);
    case 4: return pyramid.SampleGrad(s4, uv, dx, dy);
    case 5: return pyramid.SampleGrad(s5, uv, dx, dy);
    case 6: return pyramid.SampleGrad(s6, uv, dx, dy);
    default: return pyramid.SampleGrad(s7, uv, dx, dy);
    }
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint steps = 7 * 64 + 1;
    if (id.x >= steps)
        return;

    const float level = id.x / 64.0;
    const float2 uv = float2(0.375, 0.625);
    // One texel of the requested level, in uv: the footprint every path asks for.
    const float extent = exp2(level) / 256.0;

    float result;
    if (id.y == 0)
        result = sample_level(id.z, uv, level);
    else if (id.y == 1)
        result = sample_grad(id.z, uv, float2(extent, 0), float2(0, extent));
    else
    {
        // A 4:1 or 16:1 footprint whose level is still `level`: the specification divides the major axis by
        // min(ratio, anisotropy).
        const float ratio = id.y == 2 ? 4.0 : 16.0;
        const float major = extent * min(ratio, anisotropies[id.z]);
        result = sample_grad(id.z, uv, float2(major, 0), float2(0, major / ratio));
    }
    samples[(id.z * 4 + id.y) * steps + id.x] = result;
}
