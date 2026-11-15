#include "Engine/Geometry/Mesh.h"
#include "Engine/Geometry/MeshTopology.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <numeric>
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
    for (std::size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
        const auto &indices = faces[faceIndex];
        MeshFace face;
        for (std::size_t i = 0; i < indices.size(); ++i)
            face.corners.push_back({indices[i], {(uv[i][0]*0.9f + float(faceIndex%3))/3,
                                                 (uv[i][1]*0.9f + float(faceIndex/3))/2}});
        cube.faces.push_back(std::move(face));
    }
    return cube;
}

std::optional<Mesh> makePrimitive(Primitive primitive, Point3 size, unsigned segments, unsigned rings)
{
    if (!finite(size) || std::ranges::any_of(size,[](float value) { return value <= 0; })
        || segments < 3 || segments > 128 || rings < 3 || rings > 128)
        return {};
    Mesh mesh;
    constexpr float tau = 2*std::numbers::pi_v<float>;
    const auto ring = [&](float radius,float height) {
        const auto first = VertexIndex(mesh.vertices.size());
        for (unsigned i = 0; i < segments; ++i) {
            const auto angle = tau*float(i)/float(segments);
            mesh.vertices.push_back({radius*std::cos(angle),radius*std::sin(angle),height});
        }
        return first;
    };
    const auto cap = [&](VertexIndex first,bool up,float radius) {
        MeshFace face;
        for (unsigned i = 0; i < segments; ++i) {
            const auto index = first+(up ? i : segments-1-i);
            const auto p = mesh.vertices[index];
            face.corners.push_back({index,{0.5f+p[0]/(2*radius),0.5f+p[1]/(2*radius)}});
        }
        mesh.faces.push_back(std::move(face));
    };
    const auto connect = [&](VertexIndex lower,VertexIndex upper,float v0,float v1) {
        for (unsigned i = 0; i < segments; ++i) {
            const auto next = (i+1)%segments;
            const auto u0 = float(i)/float(segments), u1 = float(i+1)/float(segments);
            mesh.faces.push_back({{{lower+i,{u0,v0}},{lower+next,{u1,v0}},
                                   {upper+next,{u1,v1}},{upper+i,{u0,v1}}}});
        }
    };
    const auto pole = [&](VertexIndex rim,float z,bool up,float v) {
        const auto tip = VertexIndex(mesh.vertices.size());
        mesh.vertices.push_back({0,0,z});
        for (unsigned i = 0; i < segments; ++i) {
            const auto next = (i+1)%segments;
            const auto u0 = float(i)/float(segments), u1 = float(i+1)/float(segments);
            MeshFace face{{{rim+i,{u0,v}},{rim+next,{u1,v}},{tip,{(u0+u1)*0.5f,up ? 1.0f : 0.0f}}}};
            if (!up)
                std::ranges::reverse(face.corners);
            mesh.faces.push_back(std::move(face));
        }
    };
    switch (primitive) {
    case Primitive::Cube:
        mesh = makeCube();
        break;
    case Primitive::Plane:
        mesh.vertices = {{-0.5f,-0.5f,0},{0.5f,-0.5f,0},{0.5f,0.5f,0},{-0.5f,0.5f,0}};
        mesh.faces.push_back({{{0,{0,0}},{1,{1,0}},{2,{1,1}},{3,{0,1}}}});
        break;
    case Primitive::Circle:
        cap(ring(0.5f,0),true,0.5f);
        break;
    case Primitive::Cylinder: {
        const auto lower = ring(0.5f,-0.5f), upper = ring(0.5f,0.5f);
        connect(lower,upper,0,1);
        cap(lower,false,0.5f);
        cap(upper,true,0.5f);
        break;
    }
    case Primitive::Cone: {
        const auto base = ring(0.5f,-0.5f);
        cap(base,false,0.5f);
        pole(base,0.5f,true,0);
        break;
    }
    case Primitive::Sphere: {
        VertexIndex previous = 0;
        for (unsigned j = 1; j < rings; ++j) {
            const auto angle = std::numbers::pi_v<float>*(float(j)/float(rings)-0.5f);
            const auto next = ring(0.5f*std::cos(angle),0.5f*std::sin(angle));
            if (j == 1)
                pole(next,-0.5f,false,1.0f/float(rings));
            else
                connect(previous,next,float(j-1)/float(rings),float(j)/float(rings));
            previous = next;
        }
        pole(previous,0.5f,true,float(rings-1)/float(rings));
        break;
    }
    case Primitive::Torus: {
        for (unsigned j = 0; j < rings; ++j) {
            const auto angle = tau*float(j)/float(rings);
            ring(0.36f+0.14f*std::cos(angle),0.14f*std::sin(angle));
        }
        for (unsigned j = 0; j < rings; ++j)
            connect(j*segments,((j+1)%rings)*segments,float(j)/float(rings),float(j+1)/float(rings));
        break;
    }
    default:
        return {};
    }
    for (unsigned axis = 0; axis < 3; ++axis) {
        const auto [low,high] = std::ranges::minmax_element(mesh.vertices,{},[axis](const auto &p) { return p[axis]; });
        const float extent = (*high)[axis]-(*low)[axis];
        if (extent == 0)
            continue;
        const float center = ((*high)[axis]+(*low)[axis])*0.5f;
        for (auto &p : mesh.vertices)
            p[axis] = (p[axis]-center)*(size[axis]/extent);
    }
    return validateMesh(mesh).empty() ? std::optional{std::move(mesh)} : std::nullopt;
}

Point3 faceNormal(const Mesh &mesh, const MeshFace &face)
{
    if (face.corners.size() < 3 || std::ranges::any_of(face.corners, [&](const auto &corner) {
        return corner.vertex >= mesh.vertices.size();
    }))
        return {};
    Point3 normal{};
    const auto origin = mesh.vertices[face.corners.front().vertex];
    for (std::size_t i = 1; i + 1 < face.corners.size(); ++i)
        normal = add(normal, cross(sub(mesh.vertices[face.corners[i].vertex], origin),
                                   sub(mesh.vertices[face.corners[i+1].vertex], origin)));
    return unit(normal);
}

std::vector<std::array<std::size_t,3>> faceTriangles(const Mesh &mesh,const MeshFace &face)
{
    const auto normal = faceNormal(mesh,face);
    if (normal == Point3{})
        return {};
    int drop = std::abs(normal[0]) >= std::abs(normal[1]) ? 0 : 1;
    if (std::abs(normal[2]) > std::abs(normal[drop]))
        drop = 2;
    const int u = (drop+1)%3, v = (drop+2)%3;
    std::vector<std::array<double,2>> points;
    std::vector<std::size_t> polygon;
    for (std::size_t i = 0; i < face.corners.size(); ++i) {
        const auto p = mesh.vertices[face.corners[i].vertex];
        points.push_back({p[u],p[v]});
        polygon.push_back(i);
    }
    const double sign = normal[drop] > 0 ? 1 : -1;
    const auto turn = [&](std::size_t a,std::size_t b,std::size_t c) {
        const auto &p = points[a], &q = points[b], &r = points[c];
        return sign*((q[0]-p[0])*(r[1]-p[1])-(q[1]-p[1])*(r[0]-p[0]));
    };
    constexpr double threshold = 1e-12;
    std::vector<std::array<std::size_t,3>> result;
    result.reserve(polygon.size()-2);
    while (polygon.size() > 3) {
        const auto count = polygon.size();
        bool clipped = false;
        for (std::size_t i = 1; i <= count; ++i) {
            const auto a = polygon[(i+count-1)%count], b = polygon[i%count], c = polygon[(i+1)%count];
            if (turn(a,b,c) <= threshold)
                continue;
            if (std::ranges::any_of(polygon,[&](auto p) {
                return p != a && p != b && p != c && turn(a,b,p) >= -threshold
                    && turn(b,c,p) >= -threshold && turn(c,a,p) >= -threshold;
            }))
                continue;
            result.push_back({a,b,c});
            polygon.erase(polygon.begin()+std::ptrdiff_t(i%count));
            clipped = true;
            break;
        }
        if (!clipped)
            return {};
    }
    if (turn(polygon[0],polygon[1],polygon[2]) <= threshold)
        return {};
    result.push_back({polygon[0],polygon[1],polygon[2]});
    return result;
}

std::string triangulateFaces(Mesh &mesh,std::span<const std::size_t> faces)
{
    if (faces.empty())
        return "Select faces to triangulate.";
    auto candidate = mesh;
    const std::set<std::size_t> selected(faces.begin(),faces.end());
    for (const auto index : selected) {
        if (index >= mesh.faces.size())
            return "The face selection is no longer valid.";
        const auto &face = mesh.faces[index];
        const auto triangles = faceTriangles(mesh,face);
        if (triangles.empty())
            return "A selected face could not be triangulated.";
        for (std::size_t i = 0; i < triangles.size(); ++i) {
            const auto &t = triangles[i];
            MeshFace part{{face.corners[t[0]],face.corners[t[1]],face.corners[t[2]]},face.material};
            if (i == 0)
                candidate.faces[index] = std::move(part);
            else
                candidate.faces.push_back(std::move(part));
        }
    }
    return commit(mesh,std::move(candidate));
}

namespace {
void removeUnusedVertices(Mesh &mesh)
{
    std::vector<bool> used(mesh.vertices.size());
    for (const auto &face : mesh.faces)
        for (const auto &corner : face.corners)
            used[corner.vertex] = true;
    std::vector<VertexIndex> remap(mesh.vertices.size());
    std::vector<Point3> kept;
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i)
        if (used[i]) {
            remap[i] = VertexIndex(kept.size());
            kept.push_back(mesh.vertices[i]);
        }
    for (auto &face : mesh.faces)
        for (auto &corner : face.corners)
            corner.vertex = remap[corner.vertex];
    mesh.vertices = std::move(kept);
}

MeshCorner middleCorner(const MeshCorner &a,const MeshCorner &b,VertexIndex vertex)
{
    return {vertex,{(a.uv[0]+b.uv[0])*0.5f,(a.uv[1]+b.uv[1])*0.5f},unit(add(a.normal,b.normal))};
}

struct BoundaryLoop {
    std::vector<VertexIndex> vertices;
    std::uint32_t material = 0;
};

std::vector<BoundaryLoop> boundaryLoops(const Mesh &mesh,std::span<const MeshEdge> edges)
{
    const MeshTopology topology(mesh);
    std::map<VertexIndex,std::pair<VertexIndex,std::uint32_t>> next;
    std::set<VertexIndex> arrivals;
    for (const auto &selected : edges) {
        const MeshEdge key = std::minmax(selected.first,selected.second);
        const auto found = topology.edgeFaces.find(key);
        if (found == topology.edgeFaces.end() || found->second.size() != 1)
            return {};
        const auto faceIndex = found->second.front();
        const auto &face = mesh.faces[faceIndex];
        const auto &sides = topology.faceEdges[faceIndex];
        const auto i = std::size_t(std::ranges::find(sides,key)-sides.begin());
        const auto a = face.corners[i].vertex, b = face.corners[(i+1)%face.corners.size()].vertex;
        if (!next.emplace(b,std::pair{a,face.material}).second || !arrivals.insert(a).second)
            return {};
    }
    std::vector<BoundaryLoop> result;
    while (!next.empty()) {
        const auto start = next.begin()->first;
        auto vertex = start;
        BoundaryLoop loop{{},next.begin()->second.second};
        do {
            const auto edge = next.find(vertex);
            if (edge == next.end())
                return {};
            loop.vertices.push_back(vertex);
            vertex = edge->second.first;
            next.erase(edge);
        } while (vertex != start);
        if (loop.vertices.size() < 3)
            return {};
        result.push_back(std::move(loop));
    }
    return result;
}
}

