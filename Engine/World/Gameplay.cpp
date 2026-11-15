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
}

void Gameplay::update(WorldSimulation &world, std::optional<std::size_t> player, bool interact)
{
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
}

std::string Gameplay::objective() const
{
    const auto active = std::ranges::find_if(missions, [](const Mission &mission) {
        return mission.state != Mission::State::Complete;
    });
    if (active == missions.end())
        return missions.empty() ? std::string() : missions.back().name + " complete";
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
