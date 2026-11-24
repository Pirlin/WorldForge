#pragma once

#include "Engine/World/PlayableScene.h"

#include <algorithm>
#include <cmath>

namespace worldforge {

constexpr uint32_t meshletVertexLimit = 64, meshletTriangleLimit = 124;

// Storage layout used by PlayMeshlets.mesh. The indices address the unchanged PlayVertex stream.
struct Meshlet {
    std::array<uint32_t,4> counts{}; // vertices, triangles, first source corner, unused
    std::array<float,4> center{}, extent{}, cone{};
    std::array<uint32_t,meshletVertexLimit> vertices{};
    std::array<uint32_t,meshletTriangleLimit> triangles{}; // three local indices packed in the low 18 bits
};
static_assert(sizeof(Meshlet) == 816);

inline void appendMeshlets(std::vector<Meshlet> &output,std::span<const PlayVertex> vertices,uint32_t first,uint32_t count)
{
    const auto equal = [](const PlayVertex &a,const PlayVertex &b) {
        return a.position == b.position && a.normal == b.normal && a.color == b.color && a.wind == b.wind && a.uv == b.uv;
    };
    const auto unit = [](Point3 v) {
        const float length = std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
        if (length > 1e-10f)
            for (float &coordinate : v)
                coordinate /= length;
        return v;
    };
    const auto dot = [](Point3 a,Point3 b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; };
    Meshlet meshlet;
    auto &vertexCount = meshlet.counts[0];
    auto &triangleCount = meshlet.counts[1];
    meshlet.counts[2] = first;
    const auto flush = [&] {
        Point3 minimum = vertices[meshlet.vertices[0]].position, maximum = minimum;
        for (uint32_t v = 0; v < vertexCount; ++v) {
            const auto &vertex = vertices[meshlet.vertices[v]];
            for (std::size_t axis = 0; axis < minimum.size(); ++axis) {
                const float sway = axis < 2 ? std::abs(vertex.wind) : 0;
                minimum[axis] = std::min(minimum[axis],vertex.position[axis]-sway);
                maximum[axis] = std::max(maximum[axis],vertex.position[axis]+sway);
            }
        }
        for (std::size_t axis = 0; axis < minimum.size(); ++axis) {
            meshlet.center[axis] = (minimum[axis]+maximum[axis])*0.5f;
            meshlet.extent[axis] = (maximum[axis]-minimum[axis])*0.5f;
        }
        std::array<Point3,meshletTriangleLimit> normals{};
        Point3 axis{};
        for (uint32_t tri = 0; tri < triangleCount; ++tri) {
            const uint32_t indices = meshlet.triangles[tri];
            const auto point = [&](uint32_t corner) {
                return vertices[meshlet.vertices[(indices >> (corner*6)) & 63u]].position;
            };
            const auto p = point(0), q = point(1), r = point(2);
            const Point3 a{q[0]-p[0],q[1]-p[1],q[2]-p[2]}, b{r[0]-p[0],r[1]-p[1],r[2]-p[2]};
            normals[tri] = unit({a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]});
            for (std::size_t component = 0; component < axis.size(); ++component)
                axis[component] += normals[tri][component];
        }
        axis = unit(axis);
        float cutoff = dot(axis,axis) > 0.5f ? 1.0f : -1.0f;
        for (uint32_t tri = 0; tri < triangleCount; ++tri)
            cutoff = std::min(cutoff,dot(axis,normals[tri]));
        meshlet.cone = {axis[0],axis[1],axis[2],cutoff > 0 ? cutoff : -1.0f};
        output.push_back(meshlet);
        meshlet = {};
    };
    for (uint32_t corner = first; corner+2 < first+count; corner += 3) {
        // Reserve up to three distinct corners before accepting the next complete triangle.
        if (triangleCount == meshletTriangleLimit || vertexCount > meshletVertexLimit-3) {
            flush();
            meshlet.counts[2] = corner;
        }
        uint32_t packed = 0;
        for (uint32_t part = 0; part < 3; ++part) {
            const auto &vertex = vertices[corner+part];
            uint32_t local = 0;
            while (local < vertexCount && !equal(vertices[meshlet.vertices[local]],vertex))
                ++local;
            if (local == vertexCount)
                meshlet.vertices[vertexCount++] = corner+part;
            packed |= local << (6*part);
        }
        meshlet.triangles[triangleCount++] = packed;
    }
    if (triangleCount)
        flush();
}

}
