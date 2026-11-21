#include "State/PlayableStreet.h"
#include "State/WorldRuntime.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace wf = worldforge;

namespace street {
namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("PlayableStreet", text);
}

QString id()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QJsonArray json(QVector3D value)
{
    return {value.x(), value.y(), value.z()};
}

QString revision(const SceneObject &object)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(forge::revision(object).toLatin1());
    hash.addData(QJsonDocument(object.details).toJson(QJsonDocument::Compact));
    hash.addData(QByteArray(1, object.visible ? '1' : '0'));
    return QString::fromLatin1(hash.result().toHex().left(16));
}

bool mentions(const QString &text, const QString &pattern)
{
    return QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption).match(text).hasMatch();
}

}

void addStarter(Project &project)
{
    QHash<QString, QString> assets;
    const auto asset = [&](const QString &name, std::array<float, 4> color) {
        auto mesh = wf::makeCube();
        mesh.materials.front().color = color;
        mesh.materials.front().name = name.toStdString();
        MeshAsset created{id(), name, {}, {}, false, QSharedPointer<wf::Mesh>::create(std::move(mesh)), {}};
        assets.insert(name, created.id);
        project.assets.insert(created.id, std::move(created));
    };
    asset(tr("Sidewalk"), {0.56f, 0.57f, 0.58f, 1});
    asset(tr("Asphalt"), {0.16f, 0.17f, 0.19f, 1});
    asset(tr("Brick Wall"), {0.62f, 0.36f, 0.27f, 1});
    asset(tr("Shop Roof"), {0.30f, 0.32f, 0.36f, 1});
    asset(tr("Counter"), {0.52f, 0.38f, 0.22f, 1});
    asset(tr("Red Car"), {0.78f, 0.16f, 0.14f, 1});
    asset(tr("Blue Car"), {0.16f, 0.34f, 0.74f, 1});
    if (auto barrier = wf::makePrimitive(wf::Primitive::Cube, {3.6f, 2, 1.3f}, 8, 4)) {
        barrier->materials.front().color = {0.86f, 0.84f, 0.80f, 1};
        MeshAsset created{id(), tr("Road Barrier"), {}, {}, false, QSharedPointer<wf::Mesh>::create(std::move(*barrier)), {}};
        assets.insert(created.name, created.id);
        project.assets.insert(created.id, std::move(created));
    }
    const auto add = [&](const QString &name, const QString &type, const QString &assetName, QVector3D position,
                         QVector3D scale, float yaw, QJsonObject details, bool visible = true) {
        SceneObject object;
        object.id = id();
        object.name = name;
        object.type = type;
        object.group = type == "Empty" ? QStringLiteral("Gameplay") : QStringLiteral("Street");
        object.position = position;
        object.scale = scale;
        object.rotation = {0, 0, yaw};
        object.visible = visible;
        object.details = std::move(details);
        object.meshAsset = assets.value(assetName);
        project.objects.append(object);
        return object.id;
    };
    const QJsonObject solid{{"physics", "Static"}};
    const auto variant = [&](const QString &side, QJsonObject details = {{"physics", "Static"}}) {
        details.insert("shopEntrance", side);
        return details;
    };
    add(tr("Street Ground"), "Static Mesh", tr("Sidewalk"), {0, 0, -0.25f}, {64, 64, 0.5f}, 0, solid);
    add(tr("Main Road"), "Static Mesh", tr("Asphalt"), {0, -8, 0.005f}, {64, 8, 0.01f}, 0, {});
    add(tr("Side Street"), "Static Mesh", tr("Asphalt"), {12, 14, 0.006f}, {8, 36, 0.01f}, 0, {});

    // Repair shop: x -4..4, y 1..7. The entrance faces Main Road, or the side street when moved.
    add(tr("Shop Back Wall"), "Static Mesh", tr("Brick Wall"), {0, 6.85f, 1.5f}, {8.3f, 0.3f, 3}, 0, solid);
    add(tr("Shop West Wall"), "Static Mesh", tr("Brick Wall"), {-4, 4, 1.5f}, {0.3f, 5.4f, 3}, 0, solid);
    add(tr("Shop Front Wall Left"), "Static Mesh", tr("Brick Wall"), {-2.475f, 1.15f, 1.5f}, {3.35f, 0.3f, 3}, 0,
        variant("front"));
    add(tr("Shop Front Wall Right"), "Static Mesh", tr("Brick Wall"), {2.475f, 1.15f, 1.5f}, {3.35f, 0.3f, 3}, 0,
        variant("front"));
    auto frontDoor = variant("front");
    frontDoor.insert("shopDoor", true);
    frontDoor.insert("doorOutward", QJsonArray{0, -1, 0});
    add(tr("Shop Front Door Lintel"), "Static Mesh", tr("Brick Wall"), {0, 1.15f, 2.6f}, {1.6f, 0.3f, 0.8f}, 0, frontDoor);
    add(tr("Shop East Wall"), "Static Mesh", tr("Brick Wall"), {4, 4, 1.5f}, {0.3f, 5.4f, 3}, 0, variant("front"));
    add(tr("Shop Front Wall"), "Static Mesh", tr("Brick Wall"), {0, 1.15f, 1.5f}, {8.3f, 0.3f, 3}, 0,
        variant("side"), false);
    add(tr("Shop East Wall South"), "Static Mesh", tr("Brick Wall"), {4, 2.25f, 1.5f}, {0.3f, 1.9f, 3}, 0,
        variant("side"), false);
    add(tr("Shop East Wall North"), "Static Mesh", tr("Brick Wall"), {4, 5.75f, 1.5f}, {0.3f, 1.9f, 3}, 0,
        variant("side"), false);
    auto sideDoor = variant("side");
    sideDoor.insert("shopDoor", true);
    sideDoor.insert("doorOutward", QJsonArray{1, 0, 0});
    add(tr("Shop Side Door Lintel"), "Static Mesh", tr("Brick Wall"), {4, 4, 2.6f}, {0.3f, 1.6f, 0.8f}, 0, sideDoor,
        false);
    add(tr("Shop Roof"), "Static Mesh", tr("Shop Roof"), {0, 4, 3.15f}, {8.6f, 6.3f, 0.3f}, 0, solid);
    add(tr("Repair Counter Desk"), "Static Mesh", tr("Counter"), {0, 6.1f, 0.5f}, {3, 0.8f, 1}, 0, solid);
    add(tr("Road Barrier"), "Static Mesh", tr("Road Barrier"), {-8, 3.2f, 0.65f}, {1, 1, 1}, 0, solid);

    add(tr("Player Start"), "Player Start", {}, {-18, -2.5f, 1}, {1, 1, 1}, 0, {});
    add(tr("Mission Start"), "Empty", {}, {-12, -2.5f, 0}, {1, 1, 1}, 0,
        {{"gameplay", "missionStart"}, {"mission", missionName}, {"goal", tr("take the parts to the repair shop")},
         {"radius", 2}});
    add(tr("Repair Shop Entrance"), "Empty", {}, {0, -0.6f, 0}, {1, 1, 1}, -90,
        {{"gameplay", "missionWaypoint"}, {"mission", missionName}, {"radius", 1.8}});
    add(tr("Repair Counter"), "Empty", {}, {0, 5.1f, 0}, {1, 1, 1}, 0,
        {{"gameplay", "missionTarget"}, {"mission", missionName}, {"radius", 1.6}});
    add(tr("Main Street Entrance"), "Empty", {}, {-2, -10.6f, 0}, {1, 1, 1}, 90, {});

    QJsonArray route;
    for (const QVector3D point : {QVector3D{-26, -10, 0.8f}, QVector3D{13.5f, -10, 0.8f}, QVector3D{13.5f, 26, 0.8f},
                                  QVector3D{10.5f, 26, 0.8f}, QVector3D{10.5f, -6, 0.8f}, QVector3D{-26, -6, 0.8f}})
        route.append(json(point));
    const auto routeId = add(tr("Main Street Traffic"), "Empty", {}, {-26, -10, 0}, {1, 1, 1}, 0,
        {{"gameplay", "trafficRoute"}, {"speed", 7}, {"points", route}});
    const QJsonObject car{{"physics", "Dynamic"}, {"trafficVehicle", routeId}, {"mass", 1400}, {"friction", 0.05}};
    add(tr("Traffic Car 1"), "Static Mesh", tr("Red Car"), {-20, -10, 0.75f}, {4, 1.8f, 1.4f}, 0, car);
    add(tr("Traffic Car 2"), "Static Mesh", tr("Blue Car"), {13.5f, 10, 0.75f}, {4, 1.8f, 1.4f}, 90, car);
}

