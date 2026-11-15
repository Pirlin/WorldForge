#pragma once

#include "Engine/Geometry/Mesh.h"

#include <stop_token>

namespace worldforge {

std::string remeshVoxels(Mesh &mesh,float cellSize,std::stop_token stop = {});

}
