#include "Engine/World/Gameplay.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace worldforge {
namespace {

float planar(Point3 a, Point3 b)
{
    return std::hypot(a[0] - b[0], a[1] - b[1]);
}

// A static collider seen from above, grown by the moving body's radius.
struct Footprint {
    float x, y, cosine, sine, halfX, halfY, radius;

    std::array<float, 2> local(float px, float py) const
    {
        const float dx = px - x, dy = py - y;
        return {dx * cosine + dy * sine, -dx * sine + dy * cosine};
    }
    bool contains(float px, float py, float margin = 0) const
    {
        const auto p = local(px, py);
        return std::abs(p[0]) < halfX - margin && std::abs(p[1]) < halfY - margin;
    }
    // Liang-Barsky clip of the segment against the rectangle shrunk by `shrink`.
    bool blocks(Point3 a, Point3 b, float shrink = 0.01f) const
    {
        const auto p = local(a[0], a[1]), q = local(b[0], b[1]);
        const float d[2]{q[0] - p[0], q[1] - p[1]};
        const float limit[2]{halfX - shrink, halfY - shrink};
        float enter = 0, leave = 1;
        for (int axis = 0; axis < 2; ++axis) {
            if (std::abs(d[axis]) < 1e-7f) {
                if (std::abs(p[axis]) >= limit[axis])
                    return false;
                continue;
            }
            float t0 = (-limit[axis] - p[axis]) / d[axis];
            float t1 = (limit[axis] - p[axis]) / d[axis];
            if (t0 > t1)
                std::swap(t0, t1);
            enter = std::max(enter, t0);
            leave = std::min(leave, t1);
            if (enter >= leave)
                return false;
        }
        return true;
    }
};

std::vector<Footprint> footprints(const WorldSimulation &world, float radius, float low, float high,
                                  std::optional<std::size_t> ignore)
{
    std::vector<Footprint> result;
    for (std::size_t i = 0; i < world.bodies.size(); ++i) {
        const auto &body = world.bodies[i];
        if (body.dynamic || (ignore && *ignore == i))
            continue;
        const auto &q = body.rotation;
        const float yaw = std::atan2(2 * (q[3] * q[2] + q[0] * q[1]), 1 - 2 * (q[1] * q[1] + q[2] * q[2]));
        const float bottom = body.position[2] - body.halfSize[2], top = body.position[2] + body.halfSize[2];
        // Ground slabs and kerbs low enough to step over do not block, nor do roofs above the body.
        if (top <= low + 0.3f || bottom >= high)
            continue;
        result.push_back({body.position[0], body.position[1], std::cos(yaw), std::sin(yaw),
                          body.halfSize[0] + radius, body.halfSize[1] + radius, radius});
    }
    return result;
}

}

