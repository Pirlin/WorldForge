#version 450
layout(location = 0) in vec3 startPosition;
layout(location = 4) in vec2 corner;
layout(location = 6) in vec3 endPosition;
layout(push_constant) uniform View {
    mat4 matrix;
    vec2 pixelToNdc;
    float halfWidth;
} view;

void main()
{
    vec4 start = view.matrix * vec4(startPosition, 1.0);
    vec4 end = view.matrix * vec4(endPosition, 1.0);
    // Clip to Vulkan's near plane before expanding the segment.
    if (start.z < 0.0 && end.z < 0.0) {
        gl_Position = vec4(0.0, 0.0, -1.0, 1.0);
        return;
    }
    if (start.z < 0.0)
        start = mix(start, end, -start.z / (end.z - start.z));
    if (end.z < 0.0)
        end = mix(end, start, -end.z / (start.z - end.z));

    vec2 delta = (end.xy / end.w - start.xy / start.w) / view.pixelToNdc;
    vec2 direction = delta / max(length(delta), 0.0001);
    vec2 normal = vec2(-direction.y, direction.x);
    vec2 offset = (normal * corner.y + direction * (2.0 * corner.x - 1.0)) * view.halfWidth;
    vec4 clip = mix(start, end, corner.x);
    clip.xy += offset * view.pixelToNdc * clip.w;
    // Bring the strip just in front of its own surface while keeping scene occlusion.
    clip.z = max(0.0, clip.z - 0.000003 * clip.w);
    gl_Position = clip;
}
