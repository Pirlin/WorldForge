#include "State/ProjectStore.h"
#include "State/GlbImport.h"
#include "State/NativeComponents.h"
#include "State/StudioClip.h"
#include "State/WorldScene.h"
#include "State/PlayableStreet.h"
#include "Engine/Geometry/MeshTopology.h"

#include <QDir>
#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMatrix4x4>
#include <QQuaternion>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace {
namespace wf = worldforge;
constexpr int projectSchemaVersion = 6;
constexpr int maxBehaviorComponents = 16;

QDir sceneDirectory(const QDir &index,const QJsonObject &configuration)
{
    const auto path = configuration.value("location").toString();
    if (configuration.value("workspaceStorage").toBool() && QDir::isAbsolutePath(path))
        return QDir(QDir(path).filePath("Scenes"));
    return QDir(index.filePath("scenes"));
}

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString meshObjectName(const QList<SceneObject> &objects, const QString &name)
{
    QString result = name;
    for (int suffix = 2; std::ranges::find(objects,result,&SceneObject::name) != objects.end(); ++suffix)
        result = name + '_' + QString::number(suffix);
    return result;
}

MeshAsset independentAsset(MeshAsset asset, const QString &name)
{
    asset.id = newId();
    asset.name = name;
    asset.sourcePath.clear();
    asset.sourceHash.clear();
    if (!asset.clip)
        asset.studioSourceHash.clear();
    asset.locallyEdited = false;
    return asset;
}

QMatrix4x4 meshBasis(const SceneObject &object,bool includeLocation)
{
    QMatrix4x4 result;
    if (includeLocation)
        result.translate(object.position);
    result.rotate(QQuaternion::fromEulerAngles(object.rotation));
    result.scale(object.scale);
    return result;
}

void mapMesh(wf::Mesh &mesh,const QMatrix4x4 &matrix,bool changeNormals = true)
{
    for (auto &p : mesh.vertices) {
        const auto moved = matrix.map(QVector3D(p[0],p[1],p[2]));
        p = {moved.x(),moved.y(),moved.z()};
    }
    if (!changeNormals)
        return;
    const auto normals = matrix.normalMatrix();
    const bool flip = matrix.determinant() < 0;
    for (auto &face : mesh.faces) {
        if (flip)
            std::ranges::reverse(face.corners);
        for (auto &corner : face.corners) {
            QVector3D normal;
            for (int i = 0; i < 3; ++i)
                normal[i] = normals(i,0)*corner.normal[0]+normals(i,1)*corner.normal[1]+normals(i,2)*corner.normal[2];
            normal.normalize();
            corner.normal = {normal.x(),normal.y(),normal.z()};
        }
    }
}

wf::Mesh copyFaces(const wf::Mesh &source,const std::vector<bool> &selected,bool copySelected,
                   const std::set<wf::MeshEdge> &sourceSeams,std::set<wf::MeshEdge> &seams)
{
    wf::Mesh result;
    result.materials = source.materials;
    constexpr auto missing = std::numeric_limits<wf::VertexIndex>::max();
    std::vector<wf::VertexIndex> remap(source.vertices.size(),missing);
    for (std::size_t i = 0; i < source.faces.size(); ++i) {
        if (selected[i] != copySelected)
            continue;
        auto face = source.faces[i];
        for (auto &corner : face.corners) {
            auto &index = remap[corner.vertex];
            if (index == missing) {
                index = wf::VertexIndex(result.vertices.size());
                result.vertices.push_back(source.vertices[corner.vertex]);
            }
            corner.vertex = index;
        }
        result.faces.push_back(std::move(face));
    }
    if (!sourceSeams.empty()) {
        const wf::MeshTopology topology(result);
        for (const auto &edge : sourceSeams)
            if (remap[edge.first] != missing && remap[edge.second] != missing) {
                const wf::MeshEdge mapped = std::minmax(remap[edge.first],remap[edge.second]);
                if (topology.edgeFaces.contains(mapped))
                    seams.insert(mapped);
            }
    }
    return result;
}

bool sameMeshTopology(const wf::Mesh &before,const wf::Mesh &after)
{
    return before.vertices.size() == after.vertices.size()
        && std::ranges::equal(before.faces,after.faces,[](const auto &a,const auto &b) {
            return std::ranges::equal(a.corners,b.corners,std::ranges::equal_to{},
                &wf::MeshCorner::vertex,&wf::MeshCorner::vertex);
        });
}

std::map<wf::Point3,wf::VertexIndex> uniqueMeshPositions(const wf::Mesh &mesh)
{
    std::map<wf::Point3,wf::VertexIndex> unique;
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const auto [entry,inserted] = unique.emplace(mesh.vertices[i],wf::VertexIndex(i));
        if (!inserted)
            entry->second = wf::MissingVertex;
    }
    return unique;
}

std::set<wf::MeshEdge> remapSeams(const wf::Mesh &before,const wf::Mesh &after,const std::set<wf::MeshEdge> &seams)
{
    if (seams.empty() || sameMeshTopology(before,after))
        return seams;
    const wf::MeshTopology topology(after);
    const auto unique = uniqueMeshPositions(after);
    std::set<wf::MeshEdge> mapped;
    for (const auto &edge : seams) {
        const auto a = unique.find(before.vertices[edge.first]), b = unique.find(before.vertices[edge.second]);
        if (a == unique.end() || b == unique.end() || a->second == wf::MissingVertex || b->second == wf::MissingVertex)
            continue;
        const wf::MeshEdge next = std::minmax(a->second,b->second);
        if (topology.edgeFaces.contains(next))
            mapped.insert(next);
    }
    return mapped;
}

wf::UvPins remapUvPins(const wf::Mesh &before,const wf::Mesh &after,const wf::UvPins &pins)
{
    if (pins.empty())
        return {};
    const auto unique = uniqueMeshPositions(after);
    struct FacePins {
        std::vector<std::size_t> corners;
        bool ambiguous = false;
        std::optional<std::size_t> destination;
        std::size_t rotation = 0;
    };
    std::map<std::vector<wf::VertexIndex>,FacePins> faces;
    const auto canonical = [](std::vector<wf::VertexIndex> &loop) {
        const auto start = std::ranges::min_element(loop);
        const auto rotation = std::size_t(start-loop.begin());
        std::rotate(loop.begin(),start,loop.end());
        return rotation;
    };
    for (auto pin = pins.begin(); pin != pins.end();) {
        const auto face = pin->first;
        const auto end = pins.upper_bound({face,std::numeric_limits<std::size_t>::max()});
        std::vector<wf::VertexIndex> loop;
        for (const auto &corner : before.faces[face].corners) {
            const auto vertex = unique.find(before.vertices[corner.vertex]);
            loop.push_back(vertex == unique.end() ? wf::MissingVertex : vertex->second);
        }
        if (std::ranges::find(loop,wf::MissingVertex) == loop.end()) {
            const auto rotation = canonical(loop);
            auto &entry = faces[loop];
            entry.ambiguous |= !entry.corners.empty();
            for (auto i = pin; i != end; ++i)
                entry.corners.push_back((i->second+loop.size()-rotation)%loop.size());
        }
        pin = end;
    }
    if (faces.empty())
        return {};
    std::vector<wf::VertexIndex> loop;
    for (std::size_t index = 0; index < after.faces.size(); ++index) {
        loop.clear();
        for (const auto &corner : after.faces[index].corners)
            loop.push_back(corner.vertex);
        const auto rotation = canonical(loop);
        if (const auto face = faces.find(loop); face != faces.end()) {
            face->second.ambiguous |= face->second.destination.has_value();
            face->second.destination = index;
            face->second.rotation = rotation;
        }
    }
    wf::UvPins result;
    for (const auto &[loopKey,face] : faces)
        if (!face.ambiguous && face.destination)
            for (const auto corner : face.corners)
                result.emplace(*face.destination,(corner+face.rotation)%loopKey.size());
    return result;
}

MeshUvSet captureUvSet(const MeshAsset &asset)
{
    MeshUvSet result;
    result.seams = asset.uvSeams;
    result.pins = asset.uvPins;
    for (const auto &face : asset.mesh->faces)
        for (const auto &corner : face.corners)
            result.corners.push_back(corner.uv);
    return result;
}

void invalidateOtherUvSets(MeshAsset &asset)
{
    for (auto &set : asset.otherUvSets)
        set.needsUnwrap = true;
}

bool replaceStudioUvs(MeshAsset &target,const GlbUvLayers &layers,QString &error)
{
    if (layers.other.size() > 7) {
        error = QObject::tr("Studio publishing supports up to eight UV layers.");
        return false;
    }
    const auto active = layers.active.isEmpty() ? QStringLiteral("UV Map") : layers.active;
    auto previous = target.otherUvSets;
    previous.insert(target.activeUvSet,captureUvSet(target));
    previous.remove(active);
    for (auto &layout : previous)
        layout.needsUnwrap = true;
    for (auto it = layers.other.cbegin(); it != layers.other.cend(); ++it)
        previous.insert(it.key(),MeshUvSet{it.value()});
    if (previous.size() > 7) {
        error = QObject::tr("The published layers and retained layouts exceed eight UV layers. Keep their original names in Studio.");
        return false;
    }
    target.activeUvSet = active;
    target.otherUvSets = std::move(previous);
    target.uvSeams.clear();
    target.uvPins.clear();
    return true;
}

bool readUvPins(const QJsonValue &value,const wf::Mesh &mesh,bool stale,wf::UvPins &pins)
{
    if (value.isUndefined())
        return true;
    if (!value.isArray())
        return false;
    for (const auto &entry : value.toArray()) {
        const auto pair = entry.toArray();
        const int face = pair.at(0).toInt(-1), corner = pair.at(1).toInt(-1);
        if (pair.size() != 2 || face < 0 || face >= 2'000'000 || corner < 0 || corner >= 256
            || (!stale && (std::size_t(face) >= mesh.faces.size() || std::size_t(corner) >= mesh.faces[face].corners.size())))
            return false;
        pins.emplace(std::size_t(face),std::size_t(corner));
    }
    return true;
}

QJsonArray writeUvPins(const wf::UvPins &pins)
{
    QJsonArray rows;
    for (const auto &[face,corner] : pins)
        rows.append(QJsonArray{int(face),int(corner)});
    return rows;
}

SceneObject rollingTerrain(float size,int density)
{
    wf::Heightfield field;
    field.size = size;
    for (int y = 0; y < field.side; ++y)
        for (int x = 0; x < field.side; ++x) {
            const float u = float(x-16), v = float(y-16);
            field.samples[y*field.side+x] = 4.5f*(std::cos(u*0.19f)*std::cos(v*0.22f)-1)
                + 3.2f*std::sin(u*0.32f)*std::sin(v*0.16f)
                + 5.5f*std::sin(u*0.09f)*std::sin(u*0.09f)-0.8f;
        }
    SceneObject terrain;
    terrain.id = newId();
    terrain.name = QObject::tr("Rolling Hills");
    terrain.type = "Terrain";
    terrain.group = "Environment";
    terrain.details = writeHeightfield(field);
    terrain.details.insert("foliageDensity",density);
    terrain.details.insert("foliageSeed",42);
    return terrain;
}

