// The Lighting block and sun shadows, shared by the editor viewport and the Player.
//
// Sun shadows come from four cascades in one depth atlas (Engine/Render/ShadowCascades.h). A point uses the finest
// cascade that holds it, dithered toward the next one near a tile's edge. Soft lookups use percentage-closer soft
// shadows (PCSS): a blocker search finds how far the occluders are, and the filter then spans the penumbra the
// sun's disc casts over that distance, so contact shadows are sharp and long ones soften as they do in Cycles.
// The filter pattern turns each TAA frame, which averages it out.
layout(set = 0, binding = 0) uniform sampler2D shadowMap; // the cascade atlas, 2 x 2 tiles, nearest
layout(std140, set = 0, binding = 1) uniform Lighting {
    mat4 cascadeMatrices[4]; // world space to each cascade: x, y in [-1, 1], depth in [0, 1] away from the sun
    vec4 cascadeScales[4];   // meters per texel, depth range in meters, far end of the slice in meters, unused
    vec4 fogCenterRadius;
    vec4 settings;         // local density, shadows enabled, TAA frame index, ambient light scale from the sky
    vec4 sun;              // direction toward the sun, intensity in scene units
    vec4 skyIrradiance[9]; // spherical-harmonic coefficients of sky irradiance / pi (Engine/Render/SkyLight.h)
    vec4 skyHorizon;       // linear sky radiance at the horizon, solar disc strength in w
    vec4 skyZenith;        // linear sky radiance at the zenith, geometric horizon cosine in w
    vec4 skyGround;        // linear radiance of the ground below the horizon
    // Clustered lights (Engine/Render/LightClusters.h): the camera's forward axis (xyz), and the viewport size in
    // pixels (xy).
    vec4 clusterForward;
    vec4 clusterViewport;
    // Large-scale sky occlusion (Engine/Render/SkyVisibility.h): world space to each sky direction's depth map, and
    // the directions toward the sky with the sky's luminance in each (w).
    mat4 skyMatrices[16];
    vec4 skyDirections[16];
    vec4 skySettings;      // sky visibility maps on, their meters per texel, physical sky on, aerosol extinction per km
    vec4 sunTint;          // sunlight color at luminance 1, cloud cover in w
    // The previous frame: its clip space without jitter (always set; the current one on the first frame), and settings
    // (screen-space reflections on, the longest reflection ray in meters, contact shadows on, each on only when there
    // is a previous frame to trace; the previous frame's wind time in seconds). Screen-space reflections and contact
    // shadows (Shading.glsl) trace the previous frame; the scene vertex shaders read these two members alone
    // (SceneMotion.glsl) for each surface's motion.
    mat4 previousViewProjection;
    vec4 reflectionSettings;
    vec4 probeGrid;        // origin in world meters, spacing (0 when disabled)
    vec4 probeSettings;    // diffuse GI on, reflection probes on, TAA on (split lookups average over its frames)
    vec4 skyIrradianceBand4[9]; // band-4 coefficients of sky irradiance / pi (SkyLight.h shBasis4)
} lighting;
layout(set = 0, binding = 4) uniform sampler2D skyVisibilityMap; // 4 x 4 tiles, one per sky direction, nearest
// The sky's environment (Engine/Render/SkyLight.h): a 128 x 64 radiance map with rows closer near the horizon, then
// the map prefiltered for roughness 1/3, 2/3 and 1 at 64 x 32.
layout(std430, set = 0, binding = 5) readonly buffer SkyEnvironment { vec4 skyMap[]; };
// The lights and the clusters that list them (Engine/Render/LightClusters.h): a header (light count, directional
// count, shadowed point lights); the shadowed lights' cube faces (slot * 6 + face); per light four vec4 (position or
// direction with range; color times intensity, w: 1 directional, 0 point or spot, 2 + slot one with a shadow; the spot
// direction and cos(cone / 2); the spot scale); each cluster's offset and count.
layout(std430, set = 0, binding = 6) readonly buffer Clusters {
    uvec4 clusterHeader;
    mat4 shadowFaces[8*6];
    vec4 lightData[4096];
    uvec2 clusterRanges[16*9*24];
    uint lightIndices[];
};
// The point light shadow atlas: 8 cubes of six 256 x 256 depth faces, eight faces to a row (LightClusters.h).
layout(set = 0, binding = 7) uniform sampler2D pointShadowMap;
// The previous frame's TAA result (Engine/Render/Vulkan/HdrFrame.h): linear HDR light, with each pixel's depth along the
// camera's forward axis in alpha (0 for sky). Screen-space reflections trace it.
layout(set = 0, binding = 8) uniform sampler2D previousFrame;
layout(std430, set = 0, binding = 9) readonly buffer LightProbes { vec4 probeData[]; };

