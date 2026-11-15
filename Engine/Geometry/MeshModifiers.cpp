#include "Engine/Geometry/MeshModifiers.h"
#include "Engine/Geometry/MeshTopology.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>

namespace worldforge {
namespace {

Point3 add(Point3 a,Point3 b) { return {a[0]+b[0],a[1]+b[1],a[2]+b[2]}; }
Point3 scale(Point3 p,float amount) { return {p[0]*amount,p[1]*amount,p[2]*amount}; }
Point3 difference(Point3 a,Point3 b) { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; }
float dot(Point3 a,Point3 b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }

Point3 normal(Point3 p)
{
    const auto length = std::hypot(p[0],p[1],p[2]);
    return length > 0 && std::isfinite(length) ? scale(p,1/length) : Point3{};
}

Point3 faceAreaNormal(const Mesh &mesh,const MeshFace &face)
{
    Point3 sum{};
    const auto origin = mesh.vertices[face.corners.front().vertex];
    for (std::size_t i = 1; i+1 < face.corners.size(); ++i) {
        const auto a = add(mesh.vertices[face.corners[i].vertex],scale(origin,-1));
        const auto b = add(mesh.vertices[face.corners[i+1].vertex],scale(origin,-1));
        sum = add(sum,{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]});
    }
    return sum;
}

std::vector<Point3> areaNormals(const Mesh &mesh)
{
    std::vector<Point3> normals(mesh.vertices.size());
    for (const auto &face : mesh.faces) {
        const auto direction = faceAreaNormal(mesh,face);
        for (const auto &corner : face.corners)
            normals[corner.vertex] = add(normals[corner.vertex],direction);
    }
    for (auto &value : normals)
        value = normal(value);
    return normals;
}

std::string replace(Mesh &mesh,Mesh candidate)
{
    auto error = validateMesh(candidate);
    if (error.empty())
        mesh = std::move(candidate);
    return error;
}

std::string subdivision(Mesh &mesh,unsigned levels)
{
    if (!levels || levels > 3)
        return "Subdivision supports one to three levels.";
    auto output = mesh;
    for (unsigned level = 0; level < levels; ++level) {
        const auto &source = output;
        const MeshTopology topology(source);
        std::size_t corners = 0;
        for (const auto &face : source.faces)
            corners += face.corners.size();
        if (corners > 2'000'000 || source.vertices.size()+topology.edgeFaces.size()+source.faces.size() > 2'000'000)
            return "Subdivision exceeds the editor's mesh limit.";
        Mesh next;
        next.materials = source.materials;
        next.vertices = source.vertices;
        std::vector<MeshCorner> centers;
        std::vector<std::vector<std::size_t>> incident(source.vertices.size());
        for (std::size_t i = 0; i < source.faces.size(); ++i) {
            const auto &face = source.faces[i];
            Point3 center{};
            MeshCorner attribute{VertexIndex(next.vertices.size())};
            const auto n = float(face.corners.size());
            for (const auto &corner : face.corners) {
                center = add(center,scale(source.vertices[corner.vertex],1/n));
                attribute.uv[0] += corner.uv[0]/n;
                attribute.uv[1] += corner.uv[1]/n;
                incident[corner.vertex].push_back(i);
            }
            centers.push_back(attribute);
            next.vertices.push_back(center);
        }
        std::map<MeshEdge,VertexIndex> edgePoints;
        for (const auto &[edge,faces] : topology.edgeFaces) {
            if (faces.size() > 2)
                return "Subdivision requires manifold edges.";
            auto point = add(source.vertices[edge.first],source.vertices[edge.second]);
            if (faces.size() == 2)
                for (const auto i : faces)
                    point = add(point,next.vertices[centers[i].vertex]);
            edgePoints[edge] = VertexIndex(next.vertices.size());
            next.vertices.push_back(scale(point,faces.size() == 2 ? 0.25f : 0.5f));
        }
        for (std::size_t i = 0; i < source.vertices.size(); ++i) {
            const auto &edges = topology.vertexEdges[i];
            if (edges.empty())
                continue;
            Point3 edgeSum{}, faceSum{}, border{};
            int boundaryCount = 0;
            for (const auto &edge : edges) {
                const auto other = source.vertices[edge.first == i ? edge.second : edge.first];
                edgeSum = add(edgeSum,scale(add(source.vertices[i],other),0.5f));
                if (topology.edgeFaces.at(edge).size() == 1) {
                    border = add(border,other);
                    ++boundaryCount;
                }
            }
            if (boundaryCount == 2) {
                next.vertices[i] = add(scale(source.vertices[i],0.75f),scale(border,0.125f));
            } else if (boundaryCount == 0 && edges.size() == incident[i].size()) {
                for (const auto face : incident[i])
                    faceSum = add(faceSum,next.vertices[centers[face].vertex]);
                const auto n = float(edges.size());
                next.vertices[i] = scale(add(add(scale(faceSum,1/n),scale(edgeSum,2/n)),scale(source.vertices[i],n-3)),1/n);
            } else {
                return "Subdivision found a vertex with an irregular boundary.";
            }
        }
        for (std::size_t i = 0; i < source.faces.size(); ++i) {
            const auto &face = source.faces[i];
            const auto n = face.corners.size();
            const auto midpoint = [&](std::size_t corner) {
                const auto &a = face.corners[corner], &b = face.corners[(corner+1)%n];
                return MeshCorner{edgePoints.at(topology.faceEdges[i][corner]),{(a.uv[0]+b.uv[0])*0.5f,(a.uv[1]+b.uv[1])*0.5f}};
            };
            for (std::size_t j = 0; j < n; ++j)
                next.faces.push_back({{{face.corners[j].vertex,face.corners[j].uv},midpoint(j),centers[i],midpoint((j+n-1)%n)},face.material});
        }
        const auto normals = areaNormals(next);
        for (auto &face : next.faces)
            for (auto &corner : face.corners)
                corner.normal = normals[corner.vertex];
        const auto error = replace(output,std::move(next));
        if (!error.empty())
            return error;
    }
    mesh = std::move(output);
    return {};
}

std::string solidify(Mesh &mesh,float thickness)
{
    if (!(thickness > 0) || !std::isfinite(thickness))
        return "Enter a positive solidify thickness.";
    const MeshTopology topology(mesh);
    const auto borders = std::ranges::count_if(topology.edgeFaces,[](const auto &entry) { return entry.second.size() == 1; });
    if (mesh.vertices.size()*2 > 2'000'000 || mesh.faces.size()*2+std::size_t(borders) > 2'000'000)
        return "Solidify exceeds the editor's mesh limit.";
    if (std::ranges::any_of(topology.edgeFaces,[](const auto &entry) { return entry.second.size() > 2; }))
        return "Solidify requires manifold edges.";
    const auto normals = areaNormals(mesh);
    auto result = mesh;
    const auto back = VertexIndex(mesh.vertices.size());
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i)
        result.vertices.push_back(add(mesh.vertices[i],scale(normals[i],-thickness)));
    for (std::size_t i = 0; i < mesh.faces.size(); ++i) {
        const auto &face = mesh.faces[i];
        auto reversed = face;
        std::ranges::reverse(reversed.corners);
        for (auto &corner : reversed.corners) {
            corner.vertex += back;
            corner.normal = scale(corner.normal,-1);
        }
        result.faces.push_back(std::move(reversed));
        for (std::size_t j = 0; j < face.corners.size(); ++j) {
            if (topology.edgeFaces.at(topology.faceEdges[i][j]).size() != 1)
                continue;
            const auto a = face.corners[j].vertex, b = face.corners[(j+1)%face.corners.size()].vertex;
            result.faces.push_back({{{b,{0,0}},{a,{1,0}},{a+back,{1,1}},{b+back,{0,1}}},face.material});
        }
    }
    return replace(mesh,std::move(result));
}

