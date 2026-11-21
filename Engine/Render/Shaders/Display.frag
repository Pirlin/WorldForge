#version 450
#extension GL_GOOGLE_include_directive : require
#include "ColorPipeline.glsl"
// Converts the linear HDR scene to the display: exposure, view transform, and dither. For auto
// exposure it also counts every fourth pixel's luminance into a histogram (see meteredLuminance in
// Engine/Render/ColorPipeline.h for the bins).
layout(input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput scene;
layout(std430, set = 0, binding = 1) buffer Meter { uint bins[64]; } meter;
layout(push_constant) uniform Display {
    vec4 settings; // exposure scale, view transform (0 Standard, 1 AgX), sRGB swap chain, metering
} display;
layout(location = 0) out vec4 color;
void main()
{
    vec3 scene = subpassLoad(scene).rgb;
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    if (display.settings.w > 0.5 && ((pixel.x | pixel.y) & 1) == 0) {
        const float low = -16.0, high = 8.0;
        float luminance = dot(max(scene,vec3(0.0)),vec3(0.2126,0.7152,0.0722));
        float stops = log2(max(luminance,1e-12));
        int bin = stops < low ? 0 : clamp(1+int((stops-low)*(63.0/(high-low))),1,63);
        atomicAdd(meter.bins[bin],1u);
    }
    vec3 light = scene*display.settings.x;
    vec3 encoded = display.settings.y > 0.5 ? agx(light) : linearToSrgb(light);
    // Half a level of ordered noise keeps 8-bit sky gradients from banding.
    float noise = fract(52.9829189*fract(dot(gl_FragCoord.xy,vec2(0.06711056,0.00583715))));
    encoded = clamp(encoded+(noise-0.5)/255.0,0.0,1.0);
    color = vec4(display.settings.z > 0.5 ? srgbToLinear(encoded) : encoded,1.0);
}
