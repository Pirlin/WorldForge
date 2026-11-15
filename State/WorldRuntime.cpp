#include "State/WorldRuntime.h"
#include "State/WorldScene.h"
#include "Engine/World/PlayableScene.h"

#include <QMatrix4x4>
#include <QJsonArray>
#include <QQuaternion>
#include <algorithm>
#include <cmath>

namespace {
QVector3D vector(worldforge::Point3 p) { return {p[0],p[1],p[2]}; }
worldforge::Point3 point(QVector3D p) { return {p.x(),p.y(),p.z()}; }
worldforge::Point3 rgb(const QColor &color) { return {color.redF(),color.greenF(),color.blueF()}; }
QQuaternion rotationOf(const worldforge::BoxBody &body)
{
    const auto &q = body.rotation;
    return {q[3],q[0],q[1],q[2]};
}
}

bool WorldRuntime::start(const ProjectStore &store, bool play)
{
    const auto *project = store.project();
    if (!project)
        return false;
    simulation_ = {};
    bindings_.clear();
    player_.reset();
    clearInput();
    objects_ = project->objects;
    authoredMatrices_ = store.worldMatrices();
    objects_.removeIf([&store](const SceneObject &object) { return store.isLegacyPlaceholder(object); });
    for (auto &object : objects_) {
        const auto matrix = authoredMatrices_.value(object.id);
        object.position = matrix.map(QVector3D{});
        if (object.parentId.isEmpty() && object.parentOffset.isIdentity())
            continue;
        const auto x = matrix.mapVector({1,0,0});
        const auto y = matrix.mapVector({0,1,0});
        const auto z = matrix.mapVector({0,0,1});
        const auto right = x.normalized();
        const auto up = (y-right*QVector3D::dotProduct(y,right)).normalized();
        const auto forward = QVector3D::crossProduct(right,up);
        object.rotation = QQuaternion::fromAxes(right,up,forward).toEulerAngles();
        object.scale = {x.length(),y.length(),QVector3D::dotProduct(z,forward)};
    }
    assets_ = project->assets;
    spawn_ = {0,0,2};
    if (const auto *environment = store.environment())
        simulation_.gravity = float(std::clamp(environment->details.value("gravity").toDouble(9.81),0.0,50.0));
    for (const auto &object : objects_) {
        if (!object.visible)
            continue;
        if (object.type == "Terrain")
            simulation_.terrains.push_back({readHeightfield(object),point(object.position)});
        if (object.type == "Player Start")
            spawn_ = object.position;
        const auto body = object.details.value("physics").toString();
        const auto *asset = store.meshAsset(object.meshAsset);
        if ((body != "Static" && body != "Dynamic") || !asset)
            continue;
        if (simulation_.bodies.size() >= 256)
            return false;
        const auto &vertices = asset->evaluated().vertices;
        QMatrix4x4 transform;
        transform.scale(object.scale);
        auto low = transform.map(vector(vertices.front()));
        auto high = low;
        for (const auto &vertex : vertices) {
            const auto p = transform.map(vector(vertex));
            for (int axis = 0; axis < 3; ++axis) {
                low[axis] = std::min(low[axis],p[axis]);
                high[axis] = std::max(high[axis],p[axis]);
            }
        }
        const auto offset = (low+high)*0.5f;
        auto center = offset;
        auto half = point((high-low)*0.5f);
        const auto proxy = object.details.value("collisionProxy").toObject();
        const auto proxyCenter = proxy.value("center").toArray();
        const auto proxyHalf = proxy.value("half").toArray();
        if (proxyCenter.size() == 3 && proxyHalf.size() == 3) {
            for (int axis = 0; axis < 3; ++axis) {
                center[axis] = float(proxyCenter[axis].toDouble())*object.scale[axis];
                half[axis] = float(proxyHalf[axis].toDouble(0.025))*std::abs(object.scale[axis]);
            }
        }
        for (auto &axis : half)
            axis = std::max(axis,0.025f);
        bindings_.insert(object.id,{simulation_.bodies.size(),center});
        const auto q = QQuaternion::fromEulerAngles(object.rotation);
        worldforge::BoxBody collider{point(object.position+q.rotatedVector(center)),half,{},
            float(std::clamp(object.details.value("bounce").toDouble(),0.0,1.0)),body == "Dynamic"};
        collider.rotation = {q.x(),q.y(),q.z(),q.scalar()};
        collider.angularVelocity[2] = float(std::clamp(object.details.value("spin").toDouble(),-720.0,720.0)
                                            * 0.01745329252);
        collider.mass = float(std::clamp(object.details.value("mass").toDouble(),0.0,100000.0));
        collider.friction = float(std::clamp(object.details.value("friction").toDouble(0.55),0.0,1.0));
        collider.shape = worldforge::BoxBody::Shape(std::clamp(object.details.value("colliderShape").toInt(),0,2));
        simulation_.bodies.push_back(collider);
    }
    using Type = worldforge::WorldSimulation::Joint::Type;
    for (const auto &object : objects_) {
        const auto first = bodyIndex(object.id);
        const auto second = bodyIndex(object.details.value("jointTarget").toString());
        const auto type = object.details.value("jointType").toString();
        if (!first || !second || *first == *second || type.isEmpty())
            continue;
        worldforge::Point3 axis{};
        axis[std::clamp(object.details.value("jointAxis").toInt(2),0,2)] = 1;
        auto anchor = object.position;
        const auto offset = object.details.value("jointOffset").toArray();
        for (int i = 0; i < std::min(3,int(offset.size())); ++i)
            anchor[i] += float(std::clamp(offset.at(i).toDouble(),-10000.0,10000.0));
        simulation_.joints.push_back({type == "Hinge" ? Type::Hinge : type == "Distance" ? Type::Distance : Type::Fixed,
                                      *first,*second,point(anchor),axis});
    }
    if (play) {
        for (const auto &terrain : simulation_.terrains) {
            const auto h = terrain.field.height(spawn_.x()-terrain.position[0],spawn_.y()-terrain.position[1]);
            if (h)
                spawn_.setZ(std::max(spawn_.z(),*h+terrain.position[2]+0.9f));
        }
        player_ = simulation_.bodies.size();
        simulation_.bodies.push_back({point(spawn_),{0.30f,0.30f,0.85f},{},0,true});
        simulation_.bodies.back().lockRotation = true;
        simulation_.bodies.back().friction = 0.05f;
        simulation_.bodies.back().shape = worldforge::BoxBody::Shape::Capsule;
    }
    buildGameplay();
    return true;
}

