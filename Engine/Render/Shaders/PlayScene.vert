#version 450
#extension GL_GOOGLE_include_directive : require
#include "PlayView.glsl"
#include "ColorPipeline.glsl"
// Runtime scene shader. Lighting is per pixel in PlayScene.frag (Shading.glsl), shared with the editor.
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec3 color;
layout(location = 3) in float wind;
layout(location = 4) in vec2 uv;
layout(location = 0) out vec3 worldPosition;
layout(location = 1) out vec3 albedo;
layout(location = 2) out vec2 surfaceUv;
layout(location = 3) out vec3 surfaceNormal;
void main()
{
    vec3 p = position;
    float phase = dot(p.xy,vec2(0.36,0.29))+view.time.x*1.7;
    p.xy += vec2(sin(phase),0.6*cos(phase*0.83))*wind;
    worldPosition = p;
    gl_Position = view.viewProjection*vec4(p,1.0);
    albedo = srgbToLinear(color);
    surfaceNormal = normal;
    surfaceUv = uv;
}
