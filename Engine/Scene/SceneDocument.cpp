#include "Engine/Scene/SceneDocument.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace worldforge {
namespace {
template<class... Functions>
struct Overloaded : Functions... { using Functions::operator()...; };
}

SceneDocument::SceneDocument(Guid project, Guid scene, Revision revision, Entities entities)
    : project_(project), scene_(scene), revision_(revision), entities_(std::move(entities))
{
}

std::variant<SceneDocument, SceneError> SceneDocument::open(const SceneSnapshot &snapshot)
{
    if (snapshot.schemaVersion != sceneSchemaVersion)
        return SceneError::UnsupportedSchema;
    if (snapshot.project.isNull() || snapshot.scene.isNull())
        return SceneError::InvalidIdentity;

    Entities entities;
    for (const auto &entity : snapshot.entities) {
        if (const auto error = validateFields(entity); error != SceneError::None)
            return error;
        if (!entities.emplace(entity.id, entity).second)
            return SceneError::DuplicateEntity;
    }

    SceneDocument result(snapshot.project, snapshot.scene, snapshot.revision, std::move(entities));
    for (const auto &[key, entity] : result.entities_) {
        if (const auto error = result.validateParent(entity.id, entity.parent); error != SceneError::None)
            return error;
    }
    return result;
}

SceneSnapshot SceneDocument::snapshot() const
{
    SceneSnapshot result{project_, scene_, revision_, {}};
    result.entities.reserve(entities_.size());
    for (const auto &[key, entity] : entities_)
        result.entities.push_back(entity);
    return result;
}

const Entity *SceneDocument::find(const Guid &id) const
{
    const auto found = entities_.find(id);
    return found == entities_.end() ? nullptr : &found->second;
}

SceneError SceneDocument::validateFields(const Entity &entity)
{
    if (entity.id.isNull() || (entity.parent && entity.parent->isNull()))
        return SceneError::InvalidIdentity;
    if (entity.name.size() > 256 || entity.name.find_first_not_of(" \t\n\r\f\v") == std::string::npos
        || std::ranges::any_of(entity.name, [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
        return SceneError::InvalidName;

    const auto finite = [](double value) { return std::isfinite(value); };
    if (!std::ranges::all_of(entity.transform.positionMeters, finite)
        || !std::ranges::all_of(entity.transform.rotationDegrees, finite)
        || !std::ranges::all_of(entity.transform.scale, [&](double value) { return finite(value) && value != 0; }))
        return SceneError::InvalidTransform;
    return SceneError::None;
}

SceneError SceneDocument::validateParent(Guid id, const std::optional<Guid> &parent) const
{
    auto ancestor = parent;
    for (std::size_t hops = 0; ancestor; ++hops) {
        if (ancestor->isNull())
            return SceneError::InvalidIdentity;
        if (*ancestor == id || hops >= entities_.size())
            return SceneError::HierarchyCycle;
        const auto *entity = find(*ancestor);
        if (!entity)
            return SceneError::MissingParent;
        ancestor = entity->parent;
    }
    return SceneError::None;
}

EditResult SceneDocument::preview(const SceneCommand &command) const
{
    const auto failed = [this](SceneError error) { return EditResult{error, revision_, std::nullopt}; };
    const auto ready = [this](SceneEdit inverse) {
        return EditResult{SceneError::None, revision_ + 1, std::move(inverse)};
    };
    if (command.schemaVersion != commandSchemaVersion)
        return failed(SceneError::UnsupportedSchema);
    if (command.id.isNull())
        return failed(SceneError::InvalidIdentity);
    if (command.project != project_ || command.scene != scene_)
        return failed(SceneError::WrongDocument);
    if (command.expectedRevision != revision_)
        return failed(SceneError::RevisionConflict);
    if (revision_ == std::numeric_limits<Revision>::max())
        return failed(SceneError::RevisionExhausted);

    return std::visit(Overloaded{
        [&](const AddEntity &add) {
            if (const auto error = validateFields(add.entity); error != SceneError::None)
                return failed(error);
            if (find(add.entity.id))
                return failed(SceneError::DuplicateEntity);
            if (const auto error = validateParent(add.entity.id, add.entity.parent); error != SceneError::None)
                return failed(error);
            return ready(RemoveEntity{add.entity.id});
        },
        [&](const RemoveEntity &remove) {
            const auto *entity = find(remove.id);
            if (!entity)
                return failed(SceneError::MissingEntity);
            if (std::ranges::any_of(entities_, [&](const auto &entry) { return entry.second.parent == remove.id; }))
                return failed(SceneError::HasChildren);
            return ready(AddEntity{*entity});
        },
        [&](const RenameEntity &rename) {
            const auto *entity = find(rename.id);
            if (!entity)
                return failed(SceneError::MissingEntity);
            auto changed = *entity;
            changed.name = rename.name;
            if (const auto error = validateFields(changed); error != SceneError::None)
                return failed(error);
            return ready(RenameEntity{rename.id, entity->name});
        },
        [&](const ReparentEntity &reparent) {
            const auto *entity = find(reparent.id);
            if (!entity)
                return failed(SceneError::MissingEntity);
            if (const auto error = validateParent(reparent.id, reparent.parent); error != SceneError::None)
                return failed(error);
            return ready(ReparentEntity{reparent.id, entity->parent});
        },
        [&](const SetTransform &transform) {
            const auto *entity = find(transform.id);
            if (!entity)
                return failed(SceneError::MissingEntity);
            auto changed = *entity;
            changed.transform = transform.transform;
            if (const auto error = validateFields(changed); error != SceneError::None)
                return failed(error);
            return ready(SetTransform{transform.id, entity->transform});
        },
    }, command.edit);
}

EditResult SceneDocument::apply(const SceneCommand &command)
{
    auto result = preview(command);
    if (!result.ok())
        return result;
    std::visit(Overloaded{
        [&](const AddEntity &add) { entities_.emplace(add.entity.id, add.entity); },
        [&](const RemoveEntity &remove) { entities_.erase(remove.id); },
        [&](const RenameEntity &rename) { entities_.at(rename.id).name = rename.name; },
        [&](const ReparentEntity &reparent) { entities_.at(reparent.id).parent = reparent.parent; },
        [&](const SetTransform &transform) { entities_.at(transform.id).transform = transform.transform; },
    }, command.edit);
    revision_ = result.revision;
    return result;
}

}
