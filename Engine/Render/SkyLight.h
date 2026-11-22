#pragma once

// Light from the sky for image-based lighting, shared by the editor viewport and the Player.
//
// The sky (the authored horizon-to-zenith gradient with the glow around the sun, or the physical atmosphere in
// Atmosphere.h) is the light source for everything the sun does not light directly. It is captured once, whenever it
// changes, as an environment:
// - A radiance map: equirectangular, with rows spaced closer near the horizon, where the sky changes fastest.
// - The map prefiltered with the GGX lobe for roughness 1/3, 2/3 and 1 (split-sum, Karis 2013), for reflections.
//   Upward reflections leave out the part of the lobe below the horizon, which on ground and roofs lies under the
//   surface itself and never reaches the ground.
// - Its irradiance, projected onto nine spherical-harmonic coefficients, for diffuse light.
// Shading.glsl reads the map and its levels from a storage buffer, laid out as mapOffset / levelOffset give.
// Below the horizon, the gradient sky shows ground: a flat surface of groundAlbedo lit by the sun and the sky,
// which is what most of the lower hemisphere of a street scene is. The atmosphere shows its own ground, seen through
// the air.
//
// Units are scene units of radiance (see ColorPipeline.h): a white surface under a uniform sky of
// radiance L reflects L, and under a sun of intensity I facing it reflects I.

#include "Engine/Render/Atmosphere.h"
#include "Engine/Render/ColorPipeline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace worldforge::color {

inline constexpr float groundAlbedo = 0.25f;

// The environment map (Shading.glsl mirrors these): the radiance map, then the prefiltered levels.
inline constexpr int mapWidth = 128, mapHeight = 64;
inline constexpr int levelWidth = 64, levelHeight = 32, prefilteredLevels = 3; // roughness 1/3, 2/3, 1
inline constexpr std::size_t mapTexels = std::size_t(mapWidth*mapHeight+prefilteredLevels*levelWidth*levelHeight);
inline constexpr std::size_t levelOffset(int level) // level 0 is the radiance map
{
    return level == 0 ? 0 : std::size_t(mapWidth*mapHeight+(level-1)*levelWidth*levelHeight);
}

struct SkyLight {
    // Coefficients of irradiance / pi (the radiance a white diffuse surface reflects) for the SH basis in
    // Shading.glsl, RGB in xyz.
    std::array<std::array<float,4>,9> irradiance{};
    Rgb horizon{}, zenith{}, ground{}; // linear radiance, for reference
    // The radiance map and its prefiltered levels, RGB in xyz (mapTexels entries).
    std::vector<std::array<float,4>> environment;
    // Sunlight at the ground in scene units per channel, for the sun that lights the scene.
    Rgb sunLight{};
};

namespace sky {

inline constexpr float pi = 3.14159265358979f;

// The direction at map coordinates (u, v) in [0, 1]: u turns around the vertical, v goes from straight down to
// straight up with elevation = sign(t) t^2 pi/2, t = 2v - 1.
inline std::array<float,3> direction(float u,float v)
{
    const float phi = (u-0.5f)*2*pi, t = 2*v-1;
    const float elevation = (t < 0 ? -1.0f : 1.0f)*t*t*pi/2;
    return {std::cos(elevation)*std::cos(phi),std::cos(elevation)*std::sin(phi),std::sin(elevation)};
}

inline std::array<float,2> coordinates(const float d[3])
{
    const float u = std::atan2(d[1],d[0])/(2*pi)+0.5f;
    const float elevation = std::asin(std::clamp(d[2],-1.0f,1.0f));
    const float t = (elevation < 0 ? -1.0f : 1.0f)*std::sqrt(std::abs(elevation)/(pi/2));
    return {u,0.5f+0.5f*t};
}

// Bilinear lookup in a map of width x height at offset, wrapping around the vertical and clamping at the poles.
inline Rgb sample(const std::vector<std::array<float,4>> &environment,std::size_t offset,int width,int height,
                  const float d[3])
{
    const auto uv = coordinates(d);
    const float x = uv[0]*float(width)-0.5f, y = std::clamp(uv[1]*float(height)-0.5f,0.0f,float(height-1));
    const int x0 = int(std::floor(x)), y0 = std::min(int(y),height-2);
    const float sx = x-float(x0), sy = std::clamp(y-float(y0),0.0f,1.0f);
    const auto at = [&](int cx,int cy) {
        const auto &texel = environment[offset+std::size_t(cy*width+((cx%width)+width)%width)];
        return Rgb{texel[0],texel[1],texel[2]};
    };
    Rgb result{};
    for (int c = 0; c < 3; ++c) {
        const auto ci = std::size_t(c);
        result[ci] = (at(x0,y0)[ci]*(1-sx)+at(x0+1,y0)[ci]*sx)*(1-sy)+(at(x0,y0+1)[ci]*(1-sx)+at(x0+1,y0+1)[ci]*sx)*sy;
    }
    return result;
}

} // namespace sky

