#pragma once

#include "Engine/Render/ShadowCascades.h"
#include "Engine/Render/LightClusters.h"
#include "Engine/Render/SkyLight.h"
#include "Engine/Render/SkyVisibility.h"

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
    float skyHorizon[4];
    float skyZenith[4];
    float skyGround[4];
    float clusterForward[4];    // clustered lights (LightClusters.h): the camera's forward axis
    float clusterViewport[4];   // viewport size in pixels
    float skyMatrices[worldforge::sky::directionCount][16]; // Engine/Render/SkyVisibility.h
    float skyDirections[worldforge::sky::directionCount][4];
    float skySettings[4];       // sky visibility maps on, meters per texel, physical sky on
    float sunTint[4];           // sunlight color at the ground, luminance 1
    // Screen-space reflections (Shading.glsl): the previous frame's clip space without jitter; on with a previous frame
    // to trace, the longest ray in meters.
    float previousViewProjection[16];
    float reflectionSettings[4];
    std::array<float,4> probeGrid{}, probeSettings{};
    float skyIrradianceBand4[9][4]{}; // band-4 coefficients of irradiance / pi (SkyLight.h)
    std::array<float,4> probeGridCoarse{}; // origin, spacing (ProbeField::coarseGrid)
    float skyIrradianceAbove[9][4]{}; // bands 0-2 of the sky above the horizon (SkyLight.h irradianceAbove)
};
static_assert(sizeof(LightingUniform) == 2320);

class WorldLightingVulkan {
public:
    static constexpr uint32_t shadowSize = worldforge::shadow::atlasSize; // the cascade atlas
    static constexpr uint32_t textureCapacity = 2048 * 2048;
    static constexpr uint32_t worldTextureCapacity = 1024 * 1024;
    static constexpr VkDeviceSize environmentBytes = worldforge::color::mapTexels*sizeof(std::array<float,4>);

    bool create(QVulkanWindow &window,QVulkanDeviceFunctions *vk,bool modeling);
    void destroy();
    bool upload(int frame,const LightingUniform &data);
    bool uploadTextures(int frame,std::span<const uint32_t> pixels,uint64_t revision);
    // The sky's environment map (color::SkyLight::environment) for binding 5, copied when revision changes.
    bool uploadEnvironment(int frame,std::span<const std::array<float,4>> environment,uint64_t revision);
    // The lights and their clusters for binding 6 (worldforge::lights::Clusters).
    bool uploadClusters(int frame,const worldforge::lights::Clusters &clusters);
    VkDescriptorSet descriptor(int frame) const { return frames_[frame].set; }
    VkFramebuffer framebuffer(int frame) const { return frames_[frame].shadow.framebuffer; }
    // Binding 8: the previous frame's TAA result (HdrFrame::previousFrame), which changes every frame.
    void bindPreviousFrame(int frame,VkSampler sampler,VkImageView view);
    void bindProbes(int frame,VkBuffer buffer,VkDeviceSize bytes);
    // Binding 12: the fog volume (Engine/Render/Vulkan/FogVolume.h), shared by all frames. Bind it before the first frame.
    void bindFogVolume(const VkDescriptorImageInfo &volume);
    // The sky visibility maps (Engine/Render/SkyVisibility.h), rendered with pass() like the shadow atlas.
    // One set shared by all frames: the viewport re-renders it only when the scene changes.
    VkFramebuffer skyFramebuffer() const { return sky_.framebuffer; }
    // The point light shadow atlas (LightClusters.h), shared by all frames like the sky maps.
    VkFramebuffer pointShadowFramebuffer() const { return pointShadows_.framebuffer; }
    VkRenderPass pass() const { return pass_; }
    VkFormat depthFormat() const { return depthFormat_; }
    VkRenderPass pointUpdatePass() const { return pointUpdatePass_; }
    VkDescriptorSetLayout layout() const { return layout_; }

private:
    struct DepthTarget {
        VkImage image{};
        VkDeviceMemory memory{};
        VkImageView view{};
        VkFramebuffer framebuffer{};
    };
    struct Frame {
        DepthTarget shadow;
        VkBuffer uniform{};
        VkDeviceMemory uniformMemory{};
        VkBuffer textures{};
        VkDeviceMemory textureMemory{};
        uint64_t textureRevision{};
        VkBuffer environment{};
        VkDeviceMemory environmentMemory{};
        uint64_t environmentRevision = ~uint64_t(0);
        VkBuffer clusters{};
        VkDeviceMemory clusterMemory{};
        VkDescriptorSet set{};
    };
    bool createFrame(Frame &frame,VkFormat format);
    bool createDepthTarget(DepthTarget &target,VkFormat format,uint32_t width,uint32_t height);
    void destroyDepthTarget(const DepthTarget &target);
    bool allocate(VkDeviceMemory &memory,VkMemoryRequirements requirements,VkMemoryPropertyFlags properties);
    QVulkanDeviceFunctions *vk_ = nullptr;
    VkDevice device_{};
    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkRenderPass pass_{};
    VkRenderPass pointUpdatePass_{};
    VkDescriptorSetLayout layout_{};
    VkDescriptorPool pool_{};
    VkSampler sampler_{};
    VkDeviceSize textureBytes_{};
    std::vector<Frame> frames_;
    DepthTarget sky_, pointShadows_;
};
