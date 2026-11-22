#version 450
#extension GL_GOOGLE_include_directive : require
#include "Lighting.glsl"
#include "Shading.glsl"
#include "ColorPipeline.glsl"
#include "PlayView.glsl"
layout(set = 1, binding = 0) uniform sampler2D baseColorMap;
layout(set = 1, binding = 1) uniform sampler2D normalMap;
layout(set = 1, binding = 2) uniform sampler2D metallicRoughnessMap;
layout(set = 1, binding = 3) uniform sampler2D occlusionMap;
layout(set = 1, binding = 4) uniform sampler2D emissiveMap;
layout(push_constant) uniform Material {
    vec4 emissive; // linear emission
    vec4 factors;  // metallic, roughness, advanced surface, normal map present
} material;
layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 albedo;
layout(location = 2) in vec2 surfaceUv;
layout(location = 3) in vec3 surfaceNormal;
layout(location = 4) in vec4 previousClip; // SceneMotion.glsl
layout(location = 5) in vec4 currentClip;
layout(location = 0) out vec4 color;
// The sky-light part of color, which ambient occlusion scales, and in alpha the depth along the camera's forward axis,
// which TAA keeps with its result for screen-space reflections (0 where only sky shows).
layout(location = 1) out vec4 indirectLight;
layout(location = 2) out vec4 surfaceAlbedo; // diffuse albedo and fog transmission, for bounce light
// This surface's own motion since the previous frame, apart from the camera's (HdrFrame::velocityFormat).
layout(location = 3) out vec4 surfaceMotion;

// The surface normal with the material's tangent-space normal map applied. The tangent frame comes from
// screen-space derivatives of the position and texture coordinates.
vec3 mappedNormal()
{
    vec3 n = normalize(surfaceNormal);
    vec3 positionX = dFdx(worldPosition), positionY = dFdy(worldPosition);
    vec2 uvX = dFdx(surfaceUv), uvY = dFdy(surfaceUv);
    vec3 sampled = texture(normalMap,surfaceUv).xyz*2.0-1.0;
    float determinant = uvX.x*uvY.y-uvX.y*uvY.x;
    if (material.factors.w > 0.0 && abs(determinant) > 1e-10 && dot(sampled,sampled) > 1e-6) {
        vec3 tangent = (positionX*uvY.y-positionY*uvX.y)*sign(determinant);
        vec3 bitangent = (positionY*uvX.x-positionX*uvY.x)*sign(determinant);
        tangent -= n*dot(tangent,n);
        if (dot(tangent,tangent) > 1e-14) {
            tangent = normalize(tangent);
            vec3 axis = cross(n,tangent);
            bitangent = axis*(dot(axis,bitangent) < 0.0 ? -1.0 : 1.0);
            n = normalize(sampled.x*tangent+sampled.y*bitangent+sampled.z*n);
        }
    }
    return n;
}

void main()
{
    vec3 packed = texture(metallicRoughnessMap,surfaceUv).rgb;
    Surface surface;
    surface.albedo = albedo*srgbToLinear(texture(baseColorMap,surfaceUv).rgb);
    surface.normal = mappedNormal();
    surface.metallic = clamp(material.factors.x*packed.b,0.0,1.0);
    surface.roughness = clamp(material.factors.y*packed.g,0.045,1.0);
    surface.occlusion = texture(occlusionMap,surfaceUv).r;
    vec3 indirect;
    vec3 lit = shadeSurface(surface,worldPosition,view.eye.xyz,sunVisibility(worldPosition,surfaceNormal,true),indirect)
        +material.emissive.rgb*srgbToLinear(texture(emissiveMap,surfaceUv).rgb);

    vec3 ray = worldPosition-view.eye.xyz;
    float rayLength = length(ray);
    vec3 direction = ray/max(rayLength,0.0001);
    vec3 tint = srgbToLinear(view.fog.rgb);
    vec4 fog = heightFog(view.eye.xyz,direction,rayLength,tint,view.fog.w,view.horizon.w,view.zenith.w);
    vec4 mist = localMist(view.eye.xyz,direction,rayLength,tint,view.sun.w);
    color = vec4((lit*fog.a+fog.rgb)*(1.0-mist.a)+mist.rgb,1.0);
    float transmission = fog.a*(1.0-mist.a);
    indirectLight = vec4(indirect*transmission,max(dot(ray,lighting.clusterForward.xyz),1e-3));
    surfaceAlbedo = vec4(surface.albedo*(1.0-surface.metallic),transmission);
    surfaceMotion = vec4(surfaceMotionOf(previousClip,currentClip),0.0,0.0);
}
