#ifdef WORLDFORGE_VULKAN
#include "Components/WorldLightingVulkan.h"

#include <cstring>

bool WorldLightingVulkan::create(QVulkanWindow &window,QVulkanDeviceFunctions *vk,bool modeling)
{
    textureBytes_ = sizeof(uint32_t)*(modeling ? textureCapacity : worldTextureCapacity);
    vk_ = vk;
    device_ = window.device();
    auto *instance = window.vulkanInstance()->functions();
    instance->vkGetPhysicalDeviceMemoryProperties(window.physicalDevice(),&memory_);
    VkFormat format = VK_FORMAT_UNDEFINED;
    const auto required = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    for (const auto candidate : {VK_FORMAT_D32_SFLOAT,VK_FORMAT_D16_UNORM}) {
        VkFormatProperties properties{};
        instance->vkGetPhysicalDeviceFormatProperties(window.physicalDevice(),candidate,&properties);
        if ((properties.optimalTilingFeatures & required) == required) {
            format = candidate;
            break;
        }
    }
    if (format == VK_FORMAT_UNDEFINED)
        return false;
    const VkAttachmentDescription depth{0,format,VK_SAMPLE_COUNT_1_BIT,VK_ATTACHMENT_LOAD_OP_CLEAR,
        VK_ATTACHMENT_STORE_OP_STORE,VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,
        VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    const VkAttachmentReference depthReference{0,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpass{0,VK_PIPELINE_BIND_POINT_GRAPHICS,0,nullptr,0,nullptr,nullptr,&depthReference,0,nullptr};
    const VkSubpassDependency dependencies[]{
        {VK_SUBPASS_EXTERNAL,0,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
         VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,0},
        {0,VK_SUBPASS_EXTERNAL,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
         VK_ACCESS_SHADER_READ_BIT,0}
    };
    const VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,nullptr,0,1,&depth,1,&subpass,2,dependencies};
    if (vk_->vkCreateRenderPass(device_,&pass,nullptr,&pass_) != VK_SUCCESS)
        return false;
    const VkDescriptorSetLayoutBinding bindings[]{
        {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}
    };
    const VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,nullptr,0,3,bindings};
    if (vk_->vkCreateDescriptorSetLayout(device_,&layout,nullptr,&layout_) != VK_SUCCESS)
        return false;
    const auto count = uint32_t(window.concurrentFrameCount());
    const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,count},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,count},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,count}};
    const VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,nullptr,0,count,3,sizes};
    if (vk_->vkCreateDescriptorPool(device_,&pool,nullptr,&pool_) != VK_SUCCESS)
        return false;
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vk_->vkCreateSampler(device_,&sampler,nullptr,&sampler_) != VK_SUCCESS)
        return false;
    frames_.resize(count);
    for (auto &frame : frames_)
        if (!createFrame(frame,format))
            return false;
    return true;
}

bool WorldLightingVulkan::createFrame(Frame &frame,VkFormat format)
{
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = format;
    image.extent = {shadowSize,shadowSize,1};
    image.mipLevels = image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (vk_->vkCreateImage(device_,&image,nullptr,&frame.image) != VK_SUCCESS)
        return false;
    VkMemoryRequirements requirements{};
    vk_->vkGetImageMemoryRequirements(device_,frame.image,&requirements);
    if (!allocate(frame.imageMemory,requirements,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        || vk_->vkBindImageMemory(device_,frame.image,frame.imageMemory,0) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = frame.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
    if (vk_->vkCreateImageView(device_,&view,nullptr,&frame.view) != VK_SUCCESS)
        return false;
    const VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,nullptr,0,pass_,
        1,&frame.view,shadowSize,shadowSize,1};
    if (vk_->vkCreateFramebuffer(device_,&framebuffer,nullptr,&frame.framebuffer) != VK_SUCCESS)
        return false;
    const VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,nullptr,0,sizeof(LightingUniform),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,VK_SHARING_MODE_EXCLUSIVE,0,nullptr};
    if (vk_->vkCreateBuffer(device_,&buffer,nullptr,&frame.uniform) != VK_SUCCESS)
        return false;
    vk_->vkGetBufferMemoryRequirements(device_,frame.uniform,&requirements);
    if (!allocate(frame.uniformMemory,requirements,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        || vk_->vkBindBufferMemory(device_,frame.uniform,frame.uniformMemory,0) != VK_SUCCESS)
        return false;
    const VkBufferCreateInfo textureBuffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,nullptr,0,textureBytes_,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_SHARING_MODE_EXCLUSIVE,0,nullptr};
    if (vk_->vkCreateBuffer(device_,&textureBuffer,nullptr,&frame.textures) != VK_SUCCESS)
        return false;
    vk_->vkGetBufferMemoryRequirements(device_,frame.textures,&requirements);
    if (!allocate(frame.textureMemory,requirements,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        || vk_->vkBindBufferMemory(device_,frame.textures,frame.textureMemory,0) != VK_SUCCESS)
        return false;
    const VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,nullptr,pool_,1,&layout_};
    if (vk_->vkAllocateDescriptorSets(device_,&allocation,&frame.set) != VK_SUCCESS)
        return false;
    const VkDescriptorImageInfo depth{sampler_,frame.view,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo uniform{frame.uniform,0,sizeof(LightingUniform)};
    const VkDescriptorBufferInfo textures{frame.textures,0,textureBytes_};
    const VkWriteDescriptorSet writes[]{
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,0,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&depth,nullptr,nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,1,0,1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,nullptr,&uniform,nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,2,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&textures,nullptr}
    };
    vk_->vkUpdateDescriptorSets(device_,3,writes,0,nullptr);
    return true;
}

