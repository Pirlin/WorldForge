#include "Engine/Render/Vulkan/ProbeTrace.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
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

// --replay <probe-n.bin> (written with WORLDFORGE_PROBE_DUMP) traces a recorded refresh, all probes at once and then in
// chunks, and prints the GPU wall time of each, to find a refresh or a probe that takes too long.
int replay(ComputeDevice &gpu,const char *path)
{
    std::ifstream in(path,std::ios::binary);
    uint32_t counts[2]{};
    in.read(reinterpret_cast<char *>(counts),sizeof(counts));
    std::vector<probes::SurfaceTriangle> surfaces(counts[0]);
    in.read(reinterpret_cast<char *>(surfaces.data()),std::streamsize(surfaces.size()*sizeof(surfaces[0])));
    probes::Volume volume;
    probes::CoarseVolume coarse;
    probes::Vector sun{}, radiance{};
    probes::HitLighting lighting;
    lighting.lights.resize(counts[1]);
    in.read(reinterpret_cast<char *>(&volume.origin),sizeof(volume.origin));
    in.read(reinterpret_cast<char *>(&coarse.origin),sizeof(coarse.origin));
    in.read(reinterpret_cast<char *>(&sun),sizeof(sun));
    in.read(reinterpret_cast<char *>(&radiance),sizeof(radiance));
    in.read(reinterpret_cast<char *>(lighting.sky.data()),sizeof(lighting.sky));
    in.read(reinterpret_cast<char *>(lighting.lights.data()),std::streamsize(lighting.lights.size()*sizeof(lighting.lights[0])));
    require(bool(in),"Read the recorded probe refresh");
    const auto geometry = probes::buildGeometry(surfaces);
    require(geometry.has_value(),"Build the recorded geometry");
    std::cout << surfaces.size() << " triangles, " << geometry->nodes.size() << " nodes, " << lighting.lights.size()/4
              << " lamps, sun " << sun[0] << ' ' << sun[1] << ' ' << sun[2] << ", radiance " << radiance[0] << '\n';
    for (std::size_t i = 0; i < lighting.lights.size(); ++i)
        std::cout << "  light row " << i << ": " << lighting.lights[i][0] << ' ' << lighting.lights[i][1] << ' '
                  << lighting.lights[i][2] << ' ' << lighting.lights[i][3] << '\n';
    std::vector<probes::ProbePosition> positions;
    std::ranges::copy(volume.positions(),std::back_inserter(positions));
    std::ranges::copy(coarse.positions(),std::back_inserter(positions));
    vk::ProbeTrace trace;
    require(trace.create(gpu.device,geometry.value(),positions,false),"Create the replay trace");
    const auto timed = [&](uint32_t first,uint32_t count) {
        const VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,nullptr,gpu.pool,
            VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};
        VkCommandBuffer command{};
        gpu.api.vkAllocateCommandBuffers(gpu.device.device,&allocate,&command);
        const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,nullptr,
            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,nullptr};
        gpu.api.vkBeginCommandBuffer(command,&begin);
        require(trace.record(command,first,count,sun,radiance,probes::Volume::range,lighting),"Record the replay");
        gpu.api.vkEndCommandBuffer(command);
        const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO,nullptr,0,nullptr,nullptr,1,&command,0,nullptr};
        const auto start = std::chrono::steady_clock::now();
        const bool ok = gpu.api.vkQueueSubmit(gpu.queue,1,&submit,VK_NULL_HANDLE) == VK_SUCCESS
            && gpu.api.vkQueueWaitIdle(gpu.queue) == VK_SUCCESS;
        const double ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        gpu.api.vkFreeCommandBuffers(gpu.device.device,gpu.pool,1,&command);
        return ok ? ms : -1.0;
    };
    std::cout << "all " << positions.size() << " probes: " << timed(0,uint32_t(positions.size())) << " ms\n";
    {
        const auto *values = static_cast<const std::array<float,4> *>(trace.output().mapped);
        float largest = 0, smallestMoment = 1e30f;
        std::size_t nonFinite = 0;
        for (std::size_t i = 0; i < positions.size()*12; ++i) {
            for (const float value : values[i])
                if (!std::isfinite(value))
                    ++nonFinite;
            if (i%2 == 0)
                largest = std::max({largest,values[i][0],values[i][1],values[i][2]});
            else
                smallestMoment = std::min(smallestMoment,values[i][1]-values[i][0]*values[i][0]);
        }
        std::cout << "largest irradiance " << largest << ", non-finite values " << nonFinite
                  << ", smallest variance " << smallestMoment << '\n';
    }
    for (uint32_t first = 0; first < positions.size(); first += 50) {
        const auto count = std::min<uint32_t>(50,uint32_t(positions.size())-first);
        std::cout << "probes " << first << "-" << first+count-1 << ": " << timed(first,count) << " ms\n";
    }
    return 0;
}

int main(int argc,char **argv)
{
    checkGeometry();
    ComputeDevice gpu;
    if (argc == 3 && std::strcmp(argv[1],"--replay") == 0) {
        require(gpu.open(),"Open a Vulkan compute device");
        return replay(gpu,argv[2]);
    }
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
