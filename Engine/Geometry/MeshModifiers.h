#pragma once

#include "Engine/Geometry/Mesh.h"
#include "Engine/Geometry/MeshBoolean.h"

namespace worldforge {

enum class ModifierKind { Mirror, PerimeterBevel, Subdivision, Array, Solidify, Weld, WeightedNormal, Decimate, Shrinkwrap, Boolean };

struct MeshModifier {
    ModifierKind kind = ModifierKind::Mirror;
    bool enabled = true;
    unsigned axis = 0;
    unsigned count = 2;
    float amount = 0.1f;
    std::size_t face = 0;
    std::string targetAsset;
    MirrorSide mirrorSide = MirrorSide::Automatic;
    BooleanOperation booleanOperation = BooleanOperation::Difference;
    Point3 targetOffset{};
    bool operator==(const MeshModifier &) const = default;
};

std::string applyMeshModifier(Mesh &mesh,const MeshModifier &modifier,const Mesh *target = nullptr);

}
