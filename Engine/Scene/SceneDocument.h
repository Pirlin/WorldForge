#pragma once

#include "Engine/Core/Guid.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace worldforge {

using Revision = std::uint64_t;
inline constexpr unsigned sceneSchemaVersion = 1;
inline constexpr unsigned commandSchemaVersion = 1;

struct Transform {
    std::array<double, 3> positionMeters{};
    std::array<double, 3> rotationDegrees{};
    std::array<double, 3> scale{1, 1, 1};
    bool operator==(const Transform &) const = default;
};

struct Entity {
    Guid id;
    std::optional<Guid> parent;
    std::string name;
    Transform transform;
    bool operator==(const Entity &) const = default;
};

struct SceneSnapshot {
    Guid project;
    Guid scene;
    Revision revision = 0;
    std::vector<Entity> entities;
    unsigned schemaVersion = sceneSchemaVersion;
    bool operator==(const SceneSnapshot &) const = default;
};

struct AddEntity { Entity entity; };
struct RemoveEntity { Guid id; };
struct RenameEntity { Guid id; std::string name; };
struct ReparentEntity { Guid id; std::optional<Guid> parent; };
struct SetTransform { Guid id; Transform transform; };
using SceneEdit = std::variant<AddEntity, RemoveEntity, RenameEntity, ReparentEntity, SetTransform>;

struct SceneCommand {
    Guid id;
    Guid project;
    Guid scene;
    Revision expectedRevision;
    SceneEdit edit;
    unsigned schemaVersion = commandSchemaVersion;
};

enum class SceneError {
    None,
    UnsupportedSchema,
    InvalidIdentity,
    WrongDocument,
    RevisionConflict,
    RevisionExhausted,
    DuplicateEntity,
    MissingEntity,
    MissingParent,
    HierarchyCycle,
    HasChildren,
    InvalidName,
    InvalidTransform,
};

struct EditResult {
    SceneError error;
    Revision revision;
    std::optional<SceneEdit> inverse;
    bool ok() const { return error == SceneError::None; }
};

class SceneDocument {
public:
    // A rejected snapshot leaves the caller's active document untouched.
    static std::variant<SceneDocument, SceneError> open(const SceneSnapshot &snapshot);

    SceneSnapshot snapshot() const;
    Guid projectId() const { return project_; }
    Guid sceneId() const { return scene_; }
    const Entity *find(const Guid &id) const;
    Revision revision() const { return revision_; }

    EditResult preview(const SceneCommand &command) const;
    EditResult apply(const SceneCommand &command);

private:
    using Entities = std::map<Guid, Entity>;
    SceneDocument(Guid project, Guid scene, Revision revision, Entities entities);

    SceneError validateParent(Guid id, const std::optional<Guid> &parent) const;
    static SceneError validateFields(const Entity &entity);

    Guid project_;
    Guid scene_;
    Revision revision_;
    Entities entities_;
};

}
