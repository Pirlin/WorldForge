#version 450
// Ground-truth ambient occlusion (GTAO; Jimenez et al. 2016) and screen-space bounce light from the scene depth
// and color.
//
// For a few directions around the pixel (slices), it finds the highest horizon on each side within a
// world-space radius and integrates the visible arc of the cosine lobe around the surface normal. The
// result is the share of sky light that reaches the surface, which the TAA resolve applies to the
// indirect light only (Temporal.frag). The slice directions rotate every frame and TAA averages them.
//
// Bounce light comes from the same search: each sample that raises a horizon hides part of the sky, and that part
// of the cosine lobe now sees the sample instead, which sends its lit color (this frame's scene) toward the
// surface. The sum is the bounce radiance a white surface would reflect; the resolve multiplies it by the albedo.
//
// Everything works in world space from the inverse view-projection, so it serves perspective and
// orthographic cameras alike: the view direction and the size of a pixel come from unprojecting.
layout(set = 0, binding = 0) uniform sampler2D depth; // nearest
layout(set = 0, binding = 1) uniform sampler2D scene; // this frame's lit scene, nearest
layout(push_constant) uniform Occlusion {
    mat4 inverseViewProjection; // clip space (with this frame's jitter) to world space
    vec4 settings;              // radius, 1 for AO + 2 for screen GI + 4 with TAA, frame index, probe GI on
} occlusionSettings;
layout(location = 0) out vec4 occlusion; // bounce light, sky visibility

const float pi = 3.14159265359;
const int steps = 6;
const float angleBias = 0.06; // radians

vec3 worldAt(vec2 uv,float z)
{
    vec4 p = occlusionSettings.inverseViewProjection*vec4(uv*2.0-1.0,z,1.0);
    return p.xyz/p.w;
}

float integrateArc(float h,float n)
{
    return 0.25*(-cos(2.0*h-n)+cos(n)+2.0*h*sin(n));
}

vec3 surfaceNormalAt(ivec2 pixel,ivec2 size,vec2 texel,vec2 uv,float z,vec3 position,vec3 toCamera)
{
    // The normal from depth, using the neighbour on the side with the smaller depth step at each axis
    // so edges do not blend two surfaces.
    float left = texelFetch(depth,clamp(pixel-ivec2(1,0),ivec2(0),size-1),0).r;
    float right = texelFetch(depth,clamp(pixel+ivec2(1,0),ivec2(0),size-1),0).r;
    float down = texelFetch(depth,clamp(pixel-ivec2(0,1),ivec2(0),size-1),0).r;
    float up = texelFetch(depth,clamp(pixel+ivec2(0,1),ivec2(0),size-1),0).r;
    vec3 alongX = abs(right-z) < abs(z-left) ? worldAt(uv+vec2(texel.x,0.0),right)-position
                                             : position-worldAt(uv-vec2(texel.x,0.0),left);
    vec3 alongY = abs(up-z) < abs(z-down) ? worldAt(uv+vec2(0.0,texel.y),up)-position
                                          : position-worldAt(uv-vec2(0.0,texel.y),down);
    vec3 normal = normalize(cross(alongX,alongY));
    if (dot(normal,toCamera) < 0.0)
        normal = -normal;
    return normal;
}

