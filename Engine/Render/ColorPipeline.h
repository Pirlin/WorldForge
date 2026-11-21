#pragma once

// The WorldForge color pipeline. The editor viewport (GLSL) and the Player (HLSL) implement
// the same steps; this C++ version is the reference that the tests check.
//
// - Authored colors (material base color, terrain colors, sky and fog colors, vertex colors)
//   and base-color and emissive textures are sRGB-encoded. Renderers decode them to linear.
// - Emission strengths, normal, metallic-roughness and occlusion maps are linear.
// - Lighting is computed in linear Rec.709 and stored in a 16-bit float target.
// - The display step applies exposure, then a view transform:
//   AgX matches Studio's default view (fitted by Tests/FitAgx.py), and
//   Standard is a plain sRGB encode that clips highlights.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>

namespace worldforge::color {

using Rgb = std::array<float,3>;

enum class ViewTransform : std::uint8_t { Standard = 0, AgX = 1 };

inline constexpr float agxLowStops = -12.47393f;
inline constexpr float agxHighStops = 4.026069f;
inline constexpr double agxCurve[] = {
#include "Engine/Render/AgxCurve.inc"
};
inline constexpr double agxMatrices[] = {
#include "Engine/Render/AgxMatrices.inc"
};
static_assert(std::size(agxCurve) == 129 && std::size(agxMatrices) == 18);

// Photometric units. Renderers work in scene units that are pre-exposed for a camera at referenceEv100,
// which keeps sunlight inside 16-bit float range: one unit of luminance is nitsPerUnit cd/m^2, the
// brightest luminance such a camera records (saturation-based exposure, 1.2 * 2^EV100). A white
// diffuse surface under luxPerUnit lux therefore has a luminance of one unit.
// - Directional lights are authored in lux (a clear midday sun is about 100,000 lux).
// - Point lights are authored in candela; a light of luxPerUnit candela gives luxPerUnit lux at 1 m.
// - Camera exposure is authored in EV100 (sunny day about 15, overcast 12, indoors 7, lit street at
//   night 4). Exposure stops are referenceEv100 - EV100.
// The engine's earlier unitless intensity 1 is one scene unit, so old scenes keep their look.
inline constexpr float referenceEv100 = 15;
inline constexpr double nitsPerUnit = 1.2*32768;
inline constexpr double luxPerUnit = 3.14159265358979323846*nitsPerUnit;
inline constexpr float minimumExposure = -10, maximumExposure = 17; // EV100 25 down to -2
inline constexpr float maximumCompensation = 6;

inline float stopsFromEv100(float ev100) { return referenceEv100-ev100; }
inline float ev100FromStops(float stops) { return referenceEv100-stops; }
inline float unitsFromLux(double lux) { return float(lux/luxPerUnit); }
inline float unitsFromCandela(double candela) { return float(candela/luxPerUnit); }
inline double luxFromUnits(float units) { return double(units)*luxPerUnit; }
inline double candelaFromUnits(float units) { return double(units)*luxPerUnit; }


inline float srgbToLinear(float value)
{
    return value <= 0.04045f ? value/12.92f : std::pow((value+0.055f)/1.055f,2.4f);
}

inline float linearToSrgb(float value)
{
    value = std::clamp(value,0.0f,1.0f);
    return value <= 0.0031308f ? value*12.92f : 1.055f*std::pow(value,1/2.4f)-0.055f;
}

inline Rgb srgbToLinear(const Rgb &value)
{
    return {srgbToLinear(value[0]),srgbToLinear(value[1]),srgbToLinear(value[2])};
}

inline Rgb linearToSrgb(const Rgb &value)
{
    return {linearToSrgb(value[0]),linearToSrgb(value[1]),linearToSrgb(value[2])};
}

inline float exposureScale(float stops)
{
    return std::exp2(std::clamp(stops,minimumExposure,maximumExposure));
}

// Auto exposure. The display pass counts scene luminance into a histogram: bin 0 holds black pixels
// and bins 1 to 63 cover log2 luminance in scene units from meterLowStops to meterHighStops.
inline constexpr int meterBins = 64;
inline constexpr float meterLowStops = -16, meterHighStops = 8;

// The log2 average scene luminance of the metered pixels between the 10th and 90th percentile,
// which keeps a few bright lights or dark corners from swinging the exposure. Returns false when
// nothing was metered.
inline bool meteredLuminance(std::span<const std::uint32_t> bins,float &log2Average)
{
    if (bins.size() != std::size_t(meterBins))
        return false;
    double total = 0;
    for (const auto count : bins)
        total += count;
    if (total <= 0)
        return false;
    const double low = total*0.1, high = total*0.9;
    double seen = 0, weight = 0, sum = 0;
    for (int bin = 0; bin < meterBins; ++bin) {
        const double start = seen, end = seen+bins[std::size_t(bin)];
        seen = end;
        const double used = std::max(0.0,std::min(end,high)-std::max(start,low));
        if (used <= 0)
            continue;
        const double stops = bin == 0 ? meterLowStops-1
            : meterLowStops+(bin-0.5)*double(meterHighStops-meterLowStops)/(meterBins-1);
        sum += used*stops;
        weight += used;
    }
    if (weight <= 0)
        return false;
    log2Average = float(sum/weight);
    return true;
}

// The camera exposure in stops that shows a scene of the given log2 average luminance (scene units)
// as middle grey (18%), brightened by compensation stops.
inline float autoExposureStops(float log2Average,float compensation)
{
    return std::clamp(std::log2(0.18f)-log2Average+compensation,minimumExposure,maximumExposure);
}

// Moves the exposure toward the target over `seconds`. Eyes adapt to brightness faster than to darkness.
inline float adaptExposure(float current,float target,float seconds)
{
    const float rate = target < current ? 3.0f : 1.2f;
    return current+(target-current)*(1-std::exp(-rate*std::max(seconds,0.0f)));
}

inline float agxCurveAt(float position)
{
    const float scaled = std::clamp(position,0.0f,1.0f)*128;
    const int index = std::min(int(scaled),127);
    return float(agxCurve[index]+(agxCurve[index+1]-agxCurve[index])*(scaled-float(index)));
}

// Linear Rec.709 scene color to sRGB-encoded display color through AgX.
inline Rgb agx(const Rgb &linear)
{
    Rgb inset{}, curved{}, result{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            inset[row] += float(agxMatrices[row*3+column])*std::max(linear[column],0.0f);
    for (int channel = 0; channel < 3; ++channel) {
        const float stops = std::log2(std::max(inset[channel],1e-10f));
        const float position = (stops-agxLowStops)/(agxHighStops-agxLowStops);
        curved[channel] = std::pow(std::clamp(agxCurveAt(position),0.0f,1.0f),2.4f);
    }
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            result[row] += float(agxMatrices[9+row*3+column])*curved[column];
    return linearToSrgb(result);
}

inline Rgb displayColor(Rgb linear,float exposureStops,ViewTransform transform)
{
    const float scale = exposureScale(exposureStops);
    for (auto &channel : linear)
        channel *= scale;
    return transform == ViewTransform::AgX ? agx(linear) : linearToSrgb(linear);
}

// Ambient light comes from the sky. Until the physical atmosphere (R1) computes it, renderers scale their
// ambient term by how bright the authored sky colors are compared with the default sky (horizon #a6c5db,
// zenith #4177b1), so a dark night sky also darkens the ambient light. The default sky gives exactly 1.
inline float skyBrightness(const Rgb &horizonSrgb,const Rgb &zenithSrgb)
{
    const auto horizon = srgbToLinear(horizonSrgb), zenith = srgbToLinear(zenithSrgb);
    float luminance = 0;
    for (int channel = 0; channel < 3; ++channel)
        luminance += std::array{0.2126f,0.7152f,0.0722f}[std::size_t(channel)]*(horizon[std::size_t(channel)]
            +zenith[std::size_t(channel)]);
    return luminance;
}

inline float skyAmbientScale(const Rgb &horizonSrgb,const Rgb &zenithSrgb)
{
    static const float reference = skyBrightness({166/255.0f,197/255.0f,219/255.0f},{65/255.0f,119/255.0f,177/255.0f});
    return std::clamp(skyBrightness(horizonSrgb,zenithSrgb)/reference,0.0f,4.0f);
}

// Shader source text for the shared tables, for renderers that compile shaders at run time.
inline std::string shaderFloatList(std::span<const double> values)
{
    std::string text;
    char number[32];
    for (std::size_t i = 0; i < values.size(); ++i) {
        std::snprintf(number,sizeof(number),"%.9g",values[i]);
        text += number;
        text += i+1 < values.size() ? (i%8 == 7 ? ",\n" : ",") : "";
    }
    return text;
}

} // namespace worldforge::color
