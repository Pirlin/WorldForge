#include "Engine/Geometry/MeshBoolean.h"

#include <manifold/manifold.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace worldforge {
namespace {

constexpr std::size_t geometryLimit = 2'000'000;
using BooleanMesh = manifold::MeshGL64;

std::string toManifoldMesh(const Mesh &source,Point3 offset,std::uint32_t firstId,BooleanMesh &output)
{
    output.numProp = 8;
    std::vector<std::vector<std::size_t>> materials(source.materials.size());
    for (std::size_t i = 0; i < source.faces.size(); ++i)
        materials[source.faces[i].material].push_back(i);
    const auto unused = std::uint64_t(-1);
    std::vector<std::uint64_t> firstVertex(source.vertices.size(),unused);
    for (std::size_t material = 0; material < materials.size(); ++material) {
        if (materials[material].empty())
            continue;
        output.runIndex.push_back(output.triVerts.size());
        output.runOriginalID.push_back(firstId+std::uint32_t(material));
        for (const auto index : materials[material]) {
            const auto &face = source.faces[index];
            const auto base = output.vertProperties.size()/output.numProp;
            const auto triangles = faceTriangles(source,face);
            if (triangles.empty())
                return "Boolean could not triangulate an input face.";
            if (base+face.corners.size() > 3*geometryLimit || output.triVerts.size()/3+triangles.size() > geometryLimit)
                return "Boolean input exceeds the editor's triangulated mesh limit.";
            const auto normal = faceNormal(source,face);
            for (const auto &corner : face.corners) {
                const auto p = source.vertices[corner.vertex];
                const auto n = corner.normal == Point3{} ? normal : corner.normal;
                const auto vertex = output.vertProperties.size()/output.numProp;
                auto &first = firstVertex[corner.vertex];
                if (first == unused) {
                    first = vertex;
                } else {
                    output.mergeFromVert.push_back(vertex);
                    output.mergeToVert.push_back(first);
                }
                output.vertProperties.insert(output.vertProperties.end(),{double(p[0])+offset[0],
                    double(p[1])+offset[1],double(p[2])+offset[2],n[0],n[1],n[2],corner.uv[0],corner.uv[1]});
            }
            for (const auto &triangle : triangles)
                for (const auto corner : triangle)
                    output.triVerts.push_back(base+corner);
        }
    }
    output.runIndex.push_back(output.triVerts.size());
    return {};
}

std::string fromManifoldMesh(const BooleanMesh &source,std::uint32_t firstId,Mesh &mesh)
{
    const auto count = source.vertProperties.size()/source.numProp;
    if (source.triVerts.size()/3 > geometryLimit)
        return "Boolean output exceeds the editor's triangle limit.";
    std::vector<std::size_t> parent(count);
    std::iota(parent.begin(),parent.end(),std::size_t(0));
    const auto root = [&](std::size_t index) {
        while (parent[index] != index) {
            parent[index] = parent[parent[index]];
            index = parent[index];
        }
        return index;
    };
    for (std::size_t i = 0; i < source.mergeFromVert.size(); ++i)
        parent[root(source.mergeFromVert[i])] = root(source.mergeToVert[i]);
    const auto unused = VertexIndex(-1);
    std::vector<VertexIndex> ids(count,unused);
    for (std::size_t run = 0; run < source.runOriginalID.size(); ++run) {
        const auto material = source.runOriginalID[run]-firstId;
        if (material >= mesh.materials.size())
            return "Boolean output lost its source material mapping.";
        for (auto i = source.runIndex[run]; i < source.runIndex[run+1]; i += 3) {
            MeshFace face{{},material};
            for (unsigned j = 0; j < 3; ++j) {
                const auto property = source.triVerts[i+j];
                const auto start = property*source.numProp;
                const auto p = [&](unsigned channel) { return float(source.vertProperties[start+channel]); };
                auto &index = ids[root(property)];
                if (index == unused) {
                    index = VertexIndex(mesh.vertices.size());
                    mesh.vertices.push_back({p(0),p(1),p(2)});
                }
                face.corners.push_back({index,{p(6),p(7)},{p(3),p(4),p(5)}});
            }
            mesh.faces.push_back(std::move(face));
        }
    }
    return validateMesh(mesh);
}

}

std::string booleanMesh(Mesh &mesh,const Mesh &target,BooleanOperation operation,Point3 offset)
{
    if (!std::ranges::all_of(offset,[](float value) { return std::isfinite(value); }))
        return "Enter a finite Boolean cutter offset.";
    if (!validateMesh(mesh).empty() || !validateMesh(target).empty())
        return "Boolean requires valid input meshes.";
    const auto firstId = manifold::Manifold::ReserveIDs(std::uint32_t(mesh.materials.size()+target.materials.size()));
    BooleanMesh sourceGeometry, targetGeometry;
    auto error = toManifoldMesh(mesh,{},firstId,sourceGeometry);
    if (error.empty())
        error = toManifoldMesh(target,offset,firstId+std::uint32_t(mesh.materials.size()),targetGeometry);
    if (!error.empty())
        return error;
    const manifold::Manifold a(sourceGeometry), b(targetGeometry);
    if (a.Status() != manifold::Manifold::Error::NoError || b.Status() != manifold::Manifold::Error::NoError
        || !(a.Volume() > 0) || !(b.Volume() > 0))
        return "Boolean requires two closed solids with consistently outward-facing polygons.";
    manifold::Manifold combined;
    switch (operation) {
    case BooleanOperation::Difference: combined = a-b; break;
    case BooleanOperation::Union: combined = a+b; break;
    case BooleanOperation::Intersection: combined = a^b; break;
    default: return "Choose a valid Boolean operation.";
    }
    if (combined.Status() != manifold::Manifold::Error::NoError || combined.IsEmpty())
        return "Boolean produced no valid solid. Adjust the operands or choose another operation.";
    Mesh result;
    result.materials = mesh.materials;
    result.materials.insert(result.materials.end(),target.materials.begin(),target.materials.end());
    error = fromManifoldMesh(combined.GetMeshGL64(0),firstId,result);
    if (error.empty())
        mesh = std::move(result);
    return error;
}

}
