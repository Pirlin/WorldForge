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
    float4 display; // exposure scale, view transform (0 Standard, 1 AgX)
    float4 skyLight; // ambient light scale from the sky colors
    float4 lightColors[16];
};
Texture2D<float> shadowMap : register(t0);
SamplerState shadowSampler : register(s0);
Texture2D<float4> baseColorMap : register(t1);
Texture2D<float4> normalMap : register(t2);
Texture2D<float4> metallicRoughnessMap : register(t3);
Texture2D<float4> occlusionMap : register(t4);
Texture2D<float4> emissiveMap : register(t5);
SamplerState baseColorSampler : register(s1);
cbuffer Material : register(b1) {
    float4 emissive;
    float4 factors; // metallic, roughness, advanced surface, normal map present
};
// The color pipeline matches Engine/Render/ColorPipeline.h. agxCurve and agxMatrices are
// prepended from the shared tables when the shaders compile.
float3 srgbToLinear(float3 value) {
    return value <= 0.04045 ? value/12.92 : pow((value+0.055)/1.055,2.4);
}
float3 linearToSrgb(float3 value) {
    value = saturate(value);
    return value <= 0.0031308 ? value*12.92 : 1.055*pow(value,1/2.4)-0.055;
}
float agxCurveAt(float position) {
    float scaled = saturate(position)*128;
    int index = min(int(scaled),127);
    return lerp(agxCurve[index],agxCurve[index+1],scaled-index);
}
float3 agxRow(int first,float3 value) {
    return float3(dot(float3(agxMatrices[first],agxMatrices[first+1],agxMatrices[first+2]),value),
                  dot(float3(agxMatrices[first+3],agxMatrices[first+4],agxMatrices[first+5]),value),
                  dot(float3(agxMatrices[first+6],agxMatrices[first+7],agxMatrices[first+8]),value));
}
float3 agx(float3 light) {
    float3 inset = agxRow(0,max(light,0));
    float3 position = (log2(max(inset,1e-10))+12.47393)/(4.026069+12.47393);
    float3 curved = pow(saturate(float3(agxCurveAt(position.r),agxCurveAt(position.g),agxCurveAt(position.b))),2.4);
    return linearToSrgb(agxRow(9,curved));
}
struct Vertex { float3 position : POSITION; float3 normal : NORMAL; float3 color : COLOR; float wind : TEXCOORD0; float2 uv : TEXCOORD1; };
struct Surface {
    float4 clip : SV_Position;
    float3 position : POSITION;
    float3 color : COLOR0;
    float3 tint : COLOR1;
    float3 directSun : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float3 normal : TEXCOORD2;
};
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
        inscatter += transmission*opacity*srgbToLinear(fog.rgb)*illumination;
        transmission *= 1-opacity;
    }
    return float4(inscatter,1-transmission);
}
Surface sceneVS(Vertex input) {
    Surface output;
    output.position = displace(input);
    output.clip = mul(float4(output.position,1),viewProjection);
    float3 normal = input.normal;
    float3 strength = skyLight.x*(0.34 + 0.16*max(normal.z,0));
    float key = sun.w*max(0,dot(normal,normalize(sun.xyz)));
    strength += key;
    for (int i = 0; i < int(eye.w); ++i) {
        bool directional = lights[i].w < 0;
        float3 offset = directional ? lights[i].xyz : lights[i].xyz-input.position;
        float distanceSquared = dot(offset,offset);
        float attenuation = directional ? 1 : 1/(1+distanceSquared);
        strength += lightColors[i].rgb*abs(lights[i].w)*attenuation*max(0,dot(normal,offset*rsqrt(max(1e-8,distanceSquared))));
    }
    float3 albedo = srgbToLinear(input.color);
    output.color = albedo*saturate(strength);
    output.tint = albedo;
    output.normal = input.normal;
    output.directSun = (saturate(strength)-saturate(strength-key))/max(0.001,saturate(strength));
    output.uv = float2(input.uv.x,1-input.uv.y);
    return output;
}
float3 materialLighting(Surface input,float3 albedo,float shadow) {
    const float pi = 3.14159265359;
    float3 n = normalize(input.normal);
    float3 positionX = ddx(input.position), positionY = ddy(input.position);
    float2 uvX = ddx(input.uv), uvY = ddy(input.uv);
    float3 sampled = normalMap.Sample(baseColorSampler,input.uv).xyz*2-1;
    float determinant = uvX.x*uvY.y-uvX.y*uvY.x;
    if (factors.w > 0 && abs(determinant) > 1e-10 && dot(sampled,sampled) > 1e-6) {
        float3 tangent = (positionX*uvY.y-positionY*uvX.y)*sign(determinant);
        float3 bitangent = (positionY*uvX.x-positionX*uvY.x)*sign(determinant);
        tangent -= n*dot(tangent,n);
        if (dot(tangent,tangent) > 1e-14) {
            tangent = normalize(tangent);
            float3 axis = cross(n,tangent);
            bitangent = axis*(dot(axis,bitangent) < 0 ? -1 : 1);
            n = normalize(sampled.x*tangent+sampled.y*bitangent+sampled.z*n);
        }
    }
    float3 light = normalize(sun.xyz);
    float3 view = normalize(eye.xyz-input.position);
    float3 halfVector = light+view;
    float3 h = halfVector/max(length(halfVector),0.0001);
    float nl = max(dot(n,light),0);
    float nv = max(dot(n,view),0.0001);
    float nh = max(dot(n,h),0);
    float vh = saturate(dot(view,h));
    float3 packed = metallicRoughnessMap.Sample(baseColorSampler,input.uv).rgb;
    float metallic = factors.x*packed.b;
    float roughness = max(0.045,factors.y*packed.g);
    float a = roughness*roughness;
    float a2 = a*a;
    float denominator = nh*nh*(a2-1)+1;
    float distribution = a2/(pi*denominator*denominator);
    float k = (roughness+1)*(roughness+1)/8;
    float geometry = nl/(nl*(1-k)+k)*nv/(nv*(1-k)+k);
    float3 f0 = lerp(float3(0.04,0.04,0.04),albedo,metallic);
    float3 f = f0+(1-f0)*pow(1-vh,5);
    float3 diffuse = (1-f)*(1-metallic)*albedo/pi;
    float3 specular = distribution*geometry*f/max(4*nl*nv,0.0001);
    return skyLight.x*albedo*lerp(0.34,0.18,metallic)*occlusionMap.Sample(baseColorSampler,input.uv).r
        +2.1*sun.w*shadow*(diffuse+specular)*nl+emissive.rgb*srgbToLinear(emissiveMap.Sample(baseColorSampler,input.uv).rgb);
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
    float3 baseColor = srgbToLinear(baseColorMap.Sample(baseColorSampler,input.uv).rgb);
    float shadow = visibility(input.position,true);
    float3 surface = factors.z > 0 ? materialLighting(input,input.tint*baseColor,shadow)
        : input.color*baseColor*(1-input.directSun*(1-shadow));
    float3 haze = lerp(surface,lerp(srgbToLinear(fog.rgb),srgbToLinear(float3(1,0.83,0.64)),scattering),amount);
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
    float3 horizonLight = srgbToLinear(horizon.rgb);
    float3 color = lerp(horizonLight,srgbToLinear(zenith.rgb),height);
    color = lerp(color,horizonLight*srgbToLinear(float3(0.67,0.74,0.80)),smoothstep(0,0.8,-direction.z));
    float towardSun = max(0,dot(direction,normalize(sun.xyz)));
    color += sun.w*(srgbToLinear(float3(0.42,0.25,0.12))*pow(towardSun,32)
                 + srgbToLinear(float3(0.95,0.78,0.56))*smoothstep(0.99965,0.9999,towardSun));
    float3 haze = lerp(color,srgbToLinear(fog.rgb),min(0.85,fog.w*8)*(1-height));
    float4 mist = localMist(direction,2000);
    return float4(haze*(1-mist.a)+mist.rgb,1);
}
// Display pass: exposure and view transform from the linear HDR scene to the swap chain.
Texture2D<float4> sceneLight : register(t6);
float4 displayVS(uint vertex : SV_VertexID) : SV_Position {
    return float4(vertex == 1 ? 3 : -1,vertex == 2 ? 3 : -1,0,1);
}
float4 displayPS(float4 position : SV_Position) : SV_Target {
    float3 light = sceneLight.Load(int3(position.xy,0)).rgb*display.x;
    float3 encoded = display.y > 0.5 ? agx(light) : linearToSrgb(light);
    // Half a level of ordered noise keeps 8-bit sky gradients from banding.
    float noise = frac(52.9829189*frac(dot(position.xy,float2(0.06711056,0.00583715))));
    return float4(saturate(encoded+(noise-0.5)/255),1);
}
)";
