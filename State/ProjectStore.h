#pragma once

#include "Engine/Scene/SceneDocument.h"
#include "Engine/Geometry/Mesh.h"
#include "Engine/Geometry/MeshModifiers.h"
#include "Engine/Geometry/MeshUv.h"
#include "Engine/World/Gameplay.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QMap>
#include <QMatrix4x4>
#include <QSharedPointer>
#include <QStringList>
#include <QUndoStack>
#include <QVector3D>
#include <set>

struct StudioMeshData;
namespace worldforge { struct MeshClip; }

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
    QString parentId;
    QMatrix4x4 parentOffset;
    bool operator==(const SceneObject &) const = default;
};

struct MeshUvSet {
    std::vector<worldforge::Point2> corners;
    std::set<worldforge::MeshEdge> seams;
    bool needsUnwrap = false;
    worldforge::UvPins pins;
    bool operator==(const MeshUvSet &) const = default;
};

struct MeshAsset {
    QString id;
    QString name;
    QString sourcePath;
    QString sourceHash;
    bool locallyEdited = false;
    QSharedPointer<const worldforge::Mesh> mesh;
    QSharedPointer<const worldforge::Mesh> evaluatedMesh;
    std::vector<worldforge::MeshModifier> modifiers;
    std::set<worldforge::MeshEdge> uvSeams;
    float lodCell = 0;
    float lodDistance = 25;
    QSharedPointer<const worldforge::Mesh> lodMesh;
    QString activeUvSet = QStringLiteral("UV Map");
    QMap<QString,MeshUvSet> otherUvSets;
    worldforge::UvPins uvPins;
    QJsonObject revolveProfile;
    QString studioSourceHash;
    QSharedPointer<const worldforge::MeshClip> clip;
    const worldforge::Mesh &evaluated() const { return evaluatedMesh ? *evaluatedMesh : *mesh; }
    bool operator==(const MeshAsset &) const = default;
};

struct ProjectTexture {
    QString name;
    QString fileName;
    int width = 0;
    int height = 0;
    QByteArray image;
    QString sourcePath;
    bool operator==(const ProjectTexture &) const = default;
};

struct TextureReimport {
    QString projectId, id;
    ProjectTexture before, after;
};

struct MeshReimport {
    QString projectId;
    MeshAsset before, after;
};

struct ProjectMaterial {
    QString name;
    QString textureId;
    std::array<float,4> tint{1,1,1,1};
    // Normal, metallic-roughness, occlusion, emissive.
    std::array<QString,4> maps;
    float metallic = 0;
    float roughness = 0.65f;
    float emission = 1;
    bool operator==(const ProjectMaterial &) const = default;
};

struct SurfaceLibrary {
    QMap<QString,ProjectTexture> textures;
    QMap<QString,ProjectMaterial> materials;
    bool empty() const { return textures.isEmpty() && materials.isEmpty(); }
    bool operator==(const SurfaceLibrary &) const = default;
};

struct ProjectBehavior {
    QString name;
    worldforge::BehaviorRule::Event event = worldforge::BehaviorRule::Event::PlayStarted;
    worldforge::BehaviorRule::Action action = worldforge::BehaviorRule::Action::PulseBeacon;
    QString mission, requiredMission;
    float amount = 2;
    bool operator==(const ProjectBehavior &) const = default;
};

struct BehaviorComponent {
    QString assetId;
    bool enabled = true;
    QString targetId;
    std::optional<float> amount;
    bool operator==(const BehaviorComponent &) const = default;
};

QList<BehaviorComponent> behaviorComponents(const SceneObject &object);

struct Project {
    QString id;
    QString name;
    QList<SceneObject> objects;
    std::optional<worldforge::SceneDocument> scene;
    QMap<QString, MeshAsset> assets;
    QJsonObject configuration;
    SurfaceLibrary surfaces;
    QMap<QString,ProjectBehavior> behaviors;
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
    QString nativeGameplayPath() const;
    QUndoStack &history() { return history_; }
    bool isDirty() const { return dirty_; }
    QString loadError() const { return loadError_; }
    worldforge::Guid entityGuid(const QString &id) const;
    const MeshAsset *meshAsset(const QString &id) const;
    const MeshAsset *selectedMesh() const;
    qsizetype unusedDemoCount() const;
    bool isLegacyPlaceholder(const SceneObject &object) const;

