#version 450
#extension GL_GOOGLE_include_directive : require
#include "Lighting.glsl"
layout(location = 0) in vec3 shade;
layout(location = 1) in vec3 worldPosition;
layout(location = 2) in float directSun;
layout(location = 3) in vec2 surfaceUv;
layout(location = 4) flat in uvec3 baseColorRange;
layout(location = 5) in vec3 materialNormal;
layout(location = 6) flat in vec2 surfaceFactors;
layout(set = 0, binding = 2, std430) readonly buffer MaterialImages { uint pixels[]; } images;
layout(location = 0) out vec4 color;
layout(push_constant) uniform View {
    mat4 matrix;
    vec4 eye;
    vec4 fog;
    vec4 horizon;
    vec4 zenith;
} camera;
uvec3 imageRange(uint channel)
{
    // World geometry passes the base-color image directly. Studio uses a material table.
    if (baseColorRange.y > 0)
        return channel == 0 ? baseColorRange : uvec3(0);
    if (baseColorRange.x == 0)
        return uvec3(0);
    uint offset = baseColorRange.x+3*channel;
    return uvec3(images.pixels[offset],images.pixels[offset+1],images.pixels[offset+2]);
}
vec3 texel(ivec2 p,uvec3 range)
{
    ivec2 size = ivec2(range.yz);
    ivec2 wrapped = (p + size) % size;
    return unpackUnorm4x8(images.pixels[range.x+uint(wrapped.y*size.x+wrapped.x)]).rgb;
}
vec3 materialImage(uint channel,vec3 fallback)
{
    uvec3 range = imageRange(channel);
    if (range.y == 0)
        return fallback;
    vec2 p = fract(surfaceUv)*vec2(range.yz)-0.5;
    ivec2 lower = ivec2(floor(p));
    vec2 t = fract(p);
    return mix(mix(texel(lower,range),texel(lower+ivec2(1,0),range),t.x),
        mix(texel(lower+ivec2(0,1),range),texel(lower+ivec2(1,1),range),t.x),t.y);
}
vec3 studioLighting(vec3 albedo)
{
    const float pi = 3.14159265359;
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
    vec3 light = normalize(vec3(-0.45, -0.7, 1.0));
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
    vec3 f0 = mix(vec3(0.04),albedo,metallic);
    vec3 f = f0+(1.0-f0)*pow(1.0-vh,5.0);
    vec3 diffuse = (1.0-f)*(1.0-metallic)*albedo/pi;
    vec3 specular = distribution*geometry*f/max(4.0*nl*nv,0.0001);
    vec3 emission = vec3(0.0);
    if (baseColorRange.x > 0) {
        uint offset = baseColorRange.x+15;
        emission = uintBitsToFloat(uvec3(images.pixels[offset],images.pixels[offset+1],images.pixels[offset+2]));
    }
    return albedo*mix(0.34,0.18,metallic)*materialImage(3,vec3(1.0)).r
        +2.1*(diffuse+specular)*nl+emission*materialImage(4,vec3(1.0));
}
void main()
{
    float falloff = camera.horizon.w;
    float base = camera.zenith.w;
    float eyeDensity = clamp(-(camera.eye.z - base) * falloff, -15.0, 10.0);
    float surfaceDensity = clamp(-(worldPosition.z - base) * falloff, -15.0, 10.0);
    float difference = eyeDensity - surfaceDensity;
    float meanDensity = abs(difference) < 0.001 ? exp((eyeDensity + surfaceDensity) * 0.5)
        : (exp(eyeDensity) - exp(surfaceDensity)) / difference;
    vec3 ray = worldPosition - camera.eye.xyz;
    float rayLength = length(ray);
    float amount = 1.0 - exp(-min(40.0, camera.fog.w * meanDensity * rayLength));
    float towardSun = max(0.0, dot(ray / max(rayLength, 0.0001), camera.horizon.xyz));
    float scattering = min(1.0, camera.zenith.x) * pow(towardSun, 12.0) * 0.22;
    vec3 inscatter = mix(camera.fog.rgb, vec3(1.0, 0.83, 0.64), scattering);
    float visibility = sunVisibility(worldPosition,true);
    vec3 surface = shade * materialImage(0,vec3(1.0)) * (1.0 - directSun*(1.0-visibility));
    if (surfaceFactors.y > 0.0 && dot(materialNormal,materialNormal) > 0.001)
        surface = studioLighting(surface);
    vec3 haze = mix(surface,inscatter,amount);
    vec4 local = localMist(camera.eye.xyz,ray/max(rayLength,0.0001),rayLength,camera.fog.rgb,camera.zenith.x);
    color = vec4(haze*(1.0-local.a)+local.rgb,1.0);
}
