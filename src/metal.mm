// The Metal backend: one compute dispatch per device, its MSL compiled at run time.
//
// Built with ARC; Metal reports no driver version, so the report's OS version stands in for it.

#include "lod_check.hh"
#include "shader_sources.hh"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstring>
#include <format>

namespace lod_check
{
namespace
{
std::vector<float> measure(id<MTLDevice> device, std::string& error)
{
    NSError* ns_error = nil;
    auto const library = [device newLibraryWithSource:@(probe_metal) options:[MTLCompileOptions new] error:&ns_error];
    if (!library)
    {
        error = std::format("compiling probe.metal failed: {}", ns_error.localizedDescription.UTF8String);
        return {};
    }
    auto const function = [library newFunctionWithName:@"probe"];
    auto const pipeline = [device newComputePipelineStateWithFunction:function error:&ns_error];
    if (!pipeline)
    {
        error = std::format("creating the pipeline failed: {}", ns_error.localizedDescription.UTF8String);
        return {};
    }

    auto const texture_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Float
                                                                                 width:pyramid_size
                                                                                height:pyramid_size
                                                                             mipmapped:YES];
    texture_desc.usage = MTLTextureUsageShaderRead;
    auto const texture = [device newTextureWithDescriptor:texture_desc];
    for (auto mip = 0; mip < pyramid_mips; ++mip)
    {
        auto const data = pyramid_mip(mip);
        auto const size = NSUInteger(mip_size(mip));
        [texture replaceRegion:MTLRegionMake2D(0, 0, size, size)
                   mipmapLevel:NSUInteger(mip)
                     withBytes:data.data()
                   bytesPerRow:size * sizeof(std::uint16_t)];
    }

    id<MTLSamplerState> samplers[anisotropy_count];
    for (auto i = 0; i < anisotropy_count; ++i)
    {
        auto const desc = [MTLSamplerDescriptor new];
        desc.minFilter = MTLSamplerMinMagFilterLinear;
        desc.magFilter = MTLSamplerMinMagFilterLinear;
        desc.mipFilter = MTLSamplerMipFilterLinear;
        desc.sAddressMode = MTLSamplerAddressModeRepeat;
        desc.tAddressMode = MTLSamplerAddressModeRepeat;
        desc.maxAnisotropy = NSUInteger(anisotropies[i]);
        samplers[i] = [device newSamplerStateWithDescriptor:desc];
    }

    auto const output_bytes = NSUInteger(anisotropy_count * path_count * sweep_steps) * sizeof(float);
    auto const output = [device newBufferWithLength:output_bytes options:MTLResourceStorageModeShared];

    auto const queue = [device newCommandQueue];
    auto const commands = [queue commandBuffer];
    auto const encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setTexture:texture atIndex:0];
    [encoder setSamplerStates:samplers withRange:NSMakeRange(0, anisotropy_count)];
    [encoder setBuffer:output offset:0 atIndex:0];
    [encoder dispatchThreadgroups:MTLSizeMake(sweep_groups, path_count, anisotropy_count)
            threadsPerThreadgroup:MTLSizeMake(group_size, 1, 1)];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.error)
    {
        error = std::format("the dispatch failed: {}", commands.error.localizedDescription.UTF8String);
        return {};
    }

    auto samples = std::vector<float>(std::size_t(anisotropy_count * path_count * sweep_steps));
    std::memcpy(samples.data(), output.contents, output_bytes);
    return samples;
}
} // namespace

std::vector<gpu_run> run_metal(options const&)
{
    auto runs = std::vector<gpu_run>();
    @autoreleasepool
    {
        auto const devices = MTLCopyAllDevices();
        if (devices.count == 0)
        {
            runs.push_back({.api = "Metal", .error = "no Metal device"});
            return runs;
        }
        for (id<MTLDevice> device in devices)
        {
            auto run = gpu_run{
                .api = "Metal",
                .adapter = device.name.UTF8String,
                .driver = "the OS's",
                .pairing_key = device.registryID,
                .has_pairing_key = true,
            };
            run.samples = measure(device, run.error);
            runs.push_back(std::move(run));
        }
    }
    return runs;
}

std::string os_description()
{
    @autoreleasepool
    {
        auto const version = NSProcessInfo.processInfo.operatingSystemVersion;
        auto const full = NSProcessInfo.processInfo.operatingSystemVersionString;
        return std::format("macOS {}.{}.{} ({})", version.majorVersion, version.minorVersion, version.patchVersion,
                           full.UTF8String);
    }
}
} // namespace lod_check