// Radiance of the gradient sky in direction (x, y, z), z up, matching Sky.frag without fog. Directions below the
// horizon return ground.
inline Rgb skyRadiance(const float direction[3],const Rgb &horizon,const Rgb &zenith,const Rgb &ground,
                       const float sun[3],float sunIntensity)
{
    if (direction[2] < 0)
        return ground;
    const float height = std::pow(direction[2],0.55f);
    Rgb result{};
    const float toward = std::max(0.0f,direction[0]*sun[0]+direction[1]*sun[1]+direction[2]*sun[2]);
    const float glow = sunIntensity*std::pow(toward,32.0f);
    const Rgb glowColor = srgbToLinear(Rgb{0.42f,0.25f,0.12f});
    for (int c = 0; c < 3; ++c)
        result[std::size_t(c)] = horizon[std::size_t(c)]+(zenith[std::size_t(c)]-horizon[std::size_t(c)])*height
            +glow*glowColor[std::size_t(c)];
    return result;
}

// The real spherical-harmonic basis up to band 2, in the order the shader uses.
inline std::array<float,9> shBasis(float x,float y,float z)
{
    return {0.282095f,0.488603f*y,0.488603f*z,0.488603f*x,1.092548f*x*y,1.092548f*y*z,
            0.315392f*(3*z*z-1),1.092548f*x*z,0.546274f*(x*x-y*y)};
}

