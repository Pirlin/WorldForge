#version 450
#extension GL_GOOGLE_include_directive : require
#include "Engine/Render/Shaders/Lighting.glsl"
#define MATERIAL_PARALLAX
#include "Engine/Render/Shaders/Shading.glsl"
#include "Engine/Render/Shaders/ColorPipeline.glsl"
// The scene pass writes linear HDR light. Editor lines and overlays draw after the display
// transform, so their pipelines set displayPass and keep sRGB-encoded colors.
layout(constant_id = 0) const bool displayPass = false;
layout(constant_id = 1) const int materialPass = 0;
layout(location = 0) in vec3 shade;
layout(location = 1) in vec3 worldPosition;
layout(location = 2) in float directSun;
layout(location = 3) in vec2 surfaceUv;
layout(location = 4) flat in uvec3 baseColorRange;
layout(location = 5) in vec3 materialNormal;
layout(location = 6) flat in vec2 surfaceFactors;
layout(location = 7) in vec4 previousClip; // SceneMotion.glsl
layout(location = 8) in vec4 currentClip;
layout(set = 0, binding = 2, std430) readonly buffer MaterialImages { uint pixels[]; } images;
layout(location = 0) out vec4 color;
// The sky-light part of color, which ambient occlusion scales, and in alpha the depth along the camera's forward axis,
// which TAA keeps with its result for screen-space reflections (0 where only sky shows).
layout(location = 1) out vec4 indirectLight;
layout(location = 2) out vec4 surfaceAlbedo; // diffuse albedo and fog transmission, for bounce light
// This surface's own motion since the previous frame, apart from the camera's (HdrFrame::velocityFormat).
layout(location = 3) out vec4 surfaceMotion;
layout(push_constant) uniform View {
    mat4 matrix;
    vec4 eye;
    vec4 fog;
    vec4 horizon;
    vec4 zenith;
} camera;
uvec3 imageRange(uint channel)
{
    // Base-color-only geometry passes its image directly.
    if (baseColorRange.y > 0)
        return channel == 0 ? baseColorRange : uvec3(0);
    if (baseColorRange.x == 0)
        return uvec3(0);
    uint offset = baseColorRange.x+3*channel;
    return uvec3(images.pixels[offset],images.pixels[offset+1],images.pixels[offset+2]);
}
vec3 working(vec3 encoded)
{
    return displayPass ? encoded : srgbToLinear(encoded);
}
vec4 texel(ivec2 p,uvec3 range,bool encoded)
{
    ivec2 size = ivec2(range.yz);
    ivec2 wrapped = (p + size) % size;
    vec4 value = unpackUnorm4x8(images.pixels[range.x+uint(wrapped.y*size.x+wrapped.x)]);
    return encoded ? vec4(working(value.rgb),value.a) : value;
}
vec4 materialTexelAt(uint channel,vec4 fallback,vec2 uv)
{
    uvec3 range = imageRange(channel);
    if (range.y == 0)
        return fallback;
    vec2 p = fract(uv)*vec2(range.yz)-0.5;
    ivec2 lower = ivec2(floor(p));
    vec2 t = fract(p);
    // Base color and emission maps are sRGB-encoded; the others hold linear data.
    bool encoded = channel == 0 || channel == 4 || channel == 8;
    return mix(mix(texel(lower,range,encoded),texel(lower+ivec2(1,0),range,encoded),t.x),
        mix(texel(lower+ivec2(0,1),range,encoded),texel(lower+ivec2(1,1),range,encoded),t.x),t.y);
}
vec2 shadingUv;
vec4 materialTexel(uint channel,vec4 fallback)
{
    return materialTexelAt(channel,fallback,shadingUv);
}

float parallaxSample(vec2 uv,vec2 gradientsX,vec2 gradientsY)
{
    return 1.0-materialTexelAt(3,vec4(1.0),uv).g;
}