void main()
{
    ivec2 size = textureSize(depth,0);
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    int mode = int(occlusionSettings.settings.y);
    bool useAo = (mode&1) != 0, useGi = (mode&2) != 0;
    // With TAA one slice a frame: its angle turns each frame, so a still view still averages 16 directions.
    int slices = (mode&4) != 0 ? 1 : 2;
    if (useGi)
        pixel = min(2*pixel+ivec2(1),size-1);
    float z = texelFetch(depth,pixel,0).r;
    if (!(useAo || useGi) || z >= 1.0) {
        occlusion = vec4(0.0,0.0,0.0,1.0);
        return;
    }
    vec2 texel = 1.0/vec2(size);
    vec2 uv = (vec2(pixel)+0.5)*texel;
    vec3 position = worldAt(uv,z);
    vec3 toCamera = normalize(worldAt(uv,0.0)-worldAt(uv,1.0));
    vec3 normal = surfaceNormalAt(pixel,size,texel,uv,z,position,toCamera);
    float radius = occlusionSettings.settings.x*(useGi && !useAo ? 1.6 : 1.0);
    float metersPerPixel = max(length(worldAt(uv+vec2(texel.x,0.0),z)-position),1e-6);
    float radiusPixels = min(radius/metersPerPixel,96.0);
    if (radiusPixels < 1.0) {
        occlusion = vec4(0.0,0.0,0.0,1.0);
        return;
    }
    // Interleaved gradient noise gives each pixel its own starting angle; each of the 16 TAA frames then
    // turns the slices by 1/16 of their spacing, so a still view averages 32 evenly spaced directions.
    // The step offsets advance by the golden ratio.
    float frame = occlusionSettings.settings.z;
    float pixelNoise = fract(52.9829189*fract(dot(vec2(pixel),vec2(0.06711056,0.00583715))));
    float noise = fract(pixelNoise+frame/16.0);
    float jitter = fract(pixelNoise*7.31+frame*0.618034);

    float visibility = 0.0;
    vec3 bounce = vec3(0.0);
    for (int slice = 0; slice < slices; ++slice) {
        float angle = (float(slice)+noise)*pi/float(slices);
        vec2 direction = vec2(cos(angle),sin(angle));
        vec3 sliceDirection = normalize(worldAt(uv+direction*texel,z)-position);
        vec3 axis = normalize(cross(sliceDirection,toCamera));
        vec3 orthogonal = sliceDirection-dot(sliceDirection,toCamera)*toCamera;
        vec3 projected = normal-axis*dot(normal,axis);
        float projectedLength = length(projected);
        if (projectedLength < 1e-4)
            continue;
        float cosine = clamp(dot(projected,toCamera)/projectedLength,-1.0,1.0);
        float n = sign(dot(orthogonal,projected))*acos(cosine);
        // Horizons start to count a little above the tangent plane (angle bias, as in HBAO). The normal comes
        // from quantized depth and tilts slightly; without the bias, open ground darkens where it tilts down.
        float horizons[2];
        for (int side = 0; side < 2; ++side) {
            float sideSign = side == 0 ? -1.0 : 1.0;
            float tangent = n+sideSign*0.5*pi;
            float edge = tangent; // the highest horizon so far, after the bias
            for (int index = 0; index < steps; ++index) {
                float t = (float(index)+jitter)/float(steps);
                vec2 offset = direction*sideSign*max(t*t*radiusPixels,float(index)+1.0);
                ivec2 samplePixel = ivec2(floor(vec2(pixel)+0.5+offset));
                if (any(lessThan(samplePixel,ivec2(0))) || any(greaterThanEqual(samplePixel,size)))
                    break;
                // The position at the texel center, where its depth was rendered. Off-center positions sit
                // above or below sloped surfaces, and the highest-horizon search keeps the ones above.
                vec2 sampleUv = (vec2(samplePixel)+0.5)*texel;
                float sampleDepth = texelFetch(depth,samplePixel,0).r;
                vec3 delta = worldAt(sampleUv,sampleDepth)-position;
                float distanceToSample = length(delta);
                if (distanceToSample < 1e-5)
                    continue;
                // Occluders fade out toward the radius, so distant geometry does not darken the surface.
                float falloff = clamp((radius-distanceToSample)/(0.4*radius),0.0,1.0);
                float horizon = sideSign*acos(clamp(mix(-1.0,dot(delta/distanceToSample,toCamera),falloff),-1.0,1.0));
                float raised = tangent-sideSign*max(sideSign*(tangent-horizon)-angleBias,0.0);
                // A higher horizon: the arc it newly hides sees this sample instead of the sky.
                if (sideSign*(edge-raised) > 0.0) {
                    float transport = 1.0;
                    if (useGi) {
                        // When the probe grid also contributes, keep only the shorter screen-space transport.
                        float radiusFactor = occlusionSettings.settings.w > 0.5 ? 0.75 : 1.0;
                        transport = 0.0;
                        if (sampleDepth < 1.0 && distanceToSample < radiusFactor*radius) {
                            vec3 sourceNormal = surfaceNormalAt(samplePixel,size,texel,sampleUv,sampleDepth,position+delta,toCamera);
                            transport = max(0.0,dot(sourceNormal,-delta/distanceToSample))*
                                (1.0-smoothstep(0.25*radius,radiusFactor*radius,distanceToSample));
                        }
                        if (isnan(transport))
                            transport = 0.0;
                    }
                    if (transport > 0.0)
                        bounce += texelFetch(scene,samplePixel,0).rgb*projectedLength*transport*
                            (integrateArc(edge,n)-integrateArc(raised,n));
                    edge = raised;
                }
            }
            horizons[side] = edge;
        }
        visibility += projectedLength*(integrateArc(horizons[0],n)+integrateArc(horizons[1],n));
    }
    // Not clamped to 1: single slices of an open surface exceed 1 or fall short, and only their average
    // (over the TAA samples) is exact. Clamping first would darken every flat surface.
    occlusion = vec4(bounce/float(slices),useAo ? clamp(visibility/float(slices),0.0,2.0) : 1.0);
}