std::string subdivideFaces(Mesh &mesh,std::span<const std::size_t> faces)
{
    if (faces.empty())
        return "Select faces to subdivide.";
    const std::set<std::size_t> selected(faces.begin(),faces.end());
    if (*selected.rbegin() >= mesh.faces.size())
        return "The face selection is no longer valid.";
    auto candidate = mesh;
    std::map<MeshEdge,VertexIndex> midpoints;
    for (const auto i : selected) {
        const auto &face = mesh.faces[i];
        const auto count = face.corners.size();
        for (std::size_t j = 0; j < count; ++j) {
            const auto edge = edgeAt(face,j);
            if (midpoints.try_emplace(edge,VertexIndex(candidate.vertices.size())).second)
                candidate.vertices.push_back(mul(add(mesh.vertices[edge.first],mesh.vertices[edge.second]),0.5f));
        }
    }
    for (std::size_t i = 0; i < mesh.faces.size(); ++i) {
        const auto &face = mesh.faces[i];
        const auto count = face.corners.size();
        if (!selected.contains(i)) {
            std::vector<MeshCorner> expanded;
            for (std::size_t j = 0; j < count; ++j) {
                expanded.push_back(face.corners[j]);
                if (const auto edge = midpoints.find(edgeAt(face,j)); edge != midpoints.end())
                    expanded.push_back(middleCorner(face.corners[j],face.corners[(j+1)%count],edge->second));
            }
            candidate.faces[i].corners = std::move(expanded);
            continue;
        }
        Point3 center{};
        MeshCorner middle{VertexIndex(candidate.vertices.size())};
        for (const auto &corner : face.corners) {
            center = add(center,mesh.vertices[corner.vertex]);
            middle.uv[0] += corner.uv[0]/float(count);
            middle.uv[1] += corner.uv[1]/float(count);
            middle.normal = add(middle.normal,corner.normal);
        }
        middle.normal = unit(middle.normal);
        candidate.vertices.push_back(mul(center,1.0f/float(count)));
        for (std::size_t j = 0; j < count; ++j) {
            const auto prev = (j+count-1)%count, next = (j+1)%count;
            MeshFace quad{{face.corners[j],middleCorner(face.corners[j],face.corners[next],midpoints.at(edgeAt(face,j))),
                middle,middleCorner(face.corners[prev],face.corners[j],midpoints.at(edgeAt(face,prev)))},face.material};
            if (j == 0)
                candidate.faces[i] = std::move(quad);
            else
                candidate.faces.push_back(std::move(quad));
        }
    }
    return commit(mesh,std::move(candidate));
}

std::string deleteFaces(Mesh &mesh,std::span<const std::size_t> faces)
{
    const std::set<std::size_t> selected(faces.begin(),faces.end());
    if (selected.empty() || *selected.rbegin() >= mesh.faces.size())
        return "Select valid mesh components to delete.";
    if (selected.size() == mesh.faces.size())
        return "Delete the object to remove its entire mesh.";
    auto candidate = mesh;
    for (auto i = selected.rbegin(); i != selected.rend(); ++i)
        candidate.faces.erase(candidate.faces.begin()+std::ptrdiff_t(*i));
    removeUnusedVertices(candidate);
    return commit(mesh,std::move(candidate));
}

std::string weldVertices(Mesh &mesh,std::span<const VertexIndex> vertices,float distance,
                         std::optional<VertexIndex> target,bool allowNoChange)
{
    const std::set<VertexIndex> selected(vertices.begin(),vertices.end());
    if (selected.size() < 2 || *selected.rbegin() >= mesh.vertices.size()
        || (target && !selected.contains(*target)) || (!target && (!(distance > 0) || !std::isfinite(distance))))
        return "Select two or more vertices and a valid weld target or distance.";
    std::vector<VertexIndex> parent(mesh.vertices.size());
    std::iota(parent.begin(),parent.end(),VertexIndex(0));
    const auto root = [&](VertexIndex i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    if (target) {
        for (const auto index : selected)
            parent[index] = *target;
    } else {
        using Bin = std::array<double,3>;
        std::map<Bin,std::vector<VertexIndex>> bins;
        for (const auto i : selected) {
            const auto p = mesh.vertices[i];
            const Bin key{std::floor(double(p[0])/distance),std::floor(double(p[1])/distance),std::floor(double(p[2])/distance)};
            for (int x = -1; x <= 1; ++x)
                for (int y = -1; y <= 1; ++y)
                    for (int z = -1; z <= 1; ++z) {
                        const auto entry = bins.find({key[0]+x,key[1]+y,key[2]+z});
                        if (entry == bins.end())
                            continue;
                        for (const auto j : entry->second) {
                            const auto delta = sub(p,mesh.vertices[j]);
                            if (std::hypot(delta[0],delta[1],delta[2]) <= distance) {
                                const auto a = root(i), b = root(j);
                                parent[std::max(a,b)] = std::min(a,b);
                            }
                        }
                    }
            bins[key].push_back(i);
        }
    }
    auto candidate = mesh;
    std::vector<bool> touched(mesh.vertices.size());
    std::map<VertexIndex,std::pair<std::array<double,3>,unsigned>> clusters;
    for (const auto i : selected) {
        const auto representative = root(i);
        auto &[sum,count] = clusters[representative];
        for (int axis = 0; axis < 3; ++axis)
            sum[axis] += mesh.vertices[i][axis];
        ++count;
    }
    if (clusters.size() == selected.size())
        return allowNoChange ? std::string{} : "No selected vertices are close enough to weld.";
    for (const auto &[index,cluster] : clusters) {
        const auto &[sum,count] = cluster;
        if (count > 1) {
            touched[index] = true;
            if (!target)
                for (int axis = 0; axis < 3; ++axis)
                    candidate.vertices[index][axis] = float(sum[axis]/count);
        }
    }
    for (auto &face : candidate.faces) {
        bool changed = false;
        for (auto &corner : face.corners) {
            const auto next = root(corner.vertex);
            changed |= next != corner.vertex || touched[next];
            corner.vertex = next;
        }
        if (!changed)
            continue;
        const auto end = std::unique(face.corners.begin(),face.corners.end(),[](const auto &a,const auto &b) {
            return a.vertex == b.vertex;
        });
        face.corners.erase(end,face.corners.end());
        if (face.corners.size() > 1 && face.corners.front().vertex == face.corners.back().vertex)
            face.corners.pop_back();
        for (auto &corner : face.corners)
            corner.normal = {};
    }
    std::erase_if(candidate.faces,[](const auto &face) { return face.corners.size() < 3; });
    removeUnusedVertices(candidate);
    return commit(mesh,std::move(candidate));
}

std::string weldBoundaryEdges(Mesh &mesh,MeshEdge source,MeshEdge target)
{
    const MeshTopology topology(mesh);
    const auto oriented = [&](MeshEdge edge) -> std::optional<MeshEdge> {
        edge = MeshEdge(std::minmax(edge.first,edge.second));
        const auto found = topology.edgeFaces.find(edge);
        if (found == topology.edgeFaces.end() || found->second.size() != 1)
            return {};
        const auto face = found->second.front();
        const auto &edges = topology.faceEdges[face];
        const auto offset = std::ranges::find(edges,edge)-edges.begin();
        if (mesh.faces[face].corners[offset].vertex != edge.first)
            std::swap(edge.first,edge.second);
        return edge;
    };
    const auto a = oriented(source), b = oriented(target);
    if (!a || !b || a->first == b->first || a->first == b->second
        || a->second == b->first || a->second == b->second)
        return "Choose two disjoint boundary edges.";
    auto candidate = mesh;
    for (auto &face : candidate.faces) {
        bool changed = false;
        for (auto &corner : face.corners) {
            if (corner.vertex == a->first) {
                corner.vertex = b->second;
                changed = true;
            } else if (corner.vertex == a->second) {
                corner.vertex = b->first;
                changed = true;
            }
        }
        if (changed)
            for (auto &corner : face.corners)
                corner.normal = {};
    }
    const auto merged = MeshEdge(std::minmax(b->first,b->second));
    const MeshTopology result(candidate);
    if (result.edgeFaces.at(merged).size() != 2)
        return "The weld would create an invalid shared edge.";
    for (const auto &[edge,faces] : result.edgeFaces)
        if (faces.size() > 2 && (edge.first == merged.first || edge.second == merged.first
            || edge.first == merged.second || edge.second == merged.second))
            return "The weld would create a non-manifold junction.";
    removeUnusedVertices(candidate);
    return commit(mesh,std::move(candidate));
}

std::string fillBoundary(Mesh &mesh,std::span<const MeshEdge> edges)
{
    const auto loops = boundaryLoops(mesh,edges);
    if (loops.size() != 1 || loops.front().vertices.size() > 256)
        return "Fill needs one closed boundary loop of 3 to 256 edges.";
    auto candidate = mesh;
    MeshFace face;
    face.material = loops.front().material;
    for (const auto vertex : loops.front().vertices)
        face.corners.push_back({vertex});
    if (faceTriangles(candidate,face).empty())
        return "This boundary cannot form one simple face.";
    candidate.faces.push_back(std::move(face));
    const auto error = unwrapFace(candidate,candidate.faces.size()-1);
    return error.empty() ? commit(mesh,std::move(candidate)) : error;
}

std::string gridFillBoundary(Mesh &mesh,std::span<const MeshEdge> edges,unsigned columns)
{
    const auto loops = boundaryLoops(mesh,edges);
    if (loops.size() != 1)
        return "Grid Fill needs one closed boundary.";
    const auto &border = loops.front().vertices;
    if (border.size()%2 || columns == 0 || columns >= border.size()/2)
        return "Choose a span that divides an even boundary into four sides.";
    const auto rows = border.size()/2-columns;
    if (rows > 128 || columns > 128)
        return "Grid Fill supports up to 128 rows and columns.";
    const auto bottom = [&](std::size_t column) { return border[column]; };
    const auto top = [&](std::size_t column) { return border[2*columns+rows-column]; };
    const auto right = [&](std::size_t row) { return border[columns+row]; };
    const auto left = [&](std::size_t row) { return border[(border.size()-row)%border.size()]; };
    auto candidate = mesh;
    std::vector<VertexIndex> grid((rows+1)*(columns+1));
    const auto at = [&](std::size_t row,std::size_t column) -> VertexIndex & { return grid[row*(columns+1)+column]; };
    const auto blend = [&](VertexIndex a,VertexIndex b,float t) {
        return add(mul(mesh.vertices[a],1-t),mul(mesh.vertices[b],t));
    };
    for (std::size_t row = 0; row <= rows; ++row) {
        const float v = float(row)/float(rows);
        for (std::size_t col = 0; col <= columns; ++col) {
            if (!row) at(row,col) = bottom(col);
            else if (row == rows) at(row,col) = top(col);
            else if (!col) at(row,col) = left(row);
            else if (col == columns) at(row,col) = right(row);
            else {
                const float u = float(col)/float(columns);
                const auto base = blend(bottom(col),top(col),v);
                const auto across = blend(left(row),right(row),u);
                const auto corners = add(mul(blend(bottom(0),bottom(columns),u),1-v),
                    mul(blend(top(0),top(columns),u),v));
                at(row,col) = VertexIndex(candidate.vertices.size());
                candidate.vertices.push_back(sub(add(base,across),corners));
            }
        }
    }
    const auto corner = [&](std::size_t row,std::size_t col) {
        return MeshCorner{at(row,col),{float(col)/float(columns),float(row)/float(rows)}};
    };
    for (std::size_t row = 0; row < rows; ++row)
        for (std::size_t col = 0; col < columns; ++col)
            candidate.faces.push_back({{corner(row,col),corner(row,col+1),corner(row+1,col+1),corner(row+1,col)},
                loops.front().material});
    return commit(mesh,std::move(candidate));
}

std::string bridgeBoundaries(Mesh &mesh,std::span<const MeshEdge> edges)
{
    auto loops = boundaryLoops(mesh,edges);
    if (loops.size() != 2 || loops[0].vertices.size() != loops[1].vertices.size())
        return "Bridge needs two disjoint closed boundary loops with equal edge counts.";
    const auto &a = loops[0].vertices;
    auto &b = loops[1].vertices;
    std::ranges::reverse(b);
    const auto start = mesh.vertices[a.front()];
    const auto nearest = std::ranges::min_element(b,{},[&](auto i) {
        const auto delta = sub(mesh.vertices[i],start);
        return dot(delta,delta);
    });
    std::rotate(b.begin(),nearest,b.end());
    auto candidate = mesh;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto next = (i+1)%a.size();
        const float u0 = float(i)/float(a.size()), u1 = float(i+1)/float(a.size());
        candidate.faces.push_back({{{a[i],{u0,0}},{a[next],{u1,0}},{b[next],{u1,1}},{b[i],{u0,1}}},loops[0].material});
    }
    return commit(mesh,std::move(candidate));
}

