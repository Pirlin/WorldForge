#include "Engine/Render/Vulkan/ProbeTrace.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

namespace {
namespace probes = worldforge::probes;
namespace vk = worldforge::vk;

void require(bool condition,const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void plane(std::vector<probes::SurfaceTriangle> &surfaces,float x,float height,float radius,probes::Vector diffuse,
           probes::Vector emission = {})
{
    const probes::Vector a{x-radius,-radius,height}, b{x+radius,-radius,height};
    const probes::Vector c{x+radius,radius,height}, d{x-radius,radius,height};
    surfaces.push_back({{a,b,c},diffuse,emission});
    surfaces.push_back({{a,c,d},diffuse,emission});
}

void checkGeometry()
{
    std::vector<probes::SurfaceTriangle> surfaces;
    plane(surfaces,0,0,2,{1,0,0});
    plane(surfaces,0,4,2,{0,1,0});
    for (int i = 1; i < 8; ++i)
        plane(surfaces,float(i*10),1,2,{0,0,1});
    surfaces.push_back({});
    const auto geometry = probes::buildGeometry(surfaces);
    require(geometry && geometry->nodes.size() > 1 && geometry->triangles.size() == surfaces.size()-1,
            "Build a multilevel BVH and omit the degenerate triangle");
    require(geometry->nodes.front().escape == geometry->nodes.size(),"Root escapes to the end of the BVH");
    const auto nearest = probes::trace(*geometry,{0.5f,0,7},{0,0,-1},10);
    require(nearest && nearest->distance == 3 && geometry->triangles[nearest->triangle].diffuse[1] == 1,
            "Trace the nearest of two overlapping surfaces through the hierarchy");
    const auto distantChild = probes::trace(*geometry,{71,0,8},{0,0,-1},10);
    require(distantChild && distantChild->distance == 7,"Traverse a distant sibling after skipping earlier subtrees");
    require(!probes::trace(*geometry,{3,0,7},{0,0,-1},10),"A parallel ray outside the slab misses");
    require(!probes::trace(*geometry,{0.5f,0,7},{0,0,-1},2),"A hit past the requested range misses");
    surfaces.front().vertices[0][0] = std::numeric_limits<float>::quiet_NaN();
    require(!probes::buildGeometry(surfaces),"Reject a non-finite world position before uploading");
}

struct ComputeDevice {
    vk::Api api;
    vk::Device device;
    VkInstance instance{};
    VkQueue queue{};
    VkCommandPool pool{};

    ~ComputeDevice()
    {
        if (device.device) {
            api.vkDestroyCommandPool(device.device,pool,nullptr);
            api.vkDestroyDevice(device.device,nullptr);
        }
        if (instance && api.vkDestroyInstance)
            api.vkDestroyInstance(instance,nullptr);
    }

    bool open()
    {
        if (!api.loadLibrary())
            return false;
        const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO,nullptr,"WorldForge Probe Test",1,
            "WorldForge",1,VK_API_VERSION_1_0};
        const VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,nullptr,0,&app,0,nullptr,0,nullptr};
        if (api.vkCreateInstance(&info,nullptr,&instance) != VK_SUCCESS || !api.loadInstance(instance))
            return false;
        uint32_t count = 0;
        api.vkEnumeratePhysicalDevices(instance,&count,nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        api.vkEnumeratePhysicalDevices(instance,&count,devices.data());
        VkPhysicalDevice selected{};
        uint32_t family = 0;
        int score = -1;
        for (const auto physical : devices) {
            uint32_t familyCount = 0;
            api.vkGetPhysicalDeviceQueueFamilyProperties(physical,&familyCount,nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            api.vkGetPhysicalDeviceQueueFamilyProperties(physical,&familyCount,families.data());
            VkPhysicalDeviceProperties properties{};
            api.vkGetPhysicalDeviceProperties(physical,&properties);
            const int rank = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2
                : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
            for (uint32_t i = 0; i < familyCount && rank > score; ++i) {
                if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                    selected = physical;
                    family = i;
                    score = rank;
                    std::cout << "Vulkan probe device: " << properties.deviceName << '\n';
                }
            }
        }
        if (!selected)
            return false;
        const float priority = 1;
        const VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,nullptr,0,family,1,&priority};
        const VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,nullptr,0,1,&queueInfo,0,nullptr,0,nullptr,nullptr};
        VkDevice logical{};
        require(api.vkCreateDevice(selected,&deviceInfo,nullptr,&logical) == VK_SUCCESS && api.loadDevice(logical),
                "Create a Vulkan device with compute support");
        device.init(api,selected,logical);
        api.vkGetDeviceQueue(logical,family,0,&queue);
        const VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,nullptr,
            VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,family};
        require(api.vkCreateCommandPool(logical,&poolInfo,nullptr,&pool) == VK_SUCCESS,"Create a compute command pool");
        return true;
    }

    std::array<probes::Probe,2> run(const probes::Geometry &geometry,
                                    std::array<probes::ReflectionMap,2> *reflections = nullptr)
    {
        constexpr probes::ProbePosition positions[]{{0,0,1,0},{120,120,1,0}};
        vk::ProbeTrace trace;
        require(trace.create(device,geometry,positions,reflections != nullptr),"Create the probe compute pipeline and upload inputs");
        const VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,nullptr,pool,
            VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};
        VkCommandBuffer command{};
        require(api.vkAllocateCommandBuffers(device.device,&allocate,&command) == VK_SUCCESS,"Allocate a compute command buffer");
        const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,nullptr,
            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,nullptr};
        require(api.vkBeginCommandBuffer(command,&begin) == VK_SUCCESS,"Begin compute commands");
        require(!trace.record(command,1,2,{0,0,1},{1,1,1},60),"Reject an out-of-bounds probe update");
        require(trace.record(command,0,1,{0,0,1},{1,1,1},60)
             && trace.record(command,1,1,{0,0,1},{1,1,1},60),"Dispatch two independent probe update ranges");
        const VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER,nullptr,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT};
        api.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,
                                 0,nullptr,0,nullptr);
        require(api.vkEndCommandBuffer(command) == VK_SUCCESS,"Finish compute commands");
        const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO,nullptr,0,nullptr,nullptr,1,&command,0,nullptr};
        require(api.vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE) == VK_SUCCESS && api.vkQueueWaitIdle(queue) == VK_SUCCESS,
                "Dispatch and wait for completed probe writes");
        std::array<probes::Probe,2> values;
        std::memcpy(values.data(),trace.output().mapped,sizeof(values));
        if (reflections) {
            require(trace.output().size == sizeof(values)+sizeof(*reflections),"Reserve the reflection atlas after diffuse lighting");
            const auto *bytes = static_cast<const std::byte *>(trace.output().mapped);
            std::memcpy(reflections->data(),bytes+sizeof(values),sizeof(*reflections));
        }
        api.vkFreeCommandBuffers(device.device,pool,1,&command);
        return values;
    }
};
}

