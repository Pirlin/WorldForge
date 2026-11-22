#pragma once

// Clustered lighting, shared by the editor viewport and the Player (Shading.glsl reads the result).
//
// A night street has dozens of lamps, but each pixel is lit by only the few near it. The view is cut into clusters:
// 16 x 9 screen tiles by 24 depth slices, spaced exponentially from nearDepth to farDepth. Each frame, every point
// light's sphere of influence is projected to find the clusters it touches, and its index is listed in each; a pixel
// then shades only its cluster's lights. Directional lights reach everywhere and are shaded by every pixel.
//
// A point light's range is where its light falls to about invisibleRadiance (scene units, see ColorPipeline.h), dim
// enough to vanish even at night exposures; within it the falloff is the physical inverse square, with a smooth
// window over the last part (Shading.glsl).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace worldforge::lights {

constexpr int tilesX = 16, tilesY = 9, slices = 24;
constexpr int clusterCount = tilesX*tilesY*slices;
constexpr int maximumLights = 1024;
constexpr int maximumIndices = 65536;
constexpr float nearDepth = 0.1f, farDepth = 1000.0f;
constexpr float invisibleRadiance = 2e-6f;
constexpr float minimumRange = 2, maximumRange = 250;

struct Light {
    std::array<float,3> vector{};      // position, or the direction toward a directional light
    float intensity = 0;               // scene units at 1 m (point) or on a facing surface (directional)
    std::array<float,3> color{1,1,1};  // linear tint
    bool directional = false;
};

// The storage buffer Shading.glsl reads, in std430 layout: a header (light count, directional count), then two vec4 per
// light, then each cluster's offset and count, then the light indices the clusters list.
struct Clusters {
    std::array<uint32_t,4> header{};
    std::vector<std::array<float,4>> lights;      // 2 per light: position or direction and range; color times intensity
    std::vector<std::array<uint32_t,2>> ranges;   // clusterCount entries: offset, count
    std::vector<uint32_t> indices;

    static constexpr std::size_t lightsOffset = 16;
    static constexpr std::size_t rangesOffset = lightsOffset+std::size_t(maximumLights)*32;
    static constexpr std::size_t indicesOffset = rangesOffset+std::size_t(clusterCount)*8;
    static constexpr std::size_t bufferBytes = indicesOffset+std::size_t(maximumIndices)*4;

    // Writes the parts in use into a mapped buffer of bufferBytes.
    void write(void *buffer) const
    {
        auto *bytes = static_cast<unsigned char *>(buffer);
        std::copy_n(reinterpret_cast<const unsigned char *>(header.data()),16,bytes);
        std::copy_n(reinterpret_cast<const unsigned char *>(lights.data()),lights.size()*16,bytes+lightsOffset);
        std::copy_n(reinterpret_cast<const unsigned char *>(ranges.data()),ranges.size()*8,bytes+rangesOffset);
        std::copy_n(reinterpret_cast<const unsigned char *>(indices.data()),indices.size()*4,bytes+indicesOffset);
    }
};

inline float range(float intensity)
{
    return std::clamp(std::sqrt(std::max(intensity,0.0f)/invisibleRadiance),minimumRange,maximumRange);
}

// The depth slice of a distance along the camera's forward axis (Shading.glsl mirrors this).
inline int slice(float depth)
{
    if (depth <= nearDepth)
        return 0;
    const int index = int(std::log(depth/nearDepth)/std::log(farDepth/nearDepth)*slices);
    return std::clamp(index,0,slices-1);
}

