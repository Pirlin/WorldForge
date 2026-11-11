#pragma once

#include "Engine/Geometry/Mesh.h"

#include <QByteArray>
#include <QString>
#include <optional>

std::optional<worldforge::Mesh> importGlb(const QByteArray &bytes, QString &error);