QString Report::summary() const
{
    QStringList lines;
    lines << (missionComplete ? tr("✓ %1 completed after %2 s.").arg(missionName).arg(missionSeconds, 0, 'f', 1)
                              : tr("✗ %1 did not complete.").arg(missionName));
    lines << (enteredShop ? tr("✓ The player entered the repair shop and reached the counter.")
                          : tr("✗ The player could not reach the counter inside the repair shop."));
    lines << (trafficLooped ? tr("✓ Every traffic vehicle completed its route.")
                            : tr("✗ Traffic did not complete its route. Check for obstacles in the lanes."));
    if (trace.beaconActive) lines << (trace.beaconEnded ? tr("✓ The beacon activated and expired.") : tr("✗ The beacon did not expire."));
    if (trace.shotActive) lines << (trace.shotEnded ? tr("✓ The camera shot played and returned control.") : tr("✗ The camera shot did not end."));
    if (trace.patrolWaited) lines << (trace.patrolReturned ? tr("✓ The NPC waited and returned.") : tr("✗ The NPC did not return."));
    if (!gameplayVerified) lines << tr("✗ Some authored gameplay did not complete.");
    lines.append(gameplayErrors);
    return lines.join('\n');
}

Report verify(const ProjectStore &store, float maxSeconds)
{
    Report report;
    WorldRuntime runtime;
    if (!runtime.start(store, true) || !runtime.playerBody())
        return report;
    if (!runtime.gameplayErrors().isEmpty()) {
        report.gameplayErrors = runtime.gameplayErrors();
        report.gameplayVerified = false;
        return report;
    }
    const auto player = *runtime.playerBody();
    wf::Autopilot pilot;
    const int steps = int(maxSeconds / wf::WorldSimulation::stepSeconds);
    for (int step = 0; step < steps; ++step) {
        float yaw = 0;
        bool interact = false;
        const bool walk = pilot.drive(runtime.simulation(), player, runtime.gameplay(), yaw, interact);
        runtime.key(Qt::Key_W, walk);
        if (interact)
            runtime.key(Qt::Key_E, true);
        runtime.tick(yaw);
        report.seconds = float(step + 1) * wf::WorldSimulation::stepSeconds;
        const auto objective = runtime.objective();
        if (!objective.isEmpty() && (report.objectives.isEmpty() || report.objectives.back() != objective))
            report.objectives << objective;
        const auto &gameplay = runtime.gameplay();
        report.trace.observe(gameplay);
        report.gameplayVerified = report.trace.complete(gameplay);
        const auto &position = runtime.simulation().bodies[player].position;
        for (const auto &mission : gameplay.missions)
            if (std::hypot(position[0] - mission.target[0], position[1] - mission.target[1]) <= mission.targetRadius
                && mission.state != wf::Mission::State::Waiting)
                report.enteredShop = true;
        if (!report.missionComplete && gameplay.missionsComplete()) {
            report.missionComplete = true;
            report.missionSeconds = report.seconds;
        }
        report.trafficLooped = gameplay.trafficLooped();
        if (report.ok())
            break;
    }
    return report;
}

