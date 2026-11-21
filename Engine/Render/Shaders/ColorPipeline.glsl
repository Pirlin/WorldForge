// GLSL version of Engine/Render/ColorPipeline.h. The AgX tables are shared with C++ and HLSL.
const float agxLowStops = -12.47393;
const float agxHighStops = 4.026069;
const float agxCurve[129] = float[129](
#include "Engine/Render/AgxCurve.inc"
);
const float agxMatrices[18] = float[18](
#include "Engine/Render/AgxMatrices.inc"
);

vec3 srgbToLinear(vec3 value)
{
    return mix(value/12.92,pow((value+0.055)/1.055,vec3(2.4)),greaterThan(value,vec3(0.04045)));
}

vec3 linearToSrgb(vec3 value)
{
    value = clamp(value,0.0,1.0);
    return mix(value*12.92,1.055*pow(value,vec3(1.0/2.4))-0.055,greaterThan(value,vec3(0.0031308)));
}

float agxCurveAt(float position)
{
    float scaled = clamp(position,0.0,1.0)*128.0;
    int index = min(int(scaled),127);
    return mix(agxCurve[index],agxCurve[index+1],scaled-float(index));
}

vec3 agxRow(int first,vec3 value)
{
    return vec3(dot(vec3(agxMatrices[first],agxMatrices[first+1],agxMatrices[first+2]),value),
                dot(vec3(agxMatrices[first+3],agxMatrices[first+4],agxMatrices[first+5]),value),
                dot(vec3(agxMatrices[first+6],agxMatrices[first+7],agxMatrices[first+8]),value));
}

// Linear Rec.709 scene color to sRGB-encoded display color through AgX.
vec3 agx(vec3 light)
{
    vec3 inset = agxRow(0,max(light,vec3(0.0)));
    vec3 position = (log2(max(inset,vec3(1e-10)))-agxLowStops)/(agxHighStops-agxLowStops);
    vec3 curved = pow(clamp(vec3(agxCurveAt(position.r),agxCurveAt(position.g),agxCurveAt(position.b)),0.0,1.0),vec3(2.4));
    return linearToSrgb(agxRow(9,curved));
}
