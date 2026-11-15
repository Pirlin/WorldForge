#include "State/ForgeAi.h"
#include "State/GlbImport.h"
#include "State/WorldRuntime.h"

#include <QCoreApplication>
#include <QVector2D>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QQuaternion>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace forge {
namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("ForgeAi", text);
}

bool hasWord(const QString &text, const QString &word)
{
    return QRegularExpression(QStringLiteral("\\b%1\\b").arg(QRegularExpression::escape(word)),
                              QRegularExpression::CaseInsensitiveOption).match(text).hasMatch();
}

bool hasAny(const QString &text, std::initializer_list<const char *> words)
{
    return std::ranges::any_of(words, [&](const char *word) { return hasWord(text, QString::fromLatin1(word)); });
}

// Words in a target name that tell it apart from others, such as "North" in "North Entrance".
QStringList qualifiers(const QString &name, std::initializer_list<const char *> generic)
{
    QStringList words;
    for (const auto &word : name.toLower().split(QRegularExpression("[^a-z0-9]+"), Qt::SkipEmptyParts))
        if (std::ranges::none_of(generic, [&](const char *g) { return word == QLatin1String(g); }))
            words << word;
    return words;
}

QList<Candidate> narrow(const QList<Candidate> &all, const QString &prompt, std::initializer_list<const char *> generic)
{
    QList<Candidate> named;
    for (const auto &candidate : all) {
        const auto words = qualifiers(candidate.name, generic);
        if (!words.isEmpty() && std::ranges::all_of(words, [&](const QString &word) { return hasWord(prompt, word); }))
            named << candidate;
    }
    return named.isEmpty() ? all : named;
}

int stripeCount(const QString &prompt)
{
    static const QStringList numbers{"one","two","three","four","five","six","seven","eight","nine","ten",
                                     "eleven","twelve"};
    const auto match = QRegularExpression(QStringLiteral("\\b(\\d+|%1)\\s+(?:[a-z-]+\\s+){0,2}stripes?\\b")
        .arg(numbers.join('|')), QRegularExpression::CaseInsensitiveOption).match(prompt);
    if (!match.hasMatch())
        return -1;
    bool ok = false;
    const auto value = match.captured(1).toInt(&ok);
    if (ok)
        return value;
    const auto index = numbers.indexOf(match.captured(1).toLower());
    return index < 0 ? -1 : index + 1;
}

std::optional<std::pair<worldforge::Point3, worldforge::Point3>> bounds(const worldforge::Mesh &mesh)
{
    if (mesh.vertices.empty())
        return std::nullopt;
    auto low = mesh.vertices.front(), high = low;
    for (const auto &vertex : mesh.vertices)
        for (int axis = 0; axis < 3; ++axis) {
            low[axis] = std::min(low[axis], vertex[axis]);
            high[axis] = std::max(high[axis], vertex[axis]);
        }
    return std::pair{low, high};
}

QJsonArray json(const QVector3D &value)
{
    return {value.x(), value.y(), value.z()};
}

}

QString Command::describe() const
{
    const auto label = parameters.value("label").toString();
    if (operation == "bevel_edges")
        return parameters.value("width").toDouble() <= 0 ? tr("Keep the edges of %1 sharp").arg(label)
            : tr("Bevel the sharp edges of %1 by %2 m in %3 segments").arg(label)
                  .arg(parameters.value("width").toDouble(), 0, 'f', 3).arg(parameters.value("segments").toInt());
    if (operation == "paint_stripes")
        return tr("Paint %1 %2 stripes on %3 with %4% wear and %5% fade").arg(parameters.value("stripes").toInt())
            .arg(parameters.value("colorName").toString(), label)
            .arg(qRound(parameters.value("wear").toDouble()*100)).arg(qRound(parameters.value("fade").toDouble()*100));
    if (operation == "place_instance")
        return tr("Place a new barrier at %1").arg(label);
    if (operation == "check_play")
        return tr("Check in Play that the barrier stands at %1, shows the stripes, and blocks the player").arg(label);
    if (operation == "move_entrance")
        return tr("Open the repair shop entrance onto %1").arg(label);
    if (operation == "move_mission_marker")
        return tr("Move %1 to the new doorway").arg(label);
    if (operation == "reroute_traffic")
        return tr("Route %1 around obstacles in its lanes").arg(label);
    if (operation == "check_street")
        return tr("Check in Play that traffic completes its route, the shop is enterable, and the mission completes");
    return operation;
}

