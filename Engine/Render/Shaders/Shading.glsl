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
    vec3 coatNormal; // the unperturbed mesh normal for the independent coating
    vec2 clearCoat;  // weight and perceptual roughness
    vec4 sheen;      // linear reflectance RGB, independent roughness
    vec2 optics;     // transmission weight, index of refraction
    vec4 scattering; // subsurface weight, thickness, mean free path, anisotropy strength
    vec3 tangent;    // direction of the anisotropic specular lobe
};

// Irradiance / pi from the sky in direction n: the radiance a white diffuse surface facing n reflects.
vec3 skyIrradiance(vec3 n)
{
    float basis[9] = float[9](0.282095,0.488603*n.y,0.488603*n.z,0.488603*n.x,1.092548*n.x*n.y,1.092548*n.y*n.z,
                              0.315392*(3.0*n.z*n.z-1.0),1.092548*n.x*n.z,0.546274*(n.x*n.x-n.y*n.y));
    vec3 result = vec3(0.0);
    for (int i = 0; i < 9; ++i)
        result += lighting.skyIrradiance[i].rgb*basis[i];
    float xx = n.x*n.x, yy = n.y*n.y, zz = n.z*n.z;
    float band4[9] = float[9](2.503343*n.x*n.y*(xx-yy),-1.770131*n.y*n.z*(3.0*xx-yy),0.946175*n.x*n.y*(7.0*zz-1.0),
                              -0.669047*n.y*n.z*(7.0*zz-3.0),0.105786*(35.0*zz*zz-30.0*zz+3.0),
                              -0.669047*n.x*n.z*(7.0*zz-3.0),0.473087*(xx-yy)*(7.0*zz-1.0),
                              -1.770131*n.x*n.z*(xx-3.0*yy),0.625836*(xx*(xx-3.0*yy)-yy*(3.0*xx-yy)));
    for (int i = 0; i < 9; ++i)
        result += lighting.skyIrradianceBand4[i].rgb*band4[i];
    return max(result,vec3(0.0));
}

// The sky seen in direction r (its radiance map).
vec3 skyRadianceToward(vec3 r)
{
    return skyEnvironment(r,0);
}

// Matches the 6 x 6 x 4 grid and normalized distances in ProbeGeometry.h.
const ivec3 probeDimensions = ivec3(6,6,4);
const float probeRange = 20.0;

float probeVisibility(vec2 moments,float distance)
{
    float difference = max(distance-moments.x,0.0);
    float variance = max(moments.y-moments.x*moments.x,0.00001);
    float visibility = variance/(variance+difference*difference);
    return visibility*visibility*visibility;
}

float probeCoordinates(vec3 position,out ivec3 base,out vec3 blend)
{
    vec3 coordinate = (position-lighting.probeGrid.xyz)/lighting.probeGrid.w;
    vec3 boundary = min(coordinate,vec3(probeDimensions-1)-coordinate);
    base = clamp(ivec3(floor(coordinate)),ivec3(0),probeDimensions-2);
    blend = clamp(coordinate-vec3(base),0.0,1.0);
    return clamp(2.0*min(boundary.x,min(boundary.y,boundary.z)),0.0,1.0);
}

int probeIndex(ivec3 grid)
{
    return grid.x+probeDimensions.x*(grid.y+probeDimensions.y*grid.z);
}

vec3 diffuseFromProbes(vec3 position,vec3 normal)
{
    if (lighting.probeGrid.w <= 0.0 || lighting.probeSettings.x < 0.5)
        return vec3(0.0);
    vec3 samplePosition = position+normal*0.12;
    ivec3 base;
    vec3 blend;
    float fade = probeCoordinates(samplePosition,base,blend);
    if (fade <= 0.0)
        return vec3(0.0);
    vec3 weightNormal = normal*normal;
    ivec3 lobes = ivec3(0,2,4)+ivec3(lessThan(normal,vec3(0.0)));
    vec3 result = vec3(0.0);
    float total = 0.0, accepted = 0.0;
    for (int corner = 0; corner < 8; ++corner) {
        ivec3 offset = ivec3(corner&1,(corner>>1)&1,(corner>>2)&1);
        ivec3 grid = base+offset;
        int probe = 12*probeIndex(grid);
        vec3 part = mix(1.0-blend,blend,vec3(offset));
        vec3 relative = samplePosition-(lighting.probeGrid.xyz+vec3(grid)*lighting.probeGrid.w);
        float distance = length(relative);
        vec3 direction = relative/max(1e-5,distance);
        vec3 absolute = abs(direction);
        int axis = absolute.x >= max(absolute.y,absolute.z) ? 0 : (absolute.y >= absolute.z ? 1 : 2);
        int facing = 2*axis+(direction[axis] < 0.0 ? 1 : 0);
        float front = max(0.0,0.5-0.5*dot(normal,direction));
        float weight = part.x*part.y*part.z*(0.02+front*front);
        float visible = probeVisibility(probeData[probe+2*facing+1].xy,distance/probeRange);
        vec3 irradiance = vec3(0.0);
        if (weightNormal.x > 0.0)
            irradiance += probeData[probe+2*lobes.x].rgb*weightNormal.x;
        if (weightNormal.y > 0.0)
            irradiance += probeData[probe+2*lobes.y].rgb*weightNormal.y;
        if (weightNormal.z > 0.0)
            irradiance += probeData[probe+2*lobes.z].rgb*weightNormal.z;
        total += weight;
        accepted += weight*visible;
        result += irradiance*weight*visible;
    }
    // Fade the result when all nearby probes are occluded.
    return result*(fade/max(accepted,0.3*total));
}

