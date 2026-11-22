#version 450
// Bloom, step 1: one level of the downsample chain (Jimenez 2014, "Next Generation Post Processing in Call of Duty:
// Advanced Warfare"). Thirteen bilinear taps of the level above, in five overlapping boxes, give a half-size image
// without the shimmer of a plain 2 x 2 average. The first level, read from the full-size scene, weights each box by
// the inverse of its brightness (Karis average), so a few very bright pixels such as the sun's disc cannot flicker.
layout(set = 0, binding = 0) uniform sampler2D source; // bilinear, clamped
layout(push_constant) uniform Bloom {
    vec4 settings; // texel size of the source (xy), first level, unused
    vec4 target;   // size of this level in pixels (xy)
} bloom;
layout(location = 0) out vec4 color;

vec3 tap(vec2 uv,vec2 offset)
{
    return texture(source,uv+offset*bloom.settings.xy).rgb;
}

float karis(vec3 c)
{
    return 1.0/(1.0+dot(c,vec3(0.2126,0.7152,0.0722)));
}

void main()
{
    vec2 uv = gl_FragCoord.xy/bloom.target.xy;
    vec3 a = tap(uv,vec2(-2,-2)), b = tap(uv,vec2(0,-2)), c = tap(uv,vec2(2,-2));
    vec3 d = tap(uv,vec2(-1,-1)), e = tap(uv,vec2(1,-1));
    vec3 f = tap(uv,vec2(-2,0)), g = tap(uv,vec2(0,0)), h = tap(uv,vec2(2,0));
    vec3 i = tap(uv,vec2(-1,1)), j = tap(uv,vec2(1,1));
    vec3 k = tap(uv,vec2(-2,2)), l = tap(uv,vec2(0,2)), m = tap(uv,vec2(2,2));
    vec3 boxes[5] = vec3[5]((d+e+i+j)*0.25,(a+b+f+g)*0.25,(b+c+g+h)*0.25,(f+g+k+l)*0.25,(g+h+l+m)*0.25);
    float weights[5] = float[5](0.5,0.125,0.125,0.125,0.125);
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int n = 0; n < 5; ++n) {
        float w = weights[n]*(bloom.settings.z > 0.5 ? karis(boxes[n]) : 1.0);
        sum += boxes[n]*w;
        total += w;
    }
    color = vec4(max(sum/max(total,1e-6),vec3(0.0)),1.0);
}