vec3 materialImage(uint channel,vec3 fallback)
{
    return materialTexel(channel,vec4(fallback,1.0)).rgb;
}
// The surface normal with the material normal map applied, using a tangent frame from screen-space derivatives.
vec3 mappedNormal()
{
    vec3 n = normalize(materialNormal);
    vec3 positionX = dFdx(worldPosition), positionY = dFdy(worldPosition);
    vec2 uvX = dFdx(surfaceUv), uvY = dFdy(surfaceUv);
    if (imageRange(1).y > 0) {
        float determinant = uvX.x*uvY.y-uvX.y*uvY.x;
        if (abs(determinant) > 1e-10) {
            vec3 tangent = (positionX*uvY.y-positionY*uvX.y)*sign(determinant);
            vec3 bitangent = (positionY*uvX.x-positionX*uvY.x)*sign(determinant);
            tangent -= n*dot(tangent,n);
            if (dot(tangent,tangent) > 1e-14) {
                tangent = normalize(tangent);
                vec3 crossAxis = cross(n,tangent);
                bitangent = crossAxis*(dot(crossAxis,bitangent) < 0.0 ? -1.0 : 1.0);
                vec3 sampled = materialImage(1,vec3(0.5,0.5,1.0))*2.0-1.0;
                if (dot(sampled,sampled) > 1e-6)
                    n = normalize(mat3(tangent,bitangent,n)*sampled);
            }
        }
    }
    return n;
}

const uint materialFactorsOffset = 3*12;

vec3 surfaceEmission()
{
    vec3 emission = vec3(0.0);
    if (baseColorRange.x > 0 && baseColorRange.y == 0) {
        uint offset = baseColorRange.x+materialFactorsOffset;
        emission = uintBitsToFloat(uvec3(images.pixels[offset],images.pixels[offset+1],images.pixels[offset+2]));
    }
    return emission*materialImage(4,vec3(1.0));
}

vec2 materialClearCoat()
{
    if (baseColorRange.x == 0 || baseColorRange.y != 0)
        return vec2(0.0);
    uint offset = baseColorRange.x+materialFactorsOffset+3;
    vec2 coat = uintBitsToFloat(uvec2(images.pixels[offset],images.pixels[offset+1]));
    if (coat.x > 0.0)
        coat *= vec2(materialImage(5,vec3(1.0)).r,materialImage(6,vec3(1.0)).g);
    return vec2(coat.x,clamp(coat.y,0.045,1.0));
}

vec3 materialCoatNormal()
{
    vec3 n = normalize(materialNormal);
    if (imageRange(7).y > 0) {
        float scale = uintBitsToFloat(images.pixels[baseColorRange.x+materialFactorsOffset+16]);
        n = layeredCoatNormal(surfaceTangentFrame(n,worldPosition,surfaceUv),materialImage(7,vec3(0.5,0.5,1.0)),scale);
    }
    return n;
}

vec4 materialSheen()
{
    if (baseColorRange.x == 0 || baseColorRange.y != 0)
        return vec4(0.0);
    uint i = baseColorRange.x+materialFactorsOffset+5;
    vec4 sheen = uintBitsToFloat(uvec4(images.pixels[i],images.pixels[i+1],images.pixels[i+2],images.pixels[i+3]));
    if (maxComponent(sheen.rgb) > 0.0)
        sheen *= vec4(materialImage(8,vec3(1.0)),materialTexel(9,vec4(1.0)).a);
    return sheen;
}

vec2 materialOptics()
{
    if (baseColorRange.x == 0 || baseColorRange.y != 0)
        return vec2(0.0,1.5);
    uint i = baseColorRange.x+materialFactorsOffset+9;
    vec2 optics = uintBitsToFloat(uvec2(images.pixels[i],images.pixels[i+1]));
    if (optics.x > 0.0)
        optics.x *= materialImage(10,vec3(1.0)).r;
    return optics;
}