int main()
{
    checkGeometry();
    ComputeDevice gpu;
    if (!gpu.open()) {
        std::cout << "Vulkan compute device unavailable. CPU BVH checks passed.\n";
        return 77;
    }

    std::vector<probes::SurfaceTriangle> surfaces;
    plane(surfaces,0,0,25,{0.8f,0.1f,0.2f});
    for (int i = 1; i < 7; ++i)
        plane(surfaces,float(i*80),0,4,{1,1,1});
    const auto open = probes::buildGeometry(surfaces);
    require(open.has_value(),"Build the open probe test geometry");
    const auto lit = gpu.run(*open);
    plane(surfaces,0,2,25,{0,0,0},{0.1f,0.2f,0.4f});
    const auto covered = probes::buildGeometry(surfaces);
    require(covered.has_value(),"Build the covered probe test geometry");
    std::array<probes::ReflectionMap,2> reflections;
    const auto shadowed = gpu.run(*covered,&reflections);
    const auto empty = gpu.run(*probes::buildGeometry({}));
    constexpr int up = 4, down = 5;
    const auto &bounce = lit[0][down].irradiance;
    const auto &emission = shadowed[0][up].irradiance;
    const auto &moments = lit[0][down].distanceMoments;
    require(bounce[0] > 0.77f && bounce[0] < 0.81f && bounce[0] > 7*bounce[1],
            "A lit off-screen floor supplies diffuse red bounce below the probe");
    require(shadowed[0][down].irradiance[0] < 0.01f,"A roof blocks the light on the floor and removes the red bounce");
    require(emission[2] > 0.38f && emission[0] > 0.09f && emission[2] > 3*emission[0],
            "The upper probe lobe receives colored emissive radiance");
    require(moments[0] > 0 && moments[0] < 0.08f && moments[1] >= moments[0]*moments[0],
            "A floor hit records finite normalized distance moments");
    require(lit[0][up].distanceMoments[0] == 1.0f && lit[1][down].distanceMoments[0] == 1.0f
         && empty[0][down].distanceMoments[0] == 1.0f && empty[0][down].irradiance[0] == 0.0f,
            "Open directions and empty scenes return the trace range and no geometry bounce");
    constexpr auto centerTexel = probes::reflectionMapSize/2*(probes::reflectionMapSize+1);
    const auto &reflected = reflections[0][centerTexel];
    std::cout << "Ceiling reflection blue " << reflected[2] << ", first-hit distance " << reflected[3]*60 << " m.\n";
    require(std::abs(reflected[2]-0.4f) < 0.001f && reflected[3]*60 > 1.02f && reflected[3]*60 < 1.04f,
            "An upward reflection texel stores the ceiling emission and its first-hit distance");
    require(reflections[1][centerTexel][2] == 0.0f && reflections[1][centerTexel][3] == 1.0f,
            "An open reflection direction preserves the sky fallback");
    std::cout << "World-space bounce " << bounce[0] << ", roof shadow " << shadowed[0][down].irradiance[0]
              << ", emissive blue " << emission[2] << ", mean hit distance " << moments[0]*60 << " m.\n";
}
