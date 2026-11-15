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

bool readText(std::istream &in,std::string &text)
{
    return bool(in >> std::quoted(text)) && text.size() <= 160;
}
}

std::vector<PlayVertex> makePlayVertices(const Mesh &mesh,const Heightfield *terrain,float foliageWind,
                                         std::optional<std::uint32_t> material)
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
    out << "WORLDFORGE_PLAY 5\n" << world.bodies.size() << ' ' << world.terrains.size() << ' '
        << world.joints.size() << ' ' << scene.meshes.size() << ' ' << scene.lights.size() << ' '
        << scene.player << ' ' << world.gravity << '\n';
    out << scene.sky << ' ' << scene.fogDensity << ' ' << scene.fogFalloff << ' ' << scene.fogHeight << ' ';
    writeVector(out,scene.horizon);
    writeVector(out,scene.zenith);
    writeVector(out,scene.fog);
    out << scene.shadows << ' ' << scene.localFogRadius << ' ' << scene.localFogDensity << ' ';
    writeVector(out,scene.localFogCenter);
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
        out << light.intensity << ' ' << light.directional << '\n';
    }
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
        out << std::quoted(mission.name) << ' ' << std::quoted(mission.goal) << ' ';
        writeVector(out,mission.start);
        writeVector(out,mission.waypoint);
        writeVector(out,mission.target);
        out << mission.hasWaypoint << ' ' << mission.startRadius << ' ' << mission.waypointRadius << ' '
            << mission.targetRadius << '\n';
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
    if (!(in >> std::setw(32) >> signature >> version) || signature != "WORLDFORGE_PLAY" || version < 1 || version > 5
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
        if (!readVector(in,light.vector) || !readValue(in,light.intensity,0,4) || !(in >> light.directional))
            return {};
    scene.meshes.resize(meshes);
    std::size_t remaining = 1500000;
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
                || !readValue(in,mission.targetRadius,0.1f,100))
                return {};
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
