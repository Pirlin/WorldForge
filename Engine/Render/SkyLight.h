#pragma once

// Light from the sky for image-based lighting, shared by the editor viewport and the Player.
//
// The sky the user sees (Engine/Render/Shaders/Sky.frag: the horizon-to-zenith gradient and the glow
// around the sun) is the light source for everything the sun does not light directly:
// - Diffuse light uses its irradiance, projected onto nine spherical-harmonic coefficients.
// - Reflections look the sky up in the reflected direction (Shading.glsl).
// Below the horizon, the sky shows ground: in the absence of global illumination the ground is
// treated as a flat surface of groundAlbedo lit by the sun and the sky, which is what most of the
// lower hemisphere of a street scene is.
//
// Units are scene units of radiance (see ColorPipeline.h): a white surface under a uniform sky of
// radiance L reflects L, and under a sun of intensity I facing it reflects I.

#include "Engine/Render/ColorPipeline.h"

#include <array>
#include <cmath>

namespace worldforge::color {

inline constexpr float groundAlbedo = 0.25f;

struct SkyLight {
    // Coefficients of irradiance / pi (the radiance a white diffuse surface reflects) for the SH basis in
    // Shading.glsl, RGB in xyz.
    std::array<std::array<float,4>,9> irradiance{};
    Rgb horizon{}, zenith{}, ground{}; // linear radiance
};

// Radiance of the sky in direction (x, y, z), z up, matching Sky.frag without fog. Directions below the
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

// sun is the direction toward the sun (any length) and sunIntensity its strength in scene units.
inline SkyLight skyLight(const Rgb &horizonSrgb,const Rgb &zenithSrgb,std::array<float,3> sun,float sunIntensity)
{
    constexpr float pi = 3.14159265358979f;
    SkyLight light;
    light.horizon = srgbToLinear(horizonSrgb);
    light.zenith = srgbToLinear(zenithSrgb);
    const float length = std::sqrt(sun[0]*sun[0]+sun[1]*sun[1]+sun[2]*sun[2]);
    if (length > 1e-6f) {
        for (auto &axis : sun)
            axis /= length;
    } else {
        sun = {0,0,1};
        sunIntensity = 0;
    }
    // Midpoint integration over the sphere; the sky changes slowly except for the sun glow, which this
    // resolution still covers.
    constexpr int rows = 64, columns = 128;
    const auto integrate = [&](auto &&visit) {
        for (int row = 0; row < rows; ++row) {
            const float theta = (float(row)+0.5f)*pi/rows;
            const float weight = std::sin(theta)*(pi/rows)*(2*pi/columns);
            for (int column = 0; column < columns; ++column) {
                const float phi = (float(column)+0.5f)*2*pi/columns;
                const float direction[3]{std::sin(theta)*std::cos(phi),std::sin(theta)*std::sin(phi),std::cos(theta)};
                visit(direction,weight);
            }
        }
    };
    // The ground reflects the sun and the upper sky that reach it.
    Rgb skyOnGround{};
    integrate([&](const float *direction,float weight) {
        if (direction[2] <= 0)
            return;
        const auto radiance = skyRadiance(direction,light.horizon,light.zenith,{},sun.data(),sunIntensity);
        for (int c = 0; c < 3; ++c)
            skyOnGround[std::size_t(c)] += radiance[std::size_t(c)]*direction[2]*weight;
    });
    for (int c = 0; c < 3; ++c)
        light.ground[std::size_t(c)] = groundAlbedo/pi*(pi*sunIntensity*std::max(sun[2],0.0f)+skyOnGround[std::size_t(c)]);
    // Project the radiance, then convolve with the cosine lobe (Ramamoorthi and Hanrahan): band l scales by
    // A_l / pi = 1, 2/3, 1/4, which gives irradiance / pi.
    std::array<Rgb,9> projected{};
    integrate([&](const float *direction,float weight) {
        const auto radiance = skyRadiance(direction,light.horizon,light.zenith,light.ground,sun.data(),sunIntensity);
        const auto basis = shBasis(direction[0],direction[1],direction[2]);
        for (std::size_t i = 0; i < 9; ++i)
            for (int c = 0; c < 3; ++c)
                projected[i][std::size_t(c)] += radiance[std::size_t(c)]*basis[i]*weight;
    });
    constexpr float band[9]{1,2/3.0f,2/3.0f,2/3.0f,0.25f,0.25f,0.25f,0.25f,0.25f};
    for (std::size_t i = 0; i < 9; ++i)
        for (int c = 0; c < 3; ++c)
            light.irradiance[i][std::size_t(c)] = band[i]*projected[i][std::size_t(c)];
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

} // namespace worldforge::color
