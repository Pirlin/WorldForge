#pragma once

#include "Engine/Geometry/Mesh.h"

#include <QByteArray>
#include <QString>
#include <optional>

std::optional<worldforge::Mesh> importGlb(const QByteArray &bytes, QString &error);
QByteArray exportGlb(const worldforge::Mesh &mesh,const QString &name,QString &error,bool studioTopology = false);
