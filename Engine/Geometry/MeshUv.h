#pragma once

#include "Engine/Geometry/Mesh.h"

#include <set>

namespace worldforge {

using UvIsland = std::vector<std::size_t>;
using UvCorner = std::pair<std::size_t,std::size_t>;
using UvPins = std::set<UvCorner>;
enum class UvProjection { Planar, Box, Cylinder };

std::vector<UvIsland> uvIslands(const Mesh &mesh,const std::set<MeshEdge> &seams,bool useExistingUvs);
std::set<MeshEdge> sharpUvSeams(const Mesh &mesh);
std::string unwrapUvIslands(Mesh &mesh,const std::set<MeshEdge> &seams,float padding,const UvPins &pins = {});
std::string packUvIslands(Mesh &mesh,const std::set<MeshEdge> &seams,float padding,const UvPins &pins = {});
std::string projectUvFaces(Mesh &mesh,std::span<const std::size_t> faces,UvProjection projection);
std::string stitchUvEdges(Mesh &mesh,std::span<const MeshEdge> edges,const std::set<MeshEdge> &seams = {});
std::string normalizeUvDensity(Mesh &mesh,const std::set<MeshEdge> &seams,float padding,const UvPins &pins = {});

struct UvReport {
    std::size_t islands = 0;
    std::size_t outside = 0;
    std::size_t degenerate = 0;
    std::size_t overlaps = 0;
};
UvReport inspectUvs(const Mesh &mesh,const std::set<MeshEdge> &seams);

}
