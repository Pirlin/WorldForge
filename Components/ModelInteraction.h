#pragma once

#include "Engine/Geometry/Mesh.h"

#include <QList>
#include <QSharedPointer>
#include <QVector3D>
#include <array>
#include <compare>
#include <set>

enum class ModelDomain { Object, Vertex, Edge, Face };
enum class ModelShading { Wireframe, Solid, Material, ShadedWire };
enum class ModelHandle { Move, Rotate, Scale };

struct ModelComponent {
    int index;
    int end = -1;
    auto operator<=>(const ModelComponent &) const = default;
};

struct ModelGizmo {
    QVector3D pivot;
    std::array<QVector3D, 3> axes;
    int axis = -1;
    bool vertexSnap = false;
    bool surfaceSnap = false;
    ModelHandle kind = ModelHandle::Move;
    bool passive = false;
};

inline std::set<int> modelVertices(const worldforge::Mesh &mesh, ModelDomain domain,
                                  const std::set<ModelComponent> &selection)
{
    std::set<int> vertices;
    for (const auto component : selection) {
        if (domain == ModelDomain::Face && component.index >= 0 && std::size_t(component.index) < mesh.faces.size()) {
            for (const auto &corner : mesh.faces[component.index].corners)
                vertices.insert(int(corner.vertex));
        } else if (domain == ModelDomain::Vertex || domain == ModelDomain::Edge) {
            if (component.index >= 0 && std::size_t(component.index) < mesh.vertices.size())
                vertices.insert(component.index);
            if (domain == ModelDomain::Edge && component.end >= 0 && std::size_t(component.end) < mesh.vertices.size())
                vertices.insert(component.end);
        }
    }
    return vertices;
}
