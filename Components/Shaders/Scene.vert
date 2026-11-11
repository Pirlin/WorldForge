#version 450
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 color;
layout(location = 0) out vec3 shade;
layout(push_constant) uniform Camera { mat4 viewProjection; } camera;
void main()
{
    gl_Position = camera.viewProjection * vec4(position, 1.0);
    shade = color;
}
