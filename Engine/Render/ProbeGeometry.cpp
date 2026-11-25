#include "Engine/Render/ProbeGeometry.h"

#include "Engine/Render/ShadowCascades.h"

#include <algorithm>
#include <cmath>

namespace worldforge::probes {
namespace {
using shadow::detail::cross;
using shadow::detail::dot;
using shadow::detail::subtract;

struct Primitive {
    Triangle triangle;
    shadow::Bounds bounds;
    Vector center;
};

constexpr float coordinateLimit = 1'000'000;
constexpr float rayBias = 0.002f;

std::array<float,4> packed(Vector value)
{
    return {value[0],value[1],value[2],0};
}

Vector xyz(const std::array<float,4> &value)
{
    return {value[0],value[1],value[2]};
}

void include(shadow::Bounds &bounds,const shadow::Bounds &other)
{
    for (int axis = 0; axis < 3; ++axis) {
        bounds.minimum[axis] = std::min(bounds.minimum[axis],other.minimum[axis]);
        bounds.maximum[axis] = std::max(bounds.maximum[axis],other.maximum[axis]);
    }
}

void buildSubtree(Geometry &geometry,std::span<Primitive> primitives)
{
    auto bounds = primitives.front().bounds;
    shadow::Bounds centers{primitives.front().center,primitives.front().center};
    for (const auto &primitive : primitives.subspan(1)) {
        include(bounds,primitive.bounds);
        include(centers,{primitive.center,primitive.center});
    }
    const auto node = geometry.nodes.size();
    geometry.nodes.push_back({bounds.minimum,0,bounds.maximum,0,0});
    if (primitives.size() <= 4) {
        geometry.nodes[node].first = uint32_t(geometry.triangles.size());
        geometry.nodes[node].count = uint32_t(primitives.size());
        for (const auto &primitive : primitives)
            geometry.triangles.push_back(primitive.triangle);
    } else {
        const auto spread = subtract(centers.maximum,centers.minimum);
        const auto axis = std::ranges::max_element(spread)-spread.begin();
        const auto middle = primitives.size()/2;
        std::ranges::nth_element(primitives,primitives.begin()+middle,{},[axis](const Primitive &p) { return p.center[axis]; });
        buildSubtree(geometry,primitives.first(middle));
        buildSubtree(geometry,primitives.subspan(middle));
    }
    geometry.nodes[node].escape = uint32_t(geometry.nodes.size());
}

bool intersects(const Node &node,Vector origin,Vector direction,float upper)
{
    float lower = rayBias;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(direction[axis]) < 1e-8f) {
            if (origin[axis] < node.minimum[axis] || origin[axis] > node.maximum[axis])
                return false;
            continue;
        }
        const float a = (node.minimum[axis]-origin[axis])/direction[axis];
        const float b = (node.maximum[axis]-origin[axis])/direction[axis];
        lower = std::max(lower,std::min(a,b));
        upper = std::min(upper,std::max(a,b));
        if (lower > upper)
            return false;
    }
    return true;
}

float triangleDistance(const Triangle &triangle,Vector origin,Vector direction,float limit)
{
    constexpr float edgeTolerance = 1e-6f;
    const auto edge1 = xyz(triangle.edge1), edge2 = xyz(triangle.edge2);
    const auto perpendicular = cross(direction,edge2);
    const float determinant = dot(edge1,perpendicular);
    if (std::abs(determinant) < 1e-8f)
        return limit;
    const auto offset = subtract(origin,xyz(triangle.origin));
    const float u = dot(offset,perpendicular)/determinant;
    if (u < -edgeTolerance || u > 1+edgeTolerance)
        return limit;
    const auto across = cross(offset,edge1);
    const float v = dot(direction,across)/determinant;
    if (v < -edgeTolerance || u+v > 1+edgeTolerance)
        return limit;
    const float distance = dot(edge2,across)/determinant;
    return distance > rayBias && distance < limit ? distance : limit;
}
}

