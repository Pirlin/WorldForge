#include "Engine/Render/Vulkan/DrawCulling.h"

#include "CullDraws_comp.h"

#include <cstring>

namespace worldforge::vk {
namespace {
constexpr uint32_t groupSize = 64;
struct CullView {
    std::array<float,16> matrix;
    uint32_t count, base, compactBase, countBase;
    VkExtent2D depthSize;
};
}

std::vector<uint32_t> occlusionSeeds(std::span<const DrawBounds> batches,std::span<const VkDrawIndirectCommand> previous,
                                     const std::array<float,16> &view,std::array<float,3> eye)
{
    struct Candidate { uint32_t index; float score; };
    std::vector<Candidate> candidates;
    candidates.reserve(batches.size());
    for (uint32_t i = 0; i < batches.size(); ++i) {
        const auto &batch = batches[i];
        if (!batch.vertices[1])
            continue;
        const auto row = [&](uint32_t axis) {
            return view[axis]*batch.center[0]+view[axis+4]*batch.center[1]+view[axis+8]*batch.center[2]+view[axis+12];
        };
        const auto &e = batch.extent;
        const float w = row(3);
        const float reach = std::abs(view[3])*e[0]+std::abs(view[7])*e[1]+std::abs(view[11])*e[2];
        if (w+reach <= 0)
            continue;
        const auto visibleAxis = [&](uint32_t axis) {
            return std::abs(row(axis)) <= w+reach+std::abs(view[axis])*e[0]
                +std::abs(view[axis+4])*e[1]+std::abs(view[axis+8])*e[2];
        };
        if (!visibleAxis(0) || !visibleAxis(1))
            continue;
        float distance = 1;
        for (uint32_t axis = 0; axis < 3; ++axis) {
            const float gap = std::max(0.0f,std::abs(eye[axis]-batch.center[axis])-e[axis]);
            distance += gap*gap;
        }
        const float area = e[0]*e[1]+e[1]*e[2]+e[0]*e[2];
        const bool recent = i < previous.size() && batch.vertices[0] == previous[i].firstVertex && previous[i].vertexCount > 0;
        candidates.push_back({i,area/(distance*(recent ? 1.0f : 2.0f))});
    }
    const auto count = std::min<std::size_t>(16,candidates.size());
    std::partial_sort(candidates.begin(),candidates.begin()+count,candidates.end(),[](const Candidate &a,const Candidate &b) {
        return a.score == b.score ? a.index < b.index : a.score > b.score;
    });
    std::vector<uint32_t> seeds;
    seeds.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        seeds.push_back(candidates[i].index);
    return seeds;
}

bool DrawCulling::create(const Device &device,uint32_t frameSlots,bool multiDraw,bool indirectCount)
{
    destroy();
    device_ = &device;
    if (multiDraw) {
        VkPhysicalDeviceProperties properties{};
        device.api->vkGetPhysicalDeviceProperties(device.physical,&properties);
        maxMultiDraw_ = std::max(1u,properties.limits.maxDrawIndirectCount);
    }
    const auto &api = *device.api;
    indirectCount_ = indirectCount && api.vkCmdDrawIndirectCountKHR && maxMultiDraw_ >= groupSize;
    constexpr VkDescriptorSetLayoutBinding bindings[]{
        {0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    const VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,nullptr,0,4,bindings};
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,4*frameSlots};
    const VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,nullptr,0,frameSlots,1,&size};
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(CullView)};
    const VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,nullptr,0,1,&setLayout_,1,&push};
    if (api.vkCreateDescriptorSetLayout(device.device,&setInfo,nullptr,&setLayout_) != VK_SUCCESS
        || api.vkCreateDescriptorPool(device.device,&poolInfo,nullptr,&pool_) != VK_SUCCESS
        || api.vkCreatePipelineLayout(device.device,&layoutInfo,nullptr,&layout_) != VK_SUCCESS) {
        destroy();
        return false;
    }
    frames_.resize(frameSlots);
    const VkDescriptorSetAllocateInfo sets{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,nullptr,pool_,1,&setLayout_};
    for (auto &frame : frames_)
        if (api.vkAllocateDescriptorSets(device.device,&sets,&frame.set) != VK_SUCCESS) {
            destroy();
            return false;
        }
    const auto module = device.shader(spirv_CullDraws_comp);
    if (!module) {
        destroy();
        return false;
    }
    const VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,
        VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};
    const VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,nullptr,0,stage,layout_,
        VK_NULL_HANDLE,-1};
    const bool created = api.vkCreateComputePipelines(device.device,VK_NULL_HANDLE,1,&pipeline,nullptr,&pipeline_) == VK_SUCCESS;
    api.vkDestroyShaderModule(device.device,module,nullptr);
    if (!created)
        destroy();
    return created;
}

void DrawCulling::destroy()
{
    if (!device_)
        return;
    const auto &api = *device_->api;
    api.vkDestroyPipeline(device_->device,pipeline_,nullptr);
    api.vkDestroyPipelineLayout(device_->device,layout_,nullptr);
    api.vkDestroyDescriptorPool(device_->device,pool_,nullptr);
    api.vkDestroyDescriptorSetLayout(device_->device,setLayout_,nullptr);
    for (auto &frame : frames_) {
        frame.batches.destroy(*device_);
        frame.indirect.destroy(*device_);
        frame.compact.destroy(*device_);
    }
    frames_.clear();
    device_ = nullptr;
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    maxMultiDraw_ = 1;
    indirectCount_ = false;
}