std::vector<Point3> findPath(const WorldSimulation &world, Point3 from, Point3 to, float radius, float low,
                             float high, std::optional<std::size_t> ignore)
{
    const auto obstacles = footprints(world, radius, low, high, ignore);
    std::vector<Point3> nodes{from, to};
    for (const auto &box : obstacles)
        for (const float sx : {-1.0f, 1.0f})
            for (const float sy : {-1.0f, 1.0f}) {
                const float lx = sx * (box.halfX + 0.08f), ly = sy * (box.halfY + 0.08f);
                const Point3 corner{box.x + lx * box.cosine - ly * box.sine, box.y + lx * box.sine + ly * box.cosine,
                                    from[2]};
                if (std::ranges::none_of(obstacles, [&](const Footprint &other) {
                        return other.contains(corner[0], corner[1]);
                    }))
                    nodes.push_back(corner);
            }
    // When an end point is within the clearance of an obstacle (a body standing close to a wall, or a marker
    // placed against a counter), test against the collider itself so the point stays reachable but the
    // segment still cannot pass through the obstacle.
    const auto clear = [&](const Point3 &a, const Point3 &b) {
        return std::ranges::none_of(obstacles, [&](const Footprint &box) {
            const bool near = box.contains(a[0], a[1]) || box.contains(b[0], b[1]);
            return box.blocks(a, b, near ? box.radius + 0.01f : 0.01f);
        });
    };
    const std::size_t count = nodes.size();
    std::vector<float> cost(count, std::numeric_limits<float>::infinity());
    std::vector<std::size_t> previous(count, count);
    std::vector<bool> done(count, false);
    cost[0] = 0;
    for (std::size_t iteration = 0; iteration < count; ++iteration) {
        std::size_t current = count;
        for (std::size_t i = 0; i < count; ++i)
            if (!done[i] && std::isfinite(cost[i]) && (current == count || cost[i] < cost[current]))
                current = i;
        if (current == count || current == 1)
            break;
        done[current] = true;
        for (std::size_t next = 0; next < count; ++next) {
            if (done[next] || next == current)
                continue;
            const float candidate = cost[current] + planar(nodes[current], nodes[next]);
            if (candidate < cost[next] && clear(nodes[current], nodes[next])) {
                cost[next] = candidate;
                previous[next] = current;
            }
        }
    }
    if (!std::isfinite(cost[1]))
        return {};
    std::vector<Point3> path;
    for (std::size_t node = 1; node != 0; node = previous[node])
        path.push_back(nodes[node]);
    std::ranges::reverse(path);
    return path;
}

bool BehaviorRule::valid(std::size_t missions,std::size_t bodies,std::size_t beacons) const
{
    return !label.empty() && label.size() <= 150 && int(event) >= 0 && int(event) <= 2
        && int(action) >= 0 && action <= Action::LaunchUpward && (!mission || *mission < missions)
        && (event == Event::MissionCompleted) == mission.has_value() && (!requiredMission || *requiredMission < missions)
        && target < (action == Action::PulseBeacon ? beacons : bodies)
        && amount >= 0.1f && amount <= 15 && std::ranges::all_of(position,[](float v) {
            return std::isfinite(v) && std::abs(v) <= 5'000'000;
        });
}

bool BehaviorRule::validBody(const WorldSimulation &world) const
{
    return action == Action::PulseBeacon
        || (target < world.bodies.size() && world.bodies[target].dynamic == (action == Action::LaunchUpward));
}

bool Gameplay::movesBody(std::size_t body) const
{
    return std::ranges::any_of(behaviors,[body](const BehaviorRule &rule) {
        return rule.action == BehaviorRule::Action::RaiseObject && rule.target == body;
    });
}

void Gameplay::start(WorldSimulation &world)
{
    for (auto &vehicle : vehicles) {
        if (vehicle.route >= routes.size() || vehicle.body >= world.bodies.size()
            || routes[vehicle.route].points.size() < 2)
            continue;
        const auto &points = routes[vehicle.route].points;
        const auto &position = world.bodies[vehicle.body].position;
        std::size_t nearest = 0;
        for (std::size_t i = 1; i < points.size(); ++i)
            if (planar(points[i], position) < planar(points[nearest], position))
                nearest = i;
        vehicle.next = (nearest + 1) % points.size();
        vehicle.reached = 0;
        vehicle.laps = 0;
        vehicle.stalled = 0;
        vehicle.lastDistance = planar(points[vehicle.next], position);
    }
    for (auto &mission : missions)
        mission.state = Mission::State::Waiting;
    for (auto &rule : behaviors)
        rule.result = BehaviorRule::Result::Waiting;
    for (auto &beacon : behaviorBeacons)
        beacon.remaining = 0;
    for (auto &beacon : beacons) {
        beacon.triggered = false;
        beacon.remaining = 0;
    }
    for (auto &shot : shots) {
        shot.triggered = false;
        shot.remaining = 0;
    }
    for (auto &patrol : patrols) {
        patrol.position = patrol.start;
        patrol.remaining = 0;
        patrol.state = NpcPatrol::State::Walking;
        patrol.resumed = false;
    }
}