uvec2 pointLightCluster(float depth)
{
    const float nearDepth = 0.1, farDepth = 1000.0;
    int slice = depth <= nearDepth ? 0 : clamp(int(log(depth/nearDepth)/log(farDepth/nearDepth)*24.0),0,23);
    ivec2 tile = clamp(ivec2(gl_FragCoord.xy/max(lighting.clusterViewport.xy,vec2(1.0))*vec2(16.0,9.0)),ivec2(0),ivec2(15,8));
    return clusterRanges[(slice*9+tile.y)*16+tile.x];
}

// How much of a shadowed point light at lightPosition reaches position (on a surface facing normal): the cube face
// the point lies in, looked up with four samples around it.
float pointShadow(uint slot,vec3 position,vec3 normal,vec3 lightPosition)
{
    vec3 offset = position-lightPosition;
    vec3 size = abs(offset);
    int face = size.x >= size.y && size.x >= size.z ? (offset.x > 0.0 ? 0 : 1)
        : size.y >= size.z ? (offset.y > 0.0 ? 2 : 3) : (offset.z > 0.0 ? 4 : 5);
    // Off the surface by about a texel of the face (which grows with distance), toward the light.
    float texel = 2.0*max(size.x,max(size.y,size.z))/256.0;
    vec3 n = dot(normal,offset) > 0.0 ? -normal : normal;
    vec3 moved = position+n*texel*1.5-normalize(offset)*texel;
    vec4 clip = shadowFaces[slot*6u+uint(face)]*vec4(moved,1.0);
    vec3 c = clip.xyz/clip.w;
    int tile = int(slot)*6+face;
    vec2 uv = clamp(c.xy*0.5+0.5,vec2(1.0/256.0),vec2(1.0-1.0/256.0));
    vec2 atlas = vec2(textureSize(pointShadowMap,0));
    vec2 base = vec2(float(tile%8),float(tile/8))*256.0;
    float visible = 0.0;
    for (int i = 0; i < 4; ++i) {
        vec2 at = base+uv*256.0+vec2(i%2 == 0 ? -0.5 : 0.5,i/2 == 0 ? -0.5 : 0.5);
        visible += step(c.z,textureLod(pointShadowMap,at/atlas,0.0).r);
    }
    return visible*0.25;
}

const float skyPi = 3.14159265359;

// Inverse-square lamp irradiance scattered toward the eye at one sample along a mist ray.
vec3 lampMistLight(vec3 position,vec3 direction,float depth)
{
    uvec2 range = pointLightCluster(depth);
    vec3 incident = vec3(0.0);
    for (uint k = 0; k < range.y; ++k) {
        uint index = 4u*lightIndices[range.x+k];
        vec4 place = lightData[index];
        vec3 offset = place.xyz-position;
        float squared = max(dot(offset,offset),0.01);
        float relative = squared/(place.w*place.w);
        if (relative >= 1.0)
            continue;
        vec3 toward = offset*inversesqrt(squared);
        vec4 cone = lightData[index+2u];
        float spot = smoothstep(0.0,1.0,(dot(-toward,cone.xyz)-cone.w)*lightData[index+3u].x);
        if (spot <= 0.0)
            continue;
        vec4 lamp = lightData[index+1u];
        float visibility = lamp.w >= 2.0 && squared > 0.01
            ? pointShadow(uint(lamp.w-1.5),position,vec3(0.0),place.xyz) : 1.0;
        const float g = 0.45;
        float phase = (1.0-g*g)/(4.0*pow(1.0+g*g-2.0*g*clamp(dot(direction,toward),-1.0,1.0),1.5));
        float fade = 1.0-relative*relative;
        incident += lamp.rgb*(visibility*spot*phase*fade*fade/squared);
    }
    return incident;
}

