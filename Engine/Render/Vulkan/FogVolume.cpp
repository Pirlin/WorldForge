#include "Engine/Render/Vulkan/FogVolume.h"

#include "FogVolume_comp.h"

#include <algorithm>
#include <cmath>

namespace worldforge::vk {

bool FogVolume::create(const Device &device,VkDescriptorSetLayout frameLayout)
{
    destroy();
    device_ = &device;
    const auto &vk = *device.api;
    constexpr auto format = VK_FORMAT_R16G16B16A16_SFLOAT;
    if (!device.supports(format,VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
        || !image_.create(device,format,{width,height},VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT,1,2*depth)) {
        destroy();
        return false;
    }
    constexpr VkDescriptorSetLayoutBinding output{0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    const VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,nullptr,0,1,&output};
    const VkDescriptorPoolSize capacity{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1};
    const VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,nullptr,0,1,1,&capacity};
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vk.vkCreateDescriptorSetLayout(device.device,&set,nullptr,&setLayout_) != VK_SUCCESS
        || vk.vkCreateDescriptorPool(device.device,&pool,nullptr,&pool_) != VK_SUCCESS
        || vk.vkCreateSampler(device.device,&sampler,nullptr,&sampler_) != VK_SUCCESS) {
        destroy();
        return false;
    }
    const VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,nullptr,pool_,1,&setLayout_};
    const VkDescriptorSetLayout sets[]{frameLayout,setLayout_};
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(View)};
    const VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,nullptr,0,2,sets,1,&push};
    if (vk.vkAllocateDescriptorSets(device.device,&allocation,&set_) != VK_SUCCESS
        || vk.vkCreatePipelineLayout(device.device,&layout,nullptr,&layout_) != VK_SUCCESS) {
        destroy();
        return false;
    }
    const VkDescriptorImageInfo target{VK_NULL_HANDLE,image_.view,VK_IMAGE_LAYOUT_GENERAL};
    const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,set_,0,0,1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        &target,nullptr,nullptr};
    vk.vkUpdateDescriptorSets(device.device,1,&write,0,nullptr);
    const auto module = device.shader(spirv_FogVolume_comp);
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

void FogVolume::destroy()
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
    image_.destroy(*device_);
    device_ = nullptr;
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    set_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    initialized_ = false;
}

float FogVolume::reach(float shadowFar,const float eye[3],const float mist[4])
{
    float far = shadowFar;
    if (mist[3] > 0) {
        const float dx = mist[0]-eye[0], dy = mist[1]-eye[1], dz = mist[2]-eye[2];
        far = std::max(far,std::sqrt(dx*dx+dy*dy+dz*dz)+mist[3]*1.1f+1);
    }
    return std::clamp(far,2*nearDepth,4000.0f);
}

bool FogVolume::needed(float fogDensity,bool shadows,float sunIntensity,const float mist[4],float mistDensity)
{
    return (fogDensity > 0 && shadows && sunIntensity > 0) || (mist[3] > 0 && mistDensity > 0);
}

void FogVolume::prepare(VkCommandBuffer command)
{
    if (!pipeline_ || initialized_)
        return;
    const VkImageMemoryBarrier general{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,nullptr,0,VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,image_.image,
        {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    device_->api->vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,
        nullptr,0,nullptr,1,&general);
    initialized_ = true;
}

void FogVolume::record(VkCommandBuffer command,VkDescriptorSet frameSet,const View &view)
{
    if (!pipeline_)
        return;
    const auto &vk = *device_->api;
    // The shadow maps were just drawn, and the previous frame's scene pass may still read the volume.
    const VkMemoryBarrier inputs{VK_STRUCTURE_TYPE_MEMORY_BARRIER,nullptr,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT};
    const VkImageMemoryBarrier writable{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,nullptr,VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_SHADER_WRITE_BIT,initialized_ ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,
        VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,image_.image,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vk.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
        | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&inputs,0,nullptr,1,&writable);
    initialized_ = true;
    vk.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_);
    const VkDescriptorSet sets[]{frameSet,set_};
    vk.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,2,sets,0,nullptr);
    vk.vkCmdPushConstants(command,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(view),&view);
    vk.vkCmdDispatch(command,(width+7)/8,(height+7)/8,1);
    const VkImageMemoryBarrier readable{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,nullptr,VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,
        image_.image,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    vk.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,
        0,nullptr,1,&readable);
}

} // namespace worldforge::vk
