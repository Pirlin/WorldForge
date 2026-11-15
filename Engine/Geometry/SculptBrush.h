#pragma once

#include "Engine/Geometry/Mesh.h"

namespace worldforge {

enum class SculptBrush { Draw, Smooth, Inflate, Flatten };

class SculptStroke {
public:
    explicit SculptStroke(const Mesh &source);
    bool dab(Mesh &mesh,Point3 center,Point3 normal,float radius,float strength,SculptBrush brush,bool symmetry);

private:
    std::vector<std::vector<VertexIndex>> neighbors_;
    std::vector<std::vector<std::size_t>> faces_;
};

}