// Bilinear lookup in direction d (unit, z up) of the map at offset, wrapping around the vertical.
vec3 skyMapSample(int offset,int width,int height,vec3 d)
{
    float elevation = asin(clamp(d.z,-1.0,1.0));
    float t = sign(elevation)*sqrt(abs(elevation)/(0.5*skyPi));
    vec2 uv = vec2(atan(d.y,d.x)/(2.0*skyPi)+0.5,0.5+0.5*t);
    float x = uv.x*float(width)-0.5, y = clamp(uv.y*float(height)-0.5,0.0,float(height-1));
    int x0 = int(floor(x)), y0 = min(int(y),height-2);
    float sx = x-float(x0), sy = clamp(y-float(y0),0.0,1.0);
    int xa = ((x0%width)+width)%width, xb = ((x0+1)%width+width)%width;
    vec3 low = mix(skyMap[offset+y0*width+xa].rgb,skyMap[offset+y0*width+xb].rgb,sx);
    vec3 high = mix(skyMap[offset+(y0+1)*width+xa].rgb,skyMap[offset+(y0+1)*width+xb].rgb,sx);
    return mix(low,high,sy);
}

// The sky in direction d: level 0 is its radiance, levels 1 to 3 its reflection by roughness 1/3, 2/3 and 1.
vec3 skyEnvironment(vec3 d,int level)
{
    vec3 radiance = level == 0 ? skyMapSample(0,128,64,d)
        : skyMapSample(128*64+(level-1)*64*32,64,32,d);
    float cover = lighting.sunTint.w;
    if (cover > 0.0)
        radiance = mix(radiance*(1.0-0.35*cover),lighting.skyIrradiance[0].rgb*0.282095,0.75*cover);
    return radiance;
}

const float sunAngularDiameter = 0.00925; // radians, as in ShadowCascades.h
const vec2 shadowDisk[16] = vec2[16](
    vec2(-0.9420,-0.3991),vec2(0.9456,-0.7689),vec2(-0.0942,-0.9294),vec2(0.3450,0.2939),
    vec2(-0.9159,0.4577),vec2(-0.8154,-0.8791),vec2(-0.3828,0.2768),vec2(0.9748,0.7565),
    vec2(0.4432,-0.9751),vec2(0.5374,-0.4737),vec2(-0.2650,-0.4189),vec2(0.7920,0.1909),
    vec2(-0.2419,0.9971),vec2(-0.8141,0.9144),vec2(0.1998,0.7864),vec2(0.1438,-0.1410));

// Depth stored in a cascade's tile at tile coordinates uv, kept inside the tile.
float cascadeDepth(int cascade,vec2 uv)
{
    float texels = float(textureSize(shadowMap,0).x)*0.5;
    uv = clamp(uv,vec2(0.5/texels),vec2(1.0-0.5/texels));
    return textureLod(shadowMap,(uv+vec2(cascade%2,cascade/2))*0.5,0.0).r;
}

