#include "State/ProjectStore.h"
#include "State/GlbImport.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <functional>
#include <limits>

namespace {
namespace wf = worldforge;
constexpr int projectSchemaVersion = 1;

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QVector3D readVector(const QJsonObject &object, const QString &key, QVector3D fallback = {})
{
    const auto values = object.value(key).toArray();
    if (values.size() != 3)
        return fallback;
    return {float(values[0].toDouble()), float(values[1].toDouble()), float(values[2].toDouble())};
}

QJsonArray writeVector(const QVector3D &v)
{
    return {v.x(), v.y(), v.z()};
}

wf::Guid stableGuid(const QString &alias, const QUuid &scope)
{
    if (const auto parsed = wf::Guid::parse(alias.toStdString()); parsed && !parsed->isNull())
        return *parsed;
    const auto generated = QUuid::createUuidV5(scope, alias.toUtf8()).toString(QUuid::WithoutBraces);
    return *wf::Guid::parse(generated.toStdString());
}

wf::Guid projectGuid(const QString &alias)
{
    return stableGuid(alias, QUuid("{648a2b95-9cae-452e-82ef-46f342f263a8}"));
}

wf::Guid sceneGuid(wf::Guid project)
{
    return stableGuid("worldforge-main-scene", QUuid(QString::fromStdString(project.toString())));
}

wf::Guid objectGuid(wf::Guid project, const QString &alias)
{
    return stableGuid(alias, QUuid(QString::fromStdString(project.toString())));
}

QString guidText(wf::Guid id)
{
    return QString::fromStdString(id.toString());
}

wf::Transform transform(const SceneObject &object)
{
    return {{object.position.x(), object.position.y(), object.position.z()},
            {object.rotation.x(), object.rotation.y(), object.rotation.z()},
            {object.scale.x(), object.scale.y(), object.scale.z()}};
}

std::optional<wf::SceneDocument> openScene(const Project &project, wf::Revision revision)
{
    const auto id = projectGuid(project.id);
    wf::SceneSnapshot snapshot{id, sceneGuid(id), revision, {}};
    for (const auto &object : project.objects)
        snapshot.entities.push_back({objectGuid(id, object.id), std::nullopt, object.name.toStdString(), transform(object)});
    auto result = wf::SceneDocument::open(snapshot);
    if (auto *scene = std::get_if<wf::SceneDocument>(&result))
        return std::move(*scene);
    return std::nullopt;
}

std::optional<wf::SceneDocument> editedScene(const Project &project, const QList<SceneObject> &next)
{
    auto scene = *project.scene;
    const auto edit = [&scene](wf::SceneEdit operation) {
        return scene.apply({wf::Guid::generate(), scene.projectId(), scene.sceneId(), scene.revision(),
                            std::move(operation)}).ok();
    };
    QHash<QString, const SceneObject *> oldObjects;
    QSet<QString> newIds;
    for (const auto &object : project.objects)
        oldObjects.insert(object.id, &object);
    for (const auto &object : next) {
        if (object.id.isEmpty() || newIds.contains(object.id))
            return std::nullopt;
        newIds.insert(object.id);
        const auto id = objectGuid(scene.projectId(), object.id);
        const auto *previous = oldObjects.value(object.id);
        if (!previous) {
            if (!edit(wf::AddEntity{{id, std::nullopt, object.name.toStdString(), transform(object)}}))
                return std::nullopt;
            continue;
        }
        if (object.name != previous->name && !edit(wf::RenameEntity{id, object.name.toStdString()}))
            return std::nullopt;
        const auto nextTransform = transform(object);
        if (nextTransform != transform(*previous) && !edit(wf::SetTransform{id, nextTransform}))
            return std::nullopt;
    }
    for (const auto &object : project.objects)
        if (!newIds.contains(object.id) && !edit(wf::RemoveEntity{objectGuid(scene.projectId(), object.id)}))
            return std::nullopt;
    return scene;
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QString checksum(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

bool atomicWrite(const QString &path, const QByteArray &bytes, QString &error)
{
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit())
        return true;
    error = file.errorString();
    return false;
}

template<std::size_t N>
std::array<float, N> meshVector(const QJsonValue &field)
{
    const auto array = field.toArray();
    std::array<float, N> result;
    for (std::size_t i = 0; i < N; ++i)
        result[i] = float(array.at(i).toDouble(std::numeric_limits<double>::quiet_NaN()));
    return result;
}

template<std::size_t N>
QJsonArray meshVector(const std::array<float, N> &field)
{
    QJsonArray array;
    for (auto value : field)
        array.append(value);
    return array;
}

std::optional<MeshAsset> readAsset(const QJsonObject &row)
{
    MeshAsset asset;
    asset.id = row.value("id").toString();
    asset.name = row.value("name").toString();
    asset.sourcePath = row.value("sourcePath").toString();
    asset.sourceHash = row.value("sourceHash").toString();
    asset.locallyEdited = row.value("locallyEdited").toBool();
    if (!wf::Guid::parse(asset.id.toStdString()) || asset.name.isEmpty())
        return std::nullopt;
    wf::Mesh mesh;
    mesh.materials.clear();
    for (const auto &value : row.value("materials").toArray()) {
        const auto material = value.toObject();
        mesh.materials.push_back({material.value("name").toString().toStdString(),
            meshVector<4>(material.value("color")), float(material.value("metallic").toDouble()),
            float(material.value("roughness").toDouble())});
    }
    for (const auto &value : row.value("vertices").toArray())
        mesh.vertices.push_back(meshVector<3>(value));
    for (const auto &value : row.value("faces").toArray()) {
        const auto fields = value.toObject();
        wf::MeshFace face;
        face.material = std::uint32_t(fields.value("material").toInt(-1));
        for (const auto &value : fields.value("corners").toArray()) {
            const auto corner = value.toArray();
            face.corners.push_back({wf::VertexIndex(corner.at(0).toInt(-1)), meshVector<2>(corner.at(1)),
                                    meshVector<3>(corner.at(2))});
        }
        mesh.faces.push_back(std::move(face));
    }
    if (!wf::validateMesh(mesh).empty())
        return std::nullopt;
    asset.mesh = QSharedPointer<wf::Mesh>::create(std::move(mesh));
    return asset;
}

QJsonObject writeAsset(const MeshAsset &asset)
{
    QJsonArray vertices, faces, materials;
    for (const auto &vertex : asset.mesh->vertices)
        vertices.append(meshVector(vertex));
    for (const auto &face : asset.mesh->faces) {
        QJsonArray corners;
        for (const auto &corner : face.corners)
            corners.append(QJsonArray{int(corner.vertex), meshVector(corner.uv), meshVector(corner.normal)});
        faces.append(QJsonObject{{"material", int(face.material)}, {"corners", corners}});
    }
    for (const auto &material : asset.mesh->materials)
        materials.append(QJsonObject{{"name", QString::fromStdString(material.name)}, {"color", meshVector(material.color)},
                                     {"metallic", material.metallic}, {"roughness", material.roughness}});
    return {{"id", asset.id}, {"name", asset.name}, {"sourcePath", asset.sourcePath}, {"sourceHash", asset.sourceHash},
            {"locallyEdited", asset.locallyEdited}, {"vertices", vertices}, {"faces", faces}, {"materials", materials}};
}

class UndoSceneEdit : public QUndoCommand {
public:
    UndoSceneEdit(const QString &action, std::function<void()> redo, std::function<void()> undo)
        : QUndoCommand(action), redo_(std::move(redo)), undo_(std::move(undo)) {}
    void redo() override { redo_(); }
    void undo() override { undo_(); }

private:
    std::function<void()> redo_;
    std::function<void()> undo_;
};
}

ProjectStore::ProjectStore(QObject *parent) : QObject(parent)
{
    const QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    path_ = dir.filePath("projects.wfproject");
    baselineManifest_ = readFile(path_);
    const bool hadManifest = QFile::exists(path_);
    if (hadManifest && load(path_))
        return;
    if (load(dir.filePath("projects.previous.wfproject"))) {
        loadError_ = tr("Opened the previous complete save. The latest project or scene file could not be read.");
        setDirty(true);
        return;
    }
    const auto legacy = dir.filePath("projects.json");
    if (!hadManifest && load(legacy)) {
        setDirty(true);
        return;
    }
    if (hadManifest || QFile::exists(legacy)) {
        const auto recovered = dir.entryInfoList({"projects-recovered-*.wfproject"}, QDir::Files, QDir::Time);
        for (const auto &candidate : recovered) {
            if (!load(candidate.absoluteFilePath()))
                continue;
            path_ = candidate.absoluteFilePath();
            baselineManifest_ = persistedManifest_;
            loadError_ = tr("Opened recovered projects from %1.").arg(candidate.fileName());
            return;
        }
        path_ = dir.filePath("projects-recovered-" + newId() + ".wfproject");
        baselineManifest_.clear();
        const bool recoveredLegacy = load(legacy);
        loadError_ = tr("The saved project files could not be read. %1 New changes will go to %2.")
            .arg(recoveredLegacy ? tr("Opened the legacy projects.") : tr("Opened demo data."), QFileInfo(path_).fileName());
        if (recoveredLegacy)
            return;
    }
    load(":/Data/demo.json");
}

bool ProjectStore::load(const QString &fileName)
{
    const auto bytes = readFile(fileName);
    const auto document = QJsonDocument::fromJson(bytes);
    const auto data = document.object();
    if (!data.value("projects").isArray())
        return false;
    const bool versioned = fileName.endsWith(".wfproject");
    if (versioned && data.value("schemaVersion").toInt() != projectSchemaVersion)
        return false;

    QList<Project> loaded;
    QSet<QString> projectIds;
    for (const auto &entry : data.value("projects").toArray()) {
        const auto fields = entry.toObject();
        Project project{fields.value("id").toString(), fields.value("name").toString(), {}, {}, {}};
        if (project.id.isEmpty() || projectIds.contains(project.id)
            || project.name.isEmpty())
            return false;
        projectIds.insert(project.id);

        QJsonObject scene = fields;
        wf::Revision revision = 0;
        const auto id = projectGuid(project.id);
        if (versioned) {
            const auto sceneFile = fields.value("sceneFile").toString();
            const auto prefix = guidText(sceneGuid(id)) + '-';
            if (!sceneFile.startsWith(prefix) || !sceneFile.endsWith(".wfscene")
                || sceneFile.size() != prefix.size() + 36 + 8
                || !wf::Guid::parse(sceneFile.mid(prefix.size(), 36).toStdString()))
                return false;
            const auto sceneBytes = readFile(QFileInfo(fileName).dir().filePath("scenes/" + sceneFile));
            if (sceneBytes.isEmpty() || checksum(sceneBytes) != fields.value("sceneHash").toString())
                return false;
            scene = QJsonDocument::fromJson(sceneBytes).object();
            bool validRevision = false;
            revision = scene.value("revision").toString().toULongLong(&validRevision);
            if (!validRevision || scene.value("schemaVersion").toInt() != int(wf::sceneSchemaVersion)
                || scene.value("projectGuid").toString() != guidText(id)
                || scene.value("sceneGuid").toString() != guidText(sceneGuid(id)))
                return false;
        }
        if (!scene.value("objects").isArray())
            return false;
        for (const auto &entry : scene.value("meshAssets").toArray()) {
            const auto asset = readAsset(entry.toObject());
            if (!asset || project.assets.contains(asset->id))
                return false;
            project.assets.insert(asset->id, *asset);
        }
        QSet<QString> objectIds;
        for (const auto &value : scene.value("objects").toArray()) {
            const auto row = value.toObject();
            SceneObject object;
            object.id = row.value("id").toString();
            object.name = row.value("name").toString();
            if (object.id.isEmpty() || objectIds.contains(object.id) || object.name.isEmpty())
                return false;
            if (versioned && row.value("guid").toString() != guidText(objectGuid(id, object.id)))
                return false;
            objectIds.insert(object.id);
            object.type = row.value("type").toString();
            object.group = row.value("group").toString();
            object.material = row.value("material").toString();
            object.position = readVector(row, "position");
            object.rotation = readVector(row, "rotation");
            object.scale = readVector(row, "scale", {1, 1, 1});
            object.visible = row.value("visible").toBool(true);
            object.details = row.value("details").toObject();
            object.meshAsset = row.value("meshAsset").toString();
            if (!object.meshAsset.isEmpty() && !project.assets.contains(object.meshAsset))
                return false;
            project.objects.append(object);
        }
        project.scene = openScene(project, revision);
        if (!project.scene)
            return false;
        loaded.append(project);
    }

    projects_ = std::move(loaded);
    activeId_ = data.value("activeProject").toString();
    if (!project())
        activeId_ = projects_.isEmpty() ? QString() : projects_.first().id;
    selection_ = data.value("selection").toString();
    if (!object(selection_))
        selection_ = project() && !project()->objects.isEmpty() ? project()->objects.first().id : QString();
    persistedManifest_ = versioned ? bytes : QByteArray();
    return true;
}

bool ProjectStore::save(QString *error)
{
    QString message;
    const auto dir = QFileInfo(path_).dir();
    if (!QDir().mkpath(dir.filePath("scenes"))) {
        *error = tr("Could not create the local project directory.");
        return false;
    }
    QLockFile lock(path_ + ".lock");
    if (!lock.tryLock() || readFile(path_) != baselineManifest_) {
        *error = tr("The project files changed in another editor. Reopen the project before saving.");
        return false;
    }

    const auto generation = newId();
    const auto previousPath = path_ == dir.filePath("projects.wfproject")
        ? dir.filePath("projects.previous.wfproject") : path_ + ".previous";
    const auto older = QJsonDocument::fromJson(readFile(previousPath)).object().value("projects").toArray();
    QJsonArray projects;
    for (const auto &project : projects_) {
        const auto &scene = *project.scene;
        QJsonArray objects, assets;
        for (const auto &asset : project.assets)
            assets.append(writeAsset(asset));
        for (const auto &object : project.objects) {
            objects.append(QJsonObject{{"id", object.id}, {"name", object.name}, {"type", object.type},
                {"guid", guidText(objectGuid(scene.projectId(), object.id))},
                {"group", object.group}, {"material", object.material}, {"position", writeVector(object.position)},
                {"rotation", writeVector(object.rotation)}, {"scale", writeVector(object.scale)}, {"visible", object.visible},
                {"details", object.details}, {"meshAsset", object.meshAsset}});
        }
        const auto sceneFile = guidText(scene.sceneId()) + '-' + generation + ".wfscene";
        const auto sceneBytes = QJsonDocument(QJsonObject{{"schemaVersion", int(wf::sceneSchemaVersion)},
            {"projectGuid", guidText(scene.projectId())}, {"sceneGuid", guidText(scene.sceneId())},
            {"revision", QString::number(scene.revision())}, {"objects", objects}, {"meshAssets", assets}}).toJson(QJsonDocument::Compact);
        if (!atomicWrite(dir.filePath("scenes/" + sceneFile), sceneBytes, message)) {
            *error = message;
            return false;
        }
        projects.append(QJsonObject{{"id", project.id}, {"name", project.name}, {"sceneFile", sceneFile},
                                    {"sceneHash", checksum(sceneBytes)}});
    }
    const auto bytes = QJsonDocument(QJsonObject{{"schemaVersion", projectSchemaVersion},
        {"activeProject", activeId_}, {"selection", selection_}, {"projects", projects}}).toJson();
    if ((!persistedManifest_.isEmpty() && !atomicWrite(previousPath, persistedManifest_, message))
        || !atomicWrite(path_, bytes, message)) {
        *error = message;
        return false;
    }
    QSet<QString> retained;
    for (const auto &entry : QJsonDocument::fromJson(persistedManifest_).object().value("projects").toArray())
        retained.insert(entry.toObject().value("sceneFile").toString());
    static const QRegularExpression ownedFile("^[a-f0-9-]{36}-[a-f0-9-]{36}\\.wfscene$");
    for (const auto &entry : older) {
        const auto name = entry.toObject().value("sceneFile").toString();
        if (ownedFile.match(name).hasMatch() && !retained.contains(name))
            QFile::remove(dir.filePath("scenes/" + name));
    }
    persistedManifest_ = baselineManifest_ = bytes;
    setDirty(false);
    return true;
}

const Project *ProjectStore::project() const
{
    const auto match = std::ranges::find(projects_, activeId_, &Project::id);
    return match == projects_.cend() ? nullptr : &*match;
}

const SceneObject *ProjectStore::object(const QString &id) const
{
    const auto *active = project();
    if (!active)
        return nullptr;
    const auto match = std::ranges::find(active->objects, id, &SceneObject::id);
    return match == active->objects.cend() ? nullptr : &*match;
}

const SceneObject *ProjectStore::selectedObject() const
{
    return object(selection_);
}

wf::Guid ProjectStore::entityGuid(const QString &id) const
{
    const auto *active = project();
    return active && object(id) ? objectGuid(active->scene->projectId(), id) : wf::Guid{};
}

const MeshAsset *ProjectStore::meshAsset(const QString &id) const
{
    const auto *active = project();
    if (!active)
        return nullptr;
    const auto asset = active->assets.constFind(id);
    return asset == active->assets.cend() ? nullptr : &*asset;
}

const MeshAsset *ProjectStore::selectedMesh() const
{
    const auto *selected = selectedObject();
    return selected ? meshAsset(selected->meshAsset) : nullptr;
}

void ProjectStore::select(const QString &id)
{
    const auto next = object(id) ? id : QString();
    if (selection_ == next)
        return;
    selection_ = next;
    projectSelections_.insert(activeId_, selection_);
    emit selectionChanged();
}

void ProjectStore::setDirty(bool dirty)
{
    if (dirty_ == dirty)
        return;
    dirty_ = dirty;
    emit dirtyChanged();
}

bool ProjectStore::validName(const QString &name, const QString &ignoreId) const
{
    const auto trimmed = name.trimmed();
    return !trimmed.isEmpty() && trimmed.size() <= 64 && std::ranges::none_of(projects_, [&](const Project &project) {
        return project.id != ignoreId && project.name.compare(trimmed, Qt::CaseInsensitive) == 0;
    });
}

void ProjectStore::activateProject(const QString &id)
{
    history_.clear();
    activeId_ = id;
    const auto *active = project();
    selection_ = projectSelections_.value(id, active && !active->objects.isEmpty() ? active->objects.first().id : QString());
    if (!object(selection_))
        selection_.clear();
    setDirty(true);
    emit projectsChanged();
    emit sceneChanged();
    emit selectionChanged();
}

void ProjectStore::switchProject(const QString &id)
{
    if (id != activeId_ && std::ranges::find(projects_, id, &Project::id) != projects_.end())
        activateProject(id);
}

bool ProjectStore::createProject(const QString &name)
{
    if (!validName(name))
        return false;
    const auto id = newId();
    Project created{id, name.trimmed(), {}, {}, {}};
    created.scene = openScene(created, 0);
    projects_.append(std::move(created));
    activateProject(id);
    return true;
}

bool ProjectStore::renameProject(const QString &name)
{
    if (!project() || !validName(name, activeId_))
        return false;
    auto match = std::ranges::find(projects_, activeId_, &Project::id);
    if (match->name != name.trimmed()) {
        match->name = name.trimmed();
        setDirty(true);
        emit projectsChanged();
    }
    return true;
}

void ProjectStore::deleteProject()
{
    if (!project())
        return;
    projectSelections_.remove(activeId_);
    projects_.removeIf([this](const Project &project) { return project.id == activeId_; });
    activateProject(projects_.isEmpty() ? QString() : projects_.first().id);
}

void ProjectStore::pushSceneEdit(const QList<SceneObject> &objects, const QString &selection, const QString &label)
{
    if (const auto *active = project())
        pushProjectEdit(objects, active->assets, selection, label);
}

void ProjectStore::pushProjectEdit(const QList<SceneObject> &objects, const QMap<QString, MeshAsset> &assets,
                                  const QString &selection, const QString &label)
{
    const auto *active = project();
    if (!active || (active->objects == objects && active->assets == assets))
        return;
    if (!editedScene(*active, objects)) {
        emit editRejected(tr("The scene rejected this edit. Check the name and transform values."));
        emit sceneChanged();
        return;
    }
    const auto previous = active->objects;
    const auto previousAssets = active->assets;
    history_.push(new UndoSceneEdit(label,
        [this, objects, assets, selection] { applyScene(objects, assets, selection); },
        [this, previous, previousAssets, selection = selection_] { applyScene(previous, previousAssets, selection); }));
}

bool ProjectStore::applyScene(const QList<SceneObject> &objects, const QMap<QString, MeshAsset> &assets,
                              const QString &selection)
{
    auto match = std::ranges::find(projects_, activeId_, &Project::id);
    if (match == projects_.end())
        return false;
    auto updated = editedScene(*match, objects);
    if (!updated)
        return false;
    match->scene = std::move(updated);
    match->objects = objects;
    match->assets = assets;
    selection_ = object(selection) ? selection : QString();
    projectSelections_.insert(activeId_, selection_);
    setDirty(true);
    emit sceneChanged();
    emit selectionChanged();
    return true;
}

void ProjectStore::addCube()
{
    const auto *active = project();
    if (!active)
        return;
    auto assets = active->assets;
    auto objects = active->objects;
    MeshAsset asset{newId(), tr("Cube"), {}, {}, false, QSharedPointer<wf::Mesh>::create(wf::makeCube())};
    SceneObject object;
    object.id = newId();
    object.name = asset.name;
    object.type = "Static Mesh";
    object.group = "Props";
    object.meshAsset = asset.id;
    int suffix = 2;
    while (std::ranges::find(objects, object.name, &SceneObject::name) != objects.end())
        object.name = asset.name + '_' + QString::number(suffix++);
    asset.name = object.name;
    assets.insert(asset.id, asset);
    objects.append(object);
    pushProjectEdit(objects, assets, object.id, tr("Add Cube"));
}

void ProjectStore::addMeshInstance()
{
    const auto *asset = selectedMesh();
    if (!asset)
        return;
    auto objects = project()->objects;
    auto copy = *selectedObject();
    copy.id = newId();
    copy.position += QVector3D(2, 0, 0);
    int suffix = 2;
    do {
        copy.name = asset->name + '_' + QString::number(suffix++);
    } while (std::ranges::find(objects, copy.name, &SceneObject::name) != objects.end());
    objects.append(copy);
    pushSceneEdit(objects, copy.id, tr("Add Mesh Instance"));
}

bool ProjectStore::importMesh(const QString &fileName, QString &error, bool replaceLocalEdits)
{
    const auto *active = project();
    if (!active) {
        error = tr("Open a project before importing.");
        return false;
    }
    const auto path = QFileInfo(fileName).canonicalFilePath();
    QFile file(path);
    constexpr qsizetype maxBytes = 64 * 1024 * 1024;
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly) || file.size() > maxBytes) {
        error = tr("Choose a readable GLB file no larger than 64 MiB.");
        return false;
    }
    const auto bytes = file.read(maxBytes + 1);
    const auto hash = checksum(bytes);
    const auto found = std::ranges::find_if(active->assets, [&](const MeshAsset &asset) {
        return asset.sourcePath == path;
    });
    const bool reimport = found != active->assets.cend();
    if (reimport && found->locallyEdited && !replaceLocalEdits) {
        error = tr("This mesh has local edits. Use Reimport Source to review replacement.");
        return false;
    }
    if (reimport && found->sourceHash == hash && !found->locallyEdited)
        return true;
    auto mesh = importGlb(bytes, error);
    if (!mesh)
        return false;
    auto assets = active->assets;
    auto objects = active->objects;
    auto selection = selection_;
    MeshAsset asset{reimport ? found->id : newId(),
        reimport ? found->name : QFileInfo(path).completeBaseName().simplified().left(64), path, hash, false,
        QSharedPointer<wf::Mesh>::create(std::move(*mesh))};
    if (asset.name.isEmpty())
        asset.name = tr("Imported Mesh");
    if (!reimport) {
        SceneObject object;
        object.id = newId();
        object.name = asset.name;
        object.type = "Static Mesh";
        object.group = "Props";
        object.meshAsset = asset.id;
        objects.append(object);
        selection = object.id;
    }
    assets.insert(asset.id, asset);
    pushProjectEdit(objects, assets, selection, reimport ? tr("Reimport GLB") : tr("Import GLB"));
    return true;
}

