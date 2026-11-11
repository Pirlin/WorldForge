#pragma once

#include "Engine/Scene/SceneDocument.h"
#include "Engine/Geometry/Mesh.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QMap>
#include <QSharedPointer>
#include <QStringList>
#include <QUndoStack>
#include <QVector3D>

struct SceneObject {
    QString id;
    QString name;
    QString type;
    QString group;
    QString material;
    QVector3D position;
    QVector3D rotation;
    QVector3D scale{1, 1, 1};
    bool visible = true;
    QJsonObject details;
    QString meshAsset;
    bool operator==(const SceneObject &) const = default;
};

struct MeshAsset {
    QString id;
    QString name;
    QString sourcePath;
    QString sourceHash;
    bool locallyEdited = false;
    QSharedPointer<const worldforge::Mesh> mesh;
    bool operator==(const MeshAsset &) const = default;
};

struct Project {
    QString id;
    QString name;
    QList<SceneObject> objects;
    std::optional<worldforge::SceneDocument> scene;
    QMap<QString, MeshAsset> assets;
};

class ProjectStore : public QObject {
    Q_OBJECT

public:
    explicit ProjectStore(QObject *parent = nullptr);
    const QList<Project> &projects() const { return projects_; }
    const Project *project() const;
    const SceneObject *selectedObject() const;
    const SceneObject *object(const QString &id) const;
    const QString &selection() const { return selection_; }
    QUndoStack &history() { return history_; }
    bool isDirty() const { return dirty_; }
    QString loadError() const { return loadError_; }
    worldforge::Guid entityGuid(const QString &id) const;
    const MeshAsset *meshAsset(const QString &id) const;
    const MeshAsset *selectedMesh() const;

    void select(const QString &id);
    void switchProject(const QString &id);
    bool createProject(const QString &name);
    bool renameProject(const QString &name);
    void deleteProject();
    void addObject(const QString &type, const QString &group);
    void addCube();
    void addMeshInstance();
    bool importMesh(const QString &file, QString &error, bool replaceLocalEdits = false);
    bool reimportMesh(QString &error, bool replaceLocalEdits = false);
    bool updateMesh(const QString &id, worldforge::Mesh mesh, const QString &label, QString &error);
    void updateObject(const SceneObject &object, const QString &action);
    void deleteObject();
    bool save(QString *error);

signals:
    void projectsChanged();
    void sceneChanged();
    void selectionChanged();
    void dirtyChanged();
    void editRejected(const QString &reason);

private:
    void pushSceneEdit(const QList<SceneObject> &objects, const QString &selection, const QString &label);
    void pushProjectEdit(const QList<SceneObject> &objects, const QMap<QString, MeshAsset> &assets,
                         const QString &selection, const QString &label);
    bool applyScene(const QList<SceneObject> &objects, const QMap<QString, MeshAsset> &assets,
                    const QString &selection);
    void activateProject(const QString &id);
    void setDirty(bool dirty);
    bool validName(const QString &name, const QString &ignoreId = {}) const;
    bool load(const QString &fileName);

    QList<Project> projects_;
    QString activeId_;
    QString selection_;
    QHash<QString, QString> projectSelections_;
    QUndoStack history_;
    QString path_;
    QByteArray persistedManifest_;
    QByteArray baselineManifest_;
    QString loadError_;
    bool dirty_ = false;
};