// Which cascade holds position, and where: tile coordinates xy and depth z. -1 when none does. noise in [0, 1)
// dithers the change to the next cascade across a band near the tile edge.
int shadowCascade(vec3 position,float noise,out vec3 coordinates)
{
    const float margin = 0.02, band = 0.08; // tile fractions: kept clear for the filter; dithered
    for (int cascade = 0; cascade < 4; ++cascade) {
        vec3 c = (lighting.cascadeMatrices[cascade]*vec4(position,1.0)).xyz;
        coordinates = vec3(c.xy*0.5+0.5,c.z);
        float edge = min(min(coordinates.x,coordinates.y),min(1.0-coordinates.x,1.0-coordinates.y));
        if (edge <= margin || c.z < 0.0 || c.z > 1.0)
            continue;
        if (cascade < 3 && noise > (edge-margin)/band) {
            vec3 next = (lighting.cascadeMatrices[cascade+1]*vec4(position,1.0)).xyz;
            vec2 uv = next.xy*0.5+0.5;
            if (min(min(uv.x,uv.y),min(1.0-uv.x,1.0-uv.y)) > margin && next.z >= 0.0 && next.z <= 1.0) {
                coordinates = vec3(uv,next.z);
                return cascade+1;
            }
        }
        return cascade;
    }
    return -1;
}

// How much of the sun reaches position: 1 lit, 0 shadowed. normal (zero when unknown) moves the lookup off the
// surface by about a texel to keep it from shadowing itself. soft selects PCSS; otherwise one sample decides.
float sunVisibility(vec3 position,vec3 normal,bool soft)
{
    if (lighting.settings.y < 0.5)
        return 1.0;
    float noise = 0.5;
    if (soft) {
        vec2 pixel = gl_FragCoord.xy+5.588238*lighting.settings.z;
        noise = fract(52.9829189*fract(dot(pixel,vec2(0.06711056,0.00583715))));
    }
    vec3 coordinates;
    int cascade = shadowCascade(position,noise,coordinates);
    if (cascade < 0)
        return 1.0;
    vec4 scale = lighting.cascadeScales[cascade];
    if (dot(normal,normal) > 1e-8) {
        vec3 n = normalize(normal);
        vec3 towardSun = normalize(lighting.sun.xyz);
        if (dot(n,towardSun) < 0.0)
            n = -n;
        // Off the surface by half a texel, then toward the sun, more where the sun grazes the surface and a texel
        // spans more depth. Moving along the light ray changes only the depth compared, not where the shadow edge
        // falls; a larger offset along the normal would shift edges toward their casters (by its height over the
        // tangent of the sun's elevation, four times it for a low sun).
        float grazing = 1.0-clamp(dot(n,towardSun),0.0,1.0);
        vec3 moved = position+n*scale.x*0.5+towardSun*scale.x*(1.0+2.0*grazing);
        vec3 c = (lighting.cascadeMatrices[cascade]*vec4(moved,1.0)).xyz;
        coordinates = vec3(c.xy*0.5+0.5,c.z);
    }
    float receiver = coordinates.z-scale.x/scale.y;
    if (!soft)
        return step(receiver,cascadeDepth(cascade,coordinates.xy));

    float texels = float(textureSize(shadowMap,0).x)*0.5;
    float angle = noise*6.2831853;
    mat2 rotation = mat2(cos(angle),sin(angle),-sin(angle),cos(angle));
    // Blocker search: occluders up to 40 m toward the sun can widen the penumbra over this point.
    float searchTexels = clamp(0.5*sunAngularDiameter*40.0/scale.x,1.5,16.0);
    // With TAA each pixel takes every other tap of both disks, alternating in a
    // checkerboard that flips each frame, and the resolve averages the halves. Without TAA every tap is taken.
    int stride = lighting.probeSettings.z > 0.5 ? 2 : 1;
    int first = stride == 2 ? (int(gl_FragCoord.x)+int(gl_FragCoord.y)+int(lighting.settings.z)) & 1 : 0;
    float blockers = 0.0, count = 0.0;
    for (int i = first; i < 16; i += stride) {
        float depth = cascadeDepth(cascade,coordinates.xy+rotation*shadowDisk[i]*searchTexels/texels);
        if (depth < receiver) {
            blockers += depth;
            count += 1.0;
        }
    }
    if (count == 0.0)
        return 1.0;
    // The sun's disc, seen past an occluder d meters away, blurs the shadow edge over a disc of radius d * angle / 2.
    float distanceToBlockers = (receiver-blockers/count)*scale.y;
    float filterTexels = clamp(0.5*sunAngularDiameter*distanceToBlockers/scale.x,0.75,24.0);
    float visibility = 0.0;
    for (int i = first; i < 16; i += stride)
        visibility += step(receiver,cascadeDepth(cascade,coordinates.xy+rotation*shadowDisk[i]*filterTexels/texels));
    return visibility*float(stride)/16.0;
}