// The sky light of any sky: radiance(const float direction[3]) gives its radiance in scene units.
template<class Radiance>
SkyLight skyLightFrom(Radiance &&radiance)
{
    constexpr float pi = sky::pi;
    SkyLight light;
    light.environment.assign(mapTexels,{});
    for (int y = 0; y < mapHeight; ++y)
        for (int x = 0; x < mapWidth; ++x) {
            const auto d = sky::direction((float(x)+0.5f)/mapWidth,(float(y)+0.5f)/mapHeight);
            const auto value = radiance(d.data());
            light.environment[std::size_t(y*mapWidth+x)] = {value[0],value[1],value[2],0};
        }
    // Irradiance: project the map, weighting each texel by its solid angle, then convolve with the cosine lobe
    // (Ramamoorthi and Hanrahan): band l scales by A_l / pi = 1, 2/3, 1/4, which gives irradiance / pi.
    std::array<Rgb,9> projected{};
    for (int y = 0; y < mapHeight; ++y) {
        const float t = 2*(float(y)+0.5f)/mapHeight-1;
        const float elevation = (t < 0 ? -1.0f : 1.0f)*t*t*pi/2;
        const float solidAngle = std::cos(elevation)*(2*pi/mapWidth)*(2*pi*std::abs(t)/mapHeight);
        for (int x = 0; x < mapWidth; ++x) {
            const auto d = sky::direction((float(x)+0.5f)/mapWidth,(float(y)+0.5f)/mapHeight);
            const auto basis = shBasis(d[0],d[1],d[2]);
            const auto &value = light.environment[std::size_t(y*mapWidth+x)];
            for (std::size_t i = 0; i < 9; ++i)
                for (int c = 0; c < 3; ++c)
                    projected[i][std::size_t(c)] += value[std::size_t(c)]*basis[i]*solidAngle;
        }
    }
    constexpr float band[9]{1,2/3.0f,2/3.0f,2/3.0f,0.25f,0.25f,0.25f,0.25f,0.25f};
    for (std::size_t i = 0; i < 9; ++i)
        for (int c = 0; c < 3; ++c)
            light.irradiance[i][std::size_t(c)] = band[i]*projected[i][std::size_t(c)];
    // Prefiltered levels: GGX importance samples around the reflected direction, weighted by the cosine to it
    // (the split-sum approximation takes the normal and the view along it).
    constexpr int samples = 256;
    const auto radicalInverse = [](uint32_t bits) {
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
        bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
        bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
        bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
        return float(bits)*2.3283064365386963e-10f;
    };
    for (int level = 1; level <= prefilteredLevels; ++level) {
        const float roughness = float(level)/prefilteredLevels;
        const float a2 = roughness*roughness*roughness*roughness;
        for (int y = 0; y < levelHeight; ++y)
            for (int x = 0; x < levelWidth; ++x) {
                const auto r = sky::direction((float(x)+0.5f)/levelWidth,(float(y)+0.5f)/levelHeight);
                // An orthonormal frame around r.
                const std::array<float,3> reference = std::abs(r[2]) > 0.99f ? std::array<float,3>{1,0,0}
                                                                             : std::array<float,3>{0,0,1};
                std::array<float,3> tangent{reference[1]*r[2]-reference[2]*r[1],reference[2]*r[0]-reference[0]*r[2],
                                            reference[0]*r[1]-reference[1]*r[0]};
                const float length = std::sqrt(tangent[0]*tangent[0]+tangent[1]*tangent[1]+tangent[2]*tangent[2]);
                for (auto &axis : tangent)
                    axis /= length;
                const std::array<float,3> bitangent{r[1]*tangent[2]-r[2]*tangent[1],r[2]*tangent[0]-r[0]*tangent[2],
                                                    r[0]*tangent[1]-r[1]*tangent[0]};
                Rgb sum{};
                float total = 0;
                for (int i = 0; i < samples; ++i) {
                    const float phi = 2*pi*(float(i)+0.5f)/samples;
                    const float u = radicalInverse(uint32_t(i));
                    const float cosine = std::sqrt((1-u)/(1+(a2-1)*u));
                    const float sine = std::sqrt(std::max(0.0f,1-cosine*cosine));
                    float h[3];
                    for (int k = 0; k < 3; ++k)
                        h[k] = tangent[std::size_t(k)]*sine*std::cos(phi)+bitangent[std::size_t(k)]*sine*std::sin(phi)
                            +r[std::size_t(k)]*cosine;
                    const float rh = r[0]*h[0]+r[1]*h[1]+r[2]*h[2];
                    const float l[3]{2*rh*h[0]-r[0],2*rh*h[1]-r[1],2*rh*h[2]-r[2]};
                    const float weight = l[0]*r[0]+l[1]*r[1]+l[2]*r[2];
                    if (weight <= 0 || (r[2] >= 0 && l[2] < 0))
                        continue;
                    const auto value = sky::sample(light.environment,0,mapWidth,mapHeight,l);
                    for (int c = 0; c < 3; ++c)
                        sum[std::size_t(c)] += value[std::size_t(c)]*weight;
                    total += weight;
                }
                auto &texel = light.environment[levelOffset(level)+std::size_t(y*levelWidth+x)];
                for (int c = 0; c < 3; ++c)
                    texel[std::size_t(c)] = total > 0 ? sum[std::size_t(c)]/total : 0;
            }
    }
    const float up[3]{0,0,1}, down[3]{0,0,-1};
    light.zenith = radiance(up);
    light.ground = radiance(down);
    for (int x = 0; x < mapWidth; ++x) {
        const auto d = sky::direction((float(x)+0.5f)/mapWidth,0.5f+0.5f/mapHeight);
        const auto value = radiance(d.data());
        for (int c = 0; c < 3; ++c)
            light.horizon[std::size_t(c)] += value[std::size_t(c)]/mapWidth;
    }
    return light;
}

