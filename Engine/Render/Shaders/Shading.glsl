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

// The sky seen in direction r (its radiance map).
vec3 skyRadianceToward(vec3 r)
{
    return skyEnvironment(r,0);
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
    vec3 towardSun = normalize(lighting.sun.xyz);
    if (sunVisibility > 0.0 && lighting.sun.w > 0.0 && dot(surface.normal,towardSun) > 0.0)
        sunVisibility = min(sunVisibility,contactShadow(position,surface.normal,towardSun));
    vec3 result = directLight(surface,v,towardSun,lighting.sun.w*sunVisibility,energy)*lighting.sunTint.rgb;
    // Directional lights reach every pixel; point lights come from this pixel's cluster (LightClusters.h).
    uint directionalCount = clusterHeader.y;
    for (uint i = 0; i < directionalCount; ++i)
        result += directLight(surface,v,normalize(lightData[4u*i].xyz),1.0,energy)*lightData[4u*i+1u].rgb;
    const float nearDepth = 0.1, farDepth = 1000.0;
    float depth = dot(position-eye,lighting.clusterForward.xyz);
    int slice = depth <= nearDepth ? 0 : clamp(int(log(depth/nearDepth)/log(farDepth/nearDepth)*24.0),0,23);
    ivec2 tile = clamp(ivec2(gl_FragCoord.xy/max(lighting.clusterViewport.xy,vec2(1.0))*vec2(16.0,9.0)),ivec2(0),ivec2(15,8));
    uvec2 range = clusterRanges[(slice*9+tile.y)*16+tile.x];
    for (uint k = 0u; k < range.y; ++k) {
        uint i = lightIndices[range.x+k];
        vec4 place = lightData[4u*i];
        vec3 offset = place.xyz-position;
        float distanceSquared = max(dot(offset,offset),0.01);
        // Inverse-square falloff, faded to zero over the last part of the light's range.
        float fade = clamp(1.0-pow(distanceSquared/(place.w*place.w),2.0),0.0,1.0);
        vec4 light = lightData[4u*i+1u];
        vec4 cone = lightData[4u*i+2u];
        vec3 toLight = offset*inversesqrt(distanceSquared);
        // Spot lights fade at the cone's edge as Cycles' do; point lights pass with 1.
        float spot = smoothstep(0.0,1.0,(dot(-toLight,cone.xyz)-cone.w)*lightData[4u*i+3u].x);
        if (spot <= 0.0)
            continue;
        float shadow = light.w >= 2.0 ? pointShadow(uint(light.w-1.5),position,surface.normal,place.xyz) : 1.0;
        result += directLight(surface,v,toLight,1.0,energy)*light.rgb*(fade*fade*shadow*spot/distanceSquared);
    }
    vec3 specularColor = (f0*brdf.x+brdf.y)*energy;
    vec3 r = reflect(-v,surface.normal);
    vec3 reflected = skyReflection(r,surface.roughness);
    vec3 diffuse = surface.albedo*(1.0-surface.metallic)*(1.0-specularColor)*skyIrradiance(surface.normal);
    // Occlusion: diffuse sky light keeps the share of the sky the point sees (the material's own occlusion times the
    // large-scale openness). Reflections come from a narrower cone, so they lose more of it, the more for glossy
    // surfaces seen head-on (specular occlusion, Lagarde and de Rousiers 2014).
    float ambient = surface.occlusion*skyOpenness(position,surface.normal);
    float specularOcclusion = clamp(pow(nv+ambient,exp2(-16.0*surface.roughness-1.0))-1.0+ambient,0.0,1.0);
    // Where a screen-space reflection finds a surface, it replaces the sky reflection. It already shows what blocks the
    // sky, so it skips specular occlusion, and it stays out of indirect, which ambient occlusion would darken again.
    vec4 traced = vec4(0.0);
    if (lighting.reflectionSettings.x > 0.5 && surface.roughness < 0.55)
        traced = screenReflection(position,glossyDirection(surface.normal,v,surface.roughness),surface.roughness);
    indirect = diffuse*ambient+reflected*specularColor*specularOcclusion*(1.0-traced.a);
    return result+indirect+traced.rgb*specularColor*traced.a;
}
