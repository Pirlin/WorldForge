#pragma once

#include "Engine/Render/Vulkan/VulkanApi.h"

namespace worldforge::vk {

// Per-frame targets for conservative occlusion against depth drawn from the current camera.
class OcclusionDepth {
public:
    OcclusionDepth() = default;
    ~OcclusionDepth() { destroy(); }
    OcclusionDepth(const OcclusionDepth &) = delete;
    OcclusionDepth &operator=(const OcclusionDepth &) = delete;

    bool create(const Device &device,uint32_t frameSlots);
    void destroy();
    // The depth-only pass clears to 1 and stores depth in DEPTH_STENCIL_READ_ONLY_OPTIMAL.
    bool begin(VkCommandBuffer command,uint32_t frame,VkExtent2D extent,VkFormat format,VkRenderPass pass);
    const Buffer &finish(VkCommandBuffer command,uint32_t frame);

private:
    struct Frame {
        Image depth;
        Buffer pyramid;
        VkFramebuffer framebuffer{};
        VkDescriptorSet set{};
        VkExtent2D extent{};
    };
    const Device *device_ = nullptr;
    VkSampler sampler_{};
    VkDescriptorSetLayout setLayout_{};
    VkDescriptorPool pool_{};
    VkPipelineLayout layout_{};
    VkPipeline pipeline_{};
    std::vector<Frame> frames_;
};

}
