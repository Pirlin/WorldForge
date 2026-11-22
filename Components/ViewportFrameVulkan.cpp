#ifdef WORLDFORGE_VULKAN
#include "Components/ViewportFrameVulkan.h"

#include <QVulkanInstance>
#include <vector>

namespace {
std::span<const uint32_t> words(const QByteArray &code)
{
    if (code.size()%4)
        return {};
    return {reinterpret_cast<const uint32_t *>(code.constData()),std::size_t(code.size()/4)};
}
}

bool ViewportFrameVulkan::create(QVulkanWindow &window)
{
    window_ = &window;
    auto *instance = window.vulkanInstance();
    const auto getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        instance->getInstanceProcAddr("vkGetInstanceProcAddr"));
    if (!api_.loadGlobal(getInstanceProcAddr) || !api_.loadInstance(instance->vkInstance())
        || !api_.loadDevice(window.device()))
        return false;
    device_.init(api_,window.physicalDevice(),window.device());
    return frame_.create(device_,window.colorFormat(),VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

void ViewportFrameVulkan::destroy()
{
    frame_.destroy();
}

bool ViewportFrameVulkan::createTargets()
{
    if (!window_ || !frame_.scenePass())
        return false;
    std::vector<VkImageView> outputs;
    for (int i = 0; i < window_->swapChainImageCount(); ++i)
        outputs.push_back(window_->swapChainImageView(i));
    const auto size = window_->swapChainImageSize();
    return frame_.createTargets({uint32_t(size.width()),uint32_t(size.height())},outputs);
}

bool ViewportFrameVulkan::createDisplayPipeline(const QByteArray &vertex,const QByteArray &fragment,const QByteArray &temporal,
                                                const QByteArray &ambient,const QByteArray &bloomDown,
                                                const QByteArray &bloomUp,const QByteArray &depthOfField,
                                                const QByteArray &motionBlur)
{
    return frame_.createDisplayPipeline(words(vertex),words(fragment),words(temporal),words(ambient),words(bloomDown),
                                        words(bloomUp),words(depthOfField),words(motionBlur));
}
#endif
