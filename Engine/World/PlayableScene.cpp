#include "Engine/World/PlayableScene.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <istream>
#include <limits>
#include <locale>
#include <ostream>
#include <cstring>
#include <string>

namespace worldforge {
namespace {
template<std::size_t N>
void writeVector(std::ostream &out,const std::array<float,N> &v)
{
    for (float value : v)
        out << value << ' ';
}

bool readValue(std::istream &in,float &value,float minimum,float maximum)
{
    return bool(in >> value) && std::isfinite(value) && value >= minimum && value <= maximum;
}

template<std::size_t N>
bool readVector(std::istream &in,std::array<float,N> &v,float minimum = -5000000,float maximum = 5000000)
{
    for (float &value : v)
        if (!readValue(in,value,minimum,maximum))
            return false;
    return true;
}

constexpr char base64Digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void writeBase64(std::ostream &out,const std::vector<std::uint8_t> &bytes)
{
    std::string text;
    text.reserve((bytes.size()+2)/3*4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t chunk = std::uint32_t(bytes[i]) << 16
            | (i+1 < bytes.size() ? std::uint32_t(bytes[i+1]) << 8 : 0u)
            | (i+2 < bytes.size() ? std::uint32_t(bytes[i+2]) : 0u);
        text += base64Digits[chunk >> 18 & 63];
        text += base64Digits[chunk >> 12 & 63];
        text += i+1 < bytes.size() ? base64Digits[chunk >> 6 & 63] : '=';
        text += i+2 < bytes.size() ? base64Digits[chunk & 63] : '=';
    }
    out << text;
}

bool readBase64(std::istream &in,std::size_t size,std::vector<std::uint8_t> &bytes)
{
    std::string text;
    if (!(in >> std::setw(std::streamsize((size+2)/3*4+1)) >> text) || text.size() != (size+2)/3*4)
        return false;
    bytes.clear();
    bytes.reserve(size);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        std::uint32_t chunk = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            const char c = text[i+j];
            const char *digit = c == '=' ? nullptr : std::strchr(base64Digits,c);
            if ((c != '=' && (!digit || c == '\0')) || (c == '=' && i+4 != text.size()))
                return false;
            chunk = chunk << 6 | std::uint32_t(digit ? digit-base64Digits : 0);
        }
        for (int shift : {16,8,0})
            if (bytes.size() < size)
                bytes.push_back(std::uint8_t(chunk >> shift & 255));
    }
    return bytes.size() == size;
}

constexpr std::size_t maxTextBytes = 160;

bool fitsText(const std::string &text)
{
    return text.size() <= maxTextBytes;
}

bool readText(std::istream &in,std::string &text)
{
    return bool(in >> std::quoted(text)) && fitsText(text);
}
}

std::vector<PlayVertex> makePlayVertices(const Mesh &mesh,const Heightfield *terrain,float foliageWind,
                                         std::optional<std::uint32_t> material,std::vector<VertexIndex> *indices)
{
    std::vector<PlayVertex> vertices;
    for (const auto &face : mesh.faces) {
        if (material && face.material != *material)
            continue;
        const auto normal = faceNormal(mesh,face);
        const auto &color = mesh.materials[face.material].color;
        visitFaceTriangles(mesh,face,[&](const auto &triangle) {
            for (const auto index : triangle) {
                const auto &corner = face.corners[index];
                if (indices)
                    indices->push_back(corner.vertex);
                const auto position = mesh.vertices[corner.vertex];
                const auto direction = corner.normal == Point3{} ? normal : corner.normal;
                vertices.push_back({position,direction,terrain ? terrain->surfaceColor(position,direction)
                    : Point3{color[0],color[1],color[2]},foliageWind > 0 ? Heightfield::foliageSway(mesh,corner.vertex)*foliageWind : 0,
                    corner.uv});
            }
        });
    }
    return vertices;
}

