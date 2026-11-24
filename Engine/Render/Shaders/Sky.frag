#version 450
#extension GL_GOOGLE_include_directive : require
#include "Lighting.glsl"
#include "ColorPipeline.glsl"
layout(early_fragment_tests) in;
layout(location = 0) in vec2 screen;
layout(location = 0) out vec4 color;
layout(push_constant) uniform View {
    mat4 matrix;
    vec4 eye;
    vec4 fog;
    vec4 horizon;
    vec4 zenith;
} camera;

float cloudHash(vec2 cell)
{
    vec3 p = fract(vec3(cell.xyx)*0.1031);
    p += dot(p,p.yzx+33.33);
    return fract((p.x+p.y)*p.z);
}

float cloudNoise(vec2 p)
{
    vec2 cell = floor(p), uv = fract(p);
    uv = uv*uv*(3.0-2.0*uv);
    return mix(mix(cloudHash(cell),cloudHash(cell+vec2(1,0)),uv.x),
               mix(cloudHash(cell+vec2(0,1)),cloudHash(cell+vec2(1,1)),uv.x),uv.y);
}

float cloudOpacity(vec3 eye,vec3 direction,float cover)
{
    const float cloudAltitude = 1800.0;
    float safeZ = direction.z >= 0.0 ? max(0.025,direction.z) : min(-0.025,direction.z);
    float distance = (cloudAltitude-eye.z)/safeZ;
    if (distance <= 0.0)
        return 0.0;
    vec2 p = (eye.xy+direction.xy*distance)*0.0008;
    float structure = 0.60*cloudNoise(p)+0.28*cloudNoise(2.03*p+vec2(7.0,19.0))
        +0.12*cloudNoise(4.17*p+vec2(31.0,5.0));
    float edge = 1.15-1.24*cover;
    float opacity = smoothstep(edge,edge+0.16,structure);
    return mix(cover,opacity,smoothstep(0.025,0.10,abs(direction.z)));
}

void main()
{
    vec4 world = camera.matrix * vec4(screen, 1.0, 1.0);
    vec3 direction = normalize(world.xyz / world.w - camera.eye.xyz);
    float height = pow(max(direction.z, 0.0), 0.55);
    // Sky and fog colors are authored in sRGB; the sky is lit in linear HDR.
    vec3 horizon = srgbToLinear(camera.horizon.rgb), zenith = srgbToLinear(camera.zenith.rgb);
    vec3 tint = srgbToLinear(camera.fog.rgb);
    float intensity = lighting.sun.w;
    float towardSun = max(0.0,dot(direction,lighting.sun.xyz));
    vec3 sky;
    if (lighting.skySettings.z > 0.5) {
        sky = skyEnvironment(direction, 0);
        // Use the same disc size as the sun shadows and smooth its edge over a pixel.
        float chord = length(direction-lighting.sun.xyz);
        float radius = 2.0*sin(0.25*sunAngularDiameter);
        float edge = max(0.5*fwidth(chord),0.000001);
        float disc = 1.0-smoothstep(max(0.0,radius-edge),radius+edge,chord);
        float horizonEdge = max(0.5*fwidth(direction.z),0.000001);
        float horizonCosine = lighting.skyZenith.w;
        float visible = smoothstep(horizonCosine-horizonEdge,horizonCosine+horizonEdge,direction.z);
        sky += lighting.skyHorizon.w * lighting.sunTint.rgb * 50.0 * disc * visible;
        float cover = lighting.sunTint.w;
        if (cover > 0.0) {
            float opacity = cloudOpacity(camera.eye.xyz,direction,cover);
            vec3 ambient = lighting.skyIrradiance[0].rgb*0.282095;
            vec3 cloud = mix(ambient,vec3(dot(ambient,vec3(0.2126,0.7152,0.0722))),0.55)*1.6;
            cloud += intensity*lighting.sunTint.rgb*(0.12+0.24*pow(towardSun,8.0));
            sky = mix(sky,cloud,opacity);
        }
    } else {
        sky = mix(horizon, zenith, height);
        sky = mix(sky, horizon * srgbToLinear(vec3(0.67, 0.74, 0.80)), smoothstep(0.0, 0.8, -direction.z));
        sky += intensity * srgbToLinear(vec3(0.42, 0.25, 0.12)) * pow(towardSun, 32.0);
        sky += intensity * srgbToLinear(vec3(0.95, 0.78, 0.56)) * smoothstep(0.99965, 0.9999, towardSun);
    }
    const float skyDistance = 2000.0;
    vec4 fog = heightFog(camera.eye.xyz,direction,skyDistance,tint,camera.fog.w,camera.horizon.w,camera.zenith.w);
    vec4 local = localMist(camera.eye.xyz,direction,skyDistance,tint,intensity);
    color = vec4((sky*fog.a+fog.rgb)*(1.0-local.a)+local.rgb,1.0);
}
