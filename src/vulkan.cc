// The Vulkan backend: one compute dispatch per physical device.
//
// The loader is opened at run time, so the binary starts on a machine without one and says so.
// The SPIR-V is committed in shaders/probe.comp.spv.inc, built from shaders/probe.comp by tools/compile-spirv.py.

#include "lod_check.hh"

#include <algorithm>
#include <cstring>
#include <format>
#include <functional>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace lod_check
{
namespace
{
constexpr std::uint32_t probe_spirv[] = {
#include "../shaders/probe.comp.spv.inc"
};

struct failure
{
    std::string what;
};

void check(VkResult result, char const* what)
{
    if (result != VK_SUCCESS)
        throw failure{std::format("{} failed (VkResult {})", what, int(result))};
}

#define LOD_CHECK_INSTANCE_FUNCTIONS(X)         \
    X(vkDestroyInstance)                        \
    X(vkEnumeratePhysicalDevices)               \
    X(vkGetPhysicalDeviceProperties)            \
    X(vkGetPhysicalDeviceProperties2)           \
    X(vkGetPhysicalDeviceFeatures)              \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties)      \
    X(vkCreateDevice)                           \
    X(vkGetDeviceProcAddr)

#define LOD_CHECK_DEVICE_FUNCTIONS(X) \
    X(vkDestroyDevice)                \
    X(vkGetDeviceQueue)               \
    X(vkCreateBuffer)                 \
    X(vkDestroyBuffer)                \
    X(vkCreateImage)                  \
    X(vkDestroyImage)                 \
    X(vkGetBufferMemoryRequirements)  \
    X(vkGetImageMemoryRequirements)   \
    X(vkAllocateMemory)               \
    X(vkFreeMemory)                   \
    X(vkBindBufferMemory)             \
    X(vkBindImageMemory)              \
    X(vkMapMemory)                    \
    X(vkUnmapMemory)                  \
    X(vkCreateImageView)              \
    X(vkDestroyImageView)             \
    X(vkCreateSampler)                \
    X(vkDestroySampler)               \
    X(vkCreateDescriptorSetLayout)    \
    X(vkDestroyDescriptorSetLayout)   \
    X(vkCreatePipelineLayout)         \
    X(vkDestroyPipelineLayout)        \
    X(vkCreateShaderModule)           \
    X(vkDestroyShaderModule)          \
    X(vkCreateComputePipelines)       \
    X(vkDestroyPipeline)              \
    X(vkCreateDescriptorPool)         \
    X(vkDestroyDescriptorPool)        \
    X(vkAllocateDescriptorSets)       \
    X(vkUpdateDescriptorSets)         \
    X(vkCreateCommandPool)            \
    X(vkDestroyCommandPool)           \
    X(vkAllocateCommandBuffers)       \
    X(vkBeginCommandBuffer)           \
    X(vkEndCommandBuffer)             \
    X(vkCmdPipelineBarrier)           \
    X(vkCmdCopyBufferToImage)         \
    X(vkCmdBindPipeline)              \
    X(vkCmdBindDescriptorSets)        \
    X(vkCmdDispatch)                  \
    X(vkCreateFence)                  \
    X(vkDestroyFence)                 \
    X(vkQueueSubmit)                  \
    X(vkWaitForFences)

#define LOD_CHECK_DECLARE(name) PFN_##name name = nullptr;

struct instance_functions
{
    LOD_CHECK_INSTANCE_FUNCTIONS(LOD_CHECK_DECLARE)
};

struct device_functions
{
    LOD_CHECK_DEVICE_FUNCTIONS(LOD_CHECK_DECLARE)
};

/// Destroys what was created, in reverse, however the measurement ends.
struct cleanup
{
    std::vector<std::function<void()>> steps;
    ~cleanup()
    {
        for (auto it = steps.rbegin(); it != steps.rend(); ++it)
            (*it)();
    }
    void add(std::function<void()> step) { steps.push_back(std::move(step)); }
};

PFN_vkGetInstanceProcAddr load_loader()
{
#if defined(_WIN32)
    auto const library = LoadLibraryA("vulkan-1.dll");
    return library ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(library, "vkGetInstanceProcAddr"))
                   : nullptr;
#else
    auto library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library)
        library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    return library ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr")) : nullptr;
