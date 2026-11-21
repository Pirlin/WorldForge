#pragma once

#include "State/ForgeAi.h"
#include "Engine/World/Gameplay.h"

// The playable street: a road with a side street, an enterable repair shop, a looping traffic route, and
// the "Repair Run" mission. It is the test level for coordinated Forge AI edits.
namespace street {

inline constexpr auto missionName = "Repair Run";

// Adds the street's assets and objects to a new project.
void addStarter(Project &project);

struct Report {
    bool missionComplete = false;
    bool enteredShop = false;
    bool trafficLooped = false;
    float seconds = 0;
    float missionSeconds = 0;
    worldforge::GameplayTrace trace;
    bool gameplayVerified = true;
    QStringList gameplayErrors;
    QStringList objectives;
    QString summary() const;
    bool ok() const { return missionComplete && enteredShop && trafficLooped && gameplayVerified; }
};

// Runs Play without a viewport: the autopilot completes the mission while traffic drives its route.
Report verify(const ProjectStore &store, float maxSeconds = 120);

// A coordinated request that changes level, gameplay, and traffic objects together.
struct Plan {
    QString prompt;
    QString error;
    bool moveEntrance = false;
    QString entranceSide;        // "front" or "side"
    bool rerouteTraffic = false;
    QList<forge::Command> commands;
    // The edited objects, applied as one undoable change.
    QList<SceneObject> objects;
    QHash<QString,QString> revisions;
    QStringList changes;
};

Plan plan(const ProjectStore &store, const QString &prompt);
bool apply(ProjectStore &store, const Plan &plan, QString &error);

}
