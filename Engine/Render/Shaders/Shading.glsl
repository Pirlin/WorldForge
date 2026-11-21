// Physically based shading of a surface point, shared by the editor viewport and the Player.
// Include after Lighting.glsl, whose Lighting block supplies the sun, the sky light and the lights.
//
// Units are scene units (Engine/Render/ColorPipeline.h): a light of intensity I facing a white
// diffuse surface makes it reflect radiance I (point lights: I at 1 m, falling off with the inverse
// square). Indirect light comes from the sky (Engine/Render/SkyLight.h): its spherical-harmonic
// irradiance for diffuse light, and the sky in the reflected direction for specular light.

const float shadingPi = 3.14159265359;

struct Surface {
    vec3 albedo;
    vec3 normal;     // unit length, world space
    float metallic;
    float roughness; // perceptual, 0.045 to 1
    float occlusion; // ambient occlusion from a material map, 1 when none
};

// Irradiance / pi from the sky in direction n: the radiance a white diffuse surface facing n reflects.
vec3 skyIrradiance(vec3 n)
{
    float basis[9] = float[9](0.282095,0.488603*n.y,0.488603*n.z,0.488603*n.x,1.092548*n.x*n.y,1.092548*n.y*n.z,
                              0.315392*(3.0*n.z*n.z-1.0),1.092548*n.x*n.z,0.546274*(n.x*n.x-n.y*n.y));
    vec3 result = vec3(0.0);
    for (int i = 0; i < 9; ++i)
        result += lighting.skyIrradiance[i].rgb*basis[i];
    return max(result,vec3(0.0));
}

// The sky seen in direction r, without the sun glow (direct sunlight covers the highlight).
vec3 skyRadianceToward(vec3 r)
{
    if (r.z < 0.0)
        return lighting.skyGround.rgb;
    return mix(lighting.skyHorizon.rgb,lighting.skyZenith.rgb,pow(r.z,0.55));
}

// The share of sky light that reaches position on a surface facing normal: each sky direction counts by its
// brightness times the cosine to the normal (plus a little, so surfaces facing down still see the sky around
// them), and only when its depth map shows nothing between the point and the sky. 1 when the maps are off.
float skyOpenness(vec3 position,vec3 normal)
{
    if (lighting.skySettings.x < 0.5)
        return 1.0;
    float texel = lighting.skySettings.y;
    float texels = float(textureSize(skyVisibilityMap,0).x)*0.25;
    // Off the surface by about two texels; the lookup moves within a texel each TAA frame, which smooths the
    // coarse maps into soft occlusion.
    vec3 lifted = position+normal*texel*2.0;
    vec2 pixel = gl_FragCoord.xy+5.588238*lighting.settings.z;
    float noise = fract(52.9829189*fract(dot(pixel,vec2(0.06711056,0.00583715))));
    vec2 jitter = (vec2(noise,fract(noise*7.31+0.37))-0.5)/texels;
    float open = 0.0, total = 0.0;
    for (int i = 0; i < 16; ++i) {
        vec3 direction = lighting.skyDirections[i].xyz;
        float weight = dot(skyRadianceToward(direction),vec3(0.2126,0.7152,0.0722))*(max(dot(normal,direction),0.0)+0.05);
        vec3 c = (lighting.skyMatrices[i]*vec4(lifted,1.0)).xyz;
        vec2 uv = c.xy*0.5+0.5+jitter;
        float visible = 1.0;
        if (all(greaterThan(uv,vec2(0.0))) && all(lessThan(uv,vec2(1.0))) && c.z > 0.0 && c.z < 1.0) {
            uv = clamp(uv,vec2(0.5/texels),vec2(1.0-0.5/texels));
            float depth = textureLod(skyVisibilityMap,(uv+vec2(i%4,i/4))*0.25,0.0).r;
            visible = step(c.z,depth);
        }
        open += weight*visible;
        total += weight;
    }
    return total > 0.0 ? open/total : 1.0;
}

// The split-sum environment BRDF, analytic fit (Karis 2014, after Lazarov 2013): scale and bias for F0.
vec2 environmentBrdf(float roughness,float nv)
{
    const vec4 c0 = vec4(-1.0,-0.0275,-0.572,0.022);
    const vec4 c1 = vec4(1.0,0.0425,1.04,-0.04);
    vec4 r = roughness*c0+c1;
    float a = min(r.x*r.x,exp2(-9.28*nv))*r.x+r.y;
    return vec2(-1.04,1.04)*a+r.zw;
}

