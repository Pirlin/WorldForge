#pragma once

#include "State/ProjectStore.h"

#include <QStringList>

// What applying a Studio publish will change, built before the user approves it.
struct StudioPublishReview {
    QString assetName;
    bool creating = false;
    // One line per affected placement: name and collider.
    QStringList placements;
    // Collision and size issues the user should check in Play.
    QStringList warnings;
    // Non-empty when the staged mesh cannot be applied.
    QString blocker;
    std::size_t vertices = 0;
    std::size_t faces = 0;
    std::size_t materials = 0;
    bool hasLod = false;

    QString summary() const;
};

// Reviews a staged Studio mesh against the placements of `asset` in `project`.
// Pass a null asset when the publish creates a new mesh named `name`.
StudioPublishReview reviewStudioPublish(const Project &project, const MeshAsset *asset, const QString &name,
                                        const worldforge::Mesh &preview);
