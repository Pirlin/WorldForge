#include "Engine/Geometry/SculptBrush.h"

#include <algorithm>
#include <cmath>

namespace worldforge {
namespace {

Point3 add(Point3 a,Point3 b) { return {a[0]+b[0],a[1]+b[1],a[2]+b[2]}; }
Point3 subtract(Point3 a,Point3 b) { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; }
Point3 scale(Point3 p,float amount) { return {p[0]*amount,p[1]*amount,p[2]*amount}; }
float dot(Point3 a,Point3 b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
Point3 unit(Point3 p)
{
    const float length = std::hypot(p[0],p[1],p[2]);
    return length > 0 ? scale(p,1/length) : Point3{};
}

}

SculptStroke::SculptStroke(const Mesh &source) : neighbors_(source.vertices.size()),faces_(source.vertices.size())
{
    for (std::size_t f = 0; f < source.faces.size(); ++f) {
        const auto &corners = source.faces[f].corners;
        for (std::size_t i = 0; i < corners.size(); ++i) {
            const auto vertex = corners[i].vertex;
            faces_[vertex].push_back(f);
            neighbors_[vertex].push_back(corners[(i+1)%corners.size()].vertex);
            neighbors_[vertex].push_back(corners[(i+corners.size()-1)%corners.size()].vertex);
        }
    }
    for (auto &neighbors : neighbors_) {
        std::ranges::sort(neighbors);
        neighbors.erase(std::unique(neighbors.begin(),neighbors.end()),neighbors.end());
    }
}

bool SculptStroke::dab(Mesh &mesh,Point3 center,Point3 normal,float radius,float strength,SculptBrush brush,bool symmetry)
{
    if (mesh.vertices.size() != neighbors_.size() || !std::ranges::all_of(center,[](float v) { return std::isfinite(v); })
        || !std::ranges::all_of(normal,[](float v) { return std::isfinite(v); }) || !std::isfinite(radius) || radius <= 0
        || !std::isfinite(strength) || strength == 0)
        return false;
    normal = unit(normal);
    if (normal == Point3{})
        return false;
    auto mirror = center;
    mirror[0] = -mirror[0];
    struct Move { VertexIndex vertex; Point3 position; };
    std::vector<Move> moves;
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const auto p = mesh.vertices[i];
        auto offset = subtract(p,center);
        auto axis = normal;
        const auto reflected = subtract(p,mirror);
        if (symmetry && (dot(reflected,reflected) < dot(offset,offset) || (center[0] == 0 && p[0] < 0))) {
            offset = reflected;
            axis[0] = -axis[0];
        }
        const auto distance = std::hypot(offset[0],offset[1],offset[2])/radius;
        if (!std::isfinite(distance) || distance >= 1 || faces_[i].empty())
            continue;
        const auto falloff = 1-distance*distance*(3-2*distance);
        const auto weight = std::clamp(strength,-1.0f,1.0f)*falloff;
        auto delta = scale(axis,radius*0.12f*weight);
        if (brush == SculptBrush::Smooth) {
            Point3 average{};
            for (const auto v : neighbors_[i])
                average = add(average,mesh.vertices[v]);
            delta = scale(subtract(scale(average,1.0f/float(neighbors_[i].size())),p),std::abs(weight)*0.5f);
        } else if (brush == SculptBrush::Inflate) {
            Point3 direction{};
            for (const auto f : faces_[i])
                direction = add(direction,faceNormal(mesh,mesh.faces[f]));
            delta = scale(unit(direction),radius*0.12f*weight);
        } else if (brush == SculptBrush::Flatten) {
            delta = scale(axis,-dot(offset,axis)*std::abs(weight)*0.5f);
        }
        auto next = add(p,delta);
        if (symmetry && std::abs(p[0]) <= 1e-6f)
            next[0] = 0;
        if (next != p)
            moves.push_back({VertexIndex(i),next});
    }
    for (const auto &move : moves) {
        mesh.vertices[move.vertex] = move.position;
        for (const auto f : faces_[move.vertex])
            for (auto &corner : mesh.faces[f].corners)
                corner.normal = {};
    }
    return !moves.empty();
}

}
