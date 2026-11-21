#pragma once

#include "Engine/Render/Vulkan/HdrFrame.h"

#include <QByteArray>
#include <QVulkanWindow>

// Connects a QVulkanWindow to the shared HDR frame passes in Engine/Render/Vulkan, which the Player
// also uses. Lines and overlays draw in the display subpass after TAA and the view transform.
class ViewportFrameVulkan {
public:
    using Temporal = worldforge::vk::HdrFrame::Temporal;
    using Occlusion = worldforge::vk::HdrFrame::Occlusion;
    static constexpr uint32_t sceneSubpass = worldforge::vk::HdrFrame::sceneSubpass;
    static constexpr uint32_t displaySubpass = worldforge::vk::HdrFrame::displaySubpass;

    bool create(QVulkanWindow &window);
    void destroy();
    bool createTargets();
    void releaseTargets() { frame_.releaseTargets(); }
    bool createDisplayPipeline(const QByteArray &vertex,const QByteArray &fragment,const QByteArray &temporal,
                               const QByteArray &ambient);
    void destroyDisplayPipeline() { frame_.destroyDisplayPipeline(); }

    VkRenderPass scenePass() const { return frame_.scenePass(); }
    VkRenderPass displayPass() const { return frame_.displayPass(); }
    bool displayReady() const { return frame_.displayReady(); }
    std::array<float,2> jitter() const { return frame_.jitter(); }
    void resetHistory() { frame_.resetHistory(); }
    uint32_t accumulated() const { return frame_.accumulated(); }
    uint32_t sampleIndex() const { return frame_.sampleIndex(); }
    // Begins the scene pass for swap chain image `image`.
    void begin(VkCommandBuffer command,int image,const float background[3]) { frame_.begin(command,std::size_t(image),background); }
    // Ends the scene pass, resolves TAA and draws the exposure and view transform step, metering the scene into
    // meterSlot for auto exposure when it is set (see HdrFrame).
    void display(VkCommandBuffer command,float exposureStops,worldforge::color::ViewTransform transform,int meterSlot = -1,
                 const Temporal &temporal = {},const Occlusion &occlusion = {})
    {
        frame_.display(command,exposureStops,transform,meterSlot,temporal,occlusion);
    }
    void finishMeter(VkCommandBuffer command) const { frame_.finishMeter(command); }
    bool takeMeter(uint32_t slot,float &log2Average) const { return frame_.takeMeter(slot,log2Average); }

private:
    QVulkanWindow *window_ = nullptr;
    worldforge::vk::Api api_;
    worldforge::vk::Device device_;
    worldforge::vk::HdrFrame frame_;
};