std::string dissolveEdges(Mesh &mesh,std::span<const MeshEdge> edges,std::optional<float> maxAngleDegrees)
{
    if (edges.empty() || (maxAngleDegrees && (!std::isfinite(*maxAngleDegrees) || *maxAngleDegrees < 0 || *maxAngleDegrees > 180)))
        return "Select interior edges to dissolve.";
    auto candidate = mesh;
    bool dissolved = false;
    for (const auto &edge : edges) {
        const MeshTopology topology(candidate);
        const auto key = MeshEdge(std::minmax(edge.first,edge.second));
        const auto found = topology.edgeFaces.find(key);
        if (found == topology.edgeFaces.end() || found->second.size() != 2)
            return "Dissolve requires an edge shared by exactly two faces.";
        const auto a = found->second[0], b = found->second[1];
        const auto &first = candidate.faces[a], &second = candidate.faces[b];
        if (maxAngleDegrees && dot(faceNormal(candidate,first),faceNormal(candidate,second)) + 0.000001f
            < std::cos(*maxAngleDegrees*std::numbers::pi_v<float>/180))
            continue;
        if (first.material != second.material) {
            if (maxAngleDegrees)
                continue;
            return "Dissolve cannot merge faces with different material slots.";
        }
        const auto side = [&](std::size_t face) {
            const auto &edges = topology.faceEdges[face];
            return std::size_t(std::ranges::find(edges,key)-edges.begin());
        };
        const auto i = side(a), j = side(b);
        if (first.corners[i].vertex != second.corners[(j+1)%second.corners.size()].vertex)
            return "Fix face winding before dissolving the edge.";
        const auto nearUv = [](Point2 a,Point2 b) { return std::hypot(a[0]-b[0],a[1]-b[1]) < 0.00001f; };
        if (!nearUv(first.corners[i].uv,second.corners[(j+1)%second.corners.size()].uv)
            || !nearUv(first.corners[(i+1)%first.corners.size()].uv,second.corners[j].uv)) {
            if (maxAngleDegrees)
                continue;
            return "The edge is a UV seam. Stitch its UVs before dissolving it.";
        }
        MeshFace merged;
        merged.material = first.material;
        for (std::size_t n = 1; n <= first.corners.size(); ++n)
            merged.corners.push_back(first.corners[(i+n)%first.corners.size()]);
        for (std::size_t n = 2; n < second.corners.size(); ++n)
            merged.corners.push_back(second.corners[(j+n)%second.corners.size()]);
        for (auto &corner : merged.corners)
            corner.normal = {};
        if (faceTriangles(candidate,merged).empty())
            return "Dissolve would create an invalid polygon.";
        candidate.faces[a] = std::move(merged);
        candidate.faces.erase(candidate.faces.begin()+std::ptrdiff_t(b));
        dissolved = true;
    }
    return dissolved ? commit(mesh,std::move(candidate)) : "No selected edges match the dissolve limits.";
}

std::string setFaceShading(Mesh &mesh,std::span<const std::size_t> faces,bool smooth)
{
    const std::set<std::size_t> selected(faces.begin(),faces.end());
    if (selected.empty() || *selected.rbegin() >= mesh.faces.size())
        return "Select faces to change their shading.";
    auto candidate = mesh;
    std::vector<Point3> normals(mesh.vertices.size());
    if (smooth)
        for (const auto i : selected) {
            const auto normal = faceNormal(mesh,mesh.faces[i]);
            for (const auto &corner : mesh.faces[i].corners)
                normals[corner.vertex] = add(normals[corner.vertex],normal);
        }
    for (const auto i : selected)
        for (auto &corner : candidate.faces[i].corners)
            corner.normal = smooth ? unit(normals[corner.vertex]) : Point3{};
    return commit(mesh,std::move(candidate));
}

