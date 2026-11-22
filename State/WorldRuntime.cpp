#include "State/WorldRuntime.h"
#include "State/NativeComponents.h"
#include "State/WorldScene.h"
#include "Engine/World/PlayableScene.h"

#include <QMatrix4x4>
#include <QJsonArray>
#include <QQuaternion>
#include <QDebug>
#include <QFileInfo>
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
void logNative(std::string_view message) noexcept
{
    qInfo().noquote() << "C++ gameplay ·" << QString::fromUtf8(message.data(),qsizetype(message.size()));
}
// The Player reads gameplay text up to 160 UTF-8 bytes. Shorten by bytes, ending on a character boundary.
std::string utf8Text(const QString &text,qsizetype maxBytes)
{
    auto bytes = text.toUtf8();
    if (bytes.size() > maxBytes) {
        auto end = maxBytes;
        while (end > 0 && (quint8(bytes[end]) & 0xc0) == 0x80)
            --end;
        bytes.truncate(end);
    }
    return bytes.toStdString();
}
constexpr qsizetype missionTextBytes = 120;
std::string missionName(const SceneObject &object)
{
    return utf8Text(object.details.value("mission").toString(),missionTextBytes);
}
}

bool WorldRuntime::start(const ProjectStore &store, bool play, bool enableNative)
{
    const auto *project = store.project();
    if (!project)
        return false;
    stopNative();
    simulation_ = {};
    clipTime_ = 0;
    bindings_.clear();
    nativeBodies_.clear();
    nativeInstances_.clear();
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
        nativeBodies_.push_back({object.id.toStdString(),simulation_.bodies.size()});
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
    buildGameplay(project->behaviors);
    behaviorResults_.assign(gameplay_.behaviors.size(),worldforge::BehaviorRule::Result::Waiting);
    const auto module = store.nativeGameplayPath();
    if (play && enableNative) {
        QString error;
        if (collectNativeInstances(store,nativeInstances_,error)) {
            if (QFileInfo::exists(module))
                stageNative(module,error);
            else if (!nativeInstances_.empty())
                error = QObject::tr("Build and activate the project's C++ gameplay module to run its native components.");
        }
        if (!error.isEmpty())
            qWarning().noquote() << error;
    }
    return true;
}

bool WorldRuntime::stageNative(const QString &path,QString &error)
{
    std::string detail;
    if (native_.stage(std::filesystem::path(path.toStdWString()),detail,nativeInstances_))
        return true;
    error = QString::fromStdString(detail);
    return false;
}

void WorldRuntime::stopNative()
{
    native_.stop(simulation_,nativeBodies_,player_,logNative);
}

