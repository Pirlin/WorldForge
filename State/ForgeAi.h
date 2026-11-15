#pragma once

#include "State/ProjectStore.h"

#include <QColor>
#include <QJsonObject>
#include <QStringList>

// Forge AI turns one creator request into a short list of typed authoring commands. Each command names
// its target, the revision it expects, and its parameters, so the editor can review, run, and undo it.
namespace forge {

struct Candidate {
    QString id;
    QString name;
    bool operator==(const Candidate &) const = default;
};

struct Command {
    QString operation;        // bevel_edges, paint_stripes, place_instance, check_play
    QString target;           // Asset or scene object ID.
    QString expectedRevision; // Content hash of the target when the plan was made.
    QJsonObject parameters;
    QString describe() const;
};

struct BarrierStyle {
    float bevelWidth = 0.04f;
    int bevelSegments = 2;
    int stripes = 4;
    QColor color{QStringLiteral("#f2b705")};
    float wear = 0.55f;
    float fade = 0.4f;
    int seed = 7;
};

struct Plan {
    QString prompt;
    QList<Candidate> barriers;
    QList<Candidate> entrances;
    QString barrierId;
    QString entranceId;
    BarrierStyle style;
    QString error;
    // The barrier or entrance needs the user's choice before the plan can run.
    bool needsChoice() const { return error.isEmpty() && (barrierId.isEmpty() || entranceId.isEmpty()); }
    bool ready() const { return error.isEmpty() && !needsChoice(); }
};

// Reads a request such as "make a worn barrier with faded yellow stripes at the north entrance".
Plan plan(const Project &project, const QString &prompt);
// Returns an empty string when the style parameters are inside the supported ranges.
QString validate(const BarrierStyle &style);
// Resolves the commands for a ready plan. Returns an empty list and sets `error` when targets are missing.
QList<Command> commands(const Project &project, const Plan &plan, QString &error);
// Content hash used to detect that a target changed while a preview was open.
QString revision(const worldforge::Mesh &mesh);
QString revision(const SceneObject &object);
// The new barrier placement at the plan's entrance, resting on the entrance height.
std::optional<SceneObject> placement(const Project &project, const Plan &plan, const worldforge::Mesh &mesh);
// The side the player approaches the placed barrier from in the Play check.
QVector3D approach(const SceneObject &placement);
// Writes the job file read by Workspaces/ForgeJob.py.
QJsonObject jobDescription(const Plan &plan);

// A finished Studio job waiting for review. Nothing in the project changes until apply().
struct Stage {
    Plan plan;
    QList<Command> commands;
    QString assetId;
    const worldforge::Mesh *expectedMesh = nullptr;
    QString meshRevision;
    QString entranceRevision;
    worldforge::Mesh mesh;
    QByteArray source;
    SceneObject placement;
    QStringList steps;
};

std::optional<Stage> stage(const Project &project, const Plan &plan, const QList<Command> &commands,
                           const QByteArray &glb, const QByteArray &source, const QStringList &steps, QString &error);
// Applies the staged mesh and placement as one undoable edit. Fails without changes when the barrier
// or entrance was edited after the plan was made.
bool apply(ProjectStore &store, const Stage &stage, QString &error);

struct CheckReport {
    QStringList passed;
    QStringList failed;
    bool ok() const { return failed.isEmpty() && !passed.isEmpty(); }
};
// Verifies placement, appearance, and collision of an applied stage in a Play simulation.
CheckReport checkInPlay(const ProjectStore &store, const Stage &stage);

}