Plan plan(const Project &project, const QString &prompt)
{
    Plan result;
    result.prompt = prompt.simplified();
    const auto &text = result.prompt;
    if (text.isEmpty()) {
        result.error = tr("Describe the barrier change you want, such as “Make a worn barrier with faded yellow "
                          "stripes at the north entrance.”");
        return result;
    }
    if (!hasAny(text, {"barrier", "barriers", "barricade"})) {
        result.error = tr("Forge AI can currently stage barrier edits. Mention the barrier you want to change.");
        return result;
    }
    for (const auto &asset : project.assets)
        if (asset.name.contains("barrier", Qt::CaseInsensitive) || asset.name.contains("barricade", Qt::CaseInsensitive))
            result.barriers << Candidate{asset.id, asset.name};
    std::ranges::sort(result.barriers, {}, &Candidate::name);
    for (const auto &object : project.objects)
        if (object.name.contains("entrance", Qt::CaseInsensitive) && object.meshAsset.isEmpty())
            result.entrances << Candidate{object.id, object.name};
    std::ranges::sort(result.entrances, {}, &Candidate::name);
    if (result.barriers.isEmpty()) {
        result.error = tr("This project has no barrier asset. Add or import a mesh with “Barrier” in its name.");
        return result;
    }
    if (result.entrances.isEmpty()) {
        result.error = tr("This level has no named entrance. Add an Empty named after it, such as “North Entrance”.");
        return result;
    }
    const auto barriers = narrow(result.barriers, text, {"barrier", "barricade", "road", "mesh"});
    const auto entrances = narrow(result.entrances, text, {"entrance", "road", "the"});
    if (barriers.size() == 1)
        result.barrierId = barriers.front().id;
    if (entrances.size() == 1)
        result.entranceId = entrances.front().id;

    auto &style = result.style;
    if (hasAny(text, {"new", "clean", "fresh", "pristine"}))
        style.wear = 0.1f;
    if (hasAny(text, {"worn", "weathered", "old", "used"}))
        style.wear = 0.55f;
    if (hasAny(text, {"battered", "heavily", "very"}) && hasAny(text, {"worn", "weathered", "battered"}))
        style.wear = 0.8f;
    style.fade = hasAny(text, {"faded", "fading", "sun-bleached", "dull"}) ? 0.4f
        : hasAny(text, {"bright", "vivid", "fresh", "new"}) ? 0.05f : 0.2f;
    static const QList<std::pair<const char *, const char *>> colors{
        {"yellow", "#f2b705"}, {"orange", "#f26b0f"}, {"red", "#c8261d"}, {"white", "#ece9e1"},
        {"blue", "#1f5fbf"}, {"green", "#2f8f3a"}, {"black", "#1b1b1d"}};
    for (const auto &[name, hex] : colors)
        if (hasWord(text, QString::fromLatin1(name))) {
            style.color = QColor(QString::fromLatin1(hex));
            break;
        }
    if (const int count = stripeCount(text); count != -1)
        style.stripes = count;
    if (hasAny(text, {"sharp", "square"}))
        style.bevelWidth = 0;
    else if (hasAny(text, {"rounded", "round", "soft"}))
        style.bevelWidth = 0.08f;
    style.seed = int(qHash(text) % 997u) + 1;
    if (const auto problem = validate(style); !problem.isEmpty())
        result.error = problem;
    return result;
}

QString validate(const BarrierStyle &style)
{
    if (!std::isfinite(style.bevelWidth) || style.bevelWidth < 0 || style.bevelWidth > 0.25f)
        return tr("Bevel width must be between 0 and 0.25 m.");
    if (style.bevelSegments < 1 || style.bevelSegments > 6)
        return tr("Bevel segments must be between 1 and 6.");
    if (style.stripes < 1 || style.stripes > 12)
        return tr("Choose between 1 and 12 stripes.");
    if (!std::isfinite(style.wear) || style.wear < 0 || style.wear > 1)
        return tr("Wear must be between 0% and 100%.");
    if (!std::isfinite(style.fade) || style.fade < 0 || style.fade > 1)
        return tr("Fade must be between 0% and 100%.");
    if (!style.color.isValid())
        return tr("Choose a stripe color.");
    return {};
}

