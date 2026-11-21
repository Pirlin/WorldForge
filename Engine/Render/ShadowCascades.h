#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

// Cascaded shadow maps for the sun, shared by the editor viewport and the Player (Lighting.glsl samples them).
//
// The view is cut into four depth slices, from the nearest to the farthest scene point the camera sees (at most
// maximumDistance). Each slice gets a square tile of the shadow atlas, fitted to a sphere around the slice so its
// size does not change as the camera turns, and moved in whole texels so shadows do not shimmer as it moves.
namespace worldforge::shadow {

constexpr int cascadeCount = 4;
constexpr uint32_t tileSize = 1536;            // texels per cascade, square
constexpr uint32_t atlasSize = tileSize*2;     // 2 x 2 tiles: cascade i at column i % 2, row i / 2
constexpr float sunAngularDiameter = 0.00925f; // radians (0.53 degrees): the real sun, and the Cycles reference's
constexpr float maximumDistance = 250.0f;      // meters from the camera that receive sun shadows
constexpr float casterReach = 200.0f;          // meters toward the sun beyond a slice where casters still count
constexpr float splitBlend = 0.7f;             // 0 splits evenly, 1 logarithmically

using Matrix = std::array<float,16>; // column-major, as GLSL reads it
using Vector = std::array<float,3>;

struct Bounds {
    Vector minimum, maximum;
};

struct Cascades {
    // World space to each cascade's clip space: x and y in [-1, 1] across the tile, depth in [0, 1] away from the sun.
    std::array<Matrix,cascadeCount> matrices{};
    // Per cascade: meters per texel, depth range in meters, far end of its slice in meters from the camera, 0.
    std::array<std::array<float,4>,cascadeCount> scales{};
};

inline std::array<uint32_t,2> tileOrigin(int cascade)
{
    return {uint32_t(cascade%2)*tileSize,uint32_t(cascade/2)*tileSize};
}

namespace detail {

inline Vector add(Vector a,Vector b) { return {a[0]+b[0],a[1]+b[1],a[2]+b[2]}; }
inline Vector subtract(Vector a,Vector b) { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; }
inline Vector scale(Vector a,float s) { return {a[0]*s,a[1]*s,a[2]*s}; }
inline float dot(Vector a,Vector b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
inline Vector cross(Vector a,Vector b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
inline Vector normalized(Vector a)
{
    const float length = std::sqrt(dot(a,a));
    return length > 0 ? scale(a,1/length) : Vector{0,0,1};
}
inline Vector lerp(Vector a,Vector b,float t) { return add(a,scale(subtract(b,a),t)); }

// A clip-space point (depth 0 near, 1 far) in world space.
inline Vector unproject(const Matrix &inverse,float x,float y,float z)
{
    float result[4];
    for (int row = 0; row < 4; ++row)
        result[row] = inverse[row]*x+inverse[4+row]*y+inverse[8+row]*z+inverse[12+row];
    const float w = std::abs(result[3]) > 1e-20f ? result[3] : 1e-20f;
    return {result[0]/w,result[1]/w,result[2]/w};
}

} // namespace detail

// inverseViewProjection maps the camera's clip space (depth 0 at the near plane, 1 at the far plane; no TAA jitter)
// to world space. towardSun need not be normalized. bounds, when known, are the scene's receivers: the cascades
// then cover only the depth range where the scene is.
inline Cascades fit(const Matrix &inverseViewProjection,Vector towardSun,std::optional<Bounds> bounds = std::nullopt)
{
    using namespace detail;
    std::array<Vector,4> nearCorners,farCorners;
    for (int corner = 0; corner < 4; ++corner) {
        const float x = corner%2 ? 1.0f : -1.0f, y = corner/2 ? 1.0f : -1.0f;
        nearCorners[corner] = unproject(inverseViewProjection,x,y,0);
        farCorners[corner] = unproject(inverseViewProjection,x,y,1);
    }
    const Vector nearCenter = unproject(inverseViewProjection,0,0,0);
    const Vector axisSpan = subtract(unproject(inverseViewProjection,0,0,1),nearCenter);
    const float viewDepth = std::max(std::sqrt(dot(axisSpan,axisSpan)),1e-3f);
    const Vector axis = scale(axisSpan,1/viewDepth);

    // The depth range to cover, from the near plane.
    float start = 0, end = std::min(viewDepth,maximumDistance);
    if (bounds) {
        float lowest = end, highest = 0;
        for (int corner = 0; corner < 8; ++corner) {
            const Vector point{corner & 1 ? bounds->maximum[0] : bounds->minimum[0],corner & 2 ? bounds->maximum[1] : bounds->minimum[1],
                               corner & 4 ? bounds->maximum[2] : bounds->minimum[2]};
            const float depth = dot(subtract(point,nearCenter),axis);
            lowest = std::min(lowest,depth);
            highest = std::max(highest,depth);
        }
        start = std::clamp(lowest,0.0f,end);
        end = std::clamp(highest,start,end);
    }
    end = std::max(end,start+1.0f);
    std::array<float,cascadeCount+1> splits{};
    const float logStart = std::max(start,0.5f);
    for (int i = 0; i <= cascadeCount; ++i) {
        const float fraction = float(i)/cascadeCount;
        const float logarithmic = logStart*std::pow(end/logStart,fraction);
        const float even = start+(end-start)*fraction;
        splits[i] = std::max(start,splitBlend*logarithmic+(1-splitBlend)*even);
    }
    splits[0] = start;
    splits[cascadeCount] = end;

    const Vector sun = normalized(towardSun);
    const Vector reference = std::abs(sun[2]) > 0.98f ? Vector{0,1,0} : Vector{0,0,1};
    const Vector right = normalized(cross(reference,sun));
    const Vector up = cross(sun,right);

    Cascades result;
    for (int cascade = 0; cascade < cascadeCount; ++cascade) {
        std::array<Vector,8> corners;
        for (int corner = 0; corner < 4; ++corner) {
            corners[corner] = lerp(nearCorners[corner],farCorners[corner],splits[cascade]/viewDepth);
            corners[corner+4] = lerp(nearCorners[corner],farCorners[corner],splits[cascade+1]/viewDepth);
        }
        Vector center{0,0,0};
        for (const auto &corner : corners)
            center = add(center,scale(corner,1.0f/8));
        float radius = 0;
        for (const auto &corner : corners) {
            const auto offset = subtract(corner,center);
            radius = std::max(radius,std::sqrt(dot(offset,offset)));
        }
        // Quantized so small camera motions keep the same tile size.
        radius = std::ceil(std::max(radius,0.5f)*16)/16;
        const float texel = 2*radius/float(tileSize);
        const float x = std::floor(dot(center,right)/texel)*texel;
        const float y = std::floor(dot(center,up)/texel)*texel;
        const float front = dot(center,sun)+radius+casterReach;
        const float range = 2*radius+casterReach;
        auto &m = result.matrices[cascade];
        m = {right[0]/radius,up[0]/radius,-sun[0]/range,0,
             right[1]/radius,up[1]/radius,-sun[1]/range,0,
             right[2]/radius,up[2]/radius,-sun[2]/range,0,
             -x/radius,-y/radius,front/range,1};
        result.scales[cascade] = {texel,range,splits[cascade+1],0};
    }
    return result;
}

// Where a world point lands in a cascade: tile coordinates x, y in [0, 1] and depth in [0, 1].
inline Vector project(const Cascades &cascades,int cascade,Vector point)
{
    const auto &m = cascades.matrices[cascade];
    Vector result;
    for (int row = 0; row < 3; ++row)
        result[row] = m[row]*point[0]+m[4+row]*point[1]+m[8+row]*point[2]+m[12+row];
    return {result[0]*0.5f+0.5f,result[1]*0.5f+0.5f,result[2]};
}

} // namespace worldforge::shadow
