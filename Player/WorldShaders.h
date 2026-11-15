#pragma once

inline constexpr char worldShaders[] = R"(
cbuffer View : register(b0) {
    row_major float4x4 viewProjection;
    row_major float4x4 inverseProjection;
    row_major float4x4 lightProjection;
    float4 eye;
    float4 horizon;
    float4 zenith;
    float4 fog;
    float4 sun;
    float4 lights[16];
    float4 fogCenterRadius;
    float4 lighting; // density, shadow enable, time, depth bias
};
Texture2D<float> shadowMap : register(t0);
SamplerState shadowSampler : register(s0);
Texture2D<float4> baseColorMap : register(t1);
SamplerState baseColorSampler : register(s1);
struct Vertex { float3 position : POSITION; float3 normal : NORMAL; float3 color : COLOR; float wind : TEXCOORD0; float2 uv : TEXCOORD1; };
struct Surface { float4 clip : SV_Position; float3 position : POSITION; float3 color : COLOR; float directSun : TEXCOORD0; float2 uv : TEXCOORD1; };
float3 displace(Vertex input) {
    float3 p = input.position;
    float phase = dot(p.xy,float2(0.36,0.29))+lighting.z*1.7;
    p.xy += float2(sin(phase),0.6*cos(phase*0.83))*input.wind;
    return p;
}
float4 shadowVS(Vertex input) : SV_Position { return mul(float4(displace(input),1),lightProjection); }
float visibility(float3 position,bool soft) {
    if (lighting.y < 0.5)
        return 1;
    float3 light = mul(float4(position,1),lightProjection).xyz;
    float2 uv = light.xy*float2(0.5,-0.5)+0.5;
    if (any(uv <= 0) || any(uv >= 1) || light.z <= 0 || light.z >= 1)
        return 1;
    float z = light.z-lighting.w;
    if (!soft)
        return step(z,shadowMap.SampleLevel(shadowSampler,uv,0));
    uint width,height;
    shadowMap.GetDimensions(width,height);
    float2 pixel = 1/float2(width,height);
    float result = 0;
    for (int y=-1; y<=1; ++y)
        for (int x=-1; x<=1; ++x)
            result += step(z,shadowMap.SampleLevel(shadowSampler,uv+float2(x,y)*pixel,0));
    return result/9;
}
float4 localMist(float3 direction,float distanceToSurface) {
    float radius = fogCenterRadius.w;
    if (radius <= 0 || lighting.x <= 0)
        return 0;
    float3 origin = eye.xyz-fogCenterRadius.xyz;
    float b = dot(origin,direction);
    float d = b*b-dot(origin,origin)+radius*radius;
    if (d <= 0)
        return 0;
    float nearHit = max(0,-b-sqrt(d));
    float farHit = min(distanceToSurface,-b+sqrt(d));
    if (farHit <= nearHit)
        return 0;
    float stepSize = (farHit-nearHit)/10;
    float transmission = 1;
    float3 inscatter = 0;
    for (int i=0;i<10;++i) {
        float3 p = eye.xyz+direction*(nearHit+(i+0.5)*stepSize);
        float edge = 1-smoothstep(0.60,1,length(p-fogCenterRadius.xyz)/radius);
        float opacity = 1-exp(-lighting.x*edge*stepSize);
        float illumination = 0.27+min(0.73,0.62*sun.w)*visibility(p,false);
        inscatter += transmission*opacity*fog.rgb*illumination;
        transmission *= 1-opacity;
    }
    return float4(inscatter,1-transmission);
}
Surface sceneVS(Vertex input) {
    Surface output;
    output.position = displace(input);
    output.clip = mul(float4(output.position,1),viewProjection);
    float3 normal = input.normal;
    float strength = 0.34 + 0.16*max(normal.z,0);
    float key = sun.w*max(0,dot(normal,normalize(sun.xyz)));
    strength += key;
    for (int i = 0; i < int(eye.w); ++i) {
        bool directional = lights[i].w < 0;
        float3 offset = directional ? lights[i].xyz : lights[i].xyz-input.position;
        float distanceSquared = dot(offset,offset);
        float attenuation = directional ? 1 : 1/(1+distanceSquared);
        strength += abs(lights[i].w)*attenuation*max(0,dot(normal,offset*rsqrt(max(1e-8,distanceSquared))));
    }
    output.color = input.color*saturate(strength);
    output.directSun = (saturate(strength)-saturate(strength-key))/max(0.001,saturate(strength));
    output.uv = input.uv;
    return output;
}
float4 scenePS(Surface input) : SV_Target {
    float a = clamp(-horizon.w*(eye.z-zenith.w),-15,10);
    float b = clamp(-horizon.w*(input.position.z-zenith.w),-15,10);
    float delta = abs(a-b);
    float density = delta < 0.001 ? exp((a+b)*0.5) : (exp(max(a,b))-exp(min(a,b)))/delta;
    float3 ray = input.position-eye.xyz;
    float rayLength = length(ray);
    float amount = 1-exp(-min(40,fog.w*density*rayLength));
    float towardSun = max(0,dot(ray/max(rayLength,0.0001),normalize(sun.xyz)));
    float scattering = min(1,sun.w)*pow(towardSun,12)*0.22;
    float3 albedo = input.color*baseColorMap.Sample(baseColorSampler,input.uv).rgb;
    float3 surface = albedo*(1-input.directSun*(1-visibility(input.position,true)));
    float3 haze = lerp(surface,lerp(fog.rgb,float3(1,0.83,0.64),scattering),amount);
    float4 mist = localMist(ray/max(rayLength,0.0001),rayLength);
    return float4(haze*(1-mist.a)+mist.rgb,1);
}
struct Sky { float4 clip : SV_Position; float2 ndc : TEXCOORD; };
Sky skyVS(uint vertex : SV_VertexID) {
    Sky output;
    output.ndc = float2(vertex == 1 ? 3 : -1,vertex == 2 ? 3 : -1);
    output.clip = float4(output.ndc,1,1);
    return output;
}
float4 skyPS(Sky input) : SV_Target {
    float4 world = mul(float4(input.ndc,1,1),inverseProjection);
    float3 direction = normalize(world.xyz/world.w-eye.xyz);
    float height = pow(max(direction.z,0),0.55);
    float3 color = lerp(horizon.rgb,zenith.rgb,height);
    color = lerp(color,horizon.rgb*float3(0.67,0.74,0.80),smoothstep(0,0.8,-direction.z));
    float towardSun = max(0,dot(direction,normalize(sun.xyz)));
    color += sun.w*(float3(0.42,0.25,0.12)*pow(towardSun,32)
                 + float3(0.95,0.78,0.56)*smoothstep(0.99965,0.9999,towardSun));
    float3 haze = lerp(color,fog.rgb,min(0.85,fog.w*8)*(1-height));
    float4 mist = localMist(direction,2000);
    return float4(haze*(1-mist.a)+mist.rgb,1);
}
)";
