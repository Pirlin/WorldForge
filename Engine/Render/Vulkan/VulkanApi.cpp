#include "Engine/Render/Vulkan/VulkanApi.h"

#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace worldforge::vk {

bool Api::loadLibrary()
{
#ifdef _WIN32
    auto *module = LoadLibraryW(L"vulkan-1.dll");
    if (!module)
        return false;
    library_ = module;
    return loadGlobal(reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module,"vkGetInstanceProcAddr")));
#else
    library_ = dlopen("libvulkan.so.1",RTLD_NOW | RTLD_LOCAL);
    return library_ && loadGlobal(reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library_,"vkGetInstanceProcAddr")));
#endif
}

bool Api::loadGlobal(PFN_vkGetInstanceProcAddr getInstanceProcAddr)
{
    vkGetInstanceProcAddr = getInstanceProcAddr;
    if (!vkGetInstanceProcAddr)
        return false;
#define WF_VK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(VK_NULL_HANDLE,#name));
    WF_VK_GLOBAL_FUNCTIONS(WF_VK_LOAD)
#undef WF_VK_LOAD
    return vkCreateInstance && vkEnumerateInstanceExtensionProperties;
}

bool Api::loadInstance(VkInstance instance)
{
    if (!vkGetInstanceProcAddr || !instance)
        return false;
#define WF_VK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance,#name));
    WF_VK_INSTANCE_FUNCTIONS(WF_VK_LOAD)
#undef WF_VK_LOAD
    return vkEnumeratePhysicalDevices && vkCreateDevice && vkGetDeviceProcAddr;
}

bool Api::loadDevice(VkDevice device)
{
    if (!vkGetDeviceProcAddr || !device)
        return false;
    bool complete = true;
#define WF_VK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device,#name)); \
    complete &= name != nullptr || std::string_view(#name).ends_with("KHR") || std::string_view(#name).ends_with("EXT");
    WF_VK_DEVICE_FUNCTIONS(WF_VK_LOAD)
#undef WF_VK_LOAD
    return complete;
}

void Device::init(const Api &functions,VkPhysicalDevice physicalDevice,VkDevice logicalDevice)
{
    api = &functions;
    physical = physicalDevice;
    device = logicalDevice;
    functions.vkGetPhysicalDeviceMemoryProperties(physical,&memory);
}

uint32_t Device::memoryType(uint32_t allowed,VkMemoryPropertyFlags flags) const
{
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((allowed & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return memory.memoryTypeCount;
}

bool Device::supports(VkFormat format,VkFormatFeatureFlags features) const
{
    VkFormatProperties properties{};
    api->vkGetPhysicalDeviceFormatProperties(physical,format,&properties);
    return (properties.optimalTilingFeatures & features) == features;
}

bool Device::supportsCompute(uint32_t family) const
{
    uint32_t count = 0;
    api->vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    api->vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,queues.data());
    return family < count && (queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT);
}

VkShaderModule Device::shader(std::span<const uint32_t> spirv) const
{
    if (spirv.empty())
        return VK_NULL_HANDLE;
    const VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,nullptr,0,
        spirv.size_bytes(),spirv.data()};
    VkShaderModule module{};
    return api->vkCreateShaderModule(device,&info,nullptr,&module) == VK_SUCCESS ? module : VK_NULL_HANDLE;
}

bool Buffer::create(const Device &device,VkDeviceSize bytes,VkBufferUsageFlags usage)
{
    const auto &vk = *device.api;
    const VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,nullptr,0,bytes,usage,
        VK_SHARING_MODE_EXCLUSIVE,0,nullptr};
    if (vk.vkCreateBuffer(device.device,&info,nullptr,&buffer) != VK_SUCCESS)
        return false;
    VkMemoryRequirements requirements{};
    vk.vkGetBufferMemoryRequirements(device.device,buffer,&requirements);
    const auto type = device.memoryType(requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    const VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,nullptr,requirements.size,type};
    if (type == device.memory.memoryTypeCount || vk.vkAllocateMemory(device.device,&allocation,nullptr,&memory) != VK_SUCCESS
        || vk.vkBindBufferMemory(device.device,buffer,memory,0) != VK_SUCCESS
        || vk.vkMapMemory(device.device,memory,0,VK_WHOLE_SIZE,0,&mapped) != VK_SUCCESS) {
        destroy(device);
        return false;
    }
    size = bytes;
    return true;
}

void Buffer::destroy(const Device &device)
{
    if (!device.api)
        return;
    const auto &vk = *device.api;
    if (buffer)
        vk.vkDestroyBuffer(device.device,buffer,nullptr);
    if (memory)
        vk.vkFreeMemory(device.device,memory,nullptr);
    *this = {};
}

bool Image::create(const Device &device,VkFormat format,VkExtent2D extent,VkImageUsageFlags usage,
                   VkImageAspectFlags aspect,uint32_t mipLevels)
{
    const auto &vk = *device.api;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width,extent.height,1};
    info.mipLevels = mipLevels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    if (vk.vkCreateImage(device.device,&info,nullptr,&image) != VK_SUCCESS)
        return false;
    VkMemoryRequirements requirements{};
    vk.vkGetImageMemoryRequirements(device.device,image,&requirements);
    const auto type = device.memoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    const VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,nullptr,requirements.size,type};
    if (type == device.memory.memoryTypeCount || vk.vkAllocateMemory(device.device,&allocation,nullptr,&memory) != VK_SUCCESS
        || vk.vkBindImageMemory(device.device,image,memory,0) != VK_SUCCESS) {
        destroy(device);
        return false;
    }
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {aspect,0,mipLevels,0,1};
    if (vk.vkCreateImageView(device.device,&viewInfo,nullptr,&view) != VK_SUCCESS) {
        destroy(device);
        return false;
    }
    return true;
}

void Image::destroy(const Device &device)
{
    if (!device.api)
        return;
    const auto &vk = *device.api;
    if (view)
        vk.vkDestroyImageView(device.device,view,nullptr);
    if (image)
        vk.vkDestroyImage(device.device,image,nullptr);
    if (memory)
        vk.vkFreeMemory(device.device,memory,nullptr);
    *this = {};
}

} // namespace worldforge::vk
