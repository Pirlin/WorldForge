#include "Engine/Render/Vulkan/HdrFrame.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>

namespace worldforge::vk {
namespace {
bool srgbFormat(VkFormat format)
{
    return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB
        || format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

// The radical inverse of index in base, for the Halton jitter sequence.
float halton(uint32_t index,uint32_t base)
{
    float fraction = 1, result = 0;
    while (index > 0) {
        fraction /= float(base);
        result += fraction*float(index%base);
        index /= base;
    }
    return result;
}

constexpr auto fragmentTests = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
} // namespace

bool HdrFrame::create(const Device &device,VkFormat outputFormat,VkImageLayout outputLayout,uint32_t meterSlots)
{
    device_ = &device;
    const auto &vk = *device.api;
    // TAA samples the scene depth, so the depth format must also be readable by shaders.
    for (const auto candidate : {VK_FORMAT_D32_SFLOAT,VK_FORMAT_X8_D24_UNORM_PACK32,VK_FORMAT_D16_UNORM})
        if (device.supports(candidate,VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
            depthFormat_ = candidate;
            break;
        }
    VkPhysicalDeviceFeatures features{};
    vk.vkGetPhysicalDeviceFeatures(device.physical,&features);
    // The display pass writes the auto exposure histogram, which needs fragment shader atomics.
    if (depthFormat_ == VK_FORMAT_UNDEFINED || !features.fragmentStoresAndAtomics
        || !device.supports(sceneFormat,VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
                                        | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
        || !device.supports(occlusionFormat,VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
        || !device.supports(albedoFormat,VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
        || !device.supports(velocityFormat,VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
        return false;
    srgbOutput_ = srgbFormat(outputFormat);

    // Scene pass: the lit scene, its sky-light part, the albedo, each surface's own motion and depth, all read by the
    // passes after it.
    {
        const VkAttachmentDescription attachments[]{
            {0,sceneFormat,VK_SAMPLE_COUNT_1_BIT,VK_ATTACHMENT_LOAD_OP_CLEAR,VK_ATTACHMENT_STORE_OP_STORE,
             VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,
             VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {0,sceneFormat,VK_SAMPLE_COUNT_1_BIT,VK_ATTACHMENT_LOAD_OP_CLEAR,VK_ATTACHMENT_STORE_OP_STORE,
             VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,
             VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {0,albedoFormat,VK_SAMPLE_COUNT_1_BIT,VK_ATTACHMENT_LOAD_OP_CLEAR,VK_ATTACHMENT_STORE_OP_STORE,
             VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,
             VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {0,velocityFormat,VK_SAMPLE_COUNT_1_BIT,VK_ATTACHMENT_LOAD_OP_CLEAR,VK_ATTACHMENT_STORE_OP_STORE,
             VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,
             VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {0,depthFormat_,VK_SAMPLE_COUNT_1_BIT,VK_ATTACHMENT_LOAD_OP_CLEAR,VK_ATTACHMENT_STORE_OP_STORE,
             VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,
             VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}
        };
        const VkAttachmentReference colors[]{{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                             {1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                             {2,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                             {3,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
        const VkAttachmentReference depth{sceneColorAttachments,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        const VkSubpassDescription subpass{0,VK_PIPELINE_BIND_POINT_GRAPHICS,0,nullptr,sceneColorAttachments,colors,nullptr,
            &depth,0,nullptr};
        const VkSubpassDependency dependencies[]{
            // The previous frame's display pass read the shared scene and depth targets.
            {VK_SUBPASS_EXTERNAL,0,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | fragmentTests,
             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | fragmentTests,0,
             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                 | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,0},
            // The display pass samples what this pass wrote.
            {0,VK_SUBPASS_EXTERNAL,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | fragmentTests,
             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | fragmentTests,
             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
             VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,0}
        };
        const VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,nullptr,0,uint32_t(std::size(attachments)),attachments,1,&subpass,
            uint32_t(std::size(dependencies)),dependencies};
        if (vk.vkCreateRenderPass(device.device,&info,nullptr,&scenePass_) != VK_SUCCESS)
            return false;
    }
    // Occlusion pass: ambient occlusion and bounce light from the scene depth and color.
    {
        const VkAttachmentDescription attachment{0,occlusionFormat,VK_SAMPLE_COUNT_1_BIT,VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            VK_ATTACHMENT_STORE_OP_STORE,VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,
            VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        const VkSubpassDescription subpass{0,VK_PIPELINE_BIND_POINT_GRAPHICS,0,nullptr,1,&color,nullptr,nullptr,0,nullptr};
        const VkSubpassDependency dependencies[]{
            // The scene pass wrote the depth; the previous frame's TAA read this target.
            {VK_SUBPASS_EXTERNAL,0,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | fragmentTests,
             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,0},
            {0,VK_SUBPASS_EXTERNAL,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,0}
        };
        const VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,nullptr,0,1,&attachment,1,&subpass,
            uint32_t(std::size(dependencies)),dependencies};
        if (vk.vkCreateRenderPass(device.device,&info,nullptr,&occlusionPass_) != VK_SUCCESS)
            return false;
    }
    // Display pass: the display step into the output, reading the final HDR image; the editor then draws its overlays,
    // depth-tested against the scene.
    {
        const VkAttachmentDescription attachments[]{
            {0,outputFormat,VK_SAMPLE_COUNT_1_BIT,VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_STORE,
             VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,VK_IMAGE_LAYOUT_UNDEFINED,outputLayout},
            {0,depthFormat_,VK_SAMPLE_COUNT_1_BIT,VK_ATTACHMENT_LOAD_OP_LOAD,VK_ATTACHMENT_STORE_OP_DONT_CARE,
             VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,
             VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}
        };
        const VkAttachmentReference output{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        const VkAttachmentReference depth{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        const VkSubpassDescription subpass{0,VK_PIPELINE_BIND_POINT_GRAPHICS,0,nullptr,1,&output,nullptr,&depth,0,nullptr};
        const VkSubpassDependency dependencies[]{
            // The output becomes writable once the acquire semaphore signals or a previous copy finishes; the passes
            // before wrote the image the display step reads and the depth the overlays test against.
            {VK_SUBPASS_EXTERNAL,0,
             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | fragmentTests,
             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | fragmentTests,
             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,0},
            {0,VK_SUBPASS_EXTERNAL,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
             VK_ACCESS_TRANSFER_READ_BIT,0}
        };
        const VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,nullptr,0,2,attachments,1,&subpass,
            uint32_t(std::size(dependencies)),dependencies};
        if (vk.vkCreateRenderPass(device.device,&info,nullptr,&displayPass_) != VK_SUCCESS)
            return false;
    }
    // Bloom passes: the downsample writes a level whole; the upsample adds to what the level holds.
    for (const bool upsample : {false,true}) {
        const VkAttachmentDescription attachment{0,sceneFormat,VK_SAMPLE_COUNT_1_BIT,
            upsample ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_STORE,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,
            upsample ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        const VkSubpassDescription subpass{0,VK_PIPELINE_BIND_POINT_GRAPHICS,0,nullptr,1,&color,nullptr,nullptr,0,nullptr};
        const VkSubpassDependency dependencies[]{
            // The previous pass wrote the image this one reads; earlier passes read the image this one writes.
            {VK_SUBPASS_EXTERNAL,0,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                 | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,0},
            {0,VK_SUBPASS_EXTERNAL,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,0}
        };
        const VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,nullptr,0,1,&attachment,1,&subpass,
            uint32_t(std::size(dependencies)),dependencies};
        if (vk.vkCreateRenderPass(device.device,&info,nullptr,upsample ? &bloomUpPass_ : &bloomDownPass_) != VK_SUCCESS)
            return false;
    }

    const VkDescriptorSetLayoutBinding displayBindings[]{
        {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {2,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
    const VkDescriptorSetLayoutBinding bloomBinding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,
        nullptr};
    const VkDescriptorSetLayoutCreateInfo bloomLayout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,nullptr,0,1,
        &bloomBinding};
    if (vk.vkCreateDescriptorSetLayout(device.device,&bloomLayout,nullptr,&bloomSetLayout_) != VK_SUCCESS)
        return false;
    const VkDescriptorSetLayoutBinding postBindings[]{
        {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {2,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
    const VkDescriptorSetLayoutCreateInfo postLayout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,nullptr,0,3,
        postBindings};
    if (vk.vkCreateDescriptorSetLayout(device.device,&postLayout,nullptr,&postSetLayout_) != VK_SUCCESS)
        return false;
    const VkDescriptorSetLayoutBinding temporalBindings[]{
        {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {2,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {3,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {4,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {5,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {6,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
    const VkDescriptorSetLayoutBinding occlusionBindings[]{
        {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
    const VkDescriptorSetLayoutCreateInfo displayLayout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,nullptr,0,3,
        displayBindings};
    const VkDescriptorSetLayoutCreateInfo temporalLayout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,nullptr,0,7,
        temporalBindings};
    const VkDescriptorSetLayoutCreateInfo occlusionLayout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,nullptr,0,2,
        occlusionBindings};
    if (vk.vkCreateDescriptorSetLayout(device.device,&displayLayout,nullptr,&displaySetLayout_) != VK_SUCCESS
        || vk.vkCreateDescriptorSetLayout(device.device,&temporalLayout,nullptr,&temporalSetLayout_) != VK_SUCCESS
        || vk.vkCreateDescriptorSetLayout(device.device,&occlusionLayout,nullptr,&occlusionSetLayout_) != VK_SUCCESS)
        return false;
    const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,4},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,46+bloomLevels+2}};
    const VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,nullptr,0,11+bloomLevels+2,2,sizes};
    if (vk.vkCreateDescriptorPool(device.device,&pool,nullptr,&pool_) != VK_SUCCESS)
        return false;
    const std::array displayLayouts{displaySetLayout_,displaySetLayout_,displaySetLayout_,displaySetLayout_};
    const std::array temporalLayouts{temporalSetLayout_,temporalSetLayout_};
    const VkDescriptorSetAllocateInfo displayAllocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,nullptr,pool_,4,
        displayLayouts.data()};
    const VkDescriptorSetAllocateInfo temporalAllocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,nullptr,pool_,2,
        temporalLayouts.data()};
    const VkDescriptorSetAllocateInfo occlusionAllocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,nullptr,pool_,1,
        &occlusionSetLayout_};
    if (vk.vkAllocateDescriptorSets(device.device,&displayAllocation,displaySets_.data()) != VK_SUCCESS
        || vk.vkAllocateDescriptorSets(device.device,&temporalAllocation,temporalSets_.data()) != VK_SUCCESS
        || vk.vkAllocateDescriptorSets(device.device,&occlusionAllocation,&occlusionSet_) != VK_SUCCESS)
        return false;
    std::array<VkDescriptorSetLayout,bloomLevels+2> bloomLayouts;
    bloomLayouts.fill(bloomSetLayout_);
    const VkDescriptorSetAllocateInfo bloomAllocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,nullptr,pool_,
        uint32_t(bloomLayouts.size()),bloomLayouts.data()};
    if (vk.vkAllocateDescriptorSets(device.device,&bloomAllocation,bloomSets_.data()) != VK_SUCCESS)
        return false;
    const std::array postLayouts{postSetLayout_,postSetLayout_,postSetLayout_,postSetLayout_};
    const VkDescriptorSetAllocateInfo postAllocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,nullptr,pool_,
        uint32_t(postLayouts.size()),postLayouts.data()};
    if (vk.vkAllocateDescriptorSets(device.device,&postAllocation,postSets_.data()) != VK_SUCCESS)
        return false;
    meterSlots_ = std::max(meterSlots,1u);
    if (!meter_.create(device,VkDeviceSize(meterSlots_)*meterBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
        return false;
    std::fill_n(static_cast<std::uint32_t *>(meter_.mapped),meterSlots_*color::meterBins,0u);
    for (const auto set : displaySets_) {
        const VkDescriptorBufferInfo meter{meter_.buffer,0,meterBytes};
        const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,set,1,0,1,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,nullptr,&meter,nullptr};
        vk.vkUpdateDescriptorSets(device.device,1,&write,0,nullptr);
    }
    for (const auto filter : {VK_FILTER_NEAREST,VK_FILTER_LINEAR}) {
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = sampler.minFilter = filter;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vk.vkCreateSampler(device.device,&sampler,nullptr,filter == VK_FILTER_NEAREST ? &nearest_ : &linear_) != VK_SUCCESS)
            return false;
    }
    const VkPushConstantRange displaySettings{VK_SHADER_STAGE_FRAGMENT_BIT,0,8*sizeof(float)};
    const VkPushConstantRange temporalSettings{VK_SHADER_STAGE_FRAGMENT_BIT,0,20*sizeof(float)};
    const VkPipelineLayoutCreateInfo displayPipeline{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,nullptr,0,
        1,&displaySetLayout_,1,&displaySettings};
    const VkPipelineLayoutCreateInfo temporalPipeline{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,nullptr,0,
        1,&temporalSetLayout_,1,&temporalSettings};
    const VkPipelineLayoutCreateInfo occlusionPipeline{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,nullptr,0,
        1,&occlusionSetLayout_,1,&temporalSettings};
    const VkPipelineLayoutCreateInfo bloomPipeline{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,nullptr,0,
        1,&bloomSetLayout_,1,&displaySettings};
    const VkPushConstantRange postSettings{VK_SHADER_STAGE_FRAGMENT_BIT,0,32*sizeof(float)};
    const VkPipelineLayoutCreateInfo postPipeline{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,nullptr,0,
        1,&postSetLayout_,1,&postSettings};
    return vk.vkCreatePipelineLayout(device.device,&displayPipeline,nullptr,&displayLayout_) == VK_SUCCESS
        && vk.vkCreatePipelineLayout(device.device,&temporalPipeline,nullptr,&temporalLayout_) == VK_SUCCESS
        && vk.vkCreatePipelineLayout(device.device,&occlusionPipeline,nullptr,&occlusionLayout_) == VK_SUCCESS
        && vk.vkCreatePipelineLayout(device.device,&bloomPipeline,nullptr,&bloomLayout_) == VK_SUCCESS
        && vk.vkCreatePipelineLayout(device.device,&postPipeline,nullptr,&postLayout_) == VK_SUCCESS;
}

void HdrFrame::destroy()
{
    if (!device_)
        return;
    const auto &vk = *device_->api;
    const auto device = device_->device;
    releaseTargets();
    destroyDisplayPipeline();
    for (const auto layout : {displayLayout_,temporalLayout_,occlusionLayout_,bloomLayout_,postLayout_})
        if (layout)
            vk.vkDestroyPipelineLayout(device,layout,nullptr);
    for (const auto sampler : {nearest_,linear_})
        if (sampler)
            vk.vkDestroySampler(device,sampler,nullptr);
    meter_.destroy(*device_);
    meterSlots_ = 0;
    if (pool_)
        vk.vkDestroyDescriptorPool(device,pool_,nullptr);
    for (const auto layout : {displaySetLayout_,temporalSetLayout_,occlusionSetLayout_,bloomSetLayout_,postSetLayout_})
        if (layout)
            vk.vkDestroyDescriptorSetLayout(device,layout,nullptr);
    for (const auto pass : {scenePass_,occlusionPass_,displayPass_,bloomDownPass_,bloomUpPass_})
        if (pass)
            vk.vkDestroyRenderPass(device,pass,nullptr);
    displayLayout_ = temporalLayout_ = occlusionLayout_ = bloomLayout_ = postLayout_ = VK_NULL_HANDLE;
    occlusionSet_ = VK_NULL_HANDLE;
    nearest_ = linear_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    displaySets_ = {};
    temporalSets_ = {};
    displaySetLayout_ = temporalSetLayout_ = occlusionSetLayout_ = bloomSetLayout_ = postSetLayout_ = VK_NULL_HANDLE;
    postSets_ = {};
    scenePass_ = occlusionPass_ = displayPass_ = bloomDownPass_ = bloomUpPass_ = VK_NULL_HANDLE;
    bloomSets_ = {};
    device_ = nullptr;
}

bool HdrFrame::createTargets(VkExtent2D extent,std::span<const VkImageView> outputs)
{
    releaseTargets();
    if (!scenePass_ || !displayPass_ || outputs.empty())
        return false;
    const auto &vk = *device_->api;
    extent_ = {std::max(extent.width,1u),std::max(extent.height,1u)};
    if (!scene_.create(*device_,sceneFormat,extent_,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT)
        || !indirect_.create(*device_,sceneFormat,extent_,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT)
        || !albedo_.create(*device_,albedoFormat,extent_,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT)
        || !velocity_.create(*device_,velocityFormat,extent_,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT)
        || !occlusion_.create(*device_,occlusionFormat,extent_,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT)
        || !depth_.create(*device_,depthFormat_,extent_,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          VK_IMAGE_ASPECT_DEPTH_BIT))
        return false;
    for (auto &history : history_)
        if (!history.create(*device_,sceneFormat,extent_,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT
                            | VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_COLOR_BIT))
            return false;
    historyReady_ = {};
    resetHistory();
    const auto framebuffer = [&](VkRenderPass pass,std::span<const VkImageView> views,VkFramebuffer &result) {
        const VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,nullptr,0,pass,
            uint32_t(views.size()),views.data(),extent_.width,extent_.height,1};
        return vk.vkCreateFramebuffer(device_->device,&info,nullptr,&result) == VK_SUCCESS;
    };
    const std::array sceneViews{scene_.view,indirect_.view,albedo_.view,velocity_.view,depth_.view};
    if (!framebuffer(scenePass_,sceneViews,sceneFramebuffer_)
        || !framebuffer(occlusionPass_,std::span(&occlusion_.view,1),occlusionFramebuffer_))
        return false;
    // TAA writes each history image in turn (with the single-target pass the bloom downsample also uses).
    for (std::size_t i = 0; i < history_.size(); ++i)
        if (!framebuffer(bloomDownPass_,std::span(&history_[i].view,1),temporalFramebuffers_[i]))
            return false;
    for (const auto output : outputs) {
        const std::array views{output,depth_.view};
        VkFramebuffer result{};
        if (!framebuffer(displayPass_,views,result))
            return false;
        displayFramebuffers_.push_back(result);
    }
    for (std::size_t i = 0; i < 2; ++i) {
        const VkDescriptorImageInfo input{nearest_,history_[i].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo images[]{
            {nearest_,scene_.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {nearest_,depth_.view,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            {linear_,history_[1-i].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {nearest_,indirect_.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {nearest_,occlusion_.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {nearest_,albedo_.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {nearest_,velocity_.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        const VkWriteDescriptorSet writes[]{
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,displaySets_[i],0,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             &input,nullptr,nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,temporalSets_[i],0,0,7,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             images,nullptr,nullptr}};
        vk.vkUpdateDescriptorSets(device_->device,2,writes,0,nullptr);
    }
    const VkDescriptorImageInfo occlusionImages[]{{nearest_,depth_.view,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
        {nearest_,scene_.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    const VkWriteDescriptorSet depthWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,occlusionSet_,0,0,2,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,occlusionImages,nullptr,nullptr};
    vk.vkUpdateDescriptorSets(device_->device,1,&depthWrite,0,nullptr);

    // Bloom levels: half size, then halving down to bloomLevels images (at least one pixel each).
    VkExtent2D size{std::max(extent_.width/2,1u),std::max(extent_.height/2,1u)};
    for (int level = 0; level < bloomLevels; ++level) {
        bloomExtents_[std::size_t(level)] = size;
        auto &image = bloom_[std::size_t(level)];
        if (!image.create(*device_,sceneFormat,size,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT))
            return false;
        const VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,nullptr,0,bloomDownPass_,1,&image.view,
            size.width,size.height,1};
        if (vk.vkCreateFramebuffer(device_->device,&info,nullptr,&bloomFramebuffers_[std::size_t(level)]) != VK_SUCCESS)
            return false;
        size = {std::max(size.width/2,1u),std::max(size.height/2,1u)};
    }
    for (std::size_t i = 0; i < bloomSets_.size(); ++i) {
        const auto view = i < std::size_t(bloomLevels) ? bloom_[i].view : history_[i-std::size_t(bloomLevels)].view;
        const VkDescriptorImageInfo image{linear_,view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,bloomSets_[i],0,0,1,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&image,nullptr,nullptr};
        vk.vkUpdateDescriptorSets(device_->device,1,&write,0,nullptr);
    }
    for (const auto set : displaySets_) {
        const VkDescriptorImageInfo image{linear_,bloom_[0].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,set,2,0,1,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&image,nullptr,nullptr};
        vk.vkUpdateDescriptorSets(device_->device,1,&write,0,nullptr);
    }
    // Camera effects: two full-size targets, and for each of the four images a frame can end in (history 0 and 1,
    // post 0 and 1), a set the effects read it through and a set the display step reads it through.
    for (std::size_t i = 0; i < post_.size(); ++i)
        if (!post_[i].create(*device_,sceneFormat,extent_,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT)
            || !framebuffer(bloomDownPass_,std::span(&post_[i].view,1),postFramebuffers_[i]))
            return false;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto view = i < 2 ? history_[i].view : post_[i-2].view;
        const VkDescriptorImageInfo images[]{{linear_,view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                             {nearest_,depth_.view,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
                                             {nearest_,velocity_.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        const VkDescriptorImageInfo final{nearest_,view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkWriteDescriptorSet writes[]{
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,postSets_[i],0,0,3,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             images,nullptr,nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,displaySets_[i],0,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             &final,nullptr,nullptr}};
        vk.vkUpdateDescriptorSets(device_->device,2,writes,0,nullptr);
    }
    bloomInitialized_ = false;
    framesShown_ = 0;
    return true;
}

void HdrFrame::releaseTargets()
{
    if (!device_)
        return;
    const auto &vk = *device_->api;
    for (const auto framebuffer : displayFramebuffers_)
        vk.vkDestroyFramebuffer(device_->device,framebuffer,nullptr);
    displayFramebuffers_.clear();
    for (const auto framebuffer : {sceneFramebuffer_,occlusionFramebuffer_})
        if (framebuffer)
            vk.vkDestroyFramebuffer(device_->device,framebuffer,nullptr);
    for (auto &framebuffer : temporalFramebuffers_) {
        if (framebuffer)
            vk.vkDestroyFramebuffer(device_->device,framebuffer,nullptr);
        framebuffer = VK_NULL_HANDLE;
    }
    sceneFramebuffer_ = occlusionFramebuffer_ = VK_NULL_HANDLE;
    scene_.destroy(*device_);
    indirect_.destroy(*device_);
    albedo_.destroy(*device_);
    velocity_.destroy(*device_);
    occlusion_.destroy(*device_);
    depth_.destroy(*device_);
    for (auto &history : history_)
        history.destroy(*device_);
    historyReady_ = {};
    for (auto &framebuffer : bloomFramebuffers_) {
        if (framebuffer)
            vk.vkDestroyFramebuffer(device_->device,framebuffer,nullptr);
        framebuffer = VK_NULL_HANDLE;
    }
    for (auto &image : bloom_)
        image.destroy(*device_);
    for (auto &framebuffer : postFramebuffers_) {
        if (framebuffer)
            vk.vkDestroyFramebuffer(device_->device,framebuffer,nullptr);
        framebuffer = VK_NULL_HANDLE;
    }
    for (auto &image : post_)
        image.destroy(*device_);
    bloomInitialized_ = false;
    framesShown_ = 0;
}

bool HdrFrame::createPipeline(VkShaderModule vertex,VkShaderModule fragment,VkPipelineLayout layout,VkRenderPass pass,
                              uint32_t subpass,VkPipeline &pipeline,bool additive)
{
    const auto &vk = *device_->api;
    const VkPipelineShaderStageCreateInfo stages[]{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,vertex,"main",nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,fragment,"main",nullptr}};
    const VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        nullptr,0,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,VK_FALSE};
    const VkPipelineViewportStateCreateInfo view{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        nullptr,0,1,nullptr,1,nullptr};
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
        | VK_COLOR_COMPONENT_A_BIT;
    if (additive) {
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.srcAlphaBlendFactor = blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.colorBlendOp = blend.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo colors{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colors.attachmentCount = 1;
    colors.pAttachments = &blend;
    const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,nullptr,0,2,states};
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &view;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &samples;
    info.pDepthStencilState = pass == displayPass_ && subpass == displaySubpass ? &depth : nullptr;
    info.pColorBlendState = &colors;
    info.pDynamicState = &dynamic;
    info.layout = layout;
    info.renderPass = pass;
    info.subpass = subpass;
    if (vk.vkCreateGraphicsPipelines(device_->device,VK_NULL_HANDLE,1,&info,nullptr,&pipeline) != VK_SUCCESS)
        pipeline = VK_NULL_HANDLE;
    return pipeline != VK_NULL_HANDLE;
}

bool HdrFrame::createDisplayPipeline(std::span<const uint32_t> vertexCode,std::span<const uint32_t> fragmentCode,
                                     std::span<const uint32_t> temporalCode,std::span<const uint32_t> ambientCode,
                                     std::span<const uint32_t> bloomDownCode,std::span<const uint32_t> bloomUpCode,
                                     std::span<const uint32_t> depthOfFieldCode,std::span<const uint32_t> motionBlurCode)
{
    destroyDisplayPipeline();
    if (!displayLayout_ || !temporalLayout_ || !occlusionLayout_ || !bloomLayout_ || !postLayout_)
        return false;
    const auto &vk = *device_->api;
    const auto vertex = device_->shader(vertexCode), fragment = device_->shader(fragmentCode),
        temporal = device_->shader(temporalCode), ambient = device_->shader(ambientCode),
        bloomDown = device_->shader(bloomDownCode), bloomUp = device_->shader(bloomUpCode),
        depthOfField = device_->shader(depthOfFieldCode), motionBlur = device_->shader(motionBlurCode);
    const bool created = vertex && fragment && temporal && ambient && bloomDown && bloomUp && depthOfField && motionBlur
        && createPipeline(vertex,fragment,displayLayout_,displayPass_,displaySubpass,display_)
        && createPipeline(vertex,temporal,temporalLayout_,bloomDownPass_,0,temporal_)
        && createPipeline(vertex,ambient,occlusionLayout_,occlusionPass_,0,ambient_)
        && createPipeline(vertex,bloomDown,bloomLayout_,bloomDownPass_,0,bloomDown_)
        && createPipeline(vertex,bloomUp,bloomLayout_,bloomUpPass_,0,bloomUp_,true)
        && createPipeline(vertex,depthOfField,postLayout_,bloomDownPass_,0,depthOfField_)
        && createPipeline(vertex,motionBlur,postLayout_,bloomDownPass_,0,motionBlur_);
    for (const auto module : {vertex,fragment,temporal,ambient,bloomDown,bloomUp,depthOfField,motionBlur})
        if (module)
            vk.vkDestroyShaderModule(device_->device,module,nullptr);
    if (!created)
        destroyDisplayPipeline();
    return created;
}

void HdrFrame::destroyDisplayPipeline()
{
    for (auto *pipeline : {&display_,&temporal_,&ambient_,&bloomDown_,&bloomUp_,&depthOfField_,&motionBlur_}) {
        if (*pipeline && device_)
            device_->api->vkDestroyPipeline(device_->device,*pipeline,nullptr);
        *pipeline = VK_NULL_HANDLE;
    }
}

std::array<float,2> HdrFrame::jitter() const
{
    const uint32_t index = sample_%temporalSamples+1;
    return {halton(index,2)-0.5f,halton(index,3)-0.5f};
}

void HdrFrame::resetHistory()
{
    accumulated_ = 0;
    sample_ = 0;
}

void HdrFrame::initializeLayouts(VkCommandBuffer command)
{
    const auto &vk = *device_->api;
    const auto ready = [&](VkImage image) {
        const VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,nullptr,0,VK_ACCESS_SHADER_READ_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED,image,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        vk.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,
                                0,nullptr,0,nullptr,1,&barrier);
    };
    // The history images: the scene pass reads the previous one (previousFrame) from the first frame on.
    for (std::size_t i = 0; i < history_.size(); ++i)
        if (!historyReady_[i]) {
            ready(history_[i].image);
            historyReady_[i] = true;
        }
    // The bloom levels, which the display step's descriptors name even when bloom is off.
    if (!bloomInitialized_) {
        for (const auto &image : bloom_)
            ready(image.image);
        bloomInitialized_ = true;
    }
}

void HdrFrame::begin(VkCommandBuffer command,std::size_t output,const float background[3])
{
    const auto &vk = *device_->api;
    initializeLayouts(command);
    output_ = output;
    VkClearValue clear[sceneColorAttachments+1]{};
    clear[0].color = {{background[0],background[1],background[2],1.0f}};
    clear[sceneColorAttachments].depthStencil = {1.0f,0};
    const VkRect2D area{{0,0},extent_};
    const VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,nullptr,scenePass_,sceneFramebuffer_,area,sceneColorAttachments+1,clear};
    vk.vkCmdBeginRenderPass(command,&info,VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0,0,float(extent_.width),float(extent_.height),0,1};
    vk.vkCmdSetViewport(command,0,1,&viewport);
    vk.vkCmdSetScissor(command,0,1,&area);
}

void HdrFrame::bloomChain(VkCommandBuffer command)
{
    const auto &vk = *device_->api;
    const auto draw = [&](VkRenderPass pass,int level,VkPipeline pipeline,VkDescriptorSet source,VkExtent2D sourceSize,
                          bool first) {
        const auto size = bloomExtents_[std::size_t(level)];
        const VkRect2D area{{0,0},size};
        const VkViewport viewport{0,0,float(size.width),float(size.height),0,1};
        const VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,nullptr,pass,
            bloomFramebuffers_[std::size_t(level)],area,0,nullptr};
        vk.vkCmdBeginRenderPass(command,&info,VK_SUBPASS_CONTENTS_INLINE);
        vk.vkCmdSetViewport(command,0,1,&viewport);
        vk.vkCmdSetScissor(command,0,1,&area);
        const float settings[8]{1.0f/float(sourceSize.width),1.0f/float(sourceSize.height),first ? 1.0f : 0.0f,0,
                                float(size.width),float(size.height),0,0};
        vk.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        vk.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,bloomLayout_,0,1,&source,0,nullptr);
        vk.vkCmdPushConstants(command,bloomLayout_,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(settings),settings);
        vk.vkCmdDraw(command,3,1,0,0);
        vk.vkCmdEndRenderPass(command);
    };
    // Down from the previous frame's resolve (history_[1 - parity_]), then back up, each level adding the one below.
    const auto previous = bloomSets_[std::size_t(bloomLevels)+(1-parity_)];
    for (int level = 0; level < bloomLevels; ++level)
        draw(bloomDownPass_,level,bloomDown_,level == 0 ? previous : bloomSets_[std::size_t(level-1)],
             level == 0 ? extent_ : bloomExtents_[std::size_t(level-1)],level == 0);
    for (int level = bloomLevels-2; level >= 0; --level)
        draw(bloomUpPass_,level,bloomUp_,bloomSets_[std::size_t(level+1)],bloomExtents_[std::size_t(level+1)],false);
}

void HdrFrame::display(VkCommandBuffer command,float exposureStops,color::ViewTransform transform,int meterSlot,
                       const Temporal &temporal,const Occlusion &occlusion,float bloom,const Camera &camera)
{
    const auto &vk = *device_->api;
    vk.vkCmdEndRenderPass(command);
    const std::size_t framebuffer = output_;
    if (framebuffer >= displayFramebuffers_.size() || !display_ || !temporal_ || !ambient_ || !bloomDown_ || !bloomUp_)
        return;
    const VkRect2D area{{0,0},extent_};
    const VkViewport viewport{0,0,float(extent_.width),float(extent_.height),0,1};

    // Trace the more expensive screen-space GI at half resolution. Temporal.frag reconstructs it
    // with depth rejection. Ambient occlusion alone retains its full-resolution path.
    {
        const VkExtent2D ambientSize = occlusion.screenSpaceGi
            ? VkExtent2D{(extent_.width+1)/2,(extent_.height+1)/2} : extent_;
        const VkRect2D ambientArea{{0,0},ambientSize};
        const VkViewport ambientViewport{0,0,float(ambientSize.width),float(ambientSize.height),0,1};
        const VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,nullptr,occlusionPass_,occlusionFramebuffer_,
            ambientArea,0,nullptr};
        vk.vkCmdBeginRenderPass(command,&info,VK_SUBPASS_CONTENTS_INLINE);
        vk.vkCmdSetViewport(command,0,1,&ambientViewport);
        vk.vkCmdSetScissor(command,0,1,&ambientArea);
        struct { std::array<float,16> inverseViewProjection; float settings[4]; } constants{occlusion.inverseViewProjection,
            {occlusion.radius,(occlusion.enabled ? 1.0f : 0.0f)+(occlusion.screenSpaceGi ? 2.0f : 0.0f)+(temporal.enabled ? 4.0f : 0.0f),
             float(sample_%temporalSamples),occlusion.probeGi ? 1.0f : 0.0f}};
        vk.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,ambient_);
        vk.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,occlusionLayout_,0,1,&occlusionSet_,0,nullptr);
        vk.vkCmdPushConstants(command,occlusionLayout_,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(constants),&constants);
        vk.vkCmdDraw(command,3,1,0,0);
        vk.vkCmdEndRenderPass(command);
    }
    // Bloom, from the previous frame's resolve once there is one.
    const bool bloomShown = bloom > 0 && framesShown_ > 0;
    if (bloomShown)
        bloomChain(command);
    // TAA: the first frame after a reset shows only itself; a still view averages its first frames evenly
    // and then holds; a moving view keeps blending in a sixteenth of each new frame.
    {
        const VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,nullptr,bloomDownPass_,
            temporalFramebuffers_[parity_],area,0,nullptr};
        vk.vkCmdBeginRenderPass(command,&info,VK_SUBPASS_CONTENTS_INLINE);
        vk.vkCmdSetViewport(command,0,1,&viewport);
        vk.vkCmdSetScissor(command,0,1,&area);
        const bool blend = temporal.enabled && accumulated_ > 0;
        float weight = 1;
        if (blend)
            weight = accumulated_ < temporalSamples ? 1.0f/float(accumulated_+1) : temporal.still ? 0.0f : 1.0f/temporalSamples;
        struct { std::array<float,16> reprojection; float settings[4]; } constants{temporal.reprojection,
            {weight,blend ? 1.0f : 0.0f,temporal.still ? 0.0f : 1.0f,
             occlusion.screenSpaceGi ? 2.0f : occlusion.enabled ? 1.0f : 0.0f}};
        vk.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,temporal_);
        vk.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,temporalLayout_,0,1,&temporalSets_[parity_],0,nullptr);
        vk.vkCmdPushConstants(command,temporalLayout_,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(constants),&constants);
        vk.vkCmdDraw(command,3,1,0,0);
        vk.vkCmdEndRenderPass(command);
    }

    // Camera effects, each from the image so far into the next post target. source: 0 or 1 a history image, 2 or 3
    // a post target.
    std::size_t source = parity_;
    const auto effect = [&](VkPipeline pipeline,const void *constants,uint32_t size) {
        const std::size_t target = source == 2 ? 1 : 0;
        const VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,nullptr,bloomDownPass_,
            postFramebuffers_[target],area,0,nullptr};
        vk.vkCmdBeginRenderPass(command,&info,VK_SUBPASS_CONTENTS_INLINE);
        vk.vkCmdSetViewport(command,0,1,&viewport);
        vk.vkCmdSetScissor(command,0,1,&area);
        vk.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        vk.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,postLayout_,0,1,&postSets_[source],0,nullptr);
        vk.vkCmdPushConstants(command,postLayout_,VK_SHADER_STAGE_FRAGMENT_BIT,0,size,constants);
        vk.vkCmdDraw(command,3,1,0,0);
        vk.vkCmdEndRenderPass(command);
        source = 2+target;
    };
    if (camera.depthOfField && depthOfField_) {
        // A thin lens on a 24 mm tall sensor: the focal length that gives this field of view, and the aperture it has
        // at this f-number.
        constexpr float sensorHeight = 0.024f;
        const float focalLength = sensorHeight/(2*std::tan(std::clamp(camera.verticalFov,0.01f,3.0f)/2));
        struct { std::array<float,16> inverseViewProjection; float eye[4], forward[4], lens[4]; } constants{
            camera.inverseViewProjection,{camera.eye[0],camera.eye[1],camera.eye[2],0},
            {camera.forward[0],camera.forward[1],camera.forward[2],std::max(camera.focusDistance,focalLength*1.01f)},
            {focalLength,focalLength/std::max(camera.fNumber,0.5f),float(extent_.height)/sensorHeight,24}};
        effect(depthOfField_,&constants,sizeof(constants));
    }
    if (camera.motionBlur && motionBlur_ && accumulated_ > 0) {
        struct { std::array<float,16> reprojection; float settings[4]; } constants{temporal.reprojection,
            {std::clamp(camera.shutter,0.0f,1.0f),32,0,0}};
        effect(motionBlur_,&constants,sizeof(constants));
    }

    const VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,nullptr,displayPass_,
        displayFramebuffers_[framebuffer],area,0,nullptr};
    vk.vkCmdBeginRenderPass(command,&info,VK_SUBPASS_CONTENTS_INLINE);
    vk.vkCmdSetViewport(command,0,1,&viewport);
    vk.vkCmdSetScissor(command,0,1,&area);
    const bool metering = meterSlot >= 0 && uint32_t(meterSlot) < meterSlots_;
    const float settings[8]{color::exposureScale(exposureStops),transform == color::ViewTransform::AgX ? 1.0f : 0.0f,
        srgbOutput_ ? 1.0f : 0.0f,metering ? 1.0f : 0.0f,bloomShown ? bloom : 0.0f,float(bloomLevels),
        color::meterLowStops,color::meterHighStops};
    const uint32_t offset = metering ? uint32_t(meterSlot)*meterBytes : 0;
    vk.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,display_);
    vk.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,displayLayout_,0,1,&displaySets_[source],1,&offset);
    vk.vkCmdPushConstants(command,displayLayout_,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(settings),settings);
    vk.vkCmdDraw(command,3,1,0,0);

    parity_ ^= 1;
    ++framesShown_;
    if (temporal.enabled) {
        accumulated_ = std::min(accumulated_+1,1u << 20);
        ++sample_;
    } else {
        resetHistory();
    }
}

void HdrFrame::finishMeter(VkCommandBuffer command) const
{
    const VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER,nullptr,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT};
    device_->api->vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,
                                       1,&barrier,0,nullptr,0,nullptr);
}

bool HdrFrame::takeMeter(uint32_t slot,float &log2Average) const
{
    if (slot >= meterSlots_ || !meter_.mapped)
        return false;
    auto *bins = static_cast<std::uint32_t *>(meter_.mapped)+std::size_t(slot)*color::meterBins;
    std::array<std::uint32_t,color::meterBins> copy{};
    std::copy_n(bins,copy.size(),copy.begin());
    std::fill_n(bins,copy.size(),0u);
    return color::meteredLuminance(copy,log2Average);
}

} // namespace worldforge::vk
