#version 450
#extension GL_GOOGLE_include_directive : require
#include "Lighting.glsl"
#include "ColorPipeline.glsl"
layout(early_fragment_tests) in;
layout(location = 0) in vec2 screen;
layout(location = 0) out vec4 color;
layout(push_constant) uniform View {
    mat4 matrix;
    vec4 eye;
    vec4 fog;
    vec4 horizon;
    vec4 zenith;
} camera;
void main()
{
    vec4 world = camera.matrix * vec4(screen, 1.0, 1.0);
    vec3 direction = normalize(world.xyz / world.w - camera.eye.xyz);
    float height = pow(max(direction.z, 0.0), 0.55);
    // Sky and fog colors are authored in sRGB; the sky is lit in linear HDR.
    vec3 horizon = srgbToLinear(camera.horizon.rgb), zenith = srgbToLinear(camera.zenith.rgb);
    vec3 tint = srgbToLinear(camera.fog.rgb);
    float intensity = lighting.sun.w;
    float towardSun = max(0.0,dot(direction,lighting.sun.xyz));
    vec3 sky;
    if (lighting.skySettings.z > 0.5) {
        // The physical atmosphere (Engine/Render/Atmosphere.h), with the sun's disc in the color the air gives it.
        sky = skyEnvironment(direction, 0);
        sky += intensity * lighting.sunTint.rgb * 50.0 * smoothstep(0.99996, 0.99999, towardSun);
    } else {
        sky = mix(horizon, zenith, height);
        sky = mix(sky, horizon * srgbToLinear(vec3(0.67, 0.74, 0.80)), smoothstep(0.0, 0.8, -direction.z));
        sky += intensity * srgbToLinear(vec3(0.42, 0.25, 0.12)) * pow(towardSun, 32.0);
        sky += intensity * srgbToLinear(vec3(0.95, 0.78, 0.56)) * smoothstep(0.99965, 0.9999, towardSun);
    }
    const float skyDistance = 2000.0;
    vec4 fog = heightFog(camera.eye.xyz,direction,skyDistance,tint,camera.fog.w,camera.horizon.w,camera.zenith.w);
    vec4 local = localMist(camera.eye.xyz,direction,skyDistance,tint,intensity);
    color = vec4((sky*fog.a+fog.rgb)*(1.0-local.a)+local.rgb,1.0);
}
