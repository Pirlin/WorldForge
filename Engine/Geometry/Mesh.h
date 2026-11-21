#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace worldforge {

using Point3 = std::array<float, 3>;
using Point2 = std::array<float, 2>;
using VertexIndex = std::uint32_t;
using MeshEdge = std::pair<VertexIndex,VertexIndex>;
struct MeshCutPoint {
    MeshEdge edge;
    float fraction = 0;
};

struct MeshCorner {
    VertexIndex vertex;
    Point2 uv{};
    Point3 normal{};
    bool operator==(const MeshCorner &) const = default;
};

struct MeshFace {
    std::vector<MeshCorner> corners;
    std::uint32_t material = 0;
    bool operator==(const MeshFace &) const = default;
};

struct MeshMaterial {
    std::string name = "Default";
    std::array<float, 4> color{0.55f, 0.61f, 0.7f, 1.0f};
    float metallic = 0.0f;
    float roughness = 0.65f;
    static constexpr std::size_t maxTextureBytes = 16 * 1024 * 1024;
    std::vector<std::uint8_t> baseColorTexture;
    std::vector<std::uint8_t> normalTexture, metallicRoughnessTexture, occlusionTexture, emissiveTexture;
    Point3 emissive{};
    std::string sharedMaterialId;
    bool hasAdvancedSurface() const {
        return metallic != 0 || roughness != 0.65f || emissive != Point3{}
            || !normalTexture.empty() || !metallicRoughnessTexture.empty()
            || !occlusionTexture.empty() || !emissiveTexture.empty();
    }
    auto previewTextures() const {
        return std::array{&baseColorTexture,&normalTexture,&metallicRoughnessTexture,&occlusionTexture,&emissiveTexture};
    }
    auto previewTextures() {
        return std::array{&baseColorTexture,&normalTexture,&metallicRoughnessTexture,&occlusionTexture,&emissiveTexture};
    }
    bool operator==(const MeshMaterial &) const = default;
};

struct Mesh {
    std::vector<Point3> vertices;
    std::vector<MeshFace> faces;
    std::vector<MeshMaterial> materials{{}};
    bool operator==(const Mesh &) const = default;
};

Mesh makeCube();
enum class Primitive { Cube, Plane, Sphere, Cylinder, Cone, Torus, Circle };
std::optional<Mesh> makePrimitive(Primitive primitive, Point3 size, unsigned segments = 24, unsigned rings = 12);
Point3 faceNormal(const Mesh &mesh, const MeshFace &face);
std::string validateMesh(const Mesh &mesh);
std::string extrudeFace(Mesh &mesh, std::size_t face, float distance);
std::string extrudeFaces(Mesh &mesh,std::span<const std::size_t> faces,float distance,bool individual);
std::string insetFaces(Mesh &mesh,std::span<const std::size_t> faces,float width);
std::string bevelFace(Mesh &mesh,std::size_t face,float width,unsigned segments = 1);
std::string bevelEdges(Mesh &mesh,std::span<const MeshEdge> edges,float width,unsigned segments);
std::string bevelVertices(Mesh &mesh,std::span<const VertexIndex> vertices,float width);
std::string slideVertex(Mesh &mesh,VertexIndex vertex,VertexIndex toward,float factor);
std::string slideEdge(Mesh &mesh,MeshEdge edge,float factor);
std::string splitFaces(Mesh &mesh,std::span<const std::size_t> faces);
std::string knifeFace(Mesh &mesh,std::size_t face,MeshCutPoint start,MeshCutPoint end);
struct MeshKnifeSegment {
    std::size_t face;
    MeshCutPoint start, end;
};
std::string knifePath(Mesh &mesh,std::span<const MeshKnifeSegment> path,std::set<MeshEdge> *seams = nullptr);
std::string bisectMesh(Mesh &mesh,unsigned axis,float coordinate);
std::string rotateEdge(Mesh &mesh,MeshEdge edge);
std::string spinEdges(Mesh &mesh,std::span<const MeshEdge> profile,unsigned axis,Point3 center,float degrees,unsigned steps);
std::string revolveProfile(Mesh &mesh,std::span<const Point2> profile,unsigned axis,float degrees,unsigned steps);
std::optional<std::vector<Point2>> sampleNurbsProfile(std::span<const Point2> points,
    std::span<const float> weights,unsigned degree,unsigned segments);
std::optional<Mesh> makeRetopologyQuad(std::array<Point3,4> positions,Point3 normal);
std::string extendQuadBoundary(Mesh &mesh,MeshEdge edge,std::array<Point3,2> positions);
std::string relaxVertices(Mesh &mesh,std::span<const VertexIndex> vertices,float strength);
std::string cutQuadLoop(Mesh &mesh,std::size_t face,std::size_t edge,unsigned count = 1,float slide = 0,
    std::set<MeshEdge> *seams = nullptr);
std::vector<std::array<std::size_t,3>> faceTriangles(const Mesh &mesh,const MeshFace &face);
template<class Visitor>
void visitFaceTriangles(const Mesh &mesh,const MeshFace &face,Visitor visit)
{
    if (face.corners.size() == 3) {
        visit(std::array<std::size_t,3>{0,1,2});
        return;
    }
    for (const auto &triangle : faceTriangles(mesh,face))
        visit(triangle);
}
std::string triangulateFaces(Mesh &mesh,std::span<const std::size_t> faces);
std::string subdivideFaces(Mesh &mesh,std::span<const std::size_t> faces);
std::string deleteFaces(Mesh &mesh,std::span<const std::size_t> faces);
std::string weldVertices(Mesh &mesh,std::span<const VertexIndex> vertices,float distance,
                         std::optional<VertexIndex> target = {},bool allowNoChange = false);
std::string weldBoundaryEdges(Mesh &mesh,MeshEdge source,MeshEdge target);
std::string connectVertices(Mesh &mesh,VertexIndex start,VertexIndex end);
std::string connectEdges(Mesh &mesh,MeshEdge start,MeshEdge end,std::set<MeshEdge> *seams = nullptr);
std::string fillBoundary(Mesh &mesh,std::span<const MeshEdge> edges);
std::string gridFillBoundary(Mesh &mesh,std::span<const MeshEdge> edges,unsigned columns);
std::string bridgeBoundaries(Mesh &mesh,std::span<const MeshEdge> edges);
std::string dissolveEdges(Mesh &mesh,std::span<const MeshEdge> edges,std::optional<float> maxAngleDegrees = {});
std::string setFaceShading(Mesh &mesh,std::span<const std::size_t> faces,bool smooth);
enum class MirrorSide { Automatic, Positive, Negative };
std::string mirrorMeshX(Mesh &mesh,MirrorSide source = MirrorSide::Automatic);
std::string unwrapFace(Mesh &mesh, std::size_t face);
std::string translateFaceUV(Mesh &mesh, std::size_t face, Point2 delta);

}