std::optional<Geometry> buildGeometry(std::span<const SurfaceTriangle> surfaces)
{
    if (surfaces.size() > Geometry::maximumTriangles)
        return std::nullopt;
    std::vector<Primitive> primitives;
    primitives.reserve(surfaces.size());
    for (const auto &surface : surfaces) {
        const auto coordinate = [](float component) { return std::abs(component) <= coordinateLimit; };
        if (!std::ranges::all_of(surface.vertices,[&](const Vector &value) { return std::ranges::all_of(value,coordinate); })
            || !std::ranges::all_of(surface.diffuse,[](float value) { return value >= 0 && value <= 1; })
            || !std::ranges::all_of(surface.emission,[](float value) { return value >= 0 && std::isfinite(value); }))
            return std::nullopt;
        const auto &a = surface.vertices[0], &b = surface.vertices[1], &c = surface.vertices[2];
        const auto edge1 = subtract(b,a), edge2 = subtract(c,a);
        const auto normal = cross(edge1,edge2);
        if (dot(normal,normal) <= 1e-16f)
            continue;
        Primitive primitive{{packed(a),packed(edge1),packed(edge2),packed(surface.diffuse),packed(surface.emission)},{a,a},{}};
        include(primitive.bounds,{b,b});
        include(primitive.bounds,{c,c});
        for (int axis = 0; axis < 3; ++axis)
            primitive.center[axis] = (a[axis]+b[axis]+c[axis])/3;
        primitives.push_back(primitive);
    }
    Geometry geometry;
    if (!primitives.empty()) {
        geometry.nodes.reserve(primitives.size());
        geometry.triangles.reserve(primitives.size());
        buildSubtree(geometry,primitives);
    }
    return geometry;
}

std::optional<Hit> trace(const Geometry &geometry,Vector origin,Vector direction,float maximumDistance)
{
    std::optional<Hit> hit;
    auto closest = maximumDistance;
    for (uint32_t current = 0; current < geometry.nodes.size();) {
        const auto &node = geometry.nodes[current];
        if (!intersects(node,origin,direction,closest)) {
            current = node.escape;
            continue;
        }
        for (auto index = node.first; index < node.first+node.count; ++index) {
            const auto distance = triangleDistance(geometry.triangles[index],origin,direction,closest);
            if (distance < closest) {
                closest = distance;
                hit = Hit{index,distance};
            }
        }
        ++current;
    }
    return hit;
}

Volume Volume::around(Vector focus)
{
    Volume volume;
    // Shift the grid by more than one cell at a time and keep its probes away from integer-height floors.
    for (std::size_t axis = 0; axis < focus.size(); ++axis) {
        const float step = axis == 2 ? spacing : 2*spacing;
        volume.origin[axis] = std::round(focus[axis]/step)*step-float(dimensions[axis]-1)*spacing*0.5f+0.35f;
    }
    return volume;
}

std::array<ProbePosition,Volume::count> Volume::positions() const
{
    std::array<ProbePosition,count> result;
    for (uint32_t z = 0; z < dimensions[2]; ++z)
        for (uint32_t y = 0; y < dimensions[1]; ++y)
            for (uint32_t x = 0; x < dimensions[0]; ++x)
                result[x+dimensions[0]*(y+dimensions[1]*z)] =
                    {origin[0]+float(x)*spacing,origin[1]+float(y)*spacing,origin[2]+float(z)*spacing,0};
    return result;
}

bool Volume::intersects(const std::array<Vector,3> &vertices) const
{
    for (std::size_t axis = 0; axis < origin.size(); ++axis) {
        const float a = vertices[0][axis], b = vertices[1][axis], c = vertices[2][axis];
        if (std::max({a,b,c}) < origin[axis]-range
            || std::min({a,b,c}) > origin[axis]+float(dimensions[axis]-1)*spacing+range)
            return false;
    }
    return true;
}

HitLighting hitLighting(const std::array<std::array<float,4>,9> &sky,std::span<const LightSource> lights,const Volume &volume)
{
    HitLighting result;
    result.sky = sky;
    // Rays leave the probes for up to Volume::range, so a light matters when its range reaches that far around the grid.
    Vector center{}, half{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        half[axis] = float(Volume::dimensions[axis]-1)*Volume::spacing*0.5f;
        center[axis] = volume.origin[axis]+half[axis];
    }
    std::vector<std::pair<float,const LightSource *>> reaching;
    for (const auto &light : lights) {
        if (!(light.intensity > 0) || !(light.range > 0))
            continue;
        float outside = 0, squared = 0;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const float offset = std::abs(light.position[axis]-center[axis]);
            outside += std::pow(std::max(0.0f,offset-half[axis]-Volume::range),2.0f);
            squared += offset*offset;
        }
        if (outside < light.range*light.range)
            reaching.push_back({squared,&light});
    }
    std::ranges::sort(reaching,{},&std::pair<float,const LightSource *>::first);
    for (std::size_t i = 0; i < std::min(reaching.size(),HitLighting::maximumLights); ++i) {
        const auto &light = *reaching[i].second;
        result.lights.push_back({light.position[0],light.position[1],light.position[2],light.range});
        result.lights.push_back({light.color[0]*light.intensity,light.color[1]*light.intensity,light.color[2]*light.intensity,0});
        result.lights.push_back({light.spotDirection[0],light.spotDirection[1],light.spotDirection[2],light.cone[0]});
        result.lights.push_back({light.cone[1],0,0,0});
    }
    return result;
}

}
