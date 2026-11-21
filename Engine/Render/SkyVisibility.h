#pragma once

#include "Engine/Render/ShadowCascades.h"

#include <array>
#include <cmath>
#include <cstdint>

// Large-scale sky occlusion, shared by the editor viewport and the Player (Shading.glsl samples it).
//
// The scene is rendered into depth maps from 16 directions spread over the sky, like a sun shadow map for each. A
// surface point then tests which of those directions reach the sky, and the share of sky light it receives is the
// sum over the open directions of their sky brightness times the cosine to its normal. This finds what screen-space
// ambient occlusion cannot see: the roof over a room seen through its doorway, an awning, a narrow alley.
namespace worldforge::sky {

constexpr int directionCount = 16;
constexpr uint32_t tileSize = 512;              // texels per direction, square
constexpr uint32_t atlasSize = tileSize*4;      // 4 x 4 tiles: direction i at column i % 4, row i / 4
constexpr float maximumRadius = 150.0f;         // meters: the largest region the maps cover
constexpr float lowestElevation = 0.08f;        // sine of the lowest direction's elevation (about 4.6 degrees)

struct Visibility {
    // World space to each direction's map: x and y in [-1, 1] across the tile, depth in [0, 1] away from the sky.
    std::array<shadow::Matrix,directionCount> matrices{};
    // Directions toward the sky, unit length; w unused.
    std::array<std::array<float,4>,directionCount> directions{};
    float metersPerTexel = 0;
    shadow::Vector center{};
    float radius = 0;
};

inline std::array<uint32_t,2> tileOrigin(int direction)
{
    return {uint32_t(direction%4)*tileSize,uint32_t(direction/4)*tileSize};
}

// The directions, spread evenly over the sky above lowestElevation (a Fibonacci spiral), as unit vectors.
inline std::array<shadow::Vector,directionCount> skyDirections()
{
    constexpr float goldenAngle = 2.39996323f;
    std::array<shadow::Vector,directionCount> result;
    for (int i = 0; i < directionCount; ++i) {
        const float z = 1-(1-lowestElevation)*(float(i)+0.5f)/directionCount;
        const float ring = std::sqrt(std::max(0.0f,1-z*z));
        result[std::size_t(i)] = {ring*std::cos(goldenAngle*float(i)),ring*std::sin(goldenAngle*float(i)),z};
    }
    return result;
}

// Maps covering bounds (at most maximumRadius around their center).
inline Visibility fit(const shadow::Bounds &bounds)
{
    using namespace shadow::detail;
    using shadow::Vector;
    const Vector center = scale(add(bounds.minimum,bounds.maximum),0.5f);
    const Vector half = scale(subtract(bounds.maximum,bounds.minimum),0.5f);
    const float radius = std::clamp(std::sqrt(dot(half,half))+1.0f,2.0f,maximumRadius);
    Visibility result;
    result.metersPerTexel = 2*radius/float(tileSize);
    result.center = center;
    result.radius = radius;
    const auto directions = skyDirections();
    for (int i = 0; i < directionCount; ++i) {
        const Vector toward = directions[std::size_t(i)];
        const Vector reference = std::abs(toward[2]) > 0.98f ? Vector{0,1,0} : Vector{0,0,1};
        const Vector right = normalized(cross(reference,toward));
        const Vector up = cross(toward,right);
        const float x = dot(center,right), y = dot(center,up);
        const float front = dot(center,toward)+radius, range = 2*radius;
        result.matrices[std::size_t(i)] = {right[0]/radius,up[0]/radius,-toward[0]/range,0,
                                           right[1]/radius,up[1]/radius,-toward[1]/range,0,
                                           right[2]/radius,up[2]/radius,-toward[2]/range,0,
                                           -x/radius,-y/radius,front/range,1};
        result.directions[std::size_t(i)] = {toward[0],toward[1],toward[2],0};
    }
    return result;
}

} // namespace worldforge::sky
