#pragma once

#include "Engine/Render/ColorPipeline.h"
#include "Engine/Render/Vulkan/VulkanApi.h"

#include <array>

namespace worldforge::vk {

// The frame passes shared by the editor viewport and the Player:
//   Scene pass: sky and lit geometry into a linear 16-bit float color target and a depth target.
//   Occlusion pass: ambient occlusion and bounce light from the scene depth and color.
//   Bloom passes: a blur chain from the previous frame's result.
//   TAA pass: temporal anti-aliasing blends the scene into a history target.
//   Display pass: exposure and view transform into the output image. The editor then draws its lines and
//   overlays in display colors, depth-tested against the scene.
class HdrFrame {
public:
    static constexpr VkFormat sceneFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr uint32_t sceneSubpass = 0, displaySubpass = 0;
    // Samples in the jitter sequence, and how many frames a still view averages.
    static constexpr uint32_t temporalSamples = 16;
    // The scene pass writes the lit scene and, separately, its sky-light part (which ambient occlusion scales), the
    // surfaces' diffuse albedo with the fog transmission in alpha (which bounce light needs) and their motion.
    static constexpr uint32_t sceneColorAttachments = 4;
    static constexpr VkFormat albedoFormat = VK_FORMAT_R8G8B8A8_UNORM;
    // Each surface's own motion since the previous frame, apart from the camera's: the change in its position in the
    // previous frame's screen (UV units), 0 for static geometry and sky. TAA and motion blur add it to the camera's.
    static constexpr VkFormat velocityFormat = VK_FORMAT_R16G16_SFLOAT;
    // Bounce light in RGB and sky visibility in alpha (Ambient.frag). Float, so a frame may exceed 1: the noisy
    // per-frame values average to the unbiased result under TAA.
    static constexpr VkFormat occlusionFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    // Bloom: a chain of bloomLevels images from half size down, built from the previous frame's TAA result before the
    // display pass (one frame late, which motion hides), and mixed into the scene by the display step.
    static constexpr int bloomLevels = 6;
    static constexpr float bloomStrength = color::bloomStrength;
    // Screen-space reflections (Shading.glsl) march rays up to this many meters through the previous frame.
    static constexpr float reflectionRange = 100.0f;

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

    // Camera effects applied after TAA (DepthOfField.frag, MotionBlur.frag); both off by default.
    struct Camera {
        // Depth of field: a thin lens focused at focusDistance meters with aperture f-number fNumber, its focal length
        // from verticalFov (radians) on a 24 mm tall sensor. eye, forward and inverseViewProjection (clip space to
        // world space, column-major, without jitter) locate each pixel's depth.
        bool depthOfField = false;
        float focusDistance = 10, fNumber = 2.8f, verticalFov = 0.785398f;
        std::array<float,3> eye{}, forward{0,1,0};
        std::array<float,16> inverseViewProjection{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        // Motion blur from the camera's movement (Temporal::reprojection), with the shutter open for this share of
        // the frame (0.5 is a 180-degree shutter).
        bool motionBlur = false;
        float shutter = 0.5f;
    };

    // outputLayout is the output image's layout after the frame: PRESENT_SRC for a swap chain,
    // TRANSFER_SRC for an image that is read back. meterSlots is how many auto exposure histograms to
    // keep, one per frame in flight.
    bool create(const Device &device,VkFormat outputFormat,VkImageLayout outputLayout,uint32_t meterSlots = 4);
    void destroy();
    bool createTargets(VkExtent2D extent,std::span<const VkImageView> outputs);
    void releaseTargets();
    // The display step uses vertex and fragment (Display.vert, Display.frag); TAA uses vertex and temporal
    // (Temporal.frag); ambient occlusion uses vertex and ambient (Ambient.frag); bloom uses vertex with bloomDown and
    // bloomUp (BloomDown.frag, BloomUp.frag); camera effects use vertex with depthOfField and motionBlur.
    bool createDisplayPipeline(std::span<const uint32_t> vertex,std::span<const uint32_t> fragment,
                               std::span<const uint32_t> temporal,std::span<const uint32_t> ambient,
                               std::span<const uint32_t> bloomDown,std::span<const uint32_t> bloomUp,
                               std::span<const uint32_t> depthOfField,std::span<const uint32_t> motionBlur);
    void destroyDisplayPipeline();

    // Scene geometry pipelines use scenePass subpass 0; editor overlays use displayPass subpass 0.
    VkRenderPass scenePass() const { return scenePass_; }
    VkRenderPass displayPass() const { return displayPass_; }
    VkFormat depthFormat() const { return depthFormat_; }
    VkExtent2D extent() const { return extent_; }
    bool displayReady() const { return display_ && temporal_ && ambient_ && bloomDown_ && bloomUp_ && depthOfField_ && motionBlur_ && sceneFramebuffer_; }