vec2 octahedralUv(vec3 direction)
{
    direction /= max(dot(abs(direction),vec3(1.0)),1e-5);
    vec2 oct = direction.z >= 0.0 ? direction.xy :
        (1.0-abs(direction.yx))*mix(vec2(-1.0),vec2(1.0),greaterThanEqual(direction.xy,vec2(0.0)));
    return oct*0.5+0.5;
}

vec4 probeReflectionTexel(int first,ivec2 pixel)
{
    pixel = clamp(pixel,ivec2(0),ivec2(7));
    vec4 value = probeData[first+pixel.x+8*pixel.y];
    return vec4(value.rgb,value.w < 1.0 ? 1.0 : 0.0);
}

// A first-hit distance gives each captured direction a local depth proxy. Intersect the surface's
// reflection ray with that proxy, then sample from the probe toward the intersection.
vec4 reflectionFromProbes(vec3 position,vec3 normal,vec3 reflection,float roughness)
{
    if (lighting.probeGrid.w <= 0.0 || lighting.probeSettings.y < 0.5 || roughness >= 0.65)
        return vec4(0.0);
    position += normal*0.12;
    ivec3 base;
    vec3 blend;
    float fade = probeCoordinates(position,base,blend)*(1.0-smoothstep(0.25,0.65,roughness));
    if (fade <= 0.0)
        return vec4(0.0);
    vec4 light = vec4(0.0);
    const int mapStart = 12*probeDimensions.x*probeDimensions.y*probeDimensions.z;
    ivec2 nearest = clamp(ivec2(octahedralUv(reflection)*8.0),ivec2(0),ivec2(7));
    for (int corner = 0; corner < 8; ++corner) {
        ivec3 offset = ivec3(corner&1,(corner>>1)&1,(corner>>2)&1);
        ivec3 grid = base+offset;
        vec3 part = mix(1.0-blend,blend,vec3(offset));
        float weight = part.x*part.y*part.z;
        if (weight < 0.001)
            continue;
        int probe = probeIndex(grid);
        int first = mapStart+64*probe;
        vec3 relative = position-(lighting.probeGrid.xyz+vec3(grid)*lighting.probeGrid.w);
        vec3 absolute = abs(relative);
        int axis = absolute.x >= max(absolute.y,absolute.z) ? 0 : (absolute.y >= absolute.z ? 1 : 2);
        int facing = 2*axis+(relative[axis] < 0.0 ? 1 : 0);
        weight *= probeVisibility(probeData[12*probe+2*facing+1].xy,length(relative)/probeRange);
        float radius = probeData[first+nearest.x+8*nearest.y].w*probeRange;
        float projected = dot(relative,reflection);
        float discriminant = radius*radius-dot(relative,relative)+projected*projected;
        float distance = sqrt(max(discriminant,0.0))-projected;
        vec3 corrected = discriminant > 0.0 && distance > 0.0 ? relative+distance*reflection : reflection;
        vec2 texel = octahedralUv(corrected)*8.0-0.5;
        ivec2 pixel = ivec2(floor(texel));
        vec2 sub = fract(texel);
        vec4 low = mix(probeReflectionTexel(first,pixel),probeReflectionTexel(first,pixel+ivec2(1,0)),sub.x);
        vec4 high = mix(probeReflectionTexel(first,pixel+ivec2(0,1)),probeReflectionTexel(first,pixel+ivec2(1,1)),sub.x);
        light += weight*mix(low,high,sub.y);
    }
    return light*fade;
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
    // With TAA each pixel tests every other direction, alternating in a checkerboard that
    // flips each frame, and the resolve averages the halves: each half of the spiral covers the whole sky. Without
    // TAA every direction is tested.
    int stride = lighting.probeSettings.z > 0.5 ? 2 : 1;
    int first = stride == 2 ? (int(gl_FragCoord.x)+int(gl_FragCoord.y)+int(lighting.settings.z)) & 1 : 0;
    for (int i = first; i < 16; i += stride) {
        vec3 direction = lighting.skyDirections[i].xyz;
        // w: the sky's luminance in that direction, the same for every pixel, so the renderer computes it once.
        float weight = lighting.skyDirections[i].w*(max(dot(normal,direction),0.0)+0.05);
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

// The sky reflected in direction r by a surface of the given roughness: the radiance map for a mirror, then its
// levels prefiltered with the GGX lobe of roughness 1/3, 2/3 and 1.
vec3 skyReflection(vec3 r,float roughness)
{
    float level = clamp(roughness,0.0,1.0)*3.0;
    int low = min(int(level),2);
    return mix(skyEnvironment(r,low),skyEnvironment(r,low+1),level-float(low));
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

// Untinted dielectric with IOR 1.5. Fresnel reflection reduces the base layer's contribution at grazing angles.
float clearCoatFresnel(float cosine)
{
    return 0.04+0.96*pow(1.0-clamp(cosine,0.0,1.0),5.0);
}

float dielectricF0(float ior)
{
    if (ior == 1.5)
        return 0.04;
    float ratio = (ior-1.0)/(ior+1.0);
    return ratio*ratio;
}

// An optically thin pane leaves the transmitted ray parallel to its incident direction.
vec3 thinGlassTransmission(Surface surface,vec3 view)
{
    float nv = clamp(abs(dot(surface.normal,view)),0.0,1.0);
    float f0 = dielectricF0(surface.optics.y);
    float fresnel = f0+(1.0-f0)*pow(1.0-nv,5.0);
    float coat = surface.clearCoat.x*clearCoatFresnel(abs(dot(surface.coatNormal,view)));
    return clamp(surface.albedo,0.0,1.0)*surface.optics.x*(1.0-surface.metallic)*(1.0-fresnel)*(1.0-coat);
}

float clearCoatDirect(vec3 n,vec3 v,vec3 l,float roughness)
{
    float nl = dot(n,l), nv = dot(n,v);
    if (nl <= 0.0 || nv <= 0.0)
        return 0.0;
    vec3 h = normalize(l+v);
    float nh = clamp(dot(n,h),0.0,1.0);
    float a2 = roughness*roughness;
    a2 *= a2;
    float denominator = (1.0-nh*nh)+nh*nh*a2;
    float visibility = 0.5/(nl*sqrt(nv*nv*(1.0-a2)+a2)+nv*sqrt(nl*nl*(1.0-a2)+a2));
    return a2/(denominator*denominator)*visibility*clearCoatFresnel(dot(v,h))*nl;
}

float maxComponent(vec3 value)
{
    return max(max(value.r,value.g),value.b);
}

// Directional albedo from midpoint integration of the Charlie lobe over the incident hemisphere,
// using 96 cosine samples and 192 azimuth samples. Rows span roughness .15 to 1, columns NdotV .02 to 1.
float sheenAlbedo(float nv,float roughness)
{
    const float albedo[64] = float[64](
        1.25934,0.33071,0.09326,0.02239,0.00401,0.00044,0.00002,0.00000,
        0.89101,0.39966,0.20215,0.09944,0.04491,0.01748,0.00484,0.00034,
        0.78371,0.43308,0.26940,0.16763,0.10048,0.05652,0.02694,0.00770,
        0.77534,0.47643,0.32827,0.22868,0.15648,0.10412,0.06325,0.03042,
        0.81533,0.53030,0.38630,0.28611,0.21036,0.15315,0.10586,0.06495,
        0.87350,0.58493,0.43957,0.33721,0.25854,0.19835,0.14770,0.10309,
        0.91881,0.62485,0.47854,0.37530,0.29561,0.23454,0.18306,0.13772,
        0.91538,0.63119,0.49093,0.39188,0.31522,0.25666,0.20727,0.16380
    );
    vec2 uv = clamp((vec2(nv,roughness)-vec2(0.02,0.15))/vec2(0.98,0.85),0.0,1.0)*7.0;
    ivec2 cell = min(ivec2(uv),ivec2(6));
    vec2 weight = uv-vec2(cell);
    int i = cell.y*8+cell.x;
    return min(1.0,mix(mix(albedo[i],albedo[i+1],weight.x),mix(albedo[i+8],albedo[i+9],weight.x),weight.y));
}

// Conty and Kulla's Charlie distribution and fitted masking, as specified by KHR_materials_sheen.
float sheenMaskFit(float cosine,vec4 coefficients,float offset)
{
    return coefficients.x/(1.0+coefficients.y*pow(cosine,coefficients.z))+coefficients.w*cosine+offset;
}

float sheenLambda(float cosine,vec4 coefficients,float offset)
{
    if (cosine < 0.5)
        return exp(sheenMaskFit(cosine,coefficients,offset));
    return exp(2.0*sheenMaskFit(0.5,coefficients,offset)-sheenMaskFit(1.0-cosine,coefficients,offset));
}

float sheenDirect(float nv,float nl,float nh,float roughness)
{
    if (nl <= 0.0)
        return 0.0;
    nv = clamp(nv,0.02,1.0);
    float alpha = max(roughness,0.15);
    alpha *= alpha;
    float blend = (1.0-alpha)*(1.0-alpha);
    vec4 coefficients = mix(vec4(21.5473,3.82987,0.19823,-1.97760),vec4(25.3245,3.32435,0.16801,-1.27393),blend);
    float offset = mix(-4.32054,-4.85967,blend);
    float invAlpha = 1.0/alpha;
    float distribution = (2.0+invAlpha)*pow(max(0.0,1.0-nh*nh),0.5*invAlpha)*0.5;
    return distribution/(4.0*nv*(1.0+sheenLambda(nv,coefficients,offset)+sheenLambda(nl,coefficients,offset)));
}

// Reconstruct the UV frame from screen derivatives. The handedness also handles mirrored islands.
mat3 surfaceTangentFrame(vec3 n,vec3 position,vec2 uv)
{
    vec3 dx = dFdx(position), dy = dFdy(position);
    vec2 u = dFdx(uv), v = dFdy(uv);
    float determinant = u.x*v.y-u.y*v.x;
    vec3 fallback = normalize(cross(abs(n.z) < 0.999 ? vec3(0,0,1) : vec3(1,0,0),n));
    if (abs(determinant) < 1e-10)
        return mat3(fallback,cross(n,fallback),n);
    vec3 tangent = (dx*v.y-dy*u.y)*sign(determinant);
    vec3 bitangent = (dy*u.x-dx*v.x)*sign(determinant);
    tangent -= n*dot(n,tangent);
    tangent = dot(tangent,tangent) > 1e-14 ? normalize(tangent) : fallback;
    vec3 orthogonal = cross(n,tangent);
    return mat3(tangent,orthogonal*(dot(orthogonal,bitangent) < 0.0 ? -1.0 : 1.0),n);
}

// The shader UV frame uses the image's V axis as glTF texture maps do.
vec3 layeredCoatNormal(mat3 frame,vec3 sampleRgb,float scale)
{
    vec3 normal = sampleRgb*2.0-1.0;
    normal.xy *= scale;
    return dot(normal,normal) > 1e-6 ? normalize(frame*normal) : frame[2];
}

vec3 anisotropyTangent(mat3 frame,vec2 direction,float rotation)
{
    direction = direction*2.0-1.0;
    if (dot(direction,direction) < 1e-5)
        direction = vec2(1.0,0.0);
    float c = cos(rotation), s = sin(rotation);
    vec2 rotated = mat2(c,s,-s,c)*normalize(direction);
    return frame[0]*rotated.x+frame[1]*rotated.y;
}

#ifdef MATERIAL_PARALLAX
float parallaxSample(vec2 uv,vec2 gradientsX,vec2 gradientsY);

// March from the top of the relief toward the base and interpolate across the first crossing.
vec2 parallaxOcclusion(vec2 uv,vec3 tangentView,float scale)
{
    vec2 gradientsX = dFdx(uv), gradientsY = dFdy(uv);
    float facing = abs(tangentView.z);
    int steps = int(mix(24.0,8.0,facing));
    float stepDepth = 1.0/float(steps);
    vec2 offset = tangentView.xy*(scale*smoothstep(0.02,0.2,facing)/max(0.12,facing));
    vec2 stepUv = offset*stepDepth;
    float before = parallaxSample(uv,gradientsX,gradientsY);
    if (before <= 0.0)
        return uv;
    for (int i = 1; i <= steps; ++i) {
        vec2 nextUv = uv-stepUv;
        float after = parallaxSample(nextUv,gradientsX,gradientsY)-stepDepth*float(i);
        if (after <= 0.0)
            return mix(uv,nextUv,before/max(before-after,1e-5));
        uv = nextUv;
        before = after;
    }
    return uv;
}
#endif

float puddlePattern(vec2 world)
{
    vec2 cell = floor(world), t = fract(world);
    t = t*t*(3.0-2.0*t);
    vec4 keys = dot(cell,vec2(127.1,311.7))+vec4(0.0,127.1,311.7,438.8);
    vec4 values = fract(sin(keys)*43758.5453);
    return mix(mix(values.x,values.y,t.x),mix(values.z,values.w,t.x),t.y);
}

void applyWetSurface(inout Surface surface,vec3 position,vec3 geometryNormal,float wetness,float puddles,float mask)
{
    if (wetness <= 0.0 && puddles <= 0.0)
        return;
    float water = 0.0;
    if (puddles > 0.0) {
        float basin = mix(0.3,0.7,puddlePattern(position.xy*1.6))+(1.0-mask);
        water = smoothstep(basin-0.08,basin+0.08,puddles)*smoothstep(0.78,0.98,geometryNormal.z);
    }
    float damp = max(wetness,water);
    surface.albedo *= 1.0-0.4*damp*(1.0-surface.metallic);
    surface.roughness = mix(surface.roughness,min(surface.roughness,0.23),damp);
    float coat = max(0.65*wetness,water);
    float wetRoughness = mix(0.21,0.055,water);
    vec3 waterNormal = mix(surface.coatNormal,vec3(0,0,1),water);
    surface.coatNormal = dot(waterNormal,waterNormal) > 1e-6 ? normalize(waterNormal) : geometryNormal;
    surface.clearCoat.y = surface.clearCoat.x > 0.0 ? mix(surface.clearCoat.y,wetRoughness,coat) : wetRoughness;
    surface.clearCoat.x = max(surface.clearCoat.x,coat);
}

vec2 anisotropicAlpha(float roughness,float strength)
{
    float alpha = roughness*roughness;
    return vec2(mix(alpha,1.0,strength*strength),alpha);
}

// Height-correlated Smith visibility and GGX distribution with different roughness along each UV axis.
vec2 anisotropicGgx(Surface surface,vec3 v,vec3 l,vec3 h,float nv,float nl,float nh)
{
    vec3 t = surface.tangent, b = cross(surface.normal,t);
    vec2 alpha = anisotropicAlpha(surface.roughness,surface.scattering.w);
    vec3 scaledH = vec3(vec2(dot(t,h),dot(b,h))/alpha,nh);
    float d = dot(scaledH,scaledH);
    float distribution = 1.0/(shadingPi*alpha.x*alpha.y*d*d);
    float vLength = length(vec3(alpha*vec2(dot(t,v),dot(b,v)),nv));
    float lLength = length(vec3(alpha*vec2(dot(t,l),dot(b,l)),nl));
    return vec2(distribution,0.5/(nl*vLength+nv*lLength));
}

vec3 subsurfaceProfile(Surface surface,float cosine)
{
    float path = surface.scattering.y/max(abs(cosine),0.2);
    return exp(-path/(surface.scattering.z*vec3(1.0,0.55,0.28)));
}

// Local approximation of diffusion. A wrapped front term softens the terminator and a thickness-dependent
// back term accounts for light entering from the other side, with a longer mean free path for red wavelengths.
vec3 subsurfaceLight(Surface surface,vec3 v,vec3 l,float nl)
{
    if (surface.scattering.x <= 0.0)
        return vec3(0.0);
    float wrap = 0.35;
    float front = max(nl+wrap,0.0)/((1.0+wrap)*(1.0+wrap));
    float back = max(-nl,0.0)*mix(0.55,1.0,max(dot(v,-l),0.0));
    return surface.albedo*(surface.scattering.x*(1.0-surface.metallic)*(1.0-surface.optics.x))
        *(vec3(front)+subsurfaceProfile(surface,nl)*back);
}

float reflectionOcclusion(float nv,float ambient,float roughness)
{
    return clamp(pow(nv+ambient,exp2(-16.0*roughness-1.0))-1.0+ambient,0.0,1.0);
}

// Light arriving from direction l with intensity in scene units: GGX specular with the height-correlated Smith
// visibility and Schlick Fresnel, plus Lambert diffuse for what the surface does not reflect. This is the model
// Cycles' Principled BSDF uses (alpha = roughness^2, no roughness remapping), so grazing sunlight on rough
// ground reflects as brightly as in the reference. energy scales the specular lobe for the light that bounces
// between microfacets (multiple scattering), which a single GGX bounce loses at high roughness.
vec3 directLight(Surface surface,vec3 v,vec3 l,float intensity,vec3 energy,float throughCoat,float throughSheen)
{
    if (intensity <= 0.0)
        return vec3(0.0);
    float coat = 0.0;
    if (surface.clearCoat.x > 0.0)
        coat = surface.clearCoat.x*clearCoatDirect(surface.coatNormal,v,l,surface.clearCoat.y)*intensity;
    vec3 n = surface.normal;
    float nl = dot(n,l);
    vec3 scattered = subsurfaceLight(surface,v,l,nl);
    if (nl <= 0.0)
        return vec3(coat)+scattered*(intensity*throughCoat*throughSheen);
    vec3 h = normalize(l+v);
    float nv = max(dot(n,v),1e-4);
    float nh = max(dot(n,h),0.0);
    float vh = clamp(dot(v,h),0.0,1.0);
    float a = surface.roughness*surface.roughness;
    float a2 = a*a;
    float denominator = nh*nh*(a2-1.0)+1.0;
    float distribution = a2/(shadingPi*denominator*denominator);
    float visibility = 0.5/(nl*sqrt(nv*nv*(1.0-a2)+a2)+nv*sqrt(nl*nl*(1.0-a2)+a2));
    if (surface.scattering.w > 0.0) {
        vec2 terms = anisotropicGgx(surface,v,l,h,nv,nl,nh);
        distribution = terms.x;
        visibility = terms.y;
    }
    vec3 f0 = mix(vec3(dielectricF0(surface.optics.y)),surface.albedo,surface.metallic);
    vec3 fresnel = f0+(1.0-f0)*pow(1.0-vh,5.0);
    vec3 diffuse = (1.0-fresnel)*(1.0-surface.metallic)*surface.albedo/shadingPi
        *(1.0-surface.optics.x)*(1.0-surface.scattering.x);
    vec3 specular = distribution*visibility*fresnel*energy;
    vec3 base = ((diffuse+specular)*nl*shadingPi+scattered)*throughSheen;
    if (maxComponent(surface.sheen.rgb) > 0.0)
        base += surface.sheen.rgb*sheenDirect(nv,nl,nh,surface.sheen.w);
    return base*intensity*throughCoat+coat;
}

// Interleaved gradient noise for this pixel, turned each TAA frame and offset by shift, in [0, 1).
float pixelNoise(vec2 shift)
{
    vec2 pixel = gl_FragCoord.xy+shift+5.588238*lighting.settings.z;
    return fract(52.9829189*fract(dot(pixel,vec2(0.06711056,0.00583715))));
}

// A reflection direction for a surface of the given roughness: a GGX microfacet normal drawn with this pixel's noise,
// so the TAA average blurs traced reflections as the lobe does. Mirror-like surfaces reflect about n.
vec3 glossyDirection(vec3 n,vec3 v,float roughness)
{
    float a = roughness*roughness;
    vec3 mirror = reflect(-v,n);
    if (a < 0.002)
        return mirror;
    float phi = 6.2831853*pixelNoise(vec2(0.0));
    float u = pixelNoise(vec2(37.0,17.0));
    float cosTheta = sqrt((1.0-u)/(1.0+(a*a-1.0)*u));
    float sinTheta = sqrt(max(1.0-cosTheta*cosTheta,0.0));
    vec3 tangent = normalize(cross(abs(n.z) < 0.999 ? vec3(0.0,0.0,1.0) : vec3(1.0,0.0,0.0),n));
    vec3 h = normalize(tangent*(sinTheta*cos(phi))+cross(n,tangent)*(sinTheta*sin(phi))+n*cosTheta);
    vec3 r = reflect(-v,h);
    return dot(r,n) > 0.0 ? r : mirror;
}

vec3 anisotropicGlossyDirection(Surface surface,vec3 v)
{
    float phi = 6.2831853*pixelNoise(vec2(0.0));
    float u = pixelNoise(vec2(37.0,17.0));
    vec2 radius = anisotropicAlpha(surface.roughness,surface.scattering.w)*sqrt(u/max(1.0-u,1e-5));
    vec3 b = cross(surface.normal,surface.tangent);
    vec3 h = normalize(surface.normal+surface.tangent*(radius.x*cos(phi))+b*(radius.y*sin(phi)));
    vec3 r = reflect(-v,h);
    return dot(r,surface.normal) > 0.0 ? r : reflect(-v,surface.normal);
}

// Screen-space reflections: marches the reflected ray through the previous frame (its depth in alpha) and returns the
// light it finds there with a confidence in alpha, 0 where the ray leaves the screen, passes behind everything, or
// the surface is too rough to show a sharp reflection. The sky reflection fills in the rest. The previous frame has
// the hit's own fog and reflections in it, which an extra bounce adds.
vec4 screenReflection(vec3 position,vec3 direction,float roughness)
{
    float glossy = 1.0-smoothstep(0.35,0.55,roughness);
    if (lighting.reflectionSettings.x < 0.5 || glossy <= 0.0)
        return vec4(0.0);
    mat4 m = lighting.previousViewProjection;
    vec4 start = m*vec4(position,1.0);
    vec4 along = m*vec4(direction,0.0);
    const float near = 0.05;
    if (start.w <= near)
        return vec4(0.0);
    // Stop the ray before it passes behind the previous camera.
    float rayLength = lighting.reflectionSettings.y;
    if (along.w < 0.0)
        rayLength = min(rayLength,0.99*(start.w-near)/-along.w);
    vec4 end = start+along*rayLength;
    vec2 size = vec2(textureSize(previousFrame,0));
    vec2 a = (start.xy/start.w*0.5+0.5)*size, b = (end.xy/end.w*0.5+0.5)*size;
    // Clip the segment to the screen, then march it in screen space; 1/w is linear there.
    vec2 delta = b-a;
    float last = 1.0;
    for (int axis = 0; axis < 2; ++axis) {
        if (delta[axis] > 1e-4)
            last = min(last,(size[axis]-0.5-a[axis])/delta[axis]);
        else if (delta[axis] < -1e-4)
            last = min(last,(0.5-a[axis])/delta[axis]);
    }
    float pixels = length(delta)*last;
    if (last <= 0.0 || pixels < 4.0)
        return vec4(0.0);
    float inverseStart = 1.0/start.w, inverseEnd = 1.0/end.w;
    const int steps = 28;
    float noise = pixelNoise(vec2(71.0,113.0));
    // Steps grow along the ray (denser near the surface, where contact matters), starting 3 pixels out: nearer than
    // that, a ray leaving a surface seen at a grazing angle cannot be told apart from the surface itself.
    float first = min(3.0/pixels,0.5)*last;
    float before = first, beforeDepth = 1.0/mix(inverseStart,inverseEnd,first);
    float hitAt = -1.0;
    for (int i = 0; i < steps; ++i) {
        float s = (float(i)+noise)/float(steps);
        float t = first+(last-first)*s*s;
        float rayDepth = 1.0/mix(inverseStart,inverseEnd,t);
        float sceneDepth = texelFetch(previousFrame,ivec2(mix(a,b,t)),0).a;
        float behind = rayDepth-sceneDepth;
        // A hit where the ray passes behind a surface by no more than this step moved in depth (plus a margin), so
        // rays that pass far behind thin objects keep going.
        float thickness = abs(rayDepth-beforeDepth)+0.02*sceneDepth+0.05;
        // Each stored depth comes from a jittered sample up to half a pixel off the texel's center, which on a surface
        // seen at a grazing angle (a floor toward the horizon) is a large step in depth. Count a hit only once the ray
        // is behind by more than it moves in depth over a few pixels, so it does not hit the surface it left.
        float slack = 2.5*abs(rayDepth-beforeDepth)/max((t-before)*pixels/last,1e-3);
        if (sceneDepth > 0.0 && behind > slack && behind < thickness) {
            // Refine between the last point in front and this one.
            float low = before, high = t;
            for (int k = 0; k < 5; ++k) {
                float middle = 0.5*(low+high);
                float depth = texelFetch(previousFrame,ivec2(mix(a,b,middle)),0).a;
                if (depth > 0.0 && 1.0/mix(inverseStart,inverseEnd,middle) > depth)
                    high = middle;
                else
                    low = middle;
            }
            hitAt = high;
            break;
        }
        before = t;
        beforeDepth = rayDepth;
    }
    if (hitAt < 0.0)
        return vec4(0.0);
    vec2 pixel = mix(a,b,hitAt);
    vec2 uv = pixel/size;
    // Fade near the screen's edges and the ray's end, where the trace may miss what lies just beyond.
    vec2 edge = smoothstep(vec2(0.0),vec2(0.06),min(uv,1.0-uv));
    float confidence = glossy*edge.x*edge.y*(1.0-smoothstep(0.75,1.0,hitAt));
    return vec4(max(texelFetch(previousFrame,ivec2(pixel),0).rgb,vec3(0.0)),confidence);
}

// Contact shadows: a short ray from the surface toward the sun, marched through the previous frame's depth like
// screenReflection. It finds the small occluders the sun's shadow map is too coarse for (objects resting on a floor,
// gaps under a ledge) and returns 0 where one blocks the sun, 1 otherwise. A ray shorter than a few pixels on screen
// adds nothing the shadow map lacks, so it is skipped.
float contactShadow(vec3 position,vec3 normal,vec3 towardSun)
{
    if (lighting.reflectionSettings.z < 0.5)
        return 1.0;
    const float rayLength = 0.5; // meters
    mat4 m = lighting.previousViewProjection;
    vec3 origin = position+normal*0.005;
    vec4 start = m*vec4(origin,1.0);
    vec4 end = m*vec4(origin+towardSun*rayLength,1.0);
    const float near = 0.05;
    if (start.w <= near || end.w <= near)
        return 1.0;
    vec2 size = vec2(textureSize(previousFrame,0));
    vec2 a = (start.xy/start.w*0.5+0.5)*size, b = (end.xy/end.w*0.5+0.5)*size;
    float pixels = length(b-a);
    if (pixels < 4.0)
        return 1.0;
    float inverseStart = 1.0/start.w, inverseEnd = 1.0/end.w;
    const int steps = 12;
    float noise = pixelNoise(vec2(13.0,59.0));
    // From 2 pixels out, as for reflections: nearer, the ray cannot be told apart from the surface it leaves.
    float first = min(2.0/pixels,0.5);
    float before = first, beforeDepth = 1.0/mix(inverseStart,inverseEnd,first);
    for (int i = 0; i < steps; ++i) {
        float t = first+(1.0-first)*(float(i)+noise)/float(steps);
        vec2 pixel = mix(a,b,t);
        if (any(lessThan(pixel,vec2(0.0))) || any(greaterThanEqual(pixel,size)))
            return 1.0;
        float rayDepth = 1.0/mix(inverseStart,inverseEnd,t);
        float sceneDepth = texelFetch(previousFrame,ivec2(pixel),0).a;
        float behind = rayDepth-sceneDepth;
        // The jitter slack of screenReflection, plus 0.4% of the depth for its 16-bit storage (a ray toward a low sun
        // runs close above the ground it leaves), and occluders up to 30 cm thick (more far away, where depth is coarser).
        float slack = 2.5*abs(rayDepth-beforeDepth)/max((t-before)*pixels,1e-3)+0.004*sceneDepth;
        float thickness = abs(rayDepth-beforeDepth)+0.3+0.01*sceneDepth;
        if (sceneDepth > 0.0 && behind > slack && behind < thickness)
            return 0.0;
        before = t;
        beforeDepth = rayDepth;
    }
    return 1.0;
}

// Light leaving the surface toward the eye. sunVisibility is the sun's shadow term (1 lit, 0 shadowed). indirect
// receives the sky-light part of the result, which ambient occlusion later scales (Temporal.frag).
vec3 shadeSurface(Surface surface,vec3 position,vec3 eye,vec3 shadowBiasNormal,out vec3 indirect,out vec3 diffuseAlbedo)
{
    vec3 v = normalize(eye-position);
    // Thin, two-sided geometry (leaves, planes) shows its back: light it as seen.
    if (dot(surface.normal,v) < 0.0)
        surface.normal = -surface.normal;
    float throughCoat = 1.0, coatNv = 0.0;
    if (surface.clearCoat.x > 0.0) {
        if (dot(surface.coatNormal,v) < 0.0)
            surface.coatNormal = -surface.coatNormal;
        coatNv = max(dot(surface.coatNormal,v),1e-4);
        throughCoat -= surface.clearCoat.x*clearCoatFresnel(coatNv);
    }
    float nv = max(dot(surface.normal,v),1e-4);
    float sheenResponse = maxComponent(surface.sheen.rgb) > 0.0 ? sheenAlbedo(nv,surface.sheen.w) : 0.0;
    float throughSheen = 1.0-maxComponent(surface.sheen.rgb)*sheenResponse;
    diffuseAlbedo = surface.albedo*(1.0-surface.metallic)*throughCoat*throughSheen*(1.0-surface.optics.x);
    vec3 f0 = mix(vec3(dielectricF0(surface.optics.y)),surface.albedo,surface.metallic);
    float skyRoughness = surface.roughness;
    if (surface.scattering.w > 0.0) {
        vec2 alpha = anisotropicAlpha(surface.roughness,surface.scattering.w);
        skyRoughness = sqrt(sqrt(alpha.x*alpha.y));
    }
    vec2 brdf = environmentBrdf(skyRoughness,nv);
    // Multiple-scattering compensation (Turquin 2019): a single GGX bounce reflects brdf.x+brdf.y of the light for a
    // perfect mirror material; the rest bounces again and leaves tinted by f0.
    vec3 energy = 1.0+f0*(1.0/max(brdf.x+brdf.y,1e-3)-1.0);
    vec3 towardSun = normalize(lighting.sun.xyz);
    vec3 shadowNormal = surface.clearCoat.x > 0.0 ? surface.coatNormal : surface.normal;
    float sunlightDot = dot(surface.normal,towardSun);
    float sunShadow = 1.0;
    if (surface.scattering.x > 0.0 && lighting.sun.w > 0.0 && sunlightDot < 0.0) {
        vec3 entry = position+towardSun*(surface.scattering.y/max(-dot(surface.normal,towardSun),0.2)+0.002);
        sunShadow = sunVisibility(entry,-surface.normal,false);
    } else if (lighting.sun.w > 0.0 && (sunlightDot > 0.0 || surface.scattering.x > 0.0
                || (surface.clearCoat.x > 0.0 && dot(shadowNormal,towardSun) > 0.0)))
        sunShadow = sunVisibility(position,shadowBiasNormal,true);
    if (sunShadow > 0.0 && lighting.sun.w > 0.0 && dot(shadowNormal,towardSun) > 0.0)
        sunShadow = min(sunShadow,contactShadow(position,shadowNormal,towardSun));
    vec3 result = directLight(surface,v,towardSun,lighting.sun.w*sunShadow,energy,throughCoat,throughSheen)*lighting.sunTint.rgb;
    // Directional lights reach every pixel; point lights come from this pixel's cluster (LightClusters.h).
    uint directionalCount = clusterHeader.y;
    for (uint i = 0; i < directionalCount; ++i)
        result += directLight(surface,v,normalize(lightData[4u*i].xyz),1.0,energy,throughCoat,throughSheen)*lightData[4u*i+1u].rgb;
    float depth = dot(position-eye,lighting.clusterForward.xyz);
    uvec2 range = pointLightCluster(depth);
    for (uint k = 0u; k < range.y; ++k) {
        uint i = lightIndices[range.x+k];
        vec4 place = lightData[4u*i];
        vec3 offset = place.xyz-position;
        float distanceSquared = max(dot(offset,offset),0.01);
        float relative = distanceSquared/(place.w*place.w);
        if (relative >= 1.0 || (surface.clearCoat.x <= 0.0 && surface.scattering.x <= 0.0
                && dot(surface.normal,offset) <= 0.0))
            continue;
        // Inverse-square falloff, faded to zero over the last part of the light's range.
        float fade = 1.0-relative*relative;
        vec4 light = lightData[4u*i+1u];
        vec4 cone = lightData[4u*i+2u];
        vec3 toLight = offset*inversesqrt(distanceSquared);
        // Spot lights fade at the cone's edge as Cycles' do; point lights pass with 1.
        float spot = smoothstep(0.0,1.0,(dot(-toLight,cone.xyz)-cone.w)*lightData[4u*i+3u].x);
        if (spot <= 0.0)
            continue;
        vec3 samplePosition = position;
        if (surface.scattering.x > 0.0 && dot(surface.normal,toLight) < 0.0)
            samplePosition += toLight*(surface.scattering.y/max(-dot(surface.normal,toLight),0.2)+0.002);
        float shadow = light.w >= 2.0 ? pointShadow(uint(light.w-1.5),samplePosition,surface.normal,place.xyz) : 1.0;
        result += directLight(surface,v,toLight,1.0,energy,throughCoat,throughSheen)*light.rgb*(fade*fade*shadow*spot/distanceSquared);
    }
    vec3 specularColor = (f0*brdf.x+brdf.y)*energy;
    vec3 r = reflect(-v,surface.normal);
    vec3 reflected = skyReflection(r,surface.roughness);
    if (surface.scattering.w > 0.0) {
        vec2 alpha = anisotropicAlpha(surface.roughness,surface.scattering.w);
        vec3 shift = (surface.tangent-r*dot(surface.tangent,r))*(0.65*(alpha.x-alpha.y));
        reflected = 0.5*(skyReflection(normalize(r-shift),surface.roughness)+skyReflection(normalize(r+shift),surface.roughness));
    }
    vec3 irradiance = skyIrradiance(surface.normal);
    if (surface.scattering.x > 0.0)
        irradiance = mix(irradiance,skyIrradiance(-surface.normal),0.25*surface.scattering.x*subsurfaceProfile(surface,1.0));
    vec3 diffuse = diffuseAlbedo*(1.0-specularColor)*irradiance;
    specularColor *= throughCoat*throughSheen;
    // Occlusion: diffuse sky light keeps the share of the sky the point sees (the material's own occlusion times the
    // large-scale openness). Reflections come from a narrower cone, so they lose more of it, the more for glossy
    // surfaces seen head-on (specular occlusion, Lagarde and de Rousiers 2014).
    float ambient = surface.occlusion*skyOpenness(position,surface.normal);
    float specularOcclusion = reflectionOcclusion(nv,ambient,skyRoughness);
    // Where a screen-space reflection finds a surface, it replaces the sky reflection. It already shows what blocks the
    // sky, so it skips specular occlusion, and it stays out of indirect, which ambient occlusion would darken again.
    vec4 traced = vec4(0.0);
    if (lighting.reflectionSettings.x > 0.5 && skyRoughness < 0.55) {
        vec3 direction = surface.scattering.w > 0.0 ? anisotropicGlossyDirection(surface,v)
            : glossyDirection(surface.normal,v,surface.roughness);
        traced = screenReflection(position,direction,skyRoughness);
    }
    vec4 probeReflection = reflectionFromProbes(position,surface.normal,r,skyRoughness);
    reflected = reflected*(1.0-probeReflection.a)+probeReflection.rgb;
    indirect = diffuse*ambient+reflected*specularColor*specularOcclusion*(1.0-traced.a);
    if (sheenResponse > 0.0)
        indirect += skyReflection(r,max(0.65,surface.sheen.w))*surface.sheen.rgb*(sheenResponse*ambient*throughCoat);
    vec3 screenLight = traced.rgb*specularColor*traced.a;
    if (surface.clearCoat.x > 0.0) {
        float roughness = surface.clearCoat.y;
        vec2 coatBrdf = environmentBrdf(roughness,coatNv);
        float reflectance = surface.clearCoat.x*(0.04*coatBrdf.x+coatBrdf.y);
        vec4 coatTrace = vec4(0.0);
        if (lighting.reflectionSettings.x > 0.5 && roughness < 0.55)
            coatTrace = screenReflection(position,glossyDirection(surface.coatNormal,v,roughness),roughness);
        vec3 coatRay = reflect(-v,surface.coatNormal);
        vec4 coatProbe = reflectionFromProbes(position,surface.coatNormal,coatRay,roughness);
        vec3 coatSky = skyReflection(coatRay,roughness)*(1.0-coatProbe.a)+coatProbe.rgb;
        indirect += coatSky*reflectance*reflectionOcclusion(coatNv,ambient,roughness)*(1.0-coatTrace.a);
        screenLight += coatTrace.rgb*reflectance*coatTrace.a;
    }
    return result+indirect+screenLight+diffuseAlbedo*diffuseFromProbes(position,surface.normal);
}