// viewProjection maps world space to the camera's clip space (column-major, Vulkan: y down, depth 0 to 1, no TAA
// jitter); eye and forward give depth along the view.
inline Clusters build(std::span<const Light> input,const std::array<float,16> &viewProjection,
                      const std::array<float,3> &eye,const std::array<float,3> &forward)
{
    Clusters result;
    // Directional lights first, then point lights.
    std::vector<const Light *> ordered;
    for (const auto &light : input)
        if (light.directional && light.intensity > 0)
            ordered.push_back(&light);
    const auto directional = ordered.size();
    for (const auto &light : input)
        if (!light.directional && light.intensity > 0)
            ordered.push_back(&light);
    if (ordered.size() > std::size_t(maximumLights))
        ordered.resize(std::size_t(maximumLights));
    result.header = {uint32_t(ordered.size()),uint32_t(std::min<std::size_t>(directional,ordered.size())),0,0};
    for (const auto *light : ordered) {
        const float reach = light->directional ? 0 : range(light->intensity);
        result.lights.push_back({light->vector[0],light->vector[1],light->vector[2],reach});
        result.lights.push_back({light->color[0]*light->intensity,light->color[1]*light->intensity,
                                 light->color[2]*light->intensity,light->directional ? 1.0f : 0.0f});
    }

    // Each point light's box of clusters: tiles from its projected bounds, slices from its depth range.
    struct Box { int x0,x1,y0,y1,z0,z1; };
    std::vector<Box> boxes(ordered.size(),Box{1,0,1,0,1,0});
    const auto project = [&](float x,float y,float z,float &ndcX,float &ndcY) {
        const auto &m = viewProjection;
        const float cx = m[0]*x+m[4]*y+m[8]*z+m[12], cy = m[1]*x+m[5]*y+m[9]*z+m[13];
        const float w = m[3]*x+m[7]*y+m[11]*z+m[15];
        if (w <= 1e-4f)
            return false;
        ndcX = cx/w;
        ndcY = cy/w;
        return true;
    };
    for (std::size_t i = directional; i < ordered.size(); ++i) {
        const auto &light = *ordered[i];
        const float r = result.lights[i*2][3];
        const auto &c = light.vector;
        const float depth = (c[0]-eye[0])*forward[0]+(c[1]-eye[1])*forward[1]+(c[2]-eye[2])*forward[2];
        if (depth+r < 0)
            continue; // entirely behind the camera
        float minX = 1, minY = 1, maxX = -1, maxY = -1;
        bool whole = depth-r <= nearDepth;
        for (int corner = 0; corner < 8 && !whole; ++corner) {
            float x = 0, y = 0;
            if (!project(c[0]+(corner & 1 ? r : -r),c[1]+(corner & 2 ? r : -r),c[2]+(corner & 4 ? r : -r),x,y)) {
                whole = true;
                break;
            }
            minX = std::min(minX,x);
            maxX = std::max(maxX,x);
            minY = std::min(minY,y);
            maxY = std::max(maxY,y);
        }
        if (whole) {
            minX = minY = -1;
            maxX = maxY = 1;
        }
        if (maxX < -1 || minX > 1 || maxY < -1 || minY > 1)
            continue; // off screen
        const auto tile = [](float ndc,int count) {
            return std::clamp(int(std::floor((std::clamp(ndc,-1.0f,1.0f)*0.5f+0.5f)*float(count))),0,count-1);
        };
        boxes[i] = {tile(minX,tilesX),tile(maxX,tilesX),tile(minY,tilesY),tile(maxY,tilesY),slice(depth-r),slice(depth+r)};
    }

    // Count, then lay out the lists, then fill them.
    std::vector<uint32_t> counts(std::size_t(clusterCount),0);
    const auto visit = [&](const Box &box,auto &&action) {
        for (int z = box.z0; z <= box.z1; ++z)
            for (int y = box.y0; y <= box.y1; ++y)
                for (int x = box.x0; x <= box.x1; ++x)
                    action(std::size_t((z*tilesY+y)*tilesX+x));
    };
    for (std::size_t i = directional; i < ordered.size(); ++i)
        visit(boxes[i],[&](std::size_t cluster) { ++counts[cluster]; });
    result.ranges.assign(std::size_t(clusterCount),{0,0});
    uint32_t offset = 0;
    for (std::size_t cluster = 0; cluster < counts.size(); ++cluster) {
        const uint32_t count = std::min<uint32_t>(counts[cluster],uint32_t(maximumIndices)-offset);
        result.ranges[cluster] = {offset,0};
        offset += count;
        counts[cluster] = count;
    }
    result.indices.assign(offset,0);
    for (std::size_t i = directional; i < ordered.size(); ++i)
        visit(boxes[i],[&](std::size_t cluster) {
            auto &entry = result.ranges[cluster];
            if (entry[1] < counts[cluster])
                result.indices[entry[0]+entry[1]++] = uint32_t(i);
        });
    return result;
}

} // namespace worldforge::lights
