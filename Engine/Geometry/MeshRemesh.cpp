#include "Engine/Geometry/MeshRemesh.h"

#include <manifold/manifold.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace worldforge {
namespace {

using manifold::Box;
using manifold::vec3;
namespace la = manifold::la;

vec3 vector(Point3 p) { return {p[0],p[1],p[2]}; }
Point3 point(vec3 p) { return {float(p.x),float(p.y),float(p.z)}; }

struct Triangle {
    std::array<vec3,3> positions;
    std::array<MeshCorner,3> corners;
    vec3 normal;
    std::uint32_t material;

    vec3 closestWeights(vec3 p) const
    {
        const auto a = positions[0], b = positions[1], c = positions[2];
        const auto ab = b-a, ac = c-a;
        const double d1 = la::dot(ab,p-a), d2 = la::dot(ac,p-a);
        if (d1 <= 0 && d2 <= 0)
            return {1,0,0};
        const double d3 = la::dot(ab,p-b), d4 = la::dot(ac,p-b);
        if (d3 >= 0 && d4 <= d3)
            return {0,1,0};
        const double vc = d1*d4-d3*d2;
        if (vc <= 0 && d1 >= 0 && d3 <= 0) {
            const auto t = d1/(d1-d3);
            return {1-t,t,0};
        }
        const double d5 = la::dot(ab,p-c), d6 = la::dot(ac,p-c);
        if (d6 >= 0 && d5 <= d6)
            return {0,0,1};
        const double vb = d5*d2-d1*d6;
        if (vb <= 0 && d2 >= 0 && d6 <= 0) {
            const auto t = d2/(d2-d6);
            return {1-t,0,t};
        }
        const double va = d3*d6-d5*d4;
        if (va <= 0 && d4 >= d3 && d5 >= d6) {
            const auto t = (d4-d3)/(d4-d3+d5-d6);
            return {0,1-t,t};
        }
        const double inverse = 1/(va+vb+vc);
        return {1-(vb+vc)*inverse,vb*inverse,vc*inverse};
    }
};

class Surface {
public:
    struct Hit {
        const Triangle *triangle = nullptr;
        vec3 position,weights;
        double distanceSquared = std::numeric_limits<double>::infinity();
    };

    std::string build(const Mesh &mesh,std::stop_token stop)
    {
        struct Use { unsigned forward = 0, backward = 0; };
        std::map<MeshEdge,Use> uses;
        vertexNormals_.resize(mesh.vertices.size());
        double volume = 0;
        const auto origin = vector(mesh.vertices.front());
        for (const auto &face : mesh.faces) {
            if (stop.stop_requested())
                return "Remesh cancelled.";
            for (std::size_t i = 0; i < face.corners.size(); ++i) {
                const auto from = face.corners[i].vertex, to = face.corners[(i+1)%face.corners.size()].vertex;
                auto &use = uses[std::minmax(from,to)];
                ++(from < to ? use.forward : use.backward);
            }
            const auto triangles = faceTriangles(mesh,face);
            if (triangles.empty())
                return "Remesh could not triangulate the input.";
            for (const auto &indices : triangles) {
                Triangle triangle{{},{},{},face.material};
                for (std::size_t i = 0; i < 3; ++i) {
                    triangle.corners[i] = face.corners[indices[i]];
                    triangle.positions[i] = vector(mesh.vertices[triangle.corners[i].vertex]);
                }
                const auto &p = triangle.positions;
                triangle.normal = la::normalize(la::cross(p[1]-p[0],p[2]-p[0]));
                volume += la::dot(p[0]-origin,la::cross(p[1]-origin,p[2]-origin));
                for (std::size_t i = 0; i < 3; ++i) {
                    const auto j = (i+1)%3, k = (i+2)%3;
                    const auto vertex = triangle.corners[i].vertex;
                    const auto a = la::normalize(p[j]-p[i]), b = la::normalize(p[k]-p[i]);
                    vertexNormals_[vertex] += triangle.normal*std::atan2(la::length(la::cross(a,b)),la::dot(a,b));
                    edgeNormals_[std::minmax(vertex,triangle.corners[j].vertex)] += triangle.normal;
                }
                triangles_.push_back(triangle);
            }
        }
        if (!(volume > 0) || std::ranges::any_of(uses,[](const auto &edge) {
                return edge.second.forward != 1 || edge.second.backward != 1;
            }))
            return "Voxel Remesh requires a closed mesh with consistent outward-facing polygons.";
        buildNode(0,triangles_.size());
        return {};
    }

    const Box &bounds() const { return nodes_.front().bounds; }

    Hit closest(vec3 p) const
    {
        Hit hit;
        search(0,p,hit);
        return hit;
    }

    double signedDistance(vec3 p) const
    {
        const auto hit = closest(p);
        const auto &triangle = *hit.triangle;
        vec3 normal = triangle.normal;
        for (int i = 0; i < 3; ++i) {
            if (hit.weights[i] >= 1-1e-8) {
                normal = vertexNormals_[triangle.corners[i].vertex];
                break;
            }
            if (hit.weights[i] <= 1e-8) {
                const auto j = (i+1)%3, k = (i+2)%3;
                normal = edgeNormals_.at(std::minmax(triangle.corners[j].vertex,triangle.corners[k].vertex));
            }
        }
        const auto distance = std::sqrt(hit.distanceSquared);
        return la::dot(p-hit.position,normal) > 0 ? -distance : distance;
    }

private:
    struct Node { Box bounds; std::size_t first = 0,end = 0,left = 0,right = 0; };

