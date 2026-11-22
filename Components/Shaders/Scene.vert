#version 450
#extension GL_GOOGLE_include_directive : require
#include "Engine/Render/Shaders/SceneMotion.glsl"
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 color;
layout(location = 2) in float wind;
layout(location = 3) in float sunWeight;
layout(location = 4) in vec2 uv;
layout(location = 5) in uvec3 textureRange;
layout(location = 6) in vec3 normal;
layout(location = 7) in vec2 materialFactors;
layout(location = 8) in vec3 previousPosition; // the previous frame's positions (LiveViewport)
layout(location = 0) out vec3 shade;
layout(location = 1) out vec3 worldPosition;
layout(location = 2) out float directSun;
layout(location = 3) out vec2 surfaceUv;
layout(location = 4) flat out uvec3 baseColorRange;
layout(location = 5) out vec3 materialNormal;
layout(location = 6) flat out vec2 surfaceFactors;
layout(location = 7) out vec4 previousClip;
layout(location = 8) out vec4 currentClip;
layout(push_constant) uniform View {
    mat4 matrix;
    vec4 eye;
    vec4 fog;
    vec4 horizon;
    vec4 zenith;
} camera;
void main()
{
    vec3 displaced = swayed(position,wind,camera.eye.w);
    gl_Position = camera.matrix * vec4(displaced, 1.0);
    previousClip = motion.previousViewProjection*vec4(swayed(previousPosition,wind,motion.settings.w),1.0);
    currentClip = motion.previousViewProjection*vec4(displaced,1.0);
    shade = color;
    directSun = sunWeight;
    worldPosition = displaced;
    surfaceUv = uv;
    baseColorRange = textureRange;
    materialNormal = normal;
    surfaceFactors = materialFactors;
}