#endif
}

/// The driver as the driver names itself, falling back to decoding driverVersion by vendor convention.
std::string driver_version(VkPhysicalDeviceProperties const& props, VkPhysicalDeviceDriverProperties const* driver)
{
    if (driver && driver->driverInfo[0] != '\0')
        return std::format("{} {}", driver->driverName, driver->driverInfo);
    auto const v = props.driverVersion;
    if (props.vendorID == 0x10de)
        return std::format("{}.{:02}", (v >> 22) & 0x3ff, (v >> 14) & 0xff);
    return std::format("{}.{}.{}", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}

std::uint32_t find_memory(VkPhysicalDeviceMemoryProperties const& memory,
                          std::uint32_t allowed,
                          VkMemoryPropertyFlags wanted)
{
    for (auto i = 0u; i < memory.memoryTypeCount; ++i)
        if ((allowed & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted)
            return i;
    throw failure{"no suitable memory type"};
}

std::vector<float> measure(instance_functions const& vki, VkPhysicalDevice physical, VkInstance instance)
{
    auto done = cleanup();

    auto features = VkPhysicalDeviceFeatures();
    vki.vkGetPhysicalDeviceFeatures(physical, &features);
    if (!features.samplerAnisotropy)
        throw failure{"the device does not support anisotropic filtering"};
    auto props = VkPhysicalDeviceProperties();
    vki.vkGetPhysicalDeviceProperties(physical, &props);

    auto family_count = 0u;
    vki.vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
    auto families = std::vector<VkQueueFamilyProperties>(family_count);
    vki.vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
    auto const family_it = std::find_if(families.begin(), families.end(), [](VkQueueFamilyProperties const& f)
                                        { return (f.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0; });
    if (family_it == families.end())
        throw failure{"no compute queue"};
    auto const family = std::uint32_t(family_it - families.begin());

    auto const priority = 1.0f;
    auto const queue_info = VkDeviceQueueCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    auto const enabled = VkPhysicalDeviceFeatures{.samplerAnisotropy = VK_TRUE};
    auto const device_info = VkDeviceCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .pEnabledFeatures = &enabled,
    };
    VkDevice device = VK_NULL_HANDLE;
    check(vki.vkCreateDevice(physical, &device_info, nullptr, &device), "vkCreateDevice");

    auto vk = device_functions();
#define LOD_CHECK_LOAD_DEVICE(name) vk.name = reinterpret_cast<PFN_##name>(vki.vkGetDeviceProcAddr(device, #name));
    LOD_CHECK_DEVICE_FUNCTIONS(LOD_CHECK_LOAD_DEVICE)
    done.add([=] { vk.vkDestroyDevice(device, nullptr); });

    auto memory = VkPhysicalDeviceMemoryProperties();
    vki.vkGetPhysicalDeviceMemoryProperties(physical, &memory);

    auto make_buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, void** mapped)
    {
        auto const info = VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VkBuffer buffer = VK_NULL_HANDLE;
        check(vk.vkCreateBuffer(device, &info, nullptr, &buffer), "vkCreateBuffer");
        done.add([vk, device, buffer] { vk.vkDestroyBuffer(device, buffer, nullptr); });
        auto reqs = VkMemoryRequirements();
        vk.vkGetBufferMemoryRequirements(device, buffer, &reqs);
        auto const alloc = VkMemoryAllocateInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = reqs.size,
            .memoryTypeIndex = find_memory(memory, reqs.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
        };
        VkDeviceMemory mem = VK_NULL_HANDLE;
        check(vk.vkAllocateMemory(device, &alloc, nullptr, &mem), "vkAllocateMemory");
        done.add([vk, device, mem] { vk.vkFreeMemory(device, mem, nullptr); });
        check(vk.vkBindBufferMemory(device, buffer, mem, 0), "vkBindBufferMemory");
        check(vk.vkMapMemory(device, mem, 0, VK_WHOLE_SIZE, 0, mapped), "vkMapMemory");
        return buffer;
    };

    // The pyramid's mips, packed one after another into a staging buffer.
    VkBufferImageCopy copies[pyramid_mips];
    auto staging_size = VkDeviceSize(0);
    for (auto mip = 0; mip < pyramid_mips; ++mip)
    {
        auto const size = std::uint32_t(mip_size(mip));
        copies[mip] = VkBufferImageCopy{
            .bufferOffset = staging_size,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, std::uint32_t(mip), 0, 1},
            .imageExtent = {size, size, 1},
        };
        staging_size += (VkDeviceSize(size) * size * sizeof(std::uint16_t) + 15) & ~VkDeviceSize(15);
    }
    void* staging_data = nullptr;
    auto const staging = make_buffer(staging_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging_data);
    for (auto mip = 0; mip < pyramid_mips; ++mip)
    {
        auto const data = pyramid_mip(mip);
        std::memcpy(static_cast<std::byte*>(staging_data) + copies[mip].bufferOffset, data.data(),
                    data.size() * sizeof(std::uint16_t));
    }

    auto const output_bytes = VkDeviceSize(anisotropy_count * path_count * sweep_steps) * sizeof(float);
    void* output_data = nullptr;
    auto const output = make_buffer(output_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &output_data);

    auto const image_info = VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R16_SFLOAT,
        .extent = {std::uint32_t(pyramid_size), std::uint32_t(pyramid_size), 1},
        .mipLevels = std::uint32_t(pyramid_mips),
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image = VK_NULL_HANDLE;
    check(vk.vkCreateImage(device, &image_info, nullptr, &image), "vkCreateImage");
    done.add([=] { vk.vkDestroyImage(device, image, nullptr); });
    auto image_reqs = VkMemoryRequirements();
    vk.vkGetImageMemoryRequirements(device, image, &image_reqs);
    auto const image_alloc = VkMemoryAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = image_reqs.size,
        .memoryTypeIndex = find_memory(memory, image_reqs.memoryTypeBits, 0),
    };
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    check(vk.vkAllocateMemory(device, &image_alloc, nullptr, &image_memory), "vkAllocateMemory");
    done.add([=] { vk.vkFreeMemory(device, image_memory, nullptr); });
    check(vk.vkBindImageMemory(device, image, image_memory, 0), "vkBindImageMemory");

    auto const all_mips = VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, std::uint32_t(pyramid_mips), 0, 1};
    auto const view_info = VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R16_SFLOAT,
        .subresourceRange = all_mips,
    };
    VkImageView view = VK_NULL_HANDLE;
    check(vk.vkCreateImageView(device, &view_info, nullptr, &view), "vkCreateImageView");
    done.add([=] { vk.vkDestroyImageView(device, view, nullptr); });

    // One sampler per anisotropy, clamped to what the device allows.
    VkSampler samplers[anisotropy_count];
    for (auto i = 0; i < anisotropy_count; ++i)
    {
        auto const a = float(anisotropies[i]);
        auto const info = VkSamplerCreateInfo{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .mipLodBias = 0,
            .anisotropyEnable = a > 1 ? VK_TRUE : VK_FALSE,
            .maxAnisotropy = std::min(a, props.limits.maxSamplerAnisotropy),
            .minLod = 0,
            .maxLod = VK_LOD_CLAMP_NONE,
        };
        check(vk.vkCreateSampler(device, &info, nullptr, &samplers[i]), "vkCreateSampler");
        done.add([vk, device, s = samplers[i]] { vk.vkDestroySampler(device, s, nullptr); });
    }

    VkDescriptorSetLayoutBinding const bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER, std::uint32_t(anisotropy_count), VK_SHADER_STAGE_COMPUTE_BIT, samplers},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    auto const set_layout_info = VkDescriptorSetLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3,
        .pBindings = bindings,
    };
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    check(vk.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &set_layout), "vkCreateDescriptorSetLayou"
                                                                                          "t");
    done.add([=] { vk.vkDestroyDescriptorSetLayout(device, set_layout, nullptr); });

    auto const layout_info = VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout,
    };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    check(vk.vkCreatePipelineLayout(device, &layout_info, nullptr, &layout), "vkCreatePipelineLayout");
    done.add([=] { vk.vkDestroyPipelineLayout(device, layout, nullptr); });

    auto const module_info = VkShaderModuleCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(probe_spirv),
        .pCode = probe_spirv,
    };
    VkShaderModule module = VK_NULL_HANDLE;
    check(vk.vkCreateShaderModule(device, &module_info, nullptr, &module), "vkCreateShaderModule");
    done.add([=] { vk.vkDestroyShaderModule(device, module, nullptr); });

    auto const pipeline_info = VkComputePipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = module,
            .pName = "main",
        },
        .layout = layout,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    check(vk.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline), "vkCreateComputeP"
                                                                                                      "ipelines");
    done.add([=] { vk.vkDestroyPipeline(device, pipeline, nullptr); });

    VkDescriptorPoolSize const pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1},
        {VK_DESCRIPTOR_TYPE_SAMPLER, std::uint32_t(anisotropy_count)},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
    };
    auto const pool_info = VkDescriptorPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 3,
        .pPoolSizes = pool_sizes,
    };
    VkDescriptorPool pool = VK_NULL_HANDLE;
    check(vk.vkCreateDescriptorPool(device, &pool_info, nullptr, &pool), "vkCreateDescriptorPool");
    done.add([=] { vk.vkDestroyDescriptorPool(device, pool, nullptr); });

    auto const set_info = VkDescriptorSetAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &set_layout,
    };
    VkDescriptorSet set = VK_NULL_HANDLE;
    check(vk.vkAllocateDescriptorSets(device, &set_info, &set), "vkAllocateDescriptorSets");

    auto const image_desc
        = VkDescriptorImageInfo{.imageView = view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    auto const buffer_desc = VkDescriptorBufferInfo{.buffer = output, .offset = 0, .range = VK_WHOLE_SIZE};
    VkWriteDescriptorSet const writes[] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = set,
         .dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         .pImageInfo = &image_desc},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = set,
         .dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &buffer_desc},
    };
    vk.vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

    // Record: upload, dispatch, make the writes visible to the host.
    auto const command_pool_info = VkCommandPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = family,
    };
    VkCommandPool command_pool = VK_NULL_HANDLE;
    check(vk.vkCreateCommandPool(device, &command_pool_info, nullptr, &command_pool), "vkCreateCommandPool");
    done.add([=] { vk.vkDestroyCommandPool(device, command_pool, nullptr); });
    auto const command_info = VkCommandBufferAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    check(vk.vkAllocateCommandBuffers(device, &command_info, &cmd), "vkAllocateCommandBuffers");

    auto const begin = VkCommandBufferBeginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    check(vk.vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");

    auto image_barrier = VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = all_mips,
    };
    vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                            nullptr, 1, &image_barrier);
    vk.vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, std::uint32_t(pyramid_mips),
                              copies);
    image_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    image_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    image_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    image_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                            nullptr, 1, &image_barrier);

    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    vk.vkCmdDispatch(cmd, std::uint32_t(sweep_groups), std::uint32_t(path_count), std::uint32_t(anisotropy_count));

    auto const host_barrier = VkMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_barrier,
                            0, nullptr, 0, nullptr);
    check(vk.vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    auto const fence_info = VkFenceCreateInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    check(vk.vkCreateFence(device, &fence_info, nullptr, &fence), "vkCreateFence");
    done.add([=] { vk.vkDestroyFence(device, fence, nullptr); });

    VkQueue queue = VK_NULL_HANDLE;
    vk.vkGetDeviceQueue(device, family, 0, &queue);
    auto const submit = VkSubmitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    check(vk.vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit");
    check(vk.vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");

    auto samples = std::vector<float>(std::size_t(anisotropy_count * path_count * sweep_steps));
    std::memcpy(samples.data(), output_data, output_bytes);
    (void)instance;
    return samples;
}
} // namespace

std::vector<gpu_run> run_vulkan(options const& opts)
{
    auto runs = std::vector<gpu_run>();
    auto const get_instance_proc = load_loader();
    if (!get_instance_proc)
    {
        runs.push_back({.api = "Vulkan", .error = "no Vulkan loader found"});
        return runs;
    }

    // Instance-level properties need 1.1 for device ids and LUIDs, and 1.2 for the driver's own name for its version.
    auto instance_version = VK_API_VERSION_1_0;
    if (auto const enumerate_version
        = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(get_instance_proc(nullptr, "vkEnumerateInstanceVersion")))
        enumerate_version(&instance_version);
    if (instance_version < VK_API_VERSION_1_1)
    {
        runs.push_back({.api = "Vulkan", .error = "the Vulkan loader is older than 1.1"});
        return runs;
    }

    auto const create_instance = reinterpret_cast<PFN_vkCreateInstance>(get_instance_proc(nullptr, "vkCreateInstance"));
    auto const app = VkApplicationInfo{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "lod-check",
        .apiVersion = std::min(instance_version, VK_API_VERSION_1_2),
    };
    auto const instance_info = VkInstanceCreateInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance instance = VK_NULL_HANDLE;
    if (!create_instance || create_instance(&instance_info, nullptr, &instance) != VK_SUCCESS)
    {
        runs.push_back({.api = "Vulkan", .error = "vkCreateInstance failed"});
        return runs;
    }

    auto vki = instance_functions();
#define LOD_CHECK_LOAD_INSTANCE(name) vki.name = reinterpret_cast<PFN_##name>(get_instance_proc(instance, #name));
    LOD_CHECK_INSTANCE_FUNCTIONS(LOD_CHECK_LOAD_INSTANCE)

    auto count = 0u;
    vki.vkEnumeratePhysicalDevices(instance, &count, nullptr);
    auto physicals = std::vector<VkPhysicalDevice>(count);
    vki.vkEnumeratePhysicalDevices(instance, &count, physicals.data());

    for (auto const physical : physicals)
    {
        auto basic = VkPhysicalDeviceProperties();
        vki.vkGetPhysicalDeviceProperties(physical, &basic);
        auto const software = basic.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
        if (software && !opts.software)
            continue;

        auto const has_driver_props = app.apiVersion >= VK_API_VERSION_1_2 && basic.apiVersion >= VK_API_VERSION_1_2;
        auto driver = VkPhysicalDeviceDriverProperties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        auto ids = VkPhysicalDeviceIDProperties{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
            .pNext = has_driver_props ? &driver : nullptr,
        };
        auto props = VkPhysicalDeviceProperties2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &ids};
        vki.vkGetPhysicalDeviceProperties2(physical, &props);

        auto luid = std::uint64_t(0);
        std::memcpy(&luid, ids.deviceLUID, sizeof(luid));
        auto run = gpu_run{
            .api = "Vulkan",
            .adapter = props.properties.deviceName,
            .vendor_id = props.properties.vendorID,
            .device_id = props.properties.deviceID,
            .driver = driver_version(props.properties, has_driver_props ? &driver : nullptr),
            .pairing_key = luid,
            .has_pairing_key = ids.deviceLUIDValid == VK_TRUE,
            .software = software,
        };
        try
        {
            run.samples = measure(vki, physical, instance);
        }
        catch (failure const& f)
        {
            run.error = f.what;
        }
        runs.push_back(std::move(run));
    }

    vki.vkDestroyInstance(instance, nullptr);
    return runs;
}
} // namespace lod_check
