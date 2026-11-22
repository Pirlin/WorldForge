#version 450
// Bloom, step 2: each level adds the level below it, enlarged with a 3 x 3 tent filter (Jimenez 2014). Blending adds
// the result to this level's own downsample, so the top level ends up holding every level's blur.
layout(set = 0, binding = 0) uniform sampler2D source; // the smaller level, bilinear, clamped
layout(push_constant) uniform Bloom {
    vec4 settings; // texel size of the source (xy), unused, unused
    vec4 target;   // size of this level in pixels (xy)
} bloom;
layout(location = 0) out vec4 color;

void main()
{
    vec2 uv = gl_FragCoord.xy/bloom.target.xy, t = bloom.settings.xy;
    vec3 sum = texture(source,uv).rgb*4.0;
    sum += (texture(source,uv+vec2(-t.x,0)).rgb+texture(source,uv+vec2(t.x,0)).rgb
            +texture(source,uv+vec2(0,-t.y)).rgb+texture(source,uv+vec2(0,t.y)).rgb)*2.0;
    sum += texture(source,uv+vec2(-t.x,-t.y)).rgb+texture(source,uv+vec2(t.x,-t.y)).rgb
          +texture(source,uv+vec2(-t.x,t.y)).rgb+texture(source,uv+vec2(t.x,t.y)).rgb;
    color = vec4(sum/16.0,1.0);
}
