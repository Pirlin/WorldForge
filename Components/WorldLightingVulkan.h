#pragma once

#include <QVulkanFunctions>
#include <QVulkanWindow>
#include <vector>
#include <span>

struct LightingUniform {
    float lightMatrix[16];
    float fogCenterRadius[4];
    float settings[4];
};
static_assert(sizeof(LightingUniform) == 96);

class WorldLightingVulkan {
public:
    static constexpr uint32_t shadowSize = 1536;
    static constexpr uint32_t textureCapacity = 2048 * 2048;
    static constexpr uint32_t worldTextureCapacity = 1024 * 1024;

    bool create(QVulkanWindow &window,QVulkanDeviceFunctions *vk,bool modeling);
    void destroy();
    bool upload(int frame,const LightingUniform &data);
    bool uploadTextures(int frame,std::span<const uint32_t> pixels,uint64_t revision);
    VkDescriptorSet descriptor(int frame) const { return frames_[frame].set; }
    VkFramebuffer framebuffer(int frame) const { return frames_[frame].framebuffer; }
    VkRenderPass pass() const { return pass_; }
    VkDescriptorSetLayout layout() const { return layout_; }

private:
    struct Frame {
        VkImage image{};
        VkDeviceMemory imageMemory{};
        VkImageView view{};
        VkFramebuffer framebuffer{};
        VkBuffer uniform{};
        VkDeviceMemory uniformMemory{};
        VkBuffer textures{};
        VkDeviceMemory textureMemory{};
        uint64_t textureRevision{};
        VkDescriptorSet set{};
    };
    bool createFrame(Frame &frame,VkFormat format);
    bool allocate(VkDeviceMemory &memory,VkMemoryRequirements requirements,VkMemoryPropertyFlags properties);
    QVulkanDeviceFunctions *vk_ = nullptr;
    VkDevice device_{};
    VkPhysicalDeviceMemoryProperties memory_{};
    VkRenderPass pass_{};
    VkDescriptorSetLayout layout_{};
    VkDescriptorPool pool_{};
    VkSampler sampler_{};
    VkDeviceSize textureBytes_{};
    std::vector<Frame> frames_;
};