    std::size_t buildNode(std::size_t begin,std::size_t end)
    {
        const auto index = nodes_.size();
        Node node;
        node.first = begin;
        node.end = end;
        for (auto i = begin; i < end; ++i)
            for (const auto p : triangles_[i].positions) {
                node.bounds.min = la::min(node.bounds.min,p);
                node.bounds.max = la::max(node.bounds.max,p);
            }
        nodes_.push_back(node);
        if (end-begin <= 8)
            return index;
        const auto size = node.bounds.Size();
        const int axis = size.x >= size.y && size.x >= size.z ? 0 : size.y >= size.z ? 1 : 2;
        const auto middle = begin+(end-begin)/2;
        std::nth_element(triangles_.begin()+std::ptrdiff_t(begin),triangles_.begin()+std::ptrdiff_t(middle),
            triangles_.begin()+std::ptrdiff_t(end),[axis](const auto &a,const auto &b) {
                return (a.positions[0]+a.positions[1]+a.positions[2])[axis]
                    < (b.positions[0]+b.positions[1]+b.positions[2])[axis];
            });
        nodes_[index].left = buildNode(begin,middle);
        nodes_[index].right = buildNode(middle,end);
        return index;
    }

    static double distanceToBox(vec3 p,const Box &bounds)
    {
        const auto delta = p-la::max(bounds.min,la::min(p,bounds.max));
        return la::dot(delta,delta);
    }

    void search(std::size_t index,vec3 p,Hit &hit) const
    {
        const auto &node = nodes_[index];
        if (distanceToBox(p,node.bounds) > hit.distanceSquared)
            return;
        if (!node.left) {
            for (auto i = node.first; i < node.end; ++i) {
                const auto &triangle = triangles_[i];
                const auto weights = triangle.closestWeights(p);
                const auto q = triangle.positions[0]*weights.x+triangle.positions[1]*weights.y+triangle.positions[2]*weights.z;
                const auto distance = la::dot(p-q,p-q);
                if (distance < hit.distanceSquared)
                    hit = {&triangle,q,weights,distance};
            }
            return;
        }
        auto first = node.left, second = node.right;
        if (distanceToBox(p,nodes_[second].bounds) < distanceToBox(p,nodes_[first].bounds))
            std::swap(first,second);
        search(first,p,hit);
        search(second,p,hit);
    }

    std::vector<Triangle> triangles_;
    std::vector<Node> nodes_;
    std::vector<vec3> vertexNormals_;
    std::map<MeshEdge,vec3> edgeNormals_;
};

}

std::string remeshVoxels(Mesh &mesh,float cellSize,std::stop_token stop)
{
    if (!std::isfinite(cellSize) || cellSize <= 0)
        return "Enter a positive voxel cell size.";
    if (const auto error = validateMesh(mesh); !error.empty())
        return error;
    Surface surface;
    if (const auto error = surface.build(mesh,stop); !error.empty())
        return error;
    auto bounds = surface.bounds();
    const vec3 padding(2*double(cellSize));
    bounds.min -= padding;
    bounds.max += padding;
    const auto cells = bounds.Size()/double(cellSize)+vec3(1);
    if (cells.x*cells.y*cells.z > 8'000'000)
        return "Increase the voxel cell size to fit the eight-million-cell preview limit.";
    const auto rebuilt = manifold::Manifold::LevelSet([&](vec3 p) {
        return stop.stop_requested() ? -double(cellSize) : surface.signedDistance(p);
    },bounds,cellSize,0,0.015*cellSize,false);
    if (stop.stop_requested())
        return "Remesh cancelled.";
    if (rebuilt.Status() != manifold::Manifold::Error::NoError || rebuilt.IsEmpty())
        return "Voxel Remesh found no valid surface. Try a smaller cell size.";
    const auto geometry = rebuilt.GetMeshGL64();
    if (geometry.vertProperties.size()/geometry.numProp > 2'000'000 || geometry.triVerts.size()/3 > 2'000'000)
        return "Increase the voxel cell size to fit the editor's geometry limit.";
    Mesh result;
    result.materials = mesh.materials;
    result.vertices.reserve(geometry.vertProperties.size()/geometry.numProp);
    for (std::size_t i = 0; i < geometry.vertProperties.size(); i += geometry.numProp)
        result.vertices.push_back({float(geometry.vertProperties[i]),float(geometry.vertProperties[i+1]),
            float(geometry.vertProperties[i+2])});
    result.faces.reserve(geometry.triVerts.size()/3);
    for (std::size_t i = 0; i < geometry.triVerts.size(); i += 3) {
        if (stop.stop_requested())
            return "Remesh cancelled.";
        const std::array indices{VertexIndex(geometry.triVerts[i]),VertexIndex(geometry.triVerts[i+1]),
            VertexIndex(geometry.triVerts[i+2])};
        const auto center = (vector(result.vertices[indices[0]])+vector(result.vertices[indices[1]])
            +vector(result.vertices[indices[2]]))/3.0;
        const auto &source = *surface.closest(center).triangle;
        MeshFace face{{},source.material};
        for (const auto vertex : indices) {
            const auto weights = source.closestWeights(vector(result.vertices[vertex]));
            MeshCorner corner{vertex};
            vec3 normal{};
            for (int j = 0; j < 3; ++j) {
                corner.uv[0] += float(weights[j]*source.corners[j].uv[0]);
                corner.uv[1] += float(weights[j]*source.corners[j].uv[1]);
                normal += weights[j]*vector(source.corners[j].normal);
            }
            if (la::length2(normal) > 0)
                corner.normal = point(la::normalize(normal));
            face.corners.push_back(corner);
        }
        result.faces.push_back(std::move(face));
    }
    const auto error = validateMesh(result);
    if (error.empty())
        mesh = std::move(result);
    return error;
}

}