std::string validateMesh(const Mesh &mesh)
{
    if (mesh.vertices.empty() || mesh.faces.empty() || mesh.materials.empty())
        return "Mesh requires vertices, faces and a material.";
    if (mesh.vertices.size() > 2'000'000 || mesh.faces.size() > 2'000'000)
        return "Mesh exceeds the editor's geometry limit.";
    std::size_t textureBytes = 0;
    for (const auto &material : mesh.materials) {
        if (!finite(material.color) || !finite(material.emissive) || !std::isfinite(material.roughness) || !std::isfinite(material.metallic))
            return "Material contains non-finite values.";
        for (const auto *image : material.previewTextures())
            textureBytes += image->size();
        if (textureBytes > MeshMaterial::maxTextureBytes)
            return "Mesh material images exceed the 16 MiB limit.";
    }
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

std::string extrudeFaces(Mesh &mesh,std::span<const std::size_t> faces,float distance,bool individual)
{
    if (faces.empty() || !std::isfinite(distance) || std::abs(distance) < epsilon)
        return "Select faces and enter a non-zero extrusion distance.";
    const std::set<std::size_t> pendingFaces(faces.begin(),faces.end());
    if (*pendingFaces.rbegin() >= mesh.faces.size())
        return "The face selection is no longer valid.";
    auto candidate = mesh;
    if (individual) {
        for (const auto index : pendingFaces) {
            const auto &face = mesh.faces[index];
            const auto offset = mul(faceNormal(mesh,face),distance);
            std::vector<Point3> positions;
            for (const auto &corner : face.corners)
                positions.push_back(add(mesh.vertices[corner.vertex],offset));
            addRing(candidate,index,positions);
        }
        return commit(mesh,std::move(candidate));
    }
    const MeshTopology topology(mesh);
    auto unvisited = pendingFaces;
    std::vector<std::vector<std::size_t>> incident(mesh.vertices.size());
    for (std::size_t i = 0; i < mesh.faces.size(); ++i)
        for (const auto &corner : mesh.faces[i].corners)
            incident[corner.vertex].push_back(i);
    while (!unvisited.empty()) {
        std::vector<std::size_t> component{*unvisited.begin()};
        unvisited.erase(component.front());
        for (std::size_t i = 0; i < component.size(); ++i)
            for (const auto &edge : topology.faceEdges[component[i]]) {
                const auto &neighbors = topology.edgeFaces.at(edge);
                if (neighbors.size() > 2)
                    return "Region extrusion needs manifold edges.";
                for (const auto face : neighbors)
                    if (unvisited.erase(face))
                        component.push_back(face);
            }
        const std::set<std::size_t> region(component.begin(),component.end());
        Point3 normal{};
        std::map<VertexIndex,VertexIndex> vertexMap;
        std::set<VertexIndex> border;
        struct Side { VertexIndex from, to; std::uint32_t material; };
        std::vector<Side> sides;
        for (const auto index : component) {
            const auto &face = mesh.faces[index];
            const auto &corners = face.corners;
            const auto first = mesh.vertices[corners.front().vertex];
            for (std::size_t j = 1; j+1 < corners.size(); ++j)
                normal = add(normal,cross(sub(mesh.vertices[corners[j].vertex],first),
                                          sub(mesh.vertices[corners[j+1].vertex],first)));
            for (std::size_t j = 0; j < corners.size(); ++j) {
                const auto a = corners[j].vertex, b = corners[(j+1)%corners.size()].vertex;
                vertexMap.emplace(a,a);
                const auto &neighbors = topology.edgeFaces.at(edgeAt(face,j));
                if (neighbors.size() == 1 || std::ranges::any_of(neighbors,[&](auto face) { return !region.contains(face); })) {
                    border.insert(a);
                    border.insert(b);
                    sides.push_back({a,b,face.material});
                }
            }
        }
        const auto direction = unit(normal);
        if (sides.empty() || direction == Point3{})
            return "Select an open region with a consistent extrusion direction.";
        const auto offset = mul(direction,distance);
        for (auto &[oldVertex,newVertex] : vertexMap) {
            const auto moved = add(mesh.vertices[oldVertex],offset);
            if (border.contains(oldVertex) || std::ranges::any_of(incident[oldVertex],[&](auto face) { return !region.contains(face); })) {
                newVertex = VertexIndex(candidate.vertices.size());
                candidate.vertices.push_back(moved);
            } else {
                candidate.vertices[oldVertex] = moved;
            }
        }
        for (const auto index : component)
            for (auto &corner : candidate.faces[index].corners)
                corner.vertex = vertexMap.at(corner.vertex);
        for (const auto &side : sides)
            candidate.faces.push_back({{{side.from,{0,0}},{side.to,{1,0}},
                {vertexMap.at(side.to),{1,1}},{vertexMap.at(side.from),{0,1}}},side.material});
    }
    return commit(mesh,std::move(candidate));
}

namespace {
std::optional<std::vector<Point3>> insetPolygon(const Mesh &mesh,const MeshFace &face,float width)
{
    const auto normal = faceNormal(mesh,face);
    const auto count = face.corners.size();
    const auto origin = mesh.vertices[face.corners.front().vertex];
    std::vector<Point3> positions;
    for (std::size_t i = 0; i < count; ++i) {
        const auto point = mesh.vertices[face.corners[i].vertex];
        const auto previous = mesh.vertices[face.corners[(i+count-1)%count].vertex];
        const auto next = mesh.vertices[face.corners[(i+1)%count].vertex];
        const auto incoming = unit(sub(point,previous)), outgoing = unit(sub(next,point));
        if (std::abs(dot(sub(point,origin),normal)) > 0.0001f || dot(cross(incoming,outgoing),normal) <= epsilon)
            return {};
        const auto a = cross(normal,incoming), b = cross(normal,outgoing);
        positions.push_back(add(point,mul(add(a,b),width/(1+dot(a,b)))));
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto j = (i+1)%count;
        if (dot(sub(positions[j],positions[i]),sub(mesh.vertices[face.corners[j].vertex],
                                                 mesh.vertices[face.corners[i].vertex])) <= epsilon)
            return {};
    }
    return positions;
}
}

std::string insetFaces(Mesh &mesh,std::span<const std::size_t> faces,float width)
{
    if (faces.empty() || !std::isfinite(width) || width <= epsilon)
        return "Select faces and enter a positive inset width.";
    auto candidate = mesh;
    const std::set<std::size_t> unique(faces.begin(),faces.end());
    for (const auto index : unique) {
        if (index >= mesh.faces.size())
            return "The face selection is no longer valid.";
        const auto &face = mesh.faces[index];
        const auto positions = insetPolygon(mesh,face,width);
        if (!positions)
            return "Inset requires convex planar faces and a width that fits.";
        const auto triangles = faceTriangles(mesh,face);
        std::vector<Point2> insetUvs;
        for (const auto p : *positions) {
            std::optional<Point2> uv;
            for (const auto &triangle : triangles) {
                const auto &a = face.corners[triangle[0]], &b = face.corners[triangle[1]], &c = face.corners[triangle[2]];
                const auto ab = sub(mesh.vertices[b.vertex],mesh.vertices[a.vertex]);
                const auto ac = sub(mesh.vertices[c.vertex],mesh.vertices[a.vertex]);
                const auto ap = sub(p,mesh.vertices[a.vertex]);
                const auto d00 = dot(ab,ab), d01 = dot(ab,ac), d11 = dot(ac,ac);
                const auto denominator = d00*d11-d01*d01;
                if (!(denominator > 0))
                    continue;
                const auto t = (d11*dot(ap,ab)-d01*dot(ap,ac))/denominator;
                const auto s = (d00*dot(ap,ac)-d01*dot(ap,ab))/denominator;
                if (t >= -0.0001f && s >= -0.0001f && t+s <= 1.0001f) {
                    uv = Point2{a.uv[0]*(1-t-s)+b.uv[0]*t+c.uv[0]*s,a.uv[1]*(1-t-s)+b.uv[1]*t+c.uv[1]*s};
                    break;
                }
            }
            if (!uv)
                return "Inset could not interpolate the selected face's UVs.";
            insetUvs.push_back(*uv);
        }
        const auto firstSide = candidate.faces.size();
        addRing(candidate,index,*positions);
        for (std::size_t j = 0; j < insetUvs.size(); ++j) {
            const auto next = (j+1)%insetUvs.size();
            candidate.faces[index].corners[j].uv = insetUvs[j];
            auto &side = candidate.faces[firstSide+j];
            side.corners[0].uv = face.corners[j].uv;
            side.corners[1].uv = face.corners[next].uv;
            side.corners[2].uv = insetUvs[next];
            side.corners[3].uv = insetUvs[j];
        }
    }
    return commit(mesh,std::move(candidate));
}

std::string bevelFace(Mesh &mesh,std::size_t index,float width,unsigned segments)
{
    if (index >= mesh.faces.size() || !std::isfinite(width) || width < epsilon || !segments || segments > 16)
        return "Select a face and enter a positive bevel width with one to sixteen segments.";
    const auto &face = mesh.faces[index];
    const auto normal = faceNormal(mesh, face);
    const auto count = face.corners.size();
    std::vector<Point3> positions;
    std::set<VertexIndex> capVertices;
    for (const auto &corner : face.corners)
        capVertices.insert(corner.vertex);
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
                const auto plane = mesh.vertices[face.corners.front().vertex];
                for (const auto &corner : neighbor.corners)
                    if (!capVertices.contains(corner.vertex) && dot(sub(plane,mesh.vertices[corner.vertex]),normal) <= width)
                        return "Bevel width exceeds the depth of an adjacent face.";
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
        if (std::ranges::any_of(other.corners,[&](const auto &corner) { return capVertices.contains(corner.vertex); }))
            for (auto &corner : other.corners)
                corner.normal = {};
    std::vector<VertexIndex> previous;
    for (const auto &corner : face.corners)
        previous.push_back(corner.vertex);
    for (unsigned ring = 1; ring <= segments; ++ring) {
        const float t = float(ring)/float(segments), s = 1-t;
        const auto first = VertexIndex(candidate.vertices.size());
        for (std::size_t j = 0; j < count; ++j) {
            const auto old = face.corners[j].vertex;
            const auto control = mesh.vertices[old];
            candidate.vertices.push_back(add(add(mul(candidate.vertices[old],s*s),mul(control,2*s*t)),mul(positions[j],t*t)));
        }
        for (std::size_t j = 0; j < count; ++j) {
            const auto next = (j+1)%count;
            const auto v0 = float(ring-1)/float(segments);
            candidate.faces.push_back({{{previous[j],{0,v0}},{previous[next],{1,v0}},
                {first+VertexIndex(next),{1,t}},{first+VertexIndex(j),{0,t}}},face.material});
        }
        for (std::size_t j = 0; j < count; ++j)
            previous[j] = first+VertexIndex(j);
    }
    for (std::size_t j = 0; j < count; ++j)
        candidate.faces[index].corners[j].vertex = previous[j];
    return commit(mesh, std::move(candidate));
}

namespace {
MeshCorner interpolateCorner(const MeshCorner &from,const MeshCorner &to,VertexIndex vertex,float t)
{
    return {vertex,{from.uv[0]*(1-t)+to.uv[0]*t,from.uv[1]*(1-t)+to.uv[1]*t},
        unit(add(mul(from.normal,1-t),mul(to.normal,t)))};
}

float edgeLength(Point3 delta)
{
    return std::hypot(delta[0],delta[1],delta[2]);
}

std::string bevelAdjoiningEdges(Mesh &mesh,const std::set<MeshEdge> &selection,float width,unsigned segments)
{
    const MeshTopology topology(mesh);
    std::set<VertexIndex> affected;
    for (const auto &[a,b] : selection) {
        const auto found = topology.edgeFaces.find({a,b});
        if (found == topology.edgeFaces.end() || found->second.size() != 2)
            return "Bevel needs selected edges shared by exactly two faces.";
        const auto n = faceNormal(mesh,mesh.faces[found->second[0]]);
        const auto m = faceNormal(mesh,mesh.faces[found->second[1]]);
        if (std::abs(dot(n,m)) >= 1-0.00001f)
            return "Choose edges with a change in surface angle.";
        affected.insert(a);
        affected.insert(b);
    }
    for (const auto vertex : affected)
        for (const auto &edge : topology.vertexEdges[vertex])
            if (topology.edgeFaces.at(edge).size() != 2)
                return "Adjoining bevels require closed manifold corners.";

    // Each original face gets its own bevel rail. Endpoints on unselected
    // edges share the deepest trim requested by their two incident faces.
    struct FaceOffsets {
        std::vector<Point3> positions;
        std::vector<std::array<VertexIndex,2>> corners;
    };
    std::map<std::size_t,FaceOffsets> offsets;
    std::map<std::pair<VertexIndex,VertexIndex>,float> trim;
    for (const auto vertex : affected)
        for (const auto &edge : topology.vertexEdges[vertex])
            for (const auto face : topology.edgeFaces.at(edge))
                offsets.try_emplace(face);
    for (auto &[index,offset] : offsets) {
        const auto &face = mesh.faces[index];
        const auto count = face.corners.size();
        const auto normal = faceNormal(mesh,face);
        offset.positions.reserve(count);
        offset.corners.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto previous = (i+count-1)%count;
            const auto prev = face.corners[previous].vertex, vertex = face.corners[i].vertex;
            const auto next = face.corners[(i+1)%count].vertex;
            const auto p = mesh.vertices[vertex];
            const auto a = unit(sub(p,mesh.vertices[prev])), b = unit(sub(mesh.vertices[next],p));
            if (dot(cross(a,b),normal) < -epsilon || std::abs(dot(sub(p,mesh.vertices[face.corners[0].vertex]),normal)) > epsilon)
                return "Adjoining bevels require convex planar faces at the selection.";
            const bool left = selection.contains(edgeAt(face,previous)), right = selection.contains(edgeAt(face,i));
            if (!left && !right) {
                offset.positions.push_back(p);
                continue;
            }
            const auto u = cross(normal,a), v = cross(normal,b);
            const auto cosine = dot(u,v), determinant = 1-cosine*cosine;
            if (determinant <= epsilon)
                return "The selected edge meets a collinear bevel corner.";
            const float d0 = left ? width : 0, d1 = right ? width : 0;
            const auto point = add(p,add(mul(u,(d0-d1*cosine)/determinant),mul(v,(d1-d0*cosine)/determinant)));
            offset.positions.push_back(point);
            if (left != right) {
                const auto neighbor = left ? next : prev;
                const auto side = sub(mesh.vertices[neighbor],p);
                const float fraction = dot(sub(point,p),side)/dot(side,side);
                if (fraction <= 0 || fraction >= 1)
                    return "Bevel width exceeds the available edge length.";
                auto &deepest = trim[{vertex,neighbor}];
                deepest = std::max(deepest,fraction);
            }
        }
    }

    auto candidate = mesh;
    std::vector<VertexIndex> owners(mesh.vertices.size());
    std::iota(owners.begin(),owners.end(),VertexIndex(0));
    const auto addVertex = [&](VertexIndex owner,Point3 p) {
        const auto index = VertexIndex(candidate.vertices.size());
        candidate.vertices.push_back(p);
        owners.push_back(owner);
        return index;
    };
    std::map<std::pair<VertexIndex,VertexIndex>,VertexIndex> trims;
    for (const auto &[edge,fraction] : trim) {
        const auto [from,to] = edge;
        const auto opposite = trim.find({to,from});
        if (opposite != trim.end() && fraction+opposite->second >= 1-epsilon)
            return "Bevel width would collapse an unselected edge.";
        const auto p = add(mul(mesh.vertices[from],1-fraction),mul(mesh.vertices[to],fraction));
        trims.emplace(edge,addVertex(from,p));
    }
    for (auto &[index,offset] : offsets) {
        const auto &face = mesh.faces[index];
        auto &output = candidate.faces[index].corners;
        output.clear();
        const auto count = face.corners.size();
        const auto normal = faceNormal(mesh,face);
        const auto triangles = faceTriangles(mesh,face);
        const auto trimAt = [&](const MeshCorner &from,const MeshCorner &to) {
            const auto key = std::pair{from.vertex,to.vertex};
            const auto found = trims.find(key);
            return found == trims.end() ? from : interpolateCorner(from,to,found->second,trim.at(key));
        };
        for (std::size_t i = 0; i < count; ++i) {
            const auto previous = (i+count-1)%count;
            const auto &prev = face.corners[previous], &corner = face.corners[i], &next = face.corners[(i+1)%count];
            const bool left = selection.contains(edgeAt(face,previous)), right = selection.contains(edgeAt(face,i));
            auto a = trimAt(corner,prev), b = trimAt(corner,next);
            if (left && right) {
                const auto p = offset.positions[i];
                const auto vertex = addVertex(corner.vertex,p);
                bool mapped = false;
                for (const auto &triangle : triangles) {
                    const auto &x = face.corners[triangle[0]], &y = face.corners[triangle[1]], &z = face.corners[triangle[2]];
                    const auto o = mesh.vertices[x.vertex];
                    const auto u = sub(mesh.vertices[y.vertex],o), v = sub(mesh.vertices[z.vertex],o), d = sub(p,o);
                    const auto area = dot(cross(u,v),normal);
                    const auto s = dot(cross(d,v),normal)/area, t = dot(cross(u,d),normal)/area;
                    if (s < -epsilon || t < -epsilon || s+t > 1+epsilon)
                        continue;
                    a = b = {vertex,{x.uv[0]*(1-s-t)+y.uv[0]*s+z.uv[0]*t,x.uv[1]*(1-s-t)+y.uv[1]*s+z.uv[1]*t},
                        unit(add(add(mul(x.normal,1-s-t),mul(y.normal,s)),mul(z.normal,t)))};
                    mapped = true;
                    break;
                }
                if (!mapped)
                    return "Bevel width moves a corner beyond its original face.";
            } else if (left) {
                a = b;
            } else if (right) {
                b = a;
            }
            offset.corners[i] = {a.vertex,b.vertex};
            output.push_back(a);
            if (a.vertex != b.vertex)
                output.push_back(b);
        }
        for (std::size_t i = 0; i < count; ++i) {
            const auto p = mesh.vertices[face.corners[i].vertex];
            const auto q = mesh.vertices[face.corners[(i+1)%count].vertex];
            const auto inward = unit(cross(normal,sub(q,p)));
            const auto inset = selection.contains(edgeAt(face,i)) ? width : 0.0f;
            if (std::ranges::any_of(output,[&](const auto &corner) {
                    return dot(sub(candidate.vertices[corner.vertex],p),inward) < inset-0.0001f*std::max(1.0f,width);
                }))
                return "Bevel width would collapse an adjacent face.";
        }
    }

    std::map<MeshEdge,std::vector<VertexIndex>> profiles;
    for (const auto &edge : selection) {
        const auto &faces = topology.edgeFaces.at(edge);
        const auto side = [&](std::size_t f) {
            const auto &edges = topology.faceEdges[f];
            return std::size_t(std::ranges::find(edges,edge)-edges.begin());
        };
        const auto a = side(faces[0]), b = side(faces[1]);
        const auto &left = mesh.faces[faces[0]], &right = mesh.faces[faces[1]];
        const auto nextA = (a+1)%left.corners.size(), nextB = (b+1)%right.corners.size();
        if (left.corners[a].vertex != right.corners[nextB].vertex || left.corners[nextA].vertex != right.corners[b].vertex)
            return "Fix face winding before beveling the selected edges.";
        const auto &one = offsets.at(faces[0]).corners, &two = offsets.at(faces[1]).corners;
        const std::array start{one[a][1],one[nextA][0]}, end{two[nextB][0],two[b][1]};
        auto previous = start;
        for (unsigned step = 1; step <= segments; ++step) {
            const auto t = float(step)/float(segments), s = 1-t;
            auto next = end;
            if (step < segments)
                for (std::size_t i = 0; i < next.size(); ++i) {
                    const MeshEdge key = std::minmax(start[i],end[i]);
                    auto &rail = profiles[key];
                    if (rail.empty())
                        rail.resize(segments-1,VertexIndex(-1));
                    auto &index = rail[start[i] == key.first ? step-1 : segments-step-1];
                    const auto owner = owners[start[i]];
                    if (index == VertexIndex(-1))
                        index = addVertex(owner,add(add(mul(candidate.vertices[start[i]],s*s),
                            mul(mesh.vertices[owner],2*s*t)),mul(candidate.vertices[end[i]],t*t)));
                    next[i] = index;
                }
            const auto prior = float(step-1)/float(segments);
            candidate.faces.push_back({{{previous[1],{1,prior}},{previous[0],{0,prior}},
                {next[0],{0,t}},{next[1],{1,t}}},left.material});
            previous = next;
        }
    }

    struct Boundary { unsigned count = 0; VertexIndex from = 0,to = 0; std::uint32_t material = 0; };
    std::map<MeshEdge,Boundary> boundary;
    for (const auto &face : candidate.faces)
        for (std::size_t i = 0; i < face.corners.size(); ++i) {
            const auto a = face.corners[i].vertex, b = face.corners[(i+1)%face.corners.size()].vertex;
            if (owners[a] != owners[b])
                continue;
            auto &side = boundary[std::minmax(a,b)];
            if (side.count++ > 1 || (side.count == 2 && side.from == a))
                return "The selected bevel edges produce a non-manifold corner.";
            side.from = a;
            side.to = b;
            side.material = face.material;
        }
    std::map<VertexIndex,Boundary> remaining;
    for (const auto &[key,side] : boundary)
        if (side.count == 1 && !remaining.emplace(side.to,side).second)
            return "The selected bevel edges produce intersecting junction loops.";
    while (!remaining.empty()) {
        const auto start = remaining.begin()->first;
        MeshFace cap{{},remaining.begin()->second.material};
        auto next = start;
        do {
            const auto edge = remaining.find(next);
            if (edge == remaining.end())
                return "The selected bevel edges leave an open junction.";
            cap.corners.push_back({next});
            next = edge->second.from;
            remaining.erase(edge);
        } while (next != start);
        if (cap.corners.size() < 3)
            return "The selected bevel edges produce a collapsed junction.";
        candidate.faces.push_back(std::move(cap));
        const auto index = candidate.faces.size()-1;
        if (auto error = unwrapFace(candidate,index); !error.empty() || faceTriangles(candidate,candidate.faces.back()).empty())
            return "The bevel junction cannot be triangulated. Try a smaller width.";
    }
    removeUnusedVertices(candidate);
    return commit(mesh,std::move(candidate));
}
}