bool DrawCulling::record(VkCommandBuffer command,uint32_t slot,std::span<const DrawBounds> batches,
                         std::span<const std::array<float,16>> views,uint32_t firstCompactedView,
                         const Buffer *depthPyramid,VkExtent2D depthSize)
{
    if (!pipeline_ || slot >= frames_.size() || batches.empty() || batches.size() > 65536 || views.empty() || views.size() > 8)
        return false;
    auto &frame = frames_[slot];
    frame.count = 0;
    frame.views = 0;
    frame.compacted = false;
    const auto &api = *device_->api;
    const VkDeviceSize outputBytes = batches.size()*views.size()*sizeof(VkDrawIndirectCommand);
    const uint32_t groups = (uint32_t(batches.size())+groupSize-1)/groupSize;
    const auto compactBytes = outputBytes+views.size()*groups*sizeof(VkDrawIndirectCommand);
    bool reset = false;
    const auto reserve = [&](Buffer &buffer,VkDeviceSize bytes,VkBufferUsageFlags usage) {
        if (buffer.size >= bytes)
            return true;
        buffer.destroy(*device_);
        reset = true;
        return buffer.create(*device_,bytes+bytes/4,usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    };
    if (!reserve(frame.batches,batches.size_bytes(),0) || !reserve(frame.indirect,outputBytes,VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)) {
        frame.batches.destroy(*device_);
        frame.indirect.destroy(*device_);
        return false;
    }
    frame.compacted = indirectCount_ && firstCompactedView < views.size()
        && reserve(frame.compact,compactBytes,VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    const auto &compact = frame.compact.buffer ? frame.compact : frame.indirect;
    if (reset || depthPyramid || frame.hadPyramid) {
        const auto &depth = depthPyramid ? *depthPyramid : frame.batches;
        const VkDescriptorBufferInfo buffers[]{{frame.batches.buffer,0,frame.batches.size},
                                              {frame.indirect.buffer,0,frame.indirect.size},
                                              {compact.buffer,0,compact.size},{depth.buffer,0,depth.size}};
        const VkWriteDescriptorSet writes[]{
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,0,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&buffers[0],nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,1,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&buffers[1],nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,2,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&buffers[2],nullptr},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.set,3,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&buffers[3],nullptr}};
        api.vkUpdateDescriptorSets(device_->device,4,writes,0,nullptr);
    }
    frame.hadPyramid = depthPyramid != nullptr;
    std::memcpy(frame.batches.mapped,batches.data(),batches.size_bytes());
    frame.count = uint32_t(batches.size());
    frame.views = uint32_t(views.size());
    frame.groups = groups;
    frame.firstCompactedView = firstCompactedView;
    api.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_);
    api.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,1,&frame.set,0,nullptr);
    for (uint32_t view = 0; view < views.size(); ++view) {
        const uint32_t base = view*frame.count;
        const bool compactView = frame.compacted && view >= firstCompactedView;
        const CullView push{views[view],frame.count,base,compactView ? frame.views*groups+base : 0u,view*groups,
            view == 0 && depthPyramid ? depthSize : VkExtent2D{}};
        api.vkCmdPushConstants(command,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
        api.vkCmdDispatch(command,groups,1,1);
    }
    const VkBufferMemoryBarrier ready[]{
        {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,nullptr,VK_ACCESS_SHADER_WRITE_BIT,
         VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_HOST_READ_BIT,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,
         frame.indirect.buffer,0,outputBytes},
        {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,nullptr,VK_ACCESS_SHADER_WRITE_BIT,
         VK_ACCESS_INDIRECT_COMMAND_READ_BIT,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,
         frame.compact.buffer,0,compactBytes}};
    api.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                             0,0,nullptr,frame.compacted ? 2u : 1u,ready,0,nullptr);
    return true;
}

void DrawCulling::draw(VkCommandBuffer command,uint32_t slot,uint32_t view,uint32_t first,uint32_t count) const
{
    const auto &frame = frames_[slot];
    if (frame.compacted && view >= frame.firstCompactedView && first == 0 && count == frame.count) {
        constexpr auto stride = sizeof(VkDrawIndirectCommand);
        const auto commands = VkDeviceSize(frame.views*frame.groups+view*frame.count)*stride;
        const auto counts = VkDeviceSize(view*frame.groups)*stride;
        for (uint32_t firstInGroup = 0; firstInGroup < count; firstInGroup += groupSize) {
            const auto offset = VkDeviceSize(firstInGroup)*stride;
            const auto countOffset = counts+VkDeviceSize(firstInGroup/groupSize)*stride;
            device_->api->vkCmdDrawIndirectCountKHR(command,frame.compact.buffer,commands+offset,frame.compact.buffer,
                countOffset,std::min(groupSize,count-firstInGroup),uint32_t(stride));
        }
        return;
    }
    auto offset = VkDeviceSize(view*frame.count+first)*sizeof(VkDrawIndirectCommand);
    while (count) {
        const uint32_t batch = std::min(count,maxMultiDraw_);
        device_->api->vkCmdDrawIndirect(command,frame.indirect.buffer,offset,batch,sizeof(VkDrawIndirectCommand));
        offset += VkDeviceSize(batch)*sizeof(VkDrawIndirectCommand);
        count -= batch;
    }
}

std::span<const VkDrawIndirectCommand> DrawCulling::commands(uint32_t slot,uint32_t view) const
{
    if (slot >= frames_.size() || view >= frames_[slot].views)
        return {};
    const auto &frame = frames_[slot];
    return {static_cast<const VkDrawIndirectCommand *>(frame.indirect.mapped)+view*frame.count,frame.count};
}

bool DrawCulling::compacted(uint32_t slot) const
{
    return slot < frames_.size() && frames_[slot].compacted;
}

}
