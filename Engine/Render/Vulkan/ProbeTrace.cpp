#include "Engine/Render/Vulkan/ProbeTrace.h"

#include "ProbeTrace_comp.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace worldforge::vk {
namespace {
struct PushConstants {
    std::array<float,4> sunDirectionRange, sunRadiance;
    uint32_t nodeCount, firstProbe, probeCount, reflections;
};
static_assert(sizeof(PushConstants) == 48);
}

std::shared_ptr<const ProbePipeline> ProbePipeline::create(const Device &device)
{
    auto result = std::make_shared<ProbePipeline>();
    result->device = &device;
    const auto &api = *device.api;
    std::array<VkDescriptorSetLayoutBinding,4> bindings;
    for (uint32_t i = 0; i < bindings.size(); ++i)
        bindings[i] = {i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    const VkDescriptorSetLayoutCreateInfo setLayout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,nullptr,0,
        uint32_t(bindings.size()),bindings.data()};
    if (api.vkCreateDescriptorSetLayout(device.device,&setLayout,nullptr,&result->setLayout) != VK_SUCCESS)
        return nullptr;
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(PushConstants)};
    const VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,nullptr,0,1,
        &result->setLayout,1,&push};
    if (api.vkCreatePipelineLayout(device.device,&pipelineLayout,nullptr,&result->layout) != VK_SUCCESS)
        return nullptr;
    const auto shader = device.shader(spirv_ProbeTrace_comp);
    if (!shader)
        return nullptr;
    const VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,
        VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};
    const VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,nullptr,0,stage,result->layout,
        VK_NULL_HANDLE,-1};
    const bool created = api.vkCreateComputePipelines(device.device,VK_NULL_HANDLE,1,&pipeline,nullptr,&result->pipeline)
        == VK_SUCCESS;
    api.vkDestroyShaderModule(device.device,shader,nullptr);
    return created ? result : nullptr;
}

ProbePipeline::~ProbePipeline()
{
    if (!device)
        return;
    const auto &api = *device->api;
    api.vkDestroyPipeline(device->device,pipeline,nullptr);
    api.vkDestroyPipelineLayout(device->device,layout,nullptr);
    api.vkDestroyDescriptorSetLayout(device->device,setLayout,nullptr);
}

ProbeTrace::~ProbeTrace()
{
    destroy();
}

bool ProbeTrace::create(const Device &device,const probes::Geometry &geometry,std::span<const probes::ProbePosition> positions,
                        bool reflections)
{
    return create(device,ProbePipeline::create(device),geometry,positions,reflections);
}

bool ProbeTrace::create(const Device &device,std::shared_ptr<const ProbePipeline> pipeline,const probes::Geometry &geometry,
                        std::span<const probes::ProbePosition> positions,bool reflections)
{
    destroy();
    if (!pipeline || pipeline->device != &device)
        return false;
    if (positions.empty() || positions.size() > 65535 || geometry.triangles.size() > probes::Geometry::maximumTriangles
        || geometry.nodes.size() > 2*probes::Geometry::maximumTriangles
        || !std::ranges::all_of(positions,[](const probes::ProbePosition &position) {
            return std::ranges::all_of(std::span(position).first<3>(),[](float value) { return std::abs(value) <= 1'000'000; });
        }))
        return false;
    device_ = &device;
    probeCount_ = uint32_t(positions.size());
    nodeCount_ = uint32_t(geometry.nodes.size());
    reflections_ = reflections;
    const auto &api = *device.api;
    const auto upload = [&](Buffer &buffer,std::span<const std::byte> data,std::size_t minimum) {
        if (!buffer.create(device,std::max(data.size(),minimum),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
            return false;
        if (!data.empty())
            std::memcpy(buffer.mapped,data.data(),data.size());
        return true;
    };
    if (!upload(buffers_[0],std::as_bytes(std::span(geometry.nodes)),sizeof(probes::Node))
        || !upload(buffers_[1],std::as_bytes(std::span(geometry.triangles)),sizeof(probes::Triangle))
        || !upload(buffers_[2],std::as_bytes(positions),sizeof(probes::ProbePosition))
        || !buffers_[3].create(device,positions.size()*(sizeof(probes::Probe)+(reflections ? sizeof(probes::ReflectionMap) : 0)),
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
        destroy();
        return false;
    }
    pipeline_ = std::move(pipeline);
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,uint32_t(buffers_.size())};
    const VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,nullptr,0,1,1,&size};
    if (api.vkCreateDescriptorPool(device.device,&pool,nullptr,&pool_) != VK_SUCCESS) {
        destroy();
        return false;
    }
    const VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,nullptr,pool_,1,&pipeline_->setLayout};
    if (api.vkAllocateDescriptorSets(device.device,&allocate,&set_) != VK_SUCCESS) {
        destroy();
        return false;
    }
    std::array<VkDescriptorBufferInfo,4> infos;
    std::array<VkWriteDescriptorSet,4> writes;
    for (uint32_t i = 0; i < buffers_.size(); ++i) {
        infos[i] = {buffers_[i].buffer,0,buffers_[i].size};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,set_,i,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     nullptr,&infos[i],nullptr};
    }
    api.vkUpdateDescriptorSets(device.device,uint32_t(writes.size()),writes.data(),0,nullptr);
    return true;
}

