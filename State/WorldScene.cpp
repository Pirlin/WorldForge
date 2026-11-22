#include "State/WorldScene.h"
#include "State/ProjectStore.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>

worldforge::Heightfield readHeightfield(const SceneObject &object)
{
    worldforge::Heightfield field;
    field.size = float(std::clamp(object.details.value("size").toDouble(32), 4.0, 1024.0));
    const auto values = object.details.value("heightSamples").toArray();
    if (values.size() == qsizetype(field.samples.size()))
        for (qsizetype i = 0; i < values.size(); ++i)
            field.samples[i] = float(std::clamp(values.at(i).toDouble(), -256.0, 256.0));
    const auto layers = object.details.value("terrainLayers").toArray();
    if (layers.size() == qsizetype(field.layers.size()))
        for (qsizetype i = 0; i < layers.size(); ++i)
            field.layers[i] = std::uint8_t(std::clamp(layers.at(i).toInt(),0,3));
    return field;
}

QJsonObject writeHeightfield(const worldforge::Heightfield &field)
{
    QJsonArray values;
    for (float h : field.samples)
        values.append(h);
    return {{"size", field.size}, {"heightSamples", values}};
}

WorldAtmosphere readAtmosphere(const QJsonObject &details)
{
    WorldAtmosphere environment;
    const auto color = [&](const char *key, QColor fallback) {
        const QColor value(details.value(key).toString());
        return value.isValid() ? value : fallback;
    };
    environment.sky = details.value("sky").toBool(true);
    environment.horizon = color("horizon", environment.horizon);
    environment.zenith = color("zenith", environment.zenith);
    environment.fogColor = color("fogColor", environment.fogColor);
    environment.fogDensity = float(std::clamp(details.value("fogDensity").toDouble(environment.fogDensity), 0.0, 1.0));
    environment.fogFalloff = float(std::clamp(details.value("fogFalloff").toDouble(environment.fogFalloff), 0.0, 2.0));
    environment.fogHeight = float(std::clamp(details.value("fogHeight").toDouble(), -1000.0, 1000.0));
    environment.sunAzimuth = float(std::clamp(details.value("sunAzimuth").toDouble(environment.sunAzimuth),-180.0,180.0));
    environment.sunElevation = float(std::clamp(details.value("sunElevation").toDouble(environment.sunElevation),0.0,90.0));
    environment.shadows = details.value("shadows").toBool(true);
    const auto center = details.value("localFogCenter").toArray();
    if (center.size() == 3)
        for (int i = 0; i < 3; ++i)
            environment.localFogCenter[i] = float(std::clamp(center.at(i).toDouble(),-100000.0,100000.0));
    environment.localFogRadius = float(std::clamp(details.value("localFogRadius").toDouble(),0.0,500.0));
    environment.localFogDensity = float(std::clamp(details.value("localFogDensity").toDouble(0.10),0.0,1.0));
    // "ev100" is the authored camera exposure; projects from before photometric units store "exposure" in stops.
    namespace units = worldforge::color;
    const double stops = details.contains("ev100") ? units::stopsFromEv100(float(details.value("ev100").toDouble(15)))
                                                   : details.value("exposure").toDouble();
    environment.exposure = float(std::clamp(stops,double(units::minimumExposure),double(units::maximumExposure)));
    environment.autoExposure = details.value("exposureMode").toString() == "Auto";
    environment.temporalAntiAliasing = details.value("antiAliasing").toString() != "Off";
    environment.ambientOcclusion = details.value("ambientOcclusion").toBool(true);
    environment.physicalSky = details.value("skyModel").toString() == QStringLiteral("Physical");
    environment.exposureCompensation = float(std::clamp(details.value("exposureCompensation").toDouble(),
        -double(units::maximumCompensation),double(units::maximumCompensation)));
    environment.viewTransform = details.value("viewTransform").toString() == "Standard"
        ? worldforge::color::ViewTransform::Standard : worldforge::color::ViewTransform::AgX;
    return environment;
}

QVector3D sunDirection(const WorldAtmosphere &atmosphere)
{
    constexpr float radians = 0.01745329252f;
    const float azimuth = atmosphere.sunAzimuth*radians, elevation = atmosphere.sunElevation*radians;
    return {std::cos(azimuth)*std::cos(elevation),std::sin(azimuth)*std::cos(elevation),std::sin(elevation)};
}

float lightUnits(const SceneObject &object)
{
    namespace color = worldforge::color;
    if (object.type == "Directional Light" && object.details.contains("illuminance"))
        return color::unitsFromLux(std::clamp(object.details.value("illuminance").toDouble(),0.0,maximumIlluminance));
    if (object.type == "Point Light" && object.details.contains("luminousIntensity"))
        return color::unitsFromCandela(std::clamp(object.details.value("luminousIntensity").toDouble(),0.0,
                                                  maximumLuminousIntensity));
    return float(std::clamp(object.details.value("lightIntensity").toDouble(1),0.0,4.0));
}

double lightPhotometric(const SceneObject &object)
{
    const double units = lightUnits(object);
    return object.type == "Point Light" ? worldforge::color::candelaFromUnits(float(units))
                                        : worldforge::color::luxFromUnits(float(units));
}

void setLightPhotometric(QJsonObject &details,const QString &type,double value)
{
    details.remove("lightIntensity");
    if (type == "Point Light")
        details.insert("luminousIntensity",std::clamp(value,0.0,maximumLuminousIntensity));
    else
        details.insert("illuminance",std::clamp(value,0.0,maximumIlluminance));
}
