#version 450
#extension GL_GOOGLE_include_directive : require
#include "Lighting.glsl"
#define MATERIAL_PARALLAX
#include "Shading.glsl"
layout(constant_id = 1) const int materialPass = 0;
#include "ColorPipeline.glsl"
#include "PlayView.glsl"
layout(set = 1, binding = 0) uniform sampler2D baseColorMap;
layout(set = 1, binding = 1) uniform sampler2D normalMap;
layout(set = 1, binding = 2) uniform sampler2D metallicRoughnessMap;
layout(set = 1, binding = 3) uniform sampler2D occlusionMap;
layout(set = 1, binding = 4) uniform sampler2D emissiveMap;
layout(set = 1, binding = 5) uniform sampler2D coatWeightMap;
layout(set = 1, binding = 6) uniform sampler2D coatRoughnessMap;
layout(set = 1, binding = 7) uniform sampler2D coatNormalMap;
layout(set = 1, binding = 8) uniform sampler2D sheenColorMap;
layout(set = 1, binding = 9) uniform sampler2D sheenRoughnessMap;
layout(set = 1, binding = 10) uniform sampler2D transmissionMap;
layout(set = 1, binding = 11) uniform sampler2D anisotropyMap;
layout(push_constant) uniform Material {
    vec4 emissive; // linear emission
    vec4 factors;  // metallic, roughness, advanced surface, normal map present
    vec4 sheen;    // linear reflectance RGB, roughness
    vec4 scattering;
    vec2 clearCoat; // weight, roughness
    vec2 optics;   // transmission, index of refraction
    float anisotropyRotation;
    float coatNormalScale;
    uint textureFlags;
    vec4 streetLayers; // wetness, puddles, UV relief, decal opacity (-1 when off)
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
vec3 mappedNormal(vec2 uv)
{
    vec3 n = normalize(surfaceNormal);
    vec3 positionX = dFdx(worldPosition), positionY = dFdy(worldPosition);
    vec2 uvX = dFdx(surfaceUv), uvY = dFdy(surfaceUv);
    vec3 sampled = texture(normalMap,uv).xyz*2.0-1.0;
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

float parallaxSample(vec2 uv,vec2 gradientsX,vec2 gradientsY)
{
    return 1.0-textureGrad(occlusionMap,uv,gradientsX,gradientsY).g;
}

void main()
{
    vec3 geometryNormal = normalize(surfaceNormal);
    vec2 uv = surfaceUv;
    if (material.streetLayers.z > 0.0 && (material.textureFlags & (1u << 3)) != 0u) {
        vec3 tangentView = transpose(surfaceTangentFrame(geometryNormal,worldPosition,surfaceUv))
            *normalize(view.eye.xyz-worldPosition);
        uv = parallaxOcclusion(surfaceUv,tangentView,material.streetLayers.z);
    }
    vec3 packed = texture(metallicRoughnessMap,uv).rgb;
    vec4 base = texture(baseColorMap,uv);
    vec3 ao = texture(occlusionMap,uv).rgb;
    Surface surface;
    surface.albedo = albedo*srgbToLinear(base.rgb);
    surface.normal = mappedNormal(uv);
    surface.metallic = clamp(material.factors.x*packed.b,0.0,1.0);
    surface.roughness = clamp(material.factors.y*packed.g,0.045,1.0);
    surface.occlusion = ao.r;
    surface.coatNormal = geometryNormal;
    surface.clearCoat = material.clearCoat;
    if (surface.clearCoat.x > 0.0) {
        surface.clearCoat *= vec2(texture(coatWeightMap,uv).r,texture(coatRoughnessMap,uv).g);
        if ((material.textureFlags & (1u << 7)) != 0u)
            surface.coatNormal = layeredCoatNormal(surfaceTangentFrame(surface.coatNormal,worldPosition,surfaceUv),
                texture(coatNormalMap,uv).rgb,material.coatNormalScale);
    }
    surface.clearCoat.y = clamp(surface.clearCoat.y,0.045,1.0);
    surface.sheen = material.sheen;
    if (maxComponent(surface.sheen.rgb) > 0.0)
        surface.sheen *= vec4(srgbToLinear(texture(sheenColorMap,uv).rgb),texture(sheenRoughnessMap,uv).a);
    surface.optics = vec2(materialPass == 0 ? 0.0 : material.optics.x*texture(transmissionMap,uv).r,material.optics.y);
    surface.scattering = material.scattering;
    surface.tangent = vec3(0.0);
    if (surface.scattering.w > 0.0) {
        vec3 brush = (material.textureFlags & (1u << 11)) != 0u ? texture(anisotropyMap,uv).rgb : vec3(1.0,0.5,1.0);
        surface.scattering.w *= brush.b;
        surface.tangent = anisotropyTangent(surfaceTangentFrame(surface.normal,worldPosition,surfaceUv),
            brush.rg,material.anisotropyRotation);
    }
    applyWetSurface(surface,worldPosition,geometryNormal,material.streetLayers.x,material.streetLayers.y,ao.b);
    float coverage = material.streetLayers.w < 0.0 ? 1.0 : base.a*material.streetLayers.w;
    vec3 behind = vec3(0.0);
    if (materialPass != 0)
        behind = mix(vec3(1.0),thinGlassTransmission(surface,normalize(view.eye.xyz-worldPosition)),coverage);
    if (materialPass == 1) {
        color = indirectLight = surfaceAlbedo = vec4(behind,1.0);
        surfaceMotion = vec4(1.0);
        return;
    }
    vec3 indirect, diffuseAlbedo;
    vec3 lit = shadeSurface(surface,worldPosition,view.eye.xyz,surfaceNormal,indirect,diffuseAlbedo)
        +material.emissive.rgb*srgbToLinear(texture(emissiveMap,uv).rgb);

    vec3 ray = worldPosition-view.eye.xyz;
    float rayLength = length(ray);
    vec3 direction = ray/max(rayLength,0.0001);
    vec3 tint = srgbToLinear(view.fog.rgb);
    vec4 fog = surfaceFog(view.eye.xyz,direction,rayLength,tint,view.fog.w,view.horizon.w,view.zenith.w);
    vec4 mist = localMist(view.eye.xyz,direction,rayLength,tint,view.sun.w);
    if (materialPass == 2) {
        vec3 frontFog = fog.rgb*(1.0-mist.a)+mist.rgb;
        color = vec4(coverage*lit*fog.a*(1.0-mist.a)+frontFog*(1.0-behind),1.0);
        indirectLight = surfaceAlbedo = surfaceMotion = vec4(0.0);
        return;
    }
    color = vec4((lit*fog.a+fog.rgb)*(1.0-mist.a)+mist.rgb,1.0);
    float transmission = fog.a*(1.0-mist.a);
    indirectLight = vec4(indirect*transmission,max(dot(ray,lighting.clusterForward.xyz),1e-3));
    surfaceAlbedo = vec4(diffuseAlbedo,transmission);
    surfaceMotion = vec4(surfaceMotionOf(previousClip,currentClip),0.0,0.0);
}
