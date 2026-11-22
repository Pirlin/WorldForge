#version 450
// Motion blur: each pixel's surface moved on screen since the last frame (the camera's motion, from its depth and the two
// frames' cameras, plus its own, from the scene's velocity target, HdrFrame::velocityFormat); a camera shutter open for
// that share of the frame smears it along the movement.
//
// A simplified reconstruction filter (after McGuire et al. 2012): each pixel blurs along the strongest motion in its
// neighbourhood, and a sample counts only where the motion covers this pixel during the shutter (the sample's own smear
// reaches back here, or this pixel's reaches the sample). So a moving object smears over the still background beside
// it, as a camera sees it, while still background stays sharp.
layout(set = 0, binding = 0) uniform sampler2D source; // the image so far, bilinear
layout(set = 0, binding = 1) uniform sampler2D depth;  // this frame's depth, nearest
layout(set = 0, binding = 2) uniform sampler2D surfaceMotion; // each surface's own motion in the previous frame's UV, nearest
layout(push_constant) uniform Motion {
    mat4 reprojection; // this frame's clip space to the previous frame's
    vec4 settings;     // shutter (share of the frame the shutter is open), largest smear in pixels
} motion;
layout(location = 0) out vec4 color;

// The smear at a pixel in pixels: its movement on screen since the last frame for the shutter's share, at most the
// largest smear.
vec2 smearAt(ivec2 pixel,vec2 size)
{
    pixel = clamp(pixel,ivec2(0),ivec2(size)-1);
    vec2 uv = (vec2(pixel)+0.5)/size;
    float z = texelFetch(depth,pixel,0).r;
    vec4 previous = motion.reprojection*vec4(uv*2.0-1.0,z,1.0);
    vec2 previousUv = previous.w > 0.0 ? previous.xy/previous.w*0.5+0.5 : uv;
    previousUv += texelFetch(surfaceMotion,pixel,0).xy;
    vec2 smear = (uv-previousUv)*size*motion.settings.x;
    float length = length(smear);
    return length > motion.settings.y ? smear*(motion.settings.y/length) : smear;
}

void main()
{
    vec2 size = vec2(textureSize(source,0));
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec2 uv = gl_FragCoord.xy/size;
    vec2 own = smearAt(pixel,size);
    // The strongest smear nearby, on a 5 x 5 grid reaching half the largest smear.
    vec2 strongest = own;
    float spacing = motion.settings.y*0.25;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x) {
            vec2 near = smearAt(pixel+ivec2(vec2(x,y)*spacing),size);
            if (dot(near,near) > dot(strongest,strongest))
                strongest = near;
        }
    if (length(strongest) < 0.5) {
        color = vec4(texture(source,uv).rgb,1.0);
        return;
    }
    const int samples = 12;
    float ownReach = 0.5*length(own);
    vec3 sum = texture(source,uv).rgb;
    float total = 1.0;
    for (int i = 0; i < samples; ++i) {
        float t = (float(i)+0.5)/float(samples)-0.5;
        vec2 offset = strongest*t;
        float distance = length(offset);
        if (distance < 0.5)
            continue;
        vec2 at = uv+offset/size;
        float reach = 0.5*length(smearAt(ivec2(at*size),size));
        // The sample's surface passes over this pixel, or this pixel's surface passes over the sample, while the
        // shutter is open; softened over a pixel so edges do not flicker.
        float weight = max(clamp(reach-distance+1.0,0.0,1.0),clamp(ownReach-distance+1.0,0.0,1.0));
        sum += texture(source,at).rgb*weight;
        total += weight;
    }
    color = vec4(sum/total,1.0);
}
