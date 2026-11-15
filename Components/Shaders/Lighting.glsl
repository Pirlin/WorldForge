layout(set = 0, binding = 0) uniform sampler2D shadowMap;
layout(std140, set = 0, binding = 1) uniform Lighting {
    mat4 lightMatrix;
    vec4 fogCenterRadius;
    vec4 settings; // local density, shadows enabled, shadow receiver bias
} lighting;

float sunVisibility(vec3 position, bool soft)
{
    if (lighting.settings.y < 0.5)
        return 1.0;
    vec3 light = (lighting.lightMatrix * vec4(position, 1.0)).xyz;
    vec2 uv = light.xy * 0.5 + 0.5;
    if (any(lessThanEqual(uv,vec2(0.0))) || any(greaterThanEqual(uv,vec2(1.0))) || light.z <= 0.0 || light.z >= 1.0)
        return 1.0;
    float z = light.z - lighting.settings.z;
    if (!soft)
        return step(z,textureLod(shadowMap,uv,0.0).r);
    vec2 pixel = 1.0 / vec2(textureSize(shadowMap,0));
    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            visibility += step(z,textureLod(shadowMap,uv + vec2(x,y)*pixel,0.0).r);
    return visibility/9.0;
}

vec4 localMist(vec3 eye, vec3 direction, float distanceToSurface, vec3 tint, float sunlight)
{
    const int steps = 10;
    float radius = lighting.fogCenterRadius.w;
    if (radius <= 0.0 || lighting.settings.x <= 0.0)
        return vec4(0.0);
    vec3 origin = eye - lighting.fogCenterRadius.xyz;
    float b = dot(origin,direction);
    float d = b*b-dot(origin,origin)+radius*radius;
    if (d <= 0.0)
        return vec4(0.0);
    float nearHit = max(0.0,-b-sqrt(d));
    float farHit = min(distanceToSurface,-b+sqrt(d));
    if (farHit <= nearHit)
        return vec4(0.0);
    float stepSize = (farHit-nearHit)/float(steps);
    float transmission = 1.0;
    vec3 inscatter = vec3(0.0);
    for (int i = 0; i < steps; ++i) {
        vec3 p = eye+direction*(nearHit+(float(i)+0.5)*stepSize);
        float edge = 1.0-smoothstep(0.60,1.0,length(p-lighting.fogCenterRadius.xyz)/radius);
        float opacity = 1.0-exp(-lighting.settings.x*edge*stepSize);
        float illumination = 0.27 + min(0.73,0.62*sunlight)*sunVisibility(p,false);
        inscatter += transmission*opacity*tint*illumination;
        transmission *= 1.0-opacity;
    }
    return vec4(inscatter,1.0-transmission);
}
