#include "State/NativeComponents.h"
#include "State/ProjectStore.h"

#include <QRegularExpression>

worldforge::NativePropertyValue readNativeProperty(const QJsonValue &value)
{
    const auto row = value.toObject();
    const auto kind = std::uint32_t(row.value("kind").toInt());
    const auto data = row.value("value");
    if (kind == WF_GAME_BOOL && data.isBool())
        return {kind,data.toBool() ? 1.0 : 0.0,{}};
    if ((kind == WF_GAME_INT || kind == WF_GAME_REAL) && data.isDouble())
        return {kind,data.toDouble(),{}};
    if (kind >= WF_GAME_OBJECT && kind <= WF_GAME_TEXTURE && data.isString())
        return {kind,0,data.toString().toStdString()};
    return {};
}

QJsonObject writeNativeProperty(const worldforge::NativePropertyValue &value)
{
    const auto data = value.kind == WF_GAME_BOOL ? QJsonValue(value.number != 0)
        : value.kind >= WF_GAME_OBJECT ? QJsonValue(QString::fromStdString(value.reference)) : QJsonValue(value.number);
    return {{"kind",int(value.kind)},{"value",data}};
}

bool validNativeComponentData(const QJsonValue &value)
{
    if (value.isUndefined())
        return true;
    const auto components = value.toObject();
    if (!value.isObject() || components.size() > 16)
        return false;
    static const QRegularExpression id(QStringLiteral("^[A-Za-z0-9_.-]{1,64}$"));
    for (auto type = components.begin(); type != components.end(); ++type) {
        const auto component = type.value().toObject();
        const auto fields = component.value("properties");
        if (!id.match(type.key()).hasMatch() || !component.value("enabled").isBool()
            || !fields.isObject() || fields.toObject().size() > 32)
            return false;
        const auto properties = fields.toObject();
        for (auto field = properties.begin(); field != properties.end(); ++field)
            if (!id.match(field.key()).hasMatch() || !readNativeProperty(field.value()).valid())
                return false;
    }
    return true;
}

bool collectNativeInstances(const ProjectStore &store,std::vector<worldforge::NativeInstance> &instances,QString &error)
{
    instances.clear();
    const auto *project = store.project();
    if (!project)
        return false;
    std::vector<worldforge::NativeInstance> result;
    const auto hasReference = [&](const worldforge::NativePropertyValue &value) {
        if (value.reference.empty())
            return true;
        const auto id = QString::fromStdString(value.reference);
        switch (value.kind) {
        case WF_GAME_OBJECT: return store.object(id) != nullptr;
        case WF_GAME_MESH: return project->assets.contains(id);
        case WF_GAME_MATERIAL: return project->surfaces.materials.contains(id);
        case WF_GAME_TEXTURE: return project->surfaces.textures.contains(id);
        default: return true;
        }
    };
    for (const auto &object : project->objects) {
        if (!object.visible || store.isLegacyPlaceholder(object))
            continue;
        const auto components = object.details.value("nativeComponents").toObject();
        for (auto type = components.begin(); type != components.end(); ++type) {
            const auto component = type.value().toObject();
            if (!component.value("enabled").toBool())
                continue;
            if (result.size() >= 256 || object.id.toUtf8().size() > 128) {
                error = QObject::tr("The scene exceeds the native component or object ID limits.");
                return false;
            }
            worldforge::NativeInstance instance{object.id.toStdString(),type.key().toStdString(),{}};
            const auto properties = component.value("properties").toObject();
            for (auto field = properties.begin(); field != properties.end(); ++field) {
                auto value = readNativeProperty(field.value());
                if (!value.valid() || !hasReference(value)) {
                    error = QObject::tr("%1 · %2 has a missing or invalid reference in %3.")
                        .arg(object.name,type.key(),field.key());
                    return false;
                }
                instance.properties.emplace(field.key().toStdString(),std::move(value));
            }
            result.push_back(std::move(instance));
        }
    }
    instances = std::move(result);
    return true;
}