std::string decimate(Mesh &mesh,float cellSize)
{
    if (!(cellSize > 0) || !std::isfinite(cellSize))
        return "Enter a positive decimation cell size.";
    using Cell = std::array<double,3>;
    struct Cluster { std::array<double,3> sum{}; std::size_t size = 0; };
    std::map<Cell,VertexIndex> cells;
    std::vector<VertexIndex> mapped;
    std::vector<Cluster> clusters;
    mapped.reserve(mesh.vertices.size());
    for (const auto &p : mesh.vertices) {
        const Cell cell{std::floor(double(p[0])/cellSize),std::floor(double(p[1])/cellSize),
                        std::floor(double(p[2])/cellSize)};
        const auto [entry,added] = cells.try_emplace(cell,VertexIndex(clusters.size()));
        if (added)
            clusters.emplace_back();
        mapped.push_back(entry->second);
        auto &cluster = clusters[entry->second];
        for (int i = 0; i < 3; ++i)
            cluster.sum[i] += p[i];
        ++cluster.size;
    }
    if (clusters.size() == mesh.vertices.size())
        return {};
    Mesh result;
    result.materials = mesh.materials;
    for (const auto &cluster : clusters) {
        const auto count = double(cluster.size);
        result.vertices.push_back({float(cluster.sum[0]/count),float(cluster.sum[1]/count),float(cluster.sum[2]/count)});
    }
    std::set<std::pair<std::array<VertexIndex,3>,std::uint32_t>> seen;
    for (const auto &face : mesh.faces) {
        visitFaceTriangles(mesh,face,[&](const auto &triangle) {
            MeshFace part{{},face.material};
            std::array<VertexIndex,3> key;
            for (int i = 0; i < 3; ++i) {
                auto corner = face.corners[triangle[i]];
                corner.vertex = mapped[corner.vertex];
                corner.normal = {};
                key[i] = corner.vertex;
                part.corners.push_back(corner);
            }
            std::ranges::sort(key);
            if (key[0] == key[1] || key[1] == key[2] || faceNormal(result,part) == Point3{}
                || !seen.emplace(key,face.material).second)
                return;
            result.faces.push_back(std::move(part));
        });
    }
    if (result.faces.empty())
        return "The cell size removes every face. Choose a smaller size.";
    std::vector<bool> used(result.vertices.size());
    for (const auto &face : result.faces)
        for (const auto &corner : face.corners)
            used[corner.vertex] = true;
    std::vector<Point3> vertices;
    for (std::size_t i = 0; i < used.size(); ++i)
        if (used[i]) {
            mapped[i] = VertexIndex(vertices.size());
            vertices.push_back(result.vertices[i]);
        }
    for (auto &face : result.faces)
        for (auto &corner : face.corners)
            corner.vertex = mapped[corner.vertex];
    result.vertices = std::move(vertices);
    return replace(mesh,std::move(result));
}

}