std::string bevelEdges(Mesh &mesh,std::span<const MeshEdge> edges,float width,unsigned segments)
{
    if (edges.empty() || !std::isfinite(width) || width <= epsilon || !segments || segments > 16)
        return "Select edges, a positive width and 1 to 16 bevel segments.";
    std::set<VertexIndex> endpoints;
    std::set<MeshEdge> selection;
    for (const auto &edge : edges)
        selection.insert(std::minmax(edge.first,edge.second));
    bool adjoining = false;
    for (const auto &edge : selection) {
        const bool first = endpoints.insert(edge.first).second, second = endpoints.insert(edge.second).second;
        adjoining |= !first || !second;
    }
    if (adjoining) {
        for (std::size_t i = 0; i < mesh.faces.size(); ++i) {
            const auto &face = mesh.faces[i];
            if (face.corners.size() != selection.size())
                continue;
            bool perimeter = true;
            for (std::size_t j = 0; j < face.corners.size(); ++j)
                perimeter &= selection.contains(edgeAt(face,j));
            if (perimeter && bevelFace(mesh,i,width,segments).empty())
                return {};
        }
        return bevelAdjoiningEdges(mesh,selection,width,segments);
    }
    auto candidate = mesh;
    for (const auto &edge : selection) {
        const MeshTopology topology(candidate);
        const auto found = topology.edgeFaces.find(edge);
        if (found == topology.edgeFaces.end() || found->second.size() != 2)
            return "Edge bevel requires an edge shared by exactly two faces.";
        const auto first = found->second[0], second = found->second[1];
        const auto side = [&](std::size_t index) {
            const auto &sides = topology.faceEdges[index];
            return std::size_t(std::ranges::find(sides,edge)-sides.begin());
        };
        const auto faceA = candidate.faces[first], faceB = candidate.faces[second];
        const auto sideA = side(first), sideB = side(second);
        const auto a = faceA.corners[sideA].vertex, b = faceA.corners[(sideA+1)%faceA.corners.size()].vertex;
        if (faceB.corners[sideB].vertex != b || faceB.corners[(sideB+1)%faceB.corners.size()].vertex != a)
            return "Fix face winding before beveling the edge.";
        std::array<std::vector<VertexIndex>,2> rails;
        for (unsigned end = 0; end < 2; ++end) {
            const auto vertex = end ? b : a;
            if (topology.vertexEdges[vertex].size() != 3)
                return "Edge bevel currently requires three incident edges at each endpoint.";
            const auto &faceCornerA = faceA.corners[(sideA+end)%faceA.corners.size()];
            const auto &faceCornerB = faceB.corners[(sideB+1-end)%faceB.corners.size()];
            const auto &neighborA = faceA.corners[(sideA+(end ? 2 : faceA.corners.size()-1))%faceA.corners.size()];
            const auto &neighborB = faceB.corners[(sideB+(end ? faceB.corners.size()-1 : 2))%faceB.corners.size()];
            const auto third = [&](VertexIndex neighbor,std::size_t excluded) -> std::optional<std::size_t> {
                const auto &attached = topology.edgeFaces.at(MeshEdge(std::minmax(vertex,neighbor)));
                if (attached.size() != 2)
                    return {};
                return attached.front() == excluded ? attached.back() : attached.front();
            };
            const auto thirdA = third(neighborA.vertex,first), thirdB = third(neighborB.vertex,second);
            if (!thirdA || thirdA != thirdB || *thirdA == first || *thirdA == second)
                return "The bevel endpoint needs a closed three-face corner.";
            const auto p = candidate.vertices[vertex];
            const auto directionA = sub(candidate.vertices[neighborA.vertex],p);
            const auto directionB = sub(candidate.vertices[neighborB.vertex],p);
            const auto lengthA = edgeLength(directionA), lengthB = edgeLength(directionB);
            if (width >= 0.5f*std::min(lengthA,lengthB))
                return "Bevel width exceeds the available endpoint space.";
            const auto tA = width/lengthA, tB = width/lengthB;
            const auto cutA = add(p,mul(directionA,tA)), cutB = add(p,mul(directionB,tB));
            auto &rail = rails[end];
            for (unsigned j = 0; j <= segments; ++j) {
                const float t = float(j)/float(segments), s = 1-t;
                rail.push_back(VertexIndex(candidate.vertices.size()));
                candidate.vertices.push_back(add(add(mul(cutA,s*s),mul(p,2*s*t)),mul(cutB,t*t)));
            }
            auto &thirdFace = candidate.faces[*thirdA];
            const auto corner = std::ranges::find(thirdFace.corners,vertex,&MeshCorner::vertex);
            if (corner == thirdFace.corners.end())
                return "Bevel could not find the endpoint's third face corner.";
            const auto i = std::size_t(corner-thirdFace.corners.begin());
            const auto count = thirdFace.corners.size();
            const auto &prev = thirdFace.corners[(i+count-1)%count], &next = thirdFace.corners[(i+1)%count];
            const bool aFirst = prev.vertex == neighborA.vertex && next.vertex == neighborB.vertex;
            if (!aFirst && (prev.vertex != neighborB.vertex || next.vertex != neighborA.vertex))
                return "Bevel could not orient the endpoint profile.";
            const auto from = interpolateCorner(*corner,prev,aFirst ? rail.front() : rail.back(),aFirst ? tA : tB);
            const auto to = interpolateCorner(*corner,next,aFirst ? rail.back() : rail.front(),aFirst ? tB : tA);
            std::vector<MeshCorner> profile;
            for (unsigned j = 0; j <= segments; ++j)
                profile.push_back(interpolateCorner(from,to,rail[aFirst ? j : segments-j],float(j)/float(segments)));
            thirdFace.corners.erase(corner);
            thirdFace.corners.insert(thirdFace.corners.begin()+std::ptrdiff_t(i),profile.begin(),profile.end());
            candidate.faces[first].corners[(sideA+end)%faceA.corners.size()] =
                interpolateCorner(faceCornerA,neighborA,rail.front(),tA);
            candidate.faces[second].corners[(sideB+1-end)%faceB.corners.size()] =
                interpolateCorner(faceCornerB,neighborB,rail.back(),tB);
        }
        for (unsigned j = 0; j < segments; ++j) {
            const float v0 = float(j)/float(segments), v1 = float(j+1)/float(segments);
            candidate.faces.push_back({{{rails[1][j],{1,v0}},{rails[0][j],{0,v0}},
                {rails[0][j+1],{0,v1}},{rails[1][j+1],{1,v1}}},faceA.material});
        }
    }
    removeUnusedVertices(candidate);
    return commit(mesh,std::move(candidate));
}

std::string bevelVertices(Mesh &mesh,std::span<const VertexIndex> vertices,float width)
{
    const std::set<VertexIndex> selected(vertices.begin(),vertices.end());
    if (selected.empty() || *selected.rbegin() >= mesh.vertices.size() || !std::isfinite(width) || width <= epsilon)
        return "Select vertices and enter a positive chamfer width.";
    auto candidate = mesh;
    const MeshTopology topology(mesh);
    std::map<MeshEdge,std::pair<VertexIndex,float>> rails;
    std::map<VertexIndex,std::vector<MeshEdge>> boundaries;
    for (const auto i : selected) {
        if (topology.vertexEdges[i].size() < 3)
            return "Vertex chamfer requires a closed manifold corner.";
        for (const auto &edge : topology.vertexEdges[i]) {
            if (topology.edgeFaces.at(edge).size() != 2)
                return "Vertex chamfer requires a closed manifold corner.";
            const auto j = edge.first == i ? edge.second : edge.first;
            const auto direction = sub(mesh.vertices[j],mesh.vertices[i]);
            const auto length = edgeLength(direction);
            if (width >= length*0.5f)
                return "The chamfer width exceeds the available edge length.";
            rails[{i,j}] = {VertexIndex(candidate.vertices.size()),width/length};
            candidate.vertices.push_back(add(mesh.vertices[i],mul(direction,width/length)));
        }
    }
    for (std::size_t face = 0; face < mesh.faces.size(); ++face) {
        const auto &corners = mesh.faces[face].corners;
        auto &output = candidate.faces[face].corners;
        output.clear();
        for (std::size_t i = 0; i < corners.size(); ++i) {
            const auto &current = corners[i];
            if (!selected.contains(current.vertex)) {
                output.push_back(current);
                continue;
            }
            const auto &prev = corners[(i+corners.size()-1)%corners.size()], &next = corners[(i+1)%corners.size()];
            const auto [a,tA] = rails.at({current.vertex,prev.vertex});
            const auto [b,tB] = rails.at({current.vertex,next.vertex});
            output.push_back(interpolateCorner(current,prev,a,tA));
            output.push_back(interpolateCorner(current,next,b,tB));
            boundaries[current.vertex].emplace_back(a,b);
        }
    }
    for (const auto &[vertex,edges] : boundaries) {
        const auto loops = boundaryLoops(candidate,edges);
        if (loops.size() != 1 || loops.front().vertices.size() > 256)
            return "The chamfer cannot form a single cap.";
        MeshFace cap;
        cap.material = loops.front().material;
        for (auto i : loops.front().vertices)
            cap.corners.push_back({i});
        if (faceTriangles(candidate,cap).empty())
            return "The chamfer would create an invalid cap.";
        candidate.faces.push_back(std::move(cap));
        const auto error = unwrapFace(candidate,candidate.faces.size()-1);
        if (!error.empty())
            return error;
    }
    removeUnusedVertices(candidate);
    return commit(mesh,std::move(candidate));
}

