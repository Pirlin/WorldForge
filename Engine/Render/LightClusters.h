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
#include <limits>
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
// Point light shadows: the shadowSlots point lights nearest the camera each get a cube of six faceSize depth maps,
// tiled shadowColumns across an atlas (slot s, face f at tile s * 6 + f).
constexpr int shadowSlots = 8, faceSize = 256, shadowColumns = 8;
constexpr int shadowAtlasWidth = faceSize*shadowColumns, shadowAtlasHeight = faceSize*(shadowSlots*6/shadowColumns);
constexpr float shadowNear = 0.05f;

struct Light {
    std::array<float,3> vector{};      // position, or the direction toward a directional light
    float intensity = 0;               // scene units at 1 m (point) or on a facing surface (directional)
    std::array<float,3> color{1,1,1};  // linear tint
    bool directional = false;
    // A spot light: the direction it points, its full cone angle in radians (0 for a point light), and how much of the
    // cone's edge fades out (0 a hard edge, 1 a fade from the axis), as Blender's Spot Size and Blend.
    std::array<float,3> spotDirection{0,0,-1};
    float spotAngle = 0, spotBlend = 0.15f;
};

// Cycles' spot falloff: smoothstep(0, 1, (cos(angle to the axis) - cos(cone / 2)) * scale), with
// scale = 1 / ((1 - cos(cone / 2)) * blend). A point light uses cos = -2 and scale 1, which always gives 1.
inline std::array<float,2> spotCone(const Light &light)
{
    if (light.directional || light.spotAngle <= 0)
        return {-2,1};
    const float half = std::cos(std::clamp(light.spotAngle,0.001f,3.14159f)*0.5f);
    return {half,light.spotBlend > 0 ? 1/((1-half)*light.spotBlend) : 1e6f};
}

// The storage buffer Shading.glsl reads, in std430 layout: a header (light count, directional count, shadowed point
// lights), the cube face matrices of the shadowed lights, then four vec4 per light (position or direction and range;
// color times intensity, whose w is 1 for directional, 0 for a point or spot light without shadow, 2 + slot for one with
// its shadow in that slot; the spot direction and cos(cone / 2); the spot scale), then each cluster's offset and count,
// then the light indices the clusters list.
struct Clusters {
    std::array<uint32_t,4> header{};
    // World space to each shadow face's clip space (column-major, Vulkan depth 0 to 1): slot * 6 + face.
    std::array<std::array<float,16>,shadowSlots*6> shadowFaces{};
    // The shadowed lights: position and range per slot, for rendering their faces and noticing when they move.
    std::vector<std::array<float,4>> shadowed;
    std::vector<std::array<float,4>> lights;      // 4 per light (see above)
    std::vector<std::array<uint32_t,2>> ranges;   // clusterCount entries: offset, count
    std::vector<uint32_t> indices;

    static constexpr std::size_t facesOffset = 16;
    static constexpr std::size_t lightsOffset = facesOffset+std::size_t(shadowSlots)*6*64;
    static constexpr std::size_t rangesOffset = lightsOffset+std::size_t(maximumLights)*64;
    static constexpr std::size_t indicesOffset = rangesOffset+std::size_t(clusterCount)*8;
    static constexpr std::size_t bufferBytes = indicesOffset+std::size_t(maximumIndices)*4;

