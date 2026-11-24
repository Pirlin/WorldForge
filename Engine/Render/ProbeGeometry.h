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
    static constexpr std::size_t triangleBudget = 24'000;
    Vector origin{};

    static Volume around(Vector focus);
    std::array<ProbePosition,count> positions() const;
    bool intersects(const std::array<Vector,3> &vertices) const;
    bool operator==(const Volume &) const = default;
};

static_assert(sizeof(Node) == 48 && sizeof(Triangle) == 80 && sizeof(ProbeDirection) == 32);

}