namespace {
std::string slideVertices(Mesh &mesh,std::span<const MeshEdge> moves,float factor)
{
    if (!(factor > 0 && factor < 1))
        return "Use a slide value above 0% and below 100%.";
    auto candidate = mesh;
    for (const auto &[from,to] : moves)
        candidate.vertices[from] = add(mul(mesh.vertices[from],1-factor),mul(mesh.vertices[to],factor));
    for (std::size_t i = 0; i < mesh.faces.size(); ++i) {
        const auto &face = mesh.faces[i];
        for (std::size_t j = 0; j < face.corners.size(); ++j) {
            const auto &corner = face.corners[j];
            const auto move = std::ranges::find(moves,corner.vertex,&MeshEdge::first);
            if (move == moves.end())
                continue;
            const auto &prev = face.corners[(j+face.corners.size()-1)%face.corners.size()];
            const auto &next = face.corners[(j+1)%face.corners.size()];
            auto &result = candidate.faces[i].corners[j];
            if (prev.vertex == move->second)
                result = interpolateCorner(corner,prev,corner.vertex,factor);
            else if (next.vertex == move->second)
                result = interpolateCorner(corner,next,corner.vertex,factor);
            result.normal = {};
        }
    }
    return commit(mesh,std::move(candidate));
}
}

std::string slideVertex(Mesh &mesh,VertexIndex vertex,VertexIndex toward,float factor)
{
    const MeshTopology topology(mesh);
    if (!topology.edgeFaces.contains(MeshEdge(std::minmax(vertex,toward))))
        return "Select two vertices joined by an edge.";
    const std::array<MeshEdge,1> moves{{{vertex,toward}}};
    return slideVertices(mesh,moves,factor);
}

std::string slideEdge(Mesh &mesh,MeshEdge edge,float factor)
{
    if (!std::isfinite(factor) || factor == 0 || std::abs(factor) >= 1)
        return "Enter a non-zero slide between -100% and 100%.";
    edge = MeshEdge(std::minmax(edge.first,edge.second));
    const MeshTopology topology(mesh);
    const auto entry = topology.edgeFaces.find(edge);
    if (entry == topology.edgeFaces.end() || entry->second.size() != 2)
        return "Select an interior edge between two faces.";
    const auto faceIndex = entry->second[factor < 0 ? 0 : 1];
    const auto &face = mesh.faces[faceIndex];
    if (face.corners.size() != 4)
        return "Edge slide needs a quad on the chosen side.";
    const auto &sides = topology.faceEdges[faceIndex];
    const auto i = std::size_t(std::ranges::find(sides,edge)-sides.begin());
    const auto vertex = [&](std::size_t offset) { return face.corners[(i+offset)%4].vertex; };
    const std::array<MeshEdge,2> moves{{{vertex(0),vertex(3)},{vertex(1),vertex(2)}}};
    return slideVertices(mesh,moves,std::abs(factor));
}

std::string splitFaces(Mesh &mesh,std::span<const std::size_t> faces)
{
    const std::set<std::size_t> selected(faces.begin(),faces.end());
    if (selected.empty() || *selected.rbegin() >= mesh.faces.size())
        return "Select a valid face region to split.";
    std::vector<bool> outside(mesh.vertices.size());
    for (std::size_t i = 0; i < mesh.faces.size(); ++i)
        if (!selected.contains(i))
            for (const auto &corner : mesh.faces[i].corners)
                outside[corner.vertex] = true;
    auto candidate = mesh;
    std::map<VertexIndex,VertexIndex> split;
    for (const auto i : selected)
        for (auto &corner : candidate.faces[i].corners)
            if (outside[corner.vertex]) {
                const auto [entry,created] = split.try_emplace(corner.vertex,VertexIndex(candidate.vertices.size()));
                if (created)
                    candidate.vertices.push_back(mesh.vertices[corner.vertex]);
                corner.vertex = entry->second;
            }
    return split.empty() ? "The selected region is already disconnected." : commit(mesh,std::move(candidate));
}

namespace {
std::string divideFace(Mesh &mesh,std::size_t face,VertexIndex start,VertexIndex end)
{
    const auto original = mesh.faces[face];
    const auto &corners = original.corners;
    const auto a = std::size_t(std::ranges::find(corners,start,&MeshCorner::vertex)-corners.begin());
    const auto b = std::size_t(std::ranges::find(corners,end,&MeshCorner::vertex)-corners.begin());
    if (a >= corners.size() || b >= corners.size() || a == b || (a+1)%corners.size() == b || (b+1)%corners.size() == a)
        return "The cut must connect nonadjacent points on one face.";
    const auto polygon = [&](std::size_t first,std::size_t last) {
        MeshFace part{{},original.material};
        for (auto i = first; i != last; i = (i+1)%corners.size())
            part.corners.push_back(corners[i]);
        part.corners.push_back(corners[last]);
        return part;
    };
    auto left = polygon(a,b), right = polygon(b,a);
    const auto normal = faceNormal(mesh,original);
    if (dot(normal,faceNormal(mesh,left)) <= 0 || dot(normal,faceNormal(mesh,right)) <= 0
        || faceTriangles(mesh,left).empty() || faceTriangles(mesh,right).empty())
        return "This cut would leave the face or create a zero-area polygon.";
    mesh.faces[face] = std::move(left);
    mesh.faces.push_back(std::move(right));
    return {};
}

void insertEdgePoints(Mesh &mesh,const std::map<MeshEdge,std::pair<VertexIndex,float>> &points)
{
    for (auto &face : mesh.faces) {
        std::vector<MeshCorner> corners;
        corners.reserve(face.corners.size());
        for (std::size_t i = 0; i < face.corners.size(); ++i) {
            const auto &a = face.corners[i], &b = face.corners[(i+1)%face.corners.size()];
            corners.push_back(a);
            const auto key = MeshEdge(std::minmax(a.vertex,b.vertex));
            if (const auto point = points.find(key); point != points.end()) {
                const auto [index,fraction] = point->second;
                corners.push_back(interpolateCorner(a,b,index,a.vertex == key.first ? fraction : 1-fraction));
            }
        }
        face.corners = std::move(corners);
    }
}
}

std::string knifeFace(Mesh &mesh,std::size_t face,MeshCutPoint start,MeshCutPoint end)
{
    const std::array<MeshKnifeSegment,1> path{{{face,start,end}}};
    return knifePath(mesh,path);
}

std::string connectVertices(Mesh &mesh,VertexIndex start,VertexIndex end)
{
    if (start == end || start >= mesh.vertices.size() || end >= mesh.vertices.size())
        return "Choose two different vertices.";
    const MeshTopology topology(mesh);
    if (topology.edgeFaces.contains(MeshEdge(std::minmax(start,end))))
        return "These vertices already share an edge.";
    auto candidate = mesh;
    std::optional<std::size_t> common;
    for (std::size_t face = 0; face < mesh.faces.size(); ++face) {
        const auto &corners = mesh.faces[face].corners;
        if (std::ranges::find(corners,start,&MeshCorner::vertex) == corners.end()
            || std::ranges::find(corners,end,&MeshCorner::vertex) == corners.end())
            continue;
        if (common)
            return "The vertices share multiple faces. Use Knife to choose the face.";
        common = face;
    }
    if (!common)
        return "The vertices must belong to one face.";
    const auto error = divideFace(candidate,*common,start,end);
    return error.empty() ? commit(mesh,std::move(candidate)) : error;
}

std::string connectEdges(Mesh &mesh,MeshEdge start,MeshEdge end,std::set<MeshEdge> *seams)
{
    start = MeshEdge(std::minmax(start.first,start.second));
    end = MeshEdge(std::minmax(end.first,end.second));
    const MeshTopology topology(mesh);
    const auto a = topology.edgeFaces.find(start), b = topology.edgeFaces.find(end);
    if (start == end || a == topology.edgeFaces.end() || b == topology.edgeFaces.end())
        return "Choose two different edges.";
    std::optional<std::size_t> common;
    for (const auto face : a->second)
        if (std::ranges::find(b->second,face) != b->second.end()) {
            if (common)
                return "The edges share multiple faces. Use Knife to choose the face.";
            common = face;
        }
    if (!common)
        return "The edges must belong to one face.";
    const std::array<MeshKnifeSegment,1> path{{{*common,{start,0.5f},{end,0.5f}}}};
    return knifePath(mesh,path,seams);
}

std::string knifePath(Mesh &mesh,std::span<const MeshKnifeSegment> path,std::set<MeshEdge> *seams)
{
    if (path.empty())
        return "Pick at least two edges to start the cut.";
    auto candidate = mesh;
    const MeshTopology topology(mesh);
    std::map<MeshEdge,std::pair<VertexIndex,float>> newPoints;
    const auto resolvePoint = [&](std::size_t face,MeshCutPoint input) -> std::optional<VertexIndex> {
        if (!std::isfinite(input.fraction) || input.fraction < 0 || input.fraction > 1)
            return {};
        if (input.edge.first > input.edge.second) {
            std::swap(input.edge.first,input.edge.second);
            input.fraction = 1-input.fraction;
        }
        const auto &edges = topology.faceEdges[face];
        if (std::ranges::find(edges,input.edge) == edges.end())
            return {};
        if (input.fraction <= epsilon)
            return input.edge.first;
        if (input.fraction >= 1-epsilon)
            return input.edge.second;
        const auto [existing,inserted] = newPoints.try_emplace(input.edge,VertexIndex(candidate.vertices.size()),input.fraction);
        if (!inserted)
            return std::abs(existing->second.second-input.fraction) <= epsilon
                ? std::optional{existing->second.first} : std::nullopt;
        candidate.vertices.push_back(add(mul(mesh.vertices[input.edge.first],1-input.fraction),
                                         mul(mesh.vertices[input.edge.second],input.fraction)));
        return existing->second.first;
    };
    std::vector<std::pair<VertexIndex,VertexIndex>> segments;
    for (const auto &segment : path) {
        if (segment.face >= mesh.faces.size())
            return "The cut refers to a missing face.";
        const auto a = resolvePoint(segment.face,segment.start), b = resolvePoint(segment.face,segment.end);
        if (!a || !b || *a == *b)
            return "Choose distinct points on the same face for each segment.";
        if (!segments.empty() && segments.back().second != *a)
            return "Continue the cut from its last point.";
        segments.emplace_back(*a,*b);
    }
    if (candidate.vertices.size() > 2'000'000 || mesh.faces.size()+path.size() > 2'000'000)
        return "This cut exceeds the editor's mesh limit.";
    insertEdgePoints(candidate,newPoints);
    std::map<std::size_t,std::vector<std::size_t>> splitFaces;
    for (std::size_t i = 0; i < path.size(); ++i) {
        const auto [a,b] = segments[i];
        auto &pieces = splitFaces[path[i].face];
        if (pieces.empty())
            pieces.push_back(path[i].face);
        const auto piece = std::ranges::find_if(pieces,[&](const auto face) {
            const auto &corners = candidate.faces[face].corners;
            return std::ranges::find(corners,a,&MeshCorner::vertex) != corners.end()
                && std::ranges::find(corners,b,&MeshCorner::vertex) != corners.end();
        });
        if (piece == pieces.end())
            return "The cut crosses an earlier segment. End it at an existing cut point.";
        const auto error = divideFace(candidate,*piece,a,b);
        if (!error.empty())
            return error;
        pieces.push_back(candidate.faces.size()-1);
    }
    const auto error = commit(mesh,std::move(candidate));
    if (error.empty() && seams)
        for (const auto &[edge,point] : newPoints)
            if (seams->erase(edge)) {
                seams->insert(std::minmax(edge.first,point.first));
                seams->insert(std::minmax(point.first,edge.second));
            }
    return error;
}

