#pragma once

#include "Engine/Render/ShadowCascades.h"

#include <QVulkanFunctions>
#include <QVulkanWindow>
#include <vector>
#include <span>

// Engine/Render/Shaders/Lighting.glsl
struct LightingUniform {
    float cascadeMatrices[worldforge::shadow::cascadeCount][16]; // Engine/Render/ShadowCascades.h
    float cascadeScales[worldforge::shadow::cascadeCount][4];   // meters per texel, depth range, slice end, unused
    float fogCenterRadius[4];
    float settings[4];
    float sun[4];               // direction toward the sun, intensity
    float skyIrradiance[9][4];  // spherical harmonics of irradiance / pi
    float skyHorizon[4];        // w: number of lights
    float skyZenith[4];
    float skyGround[4];
    float lights[16][4];        // position or direction, intensity (negative for directional)
};
static_assert(sizeof(LightingUniform) == 816);

class WorldLightingVulkan {
public:
    static constexpr uint32_t shadowSize = worldforge::shadow::atlasSize; // the cascade atlas
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
