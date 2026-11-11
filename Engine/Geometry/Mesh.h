#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace worldforge {

using Point3 = std::array<float, 3>;
using Point2 = std::array<float, 2>;
using VertexIndex = std::uint32_t;

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
    bool operator==(const MeshMaterial &) const = default;
};

struct Mesh {
    std::vector<Point3> vertices;
    std::vector<MeshFace> faces;
    std::vector<MeshMaterial> materials{{}};
    bool operator==(const Mesh &) const = default;
};

Mesh makeCube();
Point3 faceNormal(const Mesh &mesh, const MeshFace &face);
std::string validateMesh(const Mesh &mesh);
std::string extrudeFace(Mesh &mesh, std::size_t face, float distance);
std::string bevelFace(Mesh &mesh, std::size_t face, float width);
std::string cutQuadLoop(Mesh &mesh, std::size_t face, std::size_t edge);
std::string mirrorMeshX(Mesh &mesh);
std::string unwrapFace(Mesh &mesh, std::size_t face);
std::string translateFaceUV(Mesh &mesh, std::size_t face, Point2 delta);

}
