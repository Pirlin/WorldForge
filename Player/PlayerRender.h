#pragma once

// The Player's renderer interface. Vulkan is the main backend (Player/VulkanRenderer.cpp);
// Direct3D 11 remains as the fallback for computers without a Vulkan driver.

#include "Engine/Render/ShadowCascades.h"
#include "Engine/Render/SkyLight.h"
#include "Engine/Render/SkyVisibility.h"
#include "Engine/Render/LightClusters.h"
#include "Engine/World/PlayableScene.h"

#include <windows.h>
#include <DirectXMath.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace worldforge::player {

struct VertexUpload {
    std::size_t first = 0, count = 0;
};

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
    DirectX::XMFLOAT4 lightColors[16]; // tint of the fallback renderer's first 16 lights
    // Light from the sky (color::skyLight): irradiance / pi as spherical harmonics, and linear sky radiance at the
    // horizon, the zenith and the ground. Direct3D 11 does not use them.
    DirectX::XMFLOAT4 skyIrradiance[9], skyHorizon, skyZenith, skyGround;
    DirectX::XMFLOAT4 sunTint; // the color of sunlight at the ground, luminance 1
    // The sky's environment map (color::SkyLight::environment) and a number that changes whenever it does.
    // Direct3D 11 does not use them.
    const std::vector<std::array<float,4>> *skyEnvironment = nullptr;
    std::uint64_t skyRevision = 0;
    // Every light except the sun, for clustered lighting (Engine/Render/LightClusters.h). Direct3D 11 uses the first 16
    // in lights instead.
    const std::vector<lights::Light> *sceneLights = nullptr;
    // The authored sun intensity (sunlight with the sun overhead), from which the physical sky is built; sun.w is what
    // reaches the ground, 0 once the sun has set.
    float authoredSun = 0;
    float bloom = 0; // the share of light scattered around bright areas (color::bloomStrength), 0 for none
    // Camera effects (HdrFrame::Camera): depth of field on, focus distance in meters, f-number, vertical field of view
    // in radians; motion blur on, shutter (share of the frame), screen-space reflections on, contact shadows on.
    DirectX::XMFLOAT4 lens{0,10,2.8f,0.785398f}, shutter{0,0.5f,0,0};
    // Sun shadow cascades (Engine/Render/ShadowCascades.h), fitted to the view without TAA jitter. Direct3D 11 uses
    // lightProjection instead.
    shadow::Cascades cascades;
    // Large-scale sky occlusion maps (Engine/Render/SkyVisibility.h), fitted to the scene; used with ambient occlusion.
    sky::Visibility skyVisibility;
    uint32_t temporalSample = 0; // the TAA frame in its sequence, which turns the shadow filter pattern
    uint32_t skyMaps = 0;        // 1 when skyVisibility covers the scene
    uint32_t skyRefresh = 0;     // 1 when this frame re-renders the sky visibility maps (set by the renderer)
    // Probe GI, screen-space GI and reflection probes on; movingGeometry is 1 when any mesh moves with a body or clip, so the
    // probe field refreshes as the scene moves rather than only when the camera or light changes.
    uint32_t probeGi = 0, screenSpaceGi = 0, reflectionProbes = 0, movingGeometry = 0;
    DirectX::XMFLOAT4 weather{}; // aerial haze per kilometer, cloud cover
    // The Player keeps its fixed-size mesh vertices at the same offsets during play. Each Vulkan buffer in the ring
    // needs a full upload once, then refreshes only these moving spans and anything beyond the cached prefix.
    const std::vector<VertexUpload> *movingVertices = nullptr;
    std::size_t cachedMeshVertices = 0;
    uint32_t drawCulling = 1, cullingPadding[3]{};
};
// Direct3D 11 uploads View as a constant buffer, whose size must be a multiple of 16 bytes.
static_assert(sizeof(View)%16 == 0);

struct MaterialDraw {
    std::array<int,materialTextureCount> textures = [] {
        std::array<int,materialTextureCount> indices;
        indices.fill(-1);
        return indices;
    }();
    Point3 emissive{};
    float metallic = 0, roughness = 0.65f;
    bool advanced = false;
    float clearCoat = 0, clearCoatRoughness = 0;
    MaterialLayers layers;
    float surfaceAlpha = 1;
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

// What the last frame submitted, for the city-scale benchmark (--verify-district): draw calls and triangles in the scene
// pass, and in the shadow and sky-visibility depth passes.
struct RenderStats {
    uint32_t meshletShaderGroups = 0, meshletIndexedDraws = 0;
    uint32_t texturesPromoted = 0, textureWaitFrames = 0;
    std::size_t textureDetailBytes = 0;
    double textureStallMs = 0;
    uint32_t occludedOpaqueDraws = 0;
    uint32_t rejectedOpaqueDraws = 0;
    uint32_t rejectedOpaqueTriangles = 0;
    uint32_t pointShadowFaces = 0;
    bool compactedShadowDraws = false;
    std::uint32_t sceneDraws = 0, depthDraws = 0;
    std::uint64_t sceneTriangles = 0, depthTriangles = 0;
    // GPU time in milliseconds of a recent frame (a frame or two behind, when the GPU has finished it) for each pass:
    // sun shadow cascades, sky visibility maps, point light shadows, the scene pass, and everything after it (ambient
    // occlusion, bloom, TAA, camera effects and the display step). All zero when the device cannot time.
    enum Pass { SunShadows, SkyMaps, PointShadows, Scene, PostProcess, passCount };
    std::array<float,passCount> gpuMs{};
};

class Renderer {
public:
    virtual ~Renderer() = default;
    // Backends that do not count return zeros.
    virtual RenderStats stats() const { return {}; }
    // Adds a texture with generated mipmaps. Returns its index, or -1 when it cannot be used.
    virtual int addTexture(const Image &image) = 0;
    virtual bool draw(const std::vector<PlayVertex> &vertices,const std::vector<DrawRange> &ranges,const View &view,
                      UINT width,UINT height,bool sky,bool verify,UINT sync) = 0;
    // Renders one frame off screen. Backends without capture return false.
    virtual bool capture(const std::vector<PlayVertex> &,const std::vector<DrawRange> &,const View &,
                         UINT,UINT,bool,Image &,float * = nullptr) { return false; }
    virtual const std::wstring &adapterName() const = 0;
    virtual const char *api() const = 0;
};

} // namespace worldforge::player