vec4 materialScattering()
{
    if (baseColorRange.x == 0 || baseColorRange.y != 0)
        return vec4(0.0);
    uint i = baseColorRange.x+materialFactorsOffset+11;
    return uintBitsToFloat(uvec4(images.pixels[i],images.pixels[i+1],images.pixels[i+2],images.pixels[i+3]));
}

vec4 materialStreetLayers()
{
    if (baseColorRange.x == 0 || baseColorRange.y != 0)
        return vec4(0.0,0.0,0.0,-1.0);
    uint i = baseColorRange.x+materialFactorsOffset+17;
    return uintBitsToFloat(uvec4(images.pixels[i],images.pixels[i+1],images.pixels[i+2],images.pixels[i+3]));
}

// Modeling views: a fixed studio light and constant ambient.
vec3 materialLighting(vec3 albedo,float visibility)
{
    const float pi = 3.14159265359;
    vec3 n = mappedNormal();
    bool worldMaterial = baseColorRange.z == 1;
    vec3 light = worldMaterial ? camera.horizon.xyz : normalize(vec3(-0.45, -0.7, 1.0));
    vec3 view = normalize(camera.eye.xyz-worldPosition);
    vec3 halfVector = light+view;
    vec3 h = halfVector/max(length(halfVector),0.0001);
    float nl = max(dot(n,light),0.0);
    float nv = max(dot(n,view),0.0001);
    float nh = max(dot(n,h),0.0);
    float vh = clamp(dot(view,h),0.0,1.0);
    vec3 mr = materialImage(2,vec3(1.0));
    float metallic = surfaceFactors.x*mr.b;
    float roughness = max(0.045,surfaceFactors.y*mr.g);
    float a = roughness*roughness;
    float a2 = a*a;
    float denominator = nh*nh*(a2-1.0)+1.0;
    float distribution = a2/(pi*denominator*denominator);
    float k = (roughness+1.0)*(roughness+1.0)/8.0;
    float geometry = nl/(nl*(1.0-k)+k)*nv/(nv*(1.0-k)+k);
    vec3 f0 = mix(vec3(dielectricF0(materialOptics().y)),albedo,metallic);
    vec3 f = f0+(1.0-f0)*pow(1.0-vh,5.0);
    vec3 diffuse = (1.0-f)*(1.0-metallic)*albedo/pi;
    vec3 specular = distribution*geometry*f/max(4.0*nl*nv,0.0001);
    float strength = worldMaterial ? 2.1*camera.zenith.x*visibility : 2.1;
    vec3 shaded = lighting.settings.w*albedo*mix(0.34,0.18,metallic)*materialImage(3,vec3(1.0)).r
        +strength*(diffuse+specular)*nl;
    vec4 sheen = materialSheen();
    if (maxComponent(sheen.rgb) > 0.0) {
        shaded *= 1.0-maxComponent(sheen.rgb)*sheenAlbedo(nv,sheen.w);
        shaded += strength*sheen.rgb*sheenDirect(nv,nl,nh,sheen.w)/pi;
    }
    vec2 coat = materialClearCoat();
    vec3 coatNormal = materialCoatNormal();
    if (coat.x > 0.0) {
        if (dot(coatNormal,view) < 0.0)
            coatNormal = -coatNormal;
        shaded *= 1.0-coat.x*clearCoatFresnel(dot(coatNormal,view));
        shaded += strength*coat.x*clearCoatDirect(coatNormal,view,light,coat.y)/pi;
    }
    return shaded+surfaceEmission();
}
void main()
{
    shadingUv = surfaceUv;
    vec4 streetLayers = materialStreetLayers();
    if (streetLayers.z > 0.0 && imageRange(3).y > 0 && dot(materialNormal,materialNormal) > 0.001) {
        vec3 tangentView = transpose(surfaceTangentFrame(normalize(materialNormal),worldPosition,surfaceUv))
            *normalize(camera.eye.xyz-worldPosition);
        shadingUv = parallaxOcclusion(surfaceUv,tangentView,streetLayers.z);
    }
    vec4 base = materialTexel(0,vec4(1.0));
    vec3 surface = shade * base.rgb;
    float coverage = streetLayers.w < 0.0 ? 1.0 : base.a*streetLayers.w;
    vec3 indirect = vec3(0.0);
    vec3 diffuseAlbedo = vec3(0.0);
    vec3 behind = vec3(0.0);
    vec4 scattering = materialScattering();
    if ((camera.zenith.y > 0.5 || materialPass != 0 || scattering.x > 0.0 || scattering.w > 0.0
        || streetLayers.x > 0.0 || streetLayers.y > 0.0)
        && dot(materialNormal,materialNormal) > 0.001) {
        // World views: the physically based model shared with the Player.
        vec3 packed = materialImage(2,vec3(1.0));
        Surface material;
        material.albedo = surface;
        material.normal = mappedNormal();
        material.metallic = clamp(surfaceFactors.x*packed.b,0.0,1.0);
        material.roughness = clamp(surfaceFactors.y*packed.g,0.045,1.0);
        vec3 ao = materialImage(3,vec3(1.0));
        material.occlusion = ao.r;
        material.clearCoat = materialClearCoat();
        material.coatNormal = materialCoatNormal();
        material.sheen = materialSheen();
        material.optics = materialOptics();
        material.scattering = scattering;
        material.tangent = vec3(0.0);
        if (scattering.w > 0.0) {
            float angle = uintBitsToFloat(images.pixels[baseColorRange.x+materialFactorsOffset+15]);
            vec3 brush = materialImage(11,vec3(1.0,0.5,1.0));
            material.scattering.w *= brush.b;
            material.tangent = anisotropyTangent(surfaceTangentFrame(material.normal,worldPosition,surfaceUv),brush.rg,angle);
        }
        applyWetSurface(material,worldPosition,normalize(materialNormal),streetLayers.x,streetLayers.y,ao.b);
        if (materialPass != 0)
            behind = mix(vec3(1.0),thinGlassTransmission(material,normalize(camera.eye.xyz-worldPosition)),coverage);
        else
            material.optics.x = 0.0;
        if (materialPass == 1) {
            color = indirectLight = surfaceAlbedo = vec4(behind,1.0);
            surfaceMotion = vec4(1.0);
            return;
        }
        surface = shadeSurface(material,worldPosition,camera.eye.xyz,materialNormal,indirect,diffuseAlbedo)+surfaceEmission();
    } else if (surfaceFactors.y > 0.0 && dot(materialNormal,materialNormal) > 0.001)
        surface = materialLighting(surface,sunVisibility(worldPosition,materialNormal,true));
    else
        surface *= (1.0 - directSun*(1.0-sunVisibility(worldPosition,materialNormal,true)));
    vec3 ray = worldPosition-camera.eye.xyz;
    float rayLength = length(ray);
    vec3 direction = ray/max(rayLength,0.0001);
    vec3 tint = working(camera.fog.rgb);
    vec4 fog = surfaceFog(camera.eye.xyz,direction,rayLength,tint,camera.fog.w,camera.horizon.w,camera.zenith.w);
    vec4 local = localMist(camera.eye.xyz,direction,rayLength,tint,camera.zenith.x);
    if (materialPass == 2) {
        vec3 frontFog = fog.rgb*(1.0-local.a)+local.rgb;
        color = vec4(coverage*surface*fog.a*(1.0-local.a)+frontFog*(1.0-behind),1.0);
        indirectLight = surfaceAlbedo = surfaceMotion = vec4(0.0);
        return;
    }
    color = vec4((surface*fog.a+fog.rgb)*(1.0-local.a)+local.rgb,1.0);
    float transmission = fog.a*(1.0-local.a);
    indirectLight = vec4(indirect*transmission,max(dot(ray,lighting.clusterForward.xyz),1e-3));
    surfaceAlbedo = vec4(diffuseAlbedo,transmission);
    surfaceMotion = vec4(surfaceMotionOf(previousClip,currentClip),0.0,0.0);
}