// The gradient sky: horizon and zenith colors in sRGB, sun the direction toward the sun (any length) and
// sunIntensity its strength in scene units.
inline SkyLight skyLight(const Rgb &horizonSrgb,const Rgb &zenithSrgb,std::array<float,3> sun,float sunIntensity)
{
    constexpr float pi = sky::pi;
    const Rgb horizon = srgbToLinear(horizonSrgb), zenith = srgbToLinear(zenithSrgb);
    const float length = std::sqrt(sun[0]*sun[0]+sun[1]*sun[1]+sun[2]*sun[2]);
    if (length > 1e-6f) {
        for (auto &axis : sun)
            axis /= length;
    } else {
        sun = {0,0,1};
        sunIntensity = 0;
    }
    // The ground reflects the sun and the upper sky that reach it (midpoint integration over the upper hemisphere).
    constexpr int rows = 32, columns = 64;
    Rgb skyOnGround{};
    for (int row = 0; row < rows; ++row) {
        const float theta = (float(row)+0.5f)*(pi/2)/rows;
        const float weight = std::sin(theta)*((pi/2)/rows)*(2*pi/columns);
        for (int column = 0; column < columns; ++column) {
            const float phi = (float(column)+0.5f)*2*pi/columns;
            const float direction[3]{std::sin(theta)*std::cos(phi),std::sin(theta)*std::sin(phi),std::cos(theta)};
            const auto radiance = skyRadiance(direction,horizon,zenith,{},sun.data(),sunIntensity);
            for (int c = 0; c < 3; ++c)
                skyOnGround[std::size_t(c)] += radiance[std::size_t(c)]*direction[2]*weight;
        }
    }
    Rgb ground{};
    for (int c = 0; c < 3; ++c)
        ground[std::size_t(c)] = groundAlbedo/pi*(pi*sunIntensity*std::max(sun[2],0.0f)+skyOnGround[std::size_t(c)]);
    auto light = skyLightFrom([&](const float *direction) {
        return skyRadiance(direction,horizon,zenith,ground,sun.data(),sunIntensity);
    });
    light.horizon = horizon;
    light.zenith = zenith;
    light.ground = ground;
    light.sunLight = {sunIntensity,sunIntensity,sunIntensity};
    return light;
}

// The physical atmosphere (Atmosphere.h) for the sun toward sun with the authored sunIntensity.
inline SkyLight physicalSkyLight(std::array<float,3> sun,float sunIntensity)
{
    const atmosphere::Sky atmosphereSky(sun,sunIntensity);
    auto light = skyLightFrom([&](const float *direction) { return atmosphereSky.radiance(direction); });
    light.sunLight = atmosphereSky.sunLight();
    return light;
}

// Irradiance / pi in direction (x, y, z) from the coefficients, as Shading.glsl evaluates it.
inline Rgb skyIrradiance(const SkyLight &light,float x,float y,float z)
{
    const auto basis = shBasis(x,y,z);
    Rgb result{};
    for (std::size_t i = 0; i < 9; ++i)
        for (int c = 0; c < 3; ++c)
            result[std::size_t(c)] += light.irradiance[i][std::size_t(c)]*basis[i];
    return result;
}

// The luminance of the sky's radiance map in direction d (Shading.glsl weights sky directions by it).
inline float skyLuminance(const SkyLight &light,const float d[3])
{
    if (light.environment.size() < mapTexels)
        return 0;
    const auto value = sky::sample(light.environment,0,mapWidth,mapHeight,d);
    return 0.2126f*value[0]+0.7152f*value[1]+0.0722f*value[2];
}

// The sky reflected in direction d by a surface of the given roughness, as Shading.glsl evaluates it.
inline Rgb skyReflection(const SkyLight &light,const float d[3],float roughness)
{
    const float level = std::clamp(roughness,0.0f,1.0f)*prefilteredLevels;
    const int low = std::min(int(level),prefilteredLevels-1);
    const auto at = [&](int index) {
        return index == 0 ? sky::sample(light.environment,0,mapWidth,mapHeight,d)
                          : sky::sample(light.environment,levelOffset(index),levelWidth,levelHeight,d);
    };
    const auto a = at(low), b = at(low+1);
    const float s = level-float(low);
    return {a[0]+(b[0]-a[0])*s,a[1]+(b[1]-a[1])*s,a[2]+(b[2]-a[2])*s};
}

} // namespace worldforge::color
