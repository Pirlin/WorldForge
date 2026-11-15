#version 450
layout(location = 0) in vec3 position;
layout(location = 2) in float wind;
layout(push_constant) uniform View {
    mat4 matrix;
    vec4 eye;
    vec4 fog;
    vec4 horizon;
    vec4 zenith;
} camera;
void main()
{
    vec3 displaced = position;
    float phase = dot(position.xy,vec2(0.36,0.29)) + camera.eye.w*1.7;
    displaced.xy += vec2(sin(phase),0.6*cos(phase*0.83))*wind;
    gl_Position = camera.matrix*vec4(displaced,1.0);
}