// Integrate the clamped exponential height profile over a ray segment. Outside the exponential part the
// density is constant, including below the height at which the density reaches its upper bound.
float heightFogTransmission(float distance,float logAtEye,float logSlope,float density)
{
    const float lower = -15.0, upper = 10.0;
    float change = logSlope*distance;
    float low = logAtEye+min(change,0.0), high = logAtEye+max(change,0.0);
    float a = clamp(low,lower,upper), b = clamp(high,lower,upper);
    if (a == b || abs(change) < 0.001)
        return exp(-min(40.0,density*distance*exp(0.5*(a+b))));
    float integral = exp(b)-exp(a) + max(0.0,a-low)*exp(lower) + max(0.0,high-b)*exp(upper);
    return exp(-min(40.0,density*distance*integral/abs(change)));
}

void physicalFogLighting(vec3 direction,vec3 tint,out vec3 ambient,out vec3 direct)
{
    // The constant SH term gives the mean sky radiance incident on the droplets.
    ambient = tint*lighting.skyIrradiance[0].rgb*0.282095;
    // Henyey-Greenstein phase function times pi: the sun's scene units are irradiance divided by pi.
    const float g = 0.7;
    float towardSun = clamp(dot(direction,lighting.sun.xyz),-1.0,1.0);
    float phase = (1.0-g*g)/(4.0*pow(1.0+g*g-2.0*g*towardSun,1.5));
    direct = tint*lighting.sunTint.rgb*lighting.sun.w*phase;
}

float fogShadowJitter()
{
    float pixelNoise = fract(52.9829189*fract(dot(gl_FragCoord.xy,vec2(0.06711056,0.00583715))));
    return fract(pixelNoise+lighting.settings.z*0.61803399);
}

