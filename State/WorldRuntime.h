#pragma once

#include "State/ProjectStore.h"
#include "Engine/World/WorldSimulation.h"
#include "Engine/World/Gameplay.h"
#include "Engine/World/NativeGameplay.h"
#include <QSet>
#include <iosfwd>

class WorldRuntime {
public:
    struct CollisionProbe {
        bool loaded = false;   // Play started and the object has a collider.
        bool blocked = false;  // The player stopped before passing the object's center.
        QVector3D start;
        QVector3D stop;
    };
    bool start(const ProjectStore &store, bool play, bool enableNative = true);
    // Starts Play and walks the player into `objectId` from the `approach` side. Gravity is off so
    // the check measures the object's collider rather than the surrounding ground.
    static CollisionProbe probeCollision(const ProjectStore &store, const QString &objectId, QVector3D approach);
    static bool exportScene(const ProjectStore &store, std::ostream &output, QString *error = nullptr, bool nativeBindings = false);
    bool stageNative(const QString &path, QString &error);
    void stopNative();
    std::uint64_t nativeSteps() const { return native_.steps(); }
    void tick(float yaw);
    std::optional<QVector3D> position(const QString &id) const;
    std::optional<QVector3D> rotation(const QString &id) const;
    std::optional<std::size_t> bodyIndex(const QString &id) const;
    std::optional<QVector3D> player() const;
    std::optional<std::size_t> playerBody() const { return player_; }
    void key(int key, bool down);
    void clearInput() { keys_.clear(); jumpBuffer_ = 0; }
    const QList<SceneObject> &objects() const { return objects_; }
    QHash<QString,QMatrix4x4> renderMatrices() const;
    const QMap<QString, MeshAsset> &assets() const { return assets_; }
    double clipTime() const { return clipTime_; }
    const worldforge::Gameplay &gameplay() const { return gameplay_; }
    const QStringList &gameplayErrors() const { return gameplayErrors_; }
    const worldforge::WorldSimulation &simulation() const { return simulation_; }
    QString objective() const;
    // Moves the player for scripted checks. Keeps the current velocity.
    void placePlayer(const QVector3D &position);

private:
    void buildGameplay(const QMap<QString,ProjectBehavior> &behaviors);
    std::vector<worldforge::BehaviorRule::Result> behaviorResults_;
    worldforge::Gameplay gameplay_;
    QStringList gameplayErrors_;
    struct Binding { std::size_t index; QVector3D offset; };
    QList<SceneObject> objects_;
    QHash<QString,QMatrix4x4> authoredMatrices_;
    QMap<QString, MeshAsset> assets_;
    worldforge::WorldSimulation simulation_;
    QHash<QString, Binding> bindings_;
    std::vector<worldforge::NativeBodyBinding> nativeBodies_;
    std::vector<worldforge::NativeInstance> nativeInstances_;
    worldforge::NativeGameplay native_;
    std::optional<std::size_t> player_;
    QSet<int> keys_;
    int jumpBuffer_ = 0;
    double clipTime_ = 0;
    QVector3D spawn_;
};
