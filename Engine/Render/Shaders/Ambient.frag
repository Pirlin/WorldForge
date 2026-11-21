#version 450
// Ground-truth ambient occlusion (GTAO; Jimenez et al. 2016) from the scene depth.
//
// For a few directions around the pixel (slices), it finds the highest horizon on each side within a
// world-space radius and integrates the visible arc of the cosine lobe around the surface normal. The
// result is the share of sky light that reaches the surface, which the TAA resolve applies to the
// indirect light only (Temporal.frag). The slice directions rotate every frame and TAA averages them.
//
// Everything works in world space from the inverse view-projection, so it serves perspective and
// orthographic cameras alike: the view direction and the size of a pixel come from unprojecting.
layout(set = 0, binding = 0) uniform sampler2D depth; // nearest
layout(push_constant) uniform Occlusion {
    mat4 inverseViewProjection; // clip space (with this frame's jitter) to world space
    vec4 settings;              // radius in meters, enabled, frame index, unused
} occlusionSettings;
layout(location = 0) out vec4 occlusion;

const float pi = 3.14159265359;
const int slices = 2, steps = 6;
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

void main()
{
    ivec2 size = textureSize(depth,0);
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float z = texelFetch(depth,pixel,0).r;
    if (occlusionSettings.settings.y < 0.5 || z >= 1.0) {
        occlusion = vec4(1.0);
        return;
    }
    vec2 texel = 1.0/vec2(size);
    vec2 uv = (vec2(pixel)+0.5)*texel;
    vec3 position = worldAt(uv,z);
    vec3 toCamera = normalize(worldAt(uv,0.0)-worldAt(uv,1.0));

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

    float radius = occlusionSettings.settings.x;
    float metersPerPixel = max(length(worldAt(uv+vec2(texel.x,0.0),z)-position),1e-6);
    float radiusPixels = min(radius/metersPerPixel,96.0);
    if (radiusPixels < 1.0) {
        occlusion = vec4(1.0);
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
        float horizons[2];
        for (int side = 0; side < 2; ++side) {
            float sideSign = side == 0 ? -1.0 : 1.0;
            float best = -1.0;
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
                best = max(best,mix(-1.0,dot(delta/distanceToSample,toCamera),falloff));
            }
            horizons[side] = sideSign*acos(clamp(best,-1.0,1.0));
        }
        // Horizons start to count a little above the tangent plane (angle bias, as in HBAO). The normal comes
        // from quantized depth and tilts slightly; without the bias, open ground darkens where it tilts down.
        float h0 = n-0.5*pi+max(horizons[0]-(n-0.5*pi)-angleBias,0.0);
        float h1 = n+0.5*pi-max((n+0.5*pi)-horizons[1]-angleBias,0.0);
        visibility += projectedLength*(integrateArc(h0,n)+integrateArc(h1,n));
    }
    // Not clamped to 1: single slices of an open surface exceed 1 or fall short, and only their average
    // (over the TAA samples) is exact. Clamping first would darken every flat surface.
    occlusion = vec4(clamp(visibility/float(slices),0.0,2.0));
}
