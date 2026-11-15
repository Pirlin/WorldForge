#pragma once

#include "Engine/Geometry/Mesh.h"

namespace worldforge {

enum class BooleanOperation { Difference, Union, Intersection };

std::string booleanMesh(Mesh &mesh,const Mesh &target,BooleanOperation operation,Point3 offset = {});

}