namespace {
Point3 closestPoint(Point3 p,Point3 a,Point3 b,Point3 c)
{
    const auto ab = difference(b,a), ac = difference(c,a), ap = difference(p,a);
    const float d1 = dot(ab,ap), d2 = dot(ac,ap);
    if (d1 <= 0 && d2 <= 0)
        return a;
    const auto bp = difference(p,b);
    const float d3 = dot(ab,bp), d4 = dot(ac,bp);
    if (d3 >= 0 && d4 <= d3)
        return b;
    const float vc = d1*d4-d3*d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0)
        return add(a,scale(ab,d1/(d1-d3)));
    const auto cp = difference(p,c);
    const float d5 = dot(ab,cp), d6 = dot(ac,cp);
    if (d6 >= 0 && d5 <= d6)
        return c;
    const float vb = d5*d2-d1*d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0)
        return add(a,scale(ac,d2/(d2-d6)));
    const float va = d3*d6-d5*d4;
    if (va <= 0 && d4 >= d3 && d5 >= d6)
        return add(b,scale(difference(c,b),(d4-d3)/(d4-d3+d5-d6)));
    const float denominator = va+vb+vc;
    return denominator > 0 ? add(a,add(scale(ab,vb/denominator),scale(ac,vc/denominator))) : a;
}

