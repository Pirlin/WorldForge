#pragma once

#include "Engine/Render/ColorPipeline.h"
#include "Engine/Render/Vulkan/VulkanApi.h"

#include <array>

namespace worldforge::vk {

// The frame passes shared by the editor viewport and the Player:
//   Scene pass: sky and lit geometry into a linear 16-bit float color target and a depth target.
//   Display pass, subpass 0: temporal anti-aliasing (TAA) blends the scene into a history target.
//   Display pass, subpass 1: exposure and view transform into the output image. The editor then draws
//   its lines and overlays in display colors, depth-tested against the scene.
class HdrFrame {
public:
    static constexpr VkFormat sceneFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr uint32_t sceneSubpass = 0, temporalSubpass = 0, displaySubpass = 1;
    // Samples in the jitter sequence, and how many frames a still view averages.
    static constexpr uint32_t temporalSamples = 16;
    // The scene pass writes the lit scene and, separately, its sky-light part (which ambient occlusion scales) and
    // the surfaces' diffuse albedo with the fog transmission in alpha (which bounce light needs).
    static constexpr uint32_t sceneColorAttachments = 3;
    static constexpr VkFormat albedoFormat = VK_FORMAT_R8G8B8A8_UNORM;
    // Bounce light in RGB and sky visibility in alpha (Ambient.frag). Float, so a frame may exceed 1: the noisy
    // per-frame values average to the unbiased result under TAA.
    static constexpr VkFormat occlusionFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    struct Temporal {
        bool enabled = false;
        // A still view (camera and scene unchanged since resetHistory) averages exactly temporalSamples
        // frames and then holds; a moving view blends each frame and clamps the history.
        bool still = false;
        // This frame's clip space to the previous frame's, without jitter, column-major as GLSL reads it.
        std::array<float,16> reprojection{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    };

    // Ground-truth ambient occlusion and screen-space bounce light (Ambient.frag) from the scene depth and color.
    struct Occlusion {
        bool enabled = false;
        // This frame's clip space (with its jitter) to world space, column-major as GLSL reads it.
        std::array<float,16> inverseViewProjection{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        float radius = 1.5f; // meters
    };

    // outputLayout is the output image's layout after the frame: PRESENT_SRC for a swap chain,
    // TRANSFER_SRC for an image that is read back. meterSlots is how many auto exposure histograms to
    // keep, one per frame in flight.
    bool create(const Device &device,VkFormat outputFormat,VkImageLayout outputLayout,uint32_t meterSlots = 4);
    void destroy();
    bool createTargets(VkExtent2D extent,std::span<const VkImageView> outputs);
    void releaseTargets();
    // The display step uses vertex and fragment (Display.vert, Display.frag); TAA uses vertex and temporal
    // (Temporal.frag); ambient occlusion uses vertex and ambient (Ambient.frag).
    bool createDisplayPipeline(std::span<const uint32_t> vertex,std::span<const uint32_t> fragment,
                               std::span<const uint32_t> temporal,std::span<const uint32_t> ambient);
    void destroyDisplayPipeline();

    // Scene geometry pipelines use scenePass subpass 0; editor overlays use displayPass subpass 1.
    VkRenderPass scenePass() const { return scenePass_; }
    VkRenderPass displayPass() const { return displayPass_; }
    VkFormat depthFormat() const { return depthFormat_; }
    VkExtent2D extent() const { return extent_; }
    bool displayReady() const { return display_ && temporal_ && ambient_ && sceneFramebuffer_; }

    // The sub-pixel offset in pixels, each in [-0.5, 0.5), to render the next frame with when TAA is on.
    std::array<float,2> jitter() const;
    // Starts the TAA history over: the next frame shows only itself and still views restart their average.
    void resetHistory();
    // How many frames the TAA history holds (0 after resetHistory).
    uint32_t accumulated() const { return accumulated_; }
    // The next frame's place in the jitter sequence, 0 to temporalSamples - 1; noise in other passes follows it.
    uint32_t sampleIndex() const { return sample_%temporalSamples; }

    // Begins the scene pass for output image `output`.
    void begin(VkCommandBuffer command,std::size_t output,const float background[3]);
    // Ends the scene pass, computes ambient occlusion, resolves TAA and draws the display step in the display
    // pass. With meterSlot set, it also counts the scene luminance into that histogram for auto exposure. The caller may draw editor
    // overlays, then ends the render pass; call finishMeter after that when metering.
    void display(VkCommandBuffer command,float exposureStops,color::ViewTransform transform,int meterSlot,
                 const Temporal &temporal,const Occlusion &occlusion);
    void finishMeter(VkCommandBuffer command) const;
    // After the frame that metered into slot has finished: reads and clears the histogram, and gives the
    // log2 average scene luminance. Returns false when nothing was metered.
    bool takeMeter(uint32_t slot,float &log2Average) const;
    uint32_t meterSlots() const { return meterSlots_; }

private:
    bool createPipeline(VkShaderModule vertex,VkShaderModule fragment,VkPipelineLayout layout,VkRenderPass pass,
                        uint32_t subpass,VkPipeline &pipeline);

    const Device *device_ = nullptr;
    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
    bool srgbOutput_ = false;
    VkRenderPass scenePass_{}, occlusionPass_{}, displayPass_{};
    VkDescriptorSetLayout displaySetLayout_{}, temporalSetLayout_{}, occlusionSetLayout_{};
    VkDescriptorSet occlusionSet_{};
    VkDescriptorPool pool_{};
    std::array<VkDescriptorSet,2> displaySets_{}, temporalSets_{};
    VkPipelineLayout displayLayout_{}, temporalLayout_{}, occlusionLayout_{};
    VkPipeline display_{}, temporal_{}, ambient_{};
    VkSampler nearest_{}, linear_{};
    Image scene_, indirect_, albedo_, occlusion_, depth_;
    std::array<Image,2> history_;
    std::array<bool,2> historyReady_{};
    VkExtent2D extent_{};
    VkFramebuffer sceneFramebuffer_{}, occlusionFramebuffer_{};
    std::vector<VkFramebuffer> displayFramebuffers_; // output * 2 + history index
    std::size_t output_ = 0;
    uint32_t parity_ = 0, sample_ = 0, accumulated_ = 0;
    static constexpr VkDeviceSize meterBytes = 256;
    Buffer meter_;
    uint32_t meterSlots_ = 0;
};

} // namespace worldforge::vk
