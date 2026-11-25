#pragma once

#include "Engine/Render/Vulkan/VulkanApi.h"

#include <array>

namespace worldforge::vk {

// The froxel volume the height fog and local mist read (Lighting.glsl fogVolumeAt), shared by the editor viewport and
// the Player. A compute pass (FogVolume.comp) fills it each frame after the shadow maps and before the scene pass: one
// invocation per froxel column sums the shadowed share of the fog and mist and the mist's lamp light from the eye
// outward, reading the renderer's lighting set (set 0) for the Lighting block, the sun's cascades, the lamps and
// their shadows. The image holds two stacks of `depth` slices (mist, then height fog).
// The scene and sky shaders sample it through set 0 binding 12, in GENERAL layout. This takes the fog's shadow lookups
// and the mist's lamp loop out of the scene shader, whose cost follows its register use.
class FogVolume {
public:
    // Keep these in step with fogVolumeSize and fogVolumeNear in Lighting.glsl.
    static constexpr uint32_t width = 160, height = 90, depth = 64;
    static constexpr float nearDepth = 0.5f;
    // The binding in the lighting set where the scene and sky shaders sample the volume.
    static constexpr uint32_t binding = 12;

    struct View {
        std::array<float,16> inverseViewProjection{}; // clip space (with this frame's jitter) to world space
        std::array<float,4> eye{};
        std::array<float,4> fog{}; // height fog density, falloff and base height, as the scene shaders take them
    };

    FogVolume() = default;
    ~FogVolume() { destroy(); }
    FogVolume(const FogVolume &) = delete;
    FogVolume &operator=(const FogVolume &) = delete;

    // frameLayout is the lighting set's layout; bindings 0, 1, 6 and 7 must include the compute stage.
    bool create(const Device &device,VkDescriptorSetLayout frameLayout);
    void destroy();
    bool ready() const { return pipeline_ != VK_NULL_HANDLE; }
    VkImageView view() const { return image_.view; }
    VkSampler sampler() const { return sampler_; }
    VkDescriptorImageInfo descriptor() const { return {sampler_,image_.view,VK_IMAGE_LAYOUT_GENERAL}; }

    // How far the volume reaches along the camera's forward axis: the sun cascades' far end, or the far side of the
    // local mist (center and radius) when that is farther. Write it to the Lighting block's probeSettings.w on frames
    // that call record, and 0 on frames that do not.
    static float reach(float shadowFar,const float eye[3],const float mist[4]);
    // Whether this frame needs the volume: height fog in sunlight with shadows on, or a local mist.
    static bool needed(float fogDensity,bool shadows,float sunIntensity,const float mist[4],float mistDensity);

    // Outside a render pass, after the shadow maps are drawn: fills the volume from frameSet.
    void record(VkCommandBuffer command,VkDescriptorSet frameSet,const View &view);
    // On frames that do not call record: gives the volume its GENERAL layout once, so set 0 may bind it.
    void prepare(VkCommandBuffer command);

private:
    const Device *device_ = nullptr;
    Image image_;
    bool initialized_ = false;
    VkSampler sampler_{};
    VkDescriptorSetLayout setLayout_{};
    VkDescriptorPool pool_{};
    VkDescriptorSet set_{};
    VkPipelineLayout layout_{};
    VkPipeline pipeline_{};
};

} // namespace worldforge::vk