void WorldRuntime::buildGameplay(const QMap<QString,ProjectBehavior> &behaviors)
{
    gameplay_ = {};
    gameplayErrors_.clear();
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
        const auto name = missionName(object);
        if (!object.visible || name.empty() || !role.startsWith("mission"))
            continue;
        auto &mission = missions[QString::fromStdString(name)];
        mission.name = name;
        const auto radius = float(std::clamp(object.details.value("radius").toDouble(0),0.0,50.0));
        if (role == "missionStart") {
            mission.start = point(object.position);
            mission.goal = utf8Text(object.details.value("goal").toString(),missionTextBytes);
            mission.completionText = utf8Text(object.details.value("completionText").toString(),150);
            if (object.details.contains("showMissionHud"))
                gameplay_.showMissionHud = object.details.value("showMissionHud").toBool(true);
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
    const auto visibleTarget = [this](const QString &id) -> const SceneObject * {
        const auto found = std::ranges::find(objects_,id,&SceneObject::id);
        return found != objects_.end() && found->visible ? &*found : nullptr;
    };
    for (const auto &object : objects_) {
        if (!object.visible)
            continue;
        const auto waypointId = object.details.value("patrolWaypoint").toString();
        if (const auto *waypoint = visibleTarget(waypointId);
            waypoint && waypoint->position != object.position && gameplay_.patrols.size() < 32) {
            gameplay_.patrols.push_back({point(object.position),point(waypoint->position),{},
                float(std::clamp(object.details.value("patrolSpeed").toDouble(2),0.1,10.0)),
                float(std::clamp(object.details.value("patrolWait").toDouble(1),0.0,15.0))});
        } else if (!waypointId.isEmpty()) {
            gameplayErrors_ << QObject::tr("%1 has a missing, hidden, or invalid patrol waypoint. Check the AI workspace.").arg(object.name);
        }
        if (object.details.value("gameplay").toString() != "missionStart")
            continue;
        const auto mission = std::ranges::find(gameplay_.missions,missionName(object),&worldforge::Mission::name);
        if (mission == gameplay_.missions.end())
            continue;
        const auto index = std::size_t(mission-gameplay_.missions.begin());
        const auto focusId = object.details.value("shotTarget").toString();
        if (const auto *focus = visibleTarget(focusId);
            focus && gameplay_.shots.size() < 32) {
            const auto origin = focus->position + QVector3D(0,0,1.2f);
            const auto offset = [&](const char *key,QVector3D fallback) {
                const auto values = object.details.value(QLatin1String(key)).toArray();
                return values.size() == 3
                    ? QVector3D(float(values.at(0).toDouble()),float(values.at(1).toDouble()),float(values.at(2).toDouble()))
                    : fallback;
            };
            const auto first = origin+offset("shotFirst",{3,-4,1.2f});
            const auto last = origin+offset("shotLast",{-2,-3,1.1f});
            const auto seconds = float(std::clamp(object.details.value("shotSeconds").toDouble(2),0.1,15.0));
            if (first.z() >= origin.z()+0.0999f && last.z() >= origin.z()+0.0999f)
                gameplay_.shots.push_back({index,point(first),point(last),point(origin),seconds});
            else
                gameplayErrors_ << QObject::tr("%1 has an invalid camera key. Check the Cinematic workspace.").arg(object.name);
        } else if (!focusId.isEmpty()) {
            gameplayErrors_ << QObject::tr("%1 has a missing or hidden camera target. Check the Cinematic workspace.").arg(object.name);
        }
        const auto targetId = object.details.value("completionBeacon").toString();
        const auto *target = visibleTarget(targetId);
        if (!target || gameplay_.beacons.size() >= 32) {
            if (!targetId.isEmpty())
                gameplayErrors_ << QObject::tr("%1 has a missing or hidden beacon target. Check the Programming workspace.").arg(object.name);
            continue;
        }
        worldforge::MissionBeacon beacon;
        beacon.mission = index;
        beacon.position = point(target->position + QVector3D(0,0,1.2f));
        beacon.seconds = float(std::clamp(object.details.value("beaconSeconds").toDouble(2),0.1,15.0));
        const auto color = object.details.value("beaconColor").toArray();
        if (color.size() == 3)
            for (int i = 0; i < 3; ++i)
                beacon.color[i] = float(std::clamp(color.at(i).toDouble(),0.0,1.0));
        gameplay_.beacons.push_back(beacon);
    }
    using Rule = worldforge::BehaviorRule;
    QHash<QString,const SceneObject *> byId;
    for (const auto &object : objects_)
        byId.insert(object.id,&object);
    const auto missionIndex = [&](const QString &id) -> std::optional<std::size_t> {
        const auto *marker = visibleTarget(id);
        if (!marker || marker->details.value("gameplay").toString() != "missionStart")
            return {};
        const auto name = missionName(*marker);
        if (std::ranges::count_if(objects_,[&](const SceneObject &object) {
            return object.visible && object.details.value("gameplay").toString() == "missionStart"
                && missionName(object) == name;
        }) != 1)
            return {};
        const auto item = std::ranges::find(gameplay_.missions,name,&worldforge::Mission::name);
        return item == gameplay_.missions.end() ? std::nullopt : std::optional(std::size_t(item-gameplay_.missions.begin()));
    };
    const auto addComponent = [&](const SceneObject &owner,const BehaviorComponent &component) {
        const auto asset = behaviors.constFind(component.assetId);
        if (asset == behaviors.cend() || gameplay_.behaviors.size() >= 128) {
            gameplayErrors_ << QObject::tr("%1 has a missing behavior or exceeds the level's behavior budget.").arg(owner.name);
            return;
        }
        const auto *target = component.targetId.isEmpty() ? &owner : visibleTarget(component.targetId);
        if (!target) {
            gameplayErrors_ << QObject::tr("%1 on %2 has a missing or hidden action target. Check Programming.").arg(asset->name,owner.name);
            return;
        }
        Rule rule;
        rule.label = utf8Text(asset->name.left(60)+" / "+owner.name.left(60),150);
        rule.event = asset->event;
        rule.action = asset->action;
        rule.mission = missionIndex(asset->mission);
        rule.requiredMission = missionIndex(asset->requiredMission);
        rule.position = point(owner.position);
        rule.amount = component.amount.value_or(asset->amount);
        if ((!asset->mission.isEmpty() && !rule.mission) || (!asset->requiredMission.isEmpty() && !rule.requiredMission)) {
            gameplayErrors_ << QObject::tr("%1 references a missing, hidden, or ambiguous mission. Check Programming.").arg(asset->name);
            return;
        }
        if (rule.action == Rule::Action::RaiseObject) {
            const auto body = bodyIndex(target->id);
            const bool childHasCollider = std::ranges::any_of(objects_,[&](const SceneObject &candidate) {
                if (!bindings_.contains(candidate.id))
                    return false;
                for (auto *parent = byId.value(candidate.parentId); parent; parent = byId.value(parent->parentId))
                    if (parent->id == target->id)
                        return true;
                return false;
            });
            if (!body || simulation_.bodies[*body].dynamic || !target->parentId.isEmpty()
                || childHasCollider
                || std::ranges::any_of(simulation_.joints,[&](const auto &joint) {
                    return joint.first == *body || joint.second == *body;
                })) {
                gameplayErrors_ << QObject::tr("%1 must be an unparented Static physics mesh without joints or child colliders for Raise Object.")
                    .arg(target->name);
                return;
            }
            rule.target = *body;
        } else if (rule.action == Rule::Action::LaunchUpward) {
            const auto body = bodyIndex(target->id);
            if (!body || !simulation_.bodies[*body].dynamic || !target->parentId.isEmpty()) {
                gameplayErrors_ << QObject::tr("%1 must be an unparented Dynamic physics mesh for Launch Upward.")
                    .arg(target->name);
                return;
            }
            rule.target = *body;
            rule.followLaunchTarget = target->id == owner.id;
        } else {
            rule.target = gameplay_.behaviorBeacons.size();
            worldforge::MissionBeacon beacon;
            beacon.position = point(target->position+QVector3D(0,0,1.2f));
            gameplay_.behaviorBeacons.push_back(beacon);
        }
        gameplay_.behaviors.push_back(std::move(rule));
    };
    for (const auto &object : objects_)
        if (object.visible)
            for (const auto &component : behaviorComponents(object))
                if (component.enabled)
                    addComponent(object,component);
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
    clipTime_ += worldforge::WorldSimulation::stepSeconds;
    const bool interact = keys_.remove(Qt::Key_E);
    gameplay_.update(simulation_,player_,interact);
    for (std::size_t i = 0; i < gameplay_.behaviors.size(); ++i) {
        const auto &rule = gameplay_.behaviors[i];
        if (behaviorResults_[i] == rule.result)
            continue;
        behaviorResults_[i] = rule.result;
        QString result = QObject::tr("Action executed");
        if (rule.result == worldforge::BehaviorRule::Result::ConditionFailed && rule.requiredMission)
            result = QObject::tr("Condition failed. Mission %1 is incomplete.")
                .arg(QString::fromStdString(gameplay_.missions[*rule.requiredMission].name));
        qInfo().noquote() << QObject::tr("Behavior %1 · %2").arg(QString::fromStdString(rule.label), result);
    }
    const bool playerControl = !gameplay_.activeShot();
    if (player_) {
        const bool jump = keys_.remove(Qt::Key_Space);
        worldforge::drivePlayer(simulation_.bodies[*player_],point(spawn_),yaw,
            playerControl ? int(keys_.contains(Qt::Key_W))-int(keys_.contains(Qt::Key_S)) : 0,
            playerControl ? int(keys_.contains(Qt::Key_D))-int(keys_.contains(Qt::Key_A)) : 0,
            playerControl && jump,jumpBuffer_);
        if (native_.loaded()) {
            std::uint32_t input = 0;
            if (playerControl) {
                if (keys_.contains(Qt::Key_W)) input |= WF_GAME_FORWARD;
                if (keys_.contains(Qt::Key_S)) input |= WF_GAME_BACK;
                if (keys_.contains(Qt::Key_A)) input |= WF_GAME_LEFT;
                if (keys_.contains(Qt::Key_D)) input |= WF_GAME_RIGHT;
                if (jump) input |= WF_GAME_JUMP;
                if (interact) input |= WF_GAME_INTERACT;
            }
            native_.tick(simulation_,nativeBodies_,player_,input,logNative);
        }
    }
    simulation_.step();
}

WorldRuntime::CollisionProbe WorldRuntime::probeCollision(const ProjectStore &store,const QString &objectId,
                                                          QVector3D approach)
{
    CollisionProbe probe;
    WorldRuntime runtime;
    approach.setZ(0);
    if (approach.lengthSquared() < 1e-6f || !runtime.start(store,true,false) || !runtime.player_)
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

bool WorldRuntime::exportScene(const ProjectStore &store,std::ostream &output,QString *error,bool nativeBindings)
{
    WorldRuntime runtime;
    if (!runtime.start(store,true,false))
        return false;
    if (!runtime.gameplayErrors().isEmpty()) {
        if (error)
            *error = runtime.gameplayErrors().join('\n');
        return false;
    }
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
    scene.exposure = atmosphere.exposure;
    scene.autoExposure = atmosphere.autoExposure;
    scene.temporalAntiAliasing = atmosphere.temporalAntiAliasing;
    scene.ambientOcclusion = atmosphere.ambientOcclusion;
    scene.physicalSky = atmosphere.physicalSky;
    scene.bloom = atmosphere.bloom;
    scene.exposureCompensation = atmosphere.exposureCompensation;
    scene.viewTransform = atmosphere.viewTransform;
    std::size_t vertices = 0, animationSamples = 0;
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
                lightUnits(object),directional});
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
        const bool moving = driverIndex && (runtime.simulation_.bodies[*driverIndex].dynamic
            || runtime.gameplay_.movesBody(*driverIndex));
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
        // Split surface draws so the Player can bind each material's maps and factors.
        std::vector<std::optional<std::uint32_t>> parts{std::nullopt};
        if (std::ranges::any_of(mesh.materials,[](const auto &slot) {
            return !slot.baseColorTexture.empty() || slot.hasAdvancedSurface();
        })) {
            parts.clear();
            for (std::uint32_t slot = 0; slot < mesh.materials.size(); ++slot)
                parts.push_back(slot);
        }
        for (const auto &part : parts) {
            std::vector<worldforge::VertexIndex> clipIndices;
            worldforge::PlayMesh draw{moving ? int(*driverIndex) : -1,
                worldforge::makePlayVertices(mesh,nullptr,0,part,asset->clip ? &clipIndices : nullptr)};
            if (draw.vertices.empty() || scene.meshes.size() >= 4096)
                continue;
            if (part) {
                const auto &slot = mesh.materials[*part];
                draw.baseColorTexture = slot.baseColorTexture;
                draw.advancedSurface = slot.hasAdvancedSurface();
                draw.metallic = slot.metallic;
                draw.roughness = slot.roughness;
                draw.emissive = slot.emissive;
                const auto maps = slot.previewTextures();
                for (std::size_t i = 0; i < draw.surfaceMaps.size(); ++i)
                    draw.surfaceMaps[i] = *maps[i+1];
            }
            if (asset->lodMesh) {
                draw.lodVertices = worldforge::makePlayVertices(*asset->lodMesh,nullptr,0,part);
                draw.lodDistance = asset->lodDistance;
                if (draw.lodVertices.size() >= draw.vertices.size())
                    draw.lodVertices.clear();
            }
            if (!draw.lodVertices.empty())
                draw.lodOrigin = point(transform.map(QVector3D()));
            const auto place = [&](auto &vertex) {
                vertex.position = point(transform.map(vector(vertex.position)));
                QVector3D normal;
                for (int row = 0; row < 3; ++row)
                    normal[row] = normals(row,0)*vertex.normal[0]+normals(row,1)*vertex.normal[1]+normals(row,2)*vertex.normal[2];
                vertex.normal = point(normal.normalized());
            };
            for (auto *level : {&draw.vertices,&draw.lodVertices})
                for (auto &vertex : *level)
                    place(vertex);
            if (asset->clip) {
                const auto &source = *asset->clip;
                const auto frames = source.frameCount();
                if (clipIndices.size() > worldforge::MeshClip::maxSamples/frames
                    || clipIndices.size()*frames > worldforge::MeshClip::maxSamples*3-animationSamples) {
                    if (error)
                        *error = QObject::tr("%1 exceeds the Player's animation sample budget. Reduce the mesh or shorten the clip.").arg(asset->name);
                    return false;
                }
                animationSamples += clipIndices.size()*frames;
                auto &clip = draw.clip.emplace();
                clip.name = source.name;
                clip.framesPerSecond = source.framesPerSecond;
                clip.vertexCount = clipIndices.size();
                clip.samples.reserve(clip.vertexCount*frames);
                for (std::size_t frame = 0; frame < frames; ++frame)
                    for (const auto vertex : clipIndices) {
                        auto &sample = clip.samples.emplace_back(source.samples[frame*source.vertexCount+vertex]);
                        place(sample);
                    }
            }
            scene.meshes.push_back(std::move(draw));
        }
    }
    scene.gameplay = runtime.gameplay_;
    if (nativeBindings) {
        scene.nativeBodies = std::move(runtime.nativeBodies_);
        QString problem;
        if (!collectNativeInstances(store,scene.nativeInstances,problem)) {
            if (error)
                *error = problem;
            return false;
        }
        if (!scene.nativeInstances.empty()) {
            worldforge::NativeGameplay candidate;
            std::string message;
            if (!candidate.stage(std::filesystem::path(store.nativeGameplayPath().toStdWString()),message,scene.nativeInstances)) {
                if (error)
                    *error = QString::fromStdString(message);
                return false;
            }
        }
    }
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
        if (!body || (!simulation_.bodies[*body].dynamic && !gameplay_.movesBody(*body)))
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