QString revision(const worldforge::Mesh &mesh)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const auto add = [&hash](const auto &value) {
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(&value), sizeof(value)));
    };
    for (const auto &vertex : mesh.vertices)
        add(vertex);
    for (const auto &face : mesh.faces) {
        add(face.material);
        for (const auto &corner : face.corners) {
            add(corner.vertex);
            add(corner.uv);
        }
    }
    for (const auto &material : mesh.materials) {
        add(material.color);
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(material.baseColorTexture.data()),
                                    qsizetype(material.baseColorTexture.size())));
    }
    return QString::fromLatin1(hash.result().toHex().left(16));
}

QString revision(const SceneObject &object)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto &value : {object.position, object.rotation, object.scale})
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(&value), sizeof(value)));
    hash.addData(object.name.toUtf8());
    hash.addData(object.parentId.toUtf8());
    return QString::fromLatin1(hash.result().toHex().left(16));
}

QList<Command> commands(const Project &project, const Plan &plan, QString &error)
{
    error.clear();
    if (!plan.ready()) {
        error = plan.error.isEmpty() ? tr("Choose the barrier and entrance first.") : plan.error;
        return {};
    }
    if (const auto problem = validate(plan.style); !problem.isEmpty()) {
        error = problem;
        return {};
    }
    const auto asset = project.assets.constFind(plan.barrierId);
    const auto entrance = std::ranges::find(project.objects, plan.entranceId, &SceneObject::id);
    if (asset == project.assets.cend() || entrance == project.objects.end()) {
        error = tr("The barrier or entrance is no longer in this project. Plan the request again.");
        return {};
    }
    if (!asset->modifiers.empty() || !asset->otherUvSets.isEmpty()) {
        error = tr("%1 uses WorldForge modifiers or several UV sets, which Studio jobs do not support yet.")
            .arg(asset->name);
        return {};
    }
    const auto meshRevision = revision(*asset->mesh);
    const auto &style = plan.style;
    QList<Command> list;
    list << Command{"bevel_edges", asset->id, meshRevision,
        {{"label", asset->name}, {"width", style.bevelWidth}, {"segments", style.bevelSegments}}};
    list << Command{"paint_stripes", asset->id, meshRevision,
        {{"label", asset->name}, {"stripes", style.stripes}, {"color", style.color.name()},
         {"colorName", style.color.name()}, {"wear", style.wear}, {"fade", style.fade}, {"seed", style.seed}}};
    static const QList<std::pair<const char *, const char *>> names{
        {"#f2b705", "yellow"}, {"#f26b0f", "orange"}, {"#c8261d", "red"}, {"#ece9e1", "white"},
        {"#1f5fbf", "blue"}, {"#2f8f3a", "green"}, {"#1b1b1d", "black"}};
    for (const auto &[hex, name] : names)
        if (style.color.name() == QLatin1String(hex))
            list.back().parameters.insert("colorName", QString::fromLatin1(name));
    list << Command{"place_instance", entrance->id, revision(*entrance),
        {{"label", entrance->name}, {"asset", asset->id}, {"position", json(entrance->position)},
         {"rotation", json(entrance->rotation)}}};
    list << Command{"check_play", entrance->id, revision(*entrance),
        {{"label", entrance->name}, {"expect", "blocks the player"}}};
    return list;
}

std::optional<SceneObject> placement(const Project &project, const Plan &plan, const worldforge::Mesh &mesh)
{
    const auto asset = project.assets.constFind(plan.barrierId);
    const auto entrance = std::ranges::find(project.objects, plan.entranceId, &SceneObject::id);
    const auto box = bounds(mesh);
    if (asset == project.assets.cend() || entrance == project.objects.end() || !box)
        return std::nullopt;
    SceneObject object;
    object.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    object.name = tr("%1 at %2").arg(asset->name, entrance->name);
    for (int suffix = 2; std::ranges::find(project.objects, object.name, &SceneObject::name) != project.objects.end();
         ++suffix)
        object.name = tr("%1 at %2 %3").arg(asset->name, entrance->name).arg(suffix);
    object.type = QStringLiteral("Static Mesh");
    object.group = QStringLiteral("Props");
    object.meshAsset = asset->id;
    object.position = entrance->position;
    object.position.setZ(entrance->position.z() - box->first[2]);
    object.rotation = entrance->rotation;
    object.details.insert("physics", "Static");
    object.details.insert("forgeRequest", plan.prompt);
    return object;
}

QVector3D approach(const SceneObject &placement)
{
    return QQuaternion::fromEulerAngles(placement.rotation).rotatedVector({0, 1, 0});
}

