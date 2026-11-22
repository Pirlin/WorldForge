#version 450
// Depth of field: a thin-lens camera focused at a distance blurs everything else into discs (circles of confusion)
// whose size follows from the focal length, the aperture and the distance (HdrFrame::Camera). Each pixel gathers
// the image in a spiral of samples over its own disc; a sample counts only if its own disc reaches back to the pixel,
// so a sharp subject does not bleed into the blurred background behind it.
layout(set = 0, binding = 0) uniform sampler2D source; // the anti-aliased image, bilinear
layout(set = 0, binding = 1) uniform sampler2D depth;  // this frame's depth, nearest
layout(push_constant) uniform Lens {
    mat4 inverseViewProjection; // clip space to world space
    vec4 eye;                   // camera position (xyz)
    vec4 forward;               // view direction (xyz), focus distance in meters (w)
    vec4 lens;                  // focal length (m), aperture diameter (m), pixels per meter on the sensor, largest radius
} camera;
layout(location = 0) out vec4 color;

// The radius in pixels of the disc a point at depth z (0 to 1) blurs into.
float circle(vec2 uv,float z)
{
    float focus = camera.forward.w, f = camera.lens.x, aperture = camera.lens.y;
    float distanceAlong;
    if (z >= 1.0) {
        distanceAlong = 1e9; // the sky, at infinity
    } else {
        vec4 p = camera.inverseViewProjection*vec4(uv*2.0-1.0,z,1.0);
        distanceAlong = max(dot(p.xyz/p.w-camera.eye.xyz,camera.forward.xyz),f*1.001);
    }
    float diameter = aperture*f*abs(distanceAlong-focus)/(distanceAlong*max(focus-f,1e-4)); // on the sensor, meters
    return min(0.5*diameter*camera.lens.z,camera.lens.w);
}

void main()
{
    vec2 size = vec2(textureSize(source,0));
    vec2 uv = gl_FragCoord.xy/size;
    float radius = circle(uv,texelFetch(depth,ivec2(gl_FragCoord.xy),0).r);
    vec3 center = texture(source,uv).rgb;
    if (radius < 0.5) {
        color = vec4(center,1.0);
        return;
    }
    const int samples = 32;
    const float golden = 2.39996323;
    vec3 sum = center;
    float total = 1.0;
    for (int i = 0; i < samples; ++i) {
        float r = sqrt((float(i)+0.5)/float(samples))*radius;
        vec2 offset = vec2(cos(float(i)*golden),sin(float(i)*golden))*r;
        vec2 at = uv+offset/size;
        float reach = circle(at,texture(depth,at).r);
        float weight = clamp(reach-r+1.0,0.0,1.0);
        sum += texture(source,at).rgb*weight;
        total += weight;
    }
    color = vec4(sum/total,1.0);
}