void Gameplay::update(WorldSimulation &world, std::optional<std::size_t> player, bool interact)
{
    for (auto &beacon : behaviorBeacons)
        beacon.remaining = std::max(0.0f,beacon.remaining-WorldSimulation::stepSeconds);
    for (auto &beacon : beacons)
        beacon.remaining = std::max(0.0f,beacon.remaining-WorldSimulation::stepSeconds);
    for (auto &shot : shots)
        shot.remaining = std::max(0.0f,shot.remaining-WorldSimulation::stepSeconds);
    for (auto &patrol : patrols) {
        using State = NpcPatrol::State;
        if (patrol.state == State::Waiting) {
            patrol.remaining -= WorldSimulation::stepSeconds;
            if (patrol.remaining <= 0) {
                patrol.state = State::Returning;
                patrol.resumed = true;
            }
            continue;
        }
        const auto &target = patrol.state == State::Walking ? patrol.destination : patrol.start;
        const auto delta = std::hypot(target[0]-patrol.position[0],target[1]-patrol.position[1],target[2]-patrol.position[2]);
        const auto distance = patrol.speed*WorldSimulation::stepSeconds;
        if (delta <= distance) {
            patrol.position = target;
            patrol.remaining = patrol.wait;
            patrol.state = patrol.state == State::Walking ? State::Waiting : State::Walking;
        } else {
            for (std::size_t axis = 0; axis < target.size(); ++axis)
                patrol.position[axis] = std::lerp(patrol.position[axis],target[axis],distance/delta);
        }
    }
    constexpr float dt = WorldSimulation::stepSeconds;
    for (auto &vehicle : vehicles) {
        if (vehicle.route >= routes.size() || vehicle.body >= world.bodies.size())
            continue;
        const auto &route = routes[vehicle.route];
        if (route.points.size() < 2)
            continue;
        auto &body = world.bodies[vehicle.body];
        float distance = planar(route.points[vehicle.next], body.position);
        if (distance < 1.2f) {
            ++vehicle.reached;
            vehicle.laps = int(vehicle.reached / route.points.size());
            vehicle.next = (vehicle.next + 1) % route.points.size();
            distance = planar(route.points[vehicle.next], body.position);
            vehicle.lastDistance = distance;
        }
        const auto &target = route.points[vehicle.next];
        const float dx = (target[0] - body.position[0]) / std::max(distance, 0.001f);
        const float dy = (target[1] - body.position[1]) / std::max(distance, 0.001f);
        const float speed = std::min(route.speed, distance * 4 + 1);
        body.velocity[0] = dx * speed;
        body.velocity[1] = dy * speed;
        const float yaw = std::atan2(dy, dx);
        const std::array<float, 4> heading{0, 0, std::sin(yaw * 0.5f), std::cos(yaw * 0.5f)};
        const float alignment = std::abs(heading[2] * body.rotation[2] + heading[3] * body.rotation[3]);
        if (alignment < 0.9995f)
            body.rotation = heading;
        vehicle.stalled = distance > vehicle.lastDistance - route.speed * dt * 0.2f ? vehicle.stalled + dt : 0;
        vehicle.lastDistance = std::min(vehicle.lastDistance, distance);
    }
    if (!player || *player >= world.bodies.size())
        return;
    const auto &position = world.bodies[*player].position;
    for (auto &mission : missions) {
        using State = Mission::State;
        if (mission.state == State::Waiting && planar(position, mission.start) <= mission.startRadius)
            mission.state = mission.hasWaypoint ? State::Travel : State::Interact;
        if (mission.state == State::Travel && (planar(position, mission.waypoint) <= mission.waypointRadius
                                               || planar(position, mission.target) <= mission.targetRadius))
            mission.state = State::Interact;
        if (mission.state == State::Interact && interact && planar(position, mission.target) <= mission.targetRadius)
            mission.state = State::Complete;
    }
    for (auto &beacon : beacons) {
        if (!beacon.triggered && beacon.mission < missions.size()
            && missions[beacon.mission].state == Mission::State::Complete) {
            beacon.triggered = true;
            beacon.remaining = beacon.seconds;
        }
    }
    for (auto &shot : shots) {
        if (!shot.triggered && shot.mission < missions.size() && missions[shot.mission].state == Mission::State::Complete) {
            shot.triggered = true;
            shot.remaining = shot.seconds;
        }
    }
    const auto complete = [&](std::size_t mission) {
        return mission < missions.size() && missions[mission].state == Mission::State::Complete;
    };
    using Rule = BehaviorRule;
    for (auto &rule : behaviors) {
        if (rule.result == Rule::Result::Fired
            || (rule.result == Rule::Result::ConditionFailed && rule.event != Rule::Event::InteractNearby))
            continue;
        const Point3 &targetPosition = rule.followLaunchTarget && rule.action == Rule::Action::LaunchUpward && rule.target < world.bodies.size()
            ? world.bodies[rule.target].position : rule.position;
        const bool triggered = rule.event == Rule::Event::PlayStarted
            || (rule.event == Rule::Event::MissionCompleted && rule.mission && complete(*rule.mission))
            || (rule.event == Rule::Event::InteractNearby && interact
                && std::hypot(position[0]-targetPosition[0],position[1]-targetPosition[1],position[2]-targetPosition[2]) <= 3);
        if (!triggered)
            continue;
        if (rule.requiredMission && !complete(*rule.requiredMission)) {
            // An interaction is retryable. One-shot events report the failed condition.
            rule.result = Rule::Result::ConditionFailed;
            continue;
        }
        if (rule.action == Rule::Action::PulseBeacon && rule.target < behaviorBeacons.size())
            behaviorBeacons[rule.target].remaining = rule.amount;
        else if (rule.action == Rule::Action::RaiseObject && rule.target < world.bodies.size())
            world.bodies[rule.target].position[2] += rule.amount;
        else if (rule.action != Rule::Action::LaunchUpward || !world.launchUpward(rule.target,rule.amount))
            continue;
        rule.result = Rule::Result::Fired;
    }
}

