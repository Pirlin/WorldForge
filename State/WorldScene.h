#pragma once

#include "Engine/World/WorldSimulation.h"
#include "Engine/Render/ColorPipeline.h"
#include "Engine/Render/Weather.h"
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
    bool probeGi = false, screenSpaceGi = false, reflectionProbes = false;
    // The physically based atmosphere (Engine/Render/Atmosphere.h) sets the sky and the sunlight's color from the
    // sun's position, instead of the horizon and zenith colors.
    bool physicalSky = false;
    worldforge::Weather weather;
    // Bloom: light scattered around bright areas, as a camera lens does.
    bool bloom = true;
    // Screen-space reflections: glossy surfaces reflect what the previous frame shows, where it shows it.
    bool reflections = true;
    // Contact shadows: a short ray toward the sun on screen finds small occluders the sun's shadow map misses.
    bool contactShadows = true;
    // Camera effects: depth of field (a lens focused at focusDistance meters with aperture f-number fNumber) and
    // motion blur from the camera's movement, with the shutter open for that share of a frame. Both off by default.
    bool depthOfField = false;
    float focusDistance = 10, fNumber = 2.8f;
    bool motionBlur = false;
    float shutter = 0.5f;
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
QColor lampColor(const SceneObject &object);
worldforge::color::Rgb linearLampColor(const SceneObject &object);
// A spot light's cone: the full angle in degrees ("coneAngle", default 45, as Blender's Spot Size), how much of its edge
// fades out ("coneBlend", 0 to 1, default 0.15), and the direction it points (its rotation applied to straight down).
constexpr double defaultConeAngle = 45, defaultConeBlend = 0.15;
double spotConeAngle(const SceneObject &object);
double spotConeBlend(const SceneObject &object);
QVector3D spotDirection(const SceneObject &object);
inline constexpr double maximumIlluminance = 500000, maximumLuminousIntensity = 1000000;
