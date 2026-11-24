#include "Engine/Render/Vulkan/OcclusionDepth.h"

#include "ReduceDepth_comp.h"

namespace worldforge::vk {
namespace {
struct Reduction {
    VkExtent2D source, target;
    uint32_t sourceOffset, targetOffset, first;
};

VkExtent2D tileSize(VkExtent2D size,uint32_t block)
{
    return {(size.width+block-1)/block,(size.height+block-1)/block};
}

uint32_t tileCount(VkExtent2D size) { return size.width*size.height; }
}

bool OcclusionDepth::create(const Device &device,uint32_t frameSlots)
{
    destroy();
    device_ = &device;
    const auto &vk = *device.api;
    constexpr VkDescriptorSetLayoutBinding bindings[]{
        {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    const VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,nullptr,0,2,bindings};
    const VkDescriptorPoolSize capacity[]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,frameSlots},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,frameSlots}};
    const VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,nullptr,0,frameSlots,2,capacity};
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Reduction)};
    const VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,nullptr,0,1,&setLayout_,1,&push};
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vk.vkCreateDescriptorSetLayout(device.device,&set,nullptr,&setLayout_) != VK_SUCCESS
        || vk.vkCreateDescriptorPool(device.device,&pool,nullptr,&pool_) != VK_SUCCESS
        || vk.vkCreatePipelineLayout(device.device,&layout,nullptr,&layout_) != VK_SUCCESS
        || vk.vkCreateSampler(device.device,&sampler,nullptr,&sampler_) != VK_SUCCESS) {
        destroy();
        return false;
    }
    frames_.resize(frameSlots);
    const VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,nullptr,pool_,1,&setLayout_};
    for (auto &frame : frames_)
        if (vk.vkAllocateDescriptorSets(device.device,&allocation,&frame.set) != VK_SUCCESS) {
            destroy();
            return false;
        }
    const auto module = device.shader(spirv_ReduceDepth_comp);
    if (!module) {
        destroy();
        return false;
    }
    const VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,
        VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};
    const VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,nullptr,0,stage,layout_,
        VK_NULL_HANDLE,-1};
    const bool created = vk.vkCreateComputePipelines(device.device,VK_NULL_HANDLE,1,&pipeline,nullptr,&pipeline_) == VK_SUCCESS;
    vk.vkDestroyShaderModule(device.device,module,nullptr);
    if (!created)
        destroy();
    return created;
}

void OcclusionDepth::destroy()
{
    if (!device_)
        return;
    const auto &vk = *device_->api;
    const auto device = device_->device;
    vk.vkDestroyPipeline(device,pipeline_,nullptr);
    vk.vkDestroyPipelineLayout(device,layout_,nullptr);
    vk.vkDestroyDescriptorPool(device,pool_,nullptr);
    vk.vkDestroyDescriptorSetLayout(device,setLayout_,nullptr);
    vk.vkDestroySampler(device,sampler_,nullptr);
    for (auto &frame : frames_) {
        vk.vkDestroyFramebuffer(device,frame.framebuffer,nullptr);
        frame.depth.destroy(*device_);
        frame.pyramid.destroy(*device_);
    }
    frames_.clear();
    device_ = nullptr;
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
}

bool OcclusionDepth::begin(VkCommandBuffer command,uint32_t slot,VkExtent2D extent,VkFormat format,VkRenderPass pass)
{
    if (!pipeline_ || slot >= frames_.size() || extent.width == 0 || extent.height == 0)
        return false;
    auto &frame = frames_[slot];
    const auto &vk = *device_->api;
    const auto device = device_->device;
    if (!frame.framebuffer || frame.extent.width != extent.width || frame.extent.height != extent.height) {
        vk.vkDestroyFramebuffer(device,frame.framebuffer,nullptr);
        frame.framebuffer = VK_NULL_HANDLE;
        frame.depth.destroy(*device_);
        frame.pyramid.destroy(*device_);
        uint32_t samples = 0;
        for (auto size = tileSize(extent,8);; size = tileSize(size,2)) {
            samples += tileCount(size);
            if (tileCount(size) == 1)
                break;
        }
        if (!frame.depth.create(*device_,format,extent,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_DEPTH_BIT)
            || !frame.pyramid.create(*device_,VkDeviceSize(samples)*sizeof(float),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
            return false;
        const VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,nullptr,0,pass,1,&frame.depth.view,
            extent.width,extent.height,1};
        if (vk.vkCreateFramebuffer(device,&framebuffer,nullptr,&frame.framebuffer) != VK_SUCCESS)
            return false;
        frame.extent = extent;
        const VkDescriptorImageInfo image{sampler_,frame.depth.view,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        const VkDescriptorBufferInfo buffer{frame.pyramid.buffer,0,frame.pyramid.size};
        const VkWriteDescriptorSet writes[]{
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,0,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&image,nullptr,nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,1,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&buffer,nullptr}};
        vk.vkUpdateDescriptorSets(device,2,writes,0,nullptr);
    }
    const VkRect2D area{{0,0},extent};
    const VkClearValue clear{.depthStencil = {1,0}};
    const VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,nullptr,pass,frame.framebuffer,area,1,&clear};
    vk.vkCmdBeginRenderPass(command,&begin,VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0,0,float(extent.width),float(extent.height),0,1};
    vk.vkCmdSetViewport(command,0,1,&viewport);
    vk.vkCmdSetScissor(command,0,1,&area);
    return true;
}

const Buffer &OcclusionDepth::finish(VkCommandBuffer command,uint32_t slot)
{
    const auto &frame = frames_[slot];
    const auto &vk = *device_->api;
    vk.vkCmdEndRenderPass(command);
    const VkImageMemoryBarrier depthReady{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,nullptr,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,frame.depth.image,{VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1}};
    vk.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&depthReady);
    vk.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_);
    vk.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,1,&frame.set,0,nullptr);
    const VkMemoryBarrier reduced{VK_STRUCTURE_TYPE_MEMORY_BARRIER,nullptr,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT};
    Reduction level{frame.extent,tileSize(frame.extent,8),0,0,1};
    for (;;) {
        vk.vkCmdPushConstants(command,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(level),&level);
        const auto groups = tileSize(level.target,8);
        vk.vkCmdDispatch(command,groups.width,groups.height,1);
        vk.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,1,&reduced,0,nullptr,0,nullptr);
        const auto count = tileCount(level.target);
        if (count == 1)
            return frame.pyramid;
        level = {level.target,tileSize(level.target,2),level.targetOffset,level.targetOffset+count,0};
    }
}

}