// RGB is in-scattered light and alpha is the fraction of the background that reaches the eye.
vec4 heightFog(vec3 eye,vec3 direction,float distance,vec3 tint,float density,float falloff,float base)
{
    if (density <= 0.0 || distance <= 0.0)
        return vec4(0.0,0.0,0.0,1.0);

    float logAtEye = (base-eye.z)*falloff;
    float logSlope = -direction.z*falloff;
    float transmission = heightFogTransmission(distance,logAtEye,logSlope,density);
    float opacity = 1.0-transmission;
    bool physical = lighting.skySettings.z > 0.5;
    vec3 scatter, sunlight;
    if (physical) {
        physicalFogLighting(direction,tint,scatter,sunlight);
    } else {
        float towardSun = max(0.0,dot(direction,lighting.sun.xyz));
        float sunShare = clamp(lighting.sun.w,0.0,1.0)*(0.30+0.22*pow(towardSun,12.0));
        scatter = tint*(1.0-sunShare);
        sunlight = tint*lighting.sunTint.rgb*sunShare;
    }
    float blocked = 0.0;
    if (lighting.settings.y > 0.5 && lighting.sun.w > 0.0 && opacity > 0.0001) {
        // Spend the samples within shadow map range. Fog beyond it still attenuates and scatters light.
        float shadowRange = lighting.cascadeScales[3].z/max(0.2,dot(direction,lighting.clusterForward.xyz));
        float marchEnd = min(distance,shadowRange);
        vec2 pixel = gl_FragCoord.xy+5.588238*lighting.settings.z;
        float jitter = physical ? fogShadowJitter() : fract(52.9829189*fract(dot(pixel,vec2(0.06711056,0.00583715))));
        const int steps = 8; // TAA turns the jitter each frame, so a still view averages 128 samples
        // With TAA, neighboring segments share one shadow lookup, placed in either by its
        // share of their extinction, as the local mist does. Without TAA each segment takes its own.
        bool paired = lighting.probeSettings.z > 0.5;
        // Pairs also alternate between frames (the same pairs across the screen, so neighboring pixels branch
        // alike), each taking its lookup at twice the weight.
        int parity = int(lighting.settings.z) & 1;
        float start = 0.0, before = 1.0;
        for (int i = 0; i < steps; i += 2) {
            float middle = marchEnd*float((i+1)*(i+1))/float(steps*steps);
            float end = marchEnd*float((i+2)*(i+2))/float(steps*steps);
            float between = heightFogTransmission(middle,logAtEye,logSlope,density);
            float after = heightFogTransmission(end,logAtEye,logSlope,density);
            float nearWeight = before-between, farWeight = between-after;
            if (paired) {
                float weight = nearWeight+farWeight;
                if (weight > 0.00001 && ((i/2+parity) & 1) == 0) {
                    float share = nearWeight/weight;
                    float sampleAt = jitter < share ? mix(start,middle,jitter/max(share,1e-6))
                        : mix(middle,end,(jitter-share)/max(1.0-share,1e-6));
                    float shadow = 1.0-sunVisibility(eye+direction*sampleAt,vec3(0.0),false);
                    blocked += 2.0*weight*shadow*(1.0-smoothstep(0.75*shadowRange,shadowRange,sampleAt));
                }
                if (physical)
                    jitter = fract(jitter+0.61803399);
            } else {
                if (nearWeight > 0.00001) {
                    float sampleAt = mix(start,middle,jitter);
                    float shadow = 1.0-sunVisibility(eye+direction*sampleAt,vec3(0.0),false);
                    blocked += nearWeight*shadow*(1.0-smoothstep(0.75*shadowRange,shadowRange,sampleAt));
                }
                if (physical)
                    jitter = fract(jitter+0.61803399);
                if (farWeight > 0.00001) {
                    float sampleAt = mix(middle,end,jitter);
                    float shadow = 1.0-sunVisibility(eye+direction*sampleAt,vec3(0.0),false);
                    blocked += farWeight*shadow*(1.0-smoothstep(0.75*shadowRange,shadowRange,sampleAt));
                }
                if (physical)
                    jitter = fract(jitter+0.61803399);
            }
            if (after < 0.001)
                break;
            start = end;
            before = after;
        }
    }
    return vec4(scatter*opacity+sunlight*max(0.0,opacity-blocked),transmission);
}

vec4 surfaceFog(vec3 eye,vec3 direction,float distance,vec3 tint,float density,float falloff,float base)
{
    vec4 fog = heightFog(eye,direction,distance,tint,density,falloff,base);
    float haze = lighting.skySettings.w;
    if (haze > 0.0) {
        // The aerosol density falls with altitude. Use the physical sky toward the horizon as the distant source.
        const float aerosolScale = 1.0/1200.0;
        float transmission = heightFogTransmission(distance,-eye.z*aerosolScale,-direction.z*aerosolScale,haze*0.001);
        vec3 air = skyEnvironment(normalize(vec3(direction.xy,max(0.02,direction.z))),0);
        fog.rgb += air*(1.0-transmission)*fog.a;
        fog.a *= transmission;
    }
    return fog;
}

