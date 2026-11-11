// The D3D12 backend: one compute dispatch per adapter, its HLSL compiled by the d3dcompiler that ships with Windows.

#include "lod_check.hh"
#include "shader_sources.hh"

#include <cstring>
#include <format>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace lod_check
{
namespace
{
struct failure
{
    std::string what;
};

void check(HRESULT hr, char const* what)
{
    if (FAILED(hr))
        throw failure{std::format("{} failed (0x{:08x})", what, std::uint32_t(hr))};
}

std::string narrow(wchar_t const* text)
{
    auto const size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    auto out = std::string(std::size_t(size > 0 ? size - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

/// The user-mode driver version DXGI reports, and NVIDIA's own numbering where it applies.
std::string driver_version(IDXGIAdapter1* adapter, std::uint32_t vendor_id)
{
    auto v = LARGE_INTEGER();
    if (FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v)))
        return "unknown";
    auto const a = HIWORD(v.HighPart), b = LOWORD(v.HighPart), c = HIWORD(v.LowPart), d = LOWORD(v.LowPart);
    auto out = std::format("{}.{}.{}.{}", a, b, c, d);
    if (vendor_id == 0x10de)
    {
        // 32.0.16.1088 is NVIDIA's 610.88: the last digit of the third part, then the fourth.
        auto const nv = (c % 10) * 10000 + d;
        out += std::format(" ({}.{:02})", nv / 100, nv % 100);
    }
    return out;
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    return {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Width = size,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = {1, 0},
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags = flags,
    };
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    auto barrier = D3D12_RESOURCE_BARRIER{.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

std::vector<float> measure(ID3D12Device* device)
{
    // The root signature: the pyramid in a table, the output as a root UAV, and one static sampler per anisotropy.
    auto const srv_range = D3D12_DESCRIPTOR_RANGE{
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        .NumDescriptors = 1,
        .BaseShaderRegister = 0,
        .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = 0,
    };
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {1, &srv_range};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor = {0, 0};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samplers[anisotropy_count];
    for (auto i = 0; i < anisotropy_count; ++i)
        samplers[i] = {
            .Filter = anisotropies[i] > 1 ? D3D12_FILTER_ANISOTROPIC : D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            .AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            .AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            .AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            .MipLODBias = 0,
            .MaxAnisotropy = UINT(anisotropies[i]),
            .ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
            .BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK,
            .MinLOD = 0,
            .MaxLOD = D3D12_FLOAT32_MAX,
            .ShaderRegister = UINT(i),
            .RegisterSpace = 0,
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
        };

    auto const root_desc = D3D12_ROOT_SIGNATURE_DESC{
        .NumParameters = 2,
        .pParameters = params,
        .NumStaticSamplers = UINT(anisotropy_count),
        .pStaticSamplers = samplers,
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE,
    };
    ComPtr<ID3DBlob> root_blob, error_blob;
    check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, &error_blob),
          "D3D12SerializeRootSignature");
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(), IID_PPV_ARGS(&root)),
          "CreateRootSignature");

    ComPtr<ID3DBlob> shader;
    if (FAILED(D3DCompile(probe_hlsl, std::strlen(probe_hlsl), "probe.hlsl", nullptr, nullptr, "main", "cs_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &error_blob)))
        throw failure{std::format("compiling probe.hlsl failed: {}",
                                  error_blob ? static_cast<char const*>(error_blob->GetBufferPointer()) : "no message")};

    auto const pipeline_desc = D3D12_COMPUTE_PIPELINE_STATE_DESC{
        .pRootSignature = root.Get(),
        .CS = {shader->GetBufferPointer(), shader->GetBufferSize()},
    };
    ComPtr<ID3D12PipelineState> pipeline;
    check(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline)), "CreateComputePipelineState");

    // The pyramid, and an upload buffer laid out the way CopyTextureRegion wants it.
    auto const default_heap = D3D12_HEAP_PROPERTIES{.Type = D3D12_HEAP_TYPE_DEFAULT};
    auto const upload_heap = D3D12_HEAP_PROPERTIES{.Type = D3D12_HEAP_TYPE_UPLOAD};
    auto const readback_heap = D3D12_HEAP_PROPERTIES{.Type = D3D12_HEAP_TYPE_READBACK};

    auto const texture_desc = D3D12_RESOURCE_DESC{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Width = UINT64(pyramid_size),
        .Height = UINT(pyramid_size),
        .DepthOrArraySize = 1,
        .MipLevels = UINT16(pyramid_mips),
        .Format = DXGI_FORMAT_R16_FLOAT,
        .SampleDesc = {1, 0},
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };
    ComPtr<ID3D12Resource> texture;
    check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)),
          "creating the pyramid");

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[pyramid_mips];
    UINT rows[pyramid_mips];
    UINT64 row_bytes[pyramid_mips];
    UINT64 upload_size = 0;
    device->GetCopyableFootprints(&texture_desc, 0, pyramid_mips, 0, footprints, rows, row_bytes, &upload_size);

    auto const upload_desc = buffer_desc(upload_size);
    ComPtr<ID3D12Resource> upload;
    check(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)),
          "creating the upload buffer");
    std::byte* mapped = nullptr;
    check(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "mapping the upload buffer");
    for (auto mip = 0; mip < pyramid_mips; ++mip)
    {
        auto const data = pyramid_mip(mip);
        auto const width = std::size_t(mip_size(mip));
        for (auto y = 0u; y < rows[mip]; ++y)
            std::memcpy(mapped + footprints[mip].Offset + y * footprints[mip].Footprint.RowPitch,
                        data.data() + y * width, width * sizeof(std::uint16_t));
    }
    upload->Unmap(0, nullptr);

    // The output, and the buffer it is copied to for the CPU.
    auto const output_bytes = UINT64(anisotropy_count * path_count * sweep_steps) * sizeof(float);
    auto const output_desc = buffer_desc(output_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ComPtr<ID3D12Resource> output;
    check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &output_desc,
                                          D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&output)),
          "creating the output buffer");
    auto const readback_desc = buffer_desc(output_bytes);
    ComPtr<ID3D12Resource> readback;
    check(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)),
          "creating the readback buffer");

    auto const heap_desc = D3D12_DESCRIPTOR_HEAP_DESC{
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = 1,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    };
    ComPtr<ID3D12DescriptorHeap> heap;
    check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap");
    auto srv = D3D12_SHADER_RESOURCE_VIEW_DESC{
        .Format = DXGI_FORMAT_R16_FLOAT,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
    };
    srv.Texture2D.MipLevels = UINT(pyramid_mips);
    device->CreateShaderResourceView(texture.Get(), &srv, heap->GetCPUDescriptorHandleForHeapStart());

    // Record: upload, dispatch, read back.
    auto const queue_desc = D3D12_COMMAND_QUEUE_DESC{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAlloc"
                                                                                                    "ator");
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
          "CreateCommandList");

    for (auto mip = 0; mip < pyramid_mips; ++mip)
    {
        auto dst
            = D3D12_TEXTURE_COPY_LOCATION{.pResource = texture.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst.SubresourceIndex = UINT(mip);
        auto src
            = D3D12_TEXTURE_COPY_LOCATION{.pResource = upload.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint = footprints[mip];
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    D3D12_RESOURCE_BARRIER const before_dispatch[] = {
        transition(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        transition(output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    list->ResourceBarrier(2, before_dispatch);

    list->SetPipelineState(pipeline.Get());
    list->SetComputeRootSignature(root.Get());
    ID3D12DescriptorHeap* heaps[] = {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
    list->Dispatch(UINT(sweep_groups), UINT(path_count), UINT(anisotropy_count));

    auto const after_dispatch
        = transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &after_dispatch);
    list->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, output_bytes);
    check(list->Close(), "closing the command list");

    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    check(queue->Signal(fence.Get(), 1), "signalling the fence");
    auto const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(fence->SetEventOnCompletion(1, event), "waiting for the GPU");
    WaitForSingleObject(event, INFINITE);
    CloseHandle(event);
    check(device->GetDeviceRemovedReason(), "the dispatch");

    auto samples = std::vector<float>(std::size_t(anisotropy_count * path_count * sweep_steps));
    auto const read_range = D3D12_RANGE{0, SIZE_T(output_bytes)};
    void* result = nullptr;
    check(readback->Map(0, &read_range, &result), "mapping the readback buffer");
    std::memcpy(samples.data(), result, output_bytes);
    auto const no_write = D3D12_RANGE{0, 0};
    readback->Unmap(0, &no_write);
    return samples;
}
} // namespace

std::vector<gpu_run> run_d3d12(options const& opts)
{
    auto runs = std::vector<gpu_run>();
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
    {
        runs.push_back({.api = "D3D12", .error = "DXGI is unavailable"});
        return runs;
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (auto i = 0u; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        auto desc = DXGI_ADAPTER_DESC1();
        adapter->GetDesc1(&desc);
        auto const software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        if (software && !opts.software)
            continue;

        auto run = gpu_run{
            .api = "D3D12",
            .adapter = narrow(desc.Description),
            .vendor_id = desc.VendorId,
            .device_id = desc.DeviceId,
            .driver = driver_version(adapter.Get(), desc.VendorId),
            .pairing_key = (std::uint64_t(std::uint32_t(desc.AdapterLuid.HighPart)) << 32) | desc.AdapterLuid.LowPart,
            .has_pairing_key = true,
            .software = software,
        };
        ComPtr<ID3D12Device> device;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
            run.error = "no D3D12 device at feature level 11_0";
        else
        {
            try
            {
                run.samples = measure(device.Get());
            }
            catch (failure const& f)
            {
                run.error = f.what;
            }
        }
        runs.push_back(std::move(run));
    }
    return runs;
}
} // namespace lod_check
