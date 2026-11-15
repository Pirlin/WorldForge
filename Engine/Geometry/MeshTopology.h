#pragma once

#include "Engine/Geometry/Mesh.h"

#include <map>
#include <utility>

namespace worldforge {

struct MeshTopology {
    explicit MeshTopology(const Mesh &mesh);

    std::map<MeshEdge, std::vector<std::size_t>> edgeFaces;
    std::vector<std::vector<MeshEdge>> vertexEdges;
    std::vector<std::vector<MeshEdge>> faceEdges;
};

std::vector<float> connectedFalloff(const Mesh &mesh,const std::vector<VertexIndex> &seeds,float radius);
std::vector<VertexIndex> mirroredVerticesX(const Mesh &mesh,float tolerance);
inline constexpr auto MissingVertex = VertexIndex(-1);

}
