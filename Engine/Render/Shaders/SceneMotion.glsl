// Each surface's own motion for the scene pass's velocity target (HdrFrame::velocityFormat): the part in the editor's and
// the Player's scene vertex shaders. The fragment shaders finish it with surfaceMotionOf (Lighting.glsl).
//
// The vertex shader passes two clip positions, both through the previous frame's camera: where the vertex was in the
// previous frame (its previous position, swayed by the previous frame's wind) and where it is now. Their difference on
// screen is the surface's own motion, apart from the camera's, which TAA and motion blur add to the camera's.
//
// Vertex shaders cannot include Lighting.glsl (its functions use gl_FragCoord), so this block reads only the two
// members at the end of the Lighting block (binding 1).
layout(std140, set = 0, binding = 1) uniform LightingMotion {
    layout(offset = 1904) mat4 previousViewProjection;
    vec4 settings; // w: the previous frame's wind time in seconds
} motion;

// Wind sway of foliage (the vertex's wind weight) at time seconds, as the scene shaders apply it.
vec3 swayed(vec3 position,float wind,float seconds)
{
    float phase = dot(position.xy,vec2(0.36,0.29))+seconds*1.7;
    return position+vec3(vec2(sin(phase),0.6*cos(phase*0.83))*wind,0.0);
}
