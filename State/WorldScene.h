#pragma once

#include "Engine/World/WorldSimulation.h"
#include <QColor>
#include <QJsonObject>
#include <QVector3D>

struct SceneObject;

struct WorldAtmosphere {
    QColor horizon{"#a6c5db"};
    QColor zenith{"#4177b1"};
    QColor fogColor{"#b9c9d7"};
    float fogDensity = 0.024f;
    float fogFalloff = 0.045f;
    float fogHeight = 0;
    bool sky = true;
    float sunAzimuth = 133;
    float sunElevation = 14;
    bool shadows = true;
    QVector3D localFogCenter{0,0,1};
    float localFogRadius = 0;
    float localFogDensity = 0.10f;
};

worldforge::Heightfield readHeightfield(const SceneObject &object);
QJsonObject writeHeightfield(const worldforge::Heightfield &field);
WorldAtmosphere readAtmosphere(const QJsonObject &details);
QVector3D sunDirection(const WorldAtmosphere &atmosphere);