    // Writes the parts in use into a mapped buffer of bufferBytes.
    void write(void *buffer) const
    {
        auto *bytes = static_cast<unsigned char *>(buffer);
        std::copy_n(reinterpret_cast<const unsigned char *>(header.data()),16,bytes);
        std::copy_n(reinterpret_cast<const unsigned char *>(shadowFaces.data()),shadowFaces.size()*64,bytes+facesOffset);
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

// The six faces of a point light's shadow cube at position, reaching far meters: world space to clip space (column-
// major, Vulkan depth 0 near to 1 far), each a 90-degree perspective along +x, -x, +y, -y, +z, -z.
inline std::array<std::array<float,16>,6> cubeFaces(const std::array<float,3> &position,float far)
{
    constexpr float axes[6][3]{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    constexpr float ups[6][3]{{0,0,1},{0,0,1},{0,0,1},{0,0,1},{0,1,0},{0,1,0}};
    const float a = far/(shadowNear-far), b = shadowNear*far/(shadowNear-far);
    std::array<std::array<float,16>,6> result{};
    for (int face = 0; face < 6; ++face) {
        const float *forward = axes[face], *up0 = ups[face];
        // right = forward x up, up = right x forward
        const float right[3]{forward[1]*up0[2]-forward[2]*up0[1],forward[2]*up0[0]-forward[0]*up0[2],
                             forward[0]*up0[1]-forward[1]*up0[0]};
        const float up[3]{right[1]*forward[2]-right[2]*forward[1],right[2]*forward[0]-right[0]*forward[2],
                          right[0]*forward[1]-right[1]*forward[0]};
        const auto dot = [&](const float *v) { return v[0]*position[0]+v[1]*position[1]+v[2]*position[2]; };
        auto &m = result[std::size_t(face)];
        // Rows: x = right, y = up, z = a * (view depth along -forward) + b, w = depth along forward.
        for (int column = 0; column < 3; ++column) {
            m[std::size_t(column*4+0)] = right[column];
            m[std::size_t(column*4+1)] = up[column];
            m[std::size_t(column*4+2)] = -a*forward[column];
            m[std::size_t(column*4+3)] = forward[column];
        }
        m[12] = -dot(right);
        m[13] = -dot(up);
        m[14] = a*dot(forward)+b;
        m[15] = -dot(forward);
    }
    return result;
}

// viewProjection maps world space to the camera's clip space (column-major, Vulkan: y down, depth 0 to 1, no TAA
// jitter); eye and forward give depth along the view.
inline Clusters build(std::span<const Light> input,const std::array<float,16> &viewProjection,
                      const std::array<float,3> &eye,const std::array<float,3> &forward,
                      std::span<const std::array<float,4>> previousShadows = {})
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
        const auto cone = spotCone(*light);
        const auto &d = light->spotDirection;
        const float length = std::max(std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]),1e-6f);
        result.lights.push_back({d[0]/length,d[1]/length,d[2]/length,cone[0]});
        result.lights.push_back({cone[1],0,0,0});
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
    constexpr float screenMargin = 0.01f; // covers the scene shaders' subpixel jitter
    constexpr float infinity = std::numeric_limits<float>::infinity();
    for (std::size_t i = directional; i < ordered.size(); ++i) {
        const auto &light = *ordered[i];
        const float r = result.lights[i*4][3];
        const auto &c = light.vector;
        const float depth = (c[0]-eye[0])*forward[0]+(c[1]-eye[1])*forward[1]+(c[2]-eye[2])*forward[2];
        if (depth+r < 0)
            continue; // entirely behind the camera
        float minX = infinity, minY = infinity, maxX = -infinity, maxY = -infinity;
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
        if (maxX < -1-screenMargin || minX > 1+screenMargin || maxY < -1-screenMargin || minY > 1+screenMargin)
            continue; // off screen
        const auto tile = [](float ndc,int count) {
            return std::clamp(int(std::floor((std::clamp(ndc,-1.0f,1.0f)*0.5f+0.5f)*float(count))),0,count-1);
        };
        boxes[i] = {tile(minX,tilesX),tile(maxX,tilesX),tile(minY,tilesY),tile(maxY,tilesY),slice(depth-r),slice(depth+r)};
    }

    // Shadows for the point lights nearest the camera among those on screen.
    std::vector<std::size_t> candidates;
    for (std::size_t i = directional; i < ordered.size(); ++i)
        if (boxes[i].x0 <= boxes[i].x1)
            candidates.push_back(i);
    const auto distance = [&](std::size_t i) {
        const auto &c = ordered[i]->vector;
        return (c[0]-eye[0])*(c[0]-eye[0])+(c[1]-eye[1])*(c[1]-eye[1])+(c[2]-eye[2])*(c[2]-eye[2]);
    };
    std::ranges::stable_sort(candidates,[&](std::size_t a,std::size_t b) { return distance(a) < distance(b); });
    if (candidates.size() > std::size_t(shadowSlots))
        candidates.resize(std::size_t(shadowSlots));
    // Keep a cubemap in the same atlas slot while its lamp stays selected. Moving or newly selected lamps
    // take remaining slots, so only those faces need a refresh when the camera crosses a lamp boundary.
    std::ranges::sort(candidates);
    const auto unassigned = ordered.size();
    std::array<std::size_t,shadowSlots> assigned;
    assigned.fill(unassigned);
    auto occupied = std::span(assigned).first(candidates.size());
    const auto old = previousShadows.first(std::min(previousShadows.size(),occupied.size()));
    for (auto &index : candidates)
        for (std::size_t slot = 0; slot < old.size(); ++slot)
            if (occupied[slot] == unassigned && result.lights[index*4] == old[slot]) {
                occupied[slot] = index;
                index = unassigned;
                break;
            }
    for (const auto index : candidates)
        if (index != unassigned)
            *std::ranges::find(occupied,unassigned) = index;
    for (std::size_t slot = 0; slot < occupied.size(); ++slot) {
        const auto i = occupied[slot];
        const auto &place = result.lights[i*4];
        const auto faces = cubeFaces(ordered[i]->vector,place[3]);
        std::copy(faces.begin(),faces.end(),result.shadowFaces.begin()+std::ptrdiff_t(slot*6));
        result.lights[i*4+1][3] = 2.0f+float(slot);
        result.shadowed.push_back(place);
    }
    result.header[2] = uint32_t(candidates.size());

