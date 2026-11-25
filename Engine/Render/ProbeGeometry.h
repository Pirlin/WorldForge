#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace worldforge::probes {

using Vector = std::array<float,3>;

struct SurfaceTriangle {
    std::array<Vector,3> vertices;
    Vector diffuse;   // linear RGB, after removing the metallic and transmissive shares
    Vector emission;  // linear scene radiance
};

// std430 layouts shared with ProbeTrace.comp. Nodes are depth-first, with a jump past each subtree.
struct Node {
    Vector minimum;
    uint32_t first;
    Vector maximum;
    uint32_t escape;
    uint32_t count, padding[3]{};
};

struct Triangle {
    std::array<float,4> origin, edge1, edge2, diffuse, emission;
};

struct Geometry {
    static constexpr std::size_t maximumTriangles = 1'000'000;
    std::vector<Node> nodes;
    std::vector<Triangle> triangles;
};

struct Hit {
    uint32_t triangle;
    float distance;
};

// Rejects invalid coordinates and material values. Degenerate triangles are omitted.
std::optional<Geometry> buildGeometry(std::span<const SurfaceTriangle> surfaces);

// CPU reference for the GPU traversal. Direction is normalized so distances are in meters.
std::optional<Hit> trace(const Geometry &geometry,Vector origin,Vector direction,float maximumDistance);

inline constexpr uint32_t raysPerProbe = 64, directionsPerProbe = 6;
inline constexpr uint32_t reflectionMapSize = 8;
using ProbePosition = std::array<float,4>; // world-space XYZ, unused W

struct ProbeDirection {
    // Normalized by the cosine weight sum so RGB is irradiance / pi, in scene units.
    std::array<float,4> irradiance;
    // Mean distance and second moment, divided by the trace range and its square respectively.
    std::array<float,4> distanceMoments;
};

using Probe = std::array<ProbeDirection,directionsPerProbe>; // +X, -X, +Y, -Y, +Z, -Z
// One radiance and normalized hit distance per texel of an octahedral reflection map.
using ReflectionMap = std::array<std::array<float,4>,reflectionMapSize*reflectionMapSize>;

struct Volume {
    static constexpr std::array<uint32_t,3> dimensions{6,6,4};
    static constexpr uint32_t count = dimensions[0]*dimensions[1]*dimensions[2];
    static constexpr float spacing = 2, range = 20;
    static constexpr std::size_t triangleBudget = 60'000; // around the coarse grid (CoarseVolume)
    Vector origin{};

    static Volume around(Vector focus);
    std::array<ProbePosition,count> positions() const;
    bool intersects(const std::array<Vector,3> &vertices) const;
    bool operator==(const Volume &) const = default;
};

// A coarser grid traced with the fine one, reaching about 21 m around its focus, so surfaces beyond the fine grid (a
// shop front across the street) still get bounce light. Its probes follow the fine grid's in the trace's output, and
// the shaders blend from the fine grid to it where the fine one ends (Shading.glsl diffuseFromProbes).
struct CoarseVolume {
    static constexpr std::array<uint32_t,3> dimensions{8,8,4};
    static constexpr uint32_t count = dimensions[0]*dimensions[1]*dimensions[2];
    static constexpr float spacing = 6;
    Vector origin{};

    static CoarseVolume around(Vector focus);
    std::array<ProbePosition,count> positions() const;
    // Also true for every triangle the fine grid around the same camera needs (it lies inside).
    bool intersects(const std::array<Vector,3> &vertices) const;
    bool operator==(const CoarseVolume &) const = default;
};

static_assert(sizeof(Node) == 48 && sizeof(Triangle) == 80 && sizeof(ProbeDirection) == 32);

// Light on the surfaces probe rays hit, besides the sun and their own emission (ProbeTrace.comp): the sky's irradiance
// over pi as spherical harmonics (SkyLight.h, coefficients 0 to 8, as the scene shaders get them), and the point and
// spot lights nearest the volume in the clustered-light layout (LightClusters.h: four vec4 per light), with their full
// range rather than the exposure-dependent one. Without them a night street's walls miss the lamp and sky light the
// ground bounces onto them.
struct HitLighting {
    static constexpr std::size_t maximumLights = 32;
    std::array<std::array<float,4>,9> sky{};
    std::vector<std::array<float,4>> lights;
    bool operator==(const HitLighting &) const = default;
};

// The lights (positions, intensities, colors and spot cones as LightClusters.h takes them; directional ones are
// skipped) whose light can reach a surface a probe ray in volume hits, nearest first, up to maximumLights.
struct LightSource {
    std::array<float,3> position;
    float intensity;
    std::array<float,3> color;
    std::array<float,3> spotDirection;
    std::array<float,2> cone; // cos(cone / 2) and the spot scale (LightClusters.h spotCone)
    float range;
};
HitLighting hitLighting(const std::array<std::array<float,4>,9> &sky,std::span<const LightSource> lights,const CoarseVolume &volume);

}