const MissionShot *Gameplay::activeShot() const
{
    const auto shot = std::ranges::find_if(shots,[](const auto &item) { return item.remaining > 0; });
    return shot == shots.end() ? nullptr : &*shot;
}

Point3 shotPosition(const MissionShot &shot,float progress)
{
    Point3 result{};
    for (std::size_t axis = 0; axis < result.size(); ++axis)
        result[axis] = std::lerp(shot.first[axis],shot.last[axis],std::clamp(progress,0.0f,1.0f));
    return result;
}

void GameplayTrace::observe(const Gameplay &gameplay)
{
    for (const auto &beacon : gameplay.beacons) {
        beaconActive |= beacon.remaining > 0;
        beaconEnded |= beacon.triggered && beacon.remaining <= 0;
    }
    for (const auto &shot : gameplay.shots) {
        shotActive |= shot.remaining > 0;
        shotEnded |= shot.triggered && shot.remaining <= 0;
    }
    for (const auto &patrol : gameplay.patrols) {
        patrolWaited |= patrol.state == NpcPatrol::State::Waiting;
        patrolReturned |= patrol.state == NpcPatrol::State::Returning;
    }
}

bool GameplayTrace::complete(const Gameplay &gameplay) const
{
    return std::ranges::all_of(gameplay.beacons,[](const auto &beacon) { return beacon.triggered && beacon.remaining <= 0; })
        && std::ranges::all_of(gameplay.shots,[](const auto &shot) { return shot.triggered && shot.remaining <= 0; })
        && std::ranges::all_of(gameplay.patrols,[](const auto &patrol) { return patrol.resumed; });
}

