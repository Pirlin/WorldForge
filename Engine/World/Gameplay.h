#pragma once

#include "Engine/World/WorldSimulation.h"

#include <string>

namespace worldforge {

// A looping path that traffic vehicles follow at a fixed speed.
struct TrafficRoute {
    std::vector<Point3> points;
    float speed = 7;
};

struct TrafficVehicle {
    std::size_t body = 0;
    std::size_t route = 0;
    std::size_t next = 1;
    std::size_t reached = 0;   // Waypoints reached since Play started.
    int laps = 0;
    float stalled = 0;         // Seconds spent without progress toward the next waypoint.
    float lastDistance = 0;
};

// Walk to the start marker, reach the waypoint, then interact at the target.
struct Mission {
    enum class State { Waiting, Travel, Interact, Complete };
    std::string name;
    std::string goal;
    Point3 start{}, waypoint{}, target{};
    bool hasWaypoint = false;
    float startRadius = 2;
    float waypointRadius = 2;
    float targetRadius = 1.6f;
    State state = State::Waiting;
    std::string completionText;
};

struct MissionBeacon {
    std::size_t mission = 0;
    Point3 position{};
    Point3 color{1.0f, 0.65f, 0.15f};
    float seconds = 2.0f;
    float remaining = 0;
    bool triggered = false;
};

struct MissionShot {
    std::size_t mission = 0;
    Point3 first{}, last{}, target{};
    float seconds = 2;
    float remaining = 0;
    bool triggered = false;
};

struct NpcPatrol {
    enum class State { Walking, Waiting, Returning };
    Point3 start{}, destination{}, position{};
    float speed = 2, wait = 1, remaining = 0;
    State state = State::Walking;
    bool resumed = false;
};

struct BehaviorRule {
    enum class Event { PlayStarted, MissionCompleted, InteractNearby };
    enum class Action { PulseBeacon, RaiseObject, LaunchUpward };
    enum class Result { Waiting, ConditionFailed, Fired };
    std::string label;
    Event event = Event::PlayStarted;
    Action action = Action::PulseBeacon;
    std::optional<std::size_t> mission, requiredMission;
    std::size_t target = 0;
    Point3 position{};
    float amount = 2;
    Result result = Result::Waiting;
    bool followLaunchTarget = true;
    bool valid(std::size_t missions,std::size_t bodies,std::size_t beacons) const;
    bool validBody(const WorldSimulation &world) const;
};

struct Gameplay {
    std::vector<TrafficRoute> routes;
    std::vector<TrafficVehicle> vehicles;
    std::vector<Mission> missions;
    std::vector<MissionBeacon> beacons;
    std::vector<MissionShot> shots;
    std::vector<NpcPatrol> patrols;
    std::vector<BehaviorRule> behaviors;
    std::vector<MissionBeacon> behaviorBeacons;
    bool showMissionHud = true;

    // Puts vehicles on their routes and sets the bodies they drive.
    void start(WorldSimulation &world);
    // Advances traffic and missions before one WorldSimulation::step().
    void update(WorldSimulation &world, std::optional<std::size_t> player, bool interact);
    std::string objective() const;
    bool missionsComplete() const;
    // The point the player should head for next, if a mission is in progress.
    std::optional<Point3> missionTarget() const;
    // True once every vehicle has completed at least one lap.
    bool trafficLooped() const;
    const MissionShot *activeShot() const;
    bool movesBody(std::size_t body) const;
    bool empty() const { return routes.empty() && missions.empty(); }
};

Mesh makeBeaconMesh(const MissionBeacon &beacon);
Mesh makePatrolMesh(const NpcPatrol &patrol);
Point3 shotPosition(const MissionShot &shot,float progress);

struct GameplayTrace {
    bool beaconActive = false, beaconEnded = false;
    bool shotActive = false, shotEnded = false;
    bool patrolWaited = false, patrolReturned = false;
    void observe(const Gameplay &gameplay);
    bool complete(const Gameplay &gameplay) const;
};

// Drives the player through the active mission for automated checks: walks the planned path to each
// mission target and interacts when it arrives.
struct Autopilot {
    std::vector<Point3> path;
    std::optional<Point3> goal;
    int replan = 0;
    bool stuck = false;
    // Returns true when the player should walk forward toward `yaw`. Sets `interact` at the mission target.
    bool drive(const WorldSimulation &world, std::size_t player, const Gameplay &gameplay, float &yaw, bool &interact);
};

// Shortest walkable path in the ground plane around static colliders that block a body of the given
// radius between heights `low` and `high`. Returns {} when no path exists.
std::vector<Point3> findPath(const WorldSimulation &world, Point3 from, Point3 to, float radius, float low,
                             float high, std::optional<std::size_t> ignore = std::nullopt);

}
