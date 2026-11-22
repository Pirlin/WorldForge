#version 450
// Temporal anti-aliasing resolve. Each frame the scene is rendered with a different sub-pixel jitter
// (HdrFrame::jitter); this pass blends it into the history of earlier frames, which together cover
// the pixel like supersampling.
// - Reprojection: the history is read where this pixel's surface was last frame, from its depth and
//   the camera matrices (camera motion; moving objects rely on the clamp below).
// - Clamp: the history is clipped to the colour spread of this pixel's 3x3 neighbourhood (variance
//   clipping in YCoCg), which rejects history that no longer matches, such as newly revealed surfaces.
//   Still frames skip the clamp so the result is an exact average of the jittered samples.
layout(set = 0, binding = 0) uniform sampler2D scene;   // this frame, nearest
layout(set = 0, binding = 1) uniform sampler2D depth;   // this frame, nearest
layout(set = 0, binding = 2) uniform sampler2D history; // the previous resolve, bilinear
layout(set = 0, binding = 3) uniform sampler2D indirect; // this frame's sky light after fog, nearest
layout(set = 0, binding = 4) uniform sampler2D ambient;  // bounce light and sky visibility (Ambient.frag), nearest
layout(set = 0, binding = 5) uniform sampler2D albedo;   // diffuse albedo and fog transmission, nearest
layout(push_constant) uniform Temporal {
    mat4 reprojection; // this frame's clip space to the previous frame's clip space
    vec4 settings;     // weight of this frame, history valid, clamp history, ambient occlusion on
} taa;
// RGB: the resolved light. Alpha: this frame's depth along the camera's forward axis at the pixel (from the scene's
// sky-light target; 0 for sky), which the next frame's screen-space reflections trace against.
layout(location = 0) out vec4 color;

vec3 toYCoCg(vec3 c)
{
    return vec3(dot(c,vec3(0.25,0.5,0.25)),dot(c,vec3(0.5,0.0,-0.5)),dot(c,vec3(-0.25,0.5,-0.25)));
}

vec3 fromYCoCg(vec3 c)
{
    return vec3(c.x+c.y-c.z,c.x+c.z,c.x-c.y-c.z);
}

// Moves the history toward the neighbourhood mean until it lies inside the box.
vec3 clipToBox(vec3 value,vec3 center,vec3 extent)
{
    vec3 offset = value-center;
    vec3 scaled = abs(offset/max(extent,vec3(1e-5)));
    float largest = max(scaled.x,max(scaled.y,scaled.z));
    return largest > 1.0 ? center+offset/largest : value;
}

// This frame's light at a pixel: the occluded share of the sky light taken out, and the light bounced from nearby
// surfaces added (both after fog, as the scene was).
vec3 sceneAt(ivec2 pixel)
{
    vec3 light = texelFetch(scene,pixel,0).rgb;
    if (taa.settings.w > 0.5) {
        vec4 nearby = texelFetch(ambient,pixel,0);
        vec4 surface = texelFetch(albedo,pixel,0);
        light += surface.rgb*nearby.rgb*surface.a-texelFetch(indirect,pixel,0).rgb*(1.0-nearby.a);
    }
    return max(light,vec3(0.0));
}

void main()
{
    ivec2 size = textureSize(scene,0);
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec3 current = sceneAt(pixel);
    float depthAlong = texelFetch(indirect,pixel,0).a;
    if (taa.settings.y < 0.5 || taa.settings.x >= 1.0) {
        color = vec4(current,depthAlong);
        return;
    }
    vec3 sum = vec3(0.0), squares = vec3(0.0);
    float nearest = 1.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            ivec2 neighbour = clamp(pixel+ivec2(x,y),ivec2(0),size-1);
            vec3 value = toYCoCg(sceneAt(neighbour));
            sum += value;
            squares += value*value;
            nearest = min(nearest,texelFetch(depth,neighbour,0).r);
        }
    // Reproject with the nearest depth around the pixel so edges of foreground objects follow them.
    vec2 uv = (vec2(pixel)+0.5)/vec2(size);
    vec4 previous = taa.reprojection*vec4(uv*2.0-1.0,nearest,1.0);
    vec2 previousUv = previous.xy/previous.w*0.5+0.5;
    if (previous.w <= 0.0 || any(lessThan(previousUv,vec2(0.0))) || any(greaterThan(previousUv,vec2(1.0)))) {
        color = vec4(current,depthAlong);
        return;
    }
    vec3 past = max(texture(history,previousUv).rgb,vec3(0.0));
    if (taa.settings.z > 0.5) {
        vec3 mean = sum/9.0;
        vec3 deviation = sqrt(max(squares/9.0-mean*mean,vec3(0.0)));
        past = fromYCoCg(clipToBox(toYCoCg(past),mean,1.25*deviation));
    }
    // Weighting by inverse brightness keeps single bright samples from flickering through the average.
    float weight = taa.settings.x;
    if (taa.settings.z > 0.5) {
        float currentWeight = weight/(1.0+dot(current,vec3(0.2126,0.7152,0.0722)));
        float pastWeight = (1.0-weight)/(1.0+dot(past,vec3(0.2126,0.7152,0.0722)));
        color = vec4((current*currentWeight+past*pastWeight)/max(currentWeight+pastWeight,1e-6),depthAlong);
    } else {
        color = vec4(mix(past,current,weight),depthAlong);
    }
}
