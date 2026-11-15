#include "Engine/Geometry/MeshTopology.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace worldforge {

MeshTopology::MeshTopology(const Mesh &mesh)
    : vertexEdges(mesh.vertices.size()), faceEdges(mesh.faces.size())
{
    for (std::size_t face = 0; face < mesh.faces.size(); ++face) {
        const auto &corners = mesh.faces[face].corners;
        auto &edges = faceEdges[face];
        edges.reserve(corners.size());
        for (std::size_t i = 0; i < corners.size(); ++i) {
            const MeshEdge edge = std::minmax(corners[i].vertex,corners[(i+1)%corners.size()].vertex);
            edges.push_back(edge);
            auto [entry,inserted] = edgeFaces.try_emplace(edge);
            entry->second.push_back(face);
            if (inserted) {
                vertexEdges[edge.first].push_back(edge);
                vertexEdges[edge.second].push_back(edge);
            }
        }
    }
}

std::vector<float> connectedFalloff(const Mesh &mesh,const std::vector<VertexIndex> &seeds,float radius)
{
    std::vector<float> weights(mesh.vertices.size());
    for (const auto vertex : seeds)
        if (vertex < weights.size())
            weights[vertex] = 1;
    if (!(radius > 0) || !std::isfinite(radius) || seeds.empty())
        return weights;
    const MeshTopology topology(mesh);
    std::vector<float> distance(mesh.vertices.size(),std::numeric_limits<float>::infinity());
    using Entry = std::pair<float,VertexIndex>;
    std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> queue;
    for (const auto vertex : seeds) {
        if (vertex >= weights.size() || distance[vertex] == 0)
            continue;
        distance[vertex] = 0;
        queue.emplace(0.0f,vertex);
    }
    while (!queue.empty()) {
        const auto [length,vertex] = queue.top();
        queue.pop();
        if (length != distance[vertex])
            continue;
        const auto &p = mesh.vertices[vertex];
        const auto t = 1-length/radius;
        weights[vertex] = t*t*(3-2*t);
        for (const auto &edge : topology.vertexEdges[vertex]) {
            const auto neighbor = edge.first == vertex ? edge.second : edge.first;
            const auto &q = mesh.vertices[neighbor];
            const auto next = length + std::hypot(p[0]-q[0],p[1]-q[1],p[2]-q[2]);
            if (next < radius && next < distance[neighbor]) {
                distance[neighbor] = next;
                queue.emplace(next,neighbor);
            }
        }
    }
    return weights;
}

std::vector<VertexIndex> mirroredVerticesX(const Mesh &mesh,float tolerance)
{
    std::vector<VertexIndex> partners(mesh.vertices.size(),MissingVertex);
    if (!(tolerance > 0) || !std::isfinite(tolerance))
        return partners;
    using Bin = std::array<double,3>;
    const auto key = [tolerance](Point3 p) -> Bin {
        return {std::floor(double(p[0])/tolerance),std::floor(double(p[1])/tolerance),
                std::floor(double(p[2])/tolerance)};
    };
    std::map<Bin,std::vector<VertexIndex>> negative;
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const auto p = mesh.vertices[i];
        if (std::abs(p[0]) <= tolerance)
            partners[i] = VertexIndex(i);
        else if (p[0] < 0)
            negative[key(p)].push_back(VertexIndex(i));
    }
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        auto p = mesh.vertices[i];
        if (p[0] <= tolerance)
            continue;
        p[0] = -p[0];
        const auto bin = key(p);
        float best = tolerance;
        for (int x = -1; x <= 1; ++x)
            for (int y = -1; y <= 1; ++y)
                for (int z = -1; z <= 1; ++z) {
                    const auto entry = negative.find({bin[0]+x,bin[1]+y,bin[2]+z});
                    if (entry == negative.end())
                        continue;
                    for (const auto candidate : entry->second) {
                        if (partners[candidate] != MissingVertex)
                            continue;
                        const auto &q = mesh.vertices[candidate];
                        const auto distance = std::hypot(p[0]-q[0],p[1]-q[1],p[2]-q[2]);
                        if (distance <= best) {
                            best = distance;
                            partners[i] = candidate;
                        }
                    }
                }
        if (partners[i] != MissingVertex)
            partners[partners[i]] = VertexIndex(i);
    }
    return partners;
}

}