    // A light's projected cube is only a first bound. Within a depth slice, its sphere fits a cylinder
    // whose radius is the widest cross-section in that slice. Intersect the cylinder with the tile sides
    // so rooftop pixels skip lamps that can reach the broad projected box only at another depth.
    struct Plane { std::array<float,4> equation; float axial, radial; };
    const auto side = [&](int axis,float cut,float sign) {
        Plane plane{};
        float normalSquared = 0;
        for (int i = 0; i < 4; ++i) {
            plane.equation[i] = sign*(viewProjection[i*4+axis]-cut*viewProjection[i*4+3]);
            if (i < 3) {
                plane.axial += plane.equation[i]*forward[i];
                normalSquared += plane.equation[i]*plane.equation[i];
            }
        }
        plane.radial = std::sqrt(std::max(0.0f,normalSquared-plane.axial*plane.axial));
        return plane;
    };
    const auto sides = [&](auto &lower,auto &upper,int axis) {
        for (std::size_t i = 0; i < lower.size(); ++i) {
            // Leave room for the subpixel camera jitter used by the fragment shader.
            lower[i] = side(axis,2*float(i)/float(lower.size())-1-screenMargin,1);
            upper[i] = side(axis,2*float(i+1)/float(lower.size())-1+screenMargin,-1);
        }
    };
    std::array<Plane,tilesX> left{}, right{};
    std::array<Plane,tilesY> top{}, bottom{};
    sides(left,right,0);
    sides(top,bottom,1);
    std::array<float,slices> starts{};
    for (int z = 1; z < slices; ++z)
        starts[z] = nearDepth*std::pow(farDepth/nearDepth,float(z)/slices);
    struct TileBounds { int x0 = 1,x1 = 0,y0 = 1,y1 = 0; };
    std::vector<std::array<TileBounds,slices>> footprints(ordered.size());
    for (std::size_t i = directional; i < ordered.size(); ++i) {
        const auto &box = boxes[i];
        if (box.x0 > box.x1)
            continue;
        const auto &center = ordered[i]->vector;
        const float radius = result.lights[4*i][3];
        const float depth = (center[0]-eye[0])*forward[0]+(center[1]-eye[1])*forward[1]+(center[2]-eye[2])*forward[2];
        for (int z = box.z0; z <= box.z1; ++z) {
            const float low = z == 0 ? -radius : std::max(-radius,starts[z]-depth-0.001f);
            const float high = z == slices-1 ? radius : std::min(radius,starts[z+1]-depth+0.001f);
            if (high < low)
                continue;
            const float gap = std::clamp(0.0f,low,high);
            const float cross = std::sqrt(std::max(0.0f,radius*radius-gap*gap));
            const auto misses = [&](const Plane &plane) {
                const auto &p = plane.equation;
                const float base = p[0]*center[0]+p[1]*center[1]+p[2]*center[2]+p[3];
                return base+std::max(plane.axial*low,plane.axial*high)+plane.radial*cross < -0.001f;
            };
            auto &tile = footprints[i][z];
            tile = {std::max(0,box.x0-1),std::min(tilesX-1,box.x1+1),
                    std::max(0,box.y0-1),std::min(tilesY-1,box.y1+1)};
            while (tile.x0 <= tile.x1 && misses(left[tile.x0])) ++tile.x0;
            while (tile.x0 <= tile.x1 && misses(right[tile.x1])) --tile.x1;
            while (tile.y0 <= tile.y1 && misses(top[tile.y0])) ++tile.y0;
            while (tile.y0 <= tile.y1 && misses(bottom[tile.y1])) --tile.y1;
        }
    }

    // Count, then lay out the lists, then fill them.
    std::vector<uint32_t> counts(std::size_t(clusterCount),0);
    const auto visit = [&](std::size_t light,auto &&action) {
        for (int z = boxes[light].z0; z <= boxes[light].z1; ++z) {
            const auto &tile = footprints[light][z];
            for (int y = tile.y0; y <= tile.y1; ++y)
                for (int x = tile.x0; x <= tile.x1; ++x)
                    action(std::size_t((z*tilesY+y)*tilesX+x));
        }
    };
    for (std::size_t i = directional; i < ordered.size(); ++i)
        visit(i,[&](std::size_t cluster) { ++counts[cluster]; });
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
        visit(i,[&](std::size_t cluster) {
            auto &entry = result.ranges[cluster];
            if (entry[1] < counts[cluster])
                result.indices[entry[0]+entry[1]++] = uint32_t(i);
        });
    return result;
}

inline std::array<bool,shadowSlots> shadowUpdates(std::span<const std::array<float,4>> active,
                                                  std::span<const std::array<float,4>> rendered,bool all)
{
    std::array<bool,shadowSlots> update{};
    for (std::size_t slot = 0; slot < std::min(active.size(),update.size()); ++slot)
        update[slot] = all || slot >= rendered.size() || active[slot] != rendered[slot];
    return update;
}

} // namespace worldforge::lights
