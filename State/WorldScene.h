#pragma once

#include "Engine/World/WorldSimulation.h"
#include "Engine/Render/ColorPipeline.h"
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
    // Camera exposure in stops (referenceEv100 minus the authored EV100), auto exposure and its
    // compensation in stops, and how linear scene light is shown on the display.
    float exposure = 0;
    bool autoExposure = false;
    float exposureCompensation = 0;
    // Temporal anti-aliasing (TAA) smooths jagged edges by averaging jittered frames.
    bool temporalAntiAliasing = true;
    // Ground-truth ambient occlusion darkens sky light in creases, corners and openings.
    bool ambientOcclusion = true;
    worldforge::color::ViewTransform viewTransform = worldforge::color::ViewTransform::AgX;
};

worldforge::Heightfield readHeightfield(const SceneObject &object);
QJsonObject writeHeightfield(const worldforge::Heightfield &field);
WorldAtmosphere readAtmosphere(const QJsonObject &details);
QVector3D sunDirection(const WorldAtmosphere &atmosphere);

// Light strength in scene units (see Engine/Render/ColorPipeline.h). Directional lights store "illuminance" in
// lux and point lights "luminousIntensity" in candela; projects from before photometric units store
// "lightIntensity" in scene units.
float lightUnits(const SceneObject &object);
// The authored value shown for a light: lux for a directional light, candela for a point light.
double lightPhotometric(const SceneObject &object);
// Stores an authored lux or candela value in details for a light of the given type.
void setLightPhotometric(QJsonObject &details,const QString &type,double value);
inline constexpr double maximumIlluminance = 500000, maximumLuminousIntensity = 1000000;
