#version 450
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 color;
layout(location = 2) in float wind;
layout(location = 3) in float sunWeight;
layout(location = 4) in vec2 uv;
layout(location = 5) in uvec3 textureRange;
layout(location = 6) in vec3 normal;
layout(location = 7) in vec2 materialFactors;
layout(location = 0) out vec3 shade;
layout(location = 1) out vec3 worldPosition;
layout(location = 2) out float directSun;
layout(location = 3) out vec2 surfaceUv;
layout(location = 4) flat out uvec3 baseColorRange;
layout(location = 5) out vec3 materialNormal;
layout(location = 6) flat out vec2 surfaceFactors;
layout(push_constant) uniform View {
    mat4 matrix;
    vec4 eye;
    vec4 fog;
    vec4 horizon;
    vec4 zenith;
} camera;
void main()
{
    vec3 displaced = position;
    float phase = dot(position.xy, vec2(0.36, 0.29)) + camera.eye.w * 1.7;
    displaced.xy += vec2(sin(phase), 0.6*cos(phase*0.83)) * wind;
    gl_Position = camera.matrix * vec4(displaced, 1.0);
    shade = color;
    directSun = sunWeight;
    worldPosition = displaced;
    surfaceUv = uv;
    baseColorRange = textureRange;
    materialNormal = normal;
    surfaceFactors = materialFactors;
}