    // The sub-pixel offset in pixels, each in [-0.5, 0.5), to render the next frame with when TAA is on.
    std::array<float,2> jitter() const;
    // Starts the TAA history over: the next frame shows only itself and still views restart their average.
    void resetHistory();
    // How many frames the TAA history holds (0 after resetHistory).
    uint32_t accumulated() const { return accumulated_; }
    // The next frame's place in the jitter sequence, 0 to temporalSamples - 1; noise in other passes follows it.
    uint32_t sampleIndex() const { return sample_%temporalSamples; }
    // The previous frame's TAA result, for the scene pass to read (screen-space reflections, Shading.glsl): linear
    // light in RGB and the depth along the camera's forward axis in alpha (0 for sky), in SHADER_READ_ONLY_OPTIMAL
    // layout. Which image this is changes every frame, so bind it each frame before begin(). It holds a frame only
    // when previousFrameReady().
    VkImageView previousFrame() const { return history_[1-parity_].view; }
    bool previousFrameReady() const { return framesShown_ > 0; }
    VkSampler nearestSampler() const { return nearest_; }

    // Begins the scene pass for output image `output`.
    void begin(VkCommandBuffer command,std::size_t output,const float background[3]);
    // Ends the scene pass, computes ambient occlusion, resolves TAA and draws the display step in the display
    // pass. With meterSlot set, it also counts the scene luminance into that histogram for auto exposure. bloom is the
    // share of light scattered around bright areas (bloomStrength, or 0 for none). The caller may draw editor
    // overlays, then ends the render pass; call finishMeter after that when metering.
    void display(VkCommandBuffer command,float exposureStops,color::ViewTransform transform,int meterSlot,
                 const Temporal &temporal,const Occlusion &occlusion,float bloom,const Camera &camera = {});
    void finishMeter(VkCommandBuffer command) const;
    // After the frame that metered into slot has finished: reads and clears the histogram, and gives the
    // log2 average scene luminance. Returns false when nothing was metered.
    bool takeMeter(uint32_t slot,float &log2Average) const;
    uint32_t meterSlots() const { return meterSlots_; }

private:
    bool createPipeline(VkShaderModule vertex,VkShaderModule fragment,VkPipelineLayout layout,VkRenderPass pass,
                        uint32_t subpass,VkPipeline &pipeline,bool additive = false);
    // Builds the bloom chain from the previous frame's TAA result.
    void bloomChain(VkCommandBuffer command);
    // Gives images that start undefined the layout their descriptors name, before anything reads them.
    void initializeLayouts(VkCommandBuffer command);

    const Device *device_ = nullptr;
    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
    bool srgbOutput_ = false;
    VkRenderPass scenePass_{}, occlusionPass_{}, displayPass_{}, bloomDownPass_{}, bloomUpPass_{};
    VkDescriptorSetLayout displaySetLayout_{}, temporalSetLayout_{}, occlusionSetLayout_{}, bloomSetLayout_{};
    VkDescriptorSet occlusionSet_{};
    VkDescriptorPool pool_{};
    // Display sets read history 0, history 1, post 0 or post 1; temporal sets write history 0 or 1.
    std::array<VkDescriptorSet,4> displaySets_{};
    std::array<VkDescriptorSet,2> temporalSets_{};
    // Camera effects: two targets to ping-pong between, sets reading each of the four images above with the depth.
    std::array<Image,2> post_;
    std::array<VkFramebuffer,2> postFramebuffers_{};
    VkDescriptorSetLayout postSetLayout_{};
    std::array<VkDescriptorSet,4> postSets_{};
    VkPipelineLayout postLayout_{};
    VkPipeline depthOfField_{}, motionBlur_{};
    VkPipelineLayout displayLayout_{}, temporalLayout_{}, occlusionLayout_{}, bloomLayout_{};
    VkPipeline display_{}, temporal_{}, ambient_{}, bloomDown_{}, bloomUp_{};
    // Bloom levels, their framebuffers, and descriptor sets reading each level (0 to bloomLevels - 1) and each history
    // image (bloomLevels, bloomLevels + 1).
    std::array<Image,bloomLevels> bloom_;
    std::array<VkExtent2D,bloomLevels> bloomExtents_{};
    std::array<VkFramebuffer,bloomLevels> bloomFramebuffers_{};
    std::array<VkDescriptorSet,bloomLevels+2> bloomSets_{};
    bool bloomInitialized_ = false;
    uint32_t framesShown_ = 0; // since the targets were created; the previous resolve exists from the second frame
    VkSampler nearest_{}, linear_{};
    Image scene_, indirect_, albedo_, velocity_, occlusion_, depth_;
    std::array<Image,2> history_;
    std::array<bool,2> historyReady_{};
    VkExtent2D extent_{};
    VkFramebuffer sceneFramebuffer_{}, occlusionFramebuffer_{};
    std::array<VkFramebuffer,2> temporalFramebuffers_{}; // TAA into each history image
    std::vector<VkFramebuffer> displayFramebuffers_; // one per output
    std::size_t output_ = 0;
    uint32_t parity_ = 0, sample_ = 0, accumulated_ = 0;
    static constexpr VkDeviceSize meterBytes = 256;
    Buffer meter_;
    uint32_t meterSlots_ = 0;
};

} // namespace worldforge::vk