bool ProbeTrace::record(VkCommandBuffer commands,uint32_t first,uint32_t count,probes::Vector towardSun,
                        probes::Vector sunRadiance,float range) const
{
    if (!pipeline_ || !set_ || first > probeCount_ || count > probeCount_-first || !count || !(range > 0.004f && range <= 1'000)
        || !std::ranges::all_of(sunRadiance,[](float value) { return value >= 0 && std::isfinite(value); }))
        return false;
    const float length = std::hypot(towardSun[0],towardSun[1],towardSun[2]);
    if (!(length > 0 && std::isfinite(length)))
        return false;
    const PushConstants constants{{towardSun[0]/length,towardSun[1]/length,towardSun[2]/length,range},
        {sunRadiance[0],sunRadiance[1],sunRadiance[2],0},nodeCount_,first,probeCount_,reflections_ ? 1u : 0u};
    const auto &api = *device_->api;
    api.vkCmdBindPipeline(commands,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_->pipeline);
    api.vkCmdBindDescriptorSets(commands,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_->layout,0,1,&set_,0,nullptr);
    api.vkCmdPushConstants(commands,pipeline_->layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(constants),&constants);
    api.vkCmdDispatch(commands,count,1,1);
    return true;
}

void ProbeTrace::destroy()
{
    if (!device_)
        return;
    const auto &api = *device_->api;
    const auto device = device_->device;
    api.vkDestroyDescriptorPool(device,pool_,nullptr);
    for (auto &buffer : buffers_)
        buffer.destroy(*device_);
    pipeline_.reset();
    pool_ = VK_NULL_HANDLE;
    set_ = VK_NULL_HANDLE;
    device_ = nullptr;
}

bool ProbeField::needsUpdate(bool enabled,bool reflections,probes::Volume volume,uint64_t revision,probes::Vector sun,
                             probes::Vector radiance,bool moving)
{
    if (!enabled) {
        clear();
        return false;
    }
    // A moving scene refreshes at a reduced rate. Edits and light changes also refresh the field.
    if (requested_ && reflections == reflections_ && volume == volume_ && revision == revision_ && sun == sun_ && radiance == radiance_
        && (!moving || ++age_ < 45))
        return false;
    volume_ = volume;
    revision_ = revision;
    sun_ = sun;
    radiance_ = radiance;
    age_ = 0;
    requested_ = true;
    reflections_ = reflections;
    return true;
}

bool ProbeField::update(const Device &device,VkCommandBuffer commands,std::span<const probes::SurfaceTriangle> surfaces)
{
    const auto geometry = probes::buildGeometry(surfaces);
    // Nothing in the volume: no probe light here, and nothing to retry until the volume, scene or light changes.
    if (geometry && geometry->triangles.empty()) {
        trace_.reset();
        return true;
    }
    // The pipeline is created once and reused by every refresh.
    if (!pipeline_)
        pipeline_ = ProbePipeline::create(device);
    auto next = std::make_shared<ProbeTrace>();
    if (!geometry || !pipeline_ || !next->create(device,pipeline_,*geometry,volume_.positions(),reflections_)
        || !next->record(commands,0,probes::Volume::count,sun_,radiance_,probes::Volume::range)) {
        // Try again on the next frame rather than leaving the probes off until something else changes.
        trace_.reset();
        requested_ = false;
        return false;
    }
    const VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER,nullptr,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT};
    device.api->vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,1,&barrier,0,nullptr,0,nullptr);
    trace_ = std::move(next);
    return true;
}

std::array<float,4> ProbeField::grid() const
{
    return {volume_.origin[0],volume_.origin[1],volume_.origin[2],trace_ ? probes::Volume::spacing : 0};
}

}
