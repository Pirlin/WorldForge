#pragma once

#include "Engine/Render/Vulkan/VulkanApi.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>

namespace worldforge::vk {

struct DrawBounds {
    std::array<float,4> center{}, extent{};
    std::array<uint32_t,4> vertices{};
};
static_assert(sizeof(DrawBounds) == 48);

// Prefer nearby batches visible on the previous use of this frame slot. Geometry is always drawn again
// at its current positions to seed the depth pyramid, so stale choices only cost some culling efficiency.
std::vector<uint32_t> occlusionSeeds(std::span<const DrawBounds> batches,std::span<const VkDrawIndirectCommand> previous,
                                     const std::array<float,16> &view,std::array<float,3> eye);

template<class Vertices>
DrawBounds drawBounds(const Vertices &vertices,uint32_t first,uint32_t count)
{
    std::array<float,3> low, high;
    std::copy_n(std::begin(vertices[first].position),3,low.begin());
    high = low;
    for (const auto &vertex : std::span(vertices).subspan(first,count)) {
        // Both scene vertex shaders sway x by at most wind and y by at most 0.6*wind.
        const float wind = std::abs(vertex.wind);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const float movement = axis < 2 ? wind : 0;
            low[axis] = std::min(low[axis],vertex.position[axis]-movement);
            high[axis] = std::max(high[axis],vertex.position[axis]+movement);
        }
    }
    DrawBounds bounds;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        bounds.center[axis] = (low[axis]+high[axis])*0.5f;
        bounds.extent[axis] = (high[axis]-low[axis])*0.5f;
    }
    bounds.vertices = {first,count,0,0};
    return bounds;
}

class DrawCulling {
public:
    DrawCulling() = default;
    ~DrawCulling() { destroy(); }
    DrawCulling(const DrawCulling &) = delete;
    DrawCulling &operator=(const DrawCulling &) = delete;

    // The device and its dispatch table must outlive this object. Each slot is reused only after its fence.
    bool create(const Device &device,uint32_t frameSlots,bool multiDraw,bool indirectCount);
    void destroy();
    bool record(VkCommandBuffer command,uint32_t slot,std::span<const DrawBounds> batches,
                std::span<const std::array<float,16>> views,uint32_t firstCompactedView = 0,
                const Buffer *depthPyramid = nullptr,VkExtent2D depthSize = {});
    void draw(VkCommandBuffer command,uint32_t slot,uint32_t view,uint32_t first,uint32_t count) const;
    std::span<const VkDrawIndirectCommand> commands(uint32_t slot,uint32_t view) const;
    bool compacted(uint32_t slot) const;

private:
    struct Frame {
        Buffer batches, indirect, compact;
        VkDescriptorSet set{};
        uint32_t count = 0, views = 0, groups = 0, firstCompactedView = 0;
        bool compacted = false, hadPyramid = false;
    };
    const Device *device_ = nullptr;
    VkDescriptorSetLayout setLayout_{};
    VkDescriptorPool pool_{};
    VkPipelineLayout layout_{};
    VkPipeline pipeline_{};
    uint32_t maxMultiDraw_ = 1;
    bool indirectCount_ = false;
    std::vector<Frame> frames_;
};

}
