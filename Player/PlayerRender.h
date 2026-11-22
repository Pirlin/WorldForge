#pragma once

// The Player's renderer interface. Vulkan is the main backend (Player/VulkanRenderer.cpp);
// Direct3D 11 remains as the fallback for computers without a Vulkan driver.

#include "Engine/Render/ShadowCascades.h"
#include "Engine/Render/SkyLight.h"
#include "Engine/Render/SkyVisibility.h"
#include "Engine/World/PlayableScene.h"

#include <windows.h>
#include <DirectXMath.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace worldforge::player {

// Per-frame camera, lights and atmosphere. Matrices use Direct3D conventions: row vectors,
// clip depth 0 to 1, and clip y pointing up. The Vulkan backend flips y.
struct View {
    DirectX::XMFLOAT4X4 viewProjection, inverseProjection, lightProjection;
    DirectX::XMFLOAT4 eye, horizon, zenith, fog, sun, lights[16];
    DirectX::XMFLOAT4 fogCenterRadius, lighting; // lighting: local fog density, shadows, time, depth bias (Direct3D 11)
    // Manual exposure scale, view transform (0 Standard, 1 AgX), auto exposure (1 on), and its compensation in
    // stops. Backends without metering (Direct3D 11) use the manual exposure.
    DirectX::XMFLOAT4 display;
    // Ambient light scale from the sky colors (color::skyAmbientScale), TAA, ambient occlusion, physical sky.
    DirectX::XMFLOAT4 sky;
    // Light from the sky (color::skyLight): irradiance / pi as spherical harmonics, and linear sky radiance at the
    // horizon, the zenith and the ground. Direct3D 11 does not use them.
    DirectX::XMFLOAT4 skyIrradiance[9], skyHorizon, skyZenith, skyGround;
    DirectX::XMFLOAT4 sunTint; // the color of sunlight at the ground, luminance 1
    // The sky's environment map (color::SkyLight::environment) and a number that changes whenever it does.
    // Direct3D 11 does not use them.
    const std::vector<std::array<float,4>> *skyEnvironment = nullptr;
    std::uint64_t skyRevision = 0;
    // Sun shadow cascades (Engine/Render/ShadowCascades.h), fitted to the view without TAA jitter. Direct3D 11 uses
    // lightProjection instead.
    shadow::Cascades cascades;
    // Large-scale sky occlusion maps (Engine/Render/SkyVisibility.h), fitted to the scene; used with ambient occlusion.
    sky::Visibility skyVisibility;
    uint32_t temporalSample = 0; // the TAA frame in its sequence, which turns the shadow filter pattern
    uint32_t skyMaps = 0;        // 1 when skyVisibility covers the scene
    uint32_t skyRefresh = 0;     // 1 when this frame re-renders the sky visibility maps (set by the renderer)
};
// Direct3D 11 uploads View as a constant buffer, whose size must be a multiple of 16 bytes.
static_assert(sizeof(View)%16 == 0);

struct MaterialDraw {
    std::array<int,5> textures{-1,-1,-1,-1,-1}; // base color, normal, metallic-roughness, occlusion, emission
    Point3 emissive{};
    float metallic = 0, roughness = 0.65f;
    bool advanced = false;
    bool operator==(const MaterialDraw &) const = default;
};

struct DrawRange {
    UINT start = 0, count = 0;
    MaterialDraw material;
};

// RGBA8 pixels, rows top to bottom.
struct Image {
    UINT width = 0, height = 0;
    std::vector<std::uint32_t> pixels;
};

class Renderer {
public:
    virtual ~Renderer() = default;
    // Adds a texture with generated mipmaps. Returns its index, or -1 when it cannot be used.
    virtual int addTexture(const Image &image) = 0;
    virtual bool draw(const std::vector<PlayVertex> &vertices,const std::vector<DrawRange> &ranges,const View &view,
                      UINT width,UINT height,bool sky,bool verify,UINT sync) = 0;
    // Renders one frame off screen. Backends without capture return false.
    virtual bool capture(const std::vector<PlayVertex> &,const std::vector<DrawRange> &,const View &,
                         UINT,UINT,bool,Image &) { return false; }
    virtual const std::wstring &adapterName() const = 0;
    virtual const char *api() const = 0;
};

} // namespace worldforge::player