// Light arriving from direction l with intensity in scene units: GGX specular with the height-correlated Smith
// visibility and Schlick Fresnel, plus Lambert diffuse for what the surface does not reflect. This is the model
// Cycles' Principled BSDF uses (alpha = roughness^2, no roughness remapping), so grazing sunlight on rough
// ground reflects as brightly as in the reference. energy scales the specular lobe for the light that bounces
// between microfacets (multiple scattering), which a single GGX bounce loses at high roughness.
vec3 directLight(Surface surface,vec3 v,vec3 l,float intensity,vec3 energy)
{
    vec3 n = surface.normal;
    float nl = max(dot(n,l),0.0);
    if (nl <= 0.0 || intensity <= 0.0)
        return vec3(0.0);
    vec3 h = normalize(l+v);
    float nv = max(dot(n,v),1e-4);
    float nh = max(dot(n,h),0.0);
    float vh = clamp(dot(v,h),0.0,1.0);
    float a = surface.roughness*surface.roughness;
    float a2 = a*a;
    float denominator = nh*nh*(a2-1.0)+1.0;
    float distribution = a2/(shadingPi*denominator*denominator);
    float visibility = 0.5/(nl*sqrt(nv*nv*(1.0-a2)+a2)+nv*sqrt(nl*nl*(1.0-a2)+a2));
    vec3 f0 = mix(vec3(0.04),surface.albedo,surface.metallic);
    vec3 fresnel = f0+(1.0-f0)*pow(1.0-vh,5.0);
    vec3 diffuse = (1.0-fresnel)*(1.0-surface.metallic)*surface.albedo/shadingPi;
    vec3 specular = distribution*visibility*fresnel*energy;
    return (diffuse+specular)*nl*shadingPi*intensity;
}

// Light leaving the surface toward the eye. sunVisibility is the sun's shadow term (1 lit, 0 shadowed). indirect
// receives the sky-light part of the result, which ambient occlusion later scales (Temporal.frag).
vec3 shadeSurface(Surface surface,vec3 position,vec3 eye,float sunVisibility,out vec3 indirect)
{
    vec3 v = normalize(eye-position);
    // Thin, two-sided geometry (leaves, planes) shows its back: light it as seen.
    if (dot(surface.normal,v) < 0.0)
        surface.normal = -surface.normal;
    float nv = max(dot(surface.normal,v),1e-4);
    vec3 f0 = mix(vec3(0.04),surface.albedo,surface.metallic);
    vec2 brdf = environmentBrdf(surface.roughness,nv);
    // Multiple-scattering compensation (Turquin 2019): a single GGX bounce reflects brdf.x+brdf.y of the light for a
    // perfect mirror material; the rest bounces again and leaves tinted by f0.
    vec3 energy = 1.0+f0*(1.0/max(brdf.x+brdf.y,1e-3)-1.0);
    vec3 result = directLight(surface,v,normalize(lighting.sun.xyz),lighting.sun.w*sunVisibility,energy);
    int count = int(lighting.skyHorizon.w);
    for (int i = 0; i < count && i < 16; ++i) {
        vec4 light = lighting.lights[i];
        if (light.w < 0.0) {
            result += directLight(surface,v,normalize(light.xyz),-light.w,energy);
        } else {
            vec3 offset = light.xyz-position;
            float distanceSquared = max(dot(offset,offset),0.01);
            result += directLight(surface,v,offset*inversesqrt(distanceSquared),light.w/distanceSquared,energy);
        }
    }
    vec3 specularColor = (f0*brdf.x+brdf.y)*energy;
    vec3 r = reflect(-v,surface.normal);
    // Rough surfaces see a blurrier sky; at full roughness the reflection is the irradiance.
    vec3 reflected = mix(skyRadianceToward(r),skyIrradiance(r),smoothstep(0.0,1.0,surface.roughness));
    vec3 diffuse = surface.albedo*(1.0-surface.metallic)*(1.0-specularColor)*skyIrradiance(surface.normal);
    indirect = (diffuse+reflected*specularColor)*surface.occlusion*skyOpenness(position,surface.normal);
    return result+indirect;
}