bool writePlayable(std::ostream &out,const PlayableScene &scene)
{
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<float>::max_digits10);
    const auto &world = scene.simulation;
    if (scene.foliage.size() != world.terrains.size() || scene.player >= world.bodies.size())
        return false;
    const bool hasLaunch = std::ranges::any_of(scene.gameplay.behaviors,[](const BehaviorRule &rule) {
        return rule.action == BehaviorRule::Action::LaunchUpward;
    });
    const bool hasRemoteLaunch = std::ranges::any_of(scene.gameplay.behaviors,[](const BehaviorRule &rule) {
        return !rule.followLaunchTarget;
    });
    const bool display = scene.exposure != 0 || scene.viewTransform != color::ViewTransform::AgX || scene.autoExposure
        || scene.exposureCompensation != 0 || !scene.temporalAntiAliasing || !scene.ambientOcclusion;
    const bool spots = std::ranges::any_of(scene.lights,[](const PlayLight &light) { return light.spotAngle > 0; });
    const int version = !scene.contactShadows ? 20 : !scene.reflections ? 19 : scene.depthOfField || scene.motionBlur ? 18 : spots ? 17 : !scene.bloom ? 16 : scene.physicalSky ? 15 : display ? 14 : !scene.nativeInstances.empty() ? 13 : !scene.nativeBodies.empty() ? 12
        : hasRemoteLaunch ? 11 : hasLaunch ? 10 : 9;
    if (!std::isfinite(scene.exposure) || scene.exposure < color::minimumExposure || scene.exposure > color::maximumExposure
        || !std::isfinite(scene.exposureCompensation) || std::abs(scene.exposureCompensation) > color::maximumCompensation)
        return false;
    out << "WORLDFORGE_PLAY " << version << '\n'
        << world.bodies.size() << ' ' << world.terrains.size() << ' '
        << world.joints.size() << ' ' << scene.meshes.size() << ' ' << scene.lights.size() << ' '
        << scene.player << ' ' << world.gravity << '\n';
    out << scene.sky << ' ' << scene.fogDensity << ' ' << scene.fogFalloff << ' ' << scene.fogHeight << ' ';
    writeVector(out,scene.horizon);
    writeVector(out,scene.zenith);
    writeVector(out,scene.fog);
    out << scene.shadows << ' ' << scene.localFogRadius << ' ' << scene.localFogDensity << ' ';
    writeVector(out,scene.localFogCenter);
    if (version >= 14)
        out << scene.exposure << ' ' << int(scene.viewTransform) << ' ' << scene.autoExposure << ' '
            << scene.exposureCompensation << ' ' << scene.temporalAntiAliasing << ' '
            << scene.ambientOcclusion << ' ';
    if (version >= 15)
        out << scene.physicalSky << ' ';
    if (version >= 16)
        out << scene.bloom << ' ';
    if (version >= 18)
        out << scene.depthOfField << ' ' << scene.focusDistance << ' ' << scene.fNumber << ' ' << scene.motionBlur << ' '
            << scene.shutter << ' ';
    if (version >= 19)
        out << scene.reflections << ' ';
    if (version >= 20)
        out << scene.contactShadows << ' ';
    out << '\n';
    for (const auto &body : world.bodies) {
        writeVector(out,body.position);
        writeVector(out,body.halfSize);
        writeVector(out,body.velocity);
        writeVector(out,body.rotation);
        writeVector(out,body.angularVelocity);
        out << body.bounce << ' ' << body.dynamic << ' ' << body.lockRotation << ' '
            << body.mass << ' ' << body.friction << ' ' << int(body.shape) << '\n';
    }
    for (std::size_t i = 0; i < world.terrains.size(); ++i) {
        const auto &ground = world.terrains[i];
        writeVector(out,ground.position);
        out << ground.field.size << ' ' << scene.foliage[i].density << ' ' << scene.foliage[i].seed << ' '
            << scene.foliage[i].wind << ' ';
        writeVector(out,ground.field.samples);
        for (auto layer : ground.field.layers)
            out << int(layer) << ' ';
        out << '\n';
    }
    for (const auto &joint : world.joints) {
        out << int(joint.type) << ' ' << joint.first << ' ' << joint.second << ' ';
        writeVector(out,joint.anchor);
        writeVector(out,joint.axis);
        out << '\n';
    }
    for (const auto &light : scene.lights) {
        writeVector(out,light.vector);
        out << light.intensity << ' ' << light.directional;
        if (version >= 17) {
            out << ' ';
            writeVector(out,light.spotDirection);
            out << light.spotAngle << ' ' << light.spotBlend;
        }
        out << '\n';
    }
    std::size_t remainingClipSamples = MeshClip::maxSamples*3;
    for (const auto &mesh : scene.meshes) {
        if (mesh.baseColorTexture.size() > MeshMaterial::maxTextureBytes)
            return false;
        out << mesh.body << ' ' << mesh.vertices.size() << ' ' << mesh.lodVertices.size() << ' ' << mesh.lodDistance << ' ';
        writeVector(out,mesh.lodOrigin);
        out << mesh.baseColorTexture.size() << '\n';
        if (!mesh.baseColorTexture.empty()) {
            writeBase64(out,mesh.baseColorTexture);
            out << '\n';
        }
        out << mesh.advancedSurface << '\n';
        if (mesh.advancedSurface) {
            out << mesh.metallic << ' ' << mesh.roughness << ' ';
            writeVector(out,mesh.emissive);
            out << '\n';
            for (const auto &map : mesh.surfaceMaps) {
                if (map.size() > MeshMaterial::maxTextureBytes)
                    return false;
                out << map.size() << '\n';
                if (!map.empty()) {
                    writeBase64(out,map);
                    out << '\n';
                }
            }
        }
        out << bool(mesh.clip) << '\n';
        if (mesh.clip) {
            const auto &clip = *mesh.clip;
            if (!clip.valid(mesh.vertices.size()) || !mesh.lodVertices.empty() || clip.samples.size() > remainingClipSamples)
                return false;
            remainingClipSamples -= clip.samples.size();
            out << std::quoted(clip.name) << ' ' << clip.framesPerSecond << ' ' << clip.frameCount() << '\n';
            for (const auto &vertex : clip.samples) {
                writeVector(out,vertex.position);
                writeVector(out,vertex.normal);
                out << '\n';
            }
        }
        for (const auto *vertices : {&mesh.vertices,&mesh.lodVertices})
            for (const auto &vertex : *vertices) {
                writeVector(out,vertex.position);
                writeVector(out,vertex.normal);
                writeVector(out,vertex.color);
                writeVector(out,vertex.uv);
                out << '\n';
            }
    }
    const auto &play = scene.gameplay;
    if (play.routes.size() > 64 || play.vehicles.size() > 128 || play.missions.size() > 32)
        return false;
    out << play.routes.size() << ' ' << play.vehicles.size() << ' ' << play.missions.size() << '\n';
    for (const auto &route : play.routes) {
        if (route.points.size() > 256)
            return false;
        out << route.points.size() << ' ' << route.speed << ' ';
        for (const auto &p : route.points)
            writeVector(out,p);
        out << '\n';
    }
    for (const auto &vehicle : play.vehicles) {
        if (vehicle.body >= world.bodies.size() || vehicle.route >= play.routes.size())
            return false;
        out << vehicle.body << ' ' << vehicle.route << '\n';
    }
    for (const auto &mission : play.missions) {
        if (!fitsText(mission.name) || !fitsText(mission.goal) || !fitsText(mission.completionText))
            return false;
        out << std::quoted(mission.name) << ' ' << std::quoted(mission.goal) << ' ';
        writeVector(out,mission.start);
        writeVector(out,mission.waypoint);
        writeVector(out,mission.target);
        out << mission.hasWaypoint << ' ' << mission.startRadius << ' ' << mission.waypointRadius << ' '
            << mission.targetRadius << ' ' << std::quoted(mission.completionText) << '\n';
    }
    if (play.beacons.size() > 32)
        return false;
    out << play.beacons.size() << '\n';
    for (const auto &beacon : play.beacons) {
        if (beacon.mission >= play.missions.size() || !(beacon.seconds >= 0.1f && beacon.seconds <= 15))
            return false;
        out << beacon.mission << ' ' << beacon.seconds << ' ';
        writeVector(out,beacon.position);
        writeVector(out,beacon.color);
        out << '\n';
    }
    if (play.shots.size() > 32 || play.patrols.size() > 32)
        return false;
    out << play.showMissionHud << ' ' << play.shots.size() << ' ' << play.patrols.size() << '\n';
    for (const auto &shot : play.shots) {
        if (shot.mission >= play.missions.size() || !(shot.seconds >= 0.1f && shot.seconds <= 15))
            return false;
        out << shot.mission << ' ' << shot.seconds << ' ';
        writeVector(out,shot.first);
        writeVector(out,shot.last);
        writeVector(out,shot.target);
        out << '\n';
    }
    for (const auto &patrol : play.patrols) {
        if (!(patrol.speed >= 0.1f && patrol.speed <= 10 && patrol.wait >= 0 && patrol.wait <= 15))
            return false;
        writeVector(out,patrol.start);
        writeVector(out,patrol.destination);
        out << patrol.speed << ' ' << patrol.wait << '\n';
    }
    if (play.behaviors.size() > 128 || play.behaviorBeacons.size() > 128)
        return false;
    out << play.behaviors.size() << ' ' << play.behaviorBeacons.size() << '\n';
    for (const auto &rule : play.behaviors) {
        if (!rule.valid(play.missions.size(),world.bodies.size(),play.behaviorBeacons.size())
            || !rule.validBody(world))
            return false;
        out << std::quoted(rule.label) << ' ' << int(rule.event) << ' ' << int(rule.action) << ' '
            << (rule.mission ? int(*rule.mission) : -1) << ' ' << (rule.requiredMission ? int(*rule.requiredMission) : -1)
            << ' ' << rule.target << ' ' << rule.amount << ' ';
        writeVector(out,rule.position);
        if (version >= 11)
            out << ' ' << int(rule.followLaunchTarget);
        out << '\n';
    }
    for (const auto &beacon : play.behaviorBeacons) {
        writeVector(out,beacon.position);
        writeVector(out,beacon.color);
        out << '\n';
    }
    if (version >= 12) {
        if (scene.nativeBodies.size() > 256)
            return false;
        out << scene.nativeBodies.size() << '\n';
        for (const auto &binding : scene.nativeBodies) {
            if (binding.id.empty() || binding.id.size() > 128 || binding.index >= world.bodies.size())
                return false;
            out << std::quoted(binding.id) << ' ' << binding.index << '\n';
        }
    }
    if (version >= 13) {
        if (scene.nativeInstances.size() > 256)
            return false;
        out << std::setprecision(std::numeric_limits<double>::max_digits10) << scene.nativeInstances.size() << '\n';
        for (const auto &instance : scene.nativeInstances) {
            if (instance.owner.empty() || instance.owner.size() > 128 || instance.type.empty()
                || instance.type.size() > 64 || instance.properties.size() > 32)
                return false;
            out << std::quoted(instance.owner) << ' ' << std::quoted(instance.type) << ' ' << instance.properties.size() << '\n';
            for (const auto &[id,value] : instance.properties) {
                if (id.empty() || id.size() > 64 || !value.valid())
                    return false;
                out << std::quoted(id) << ' ' << value.kind << ' ' << value.number << ' ' << std::quoted(value.reference) << '\n';
            }
        }
    }
    return bool(out);
}