std::optional<Stage> stage(const Project &project, const Plan &plan, const QList<Command> &commands,
                           const QByteArray &glb, const QByteArray &source, const QStringList &steps, QString &error)
{
    const auto asset = project.assets.constFind(plan.barrierId);
    const auto entrance = std::ranges::find(project.objects, plan.entranceId, &SceneObject::id);
    if (asset == project.assets.cend() || entrance == project.objects.end() || commands.size() != 4) {
        error = tr("The barrier or entrance is no longer in this project. Plan the request again.");
        return std::nullopt;
    }
    auto mesh = importGlb(glb, error);
    if (!mesh)
        return std::nullopt;
    if (const auto problem = worldforge::validateMesh(*mesh); !problem.empty()) {
        error = tr("Studio returned an unusable mesh: %1").arg(QString::fromStdString(problem));
        return std::nullopt;
    }
    auto object = placement(project, plan, *mesh);
    if (!object) {
        error = tr("Could not place the barrier at %1.").arg(entrance->name);
        return std::nullopt;
    }
    Stage result;
    result.plan = plan;
    result.commands = commands;
    result.assetId = asset->id;
    result.expectedMesh = asset->mesh.data();
    result.meshRevision = commands.front().expectedRevision;
    result.entranceRevision = commands.at(2).expectedRevision;
    result.mesh = std::move(*mesh);
    result.source = source;
    result.placement = std::move(*object);
    result.steps = steps;
    return result;
}

bool apply(ProjectStore &store, const Stage &stage, QString &error)
{
    const auto *asset = store.meshAsset(stage.assetId);
    if (!asset || asset->mesh.data() != stage.expectedMesh || revision(*asset->mesh) != stage.meshRevision) {
        error = tr("The barrier changed while the preview was open. Plan the request again so Forge AI does not "
                   "overwrite your edit.");
        return false;
    }
    const auto *entrance = store.object(stage.plan.entranceId);
    if (!entrance || revision(*entrance) != stage.entranceRevision) {
        error = tr("The entrance moved or was renamed while the preview was open. Plan the request again.");
        return false;
    }
    return store.publishStudioMeshWithPlacement(stage.assetId, stage.expectedMesh, stage.mesh, stage.source,
        stage.placement, tr("Forge AI: %1").arg(stage.placement.name), error);
}

CheckReport checkInPlay(const ProjectStore &store, const Stage &stage)
{
    CheckReport report;
    const auto *placed = store.object(stage.placement.id);
    const auto *entrance = store.object(stage.plan.entranceId);
    const auto *asset = store.meshAsset(stage.assetId);
    if (!placed || !entrance || !asset) {
        report.failed << tr("The staged barrier is not in the level. Apply the change, or redo it if you undid it.");
        return report;
    }
    const auto offset = QVector2D(placed->position.toVector2D() - entrance->position.toVector2D()).length();
    if (offset <= 0.05f)
        report.passed << tr("The barrier stands at %1.").arg(entrance->name);
    else
        report.failed << tr("The barrier is %1 m from %2. Move it back or plan the request again.")
            .arg(offset, 0, 'f', 2).arg(entrance->name);
    if (*asset->mesh == stage.mesh && std::ranges::any_of(asset->mesh->materials,
            [](const auto &slot) { return !slot.baseColorTexture.empty(); }))
        report.passed << tr("%1 uses the staged mesh and its striped base-color map.").arg(asset->name);
    else
        report.failed << tr("%1 no longer matches the staged mesh or lost its base-color map.").arg(asset->name);
    const auto probe = WorldRuntime::probeCollision(store, placed->id, approach(*placed));
    if (!probe.loaded)
        report.failed << tr("Play could not load a collider for %1. Set Physics to Static in Details.")
            .arg(placed->name);
    else if (probe.blocked)
        report.passed << tr("Walking into %1 in Play stops the player.").arg(placed->name);
    else
        report.failed << tr("The player walked through %1 in Play. Check its Physics and collision bounds in Details.")
            .arg(placed->name);
    return report;
}

QJsonObject jobDescription(const Plan &plan)
{
    const auto &style = plan.style;
    return {
        {"bevel", QJsonObject{{"width", style.bevelWidth}, {"segments", style.bevelSegments}}},
        {"stripes", QJsonObject{{"count", style.stripes},
            {"color", QJsonArray{style.color.redF(), style.color.greenF(), style.color.blueF()}},
            {"wear", style.wear}, {"fade", style.fade}, {"seed", style.seed}}},
    };
}

}