    void select(const QString &id);
    void switchProject(const QString &id);
    bool createProject(const QString &name, const QJsonObject &configuration = {});
    bool createStarterProject(const QString &name,QJsonObject configuration,QString &error);
    bool renameProject(const QString &name);
    void deleteProject();
    void addObject(const QString &type, const QString &group);
    void addCube(bool half = false);
    bool addMeshPrimitive(worldforge::Primitive kind, worldforge::Point3 size, unsigned segments, unsigned rings,
                          const QString &name, QString &error);
    bool addRetopologyPatch(worldforge::Mesh mesh,QString &error);
    bool addRevolvedMesh(worldforge::Mesh mesh,const QJsonObject &profile,QString &error);
    bool updateRevolvedMesh(const QString &id,worldforge::Mesh mesh,const QJsonObject &profile,
        const worldforge::Mesh *expected,QString &error);
    void addTerrain(bool rolling = false);
    void setAtmosphere(const QJsonObject &settings);
    void sculptTerrain(const QString &id, const QVector3D &point, float radius, float strength, bool smooth);
    void paintTerrain(const QString &id,const QVector3D &point,float radius,int layer);
    void addMeshInstance();
    bool duplicateMesh(QString &error);
    bool makeMeshUnique(QString &error);
    bool bakeMeshTransform(bool centerOrigin, QString &error);
    bool joinMeshes(const QStringList &objectIds, QString &error);
    bool separateMeshFaces(const std::vector<std::size_t> &faceIndices, QString &error);
    void placeMesh(const QString &assetId);
    bool importMesh(const QString &file, QString &error, bool replaceLocalEdits = false);
    bool reimportMesh(QString &error, bool replaceLocalEdits = false);
    bool publishStudioMesh(const QString &assetId, const QByteArray &glb,
                           const worldforge::Mesh *expected, QString &error,
                           const QByteArray &studioSource = {},const QByteArray &uvManifest = {},const QByteArray &clipData = {});
    bool createStudioMesh(const QString &name, worldforge::Mesh mesh, const QByteArray &studioSource, QString &error,
                          const StudioMeshData *studio = nullptr);
    QByteArray exportStudioInput(const MeshAsset &asset,QString &error) const;
    QString studioSourcePath(const MeshAsset &asset) const;
    bool updateMesh(const QString &id, worldforge::Mesh mesh, const QString &label, QString &error,
                    const worldforge::Mesh *expected = nullptr,
                    std::optional<std::set<worldforge::MeshEdge>> seams = {},
                    const QString &studioSourceHash = {}, const SceneObject *placement = nullptr,
                    const StudioMeshData *studio = nullptr);
    // Publishes a staged Studio mesh and adds one placement of it as a single undoable edit.
    bool publishStudioMeshWithPlacement(const QString &assetId, const worldforge::Mesh *expected,
                                        worldforge::Mesh mesh, const QByteArray &studioSource,
                                        const SceneObject &placement, const QString &label, QString &error);
    bool linkMaterialSlot(const QString &target,int targetSlot,const QString &source,int sourceSlot,QString &error);
    QString importProjectTexture(const QString &path,QString &error);
    std::optional<TextureReimport> previewTextureReimport(const QString &id,const QString &path,QString &error) const;
    std::optional<MeshReimport> previewMeshReimport(const QString &id,const QString &path,QString &error) const;
    bool applyTextureReimport(const TextureReimport &preview,QString &error);
    bool applyMeshReimport(const MeshReimport &preview,bool replaceLocalEdits,QString &error);
    QString saveProjectMaterial(const QString &id,const ProjectMaterial &material,QString &error);
    bool assignProjectMaterial(const QString &materialId,const QString &meshId,int slot,QString &error);
    bool preserveProjectMaterials(const QString &meshId,worldforge::Mesh &mesh,QString &error) const;
    bool transformMeshComponents(const QMap<QString,std::set<int>> &selection,
        const QMatrix4x4 &worldTransform,QString &error);
    bool setMirrorX(bool enabled, QString &error);
    bool setMeshModifiers(std::vector<worldforge::MeshModifier> modifiers,QString &error);
    bool applyMeshModifiers(std::size_t through,QString &error);
    bool setMeshLod(float cell,float distance,QString &error);
    bool selectMeshUvSet(const QString &name,bool create,bool rebuild,QString &error);
    bool toggleMeshUvPin(worldforge::UvCorner corner,QString &error);
    void updateObject(const SceneObject &object, const QString &action);
    // Replaces several objects by ID as one undoable edit.
    bool updateObjects(const QList<SceneObject> &objects, const QString &action, QString &error);
    QString saveBehavior(const QString &id,const ProjectBehavior &behavior,QString &error);
    bool attachBehavior(const QString &objectId,const QString &behaviorId,QString &error);
    bool setBehaviorComponents(const QString &objectId,const QList<BehaviorComponent> &components,QString &error);
    bool setNativeComponent(const QString &objectId,const QString &type,std::optional<QJsonObject> component,QString &error);
    bool reparentObject(const QString &childId,const QString &parentId,QString &error);
    QMatrix4x4 worldMatrix(const QString &id) const;
    QMatrix4x4 parentMatrix(const SceneObject &object) const;
    QHash<QString,QMatrix4x4> worldMatrices(const SceneObject *preview = nullptr) const;
    void deleteObject();
    void removeUnusedDemoObjects();
    const SceneObject *environment() const;
    bool save(QString *error);

signals:
    void nativeGameplayChanged();
    void projectsChanged();
    void sceneChanged();
    void selectionChanged();
    void dirtyChanged();
    void editRejected(const QString &reason);

private:
    void pushSceneEdit(const QList<SceneObject> &objects, const QString &selection, const QString &label);
    bool addMesh(worldforge::Mesh mesh,const QString &name,QString &error,const SceneObject *basis = nullptr,
        const QJsonObject &profile = {}, const QString &studioSourceHash = {},const StudioMeshData *studio = nullptr);
    bool storeStudioSource(const QByteArray &source, QString &hash, QString &error);
    bool pushProjectEdit(const QList<SceneObject> &objects,const QMap<QString,MeshAsset> &assets,
                         const QString &selection,const QString &label,QString *error = nullptr,
                         const SurfaceLibrary *surfaces = nullptr,const QString &authoritativeUvAsset = {},
                         const QMap<QString,ProjectBehavior> *behaviors = nullptr);
    bool applyScene(const QList<SceneObject> &objects, const QMap<QString, MeshAsset> &assets,
                    const QString &selection,const SurfaceLibrary &surfaces,const QMap<QString,ProjectBehavior> &behaviors);
    void activateProject(const QString &id);
    void setDirty(bool dirty);
    bool validName(const QString &name, const QString &ignoreId = {}) const;
    bool load(const QString &fileName, bool *newerSchema = nullptr);

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