void WorldRuntime::buildGameplay()
{
    gameplay_ = {};
    QHash<QString,std::size_t> routes;
    for (const auto &object : objects_) {
        if (!object.visible || object.details.value("gameplay").toString() != "trafficRoute")
            continue;
        worldforge::TrafficRoute route;
        route.speed = float(std::clamp(object.details.value("speed").toDouble(7),0.5,30.0));
        for (const auto &value : object.details.value("points").toArray()) {
            const auto p = value.toArray();
            if (p.size() == 3)
                route.points.push_back({float(p[0].toDouble()),float(p[1].toDouble()),float(p[2].toDouble())});
        }
        if (route.points.size() >= 2 && route.points.size() <= 256 && gameplay_.routes.size() < 64) {
            routes.insert(object.id,gameplay_.routes.size());
            gameplay_.routes.push_back(std::move(route));
        }
    }
    for (const auto &object : objects_) {
        const auto route = routes.constFind(object.details.value("trafficVehicle").toString());
        const auto body = bodyIndex(object.id);
        if (!object.visible || route == routes.cend() || !body || !simulation_.bodies[*body].dynamic
            || gameplay_.vehicles.size() >= 128)
            continue;
        auto &vehicle = simulation_.bodies[*body];
        vehicle.lockRotation = true;
        vehicle.friction = 0.05f;
        if (vehicle.mass <= 0)
            vehicle.mass = 1400;
        gameplay_.vehicles.push_back({*body,*route});
    }
    QMap<QString,worldforge::Mission> missions;
    for (const auto &object : objects_) {
        const auto role = object.details.value("gameplay").toString();
        const auto name = object.details.value("mission").toString().left(120);
        if (!object.visible || name.isEmpty() || !role.startsWith("mission"))
            continue;
        auto &mission = missions[name];
        mission.name = name.toStdString();
        const auto radius = float(std::clamp(object.details.value("radius").toDouble(0),0.0,50.0));
        if (role == "missionStart") {
            mission.start = point(object.position);
            mission.goal = object.details.value("goal").toString().left(120).toStdString();
            if (radius > 0)
                mission.startRadius = radius;
        } else if (role == "missionWaypoint") {
            mission.waypoint = point(object.position);
            mission.hasWaypoint = true;
            if (radius > 0)
                mission.waypointRadius = radius;
        } else if (role == "missionTarget") {
            mission.target = point(object.position);
            if (radius > 0)
                mission.targetRadius = radius;
        }
    }
    for (const auto &mission : missions)
        if (gameplay_.missions.size() < 32)
            gameplay_.missions.push_back(mission);
    gameplay_.start(simulation_);
}