bool WorldLightingVulkan::allocate(VkDeviceMemory &memory,VkMemoryRequirements requirements,VkMemoryPropertyFlags flags)
{
    for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & flags) == flags) {
            const VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,nullptr,requirements.size,i};
            return vk_->vkAllocateMemory(device_,&allocation,nullptr,&memory) == VK_SUCCESS;
        }
    return false;
}

bool WorldLightingVulkan::upload(int frame,const LightingUniform &data)
{
    const auto memory = frames_[frame].uniformMemory;
    void *destination = nullptr;
    if (vk_->vkMapMemory(device_,memory,0,sizeof(data),0,&destination) != VK_SUCCESS)
        return false;
    std::memcpy(destination,&data,sizeof(data));
    vk_->vkUnmapMemory(device_,memory);
    return true;
}

bool WorldLightingVulkan::uploadTextures(int index,std::span<const uint32_t> pixels,uint64_t revision)
{
    auto &frame = frames_[index];
    if (pixels.empty() || frame.textureRevision == revision)
        return true;
    if (pixels.size_bytes() > textureBytes_)
        return false;
    void *destination = nullptr;
    if (vk_->vkMapMemory(device_,frame.textureMemory,0,pixels.size_bytes(),0,&destination) != VK_SUCCESS)
        return false;
    std::memcpy(destination,pixels.data(),pixels.size_bytes());
    vk_->vkUnmapMemory(device_,frame.textureMemory);
    frame.textureRevision = revision;
    return true;
}

void WorldLightingVulkan::destroy()
{
    if (!vk_)
        return;
    if (pool_)
        vk_->vkDestroyDescriptorPool(device_,pool_,nullptr);
    for (const auto &frame : frames_) {
        if (frame.framebuffer) vk_->vkDestroyFramebuffer(device_,frame.framebuffer,nullptr);
        if (frame.view) vk_->vkDestroyImageView(device_,frame.view,nullptr);
        if (frame.image) vk_->vkDestroyImage(device_,frame.image,nullptr);
        if (frame.imageMemory) vk_->vkFreeMemory(device_,frame.imageMemory,nullptr);
        if (frame.uniform) vk_->vkDestroyBuffer(device_,frame.uniform,nullptr);
        if (frame.uniformMemory) vk_->vkFreeMemory(device_,frame.uniformMemory,nullptr);
        if (frame.textures) vk_->vkDestroyBuffer(device_,frame.textures,nullptr);
        if (frame.textureMemory) vk_->vkFreeMemory(device_,frame.textureMemory,nullptr);
    }
    if (sampler_) vk_->vkDestroySampler(device_,sampler_,nullptr);
    if (pass_) vk_->vkDestroyRenderPass(device_,pass_,nullptr);
    if (layout_) vk_->vkDestroyDescriptorSetLayout(device_,layout_,nullptr);
    *this = {};
}
#endif
