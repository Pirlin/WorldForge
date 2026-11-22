#pragma once

#include "Engine/World/Gameplay.h"
#include "Engine/World/NativeGameplay.h"
#include "Engine/World/WorldSimulation.h"
#include "Engine/Geometry/MeshClip.h"
#include "Engine/Render/ColorPipeline.h"

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
    std::array<std::vector<std::uint8_t>,4> surfaceMaps;
    Point3 emissive{};
    float metallic = 0, roughness = 0.65f;
    bool advancedSurface = false;
    std::optional<MeshClip> clip;
};

struct PlayLight {
    Point3 vector;
    float intensity;
    bool directional;
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
    // The physically based atmosphere (Engine/Render/Atmosphere.h) instead of the horizon and zenith gradient
    // (format version 15).
    bool physicalSky = false;
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