std::string bisectMesh(Mesh &mesh,unsigned axis,float coordinate)
{
    if (axis >= 3 || !std::isfinite(coordinate))
        return "Enter a valid axis and coordinate.";
    auto candidate = mesh;
    const MeshTopology topology(mesh);
    std::map<MeshEdge,std::pair<VertexIndex,float>> points;
    for (const auto &[edge,faces] : topology.edgeFaces) {
        const auto a = mesh.vertices[edge.first][axis]-coordinate, b = mesh.vertices[edge.second][axis]-coordinate;
        if ((a < -epsilon && b > epsilon) || (a > epsilon && b < -epsilon)) {
            const float t = -a/(b-a);
            auto vertex = add(mul(mesh.vertices[edge.first],1-t),mul(mesh.vertices[edge.second],t));
            vertex[axis] = coordinate;
            points.emplace(edge,std::pair{VertexIndex(candidate.vertices.size()),t});
            candidate.vertices.push_back(vertex);
        }
    }
    insertEdgePoints(candidate,points);
    bool split = false;
    for (std::size_t i = 0; i < mesh.faces.size(); ++i) {
        bool positive = false, negative = false;
        std::vector<VertexIndex> onPlane;
        for (const auto &corner : candidate.faces[i].corners) {
            const float distance = candidate.vertices[corner.vertex][axis]-coordinate;
            positive |= distance > epsilon;
            negative |= distance < -epsilon;
            if (std::abs(distance) <= epsilon)
                onPlane.push_back(corner.vertex);
        }
        if (!positive || !negative)
            continue;
        if (onPlane.size() != 2)
            return "Bisect encountered a face with multiple plane intersections. Triangulate that face first.";
        const auto error = divideFace(candidate,i,onPlane[0],onPlane[1]);
        if (!error.empty())
            return error;
        split = true;
    }
    return split ? commit(mesh,std::move(candidate)) : "The plane does not cross any face.";
}

std::string spinEdges(Mesh &mesh,std::span<const MeshEdge> profile,unsigned axis,Point3 center,float degrees,unsigned steps)
{
    if (profile.empty() || axis >= 3 || !finite(center) || !std::isfinite(degrees)
        || std::abs(degrees) < 0.01f || std::abs(degrees) > 360 || steps < 1 || steps > 256)
        return "Select a boundary profile and enter a valid spin angle and step count.";
    const bool full = std::abs(degrees) == 360;
    if (std::abs(degrees)/float(steps) >= 180)
        return "Use enough steps to keep each spin segment below 180 degrees.";
    const MeshTopology topology(mesh);
    struct Side { VertexIndex a,b; std::uint32_t material; };
    std::vector<Side> sides;
    std::map<VertexIndex,unsigned> degree;
    const std::set<MeshEdge> edges(profile.begin(),profile.end());
    for (const auto &edge : edges) {
        const auto found = topology.edgeFaces.find(edge);
        if (found == topology.edgeFaces.end() || found->second.size() != 1)
            return "Spin requires open boundary edges.";
        const auto &face = mesh.faces[found->second.front()];
        const auto a = std::ranges::find(face.corners,edge.first,&MeshCorner::vertex);
        const auto next = std::next(a) == face.corners.end() ? face.corners.begin() : std::next(a);
        const bool forward = next->vertex == edge.second;
        sides.push_back({forward ? edge.first : edge.second,forward ? edge.second : edge.first,face.material});
        if (++degree[edge.first] > 2 || ++degree[edge.second] > 2)
            return "Spin profiles cannot branch.";
    }
    if (mesh.vertices.size()+degree.size()*(steps+1) > 2'000'000 || mesh.faces.size()+sides.size()*steps > 2'000'000)
        return "Spin exceeds the editor's geometry limit.";
    const auto u = (axis+1)%3, v = (axis+2)%3;
    auto result = mesh;
    std::map<VertexIndex,VertexIndex> first,ring;
    for (const auto &[vertex,count] : degree) {
        first[vertex] = full ? VertexIndex(result.vertices.size()) : vertex;
        if (full)
            result.vertices.push_back(mesh.vertices[vertex]);
    }
    auto previous = first;
    for (unsigned step = 1; step <= steps; ++step) {
        const auto angle = degrees*std::numbers::pi_v<float>/180*float(step)/float(steps);
        for (const auto &[vertex,count] : degree) {
            auto p = mesh.vertices[vertex];
            const auto a = p[u]-center[u], b = p[v]-center[v];
            if ((full && step == steps) || std::hypot(a,b) <= epsilon) {
                ring[vertex] = first.at(vertex);
            } else {
                p[u] = center[u]+a*std::cos(angle)-b*std::sin(angle);
                p[v] = center[v]+a*std::sin(angle)+b*std::cos(angle);
                ring[vertex] = VertexIndex(result.vertices.size());
                result.vertices.push_back(p);
            }
        }
        for (const auto &[a,b,material] : sides) {
            const auto left = float(step-1)/float(steps), right = float(step)/float(steps);
            MeshFace face{{{previous[b],{left,1}},{previous[a],{left,0}},
                {ring[a],{right,0}},{ring[b],{right,1}}},material};
            if (previous[a] == ring[a])
                face.corners.erase(face.corners.begin()+2);
            if (previous[b] == ring[b])
                face.corners.pop_back();
            if (face.corners.size() < 3)
                return "A profile edge lies entirely on the spin axis.";
            result.faces.push_back(std::move(face));
        }
        previous.swap(ring);
    }
    return commit(mesh,std::move(result));
}

std::optional<std::vector<Point2>> sampleNurbsProfile(std::span<const Point2> points,
    std::span<const float> weights,unsigned degree,unsigned segments)
{
    if (points.size() < 2 || points.size() > 256 || points.size() != weights.size()
        || degree < 1 || degree > 5 || degree >= points.size() || segments < 2 || segments > 255)
        return {};
    for (std::size_t i = 0; i < points.size(); ++i)
        if (!finite(points[i]) || points[i][0] < 0 || !std::isfinite(weights[i])
            || weights[i] < 0.0001f || weights[i] > 10000.0f)
            return {};
    const auto count = unsigned(points.size());
    const auto intervals = count-degree;
    const auto knot = [degree,intervals](unsigned i) {
        return i <= degree ? 0.0 : std::min(1.0,double(i-degree)/intervals);
    };
    std::vector<Point2> samples;
    samples.reserve(segments+1);
    for (unsigned sample = 0; sample < segments; ++sample) {
        const double t = double(sample)/segments;
        const auto span = std::min(degree+unsigned(t*intervals),count-1);
        std::array<std::array<double,3>,6> work{};
        for (unsigned j = 0; j <= degree; ++j) {
            const auto index = span-degree+j;
            const double w = weights[index];
            work[j] = {points[index][0]*w,points[index][1]*w,w};
        }
        for (unsigned order = 1; order <= degree; ++order)
            for (unsigned j = degree; j >= order; --j) {
                const auto i = span-degree+j;
                const auto low = knot(i), high = knot(i+degree+1-order);
                const auto mix = high > low ? (t-low)/(high-low) : 0.0;
                for (unsigned axis = 0; axis < 3; ++axis)
                    work[j][axis] = std::lerp(work[j-1][axis],work[j][axis],mix);
            }
        const auto [radius,height,weight] = work[degree];
        samples.push_back({float(radius/weight),float(height/weight)});
    }
    samples.push_back(points.back());
    return samples;
}

std::string revolveProfile(Mesh &mesh,std::span<const Point2> profile,unsigned axis,float degrees,unsigned steps)
{
    if (profile.size() < 2 || profile.size() > 256 || axis >= 3 || steps < 1 || steps > 256
        || !std::isfinite(degrees) || std::abs(degrees) > 360 || std::abs(degrees) < 0.01f
        || std::abs(degrees)/float(steps) >= 180)
        return "Use 2 to 256 profile points and a valid sweep angle and step count.";
    std::vector<float> length(profile.size());
    for (std::size_t i = 0; i < profile.size(); ++i) {
        const auto [radius,height] = profile[i];
        if (!finite(profile[i]) || radius < 0 || radius > 100000 || std::abs(height) > 100000)
            return "Use finite coordinates and nonnegative radii.";
        if (radius <= epsilon && i > 0 && i+1 < profile.size())
            return "Only the ends of the profile can touch the axis.";
        if (i) {
            const float segment = std::hypot(radius-profile[i-1][0],height-profile[i-1][1]);
            if (segment <= epsilon || (radius <= epsilon && profile[i-1][0] <= epsilon))
                return "Each segment needs two distinct points and a positive radius at one end.";
            length[i] = length[i-1]+segment;
        }
    }
    const auto side = [](Point2 a,Point2 b,Point2 p) {
        return (double(b[0])-a[0])*(double(p[1])-a[1])-(double(b[1])-a[1])*(double(p[0])-a[0]);
    };
    const auto onSegment = [](Point2 a,Point2 b,Point2 p) {
        return p[0] >= std::min(a[0],b[0]) && p[0] <= std::max(a[0],b[0])
            && p[1] >= std::min(a[1],b[1]) && p[1] <= std::max(a[1],b[1]);
    };
    for (std::size_t i = 1; i < profile.size(); ++i) {
        const auto a = profile[i-1], b = profile[i];
        for (std::size_t j = i+1; j < profile.size(); ++j) {
            const auto c = profile[j-1], d = profile[j];
            if (j == i+1) {
                if (side(a,b,d) == 0 && (onSegment(a,b,d) || onSegment(c,d,a)))
                    return "The profile doubles back along itself.";
                continue;
            }
            const auto ac = side(a,b,c), ad = side(a,b,d);
            const auto ca = side(c,d,a), cb = side(c,d,b);
            if ((ac*ad < 0 && ca*cb < 0) || (ac == 0 && onSegment(a,b,c)) || (ad == 0 && onSegment(a,b,d))
                || (ca == 0 && onSegment(c,d,a)) || (cb == 0 && onSegment(c,d,b)))
                return "The profile crosses or touches itself.";
        }
    }
    Mesh candidate;
    const bool closed = std::abs(degrees) == 360;
    const unsigned rings = closed ? steps : steps+1;
    const auto u = (axis+1)%3, v = (axis+2)%3;
    std::vector<std::vector<VertexIndex>> indices(rings,std::vector<VertexIndex>(profile.size()));
    for (unsigned ring = 0; ring < rings; ++ring) {
        const float angle = degrees*std::numbers::pi_v<float>/180*float(ring)/float(steps);
        for (std::size_t i = 0; i < profile.size(); ++i) {
            const auto [radius,height] = profile[i];
            if (ring && radius <= epsilon) {
                indices[ring][i] = indices[0][i];
                continue;
            }
            Point3 p{};
            p[axis] = height;
            p[u] = radius*std::cos(angle);
            p[v] = radius*std::sin(angle);
            indices[ring][i] = VertexIndex(candidate.vertices.size());
            candidate.vertices.push_back(p);
        }
    }
    for (unsigned ring = 0; ring < steps; ++ring) {
        const auto &from = indices[ring];
        const auto &to = indices[(ring+1)%rings];
        for (std::size_t i = 0; i+1 < profile.size(); ++i) {
            const float s = float(ring)/float(steps), t = float(ring+1)/float(steps);
            const float a = length[i]/length.back(), b = length[i+1]/length.back();
            MeshFace face{{{from[i],{s,a}},{to[i],{t,a}},{to[i+1],{t,b}},{from[i+1],{s,b}}}};
            if (from[i] == to[i])
                face.corners.erase(face.corners.begin()+1);
            if (from[i+1] == to[i+1])
                face.corners.pop_back();
            if (degrees < 0)
                std::ranges::reverse(face.corners);
            candidate.faces.push_back(std::move(face));
        }
    }
    return commit(mesh,std::move(candidate));
}