std::optional<PlayableScene> readPlayable(std::istream &in)
{
    in.imbue(std::locale::classic());
    PlayableScene scene;
    auto &world = scene.simulation;
    std::string signature;
    int version;
    std::size_t bodies,terrains,joints,meshes,lights;
    if (!(in >> std::setw(32) >> signature >> version) || signature != "WORLDFORGE_PLAY" || version < 1 || version > 20
        || !(in >> bodies >> terrains >> joints >> meshes >> lights >> scene.player)
        || bodies > 257 || terrains > 256 || joints > 256 || meshes > 4096 || lights > 256 || scene.player >= bodies
        || !readValue(in,world.gravity,0,50) || !(in >> scene.sky)
        || !readValue(in,scene.fogDensity,0,1) || !readValue(in,scene.fogFalloff,0,2)
        || !readValue(in,scene.fogHeight,-1000,1000)
        || !readVector(in,scene.horizon,0,1) || !readVector(in,scene.zenith,0,1) || !readVector(in,scene.fog,0,1))
        return {};
    if (version >= 3 && (!(in >> scene.shadows) || !readValue(in,scene.localFogRadius,0,500)
        || !readValue(in,scene.localFogDensity,0,1) || !readVector(in,scene.localFogCenter,-100000,100000)))
        return {};
    if (version >= 14) {
        int transform = 0;
        if (!readValue(in,scene.exposure,color::minimumExposure,color::maximumExposure) || !(in >> transform)
            || transform < 0 || transform > 1 || !(in >> scene.autoExposure)
            || !readValue(in,scene.exposureCompensation,-color::maximumCompensation,color::maximumCompensation)
            || !(in >> scene.temporalAntiAliasing) || !(in >> scene.ambientOcclusion))
            return {};
        scene.viewTransform = color::ViewTransform(transform);
    }
    if (version >= 15 && !(in >> scene.physicalSky))
        return {};
    if (version >= 16 && !(in >> scene.bloom))
        return {};
    if (version >= 18 && (!(in >> scene.depthOfField) || !readValue(in,scene.focusDistance,0.1f,10000)
                          || !readValue(in,scene.fNumber,0.5f,64) || !(in >> scene.motionBlur) || !readValue(in,scene.shutter,0,1)))
        return {};
    if (version >= 19 && !(in >> scene.reflections))
        return {};
    if (version >= 20 && !(in >> scene.contactShadows))
        return {};
    world.bodies.resize(bodies);
    for (auto &body : world.bodies) {
        if (!readVector(in,body.position) || !readVector(in,body.halfSize,0.001f,5000000)
            || !readVector(in,body.velocity,-1000,1000) || !readVector(in,body.rotation,-1,1)
            || !readVector(in,body.angularVelocity,-1000,1000) || !readValue(in,body.bounce,0,1)
            || !(in >> body.dynamic >> body.lockRotation) || !readValue(in,body.mass,0,100000)
            || !readValue(in,body.friction,0,1))
            return {};
        if (version >= 2) {
            int shape;
            if (!(in >> shape) || shape < 0 || shape > 2)
                return {};
            body.shape = BoxBody::Shape(shape);
        }
        const auto &q = body.rotation;
        const float lengthSquared = q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3];
        if (lengthSquared < 0.98f || lengthSquared > 1.02f)
            return {};
    }
    if (!world.bodies[scene.player].dynamic || !world.bodies[scene.player].lockRotation)
        return {};
    world.terrains.resize(terrains);
    scene.foliage.resize(terrains);
    for (std::size_t i = 0; i < terrains; ++i) {
        auto &ground = world.terrains[i];
        auto &foliage = scene.foliage[i];
        if (!readVector(in,ground.position) || !readValue(in,ground.field.size,4,1024)
            || !(in >> foliage.density >> foliage.seed) || foliage.density < 0 || foliage.density > 48
            || (version >= 3 && !readValue(in,foliage.wind,0,0.5))
            || !readVector(in,ground.field.samples,-256,256))
            return {};
        if (version >= 2)
            for (auto &layer : ground.field.layers) {
                int value;
                if (!(in >> value) || value < 0 || value > 3)
                    return {};
                layer = std::uint8_t(value);
            }
    }
    world.joints.resize(joints);
    for (auto &joint : world.joints) {
        int type;
        if (!(in >> type >> joint.first >> joint.second) || type < 0 || type > 2
            || joint.first >= bodies || joint.second >= bodies || joint.first == joint.second
            || !readVector(in,joint.anchor) || !readVector(in,joint.axis,-1,1))
            return {};
        joint.type = WorldSimulation::Joint::Type(type);
    }
    scene.lights.resize(lights);
    for (auto &light : scene.lights)
        if (!readVector(in,light.vector) || !readValue(in,light.intensity,0,4) || !(in >> light.directional)
            || (version >= 17 && (!readVector(in,light.spotDirection,-1,1) || !readValue(in,light.spotAngle,0,3.1416f)
                                  || !readValue(in,light.spotBlend,0,1))))
            return {};
    scene.meshes.resize(meshes);
    std::size_t remaining = 1500000;
    std::size_t remainingClipSamples = MeshClip::maxSamples*3;
    for (auto &mesh : scene.meshes) {
        std::size_t count;
        if (!(in >> mesh.body >> count) || mesh.body < -1 || mesh.body >= int(bodies) || count > remaining || count%3)
            return {};
        remaining -= count;
        std::size_t lodCount = 0;
        if (version >= 4) {
            if (!(in >> lodCount) || lodCount > remaining || lodCount%3 || (lodCount && lodCount >= count)
                || !readValue(in,mesh.lodDistance,0,1000000) || (lodCount && mesh.lodDistance <= 0)
                || !readVector(in,mesh.lodOrigin))
                return {};
            remaining -= lodCount;
        }
        if (version >= 5) {
            std::size_t textureBytes;
            if (!(in >> textureBytes) || textureBytes > MeshMaterial::maxTextureBytes
                || (textureBytes && !readBase64(in,textureBytes,mesh.baseColorTexture)))
                return {};
        }
        if (version >= 7) {
            if (!(in >> mesh.advancedSurface))
                return {};
            if (mesh.advancedSurface) {
                if (!readValue(in,mesh.metallic,0,1) || !readValue(in,mesh.roughness,0,1)
                    || !readVector(in,mesh.emissive,0,64))
                    return {};
                for (auto &map : mesh.surfaceMaps) {
                    std::size_t bytes;
                    if (!(in >> bytes) || bytes > MeshMaterial::maxTextureBytes
                        || (bytes && !readBase64(in,bytes,map)))
                        return {};
                }
            }
        }
        if (version >= 8) {
            bool animated;
            if (!(in >> animated))
                return {};
            if (animated) {
                auto &clip = mesh.clip.emplace();
                std::size_t frames;
                if (lodCount || !count || !readText(in,clip.name) || clip.name.empty() || clip.name.size() > 128
                    || !readValue(in,clip.framesPerSecond,1,120) || !(in >> frames) || frames < 2 || frames > MeshClip::maxFrames
                    || count > std::min(remainingClipSamples,MeshClip::maxSamples)/frames)
                    return {};
                clip.vertexCount = count;
                clip.samples.resize(count*frames);
                remainingClipSamples -= clip.samples.size();
                for (auto &vertex : clip.samples)
                    if (!readVector(in,vertex.position,-1'000'000,1'000'000) || !readVector(in,vertex.normal,-1.01f,1.01f))
                        return {};
            }
        }
        mesh.vertices.resize(count);
        mesh.lodVertices.resize(lodCount);
        for (auto *vertices : {&mesh.vertices,&mesh.lodVertices})
            for (auto &vertex : *vertices)
                if (!readVector(in,vertex.position) || !readVector(in,vertex.normal,-1.01f,1.01f)
                    || !readVector(in,vertex.color,0,1) || (version >= 5 && !readVector(in,vertex.uv,-1000,1000)))
                    return {};
    }
    if (version >= 5) {
        auto &play = scene.gameplay;
        std::size_t routes, vehicles, missions;
        if (!(in >> routes >> vehicles >> missions) || routes > 64 || vehicles > 128 || missions > 32)
            return {};
        play.routes.resize(routes);
        for (auto &route : play.routes) {
            std::size_t points;
            if (!(in >> points) || points < 2 || points > 256 || !readValue(in,route.speed,0.1f,60))
                return {};
            route.points.resize(points);
            for (auto &p : route.points)
                if (!readVector(in,p))
                    return {};
        }
        play.vehicles.resize(vehicles);
        for (auto &vehicle : play.vehicles)
            if (!(in >> vehicle.body >> vehicle.route) || vehicle.body >= bodies || vehicle.route >= routes
                || vehicle.body == scene.player || !world.bodies[vehicle.body].dynamic)
                return {};
        play.missions.resize(missions);
        for (auto &mission : play.missions)
            if (!readText(in,mission.name) || !readText(in,mission.goal) || !readVector(in,mission.start)
                || !readVector(in,mission.waypoint) || !readVector(in,mission.target) || !(in >> mission.hasWaypoint)
                || !readValue(in,mission.startRadius,0.1f,100) || !readValue(in,mission.waypointRadius,0.1f,100)
                || !readValue(in,mission.targetRadius,0.1f,100)
                || (version >= 6 && !readText(in,mission.completionText)))
                return {};
    }
    if (version >= 6) {
        auto &play = scene.gameplay;
        std::size_t count = 0;
        if (!(in >> count) || count > 32)
            return {};
        play.beacons.resize(count);
        for (auto &beacon : play.beacons)
            if (!(in >> beacon.mission) || beacon.mission >= play.missions.size()
                || !readValue(in,beacon.seconds,0.1f,15) || !readVector(in,beacon.position)
                || !readVector(in,beacon.color,0,1))
                return {};
        std::size_t shots = 0, patrols = 0;
        if (!(in >> play.showMissionHud >> shots >> patrols) || shots > 32 || patrols > 32)
            return {};
        play.shots.resize(shots);
        for (auto &shot : play.shots)
            if (!(in >> shot.mission) || shot.mission >= play.missions.size()
                || !readValue(in,shot.seconds,0.1f,15)
                || !readVector(in,shot.first) || !readVector(in,shot.last) || !readVector(in,shot.target))
                return {};
        play.patrols.resize(patrols);
        for (auto &patrol : play.patrols)
            if (!readVector(in,patrol.start) || !readVector(in,patrol.destination)
                || !readValue(in,patrol.speed,0.1f,10) || !readValue(in,patrol.wait,0,15)
                || patrol.start == patrol.destination)
                return {};
    }
    if (version >= 9) {
        auto &play = scene.gameplay;
        std::size_t count, effects;
        if (!(in >> count >> effects) || count > 128 || effects > 128)
            return {};
        play.behaviors.resize(count);
        for (auto &rule : play.behaviors) {
            int event, action, mission, required;
            if (!readText(in,rule.label) || !(in >> event >> action >> mission >> required >> rule.target)
                || event < 0 || event > 2 || action < 0 || action > (version == 9 ? 1 : 2) || mission < -1 || required < -1
                || !readValue(in,rule.amount,0.1f,15) || !readVector(in,rule.position))
                return {};
            if (version >= 11 && !(in >> rule.followLaunchTarget))
                return {};
            rule.event = BehaviorRule::Event(event);
            rule.action = BehaviorRule::Action(action);
            if (mission >= 0)
                rule.mission = std::size_t(mission);
            if (required >= 0)
                rule.requiredMission = std::size_t(required);
            if (!rule.valid(play.missions.size(),bodies,effects)
                || !rule.validBody(world))
                return {};
        }
        play.behaviorBeacons.resize(effects);
        for (auto &beacon : play.behaviorBeacons)
            if (!readVector(in,beacon.position) || !readVector(in,beacon.color,0,1))
                return {};
    }
    if (version >= 12) {
        std::size_t count;
        if (!(in >> count) || count > 256)
            return {};
        while (count--) {
            NativeBodyBinding binding;
            if (!readText(in,binding.id) || binding.id.empty() || binding.id.size() > 128
                || !(in >> binding.index) || binding.index >= bodies
                || std::ranges::find(scene.nativeBodies,binding.id,&NativeBodyBinding::id) != scene.nativeBodies.end())
                return {};
            scene.nativeBodies.push_back(std::move(binding));
        }
    }
    if (version >= 13) {
        std::size_t count;
        if (!(in >> count) || count > 256)
            return {};
        while (count--) {
            NativeInstance instance;
            std::size_t size;
            if (!readText(in,instance.owner) || instance.owner.empty() || instance.owner.size() > 128
                || !readText(in,instance.type) || instance.type.empty() || instance.type.size() > 64
                || !(in >> size) || size > 32
                || std::ranges::any_of(scene.nativeInstances,[&](const auto &prior) {
                    return prior.owner == instance.owner && prior.type == instance.type;
                }))
                return {};
            while (size--) {
                std::string id;
                NativePropertyValue value;
                if (!readText(in,id) || id.empty() || id.size() > 64 || !(in >> value.kind >> value.number)
                    || !readText(in,value.reference) || !value.valid()
                    || !instance.properties.emplace(std::move(id),std::move(value)).second)
                    return {};
            }
            scene.nativeInstances.push_back(std::move(instance));
        }
    }
    in >> std::ws;
    if (!in.eof())
        return {};
    return scene;
}

void drivePlayer(BoxBody &body,Point3 spawn,float yaw,int forward,int sideways,bool jump,int &jumpBuffer)
{
    const float speed = 4.0f/std::max(1.0f,std::hypot(float(forward),float(sideways)));
    body.velocity[0] = speed*(-std::cos(yaw)*forward-std::sin(yaw)*sideways);
    body.velocity[1] = speed*(-std::sin(yaw)*forward+std::cos(yaw)*sideways);
    if (jump)
        jumpBuffer = 16;
    if (jumpBuffer > 0 && body.grounded) {
        body.velocity[2] = 5;
        jumpBuffer = 0;
    } else if (jumpBuffer > 0)
        --jumpBuffer;
    if (body.position[2] < -256) {
        body.position = spawn;
        body.velocity = {};
        jumpBuffer = 0;
    }
}

}
