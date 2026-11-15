#include "State/StudioPublishReview.h"

#include <QCoreApplication>
#include <QJsonArray>

#include <algorithm>
#include <cmath>

namespace {

QString tr(const char *text, const char *disambiguation = nullptr, int n = -1)
{
    return QCoreApplication::translate("StudioPublishReview", text, disambiguation, n);
}

struct Bounds {
    worldforge::Point3 low{}, high{};
    float extent(int axis) const { return high[axis] - low[axis]; }
};

std::optional<Bounds> bounds(const worldforge::Mesh &mesh)
{
    if (mesh.vertices.empty())
        return std::nullopt;
    Bounds result{mesh.vertices.front(), mesh.vertices.front()};
    for (const auto &vertex : mesh.vertices)
        for (int axis = 0; axis < 3; ++axis) {
            result.low[axis] = std::min(result.low[axis], vertex[axis]);
            result.high[axis] = std::max(result.high[axis], vertex[axis]);
        }
    return result;
}

QString size(const Bounds &box)
{
    return QStringLiteral("%1 × %2 × %3 m").arg(box.extent(0), 0, 'f', 2).arg(box.extent(1), 0, 'f', 2)
        .arg(box.extent(2), 0, 'f', 2);
}

// The runtime collider uses the custom proxy box in mesh space when one is set.
bool proxyCovers(const QJsonObject &proxy, const Bounds &mesh)
{
    const auto center = proxy.value("center").toArray();
    const auto half = proxy.value("half").toArray();
    if (center.size() != 3 || half.size() != 3)
        return true;
    for (int axis = 0; axis < 3; ++axis) {
        const float tolerance = std::max(0.01f, mesh.extent(axis) * 0.02f);
        const float c = float(center[axis].toDouble());
        const float h = std::abs(float(half[axis].toDouble()));
        if (mesh.low[axis] < c - h - tolerance || mesh.high[axis] > c + h + tolerance)
            return false;
    }
    return true;
}

}

StudioPublishReview reviewStudioPublish(const Project &project, const MeshAsset *asset, const QString &name,
                                        const worldforge::Mesh &preview)
{
    StudioPublishReview review;
    review.creating = asset == nullptr;
    review.assetName = asset ? asset->name : name;
    review.vertices = preview.vertices.size();
    review.faces = preview.faces.size();
    review.materials = preview.materials.size();
    review.hasLod = asset && asset->lodCell > 0;
    if (preview.faces.empty() || preview.vertices.empty()) {
        review.blocker = tr("The Studio mesh has no faces. Add geometry in Studio and publish again.");
        return review;
    }
    if (const auto problem = worldforge::validateMesh(preview); !problem.empty()) {
        review.blocker = tr("The Studio mesh can't be used: %1").arg(QString::fromStdString(problem));
        return review;
    }
    const auto next = bounds(preview);
    if (asset) {
        if (const auto current = bounds(asset->evaluated())) {
            bool resized = false;
            for (int axis = 0; axis < 3; ++axis) {
                const float before = current->extent(axis), after = next->extent(axis);
                resized |= std::abs(after - before) > std::max(0.01f, before * 0.25f);
            }
            if (resized)
                review.warnings << tr("The mesh changes size from %1 to %2.").arg(size(*current), size(*next));
        }
    }
    for (const auto &object : project.objects) {
        if (!asset || object.meshAsset != asset->id)
            continue;
        const auto physics = object.details.value("physics").toString();
        const bool collides = physics == "Static" || physics == "Dynamic";
        const auto proxy = object.details.value("collisionProxy");
        QString collider;
        if (!collides) {
            collider = tr("No physics collider");
            review.warnings << tr("%1 has no physics collider and will not block the player.").arg(object.name);
        } else if (proxy.isObject()) {
            collider = tr("%1 · custom collision bounds").arg(physics);
            if (!proxyCovers(proxy.toObject(), *next))
                review.warnings << tr("%1: the custom collision bounds no longer cover the mesh. "
                                      "Update them in Details.").arg(object.name);
        } else {
            collider = tr("%1 · collision bounds follow the mesh").arg(physics);
        }
        review.placements << tr("%1 — %2").arg(object.name, collider);
    }
    for (int axis = 0; axis < 3 && !review.placements.isEmpty(); ++axis)
        if (next->extent(axis) < 0.05f) {
            review.warnings << tr("The mesh is nearly flat. Its collider will be very thin.");
            break;
        }
    return review;
}

QString StudioPublishReview::summary() const
{
    const auto count = [](std::size_t value, const char *one, const char *many) {
        return tr(value == 1 ? one : many).arg(value);
    };
    auto text = creating ? tr("Create “%1” and place it in this level?").arg(assetName)
        : tr("Update “%1” in %2?").arg(assetName,
              count(std::size_t(placements.size()), "%1 level placement", "%1 level placements"));
    text += tr("\n\n%1 · %2 · %3\nYou can undo this edit.")
        .arg(count(vertices, "%1 vertex", "%1 vertices"), count(faces, "%1 face", "%1 faces"),
             count(materials, "%1 material slot", "%1 material slots"));
    if (hasLod)
        text += tr("\nThe mesh has a generated LOD. Review it after publishing.");
    if (!warnings.isEmpty())
        text += tr("\n\nCheck before applying:\n• %1").arg(warnings.join(QStringLiteral("\n• ")));
    else if (!placements.isEmpty())
        text += tr("\n\nCollision checks passed. Confirm the result in Play after applying.");
    return text;
}
