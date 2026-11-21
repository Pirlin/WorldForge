// Per-frame camera and lights for the runtime scene shader (Player).
layout(std140, set = 0, binding = 3) uniform PlayView {
    mat4 viewProjection; // Vulkan clip space
    vec4 eye;            // position, point light count
    vec4 horizon;        // sRGB color, fog height falloff
    vec4 zenith;         // sRGB color, fog base height
    vec4 fog;            // sRGB color, density
    vec4 sun;            // direction, intensity
    vec4 lights[16];     // position or direction, intensity (negative for directional)
    vec4 time;           // seconds, ambient light scale from the sky colors
} view;