Mesh makePatrolMesh(const NpcPatrol &patrol)
{
    auto mesh = makeCube();
    mesh.materials.front().color = {0.2f,0.72f,0.9f,1};
    for (auto &vertex : mesh.vertices) {
        vertex[0] = vertex[0]*0.45f+patrol.position[0];
        vertex[1] = vertex[1]*0.45f+patrol.position[1];
        vertex[2] = vertex[2]*1.6f+patrol.position[2]+0.8f;
    }
    return mesh;
}

Mesh makeBeaconMesh(const MissionBeacon &beacon)
{
    auto mesh = makeCube();
    mesh.materials.front().color = {beacon.color[0],beacon.color[1],beacon.color[2],1};
    const float phase = (beacon.seconds-beacon.remaining)*6.0f;
    const float scale = 0.3f + 0.1f*std::sin(phase);
    for (auto &vertex : mesh.vertices)
        for (std::size_t axis = 0; axis < vertex.size(); ++axis)
            vertex[axis] = vertex[axis]*scale+beacon.position[axis];
    return mesh;
}

std::string Gameplay::objective() const
{
    const auto active = std::ranges::find_if(missions, [](const Mission &mission) {
        return mission.state != Mission::State::Complete;
    });
    if (active == missions.end()) {
        if (missions.empty())
            return {};
        return missions.back().completionText.empty() ? missions.back().name + " complete" : missions.back().completionText;
    }
    switch (active->state) {
    case Mission::State::Waiting:
        return "Go to the " + active->name + " marker to start";
    case Mission::State::Travel:
        return active->name + ": " + active->goal;
    case Mission::State::Interact:
        return active->name + ": press E to finish";
    case Mission::State::Complete:
        break;
    }
    return {};
}

bool Gameplay::missionsComplete() const
{
    return !missions.empty() && std::ranges::all_of(missions, [](const Mission &mission) {
        return mission.state == Mission::State::Complete;
    });
}

std::optional<Point3> Gameplay::missionTarget() const
{
    for (const auto &mission : missions)
        switch (mission.state) {
        case Mission::State::Waiting:
            return mission.start;
        case Mission::State::Travel:
            return mission.waypoint;
        case Mission::State::Interact:
            return mission.target;
        case Mission::State::Complete:
            break;
        }
    return std::nullopt;
}

bool Autopilot::drive(const WorldSimulation &world, std::size_t player, const Gameplay &gameplay, float &yaw,
                      bool &interact)
{
    interact = false;
    const auto target = gameplay.missionTarget();
    if (!target || player >= world.bodies.size())
        return false;
    const auto &body = world.bodies[player];
    const auto &p = body.position;
    const auto active = std::ranges::find_if(gameplay.missions, [](const Mission &mission) {
        return mission.state != Mission::State::Complete;
    });
    if (active != gameplay.missions.end() && active->state == Mission::State::Interact
        && planar(p, *target) <= active->targetRadius * 0.8f) {
        interact = true;
        return false;
    }
    if (!goal || *goal != *target) {
        goal = target;
        path.clear();
    }
    if (--replan <= 0 || path.empty()) {
        path = findPath(world, p, *target, std::max(body.halfSize[0], body.halfSize[1]) + 0.3f,
                        p[2] - body.halfSize[2], p[2] + body.halfSize[2], player);
        stuck = path.empty();
        replan = 60;
    }
    // Reach each corner closely so the body does not clip the obstacle while turning toward the next one.
    while (path.size() > 1 && planar(path.front(), p) < 0.15f)
        path.erase(path.begin());
    const auto next = path.empty() ? *target : path.front();
    const float dx = next[0] - p[0], dy = next[1] - p[1];
    if (std::hypot(dx, dy) < 0.05f)
        return false;
    // drivePlayer moves along (-cos(yaw), -sin(yaw)).
    yaw = std::atan2(-dy, -dx);
    return true;
}

bool Gameplay::trafficLooped() const
{
    return !vehicles.empty() && std::ranges::all_of(vehicles, [&](const TrafficVehicle &vehicle) {
        return vehicle.route < routes.size() && vehicle.reached >= routes[vehicle.route].points.size();
    });
}

}
