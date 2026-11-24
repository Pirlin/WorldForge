#version 450
#extension GL_GOOGLE_include_directive : require
#include "PlayView.glsl"
#include "ColorPipeline.glsl"
#include "SceneMotion.glsl"
// Runtime scene shader. Lighting is per pixel in PlayScene.frag (Shading.glsl), shared with the editor.
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec3 color;
layout(location = 3) in float wind;
layout(location = 4) in vec2 uv;
layout(location = 5) in vec3 previousPosition; // the previous frame's vertex buffer (VulkanRenderer)
layout(location = 0) out vec3 worldPosition;
layout(location = 1) out vec3 albedo;
layout(location = 2) out vec2 surfaceUv;
layout(location = 3) out vec3 surfaceNormal;
layout(location = 4) out vec4 previousClip;
layout(location = 5) out vec4 currentClip;
void main()
{
    vec3 p = swayed(position,wind,view.time.x);
    worldPosition = p;
    gl_Position = view.viewProjection*vec4(p,1.0);
    previousClip = motion.previousViewProjection*vec4(swayed(previousPosition,wind,motion.settings.w),1.0);
    currentClip = motion.previousViewProjection*vec4(p,1.0);
    albedo = srgbToLinear(color);
    surfaceNormal = normal;
    surfaceUv = vec2(uv.x,1.0-uv.y); // package UVs go up; image rows go down
}
