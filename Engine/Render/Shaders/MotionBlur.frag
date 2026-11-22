#version 450
// Motion blur from the camera's movement: each pixel's surface moved on screen since the last frame (from its depth
// and the two frames' cameras); a camera shutter open for that share of the frame smears it along the movement.
// Moving objects are blurred only by the camera's motion (there are no per-object motion vectors yet).
layout(set = 0, binding = 0) uniform sampler2D source; // the image so far, bilinear
layout(set = 0, binding = 1) uniform sampler2D depth;  // this frame's depth, nearest
layout(push_constant) uniform Motion {
    mat4 reprojection; // this frame's clip space to the previous frame's
    vec4 settings;     // shutter (share of the frame the shutter is open), largest smear in pixels
} motion;
layout(location = 0) out vec4 color;

void main()
{
    vec2 size = vec2(textureSize(source,0));
    vec2 uv = gl_FragCoord.xy/size;
    float z = texelFetch(depth,ivec2(gl_FragCoord.xy),0).r;
    vec4 previous = motion.reprojection*vec4(uv*2.0-1.0,z,1.0);
    vec2 velocity = previous.w > 0.0 ? (uv-(previous.xy/previous.w*0.5+0.5))*size*motion.settings.x : vec2(0.0);
    float length = length(velocity);
    if (length > motion.settings.y)
        velocity *= motion.settings.y/length;
    if (length < 0.5) {
        color = vec4(texture(source,uv).rgb,1.0);
        return;
    }
    const int samples = 12;
    vec3 sum = vec3(0.0);
    for (int i = 0; i < samples; ++i) {
        float t = (float(i)+0.5)/float(samples)-0.5;
        sum += texture(source,uv+velocity*t/size).rgb;
    }
    color = vec4(sum/float(samples),1.0);
}
