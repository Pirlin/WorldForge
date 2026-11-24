#pragma once

#include "Engine/World/Gameplay.h"
#include "Engine/World/NativeGameplay.h"
#include "Engine/World/WorldSimulation.h"
#include "Engine/Geometry/MeshClip.h"
#include "Engine/Render/ColorPipeline.h"
#include "Engine/Render/Weather.h"

#include <iosfwd>

namespace worldforge {

struct PlayVertex {
    Point3 position, normal, color;
    float wind = 0;
    Point2 uv{};
};

struct PlayMesh {
    int body = -1;
    std::vector<PlayVertex> vertices;
    std::vector<PlayVertex> lodVertices;
    Point3 lodOrigin{};
    float lodDistance = 0;
    // Encoded PNG or JPEG base-color map multiplied with the vertex colors, or empty.
    std::vector<std::uint8_t> baseColorTexture;
    std::array<std::vector<std::uint8_t>,materialTextureCount-1> surfaceMaps;
    Point3 emissive{};
    float metallic = 0, roughness = 0.65f;
    bool advancedSurface = false;
    std::optional<MeshClip> clip;
    float clearCoat = 0, clearCoatRoughness = 0; // format version 21
    MaterialLayers layers; // format version 22
    float surfaceAlpha = 1; // base-color tint alpha, format version 26
};

struct PlayLight {
    Point3 vector;
    float intensity;
    bool directional;
    // A spot light points along spotDirection with a full cone of spotAngle radians (0 for a point light), its edge
    // fading over spotBlend of the cone, as Blender's Spot Size and Blend (format version 17).
    Point3 spotDirection{0,0,-1};
    float spotAngle = 0;
    float spotBlend = 0.15f;
    Point3 color{1,1,1}; // scene-linear lamp tint, format 29
};

struct PlayableScene {
    WorldSimulation simulation;
    std::vector<PlayMesh> meshes;
    struct Foliage { int density; unsigned seed; float wind = 0.09f; };
    std::vector<Foliage> foliage;
    std::vector<PlayLight> lights;
    std::size_t player = 0;
    bool sky = true;
    Point3 horizon{}, zenith{}, fog{};
    float fogDensity = 0, fogFalloff = 0, fogHeight = 0;
    bool shadows = true;
    Point3 localFogCenter{};
    float localFogRadius = 0, localFogDensity = 0;
    // Camera exposure in stops (referenceEv100 minus EV100), whether auto exposure meters the frame
    // instead, its compensation in stops, and the display view transform (format version 14).
    float exposure = 0;
    bool autoExposure = false;
    float exposureCompensation = 0;
    // Temporal anti-aliasing (format version 14).
    bool temporalAntiAliasing = true;
    // Ground-truth ambient occlusion darkens sky light in creases, corners and openings.
    bool ambientOcclusion = true;
    bool probeGi = false, screenSpaceGi = false, reflectionProbes = false; // format 27
    // The physically based atmosphere (Engine/Render/Atmosphere.h) instead of the horizon and zenith gradient
    // (format version 15).
    bool physicalSky = false;
    Weather weather; // format 28
    // Bloom: light scattered around bright areas, as a camera lens does (format version 16 when off).
    bool bloom = true;
    // Screen-space reflections: glossy surfaces reflect what the previous frame shows (format version 19 when off).
    bool reflections = true;
    // Contact shadows from a short ray toward the sun on screen (format version 20 when off).
    bool contactShadows = true;
    // Camera effects (format version 18 when either is on): depth of field focused at focusDistance meters with the
    // aperture f-number fNumber, and motion blur with the shutter open for that share of a frame.
    bool depthOfField = false;
    float focusDistance = 10, fNumber = 2.8f;
    bool motionBlur = false;
    float shutter = 0.5f;
    color::ViewTransform viewTransform = color::ViewTransform::AgX;
    Gameplay gameplay;
    std::vector<NativeBodyBinding> nativeBodies;
    std::vector<NativeInstance> nativeInstances;
};

// Pass `material` to keep only the faces that use that material slot.
std::vector<PlayVertex> makePlayVertices(const Mesh &mesh, const Heightfield *terrain = nullptr, float foliageWind = 0,
                                         std::optional<std::uint32_t> material = std::nullopt,
                                         std::vector<VertexIndex> *vertices = nullptr);
bool writePlayable(std::ostream &output, const PlayableScene &scene);
std::optional<PlayableScene> readPlayable(std::istream &input);
void drivePlayer(BoxBody &body, Point3 spawn, float yaw, int forward, int sideways, bool jump, int &jumpBuffer);

}
