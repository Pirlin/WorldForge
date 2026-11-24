#pragma once

// Vulkan entry points loaded at run time, so the Player and the editor share one dispatch table
// without linking the Vulkan loader. The Player loads vulkan-1.dll itself; the editor passes
// the vkGetInstanceProcAddr of the instance Qt created.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>
#include <vector>

namespace worldforge::vk {

#define WF_VK_GLOBAL_FUNCTIONS(X) \
    X(vkCreateInstance) X(vkEnumerateInstanceVersion) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceLayerProperties)

#define WF_VK_INSTANCE_FUNCTIONS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceFeatures) X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceProperties2) X(vkCreateDevice) \
    X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties) X(vkDestroySurfaceKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)

#define WF_VK_DEVICE_FUNCTIONS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueueWaitIdle) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) \
    X(vkQueuePresentKHR) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkFreeCommandBuffers) X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) \
    X(vkDestroySemaphore) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) \
    X(vkCmdNextSubpass) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) \
    X(vkCmdClearAttachments) \
    X(vkCmdPushConstants) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdDrawMeshTasksEXT) X(vkCmdDrawIndirect) X(vkCmdDrawIndirectCountKHR) X(vkCmdDispatch) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdPipelineBarrier) \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdBlitImage) \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults)

struct Api {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#define WF_VK_MEMBER(name) PFN_##name name = nullptr;
    WF_VK_GLOBAL_FUNCTIONS(WF_VK_MEMBER)
    WF_VK_INSTANCE_FUNCTIONS(WF_VK_MEMBER)
    WF_VK_DEVICE_FUNCTIONS(WF_VK_MEMBER)
#undef WF_VK_MEMBER

    // Loads vulkan-1.dll. Returns false when no Vulkan driver is installed.
    bool loadLibrary();
    bool loadGlobal(PFN_vkGetInstanceProcAddr getInstanceProcAddr);
    // Instance functions; surface functions stay null when the instance lacks VK_KHR_surface.
    bool loadInstance(VkInstance instance);
    bool loadDevice(VkDevice device);

private:
    void *library_ = nullptr;
};

// A logical device and the helpers both renderers use to create resources.
struct Device {
    const Api *api = nullptr;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory{};

    void init(const Api &functions,VkPhysicalDevice physicalDevice,VkDevice logicalDevice);
    // Returns memoryTypeCount when no type fits.
    uint32_t memoryType(uint32_t allowed,VkMemoryPropertyFlags flags) const;
    bool supports(VkFormat format,VkFormatFeatureFlags features) const;
    bool supportsCompute(uint32_t family) const;
    VkShaderModule shader(std::span<const uint32_t> spirv) const;
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void *mapped = nullptr;

    // Host-visible, coherent and persistently mapped.
    bool create(const Device &device,VkDeviceSize bytes,VkBufferUsageFlags usage);
    void destroy(const Device &device);
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;

    bool create(const Device &device,VkFormat format,VkExtent2D extent,VkImageUsageFlags usage,
                VkImageAspectFlags aspect,uint32_t mipLevels = 1);
    void destroy(const Device &device);
};

} // namespace worldforge::vk
