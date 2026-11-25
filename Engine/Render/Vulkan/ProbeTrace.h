#pragma once

#include "Engine/Render/ProbeGeometry.h"
#include "Engine/Render/Vulkan/VulkanApi.h"

#include <memory>

namespace worldforge::vk {

// The probe compute pipeline and its layouts. Creating a pipeline takes milliseconds, so a field creates it once and
// every refresh reuses it; each trace holds a reference, so the pipeline outlives the frames still using it.
struct ProbePipeline {
    const Device *device = nullptr;
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};

    static std::shared_ptr<const ProbePipeline> create(const Device &device);
    ProbePipeline() = default;
    ProbePipeline(const ProbePipeline &) = delete;
    ProbePipeline &operator=(const ProbePipeline &) = delete;
    ~ProbePipeline();
};

class ProbeTrace {
public:
    ProbeTrace() = default;
    ~ProbeTrace();
    ProbeTrace(const ProbeTrace &) = delete;
    ProbeTrace &operator=(const ProbeTrace &) = delete;

    // Uploads world-space geometry and positions. The device must outlive these resources. The first form creates
    // its own pipeline; the second reuses one (ProbeField).
    bool create(const Device &device,const probes::Geometry &geometry,std::span<const probes::ProbePosition> positions,
                bool reflections = false);
    bool create(const Device &device,std::shared_ptr<const ProbePipeline> pipeline,const probes::Geometry &geometry,
                std::span<const probes::ProbePosition> positions,bool reflections = false);

    // Record before the render pass on a compute-capable queue. The caller must synchronize subsequent reads
    // and wait for all uses before destroying or replacing this object.
    // lighting lights the surfaces the rays hit besides the sun (ProbeGeometry.h HitLighting).
    bool record(VkCommandBuffer commands,uint32_t first,uint32_t count,probes::Vector towardSun,
                probes::Vector sunRadiance,float range,const probes::HitLighting &lighting = {}) const;

    const Buffer &output() const { return buffers_[3]; }

private:
    void destroy();

    const Device *device_ = nullptr;
    std::array<Buffer,5> buffers_; // nodes, triangles, positions, output, hit lighting
    std::shared_ptr<const ProbePipeline> pipeline_;
    VkDescriptorPool pool_{};
    VkDescriptorSet set_{};
    uint32_t nodeCount_ = 0, probeCount_ = 0;
    bool reflections_ = false;
};

// Each in-flight frame holds its field alive. A refreshed field writes a new buffer, so older frames
// can continue sampling their own snapshot.
class ProbeField {
public:
    bool needsUpdate(bool enabled,bool reflections,probes::Volume volume,uint64_t revision,probes::Vector sun,
                     probes::Vector radiance,bool moving,const probes::HitLighting &lighting = {},probes::CoarseVolume coarse = {});
    bool update(const Device &device,VkCommandBuffer commands,std::span<const probes::SurfaceTriangle> surfaces);
    void clear() { trace_.reset(); pipeline_.reset(); requested_ = false; }
    std::shared_ptr<const ProbeTrace> trace() const { return trace_; }
    std::array<float,4> grid() const;
    // The coarse grid (probes::CoarseVolume), whose probes follow the fine grid's in the output.
    std::array<float,4> coarseGrid() const;

private:
    std::shared_ptr<ProbeTrace> trace_;
    std::shared_ptr<const ProbePipeline> pipeline_;
    probes::Volume volume_;
    probes::CoarseVolume coarse_;
    probes::Vector sun_{}, radiance_{};
    probes::HitLighting lighting_;
    uint64_t revision_ = 0;
    uint32_t age_ = 0;
    bool requested_ = false;
    bool reflections_ = false;
};

}