void WorldRuntime::placePlayer(const QVector3D &position)
{
    if (player_)
        simulation_.bodies[*player_].position = point(position);
}

QString WorldRuntime::objective() const
{
    return QString::fromStdString(gameplay_.objective());
}

void WorldRuntime::tick(float yaw)
{
    gameplay_.update(simulation_,player_,keys_.remove(Qt::Key_E));
    if (player_)
        worldforge::drivePlayer(simulation_.bodies[*player_],point(spawn_),yaw,
            int(keys_.contains(Qt::Key_W))-int(keys_.contains(Qt::Key_S)),
            int(keys_.contains(Qt::Key_D))-int(keys_.contains(Qt::Key_A)),keys_.remove(Qt::Key_Space),jumpBuffer_);
    simulation_.step();
}

WorldRuntime::CollisionProbe WorldRuntime::probeCollision(const ProjectStore &store,const QString &objectId,
                                                          QVector3D approach)
{
    CollisionProbe probe;
    WorldRuntime runtime;
    approach.setZ(0);
    if (approach.lengthSquared() < 1e-6f || !runtime.start(store,true) || !runtime.player_)
        return probe;
    const auto index = runtime.bodyIndex(objectId);
    if (!index)
        return probe;
    approach.normalize();
    const auto target = runtime.simulation_.bodies[*index];
    const auto center = vector(target.position);
    const float radius = std::hypot(target.halfSize[0],target.halfSize[1],target.halfSize[2]);
    auto &player = runtime.simulation_.bodies[*runtime.player_];
    probe.start = center + approach*(radius + 1.5f);
    probe.start.setZ(center.z() - target.halfSize[2] + player.halfSize[2] + 0.02f);
    player.position = point(probe.start);
    player.velocity = {};
    runtime.simulation_.gravity = 0;
    runtime.key(Qt::Key_W,true);
    const float yaw = std::atan2(approach.y(),approach.x());
    for (int step = 0; step < 240; ++step)
        runtime.tick(yaw);
    probe.loaded = true;
    probe.stop = vector(runtime.simulation_.bodies[*runtime.player_].position);
    probe.blocked = QVector3D::dotProduct(probe.stop-center,approach) > 0.05f;
    return probe;
}

