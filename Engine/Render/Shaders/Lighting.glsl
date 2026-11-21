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
    vec4 skyHorizon;       // linear sky radiance at the horizon; w: number of lights
    vec4 skyZenith;        // linear sky radiance at the zenith
    vec4 skyGround;        // linear radiance of the ground below the horizon
    vec4 lights[16];       // position or direction, intensity in scene units (negative for directional)
} lighting;

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
        // More offset where the sun grazes the surface, where a texel spans more of it.
        float grazing = 1.0-clamp(dot(n,towardSun),0.0,1.0);
        vec3 moved = position+n*scale.x*(1.0+2.0*grazing);
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
    float blockers = 0.0, count = 0.0;
    for (int i = 0; i < 16; ++i) {
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
    for (int i = 0; i < 16; ++i)
        visibility += step(receiver,cascadeDepth(cascade,coordinates.xy+rotation*shadowDisk[i]*filterTexels/texels));
    return visibility/16.0;
}

vec4 localMist(vec3 eye, vec3 direction, float distanceToSurface, vec3 tint, float sunlight)
{
    const int steps = 10;
    float radius = lighting.fogCenterRadius.w;
    if (radius <= 0.0 || lighting.settings.x <= 0.0)
        return vec4(0.0);
    vec3 origin = eye - lighting.fogCenterRadius.xyz;
    float b = dot(origin,direction);
    float d = b*b-dot(origin,origin)+radius*radius;
    if (d <= 0.0)
        return vec4(0.0);
    float nearHit = max(0.0,-b-sqrt(d));
    float farHit = min(distanceToSurface,-b+sqrt(d));
    if (farHit <= nearHit)
        return vec4(0.0);
    float stepSize = (farHit-nearHit)/float(steps);
    float transmission = 1.0;
    vec3 inscatter = vec3(0.0);
    for (int i = 0; i < steps; ++i) {
        vec3 p = eye+direction*(nearHit+(float(i)+0.5)*stepSize);
        float edge = 1.0-smoothstep(0.60,1.0,length(p-lighting.fogCenterRadius.xyz)/radius);
        float opacity = 1.0-exp(-lighting.settings.x*edge*stepSize);
        float illumination = 0.27 + min(0.73,0.62*sunlight)*sunVisibility(p,vec3(0.0),false);
        inscatter += transmission*opacity*tint*illumination;
        transmission *= 1.0-opacity;
    }
    return vec4(inscatter,1.0-transmission);
}
