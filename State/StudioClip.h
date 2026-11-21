#pragma once

#include "Engine/Geometry/MeshClip.h"

#include <QByteArray>
#include <optional>

std::optional<worldforge::MeshClip> readStudioClip(QByteArrayView bytes);
QByteArray writeStudioClip(const worldforge::MeshClip &clip);