bool WorldRuntime::exportScene(const ProjectStore &store,std::ostream &output)
{
    WorldRuntime runtime;
    if (!runtime.start(store,true))
        return false;
    worldforge::PlayableScene scene;
    scene.player = *runtime.player_;
    const auto *environment = store.environment();
    auto atmosphere = readAtmosphere(environment ? environment->details : QJsonObject{});
    if (environment && !environment->visible) {
        atmosphere.sky = false;
        atmosphere.fogDensity = 0;
        atmosphere.localFogRadius = 0;
    }
    scene.sky = atmosphere.sky;
    scene.horizon = rgb(atmosphere.horizon);
    scene.zenith = rgb(atmosphere.zenith);
    scene.fog = rgb(atmosphere.fogColor);
    scene.fogDensity = atmosphere.fogDensity;
    scene.fogFalloff = atmosphere.fogFalloff;
    scene.fogHeight = atmosphere.fogHeight;
    scene.shadows = atmosphere.shadows;
    scene.localFogCenter = point(atmosphere.localFogCenter);
    scene.localFogRadius = atmosphere.localFogRadius;
    scene.localFogDensity = atmosphere.localFogDensity;
    std::size_t vertices = 0;
    bool hasSun = false;
    for (const auto &object : runtime.objects_) {
        if (!object.visible)
            continue;
        if (object.type == "Terrain") {
            scene.foliage.push_back({std::clamp(object.details.value("foliageDensity").toInt(),0,48),
                unsigned(std::max(0,object.details.value("foliageSeed").toInt(42))),
                float(std::clamp(object.details.value("foliageWind").toDouble(0.09),0.0,0.5))});
            continue;
        }
        const auto q = QQuaternion::fromEulerAngles(object.rotation);
        if (object.type == "Directional Light" || object.type == "Point Light") {
            const bool directional = object.type == "Directional Light";
            hasSun |= directional;
            const auto direction = object.id == "sun" && object.rotation.isNull() ? sunDirection(atmosphere) : q.rotatedVector({0,0,1});
            scene.lights.push_back({point(directional ? direction : object.position),
                float(std::clamp(object.details.value("lightIntensity").toDouble(1),0.0,4.0)),directional});
            continue;
        }
        const auto asset = runtime.assets_.constFind(object.meshAsset);
        if (asset == runtime.assets_.cend())
            continue;
        const auto index = runtime.bodyIndex(object.id);
        auto driverIndex = index;
        const SceneObject *driver = &object;
        while ((!driverIndex || !runtime.simulation_.bodies[*driverIndex].dynamic) && !driver->parentId.isEmpty()) {
            const auto parent = std::ranges::find(runtime.objects_,driver->parentId,&SceneObject::id);
            if (parent == runtime.objects_.end())
                break;
            driver = &*parent;
            driverIndex = runtime.bodyIndex(driver->id);
        }
        const bool moving = driverIndex && runtime.simulation_.bodies[*driverIndex].dynamic;
        const auto &mesh = asset->evaluated();
        for (const auto *level : {&mesh,asset->lodMesh.data()})
            if (level)
                for (const auto &face : level->faces)
                    vertices += 3*(face.corners.size()-2);
        if (vertices > 1500000 || scene.meshes.size() >= 4096)
            return false;
        QMatrix4x4 transform;
        if (moving) {
            const auto &body = runtime.simulation_.bodies[*driverIndex];
            QMatrix4x4 pose;
            pose.translate(vector(body.position));
            pose.rotate(rotationOf(body));
            transform = pose.inverted()*runtime.authoredMatrices_.value(object.id);
        } else
            transform = runtime.authoredMatrices_.value(object.id);
        const auto normals = transform.normalMatrix();
        // Textured materials become separate draws so the Player can bind each base-color map.
        std::vector<std::optional<std::uint32_t>> parts{std::nullopt};
        if (std::ranges::any_of(mesh.materials,[](const auto &slot) { return !slot.baseColorTexture.empty(); })) {
            parts.clear();
            for (std::uint32_t slot = 0; slot < mesh.materials.size(); ++slot)
                parts.push_back(slot);
        }
        for (const auto &part : parts) {
            worldforge::PlayMesh draw{moving ? int(*driverIndex) : -1,worldforge::makePlayVertices(mesh,nullptr,0,part)};
            if (draw.vertices.empty() || scene.meshes.size() >= 4096)
                continue;
            if (part)
                draw.baseColorTexture = mesh.materials[*part].baseColorTexture;
            if (asset->lodMesh) {
                draw.lodVertices = worldforge::makePlayVertices(*asset->lodMesh,nullptr,0,part);
                draw.lodDistance = asset->lodDistance;
                if (draw.lodVertices.size() >= draw.vertices.size())
                    draw.lodVertices.clear();
            }
            if (!draw.lodVertices.empty())
                draw.lodOrigin = point(transform.map(QVector3D()));
            for (auto *level : {&draw.vertices,&draw.lodVertices})
                for (auto &v : *level) {
                    v.position = point(transform.map(vector(v.position)));
                    QVector3D normal;
                    for (int row = 0; row < 3; ++row)
                        normal[row] = normals(row,0)*v.normal[0] + normals(row,1)*v.normal[1] + normals(row,2)*v.normal[2];
                    v.normal = point(normal.normalized());
                }
            scene.meshes.push_back(std::move(draw));
        }
    }
    scene.gameplay = runtime.gameplay_;
    scene.simulation = std::move(runtime.simulation_);
    if (!hasSun)
        scene.lights.insert(scene.lights.begin(),{point(sunDirection(atmosphere)),1,true});
    if (scene.foliage.size() > 256 || scene.lights.size() > 256 || scene.simulation.joints.size() > 256)
        return false;
    return worldforge::writePlayable(output,scene);
}