void addPhysicsStarter(Project &project)
{
    const auto cube = [&](const QString &name,QVector3D position,QVector3D scale,
                          std::array<float,4> color,const QString &body,int shape) {
        auto mesh = wf::makeCube();
        mesh.materials.front().color = color;
        MeshAsset asset{newId(),name,{},{},false,QSharedPointer<wf::Mesh>::create(std::move(mesh)),{}};
        SceneObject object;
        object.id = newId();
        object.name = name;
        object.type = "Static Mesh";
        object.group = "Props";
        object.position = position;
        object.scale = scale;
        object.meshAsset = asset.id;
        object.details = {{"physics",body},{"colliderShape",shape},{"bounce",0.35}};
        project.assets.insert(object.meshAsset,std::move(asset));
        project.objects.append(object);
    };
    cube(QObject::tr("Test Floor"),{0,0,-0.25f},{14,14,0.5f},{0.29f,0.32f,0.40f,1},"Static",0);
    cube(QObject::tr("Test Platform"),{1,1,0.5f},{3,3,1},{0.40f,0.45f,0.55f,1},"Static",0);
    cube(QObject::tr("Box Test Body"),{1,1,4},{1.2f,1.2f,1.2f},{0.68f,0.36f,0.92f,1},"Dynamic",0);
    cube(QObject::tr("Sphere Collider Test"),{-2,1,3},{1,1,1},{0.94f,0.46f,0.18f,1},"Dynamic",1);
    cube(QObject::tr("Capsule Collider Test"),{0,-2,5},{0.8f,0.8f,1.6f},{0.18f,0.69f,0.68f,1},"Dynamic",2);
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

SceneObject readObject(const QJsonObject &row)
{
    SceneObject object;
    object.id = row.value("id").toString();
    object.name = row.value("name").toString();
    object.type = row.value("type").toString();
    object.group = row.value("group").toString();
    object.material = row.value("material").toString();
    object.position = readVector(row, "position");
    object.rotation = readVector(row, "rotation");
    object.scale = readVector(row, "scale", {1, 1, 1});
    object.visible = row.value("visible").toBool(true);
    object.details = row.value("details").toObject();
    object.meshAsset = row.value("meshAsset").toString();
    object.parentId = row.value("parent").toString();
    const auto offset = row.value("parentOffset").toArray();
    if (offset.size() == 16)
        for (int i = 0; i < 16; ++i)
            object.parentOffset(i/4,i%4) = float(offset[i].toDouble());
    return object;
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
    for (const auto &object : project.objects) {
        for (int i = 0; i < 16; ++i)
            if (!std::isfinite(object.parentOffset(i/4,i%4)))
                return std::nullopt;
        if (object.parentOffset.row(3) != QVector4D(0,0,0,1))
            return std::nullopt;
        snapshot.entities.push_back({objectGuid(id, object.id),
            object.parentId.isEmpty() ? std::optional<wf::Guid>{} : objectGuid(id,object.parentId),
            object.name.toStdString(), transform(object)});
    }
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
    // Detach changed links first so any valid final hierarchy is reachable on redo and undo.
    for (const auto &object : project.objects) {
        const auto found = std::ranges::find(next,object.id,&SceneObject::id);
        if (!object.parentId.isEmpty() && (found == next.end() || found->parentId != object.parentId)
            && !edit(wf::ReparentEntity{objectGuid(scene.projectId(),object.id),{}}))
            return std::nullopt;
    }
    for (const auto &object : next) {
        const auto *previous = oldObjects.value(object.id);
        if (!object.parentId.isEmpty() && (!previous || previous->parentId != object.parentId)
            && !edit(wf::ReparentEntity{objectGuid(scene.projectId(),object.id),objectGuid(scene.projectId(),object.parentId)}))
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

const QHash<QString, QHash<QString, SceneObject>> &demoPlaceholders()
{
    static const auto objects = [] {
        QHash<QString, QHash<QString, SceneObject>> result;
        const auto projects = QJsonDocument::fromJson(readFile(":/Data/demo.json")).object().value("projects").toArray();
        for (const auto &entry : projects) {
            const auto project = entry.toObject();
            auto &placeholders = result[project.value("id").toString()];
            for (const auto &row : project.value("objects").toArray()) {
                const auto object = readObject(row.toObject());
                if (object.meshAsset.isEmpty() && object.type != "Directional Light" && object.type != "Point Light"
                    && object.type != "Spot Light")
                    placeholders.insert(object.id, object);
            }
        }
        return result;
    }();
    return objects;
}

const SceneObject *originalDemo(const Project &project, const SceneObject &object)
{
    const auto group = demoPlaceholders().constFind(project.id);
    if (group == demoPlaceholders().cend())
        return nullptr;
    const auto original = group->constFind(object.id);
    return original == group->cend() ? nullptr : &*original;
}

bool isUntouchedDemo(const Project &project, const SceneObject &object)
{
    const auto *original = originalDemo(project,object);
    return original && object == *original;
}

QString previousManifestPath(const QString &path)
{
    const QFileInfo manifest(path);
    return manifest.dir().filePath(manifest.completeBaseName() + ".previous.wfproject");
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

void evaluateLod(MeshAsset &asset)
{
    asset.lodMesh.reset();
    if (!(asset.lodCell > 0))
        return;
    auto mesh = asset.evaluated();
    const auto error = wf::applyMeshModifier(mesh,{wf::ModifierKind::Decimate,true,0,2,asset.lodCell});
    const auto triangles = [](const wf::Mesh &value) {
        std::size_t count = 0;
        for (const auto &face : value.faces)
            wf::visitFaceTriangles(value,face,[&](const auto &) { ++count; });
        return count;
    };
    if (error.empty() && triangles(mesh) < triangles(asset.evaluated()))
        asset.lodMesh = QSharedPointer<wf::Mesh>::create(std::move(mesh));
}

bool usesModifierTarget(wf::ModifierKind kind)
{
    return kind == wf::ModifierKind::Shrinkwrap || kind == wf::ModifierKind::Boolean;
}

const wf::Mesh *modifierTarget(const QMap<QString,MeshAsset> &assets,const wf::MeshModifier &modifier)
{
    if (!usesModifierTarget(modifier.kind))
        return nullptr;
    const auto target = assets.constFind(QString::fromStdString(modifier.targetAsset));
    return target == assets.cend() ? nullptr : &target->evaluated();
}

bool evaluateModifiers(MeshAsset &asset,const QMap<QString,MeshAsset> &assets,QString &error)
{
    asset.evaluatedMesh.reset();
    if (std::ranges::none_of(asset.modifiers,&wf::MeshModifier::enabled)) {
        evaluateLod(asset);
        return true;
    }
    auto result = *asset.mesh;
    for (std::size_t i = 0; i < asset.modifiers.size(); ++i) {
        const auto &modifier = asset.modifiers[i];
        error = QString::fromStdString(wf::applyMeshModifier(result,modifier,modifierTarget(assets,modifier)));
        if (!error.isEmpty()) {
            error = QObject::tr("Modifier %1 failed. %2").arg(i+1).arg(error);
            return false;
        }
    }
    asset.evaluatedMesh = QSharedPointer<wf::Mesh>::create(std::move(result));
    evaluateLod(asset);
    return true;
}

bool evaluateProjectMeshes(QMap<QString,MeshAsset> &assets,const QMap<QString,MeshAsset> *before,QString &error,
                           const QString &authoritativeUvAsset = {})
{
    QSet<QString> visiting, finished, changed;
    const auto evaluate = [&](auto &&self,const QString &id,int depth) -> bool {
        if (finished.contains(id))
            return true;
        if (depth > 64 || visiting.contains(id)) {
            error = QObject::tr("The modifier targets form a cycle or exceed 64 dependency levels.");
            return false;
        }
        if (!assets.contains(id)) {
            error = QObject::tr("A modifier target mesh is missing.");
            return false;
        }
        visiting.insert(id);
        const auto asset = assets.value(id);
        const auto prior = before ? before->constFind(id) : QMap<QString,MeshAsset>::const_iterator{};
        if (asset.clip && (!asset.modifiers.empty() || asset.lodCell > 0
            || asset.clip->vertexCount != asset.mesh->vertices.size()
            || (before && prior != before->cend() && prior->clip == asset.clip && prior->mesh != asset.mesh
                && (prior->mesh->vertices != asset.mesh->vertices || !sameMeshTopology(*prior->mesh,*asset.mesh))))) {
            error = QObject::tr("%1 uses a Studio animation clip. Edit its geometry and deformation in Studio.").arg(asset.name);
            return false;
        }
        bool dirty = !before || prior == before->cend() || asset.mesh != prior->mesh
            || asset.modifiers != prior->modifiers || asset.lodCell != prior->lodCell;
        for (const auto &modifier : asset.modifiers) {
            if (!modifier.enabled || !usesModifierTarget(modifier.kind))
                continue;
            const auto target = QString::fromStdString(modifier.targetAsset);
            if (!self(self,target,depth+1))
                return false;
            dirty |= changed.contains(target);
        }
        if (dirty) {
            if (id != authoritativeUvAsset && before && prior != before->cend() && asset.mesh != prior->mesh
                && (!asset.otherUvSets.isEmpty() || !asset.uvPins.empty()) && !sameMeshTopology(*prior->mesh,*asset.mesh)) {
                auto &updated = assets[id];
                invalidateOtherUvSets(updated);
                updated.uvPins = remapUvPins(*prior->mesh,*asset.mesh,asset.uvPins);
            }
            if (!evaluateModifiers(assets[id],assets,error)) {
                error = QObject::tr("%1. %2").arg(asset.name,error);
                return false;
            }
            changed.insert(id);
        }
        visiting.remove(id);
        finished.insert(id);
        return true;
    };
    for (const auto &id : assets.keys())
        if (!evaluate(evaluate,id,0))
            return false;
    return true;
}

std::optional<MeshAsset> readAsset(const QJsonObject &row)
{
    MeshAsset asset;
    asset.id = row.value("id").toString();
    asset.name = row.value("name").toString();
    asset.sourcePath = row.value("sourcePath").toString();
    asset.sourceHash = row.value("sourceHash").toString();
    asset.studioSourceHash = row.value("studioSourceHash").toString();
    asset.revolveProfile = row.value("revolveProfile").toObject();
    asset.locallyEdited = row.value("locallyEdited").toBool();
    asset.lodCell = float(row.value("lodCell").toDouble());
    asset.lodDistance = float(row.value("lodDistance").toDouble(25));
    if (!std::isfinite(asset.lodCell) || asset.lodCell < 0 || !std::isfinite(asset.lodDistance) || asset.lodDistance <= 0)
        return std::nullopt;
    if (!wf::Guid::parse(asset.id.toStdString()) || asset.name.isEmpty())
        return std::nullopt;
    wf::Mesh mesh;
    mesh.materials.clear();
    for (const auto &value : row.value("materials").toArray()) {
        const auto material = value.toObject();
        auto &slot = mesh.materials.emplace_back(wf::MeshMaterial{material.value("name").toString().toStdString(),
            meshVector<4>(material.value("color")), float(material.value("metallic").toDouble()),
            float(material.value("roughness").toDouble())});
        if (material.contains("emissive"))
            slot.emissive = meshVector<3>(material.value("emissive"));
        slot.sharedMaterialId = material.value("sharedMaterial").toString().toStdString();
        const QString keys[]{"baseColorTexture","normalTexture","metallicRoughnessTexture","occlusionTexture","emissiveTexture"};
        const auto images = slot.previewTextures();
        for (unsigned i = 0; i < images.size(); ++i) {
            if (!material.contains(keys[i]))
                continue;
            const auto encoded = material.value(keys[i]).toString().toLatin1();
            if (encoded.size() > qsizetype((wf::MeshMaterial::maxTextureBytes+2)/3*4))
                return std::nullopt;
            const auto decoded = QByteArray::fromBase64Encoding(encoded,QByteArray::AbortOnBase64DecodingErrors);
            if (!decoded)
                return std::nullopt;
            images[i]->assign(decoded.decoded.begin(),decoded.decoded.end());
        }
    }
    for (const auto &value : row.value("vertices").toArray())
        mesh.vertices.push_back(meshVector<3>(value));
    for (const auto &value : row.value("faces").toArray()) {
        const auto fields = value.toObject();
        wf::MeshFace face;
        face.material = std::uint32_t(fields.value("material").toInt(-1));
        for (const auto &cornerField : fields.value("corners").toArray()) {
            const auto corner = cornerField.toArray();
            face.corners.push_back({wf::VertexIndex(corner.at(0).toInt(-1)), meshVector<2>(corner.at(1)),
                                    meshVector<3>(corner.at(2))});
        }
        mesh.faces.push_back(std::move(face));
    }
    if (!wf::validateMesh(mesh).empty())
        return std::nullopt;
    asset.mesh = QSharedPointer<wf::Mesh>::create(std::move(mesh));
    if (!readUvPins(row.value("uvPins"),*asset.mesh,false,asset.uvPins))
        return std::nullopt;
    const auto seamRows = row.value("uvSeams").toArray();
    if (!seamRows.isEmpty()) {
        const wf::MeshTopology topology(*asset.mesh);
        for (const auto &entry : seamRows) {
            const auto pair = entry.toArray();
            const wf::MeshEdge seam{wf::VertexIndex(pair.at(0).toInt(-1)),wf::VertexIndex(pair.at(1).toInt(-1))};
            if (!topology.edgeFaces.contains(seam))
                return std::nullopt;
            asset.uvSeams.insert(seam);
        }
    }
    if (row.contains("activeUvSet")) {
        asset.activeUvSet = row.value("activeUvSet").toString();
        const auto layers = row.value("otherUvSets").toArray();
        if (asset.activeUvSet.isEmpty() || asset.activeUvSet.size() > 64 || layers.size() > 7)
            return std::nullopt;
        const wf::MeshTopology topology(*asset.mesh);
        std::size_t expectedCorners = 0;
        for (const auto &face : asset.mesh->faces)
            expectedCorners += face.corners.size();
        for (const auto &entry : layers) {
            const auto layer = entry.toObject();
            const auto name = layer.value("name").toString();
            if (name.isEmpty() || name.size() > 64 || name == asset.activeUvSet || asset.otherUvSets.contains(name))
                return std::nullopt;
            MeshUvSet uvSet;
            uvSet.needsUnwrap = layer.value("needsUnwrap").toBool();
            if (!readUvPins(layer.value("pins"),*asset.mesh,uvSet.needsUnwrap,uvSet.pins))
                return std::nullopt;
            const auto values = layer.value("corners").toArray();
            if (!uvSet.needsUnwrap && std::size_t(values.size()) != expectedCorners)
                return std::nullopt;
            for (const auto &value : values) {
                const auto uv = meshVector<2>(value);
                if (!std::isfinite(uv[0]) || !std::isfinite(uv[1]))
                    return std::nullopt;
                uvSet.corners.push_back(uv);
            }
            for (const auto &edge : layer.value("seams").toArray()) {
                const auto pair = edge.toArray();
                const wf::MeshEdge seam{wf::VertexIndex(pair.at(0).toInt(-1)),wf::VertexIndex(pair.at(1).toInt(-1))};
                if (seam.first >= seam.second || (!uvSet.needsUnwrap && !topology.edgeFaces.contains(seam)))
                    return std::nullopt;
                uvSet.seams.insert(seam);
            }
            asset.otherUvSets.insert(name,std::move(uvSet));
        }
    }
    const auto stack = row.value("modifiers");
    if (stack.isArray()) {
        if (stack.toArray().size() > 32)
            return std::nullopt;
        for (const auto &entry : stack.toArray()) {
            const auto settings = entry.toObject();
            const int kind = settings.value("kind").toInt(-1), axis = settings.value("axis").toInt(-1);
            const int count = settings.value("count").toInt(-1), face = settings.value("face").toInt(-1);
            const int mirrorSide = settings.value("mirrorSide").toInt();
            const int booleanOperation = settings.value("booleanOperation").toInt();
            const auto offset = settings.contains("targetOffset") ? meshVector<3>(settings.value("targetOffset")) : wf::Point3{};
            const auto amount = settings.value("amount").toDouble(std::numeric_limits<double>::quiet_NaN());
            if (kind < 0 || kind > int(wf::ModifierKind::Boolean) || axis < 0 || axis > 2
                || count < 1 || count > 100 || face < 0 || face > 2'000'000 || mirrorSide < 0
                || mirrorSide > int(wf::MirrorSide::Negative) || booleanOperation < 0
                || booleanOperation > int(wf::BooleanOperation::Intersection)
                || !std::ranges::all_of(offset,[](float value) { return std::isfinite(value); }) || !std::isfinite(amount)
                || std::abs(amount) > std::numeric_limits<float>::max())
                return std::nullopt;
            asset.modifiers.push_back({wf::ModifierKind(kind),settings.value("enabled").toBool(true),
                unsigned(axis),unsigned(count),float(amount),std::size_t(face),
                settings.value("targetAsset").toString().toStdString(),wf::MirrorSide(mirrorSide),
                wf::BooleanOperation(booleanOperation),offset});
        }
    } else if (row.value("mirrorX").toBool()) {
        asset.modifiers.push_back({});
    }
    if (row.contains("meshClip")) {
        const auto encoded = row.value("meshClip").toString().toLatin1();
        if (encoded.size() > 16'001'024)
            return {};
        const auto decoded = QByteArray::fromBase64Encoding(encoded,QByteArray::AbortOnBase64DecodingErrors);
        auto clip = decoded ? readStudioClip(decoded.decoded) : std::nullopt;
        if (!clip || clip->vertexCount != asset.mesh->vertices.size())
            return {};
        asset.clip = QSharedPointer<wf::MeshClip>::create(std::move(*clip));
    }
    return asset;
}

std::optional<QSize> surfaceImageSize(const QByteArray &image,bool decode)
{
    if (image.isEmpty() || image.size() > qsizetype(wf::MeshMaterial::maxTextureBytes))
        return {};
    QBuffer data;
    data.setData(image);
    data.open(QIODevice::ReadOnly);
    QImageReader reader(&data);
    const auto format = reader.format();
    const auto size = reader.size();
    if ((format != "png" && format != "jpeg") || !size.isValid() || size.width() > 8192
        || size.height() > 8192 || qint64(size.width())*size.height() > 32*1024*1024
        || (decode && reader.read().isNull()))
        return {};
    return size;
}

bool validSurfaceName(const QString &name)
{
    return !name.trimmed().isEmpty() && name.size() <= 120;
}

bool validBehavior(const ProjectBehavior &behavior)
{
    return validSurfaceName(behavior.name) && behavior.name == behavior.name.trimmed()
        && int(behavior.event) >= 0 && int(behavior.event) <= 2
        && int(behavior.action) >= 0 && behavior.action <= wf::BehaviorRule::Action::LaunchUpward
        && (behavior.event == wf::BehaviorRule::Event::MissionCompleted) == !behavior.mission.isEmpty()
        && behavior.amount >= 0.1f && behavior.amount <= 15;
}

bool validBehaviorComponents(const QJsonValue &stored)
{
    if (stored.isUndefined())
        return true;
    if (!stored.isArray() || stored.toArray().size() > maxBehaviorComponents)
        return false;
    QSet<QString> assets;
    for (const auto &value : stored.toArray()) {
        const auto row = value.toObject();
        const auto id = row.value("asset").toString();
        const auto amount = row.value("amount");
        if (id.isEmpty() || assets.contains(id) || !row.value("enabled").isBool()
            || (row.contains("target") && !row.value("target").isString())
            || (!amount.isUndefined() && (!amount.isDouble() || !(amount.toDouble() >= 0.1 && amount.toDouble() <= 15))))
            return false;
        assets.insert(id);
    }
    return true;
}

bool validProjectMaterial(const ProjectMaterial &material,const SurfaceLibrary &library)
{
    const auto hasTexture = [&](const QString &id) { return id.isEmpty() || library.textures.contains(id); };
    return validSurfaceName(material.name)
        && hasTexture(material.textureId) && std::ranges::all_of(material.maps,hasTexture)
        && material.metallic >= 0 && material.metallic <= 1
        && material.roughness >= 0 && material.roughness <= 1
        && material.emission >= 0 && material.emission <= 1
        && std::ranges::all_of(material.tint,[](float channel) { return channel >= 0 && channel <= 1; });
}

wf::MeshMaterial resolveProjectMaterial(const QString &id,const ProjectMaterial &material,const SurfaceLibrary &library)
{
    wf::MeshMaterial resolved;
    resolved.name = material.name.toStdString();
    resolved.color = material.tint;
    resolved.sharedMaterialId = id.toStdString();
    const auto images = resolved.previewTextures();
    const std::array textureIds{material.textureId,material.maps[0],material.maps[1],material.maps[2],material.maps[3]};
    for (std::size_t i = 0; i < images.size(); ++i)
        if (const auto texture = library.textures.constFind(textureIds[i]); texture != library.textures.cend())
            images[i]->assign(texture->image.begin(),texture->image.end());
    resolved.metallic = material.metallic;
    resolved.roughness = material.roughness;
    if (!resolved.emissiveTexture.empty())
        resolved.emissive = {material.emission,material.emission,material.emission};
    return resolved;
}

bool resolveLibrarySlots(QMap<QString,MeshAsset> &assets,const SurfaceLibrary &library,QString &error)
{
    if (library.materials.isEmpty())
        return true;
    QMap<QString,wf::MeshMaterial> resolved;
    for (auto it = assets.begin(); it != assets.end(); ++it) {
        std::optional<wf::Mesh> replacement;
        for (std::size_t slot = 0; slot < it->mesh->materials.size(); ++slot) {
            const auto &old = it->mesh->materials[slot];
            const auto id = QString::fromStdString(old.sharedMaterialId);
            const auto entry = library.materials.constFind(id);
            if (entry == library.materials.cend())
                continue;
            auto material = resolved.constFind(id);
            if (material == resolved.cend())
                material = resolved.insert(id,resolveProjectMaterial(id,*entry,library));
            if (old == *material)
                continue;
            if (!replacement)
                replacement = *it->mesh;
            replacement->materials[slot] = *material;
        }
        if (replacement) {
            const auto invalid = QString::fromStdString(wf::validateMesh(*replacement));
            if (!invalid.isEmpty()) {
                error = QObject::tr("%1. %2").arg(it->name,invalid);
                return false;
            }
            it->mesh = QSharedPointer<wf::Mesh>::create(std::move(*replacement));
        }
    }
    return true;
}

std::optional<SurfaceLibrary> readSurfaceLibrary(const QJsonValue &value)
{
    SurfaceLibrary library;
    if (value.isUndefined())
        return library;
    if (!value.isObject())
        return {};
    const auto section = value.toObject();
    if (!section.value("textures").isArray() || !section.value("materials").isArray())
        return {};
    constexpr auto encodedLimit = qsizetype((wf::MeshMaterial::maxTextureBytes+2)/3*4);
    for (const auto &entry : section.value("textures").toArray()) {
        const auto row = entry.toObject();
        const auto id = row.value("id").toString();
        const auto name = row.value("name").toString();
        const auto encoded = row.value("image").toString().toLatin1();
        if (!wf::Guid::parse(id.toStdString()) || library.textures.contains(id)
            || !validSurfaceName(name) || encoded.size() > encodedLimit)
            return {};
        auto result = QByteArray::fromBase64Encoding(encoded,QByteArray::AbortOnBase64DecodingErrors);
        const auto size = result ? surfaceImageSize(result.decoded,false) : std::nullopt;
        if (!size)
            return {};
        library.textures.insert(id,{name,row.value("fileName").toString(),size->width(),size->height(),
            std::move(result.decoded),row.value("sourcePath").toString()});
    }
    for (const auto &entry : section.value("materials").toArray()) {
        const auto row = entry.toObject();
        const auto id = row.value("id").toString();
        ProjectMaterial material{row.value("name").toString(),row.value("textureId").toString(),meshVector<4>(row.value("tint"))};
        if (row.contains("maps")) {
            const auto maps = row.value("maps").toArray();
            if (maps.size() != qsizetype(material.maps.size()))
                return {};
            for (qsizetype i = 0; i < maps.size(); ++i) {
                if (!maps[i].isString())
                    return {};
                material.maps[i] = maps[i].toString();
            }
        }
        material.metallic = float(row.value("metallic").toDouble(material.metallic));
        material.roughness = float(row.value("roughness").toDouble(material.roughness));
        material.emission = float(row.value("emission").toDouble(material.emission));
        if (!wf::Guid::parse(id.toStdString()) || library.materials.contains(id) || !validProjectMaterial(material,library))
            return {};
        library.materials.insert(id,std::move(material));
    }
    return library;
}

QJsonObject writeSurfaceLibrary(const SurfaceLibrary &library)
{
    QJsonArray textures, materials;
    for (auto it = library.textures.cbegin(); it != library.textures.cend(); ++it)
        textures.append(QJsonObject{{"id",it.key()},{"name",it->name},{"fileName",it->fileName},{"sourcePath",it->sourcePath},
            {"image",QString::fromLatin1(it->image.toBase64())}});
    for (auto it = library.materials.cbegin(); it != library.materials.cend(); ++it) {
        QJsonArray maps;
        for (const auto &map : it->maps)
            maps.append(map);
        materials.append(QJsonObject{{"id",it.key()},{"name",it->name},{"textureId",it->textureId},{"tint",meshVector(it->tint)},
            {"maps",maps},{"metallic",it->metallic},{"roughness",it->roughness},{"emission",it->emission}});
    }
    return {{"textures",textures},{"materials",materials}};
}

QJsonObject writeAsset(const MeshAsset &asset,const SurfaceLibrary &library)
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
    for (const auto &material : asset.mesh->materials) {
        QJsonObject row{{"name", QString::fromStdString(material.name)}, {"color", meshVector(material.color)},
            {"metallic", material.metallic}, {"roughness", material.roughness}};
        const QString keys[]{"baseColorTexture","normalTexture","metallicRoughnessTexture","occlusionTexture","emissiveTexture"};
        const auto images = material.previewTextures();
        for (unsigned i = 0; i < images.size(); ++i)
            if (!images[i]->empty() && !library.materials.contains(QString::fromStdString(material.sharedMaterialId)))
                row.insert(keys[i],QString::fromLatin1(QByteArrayView(
                    reinterpret_cast<const char *>(images[i]->data()),images[i]->size()).toByteArray().toBase64()));
        if (material.emissive != wf::Point3{})
            row.insert("emissive",meshVector(material.emissive));
        if (!material.sharedMaterialId.empty())
            row.insert("sharedMaterial",QString::fromStdString(material.sharedMaterialId));
        materials.append(row);
    }
    QJsonArray modifiers;
    for (const auto &item : asset.modifiers)
        modifiers.append(QJsonObject{{"kind",int(item.kind)},{"enabled",item.enabled},{"axis",int(item.axis)},
            {"count",int(item.count)},{"amount",item.amount},{"face",int(item.face)},
            {"targetAsset",QString::fromStdString(item.targetAsset)},{"mirrorSide",int(item.mirrorSide)},
            {"booleanOperation",int(item.booleanOperation)},{"targetOffset",meshVector(item.targetOffset)}});
    QJsonArray seams;
    for (const auto &edge : asset.uvSeams)
        seams.append(QJsonArray{int(edge.first),int(edge.second)});
    QJsonArray uvSets;
    for (auto it = asset.otherUvSets.cbegin(); it != asset.otherUvSets.cend(); ++it) {
        QJsonArray coordinates, boundaries;
        for (const auto &uv : it->corners)
            coordinates.append(meshVector(uv));
        for (const auto &edge : it->seams)
            boundaries.append(QJsonArray{int(edge.first),int(edge.second)});
        uvSets.append(QJsonObject{{"name",it.key()},{"corners",coordinates},{"seams",boundaries},
            {"needsUnwrap",it->needsUnwrap},{"pins",writeUvPins(it->pins)}});
    }
    const bool legacyMirror = asset.modifiers.size() == 1 && asset.modifiers.front().kind == wf::ModifierKind::Mirror
        && asset.modifiers.front().enabled && asset.modifiers.front().axis == 0;
    QJsonObject row{{"id", asset.id}, {"name", asset.name}, {"sourcePath", asset.sourcePath}, {"sourceHash", asset.sourceHash},
            {"studioSourceHash", asset.studioSourceHash},
            {"locallyEdited", asset.locallyEdited}, {"mirrorX", legacyMirror},{"modifiers",modifiers},{"uvSeams",seams},
            {"activeUvSet",asset.activeUvSet},{"otherUvSets",uvSets},{"uvPins",writeUvPins(asset.uvPins)},
            {"lodCell",asset.lodCell},{"lodDistance",asset.lodDistance},
            {"vertices", vertices}, {"faces", faces}, {"materials", materials},{"revolveProfile",asset.revolveProfile}};
    if (asset.clip)
        row.insert("meshClip",QString::fromLatin1(writeStudioClip(*asset.clip).toBase64()));
    return row;
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
    bool newerSchema = false;
    if (hadManifest && load(path_, &newerSchema)) {
        baselineManifest_ = persistedManifest_;
        return;
    }
    const auto previous = previousManifestPath(path_);
    if (!newerSchema && load(previous)) {
        loadError_ = tr("Opened the previous complete save. The latest project or scene file could not be read.");
        return;
    }
    const auto legacy = dir.filePath("projects.json");
    if (!hadManifest && load(legacy))
        return;
    if (hadManifest || QFile::exists(legacy)) {
        const auto recovered = dir.entryInfoList({"projects-recovered-*.wfproject"}, QDir::Files, QDir::Time);
        for (const auto &candidate : recovered) {
            if (candidate.completeBaseName().endsWith(".previous"))
                continue;
            bool newerRecovery = false;
            if (!load(candidate.absoluteFilePath(), &newerRecovery)
                && (newerRecovery || !load(previousManifestPath(candidate.absoluteFilePath()))))
                continue;
            path_ = candidate.absoluteFilePath();
            baselineManifest_ = readFile(path_);
            loadError_ = tr("Opened recovered projects from %1.").arg(candidate.fileName());
            return;
        }
        path_ = dir.filePath("projects-recovered-" + newId() + ".wfproject");
        baselineManifest_.clear();
        bool openedBackup = load(previous) || load(legacy);
        if (!openedBackup) {
            for (const auto &candidate : dir.entryInfoList({"projects-recovered*.json"}, QDir::Files, QDir::Time)) {
                if (load(candidate.absoluteFilePath())) {
                    openedBackup = true;
                    break;
                }
            }
        }
        persistedManifest_.clear();
        loadError_ = tr("%1 %2 New changes will go to %3.")
            .arg(newerSchema ? tr("The saved project uses a newer schema and was preserved.")
                             : tr("The saved project files could not be read."),
                 openedBackup ? tr("Opened backup project data.") : tr("Create a project to continue."), QFileInfo(path_).fileName());
        if (openedBackup)
            return;
    }
}

bool ProjectStore::load(const QString &fileName, bool *newerSchema)
{
    const auto bytes = readFile(fileName);
    const auto document = QJsonDocument::fromJson(bytes);
    const auto data = document.object();
    if (!data.value("projects").isArray())
        return false;
    const bool versioned = fileName.endsWith(".wfproject");
    const int projectVersion = data.value("schemaVersion").toInt();
    if (versioned && (projectVersion < 1 || projectVersion > projectSchemaVersion)) {
        if (newerSchema)
            *newerSchema = projectVersion > projectSchemaVersion;
        return false;
    }

    QList<Project> loaded;
    QSet<QString> projectIds;
    for (const auto &entry : data.value("projects").toArray()) {
        const auto fields = entry.toObject();
        Project project{fields.value("id").toString(), fields.value("name").toString(), {}, {}, {},
                        fields.value("configuration").toObject()};
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
            const auto sceneBytes = readFile(sceneDirectory(QFileInfo(fileName).dir(),project.configuration).filePath(sceneFile));
            if (sceneBytes.isEmpty() || checksum(sceneBytes) != fields.value("sceneHash").toString())
                return false;
            scene = QJsonDocument::fromJson(sceneBytes).object();
            bool validRevision = false;
            revision = scene.value("revision").toString().toULongLong(&validRevision);
            const int sceneVersion = scene.value("schemaVersion").toInt();
            if (sceneVersion != int(wf::sceneSchemaVersion)) {
                if (newerSchema)
                    *newerSchema = sceneVersion > int(wf::sceneSchemaVersion);
                return false;
            }
            if (!validRevision
                || scene.value("projectGuid").toString() != guidText(id)
                || scene.value("sceneGuid").toString() != guidText(sceneGuid(id)))
                return false;
        }
        if (!scene.value("objects").isArray())
            return false;
        for (const auto &assetField : scene.value("meshAssets").toArray()) {
            const auto asset = readAsset(assetField.toObject());
            if (!asset || project.assets.contains(asset->id))
                return false;
            project.assets.insert(asset->id, *asset);
        }
        const auto library = readSurfaceLibrary(scene.value("surfaceLibrary"));
        if (!library)
            return false;
        project.surfaces = *library;
        const auto behaviors = scene.value("behaviors");
        if (!behaviors.isUndefined() && (!behaviors.isObject() || behaviors.toObject().size() > 256))
            return false;
        const auto definitions = behaviors.toObject();
        for (auto it = definitions.constBegin(); it != definitions.constEnd(); ++it) {
            const auto row = it.value().toObject();
            ProjectBehavior behavior{row.value("name").toString(),wf::BehaviorRule::Event(row.value("event").toInt(-1)),
                wf::BehaviorRule::Action(row.value("action").toInt(-1)),row.value("mission").toString(),
                row.value("requiredMission").toString(),float(row.value("amount").toDouble(0))};
            if (!wf::Guid::parse(it.key().toStdString()) || !validBehavior(behavior))
                return false;
            project.behaviors.insert(it.key(),std::move(behavior));
        }
        QString modifierError;
        if (!resolveLibrarySlots(project.assets,project.surfaces,modifierError)
            || !evaluateProjectMeshes(project.assets,nullptr,modifierError))
            return false;
        QSet<QString> objectIds;
        for (const auto &value : scene.value("objects").toArray()) {
            const auto row = value.toObject();
            const auto object = readObject(row);
            if (object.id.isEmpty() || objectIds.contains(object.id) || object.name.isEmpty())
                return false;
            if (versioned && row.value("guid").toString() != guidText(objectGuid(id, object.id)))
                return false;
            objectIds.insert(object.id);
            if (!object.meshAsset.isEmpty() && !project.assets.contains(object.meshAsset))
                return false;
            if (!validBehaviorComponents(object.details.value("behaviorComponents")))
                return false;
            if (!validNativeComponentData(object.details.value("nativeComponents")))
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
    if (const auto *selected = object(selection_); selected && isLegacyPlaceholder(*selected)) {
        const auto &objects = project()->objects;
        const auto mesh = std::ranges::find_if(objects, [](const SceneObject &object) { return !object.meshAsset.isEmpty(); });
        selection_ = mesh == objects.cend() ? QString() : mesh->id;
    }
    persistedManifest_ = versioned ? bytes : QByteArray();
    return true;
}

bool ProjectStore::save(QString *error)
{
    QString message;
    const auto dir = QFileInfo(path_).dir();
    if (!QDir().mkpath(dir.absolutePath())) {
        *error = tr("Could not create the local project directory.");
        return false;
    }
    QLockFile lock(path_ + ".lock");
    if (!lock.tryLock() || readFile(path_) != baselineManifest_) {
        *error = tr("The project files changed in another editor. Reopen the project before saving.");
        return false;
    }

    const auto generation = newId();
    const auto previousPath = previousManifestPath(path_);
    const auto older = QJsonDocument::fromJson(readFile(previousPath)).object().value("projects").toArray();
    QJsonArray projects;
    QStringList staged;
    auto discard = qScopeGuard([&] {
        for (const auto &file : staged)
            QFile::remove(file);
    });
    for (const auto &project : projects_) {
        const auto &scene = *project.scene;
        QJsonArray objects, assets;
        for (const auto &asset : project.assets)
            assets.append(writeAsset(asset,project.surfaces));
        for (const auto &object : project.objects) {
            QJsonObject row{{"id", object.id}, {"name", object.name}, {"type", object.type},
                {"guid", guidText(objectGuid(scene.projectId(), object.id))},
                {"group", object.group}, {"material", object.material}, {"position", writeVector(object.position)},
                {"rotation", writeVector(object.rotation)}, {"scale", writeVector(object.scale)}, {"visible", object.visible},
                {"details", object.details}, {"meshAsset", object.meshAsset}};
            if (!object.parentId.isEmpty())
                row.insert("parent",object.parentId);
            if (!object.parentOffset.isIdentity()) {
                QJsonArray parentOffset;
                for (int i = 0; i < 16; ++i)
                    parentOffset.append(object.parentOffset(i/4,i%4));
                row.insert("parentOffset",parentOffset);
            }
            objects.append(row);
        }
        const auto sceneFile = guidText(scene.sceneId()) + '-' + generation + ".wfscene";
        QJsonObject content{{"schemaVersion", int(wf::sceneSchemaVersion)},
            {"projectGuid", guidText(scene.projectId())}, {"sceneGuid", guidText(scene.sceneId())},
            {"revision", QString::number(scene.revision())}, {"objects", objects}, {"meshAssets", assets}};
        if (!project.surfaces.empty())
            content.insert("surfaceLibrary",writeSurfaceLibrary(project.surfaces));
        if (!project.behaviors.isEmpty()) {
            QJsonObject behaviors;
            for (auto it = project.behaviors.cbegin(); it != project.behaviors.cend(); ++it)
                behaviors.insert(it.key(),QJsonObject{{"name",it->name},{"event",int(it->event)},{"action",int(it->action)},
                    {"mission",it->mission},{"requiredMission",it->requiredMission},{"amount",it->amount}});
            content.insert("behaviors",behaviors);
        }
        const auto sceneBytes = QJsonDocument(content).toJson(QJsonDocument::Compact);
        const auto sceneDir = sceneDirectory(dir,project.configuration);
        if (!QDir().mkpath(sceneDir.absolutePath())) {
            *error = tr("Could not create the scene directory for %1.").arg(project.name);
            return false;
        }
        const auto scenePath = sceneDir.filePath(sceneFile);
        if (!atomicWrite(scenePath, sceneBytes, message)) {
            *error = message;
            return false;
        }
        staged.append(scenePath);
        projects.append(QJsonObject{{"id", project.id}, {"name", project.name}, {"sceneFile", sceneFile},
                                    {"sceneHash", checksum(sceneBytes)}, {"configuration",project.configuration}});
    }
    int version = 1;
    for (const auto &item : projects_) {
        if (std::ranges::any_of(item.objects,[](const SceneObject &object) {
                return object.details.contains("nativeComponents");
            })) {
            version = projectSchemaVersion;
            break;
        }
        if (std::ranges::any_of(item.objects,[](const SceneObject &object) {
                return object.details.contains("behaviorComponents");
            })) {
            version = std::max(version,5);
        }
        if (std::ranges::any_of(item.behaviors,[](const ProjectBehavior &behavior) {
                return behavior.action == wf::BehaviorRule::Action::LaunchUpward;
            })) {
            version = std::max(version,4);
        }
        if (!item.behaviors.isEmpty())
            version = std::max(version,3);
        else if (!item.surfaces.empty())
            version = std::max(version,2);
    }
    const auto bytes = QJsonDocument(QJsonObject{{"schemaVersion", version},
        {"activeProject", activeId_}, {"selection", selection_}, {"projects", projects}}).toJson();
    if ((!persistedManifest_.isEmpty() && !atomicWrite(previousPath, persistedManifest_, message))
        || !atomicWrite(path_, bytes, message)) {
        *error = message;
        return false;
    }
    discard.dismiss();
    QSet<QString> retained;
    for (const auto &entry : QJsonDocument::fromJson(persistedManifest_).object().value("projects").toArray())
        retained.insert(entry.toObject().value("sceneFile").toString());
    static const QRegularExpression ownedFile("^[a-f0-9-]{36}-[a-f0-9-]{36}\\.wfscene$");
    for (const auto &entry : older) {
        const auto name = entry.toObject().value("sceneFile").toString();
        if (ownedFile.match(name).hasMatch() && !retained.contains(name)) {
            const auto configuration = entry.toObject().value("configuration").toObject();
            QFile::remove(sceneDirectory(dir,configuration).filePath(name));
        }
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

qsizetype ProjectStore::unusedDemoCount() const
{
    const auto *active = project();
    return active ? std::ranges::count_if(active->objects, [active](const SceneObject &object) {
        return isUntouchedDemo(*active, object);
    }) : 0;
}

bool ProjectStore::isLegacyPlaceholder(const SceneObject &object) const
{
    const auto *active = project();
    const auto *original = active ? originalDemo(*active,object) : nullptr;
    return original && object.type == original->type && object.meshAsset.isEmpty();
}

void ProjectStore::removeUnusedDemoObjects()
{
    const auto *active = project();
    if (!active)
        return;
    auto objects = active->objects;
    objects.removeIf([active](const SceneObject &object) { return isUntouchedDemo(*active, object); });
    pushSceneEdit(objects, selection_, tr("Remove unused demo objects"));
}

QString ProjectStore::nativeGameplayPath() const
{
    if (!project())
        return {};
    const auto key = QCryptographicHash::hash(project()->id.toUtf8(),QCryptographicHash::Sha256).toHex();
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath("NativeGameplay/"+QString::fromLatin1(key)+"/Gameplay.dll");
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
    if (const auto *selected = object(selection_); !selected || isLegacyPlaceholder(*selected))
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

bool ProjectStore::createProject(const QString &name, const QJsonObject &configuration)
{
    if (!validName(name))
        return false;
    const auto id = newId();
    Project created{id, name.trimmed(), {}, {}, {}, configuration};
    created.scene = openScene(created, 0);
    projects_.append(std::move(created));
    activateProject(id);
    return true;
}

bool ProjectStore::createStarterProject(const QString &name,QJsonObject configuration,QString &error)
{
    const auto title = name.trimmed();
    static const QRegularExpression forbidden(R"re([<>:"/\\|?*\x00-\x1f]|\.$)re");
    static const QRegularExpression reserved(R"(^(CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³])(?:\.|$))",
                                               QRegularExpression::CaseInsensitiveOption);
    if (!validName(title) || forbidden.match(title).hasMatch() || reserved.match(title).hasMatch()) {
        error = tr("Enter a unique project name that is also a valid Windows folder name.");
        return false;
    }
    const auto location = QDir::cleanPath(configuration.value("location").toString());
    const QDir folder(location);
    if (!QDir::isAbsolutePath(location) || QFileInfo(location).isSymLink()
        || (folder.exists() && !folder.isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))) {
        error = tr("Choose a new or empty project folder.");
        return false;
    }
    const auto starter = configuration.value("starter").toString();
    if (starter != "blank" && starter != "terrain" && starter != "physics" && starter != "street") {
        error = tr("Choose an available starter template.");
        return false;
    }
    const bool folderExisted = folder.exists();
    if (!QDir().mkpath(location)) {
        error = tr("Could not create the selected project folder.");
        return false;
    }
    auto discardFolder = qScopeGuard([&] {
        QDir(location).rmdir("Scenes");
        if (!folderExisted)
            QDir().rmdir(location);
    });
    configuration.insert("location",location);
    configuration.insert("workspaceStorage",true);
    Project created{newId(),title,{},{},{},configuration};
    if (starter == "terrain")
        created.objects.append(rollingTerrain(float(std::clamp(configuration.value("terrainWidth").toInt(128),32,512)),
                                              std::clamp(configuration.value("trees").toInt(14),0,48)));
    if (starter == "physics")
        addPhysicsStarter(created);
    if (starter == "street")
        street::addStarter(created);
    const auto settings = configuration.value("worldDefaults").toObject();
    if (!settings.isEmpty()) {
        SceneObject atmosphere;
        atmosphere.id = newId();
        atmosphere.name = tr("World Atmosphere");
        atmosphere.type = "World Atmosphere";
        atmosphere.group = "Environment";
        atmosphere.details = settings;
        created.objects.append(atmosphere);
    }
    created.scene = openScene(created,0);
    if (!created.scene) {
        error = tr("Could not build the selected starter scene.");
        return false;
    }
    const auto previousId = activeId_, previousSelection = selection_;
    activeId_ = created.id;
    selection_ = created.objects.isEmpty() ? QString{} : created.objects.front().id;
    projects_.append(std::move(created));
    if (!save(&error)) {
        projects_.removeLast();
        activeId_ = previousId;
        selection_ = previousSelection;
        return false;
    }
    discardFolder.dismiss();
    history_.clear();
    emit projectsChanged();
    emit sceneChanged();
    emit selectionChanged();
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

bool ProjectStore::pushProjectEdit(const QList<SceneObject> &objects,const QMap<QString,MeshAsset> &assets,
                                  const QString &selection,const QString &label,QString *error,const SurfaceLibrary *surfaces,
                                  const QString &authoritativeUvAsset,const QMap<QString,ProjectBehavior> *behaviors)
{
    const auto *active = project();
    if (!active)
        return false;
    const auto library = surfaces ? *surfaces : active->surfaces;
    const auto rules = behaviors ? *behaviors : active->behaviors;
    if (active->objects == objects && active->assets == assets && active->surfaces == library && active->behaviors == rules)
        return true;
    if (!editedScene(*active, objects)) {
        const auto reason = tr("The scene rejected this edit. Check the name and transform values.");
        if (error)
            *error = reason;
        emit editRejected(reason);
        emit sceneChanged();
        return false;
    }
    auto evaluated = assets;
    QString reason;
    const bool resolved = (active->assets == assets && active->surfaces == library) || resolveLibrarySlots(evaluated,library,reason);
    if (!resolved || (active->assets != evaluated && !evaluateProjectMeshes(evaluated,&active->assets,reason,authoritativeUvAsset))) {
        if (error)
            *error = reason;
        emit editRejected(reason);
        return false;
    }
    const auto previous = active->objects;
    const auto previousAssets = active->assets;
    const auto previousLibrary = active->surfaces;
    const auto previousRules = active->behaviors;
    history_.push(new UndoSceneEdit(label,
        [this, objects, evaluated, selection, library, rules] { applyScene(objects, evaluated, selection, library,rules); },
        [this, previous, previousAssets, selection = selection_, previousLibrary, previousRules] {
            applyScene(previous, previousAssets, selection, previousLibrary,previousRules);
        }));
    return true;
}

bool ProjectStore::applyScene(const QList<SceneObject> &objects, const QMap<QString, MeshAsset> &assets,
                              const QString &selection,const SurfaceLibrary &surfaces,const QMap<QString,ProjectBehavior> &behaviors)
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
    match->surfaces = surfaces;
    match->behaviors = behaviors;
    selection_ = object(selection) ? selection : QString();
    projectSelections_.insert(activeId_, selection_);
    setDirty(true);
    emit sceneChanged();
    emit selectionChanged();
    return true;
}

void ProjectStore::addCube(bool half)
{
    auto mesh = wf::makeCube();
    if (half)
        for (auto &vertex : mesh.vertices)
            vertex[0] = (vertex[0] + 0.5f)*0.5f;
    QString error;
    addMesh(std::move(mesh),half ? tr("Half Cube") : tr("Cube"),error);
}

bool ProjectStore::addMeshPrimitive(wf::Primitive kind,wf::Point3 size,unsigned segments,unsigned rings,
                                    const QString &name,QString &error)
{
    auto mesh = wf::makePrimitive(kind,size,segments,rings);
    if (!mesh) {
        error = tr("These dimensions and segment counts do not produce a valid mesh.");
        return false;
    }
    return addMesh(std::move(*mesh),name,error);
}

bool ProjectStore::addRetopologyPatch(wf::Mesh mesh,QString &error)
{
    error = QString::fromStdString(wf::validateMesh(mesh));
    return error.isEmpty() && addMesh(std::move(mesh),tr("Retopology Patch"),error,selectedObject());
}

bool ProjectStore::addRevolvedMesh(wf::Mesh mesh,const QJsonObject &profile,QString &error)
{
    error = QString::fromStdString(wf::validateMesh(mesh));
    return error.isEmpty() && addMesh(std::move(mesh),tr("Revolved Profile"),error,nullptr,profile);
}

bool ProjectStore::updateRevolvedMesh(const QString &id,wf::Mesh mesh,const QJsonObject &profile,
    const wf::Mesh *expected,QString &error)
{
    const auto *asset = meshAsset(id);
    if (!asset || asset->mesh.data() != expected || asset->revolveProfile.isEmpty()) {
        error = tr("The generated mesh changed while its profile was open.");
        return false;
    }
    mesh.materials = asset->mesh->materials;
    if (sameMeshTopology(*asset->mesh,mesh))
        for (std::size_t i = 0; i < mesh.faces.size(); ++i)
            mesh.faces[i].material = asset->mesh->faces[i].material;
    error = QString::fromStdString(wf::validateMesh(mesh));
    if (!error.isEmpty())
        return false;
    if (*asset->mesh == mesh && asset->revolveProfile == profile)
        return true;
    auto assets = project()->assets;
    auto &updated = assets[id];
    updated.uvSeams = remapSeams(*asset->mesh,mesh,asset->uvSeams);
    updated.mesh = QSharedPointer<wf::Mesh>::create(std::move(mesh));
    updated.revolveProfile = profile;
    return pushProjectEdit(project()->objects,assets,selection_,tr("Edit Revolve Profile"),&error);
}

bool ProjectStore::addMesh(wf::Mesh mesh,const QString &name,QString &error,const SceneObject *basis,
                          const QJsonObject &profile,const QString &studioSourceHash,const StudioMeshData *studio)
{
    const auto *active = project();
    if (!active || name.trimmed().isEmpty()) {
        error = tr("Open a project and enter a mesh name.");
        return false;
    }
    auto assets = active->assets;
    auto objects = active->objects;
    MeshAsset asset{newId(),meshObjectName(objects,name.trimmed()),{},{},false,
                    QSharedPointer<wf::Mesh>::create(std::move(mesh)), {}};
    asset.revolveProfile = profile;
    asset.studioSourceHash = studioSourceHash;
    if (studio)
        asset.clip = studio->clip;
    if (studio && studio->uvs) {
        const auto &uvLayers = *studio->uvs;
        asset.activeUvSet = uvLayers.active.isEmpty() ? QStringLiteral("UV Map") : uvLayers.active;
        for (auto it = uvLayers.other.cbegin(); it != uvLayers.other.cend(); ++it)
            asset.otherUvSets.insert(it.key(),MeshUvSet{it.value()});
    }
    SceneObject object;
    object.id = newId();
    object.name = asset.name;
    object.type = "Static Mesh";
    object.group = "Props";
    if (basis) {
        object.position = basis->position;
        object.rotation = basis->rotation;
        object.scale = basis->scale;
        object.group = basis->group;
        object.parentId = basis->parentId;
        object.parentOffset = basis->parentOffset;
    }
    object.meshAsset = asset.id;
    assets.insert(asset.id, asset);
    objects.append(object);
    return pushProjectEdit(objects,assets,object.id,tr("Add %1").arg(name),&error);
}

void ProjectStore::addTerrain(bool rolling)
{
    if (!rolling) {
        addObject("Terrain", "Environment");
        return;
    }
    if (!project())
        return;
    const auto terrain = rollingTerrain(128,14);
    auto objects = project()->objects;
    objects.append(terrain);
    pushSceneEdit(objects,terrain.id,tr("Add Rolling Hills"));
}

const SceneObject *ProjectStore::environment() const
{
    if (const auto *active = project()) {
        const auto object = std::ranges::find(active->objects, QString("World Atmosphere"), &SceneObject::type);
        if (object != active->objects.end())
            return &*object;
    }
    return nullptr;
}

void ProjectStore::setAtmosphere(const QJsonObject &settings)
{
    const auto *active = project();
    if (!active)
        return;
    auto objects = active->objects;
    if (const auto *old = environment()) {
        auto updated = settings;
        for (const auto *key : {"behaviorAsset","behaviorComponents","nativeComponents"})
            if (old->details.contains(key))
                updated.insert(key,old->details.value(key));
        std::ranges::find(objects, old->id, &SceneObject::id)->details = updated;
    } else {
        SceneObject object;
        object.id = newId();
        object.name = tr("World Atmosphere");
        object.type = "World Atmosphere";
        object.group = "Environment";
        object.details = settings;
        objects.append(object);
    }
    pushSceneEdit(objects, selection_, tr("Edit world atmosphere"));
}

void ProjectStore::sculptTerrain(const QString &id, const QVector3D &point, float radius, float strength, bool smooth)
{
    const auto *current = object(id);
    if (!current || current->type != "Terrain")
        return;
    auto field = readHeightfield(*current);
    const auto local = point - current->position;
    if (field.sculpt(local.x(), local.y(), radius, strength, smooth)) {
        auto next = *current;
        next.details.insert("heightSamples", writeHeightfield(field).value("heightSamples"));
        updateObject(next, tr("Sculpt terrain"));
    }
}

void ProjectStore::paintTerrain(const QString &id,const QVector3D &point,float radius,int layer)
{
    const auto *object = this->object(id);
    if (!object || object->type != "Terrain" || layer < 0 || layer > 3)
        return;
    auto field = readHeightfield(*object);
    const auto local = point-object->position;
    if (!field.paint(local.x(),local.y(),radius,std::uint8_t(layer)))
        return;
    QJsonArray values;
    for (auto value : field.layers)
        values.append(int(value));
    auto next = *object;
    next.details.insert("terrainLayers",values);
    updateObject(next,tr("Paint terrain layer"));
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
    copy.name = meshObjectName(objects,asset->name);
    objects.append(copy);
    pushSceneEdit(objects, copy.id, tr("Add Mesh Instance"));
}

bool ProjectStore::duplicateMesh(QString &error)
{
    const auto *asset = selectedMesh();
    if (!asset) {
        error = tr("Select a mesh to duplicate.");
        return false;
    }
    auto objects = project()->objects;
    auto assets = project()->assets;
    auto copy = *selectedObject();
    copy.id = newId();
    copy.name = meshObjectName(objects,copy.name);
    copy.position += QVector3D(2,0,0);
    auto duplicate = independentAsset(*asset,copy.name);
    copy.meshAsset = duplicate.id;
    assets.insert(duplicate.id,std::move(duplicate));
    objects.append(copy);
    pushProjectEdit(objects,assets,copy.id,tr("Duplicate Mesh"));
    return object(copy.id) != nullptr;
}

bool ProjectStore::makeMeshUnique(QString &error)
{
    const auto *asset = selectedMesh();
    if (!asset) {
        error = tr("Select a mesh instance.");
        return false;
    }
    auto objects = project()->objects;
    if (std::ranges::count(objects,asset->id,&SceneObject::meshAsset) <= 1) {
        error = tr("This object already owns its mesh.");
        return false;
    }
    auto assets = project()->assets;
    auto instance = std::ranges::find(objects,selection_,&SceneObject::id);
    auto unique = independentAsset(*asset,instance->name);
    instance->meshAsset = unique.id;
    assets.insert(unique.id,std::move(unique));
    pushProjectEdit(objects,assets,selection_,tr("Make Mesh Unique"));
    return selectedMesh() && selectedMesh()->id == instance->meshAsset;
}

bool ProjectStore::bakeMeshTransform(bool centerOrigin,QString &error)
{
    const auto *asset = selectedMesh();
    if (!asset || !asset->modifiers.empty()) {
        error = asset ? tr("Apply or remove the modifiers before changing the mesh basis.") : tr("Select a mesh to edit.");
        return false;
    }
    auto objects = project()->objects;
    auto assets = project()->assets;
    auto instance = std::ranges::find(objects,selection_,&SceneObject::id);
    auto mesh = *asset->mesh;
    const auto basis = meshBasis(*instance,false);
    QMatrix4x4 operation = basis;
    if (centerOrigin) {
        QVector3D center;
        for (const auto &p : mesh.vertices)
            center += QVector3D(p[0],p[1],p[2]);
        center /= float(mesh.vertices.size());
        if (center.isNull())
            return true;
        instance->position += basis.mapVector(center);
        operation.setToIdentity();
        operation.translate(-center);
    } else {
        if (basis.isIdentity())
            return true;
        if (std::abs(basis.determinant()) < 1e-12f) {
            error = tr("Cannot apply a singular object scale.");
            return false;
        }
        instance->rotation = {};
        instance->scale = {1,1,1};
    }
    mapMesh(mesh,operation,!centerOrigin);
    error = QString::fromStdString(wf::validateMesh(mesh));
    if (!error.isEmpty())
        return false;
    auto edited = std::ranges::count(objects,asset->id,&SceneObject::meshAsset) > 1
        ? independentAsset(*asset,instance->name) : *asset;
    instance->meshAsset = edited.id;
    edited.mesh = QSharedPointer<wf::Mesh>::create(std::move(mesh));
    evaluateLod(edited);
    edited.locallyEdited = true;
    assets.insert(edited.id,std::move(edited));
    const auto expected = *instance;
    pushProjectEdit(objects,assets,selection_,centerOrigin ? tr("Center Mesh Origin") : tr("Apply Rotation and Scale"));
    return selectedObject() && *selectedObject() == expected;
}

bool ProjectStore::joinMeshes(const QStringList &objectIds,QString &error)
{
    const auto *active = selectedObject();
    if (!active || !selectedMesh()) {
        error = tr("Select the destination mesh.");
        return false;
    }
    const auto destinationId = active->id;
    QSet<QString> ids(objectIds.begin(),objectIds.end());
    ids.insert(destinationId);
    bool invertible = false;
    const auto inverse = worldMatrix(active->id).inverted(&invertible);
    if (ids.size() < 2 || !invertible) {
        error = tr("Choose two or more meshes and a destination with non-zero scale.");
        return false;
    }
    wf::Mesh joined;
    joined.materials.clear();
    auto objects = project()->objects;
    auto assets = project()->assets;
    for (const auto &object : objects) {
        if (!ids.remove(object.id))
            continue;
        const auto *asset = meshAsset(object.meshAsset);
        if (!asset) {
            error = tr("%1 does not reference an editable mesh.").arg(object.name);
            return false;
        }
        auto part = asset->evaluated();
        if (part.vertices.size()+joined.vertices.size() > 2'000'000 || part.faces.size()+joined.faces.size() > 2'000'000) {
            error = tr("Joined geometry exceeds the editor's mesh limit.");
            return false;
        }
        const auto transform = inverse*worldMatrix(object.id);
        if (std::abs(transform.determinant()) < 1e-12f) {
            error = tr("%1 has a singular transform.").arg(object.name);
            return false;
        }
        mapMesh(part,transform);
        const auto vertexStart = wf::VertexIndex(joined.vertices.size());
        const auto materialStart = std::uint32_t(joined.materials.size());
        joined.vertices.insert(joined.vertices.end(),part.vertices.begin(),part.vertices.end());
        joined.materials.insert(joined.materials.end(),part.materials.begin(),part.materials.end());
        for (auto &face : part.faces) {
            face.material += materialStart;
            for (auto &corner : face.corners)
                corner.vertex += vertexStart;
            joined.faces.push_back(std::move(face));
        }
    }
    if (!ids.isEmpty()) {
        error = tr("A selected mesh was removed before joining.");
        return false;
    }
    error = QString::fromStdString(wf::validateMesh(joined));
    if (!error.isEmpty())
        return false;
    const auto joinedId = newId();
    auto destination = std::ranges::find(objects,destinationId,&SceneObject::id);
    destination->meshAsset = joinedId;
    assets.insert(joinedId,{joinedId,destination->name,{},{},false,QSharedPointer<wf::Mesh>::create(std::move(joined)),{}});
    const QSet<QString> sources(objectIds.begin(),objectIds.end());
    for (auto &child : objects)
        if (child.parentId != destinationId && sources.contains(child.parentId)) {
            child.parentOffset = parentMatrix(child);
            child.parentId.clear();
        }
    objects.removeIf([&](const SceneObject &object) { return object.id != destinationId && sources.contains(object.id); });
    pushProjectEdit(objects,assets,destinationId,tr("Join Meshes"));
    return selectedMesh() && selectedMesh()->id == joinedId;
}

bool ProjectStore::separateMeshFaces(const std::vector<std::size_t> &faceIndices,QString &error)
{
    const auto *asset = selectedMesh();
    if (!asset || !asset->modifiers.empty()) {
        error = tr("Select faces on a mesh with an empty modifier stack.");
        return false;
    }
    const auto &source = *asset->mesh;
    std::vector<bool> selected(source.faces.size());
    for (const auto index : faceIndices) {
        if (index >= selected.size()) {
            error = tr("The face selection is no longer valid.");
            return false;
        }
        selected[index] = true;
    }
    const auto count = std::ranges::count(selected,true);
    if (!count || std::size_t(count) == selected.size()) {
        error = tr("Select some of the mesh's faces to separate.");
        return false;
    }
    auto objects = project()->objects;
    auto assets = project()->assets;
    auto original = std::ranges::find(objects,selection_,&SceneObject::id);
    auto remainder = std::ranges::count(objects,asset->id,&SceneObject::meshAsset) > 1
        ? independentAsset(*asset,original->name) : *asset;
    remainder.uvSeams.clear();
    remainder.mesh = QSharedPointer<wf::Mesh>::create(copyFaces(source,selected,false,asset->uvSeams,remainder.uvSeams));
    if (remainder.id != asset->id)
        remainder.uvPins = remapUvPins(source,*remainder.mesh,asset->uvPins);
    invalidateOtherUvSets(remainder);
    evaluateLod(remainder);
    remainder.locallyEdited = true;
    original->meshAsset = remainder.id;
    assets.insert(remainder.id,remainder);
    auto part = *original;
    part.id = newId();
    part.name = meshObjectName(objects,original->name + tr(" Part"));
    part.meshAsset = newId();
    part.details = {};
    std::set<wf::MeshEdge> partSeams;
    auto partMesh = copyFaces(source,selected,true,asset->uvSeams,partSeams);
    MeshAsset partAsset{part.meshAsset,part.name,{},{},false,
        QSharedPointer<wf::Mesh>::create(std::move(partMesh)),{},{},std::move(partSeams)};
    partAsset.uvPins = remapUvPins(source,*partAsset.mesh,asset->uvPins);
    assets.insert(part.meshAsset,std::move(partAsset));
    objects.append(part);
    pushProjectEdit(objects,assets,part.id,tr("Separate Selected Faces"));
    return object(part.id) != nullptr;
}

void ProjectStore::placeMesh(const QString &assetId)
{
    const auto *asset = meshAsset(assetId);
    if (!asset)
        return;
    auto objects = project()->objects;
    SceneObject object;
    object.id = newId();
    object.name = asset->name;
    object.type = "Static Mesh";
    object.group = "Props";
    object.meshAsset = assetId;
    int suffix = 2;
    while (std::ranges::find(objects, object.name, &SceneObject::name) != objects.end())
        object.name = asset->name + '_' + QString::number(suffix++);
    objects.append(object);
    pushSceneEdit(objects, object.id, tr("Place Mesh"));
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
    if (reimport) {
        const auto preview = previewMeshReimport(found->id,path,error);
        return preview && applyMeshReimport(*preview,replaceLocalEdits,error);
    }
    auto mesh = importGlb(bytes, error);
    if (!mesh)
        return false;
    auto assets = active->assets;
    auto objects = active->objects;
    MeshAsset asset{newId(),QFileInfo(path).completeBaseName().simplified().left(64), path, hash, false,
        QSharedPointer<wf::Mesh>::create(std::move(*mesh)), {}};
    if (asset.name.isEmpty())
        asset.name = tr("Imported Mesh");
    SceneObject object;
    object.id = newId();
    object.name = asset.name;
    object.type = "Static Mesh";
    object.group = "Props";
    object.meshAsset = asset.id;
    objects.append(object);
    assets.insert(asset.id, asset);
    return pushProjectEdit(objects,assets,object.id,tr("Import GLB"),&error);
}

std::optional<MeshReimport> ProjectStore::previewMeshReimport(const QString &id,const QString &fileName,QString &error) const
{
    const auto *current = meshAsset(id);
    const auto path = QFileInfo(fileName).canonicalFilePath();
    QFile input(path);
    constexpr qint64 limit = 64*1024*1024;
    if (!current || current->sourcePath.isEmpty() || path.isEmpty() || !input.open(QIODevice::ReadOnly) || input.size() > limit) {
        error = tr("Choose a readable GLB source up to 64 MiB for an imported mesh.");
        return {};
    }
    if (std::ranges::any_of(project()->assets,[&](const auto &other) { return other.id != id && other.sourcePath == path; })) {
        error = tr("That source is already tracked by another mesh asset.");
        return {};
    }
    const auto bytes = input.read(limit+1);
    auto mesh = importGlb(bytes,error);
    if (!mesh)
        return {};
    if (!preserveProjectMaterials(id,*mesh,error))
        return {};
    MeshAsset replacement{id,current->name,path,checksum(bytes),false,QSharedPointer<wf::Mesh>::create(std::move(*mesh)),{}};
    replacement.modifiers = current->modifiers;
    replacement.otherUvSets = current->otherUvSets;
    replacement.otherUvSets.remove(replacement.activeUvSet);
    if (current->activeUvSet != replacement.activeUvSet)
        replacement.otherUvSets.insert(current->activeUvSet,captureUvSet(*current));
    replacement.lodCell = current->lodCell;
    replacement.lodDistance = current->lodDistance;
    if (current->activeUvSet == replacement.activeUvSet) {
        replacement.uvSeams = remapSeams(*current->mesh,*replacement.mesh,current->uvSeams);
        replacement.uvPins = current->uvPins;
    } else if (const auto previous = current->otherUvSets.constFind(replacement.activeUvSet);
             previous != current->otherUvSets.cend() && !previous->needsUnwrap) {
        replacement.uvSeams = remapSeams(*current->mesh,*replacement.mesh,previous->seams);
        replacement.uvPins = previous->pins;
    }
    auto proposed = project()->assets;
    proposed.insert(id,replacement);
    if (!evaluateProjectMeshes(proposed,&project()->assets,error))
        return {};
    replacement.evaluatedMesh = proposed[id].evaluatedMesh;
    replacement.lodMesh = proposed[id].lodMesh;
    return MeshReimport{project()->id,*current,std::move(replacement)};
}

bool ProjectStore::applyMeshReimport(const MeshReimport &preview,bool replaceLocalEdits,QString &error)
{
    const auto *current = meshAsset(preview.before.id);
    if (!project() || project()->id != preview.projectId || !current || *current != preview.before) {
        error = tr("This mesh changed after preview. Review the updated asset before reimporting.");
        return false;
    }
    if (current->locallyEdited && !replaceLocalEdits) {
        error = tr("Confirm replacement of local mesh edits before reimporting.");
        return false;
    }
    if (!current->locallyEdited && current->sourcePath == preview.after.sourcePath
        && current->sourceHash == preview.after.sourceHash)
        return true;
    if (std::ranges::any_of(project()->assets,[&](const auto &other) {
        return other.id != current->id && other.sourcePath == preview.after.sourcePath;
    })) {
        error = tr("That source is now tracked by another mesh.");
        return false;
    }
    auto assets = project()->assets;
    assets.insert(current->id,preview.after);
    return pushProjectEdit(project()->objects,assets,selection_,tr("Reimport GLB"),&error);
}

QString ProjectStore::studioSourcePath(const MeshAsset &asset) const
{
    static const QRegularExpression hashPattern("^[a-f0-9]{64}$");
    const auto *active = project();
    if (!active || QUuid(active->id).isNull() || !hashPattern.match(asset.studioSourceHash).hasMatch())
        return {};
    const auto scenes = sceneDirectory(QFileInfo(path_).dir(),active->configuration);
    return scenes.filePath(QStringLiteral("studio-assets/%1/%2.wfasset").arg(active->id,asset.studioSourceHash));
}

bool ProjectStore::publishStudioMesh(const QString &assetId,const QByteArray &glb,
                                     const wf::Mesh *expected,QString &error,const QByteArray &studioSource,
                                     const QByteArray &uvManifest,const QByteArray &clipData)
{
    const auto *asset = meshAsset(assetId);
    if (!asset || !expected || asset->mesh.data() != expected) {
        error = tr("The target mesh changed. Reopen Studio from the current asset revision.");
        return false;
    }
    if (glb.size() > 64 * 1024 * 1024) {
        error = tr("Choose a GLB no larger than 64 MiB.");
        return false;
    }
    if (!asset->modifiers.empty() || (!asset->otherUvSets.isEmpty() && uvManifest.isEmpty())) {
        error = tr("Reopen the asset in an updated Studio session with its named UV layers. WorldForge modifiers must be applied before publishing.");
        return false;
    }
    StudioMeshData data;
    auto mesh = importStudioGlb(glb,uvManifest,clipData,data,error);
    if (!mesh || !preserveProjectMaterials(assetId,*mesh,error))
        return false;
    QString sourceHash;
    if (!studioSource.isEmpty() && !storeStudioSource(studioSource,sourceHash,error))
        return false;
    return updateMesh(assetId,std::move(*mesh),tr("Publish Studio Mesh"),error,expected,{},sourceHash,nullptr,&data);
}

QByteArray ProjectStore::exportStudioInput(const MeshAsset &asset,QString &error) const
{
    GlbUvLayers layers{asset.activeUvSet,{}};
    for (auto it = asset.otherUvSets.cbegin(); it != asset.otherUvSets.cend(); ++it)
        if (!it->needsUnwrap)
            layers.other.insert(it.key(),it->corners);
    return exportGlb(*asset.mesh,asset.name,error,true,layers);
}

bool ProjectStore::createStudioMesh(const QString &name,wf::Mesh mesh,const QByteArray &studioSource,QString &error,
                                    const StudioMeshData *studio)
{
    if (!project() || name.trimmed().isEmpty()) {
        error = tr("Open a project and enter a mesh name.");
        return false;
    }
    error = QString::fromStdString(wf::validateMesh(mesh));
    QString sourceHash;
    return error.isEmpty() && storeStudioSource(studioSource,sourceHash,error)
        && addMesh(std::move(mesh),name,error,nullptr,{},sourceHash,studio);
}

bool ProjectStore::storeStudioSource(const QByteArray &source,QString &hash,QString &error)
{
    if (!source.startsWith("PK\x03\x04") || source.size() > 128 * 1024 * 1024) {
        error = tr("The editable Studio package is invalid or exceeds 128 MiB.");
        return false;
    }
    hash = checksum(source);
    MeshAsset reference;
    reference.studioSourceHash = hash;
    const auto path = studioSourcePath(reference);
    if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath())) {
        error = tr("Could not create the Studio asset storage directory.");
        return false;
    }
    if (QFileInfo::exists(path)) {
        QFile existing(path);
        if (!existing.open(QIODevice::ReadOnly) || existing.size() != source.size()
            || existing.readAll() != source) {
            error = tr("An existing Studio asset revision is damaged.");
            return false;
        }
        return true;
    }
    return atomicWrite(path,source,error);
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

bool ProjectStore::updateMesh(const QString &id, wf::Mesh mesh, const QString &label, QString &error,
                              const wf::Mesh *expected,std::optional<std::set<wf::MeshEdge>> seams,
                              const QString &studioSourceHash,const SceneObject *placement,const StudioMeshData *studio)
{
    const auto *asset = meshAsset(id);
    if (!asset) {
        error = tr("The mesh is no longer available.");
        return false;
    }
    if (expected && asset->mesh.data() != expected) {
        error = tr("The mesh changed while the tool was active. Start the transform again.");
        return false;
    }
    error = QString::fromStdString(wf::validateMesh(mesh));
    if (!error.isEmpty())
        return false;
    if (placement && (placement->meshAsset != id || object(placement->id) || placement->name.trimmed().isEmpty())) {
        error = tr("The staged placement is invalid or already exists.");
        return false;
    }
    const auto &library = project()->surfaces;
    for (const auto &slot : mesh.materials) {
        const auto materialId = QString::fromStdString(slot.sharedMaterialId);
        const auto material = library.materials.constFind(materialId);
        if (material == library.materials.cend())
            continue;
        const auto old = std::ranges::find(asset->mesh->materials,slot.sharedMaterialId,&wf::MeshMaterial::sharedMaterialId);
        if ((old != asset->mesh->materials.end() && *old == slot) || slot == resolveProjectMaterial(materialId,*material,library))
            continue;
        error = tr("Edit the reusable material in its Material Details, or make this slot independent first.");
        return false;
    }
    const bool changed = *asset->mesh != mesh;
    const auto *uvLayers = studio && studio->uvs ? &*studio->uvs : nullptr;
    if (!uvLayers && (!studio || studio->clip == asset->clip) && !placement && !changed && (!seams || *seams == asset->uvSeams)
        && (studioSourceHash.isEmpty() || studioSourceHash == asset->studioSourceHash))
        return true;
    auto assets = project()->assets;
    auto &updated = assets[id];
    if (studio)
        updated.clip = studio->clip;
    if (uvLayers && !replaceStudioUvs(updated,*uvLayers,error))
        return false;
    const bool sourceChanged = !studioSourceHash.isEmpty() && studioSourceHash != updated.studioSourceHash;
    if (sourceChanged)
        updated.studioSourceHash = studioSourceHash;
    if (seams || !updated.uvSeams.empty()) {
        const wf::MeshTopology topology(mesh);
        if (seams) {
            if (std::ranges::any_of(*seams,[&](const auto &edge) { return !topology.edgeFaces.contains(edge); })) {
                error = tr("A UV seam refers to a missing edge.");
                return false;
            }
            updated.uvSeams = std::move(*seams);
        } else if (changed)
            updated.uvSeams = remapSeams(*asset->mesh,mesh,updated.uvSeams);
    }
    if (changed) {
        std::map<std::string,wf::MeshMaterial> sharedEdits;
        for (const auto &material : mesh.materials) {
            if (material.sharedMaterialId.empty())
                continue;
            const auto old = std::ranges::find(asset->mesh->materials,material.sharedMaterialId,&wf::MeshMaterial::sharedMaterialId);
            if (old != asset->mesh->materials.end() && *old != material)
                sharedEdits.insert_or_assign(material.sharedMaterialId,material);
        }
        if (!sharedEdits.empty()) {
            const auto synchronize = [&](auto &materials) {
                for (auto &slot : materials)
                    if (const auto edit = sharedEdits.find(slot.sharedMaterialId); edit != sharedEdits.end())
                        slot = edit->second;
            };
            synchronize(mesh.materials);
            error = QString::fromStdString(wf::validateMesh(mesh));
            if (!error.isEmpty())
                return false;
            for (auto it = assets.begin(); it != assets.end(); ++it) {
                if (it.key() == id || !std::ranges::any_of(it->mesh->materials,[&](const auto &slot) {
                    return sharedEdits.contains(slot.sharedMaterialId);
                }))
                    continue;
                auto copy = *it->mesh;
                synchronize(copy.materials);
                error = QString::fromStdString(wf::validateMesh(copy));
                if (!error.isEmpty())
                    return false;
                it->mesh = QSharedPointer<wf::Mesh>::create(std::move(copy));
                it->locallyEdited = true;
            }
        }
        const bool geometryChanged = asset->mesh->vertices != mesh.vertices
            || !std::ranges::equal(asset->mesh->faces,mesh.faces,std::ranges::equal_to{},
                &wf::MeshFace::corners,&wf::MeshFace::corners);
        if (geometryChanged)
            updated.revolveProfile = {};
        updated.mesh = QSharedPointer<wf::Mesh>::create(std::move(mesh));
    } else if (sourceChanged || uvLayers || (studio && studio->clip != asset->clip)) {
        updated.mesh = QSharedPointer<wf::Mesh>::create(std::move(mesh));
    }
    updated.locallyEdited = true;
    auto objects = project()->objects;
    if (placement)
        objects.append(*placement);
    return pushProjectEdit(objects,assets,placement ? placement->id : selection_,label,&error,nullptr,uvLayers ? id : QString{});
}

bool ProjectStore::publishStudioMeshWithPlacement(const QString &assetId,const wf::Mesh *expected,wf::Mesh mesh,
    const QByteArray &studioSource,const SceneObject &placement,const QString &label,QString &error)
{
    const auto *asset = meshAsset(assetId);
    if (!asset || !expected || asset->mesh.data() != expected) {
        error = tr("The target mesh changed while the preview was open. Plan the edit again.");
        return false;
    }
    if (!asset->modifiers.empty() || !asset->otherUvSets.isEmpty()) {
        error = tr("Studio publishing currently supports one UV set and no WorldForge modifiers.");
        return false;
    }
    if (!preserveProjectMaterials(assetId,mesh,error))
        return false;
    QString sourceHash;
    if (!studioSource.isEmpty() && !storeStudioSource(studioSource,sourceHash,error))
        return false;
    return updateMesh(assetId,std::move(mesh),label,error,expected,{},sourceHash,&placement);
}

bool ProjectStore::linkMaterialSlot(const QString &target,int targetSlot,const QString &source,int sourceSlot,QString &error)
{
    const auto *from = meshAsset(source);
    const auto *to = meshAsset(target);
    if (!from || !to || source == target || sourceSlot < 0 || targetSlot < 0
        || std::size_t(sourceSlot) >= from->mesh->materials.size() || std::size_t(targetSlot) > to->mesh->materials.size()) {
        error = tr("Choose valid material slots on two different mesh assets.");
        return false;
    }
    auto assets = project()->assets;
    auto material = from->mesh->materials[sourceSlot];
    if (material.sharedMaterialId.empty()) {
        material.sharedMaterialId = newId().toStdString();
        auto mesh = *from->mesh;
        mesh.materials[sourceSlot] = material;
        assets[source].mesh = QSharedPointer<wf::Mesh>::create(std::move(mesh));
        assets[source].locallyEdited = true;
    }
    auto mesh = *to->mesh;
    if (std::size_t(targetSlot) == mesh.materials.size())
        mesh.materials.push_back(std::move(material));
    else
        mesh.materials[targetSlot] = std::move(material);
    error = QString::fromStdString(wf::validateMesh(mesh));
    if (!error.isEmpty())
        return false;
    assets[target].mesh = QSharedPointer<wf::Mesh>::create(std::move(mesh));
    assets[target].locallyEdited = true;
    return pushProjectEdit(project()->objects,assets,selection_,tr("Link Material Slot"),&error);
}

QString ProjectStore::importProjectTexture(const QString &path,QString &error)
{
    const auto *active = project();
    QFile file(path);
    if (!active || !file.open(QIODevice::ReadOnly) || file.size() > qsizetype(wf::MeshMaterial::maxTextureBytes)) {
        error = tr("Open a project and select a readable PNG or JPEG up to 16 MiB.");
        return {};
    }
    auto bytes = file.read(qint64(wf::MeshMaterial::maxTextureBytes)+1);
    const auto size = surfaceImageSize(bytes,true);
    if (!size) {
        error = tr("Choose a valid PNG or JPEG no larger than 8192 pixels per side and 32 megapixels.");
        return {};
    }
    const QFileInfo info(path);
    const auto id = newId();
    auto library = active->surfaces;
    const auto name = info.completeBaseName().trimmed().left(120);
    library.textures.insert(id,{name.isEmpty() ? tr("Texture") : name,info.fileName(),
        size->width(),size->height(),std::move(bytes),info.canonicalFilePath()});
    return pushProjectEdit(active->objects,active->assets,selection_,tr("Import Project Texture"),&error,&library) ? id : QString{};
}

std::optional<TextureReimport> ProjectStore::previewTextureReimport(const QString &id,const QString &path,QString &error) const
{
    const auto *active = project();
    QFile file(path);
    if (!active || !active->surfaces.textures.contains(id) || !file.open(QIODevice::ReadOnly)
        || file.size() > qsizetype(wf::MeshMaterial::maxTextureBytes)) {
        error = tr("Choose a readable PNG or JPEG up to 16 MiB.");
        return {};
    }
    auto bytes = file.read(qint64(wf::MeshMaterial::maxTextureBytes)+1);
    const auto size = surfaceImageSize(bytes,true);
    if (!size) {
        error = tr("Choose a valid PNG or JPEG no larger than 8192 pixels per side and 32 megapixels.");
        return {};
    }
    const auto before = active->surfaces.textures.value(id);
    const QFileInfo info(path);
    return TextureReimport{active->id,id,before,{before.name,info.fileName(),size->width(),size->height(),
        std::move(bytes),info.canonicalFilePath()}};
}

bool ProjectStore::applyTextureReimport(const TextureReimport &preview,QString &error)
{
    const auto *active = project();
    if (!active || active->id != preview.projectId || !active->surfaces.textures.contains(preview.id)
        || active->surfaces.textures.value(preview.id) != preview.before) {
        error = tr("This texture changed after preview. Review the updated asset before reimporting.");
        return false;
    }
    auto library = active->surfaces;
    library.textures[preview.id] = preview.after;
    return pushProjectEdit(active->objects,active->assets,selection_,tr("Reimport Texture"),&error,&library);
}

QString ProjectStore::saveProjectMaterial(const QString &id,const ProjectMaterial &material,QString &error)
{
    const auto *active = project();
    if (!active || (!id.isEmpty() && !active->surfaces.materials.contains(id))
        || !validProjectMaterial(material,active->surfaces)) {
        error = tr("Enter a material name, use factors from 0 to 1, and choose textures from this project.");
        return {};
    }
    auto library = active->surfaces;
    const auto key = id.isEmpty() ? newId() : id;
    auto saved = material;
    saved.name = saved.name.trimmed();
    library.materials.insert(key,std::move(saved));
    return pushProjectEdit(active->objects,active->assets,selection_,tr("Save Project Material"),&error,&library) ? key : QString{};
}

bool ProjectStore::assignProjectMaterial(const QString &materialId,const QString &meshId,int slot,QString &error)
{
    const auto *active = project();
    const auto *asset = meshAsset(meshId);
    if (!active || !asset || !active->surfaces.materials.contains(materialId)
        || slot < 0 || std::size_t(slot) >= asset->mesh->materials.size()) {
        error = tr("Choose an existing project material and mesh slot.");
        return false;
    }
    auto candidate = *asset->mesh;
    candidate.materials[slot] = resolveProjectMaterial(materialId,active->surfaces.materials.value(materialId),active->surfaces);
    return updateMesh(meshId,std::move(candidate),tr("Assign Project Material"),error,asset->mesh.data());
}

bool ProjectStore::preserveProjectMaterials(const QString &meshId,wf::Mesh &mesh,QString &error) const
{
    const auto *asset = meshAsset(meshId);
    if (!asset)
        return false;
    for (const auto &slot : asset->mesh->materials) {
        if (!project()->surfaces.materials.contains(QString::fromStdString(slot.sharedMaterialId)))
            continue;
        const auto matches = [&slot](const auto &material) { return material.name == slot.name; };
        const auto original = std::ranges::find_if(asset->mesh->materials,[&](const auto &material) {
            return matches(material) && material.sharedMaterialId != slot.sharedMaterialId;
        });
        auto destination = std::ranges::find_if(mesh.materials,matches);
        if (original != asset->mesh->materials.end() || destination == mesh.materials.end()
            || std::ranges::count_if(mesh.materials,matches) != 1) {
            error = tr("Studio could not match linked material “%1”. Keep its name unique and unchanged, or make the slot independent.")
                .arg(QString::fromStdString(slot.name));
            return false;
        }
        *destination = slot;
    }
    return true;
}

bool ProjectStore::transformMeshComponents(const QMap<QString,std::set<int>> &selection,
    const QMatrix4x4 &worldTransform,QString &error)
{
    if (!project() || selection.size() < 2) {
        error = tr("Select components on at least two mesh objects.");
        return false;
    }
    auto assets = project()->assets;
    QSet<QString> edited;
    for (auto it = selection.cbegin(); it != selection.cend(); ++it) {
        const auto *instance = object(it.key());
        const auto *asset = instance ? meshAsset(instance->meshAsset) : nullptr;
        if (!asset || it->empty() || *it->begin() < 0 || std::size_t(*it->rbegin()) >= asset->mesh->vertices.size()) {
            error = tr("A mesh or component selection has changed.");
            return false;
        }
        if (edited.contains(asset->id)) {
            error = tr("Two selected objects share a mesh. Make one an independent copy before editing both.");
            return false;
        }
        edited.insert(asset->id);
        const auto basis = worldMatrix(instance->id);
        bool invertible = false;
        const auto inverse = basis.inverted(&invertible);
        if (!invertible) {
            error = tr("A selected object has a non-invertible transform.");
            return false;
        }
        const auto localTransform = inverse*worldTransform*basis;
        auto mesh = *asset->mesh;
        for (const auto vertex : *it) {
            const auto &p = mesh.vertices[vertex];
            const auto next = localTransform.map(QVector3D(p[0],p[1],p[2]));
            mesh.vertices[vertex] = {next.x(),next.y(),next.z()};
        }
        if (mesh.vertices == asset->mesh->vertices)
            continue;
        for (auto &face : mesh.faces)
            if (std::ranges::any_of(face.corners,[&](const auto &corner) { return it->contains(int(corner.vertex)); }))
                for (auto &corner : face.corners)
                    corner.normal = {};
        error = QString::fromStdString(wf::validateMesh(mesh));
        if (!error.isEmpty())
            return false;
        auto &updated = assets[asset->id];
        updated.mesh = QSharedPointer<wf::Mesh>::create(std::move(mesh));
        updated.revolveProfile = {};
        updated.locallyEdited = true;
    }
    return pushProjectEdit(project()->objects,assets,selection_,tr("Transform Components Across Meshes"),&error);
}

bool ProjectStore::setMirrorX(bool enabled, QString &error)
{
    const auto *asset = selectedMesh();
    if (!asset) {
        error = tr("Select a mesh in the Model Outliner.");
        return false;
    }
    auto modifiers = asset->modifiers;
    const auto match = [](const wf::MeshModifier &item) { return item.kind == wf::ModifierKind::Mirror && item.axis == 0; };
    if (enabled) {
        const auto found = std::ranges::find_if(modifiers,match);
        if (found == modifiers.end())
            modifiers.insert(modifiers.begin(),wf::MeshModifier{});
        else
            found->enabled = true;
    } else {
        std::erase_if(modifiers,match);
    }
    return setMeshModifiers(std::move(modifiers),error);
}

bool ProjectStore::setMeshModifiers(std::vector<wf::MeshModifier> modifiers,QString &error)
{
    const auto *asset = selectedMesh();
    if (!asset || modifiers.size() > 32) {
        error = tr("Select a mesh and use at most 32 modifiers.");
        return false;
    }
    if (asset->modifiers == modifiers)
        return true;
    auto assets = project()->assets;
    auto &updated = assets[asset->id];
    updated.modifiers = std::move(modifiers);
    return pushProjectEdit(project()->objects,assets,selection_,tr("Edit Modifier Stack"),&error);
}

bool ProjectStore::applyMeshModifiers(std::size_t through,QString &error)
{
    const auto *asset = selectedMesh();
    if (!asset || through >= asset->modifiers.size()) {
        error = tr("Select a modifier to apply.");
        return false;
    }
    auto assets = project()->assets;
    auto &updated = assets[asset->id];
    auto mesh = *updated.mesh;
    for (std::size_t i = 0; i <= through; ++i) {
        const auto &modifier = updated.modifiers[i];
        error = QString::fromStdString(wf::applyMeshModifier(mesh,modifier,modifierTarget(assets,modifier)));
        if (!error.isEmpty())
            return false;
    }
    updated.uvSeams = remapSeams(*asset->mesh,mesh,asset->uvSeams);
    updated.mesh = QSharedPointer<wf::Mesh>::create(std::move(mesh));
    updated.modifiers.erase(updated.modifiers.begin(),updated.modifiers.begin()+std::ptrdiff_t(through+1));
    updated.revolveProfile = {};
    updated.locallyEdited = true;
    return pushProjectEdit(project()->objects,assets,selection_,tr("Apply Modifiers"),&error);
}

bool ProjectStore::setMeshLod(float cell,float distance,QString &error)
{
    const auto *asset = selectedMesh();
    if (!asset || !std::isfinite(cell) || cell < 0 || !std::isfinite(distance) || distance <= 0) {
        error = tr("Select a mesh and enter valid LOD settings.");
        return false;
    }
    if (asset->lodCell == cell && asset->lodDistance == distance)
        return true;
    auto assets = project()->assets;
    auto &updated = assets[asset->id];
    updated.lodCell = cell;
    updated.lodDistance = distance;
    evaluateLod(updated);
    if (cell > 0 && !updated.lodMesh) {
        error = tr("This cell size did not produce a valid, smaller mesh.");
        return false;
    }
    return pushProjectEdit(project()->objects,assets,selection_,tr("Set Mesh LOD"),&error);
}

bool ProjectStore::selectMeshUvSet(const QString &name,bool create,bool rebuild,QString &error)
{
    const auto *asset = selectedMesh();
    const auto title = name.trimmed();
    if (!asset || title.isEmpty() || title.size() > 64) {
        error = tr("Select a mesh and enter a UV set name of up to 64 characters.");
        return false;
    }
    if (title == asset->activeUvSet && !create)
        return true;
    const auto layer = asset->otherUvSets.constFind(title);
    if (create && (title == asset->activeUvSet || layer != asset->otherUvSets.cend() || asset->otherUvSets.size() >= 7)) {
        error = tr("Use a unique name and at most eight UV sets.");
        return false;
    }
    if (!create && (layer == asset->otherUvSets.cend() || (layer->needsUnwrap && !rebuild))) {
        error = tr("The UV set is missing or needs to be rebuilt from the current mesh.");
        return false;
    }
    auto assets = project()->assets;
    auto &updated = assets[asset->id];
    updated.otherUvSets.insert(asset->activeUvSet,captureUvSet(*asset));
    if (!create && !layer->needsUnwrap) {
        auto mesh = *asset->mesh;
        std::size_t i = 0;
        for (auto &face : mesh.faces)
            for (auto &corner : face.corners)
                corner.uv = layer->corners.at(i++);
        updated.mesh = QSharedPointer<wf::Mesh>::create(std::move(mesh));
        updated.uvSeams = layer->seams;
        updated.uvPins = layer->pins;
    }
    updated.otherUvSets.remove(title);
    updated.activeUvSet = title;
    updated.locallyEdited = true;
    return pushProjectEdit(project()->objects,assets,selection_,create ? tr("Add UV Set") : tr("Switch UV Set"),&error);
}

bool ProjectStore::toggleMeshUvPin(wf::UvCorner corner,QString &error)
{
    const auto *asset = selectedMesh();
    if (!asset || corner.first >= asset->mesh->faces.size()
        || corner.second >= asset->mesh->faces[corner.first].corners.size()) {
        error = tr("Choose a valid UV corner.");
        return false;
    }
    auto assets = project()->assets;
    auto &updated = assets[asset->id];
    const bool pin = updated.uvPins.erase(corner) == 0;
    if (pin)
        updated.uvPins.insert(corner);
    updated.locallyEdited = true;
    return pushProjectEdit(project()->objects,assets,selection_,pin ? tr("Pin UV Corner") : tr("Unpin UV Corner"),&error);
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
    if (type == "Terrain")
        object.details = writeHeightfield({});
    after.append(object);
    pushSceneEdit(after, object.id, tr("Add %1").arg(object.name));
}

void ProjectStore::updateObject(const SceneObject &object, const QString &action)
{
    const auto *previous = this->object(object.id);
    if (!project() || !previous || object.name.trimmed().isEmpty() || previous->parentId != object.parentId)
        return;
    const auto before = project()->objects;
    auto after = before;
    *std::ranges::find(after, object.id, &SceneObject::id) = object;
    pushSceneEdit(after, object.id, action);
}

bool ProjectStore::updateObjects(const QList<SceneObject> &objects, const QString &action, QString &error)
{
    if (!project() || objects.isEmpty()) {
        error = tr("Open a project and choose the objects to change.");
        return false;
    }
    auto after = project()->objects;
    for (const auto &object : objects) {
        const auto match = std::ranges::find(after, object.id, &SceneObject::id);
        if (match == after.end() || object.name.trimmed().isEmpty() || match->parentId != object.parentId) {
            error = tr("%1 is no longer in the level.").arg(object.name);
            return false;
        }
        *match = object;
    }
    return pushProjectEdit(after, project()->assets, selection_, action, &error);
}

QString ProjectStore::saveBehavior(const QString &id,const ProjectBehavior &behavior,QString &error)
{
    const auto *active = project();
    if (!active || !validBehavior(behavior) || (!id.isEmpty() && !active->behaviors.contains(id))
        || (id.isEmpty() && active->behaviors.size() >= 256)) {
        error = tr("Choose a behavior name, event, and action. A project supports up to 256 behavior assets.");
        return {};
    }
    const auto validMission = [this](const QString &id) {
        const auto *target = object(id);
        return id.isEmpty() || (target && target->visible && target->details.value("gameplay").toString() == "missionStart");
    };
    if (!validMission(behavior.mission) || !validMission(behavior.requiredMission)) {
        error = tr("Choose a visible mission start for the event or condition.");
        return {};
    }
    auto rules = active->behaviors;
    const auto key = id.isEmpty() ? newId() : id;
    rules.insert(key,behavior);
    return pushProjectEdit(active->objects,active->assets,selection_,tr("Save Behavior"),&error,nullptr,{},&rules)
        ? key : QString{};
}

QList<BehaviorComponent> behaviorComponents(const SceneObject &object)
{
    QList<BehaviorComponent> result;
    if (object.details.contains("behaviorComponents")) {
        for (const auto &value : object.details.value("behaviorComponents").toArray()) {
            const auto row = value.toObject();
            BehaviorComponent component{row.value("asset").toString(),row.value("enabled").toBool(true),row.value("target").toString(),{}};
            if (row.contains("amount"))
                component.amount = float(row.value("amount").toDouble());
            result.append(component);
        }
    } else if (const auto asset = object.details.value("behaviorAsset").toString(); !asset.isEmpty()) {
        result.append({asset,true,{},{}});
    }
    return result;
}

bool ProjectStore::attachBehavior(const QString &objectId,const QString &behaviorId,QString &error)
{
    const auto *owner = object(objectId);
    if (!owner) {
        error = tr("Choose a level object and a behavior asset.");
        return false;
    }
    auto components = behaviorComponents(*owner);
    if (components.isEmpty()) {
        if (!behaviorId.isEmpty())
            components.append({behaviorId,true,{},{}});
    } else if (behaviorId.isEmpty()) {
        components.removeFirst();
    } else if (components.front().assetId != behaviorId) {
        components.front() = {behaviorId,true,{},{}};
    }
    return setBehaviorComponents(objectId,components,error);
}

bool ProjectStore::setBehaviorComponents(const QString &objectId,const QList<BehaviorComponent> &components,QString &error)
{
    const auto *owner = object(objectId);
    if (!owner || components.size() > maxBehaviorComponents) {
        error = tr("Choose a level object. Each object supports up to %1 behavior components.").arg(maxBehaviorComponents);
        return false;
    }
    const auto previous = behaviorComponents(*owner);
    QSet<QString> used;
    for (const auto &component : components) {
        if (component.assetId.isEmpty() || used.contains(component.assetId)
            || (component.amount && !(*component.amount >= 0.1f && *component.amount <= 15))) {
            error = tr("Each behavior asset can be attached once per object. Overrides must be between 0.1 and 15.");
            return false;
        }
        used.insert(component.assetId);
        const auto asset = project()->behaviors.constFind(component.assetId);
        if (asset == project()->behaviors.cend() && !previous.contains(component)) {
            error = tr("Choose an existing behavior asset.");
            return false;
        }
        if (!component.enabled || asset == project()->behaviors.cend())
            continue;
        const auto *target = component.targetId.isEmpty() ? owner : object(component.targetId);
        if (!target || (!component.targetId.isEmpty() && !target->visible)) {
            error = tr("Choose a visible action target.");
            return false;
        }
        const auto physics = target->details.value("physics").toString();
        using Action = wf::BehaviorRule::Action;
        if (asset->action != Action::PulseBeacon
            && (target->meshAsset.isEmpty() || !target->parentId.isEmpty()
                || physics != (asset->action == Action::RaiseObject ? "Static" : "Dynamic"))) {
            error = asset->action == Action::RaiseObject ? tr("Raise Object requires an unparented mesh with Static physics.")
                : tr("Launch Upward requires an unparented mesh with Dynamic physics.");
            return false;
        }
    }
    if (components == previous)
        return true;
    auto copy = *owner;
    copy.details.remove("behaviorAsset");
    copy.details.remove("behaviorComponents");
    if (components.size() == 1 && components.front().enabled
        && components.front().targetId.isEmpty() && !components.front().amount) {
        copy.details.insert("behaviorAsset",components.front().assetId);
    } else if (!components.isEmpty()) {
        QJsonArray rows;
        for (const auto &component : components) {
            QJsonObject row{{"asset",component.assetId},{"enabled",component.enabled}};
            if (!component.targetId.isEmpty())
                row.insert("target",component.targetId);
            if (component.amount)
                row.insert("amount",double(*component.amount));
            rows.append(row);
        }
        copy.details.insert("behaviorComponents",rows);
    }
    return updateObjects({copy},tr("Edit Behavior Components"),error);
}

bool ProjectStore::setNativeComponent(const QString &objectId,const QString &type,std::optional<QJsonObject> component,QString &error)
{
    const auto *owner = object(objectId);
    if (!owner || isLegacyPlaceholder(*owner) || type.isEmpty()) {
        error = tr("Select a scene object and a native component type.");
        return false;
    }
    auto entries = owner->details.value("nativeComponents").toObject();
    const auto previous = entries;
    if (component)
        entries.insert(type,*component);
    else
        entries.remove(type);
    if (!validNativeComponentData(entries)) {
        error = tr("Invalid native component settings. An object supports up to 16 native components.");
        return false;
    }
    if (entries == previous)
        return true;
    auto object = *owner;
    if (entries.isEmpty())
        object.details.remove("nativeComponents");
    else
        object.details.insert("nativeComponents",entries);
    return updateObjects({object},tr("Edit Native Component"),error);
}

void ProjectStore::deleteObject()
{
    if (!selectedObject())
        return;
    const auto before = project()->objects;
    auto after = before;
    for (auto &child : after)
        if (child.parentId == selection_) {
            child.parentOffset = parentMatrix(child);
            child.parentId.clear();
        }
    after.removeIf([this](const SceneObject &object) { return object.id == selection_; });
    const auto next = after.isEmpty() ? QString() : after.first().id;
    pushSceneEdit(after, next, tr("Delete %1").arg(selectedObject()->name));
}

QHash<QString,QMatrix4x4> ProjectStore::worldMatrices(const SceneObject *preview) const
{
    QHash<QString,QMatrix4x4> result;
    const auto *active = project();
    if (!active)
        return result;
    QHash<QString,const SceneObject *> objects;
    for (const auto &object : active->objects)
        objects.insert(object.id,preview && preview->id == object.id ? preview : &object);
    for (const auto *object : objects) {
        QList<const SceneObject *> chain;
        for (auto *part = object; part && !result.contains(part->id); part = objects.value(part->parentId))
            chain.append(part);
        while (!chain.isEmpty()) {
            const auto *part = chain.takeLast();
            result.insert(part->id,result.value(part->parentId)*part->parentOffset*meshBasis(*part,true));
        }
    }
    return result;
}

QMatrix4x4 ProjectStore::worldMatrix(const QString &id) const
{
    QMatrix4x4 result;
    for (auto *part = object(id); part; part = object(part->parentId))
        result = part->parentOffset*meshBasis(*part,true)*result;
    return result;
}

QMatrix4x4 ProjectStore::parentMatrix(const SceneObject &object) const
{
    return worldMatrix(object.parentId)*object.parentOffset;
}

bool ProjectStore::reparentObject(const QString &childId,const QString &parentId,QString &error)
{
    const auto *child = object(childId);
    const auto *parent = object(parentId);
    if (!child || child->meshAsset.isEmpty() || (!parentId.isEmpty() && (!parent || parent->meshAsset.isEmpty()))) {
        error = tr("Select a valid object and parent.");
        return false;
    }
    if (child->parentId == parentId)
        return true;
    for (auto *part = parent; part; part = object(part->parentId))
        if (part->id == childId) {
            error = tr("An object cannot be parented to itself or a descendant.");
            return false;
        }
    bool invertible = false;
    const auto inverse = worldMatrix(parentId).inverted(&invertible);
    if (!invertible) {
        error = tr("The parent transform cannot be inverted.");
        return false;
    }
    auto objects = project()->objects;
    auto &updated = *std::ranges::find(objects,childId,&SceneObject::id);
    updated.parentId = parentId;
    updated.parentOffset = inverse*parentMatrix(*child);
    return pushProjectEdit(objects,project()->assets,childId,
        parent ? tr("Set Object Parent") : tr("Unparent Object"),&error);
}
