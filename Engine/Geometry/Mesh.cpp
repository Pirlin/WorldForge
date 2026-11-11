#include "Engine/Geometry/Mesh.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace worldforge {
namespace {
constexpr float epsilon = 1e-6f;
using Edge = std::pair<VertexIndex, VertexIndex>;

Point3 add(Point3 a, Point3 b) { return {a[0]+b[0], a[1]+b[1], a[2]+b[2]}; }
Point3 sub(Point3 a, Point3 b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
Point3 mul(Point3 a, float s) { return {a[0]*s, a[1]*s, a[2]*s}; }
float dot(Point3 a, Point3 b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
Point3 cross(Point3 a, Point3 b)
{
    return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
}
Point3 unit(Point3 v)
{
    const auto length = std::sqrt(dot(v, v));
    return std::isfinite(length) && length > epsilon ? mul(v, 1.0f/length) : Point3{};
}

Edge edgeAt(const MeshFace &face, std::size_t index)
{
    const auto first = face.corners[index].vertex;
    const auto second = face.corners[(index + 1) % face.corners.size()].vertex;
    return std::minmax(first, second);
}

template<std::size_t N>
bool finite(const std::array<float, N> &point)
{
    return std::ranges::all_of(point, [](float value) { return std::isfinite(value); });
}

std::string commit(Mesh &mesh, Mesh candidate)
{
    auto error = validateMesh(candidate);
    if (error.empty())
        mesh = std::move(candidate);
    return error;
}

void addRing(Mesh &mesh, std::size_t faceIndex, const std::vector<Point3> &positions)
{
    const auto old = mesh.faces[faceIndex];
    auto &cap = mesh.faces[faceIndex];
    for (std::size_t i = 0; i < old.corners.size(); ++i) {
        cap.corners[i].vertex = VertexIndex(mesh.vertices.size());
        cap.corners[i].normal = {};
        mesh.vertices.push_back(positions[i]);
    }
    const auto newCorners = cap.corners;
    for (std::size_t i = 0; i < old.corners.size(); ++i) {
        const auto j = (i + 1) % old.corners.size();
        mesh.faces.push_back({{{old.corners[i].vertex, {0, 0}}, {old.corners[j].vertex, {1, 0}},
                              {newCorners[j].vertex, {1, 1}}, {newCorners[i].vertex, {0, 1}}}, old.material});
    }
}
}

Mesh makeCube()
{
    Mesh cube;
    cube.vertices = {{-0.5f,-0.5f,-0.5f}, {0.5f,-0.5f,-0.5f}, {0.5f,0.5f,-0.5f}, {-0.5f,0.5f,-0.5f},
                     {-0.5f,-0.5f,0.5f}, {0.5f,-0.5f,0.5f}, {0.5f,0.5f,0.5f}, {-0.5f,0.5f,0.5f}};
    const std::array<std::array<VertexIndex, 4>, 6> faces{{{3,2,1,0}, {4,5,6,7}, {0,1,5,4},
                                                           {1,2,6,5}, {2,3,7,6}, {3,0,4,7}}};
    const std::array<Point2, 4> uv{{{0,0}, {1,0}, {1,1}, {0,1}}};
    for (const auto &indices : faces) {
        MeshFace face;
        for (std::size_t i = 0; i < indices.size(); ++i)
            face.corners.push_back({indices[i], uv[i]});
        cube.faces.push_back(std::move(face));
    }
    return cube;
}

Point3 faceNormal(const Mesh &mesh, const MeshFace &face)
{
    Point3 normal{};
    const auto origin = mesh.vertices[face.corners.front().vertex];
    for (std::size_t i = 1; i + 1 < face.corners.size(); ++i)
        normal = add(normal, cross(sub(mesh.vertices[face.corners[i].vertex], origin),
                                   sub(mesh.vertices[face.corners[i+1].vertex], origin)));
    return unit(normal);
}

std::string validateMesh(const Mesh &mesh)
{
    if (mesh.vertices.empty() || mesh.faces.empty() || mesh.materials.empty())
        return "Mesh requires vertices, faces and a material.";
    if (mesh.vertices.size() > 2'000'000 || mesh.faces.size() > 2'000'000)
        return "Mesh exceeds the editor's geometry limit.";
    for (const auto &material : mesh.materials)
        if (!finite(material.color) || !std::isfinite(material.roughness) || !std::isfinite(material.metallic))
            return "Material contains non-finite values.";
    for (const auto &vertex : mesh.vertices)
        if (!finite(vertex))
            return "Vertex contains non-finite coordinates.";
    for (const auto &face : mesh.faces) {
        if (face.corners.size() < 3 || face.corners.size() > 256 || face.material >= mesh.materials.size())
            return "Face has an invalid size or material.";
        std::set<VertexIndex> indices;
        for (const auto &corner : face.corners)
            if (corner.vertex >= mesh.vertices.size() || !indices.insert(corner.vertex).second
                || !finite(corner.uv) || !finite(corner.normal))
                return "Face contains invalid corners.";
        if (faceNormal(mesh, face) == Point3{})
            return "Face has zero area.";
    }
    return {};
}

std::string extrudeFace(Mesh &mesh, std::size_t index, float distance)
{
    if (index >= mesh.faces.size() || !std::isfinite(distance) || std::abs(distance) < epsilon)
        return "Select one face and enter a non-zero extrusion distance.";
    const auto &face = mesh.faces[index];
    const auto offset = mul(faceNormal(mesh, face), distance);
    std::vector<Point3> positions;
    for (const auto &corner : face.corners)
        positions.push_back(add(mesh.vertices[corner.vertex], offset));
    auto candidate = mesh;
    addRing(candidate, index, positions);
    return commit(mesh, std::move(candidate));
}

std::string bevelFace(Mesh &mesh, std::size_t index, float width)
{
    if (index >= mesh.faces.size() || !std::isfinite(width) || width < epsilon)
        return "Select a face and enter a positive bevel width.";
    const auto &face = mesh.faces[index];
    const auto normal = faceNormal(mesh, face);
    const auto count = face.corners.size();
    std::vector<Point3> positions;
    std::map<Edge, unsigned> neighbors;
    for (std::size_t i = 0; i < count; ++i)
        neighbors[edgeAt(face, i)] = 0;
    for (std::size_t i = 0; i < mesh.faces.size(); ++i) {
        if (i == index)
            continue;
        const auto &neighbor = mesh.faces[i];
        for (std::size_t edge = 0; edge < neighbor.corners.size(); ++edge) {
            if (auto found = neighbors.find(edgeAt(neighbor, edge)); found != neighbors.end()) {
                if (std::abs(dot(normal, faceNormal(mesh, neighbor))) > 1e-4f)
                    return "Perimeter bevel needs perpendicular adjacent faces.";
                ++found->second;
            }
        }
    }
    if (std::ranges::any_of(neighbors, [](const auto &entry) { return entry.second != 1; }))
        return "Perimeter bevel needs one neighboring face per edge.";
    for (std::size_t i = 0; i < count; ++i) {
        const auto point = mesh.vertices[face.corners[i].vertex];
        const auto previous = mesh.vertices[face.corners[(i + count - 1) % count].vertex];
        const auto next = mesh.vertices[face.corners[(i + 1) % count].vertex];
        const auto incoming = unit(sub(point, previous));
        const auto outgoing = unit(sub(next, point));
        if (dot(cross(incoming, outgoing), normal) <= epsilon)
            return "Perimeter bevel needs a convex face.";
        const auto a = cross(normal, incoming);
        const auto b = cross(normal, outgoing);
        positions.push_back(add(point, mul(add(a, b), width / (1 + dot(a, b)))));
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto j = (i + 1) % count;
        if (dot(sub(positions[j], positions[i]),
                sub(mesh.vertices[face.corners[j].vertex], mesh.vertices[face.corners[i].vertex])) <= epsilon)
            return "Bevel width exceeds the face's available area.";
    }
    auto candidate = mesh;
    for (const auto &corner : face.corners)
        candidate.vertices[corner.vertex] = sub(candidate.vertices[corner.vertex], mul(normal, width));
    for (auto &other : candidate.faces)
        for (auto &corner : other.corners)
            corner.normal = {};
    addRing(candidate, index, positions);
    return commit(mesh, std::move(candidate));
}

std::string cutQuadLoop(Mesh &mesh, std::size_t index, std::size_t startEdge)
{
    if (index >= mesh.faces.size() || mesh.faces[index].corners.size() != 4 || startEdge >= 4)
        return "Loop Cut needs a selected quad and one of its edges.";
    std::map<Edge, std::vector<std::pair<std::size_t, std::size_t>>> adjacency;
    for (std::size_t i = 0; i < mesh.faces.size(); ++i)
        for (std::size_t edge = 0; edge < mesh.faces[i].corners.size(); ++edge)
            adjacency[edgeAt(mesh.faces[i], edge)].emplace_back(i, edge);

    std::map<std::size_t, std::size_t> cuts;
    std::vector<std::pair<std::size_t, std::size_t>> pending{{index, startEdge}};
    while (!pending.empty()) {
        const auto [faceIndex, edge] = pending.back();
        pending.pop_back();
        const auto &face = mesh.faces[faceIndex];
        if (face.corners.size() != 4)
            return "The loop encounters a face that is not a quad.";
        const auto [entry, inserted] = cuts.emplace(faceIndex, edge % 2);
        if (!inserted) {
            if (entry->second != edge % 2)
                return "The selected quad loop intersects itself.";
            continue;
        }
        for (auto side : {edge, (edge + 2) % 4}) {
            const auto &neighbors = adjacency.at(edgeAt(face, side));
            if (neighbors.size() > 2)
                return "The loop encounters a non-manifold edge.";
            for (const auto &next : neighbors)
                if (next.first != faceIndex)
                    pending.push_back(next);
        }
    }

    auto candidate = mesh;
    std::map<Edge, VertexIndex> midpoints;
    for (const auto &[faceIndex, edge] : cuts) {
        const auto &face = mesh.faces[faceIndex];
        const auto mid = [&](std::size_t i) {
            const auto key = edgeAt(face, i);
            const auto [point, inserted] = midpoints.emplace(key, VertexIndex(candidate.vertices.size()));
            if (inserted)
                candidate.vertices.push_back(mul(add(mesh.vertices[key.first], mesh.vertices[key.second]), 0.5f));
            const auto &a = face.corners[i];
            const auto &b = face.corners[(i+1)%4];
            return MeshCorner{point->second, {(a.uv[0]+b.uv[0])*0.5f, (a.uv[1]+b.uv[1])*0.5f},
                              unit(add(a.normal, b.normal))};
        };
        const auto left = mid(edge);
        const auto right = mid(edge + 2);
        const auto &a = face.corners[edge];
        const auto &b = face.corners[(edge+1)%4];
        const auto &c = face.corners[(edge+2)%4];
        const auto &d = face.corners[(edge+3)%4];
        candidate.faces[faceIndex] = {{a, left, right, d}, face.material};
        candidate.faces.push_back({{left, b, c, right}, face.material});
    }
    return commit(mesh, std::move(candidate));
}

std::string mirrorMeshX(Mesh &mesh)
{
    const bool negative = std::ranges::any_of(mesh.vertices, [](Point3 p) { return p[0] < -epsilon; });
    const bool positive = std::ranges::any_of(mesh.vertices, [](Point3 p) { return p[0] > epsilon; });
    if (negative == positive)
        return "Mirror X needs geometry on one side of X = 0.";
    auto candidate = mesh;
    std::vector<VertexIndex> reflected;
    for (VertexIndex i = 0; i < mesh.vertices.size(); ++i) {
        auto p = mesh.vertices[i];
        if (std::abs(p[0]) <= epsilon) {
            reflected.push_back(i);
            candidate.vertices[i][0] = 0;
        } else {
            reflected.push_back(VertexIndex(candidate.vertices.size()));
            p[0] = -p[0];
            candidate.vertices.push_back(p);
        }
    }
    for (const auto &face : mesh.faces) {
        if (std::ranges::all_of(face.corners, [&](const auto &corner) { return reflected[corner.vertex] == corner.vertex; }))
            continue;
        auto copy = face;
        for (auto &corner : copy.corners) {
            corner.vertex = reflected[corner.vertex];
            corner.normal[0] = -corner.normal[0];
        }
        std::ranges::reverse(copy.corners);
        candidate.faces.push_back(std::move(copy));
    }
    return commit(mesh, std::move(candidate));
}

std::string unwrapFace(Mesh &mesh, std::size_t index)
{
    if (index >= mesh.faces.size())
        return "Select one face to unwrap.";
    auto &face = mesh.faces[index];
    const auto a = mesh.vertices[face.corners[0].vertex];
    const auto u = unit(sub(mesh.vertices[face.corners[1].vertex], a));
    const auto v = cross(faceNormal(mesh, face), u);
    Point2 low{}, high{};
    for (auto &corner : face.corners) {
        const auto relative = sub(mesh.vertices[corner.vertex], a);
        corner.uv = {dot(relative, u), dot(relative, v)};
        for (std::size_t i = 0; i < 2; ++i) {
            low[i] = std::min(low[i], corner.uv[i]);
            high[i] = std::max(high[i], corner.uv[i]);
        }
    }
    const auto range = std::max({high[0]-low[0], high[1]-low[1], epsilon});
    for (auto &corner : face.corners)
        corner.uv = {(corner.uv[0]-low[0])/range, (corner.uv[1]-low[1])/range};
    return {};
}

std::string translateFaceUV(Mesh &mesh, std::size_t index, Point2 delta)
{
    if (index >= mesh.faces.size() || !finite(delta))
        return "Select a face and enter a valid UV offset.";
    auto candidate = mesh;
    for (auto &corner : candidate.faces[index].corners) {
        corner.uv[0] += delta[0];
        corner.uv[1] += delta[1];
    }
    return commit(mesh, std::move(candidate));
}
}
