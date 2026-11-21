#pragma once

#include "Engine/Geometry/Mesh.h"

#include <QByteArray>
#include <QMap>
#include <QSharedPointer>
#include <QString>
#include <optional>

struct GlbUvLayers {
    QString active;
    QMap<QString,std::vector<worldforge::Point2>> other;
};

namespace worldforge { struct MeshClip; }
struct GlbMotion {
    const worldforge::MeshClip &source;
    QSharedPointer<const worldforge::MeshClip> mapped;
};

struct StudioMeshData {
    std::optional<GlbUvLayers> uvs;
    QSharedPointer<const worldforge::MeshClip> clip;
};

std::optional<worldforge::Mesh> importGlb(const QByteArray &bytes, QString &error,
                                         const QByteArray &uvManifest = {}, GlbUvLayers *uvLayers = nullptr,
                                         GlbMotion *motion = nullptr);
std::optional<worldforge::Mesh> importStudioGlb(const QByteArray &bytes,const QByteArray &uvManifest,
    const QByteArray &clipData,StudioMeshData &data,QString &error);
QByteArray exportGlb(const worldforge::Mesh &mesh,const QString &name,QString &error,bool studioTopology = false,
                     const GlbUvLayers &uvLayers = {});