vec4 localMist(vec3 eye, vec3 direction, float distanceToSurface, vec3 tint, float sunlight)
{
    float radius = lighting.fogCenterRadius.w;
    if (radius <= 0.0 || lighting.settings.x <= 0.0)
        return vec4(0.0);
    vec3 origin = eye - lighting.fogCenterRadius.xyz;
    float b = dot(origin,direction);
    float d = b*b-dot(origin,origin)+radius*radius;
    if (d <= 0.0)
        return vec4(0.0);
    float halfChord = sqrt(d);
    float nearHit = max(0.0,-b-halfChord);
    float farHit = min(distanceToSurface,-b+halfChord);
    if (farHit <= nearHit)
        return vec4(0.0);
    bool physical = lighting.skySettings.z > 0.5;
    vec3 ambient = tint*0.27;
    vec3 direct = tint*min(0.73,0.62*sunlight);
    if (physical)
        physicalFogLighting(direction,tint,ambient,direct);
    bool shadowed = lighting.settings.y > 0.5 && any(greaterThan(direct,vec3(0.0)));
    bool lamps = physical && clusterHeader.x > clusterHeader.y;
    float jitter = physical && (shadowed || lamps) ? fogShadowJitter() : 0.5;
    // Keep ten density samples. In physical mode two neighboring samples share one shadow query,
    // with each half selected in proportion to its contribution to the segment's optical depth.
    int segments = physical ? 5 : 10;
    float stepSize = (farHit-nearHit)/float(segments);
    float transmission = 1.0;
    float blocked = 0.0;
    vec3 lamplight = vec3(0.0);
    float radialOffset = max(0.0,dot(origin,origin)-b*b)/(radius*radius);
    float densityStep = lighting.settings.x*stepSize;
    // With TAA each segment takes its lookups on alternate frames at twice the weight, the same segments across the
    // screen so neighboring pixels branch alike; the resolve averages them. Without TAA every segment takes them.
    bool alternate = lighting.probeSettings.z > 0.5;
    int parity = int(lighting.settings.z) & 1;
    for (int i = 0; i < segments; ++i) {
        float start = nearHit+float(i)*stepSize;
        float fraction = physical ? 0.25 : 0.5;
        float along = (b+start+fraction*stepSize)/radius;
        float first = 1.0-smoothstep(0.60,1.0,sqrt(radialOffset+along*along));
        float density = first;
        if (physical) {
            along += 0.5*stepSize/radius;
            float second = 1.0-smoothstep(0.60,1.0,sqrt(radialOffset+along*along));
            density = (first+second)*0.5;
        }
        float after = transmission*exp(-densityStep*density);
        float weight = transmission-after;
        if ((shadowed || lamps) && weight > 0.00001 && (!alternate || ((i+parity) & 1) == 0)) {
            float share = alternate ? 2.0*weight : weight;
            float sampleAt = 0.5;
            if (physical) {
                // Pick the subsegment by its extinction weight, including attenuation by the first half.
                float midpoint = transmission*exp(-0.5*densityStep*first);
                float nearWeight = (transmission-midpoint)/weight;
                sampleAt = jitter < nearWeight ? 0.5*jitter/max(nearWeight,1e-6)
                    : 0.5+0.5*(jitter-nearWeight)/max(1.0-nearWeight,1e-6);
            }
            float distance = start+sampleAt*stepSize;
            vec3 point = eye+direction*distance;
            if (shadowed)
                blocked += share*(1.0-sunVisibility(point,vec3(0.0),false));
            if (lamps)
                lamplight += share*lampMistLight(point,direction,distance*dot(direction,lighting.clusterForward.xyz));
        }
        transmission = after;
        if (physical)
            jitter = fract(jitter+0.61803399);
    }
    float opacity = 1.0-transmission;
    return vec4(ambient*opacity+direct*max(0.0,opacity-blocked)+tint*lamplight,opacity);
}

// This surface's own motion on the previous frame's screen in UV units (HdrFrame::velocityFormat), from the two clip
// positions the scene vertex shaders pass (SceneMotion.glsl): where it was and where it is, both through the previous
// frame's camera.
vec2 surfaceMotionOf(vec4 previousClip,vec4 currentClip)
{
    if (previousClip.w <= 0.0 || currentClip.w <= 0.0)
        return vec2(0.0);
    return (previousClip.xy/previousClip.w-currentClip.xy/currentClip.w)*0.5;
}