Plan plan(const ProjectStore &store, const QString &prompt)
{
    Plan result;
    result.prompt = prompt.simplified();
    const auto *project = store.project();
    if (!project) {
        result.error = tr("Open a project first.");
        return result;
    }
    const auto &text = result.prompt;
    result.moveEntrance = mentions(text, "\\bentrance\\b") && mentions(text, "\\b(shop|repair)\\b");
    result.entranceSide = mentions(text, "\\bside\\s+street\\b|\\bside\\b") ? "side" : "front";
    result.rerouteTraffic = mentions(text, "\\btraffic\\b") && mentions(text, "\\b(around|avoid|reroute|route)\\b");
    if (!result.moveEntrance && !result.rerouteTraffic) {
        result.error = tr("Forge AI can move the repair shop entrance with its mission marker, and route traffic "
                          "around obstacles. Describe one or both of those changes.");
        return result;
    }
    auto objects = project->objects;
    const auto edit = [&](const SceneObject &updated) {
        auto match = std::ranges::find(objects, updated.id, &SceneObject::id);
        const auto original = std::ranges::find(project->objects, updated.id, &SceneObject::id);
        if (match == objects.end() || original == project->objects.end() || *match == updated)
            return;
        *match = updated;
        result.revisions.insert(updated.id, revision(*original));
    };
    if (result.moveEntrance) {
        QList<SceneObject> parts;
        for (const auto &object : objects)
            if (object.details.contains("shopEntrance"))
                parts << object;
        const auto door = std::ranges::find_if(parts, [&](const SceneObject &object) {
            return object.details.value("shopDoor").toBool()
                && object.details.value("shopEntrance").toString() == result.entranceSide;
        });
        const auto marker = std::ranges::find_if(objects, [](const SceneObject &object) {
            return object.details.value("gameplay").toString() == "missionWaypoint"
                && object.details.value("mission").toString() == QLatin1String(missionName);
        });
        if (door == parts.end() || marker == objects.end()) {
            result.error = tr("This level has no repair shop with front and side entrances. Start from the Playable "
                              "Street template.");
            return result;
        }
        int changed = 0;
        for (auto part : parts) {
            const bool visible = part.details.value("shopEntrance").toString() == result.entranceSide;
            changed += part.visible != visible;
            part.visible = visible;
            edit(part);
        }
        const auto outward = door->details.value("doorOutward").toArray();
        const QVector3D direction(float(outward.at(0).toDouble()), float(outward.at(1).toDouble()), 0);
        auto moved = *marker;
        moved.position = QVector3D(door->position.x(), door->position.y(), 0) + direction * 1.75f;
        moved.rotation = {0, 0, float(qRadiansToDegrees(std::atan2(direction.y(), direction.x())))};
        edit(moved);
        const auto label = result.entranceSide == "side" ? tr("the side street") : tr("Main Road");
        result.commands << forge::Command{"move_entrance", door->id, revision(*door),
            {{"label", label}, {"parts", int(parts.size())}}};
        result.commands << forge::Command{"move_mission_marker", marker->id, revision(*marker),
            {{"label", marker->name}, {"position", json(moved.position)}}};
        if (changed)
            result.changes << tr("Open the repair shop entrance onto %1 and close the other doorway (%2 wall pieces).")
                .arg(label).arg(changed);
        else
            result.changes << tr("The repair shop entrance already faces %1.").arg(label);
        result.changes << tr("Move %1 to (%2, %3) outside the new doorway.").arg(marker->name)
            .arg(moved.position.x(), 0, 'f', 2).arg(moved.position.y(), 0, 'f', 2);
    }
    if (result.rerouteTraffic) {
        WorldRuntime obstacles;
        if (!obstacles.start(store, false)) {
            result.error = tr("Could not read the level's colliders.");
            return result;
        }
        bool found = false;
        for (const auto &object : project->objects) {
            if (object.details.value("gameplay").toString() != "trafficRoute" || !object.visible)
                continue;
            found = true;
            QList<wf::Point3> anchors;
            for (const auto &value : object.details.value("points").toArray()) {
                const auto p = value.toArray();
                anchors.push_back({float(p.at(0).toDouble()), float(p.at(1).toDouble()), float(p.at(2).toDouble())});
            }
            QJsonArray points;
            for (qsizetype i = 0; i < anchors.size(); ++i) {
                const auto &from = anchors[i];
                const auto &to = anchors[(i + 1) % anchors.size()];
                points.append(QJsonArray{from[0], from[1], from[2]});
                const auto path = wf::findPath(obstacles.simulation(), from, to, 1.4f, 0.1f, 1.6f);
                if (path.empty()) {
                    result.error = tr("Traffic on %1 cannot get from (%2, %3) to (%4, %5) around the obstacles.")
                        .arg(object.name).arg(from[0]).arg(from[1]).arg(to[0]).arg(to[1]);
                    return result;
                }
                for (std::size_t node = 0; node + 1 < path.size(); ++node)
                    points.append(QJsonArray{path[node][0], path[node][1], from[2]});
            }
            const int added = int(points.size() - anchors.size());
            auto rerouted = object;
            rerouted.details.insert("points", points);
            edit(rerouted);
            result.commands << forge::Command{"reroute_traffic", object.id, revision(object),
                {{"label", object.name}, {"added", added}}};
            result.changes << (added > 0 ? tr("Route %1 around obstacles with %2 detour points.").arg(object.name).arg(added)
                                         : tr("%1 already avoids every obstacle.").arg(object.name));
        }
        if (!found) {
            result.error = tr("This level has no traffic route to change.");
            return result;
        }
    }
    result.commands << forge::Command{"check_street", {}, {}, {{"label", tr("the street")}}};
    for (const auto &object : objects)
        if (result.revisions.contains(object.id))
            result.objects << object;
    return result;
}

bool apply(ProjectStore &store, const Plan &plan, QString &error)
{
    if (!plan.error.isEmpty() || plan.objects.isEmpty()) {
        error = plan.error.isEmpty() ? tr("The plan has no changes to apply.") : plan.error;
        return false;
    }
    for (const auto &object : plan.objects) {
        const auto *current = store.object(object.id);
        if (!current || revision(*current) != plan.revisions.value(object.id)) {
            error = tr("%1 changed while the preview was open. Plan the request again.").arg(object.name);
            return false;
        }
    }
    return store.updateObjects(plan.objects, tr("Forge AI: %1").arg(plan.prompt.left(60)), error);
}

}