std::string shrinkwrap(Mesh &mesh,const Mesh *target,float offset)
{
    if (!target || !std::isfinite(offset))
        return "Choose an existing target mesh asset and a finite surface offset.";
    struct Triangle { std::array<Point3,3> points; Point3 normal; };
    std::vector<Triangle> triangles;
    for (const auto &face : target->faces) {
        const auto direction = faceNormal(*target,face);
        visitFaceTriangles(*target,face,[&](const auto &indices) {
            triangles.push_back({{target->vertices[face.corners[indices[0]].vertex],
                target->vertices[face.corners[indices[1]].vertex],target->vertices[face.corners[indices[2]].vertex]},direction});
        });
    }
    if (triangles.empty() || std::uint64_t(triangles.size())*mesh.vertices.size() > 32'000'000)
        return "Shrinkwrap target is empty or exceeds the supported projection budget.";
    auto result = mesh;
    for (auto &p : result.vertices) {
        auto closest = p;
        float nearest = std::numeric_limits<float>::infinity();
        for (const auto &triangle : triangles) {
            const auto &v = triangle.points;
            const auto hit = closestPoint(p,v[0],v[1],v[2]);
            const auto delta = difference(hit,p);
            const auto distance = dot(delta,delta);
            if (distance < nearest) {
                nearest = distance;
                closest = add(hit,scale(triangle.normal,offset));
            }
        }
        p = closest;
    }
    for (auto &face : result.faces)
        for (auto &corner : face.corners)
            corner.normal = {};
    return replace(mesh,std::move(result));
}
}

std::string applyMeshModifier(Mesh &mesh,const MeshModifier &modifier,const Mesh *target)
{
    if (!modifier.enabled)
        return {};
    switch (modifier.kind) {
    case ModifierKind::Mirror: {
        if (modifier.axis > 2)
            return "Choose a valid mirror axis.";
        auto result = mesh;
        const auto swapAxis = [&](Mesh &part) {
            if (modifier.axis == 0)
                return;
            for (auto &point : part.vertices)
                std::swap(point[0],point[modifier.axis]);
            for (auto &face : part.faces)
                for (auto &corner : face.corners)
                    std::swap(corner.normal[0],corner.normal[modifier.axis]);
        };
        swapAxis(result);
        const auto error = mirrorMeshX(result,modifier.mirrorSide);
        if (!error.empty())
            return error;
        swapAxis(result);
        return replace(mesh,std::move(result));
    }
    case ModifierKind::PerimeterBevel:
        return bevelFace(mesh,modifier.face,modifier.amount);
    case ModifierKind::Subdivision:
        return subdivision(mesh,modifier.count);
    case ModifierKind::Array: {
        if (modifier.axis > 2 || modifier.count < 2 || modifier.count > 100 || !std::isfinite(modifier.amount)
            || mesh.vertices.size()*modifier.count > 2'000'000 || mesh.faces.size()*modifier.count > 2'000'000)
            return "Invalid array settings or geometry limit exceeded.";
        auto result = mesh;
        for (unsigned i = 1; i < modifier.count; ++i) {
            const auto base = VertexIndex(result.vertices.size());
            for (auto vertex : mesh.vertices) {
                vertex[modifier.axis] += modifier.amount*float(i);
                result.vertices.push_back(vertex);
            }
            for (auto face : mesh.faces) {
                for (auto &corner : face.corners)
                    corner.vertex += base;
                result.faces.push_back(std::move(face));
            }
        }
        return replace(mesh,std::move(result));
    }
    case ModifierKind::Solidify:
        return solidify(mesh,modifier.amount);
    case ModifierKind::Weld: {
        std::vector<VertexIndex> vertices(mesh.vertices.size());
        std::iota(vertices.begin(),vertices.end(),VertexIndex(0));
        return weldVertices(mesh,vertices,modifier.amount,{},true);
    }
    case ModifierKind::WeightedNormal: {
        const auto normals = areaNormals(mesh);
        for (auto &face : mesh.faces)
            for (auto &corner : face.corners)
                corner.normal = normals[corner.vertex];
        return {};
    }
    case ModifierKind::Decimate:
        return decimate(mesh,modifier.amount);
    case ModifierKind::Shrinkwrap:
        return shrinkwrap(mesh,target,modifier.amount);
    case ModifierKind::Boolean:
        return target ? booleanMesh(mesh,*target,modifier.booleanOperation,modifier.targetOffset)
            : "Choose an existing mesh asset as the Boolean cutter.";
    }
    return "Unknown mesh modifier.";
}
}