bool ProjectStore::reimportMesh(QString &error, bool replaceLocalEdits)
{
    const auto *asset = selectedMesh();
    if (!asset || asset->sourcePath.isEmpty()) {
        error = tr("Select a mesh imported from a GLB source.");
        return false;
    }
    return importMesh(asset->sourcePath, error, replaceLocalEdits);
}

bool ProjectStore::updateMesh(const QString &id, wf::Mesh mesh, const QString &label, QString &error)
{
    const auto *asset = meshAsset(id);
    if (!asset) {
        error = tr("The mesh is no longer available.");
        return false;
    }
    error = QString::fromStdString(wf::validateMesh(mesh));
    if (!error.isEmpty())
        return false;
    if (*asset->mesh == mesh)
        return true;
    auto assets = project()->assets;
    auto &updated = assets[id];
    updated.mesh = QSharedPointer<wf::Mesh>::create(std::move(mesh));
    updated.locallyEdited = true;
    pushProjectEdit(project()->objects, assets, selection_, label);
    return true;
}

void ProjectStore::addObject(const QString &type, const QString &group)
{
    if (!project())
        return;
    const auto before = project()->objects;
    auto after = before;
    SceneObject object;
    object.id = newId();
    object.name = type;
    int suffix = 2;
    while (std::ranges::find(after, object.name, &SceneObject::name) != after.end())
        object.name = type + '_' + QString::number(suffix++);
    object.type = type;
    object.group = group;
    after.append(object);
    pushSceneEdit(after, object.id, tr("Add %1").arg(object.name));
}

void ProjectStore::updateObject(const SceneObject &object, const QString &action)
{
    if (!project() || !this->object(object.id) || object.name.trimmed().isEmpty())
        return;
    const auto before = project()->objects;
    auto after = before;
    *std::ranges::find(after, object.id, &SceneObject::id) = object;
    pushSceneEdit(after, object.id, action);
}

void ProjectStore::deleteObject()
{
    if (!selectedObject())
        return;
    const auto before = project()->objects;
    auto after = before;
    after.removeIf([this](const SceneObject &object) { return object.id == selection_; });
    const auto next = after.isEmpty() ? QString() : after.first().id;
    pushSceneEdit(after, next, tr("Delete %1").arg(selectedObject()->name));
}