std::string rotateEdge(Mesh &mesh,MeshEdge edge)
{
    edge = MeshEdge(std::minmax(edge.first,edge.second));
    const MeshTopology topology(mesh);
    const auto entry = topology.edgeFaces.find(edge);
    if (entry == topology.edgeFaces.end() || entry->second.size() != 2)
        return "Select an edge between two triangles.";
    const auto i = entry->second[0], j = entry->second[1];
    if (mesh.faces[i].corners.size() != 3 || mesh.faces[j].corners.size() != 3)
        return "Edge Rotate currently requires two triangles.";
    const auto opposite = [&](const MeshFace &face) {
        return std::ranges::find_if(face.corners,[&](const auto &corner) {
            return corner.vertex != edge.first && corner.vertex != edge.second;
        })->vertex;
    };
    const auto a = opposite(mesh.faces[i]), b = opposite(mesh.faces[j]);
    if (a == b || topology.edgeFaces.contains(MeshEdge(std::minmax(a,b))))
        return "Rotating this edge would create an existing edge.";
    auto candidate = mesh;
    const std::array<MeshEdge,1> shared{edge};
    auto error = dissolveEdges(candidate,shared);
    if (!error.empty())
        return error;
    error = divideFace(candidate,i,a,b);
    return error.empty() ? commit(mesh,std::move(candidate)) : error;
}

std::optional<Mesh> makeRetopologyQuad(std::array<Point3,4> positions,Point3 normal)
{
    Mesh mesh;
    mesh.vertices.assign(positions.begin(),positions.end());
    MeshFace face{{{0,{0,0}},{1,{1,0}},{2,{1,1}},{3,{0,1}}}};
    if (dot(faceNormal(mesh,face),normal) < 0)
        std::ranges::reverse(face.corners);
    if (faceTriangles(mesh,face).empty())
        return {};
    mesh.faces.push_back(std::move(face));
    return validateMesh(mesh).empty() ? std::optional{std::move(mesh)} : std::nullopt;
}

std::string extendQuadBoundary(Mesh &mesh,MeshEdge edge,std::array<Point3,2> positions)
{
    edge = MeshEdge(std::minmax(edge.first,edge.second));
    const MeshTopology topology(mesh);
    const auto entry = topology.edgeFaces.find(edge);
    if (entry == topology.edgeFaces.end() || entry->second.size() != 1)
        return "Choose a boundary edge to extend.";
    const auto faceIndex = entry->second.front();
    const auto &sides = topology.faceEdges[faceIndex];
    const auto i = std::size_t(std::ranges::find(sides,edge)-sides.begin());
    const auto &face = mesh.faces[faceIndex];
    const auto a = face.corners[(i+1)%face.corners.size()].vertex, b = face.corners[i].vertex;
    const auto score = [&](Point3 x,Point3 y) {
        return edgeLength(sub(x,mesh.vertices[a]))+edgeLength(sub(y,mesh.vertices[b]));
    };
    if (score(positions[1],positions[0]) < score(positions[0],positions[1]))
        std::swap(positions[0],positions[1]);
    auto candidate = mesh;
    const auto next = VertexIndex(candidate.vertices.size());
    candidate.vertices.insert(candidate.vertices.end(),positions.begin(),positions.end());
    MeshFace quad{{{a,{0,0}},{b,{1,0}},{next+1,{1,1}},{next,{0,1}}},face.material};
    if (faceTriangles(candidate,quad).empty())
        return "Those points would create an invalid quad.";
    candidate.faces.push_back(std::move(quad));
    return commit(mesh,std::move(candidate));
}

std::string relaxVertices(Mesh &mesh,std::span<const VertexIndex> vertices,float strength)
{
    const std::set<VertexIndex> selection(vertices.begin(),vertices.end());
    if (selection.empty() || *selection.rbegin() >= mesh.vertices.size() || !(strength > 0 && strength <= 1))
        return "Select vertices and enter a relaxation strength between 0% and 100%.";
    auto candidate = mesh;
    const MeshTopology topology(mesh);
    bool moved = false;
    for (const auto vertex : selection) {
        const auto &edges = topology.vertexEdges[vertex];
        if (edges.empty() || std::ranges::any_of(edges,[&](auto edge) { return topology.edgeFaces.at(edge).size() != 2; }))
            continue;
        Point3 average{};
        for (const auto &edge : edges)
            average = add(average,mesh.vertices[edge.first == vertex ? edge.second : edge.first]);
        average = mul(average,1.0f/float(edges.size()));
        candidate.vertices[vertex] = add(mul(mesh.vertices[vertex],1-strength),mul(average,strength));
        moved |= candidate.vertices[vertex] != mesh.vertices[vertex];
    }
    if (!moved)
        return "Relax needs an interior vertex with room to move.";
    for (auto &face : candidate.faces)
        if (std::ranges::any_of(face.corners,[&](const auto &c) { return candidate.vertices[c.vertex] != mesh.vertices[c.vertex]; }))
            for (auto &corner : face.corners)
                corner.normal = {};
    return commit(mesh,std::move(candidate));
}

std::string cutQuadLoop(Mesh &mesh,std::size_t index,std::size_t startEdge,unsigned count,float slide,std::set<MeshEdge> *seams)
{
    if (index >= mesh.faces.size() || mesh.faces[index].corners.size() != 4 || startEdge >= 4)
        return "Loop Cut needs a selected quad and one of its edges.";
    if (!count || count > 64 || !std::isfinite(slide) || std::abs(slide) > 0.95f)
        return "Use 1 to 64 cuts and slide between -95% and 95%.";
    const MeshTopology topology(mesh);
    std::map<std::size_t,std::size_t> cuts;
    std::map<MeshEdge,VertexIndex> direction;
    direction.emplace(topology.faceEdges[index][startEdge],mesh.faces[index].corners[startEdge].vertex);
    std::vector<std::pair<std::size_t,std::size_t>> pending{{index,startEdge}};
    while (!pending.empty()) {
        const auto [faceIndex,edge] = pending.back();
        pending.pop_back();
        const auto &face = mesh.faces[faceIndex];
        if (face.corners.size() != 4)
            return "The loop encounters a face that is not a quad.";
        const auto [entry,inserted] = cuts.emplace(faceIndex,edge);
        if (!inserted) {
            if (entry->second%2 != edge%2)
                return "The selected quad loop intersects itself.";
            continue;
        }
        const auto opposite = (edge+2)%4;
        const auto side = topology.faceEdges[faceIndex][edge];
        const auto across = topology.faceEdges[faceIndex][opposite];
        const bool forward = direction.at(side) == face.corners[edge].vertex;
        const auto acrossStart = face.corners[(edge+(forward ? 3 : 2))%4].vertex;
        const auto [existing,added] = direction.emplace(across,acrossStart);
        if (!added && existing->second != acrossStart)
            return "The loop changes orientation around a twisted surface.";
        for (auto key : {side,across}) {
            const auto &neighbors = topology.edgeFaces.at(key);
            if (neighbors.size() > 2)
                return "The loop encounters a non-manifold edge.";
            for (const auto neighbor : neighbors) {
                if (neighbor == faceIndex)
                    continue;
                const auto &edges = topology.faceEdges[neighbor];
                pending.emplace_back(neighbor,std::size_t(std::ranges::find(edges,key)-edges.begin()));
            }
        }
    }
    if (mesh.faces.size()+cuts.size()*count > 2'000'000 || mesh.vertices.size()+direction.size()*count > 2'000'000)
        return "This loop cut exceeds the editor's mesh limit.";
    auto candidate = mesh;
    std::map<std::pair<MeshEdge,unsigned>,VertexIndex> points;
    for (const auto &[faceIndex,edge] : cuts) {
        const auto &face = mesh.faces[faceIndex];
        const auto &a = face.corners[edge], &b = face.corners[(edge+1)%4];
        const auto &c = face.corners[(edge+2)%4], &d = face.corners[(edge+3)%4];
        const auto sample = [&](const MeshCorner &from,const MeshCorner &to,MeshEdge key,unsigned step) {
            const bool forward = direction.at(key) == from.vertex;
            const auto point = forward ? step : count-1-step;
            float t = float(point+1)/float(count+1);
            t += slide*(slide < 0 ? t : 1-t);
            if (!forward)
                t = 1-t;
            const auto [entry,inserted] = points.emplace(std::pair{key,point},VertexIndex(candidate.vertices.size()));
            if (inserted)
                candidate.vertices.push_back(add(mul(mesh.vertices[from.vertex],1-t),mul(mesh.vertices[to.vertex],t)));
            return MeshCorner{entry->second,{from.uv[0]*(1-t)+to.uv[0]*t,from.uv[1]*(1-t)+to.uv[1]*t},
                unit(add(mul(from.normal,1-t),mul(to.normal,t)))};
        };
        MeshCorner left = a, right = d;
        for (unsigned step = 0; step <= count; ++step) {
            const auto nextLeft = step == count ? b : sample(a,b,topology.faceEdges[faceIndex][edge],step);
            const auto nextRight = step == count ? c : sample(d,c,topology.faceEdges[faceIndex][(edge+2)%4],step);
            MeshFace strip{{left,nextLeft,nextRight,right},face.material};
            if (step == 0)
                candidate.faces[faceIndex] = std::move(strip);
            else
                candidate.faces.push_back(std::move(strip));
            left = nextLeft;
            right = nextRight;
        }
    }
    const auto error = commit(mesh,std::move(candidate));
    if (error.empty() && seams) {
        for (const auto &[edge,first] : direction) {
            if (!seams->erase(edge))
                continue;
            auto previous = first;
            for (unsigned step = 0; step < count; ++step) {
                const auto vertex = points.at({edge,step});
                seams->insert(std::minmax(previous,vertex));
                previous = vertex;
            }
            seams->insert(std::minmax(previous,first == edge.first ? edge.second : edge.first));
        }
    }
    return error;
}


std::string mirrorMeshX(Mesh &mesh,MirrorSide source)
{
    const auto includesSide = [](const Mesh &part,const MeshFace &face,float side) {
        return std::ranges::any_of(face.corners,[&](const auto &corner) {
            return side*part.vertices[corner.vertex][0] > epsilon;
        });
    };
    const auto containsSide = [&](float side) {
        return std::ranges::any_of(mesh.faces,[&](const auto &face) { return includesSide(mesh,face,side); });
    };
    const bool positive = containsSide(1), negative = containsSide(-1);
    if ((!positive && !negative) || (source == MirrorSide::Positive && !positive)
        || (source == MirrorSide::Negative && !negative))
        return "Mirror requires faces on the chosen source side of the plane.";
    const float keep = source == MirrorSide::Negative || (source == MirrorSide::Automatic && !positive) ? -1.0f : 1.0f;
    auto candidate = mesh;
    if (positive && negative) {
        std::vector<std::size_t> crossed;
        for (std::size_t i = 0; i < mesh.faces.size(); ++i)
            if (includesSide(mesh,mesh.faces[i],1) && includesSide(mesh,mesh.faces[i],-1))
                crossed.push_back(i);
        if (!crossed.empty()) {
            auto error = bisectMesh(candidate,0,0);
            if (!error.empty()) {
                error = triangulateFaces(candidate,crossed);
                if (!error.empty())
                    return error;
                error = bisectMesh(candidate,0,0);
                if (!error.empty())
                    return error;
            }
        }
    }
    std::erase_if(candidate.faces,[&](const auto &face) { return !includesSide(candidate,face,keep); });
    removeUnusedVertices(candidate);
    if (candidate.faces.size() > 1'000'000 || candidate.vertices.size() > 2'000'000)
        return "Mirror exceeds the editor's geometry limit.";
    const auto vertexCount = VertexIndex(candidate.vertices.size());
    const auto faceCount = candidate.faces.size();
    std::vector<VertexIndex> reflected;
    reflected.reserve(vertexCount);
    for (VertexIndex i = 0; i < vertexCount; ++i) {
        auto p = candidate.vertices[i];
        if (std::abs(p[0]) <= epsilon) {
            reflected.push_back(i);
            candidate.vertices[i][0] = 0;
        } else {
            reflected.push_back(VertexIndex(candidate.vertices.size()));
            p[0] = -p[0];
            candidate.vertices.push_back(p);
        }
    }
    for (std::size_t i = 0; i < faceCount; ++i) {
        auto copy = candidate.faces[i];
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