QHash<QString,QMatrix4x4> WorldRuntime::renderMatrices() const
{
    QHash<QString,QMatrix4x4> result;
    QHash<QString,QMatrix4x4> motion;
    QHash<QString,const SceneObject *> byId;
    for (const auto &object : objects_) {
        byId.insert(object.id,&object);
        const auto body = bodyIndex(object.id);
        if (!body || !simulation_.bodies[*body].dynamic)
            continue;
        const auto moved = position(object.id);
        const auto rotated = rotation(object.id);
        if (!moved || !rotated)
            continue;
        QMatrix4x4 before, after;
        before.translate(object.position);
        before.rotate(QQuaternion::fromEulerAngles(object.rotation));
        after.translate(*moved);
        after.rotate(QQuaternion::fromEulerAngles(*rotated));
        motion.insert(object.id,after*before.inverted());
    }
    for (const auto &object : objects_) {
        auto *driver = &object;
        while (driver && !motion.contains(driver->id))
            driver = byId.value(driver->parentId);
        result.insert(object.id,(driver ? motion.value(driver->id) : QMatrix4x4{})*authoredMatrices_.value(object.id));
    }
    return result;
}

std::optional<QVector3D> WorldRuntime::position(const QString &id) const
{
    const auto body = bindings_.constFind(id);
    if (body == bindings_.cend())
        return {};
    const auto &collider = simulation_.bodies[body->index];
    return vector(collider.position)-rotationOf(collider).rotatedVector(body->offset);
}

std::optional<std::size_t> WorldRuntime::bodyIndex(const QString &id) const
{
    const auto body = bindings_.constFind(id);
    return body == bindings_.cend() ? std::nullopt : std::optional(body->index);
}

std::optional<QVector3D> WorldRuntime::rotation(const QString &id) const
{
    const auto index = bodyIndex(id);
    return index ? std::optional(rotationOf(simulation_.bodies[*index]).toEulerAngles()) : std::nullopt;
}

std::optional<QVector3D> WorldRuntime::player() const
{
    return player_ ? std::optional(vector(simulation_.bodies[*player_].position)) : std::nullopt;
}

void WorldRuntime::key(int key, bool down)
{
    if (down)
        keys_.insert(key);
    else if (key != Qt::Key_Space)
        keys_.remove(key);
}
