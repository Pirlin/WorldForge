#pragma once

#include <iomanip>
#include <iostream>
#include <locale>
#include <string_view>

namespace worldforge {

enum class Event { PlayStarted, MissionCompleted, InteractNearby };
enum class Action { PulseBeacon, RaiseObject, LaunchUpward };

struct Behavior {
    std::string_view name;
    Event event = Event::PlayStarted;
    Action action = Action::PulseBeacon;
    std::string_view mission;
    std::string_view requireCompletedMission;
    float amount = 2;
};

struct Trigger {
    Event event;
    std::string_view mission{};
};

namespace events {
constexpr Trigger playStarted() { return {Event::PlayStarted}; }
constexpr Trigger interactNearby() { return {Event::InteractNearby}; }
constexpr Trigger missionCompleted(std::string_view markerId) { return {Event::MissionCompleted,markerId}; }
}

struct Operation {
    Action action;
    float amount;
};

namespace effects {
constexpr Operation pulseBeacon(float seconds) { return {Action::PulseBeacon,seconds}; }
}

namespace scene {
constexpr Operation raise(float meters) { return {Action::RaiseObject,meters}; }
}

namespace physics {
constexpr Operation launchUpward(float metersPerSecond) { return {Action::LaunchUpward,metersPerSecond}; }
}

constexpr Behavior makeBehavior(std::string_view name,Trigger trigger,Operation operation,
                                std::string_view requireCompletedMission = {})
{
    return {name,trigger.event,operation.action,trigger.mission,requireCompletedMission,operation.amount};
}

inline int publish(const Behavior &rule)
{
    std::cout.imbue(std::locale::classic());
    const int version = rule.action == Action::LaunchUpward ? 2 : 1;
    std::cout << "WORLDFORGE_BEHAVIOR " << version << '\n' << std::quoted(rule.name) << ' '
              << int(rule.event) << ' ' << int(rule.action) << ' ' << std::quoted(rule.mission)
              << ' ' << std::quoted(rule.requireCompletedMission) << ' ' << rule.amount << '\n';
    return std::cout ? 0 : 1;
}

}
