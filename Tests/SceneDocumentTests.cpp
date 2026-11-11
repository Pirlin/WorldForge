#include "Engine/Scene/SceneDocument.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <utility>

using namespace worldforge;

namespace {
void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
}

int main()
{
    const auto example = Guid::parse("A1723291-8A22-47DC-B098-1F6718CFFAAC");
    require(example && example->toString() == "a1723291-8a22-47dc-b098-1f6718cffaac", "GUID canonicalizes");
    require(!Guid::parse("A1723291-8A22-47DC-B098-1F6718CFFAAX"), "Invalid GUID rejected");
    require(!Guid::parse("A17232918A22-47DC-B098-1F6718CFFAAC"), "Missing separator rejected");
    const auto generated = Guid::generate();
    const auto generatedText = generated.toString();
    require(!generated.isNull() && generatedText[14] == '4'
            && std::string_view("89ab").find(generatedText[19]) != std::string_view::npos,
            "Generated ID has version-4 layout");
    require(Guid::parse(generatedText) == generated, "Generated ID round trips");

    const auto project = Guid::generate();
    const auto scene = Guid::generate();
    const auto root = Guid::generate();
    const auto child = Guid::generate();
    auto document = std::get<SceneDocument>(SceneDocument::open({project, scene, 0, {}}));
    const auto command = [&](SceneEdit edit) {
        return SceneCommand{Guid::generate(), project, scene, document.revision(), std::move(edit)};
    };
    const auto checkRejected = [&](SceneEdit edit, SceneError expected, const char *message) {
        const auto old = document.snapshot();
        const auto result = document.apply(command(std::move(edit)));
        require(result.error == expected && !result.inverse && result.revision == old.revision
                && document.snapshot() == old, message);
    };

    const auto createRoot = command(AddEntity{{root, {}, "Root", {}}});
    const auto preview = document.preview(createRoot);
    require(preview.ok() && preview.revision == 1 && !document.find(root), "Preview does not change the scene");
    require(document.apply(createRoot).ok(), "Add root");
    require(document.apply(createRoot).error == SceneError::RevisionConflict, "Replay at stale revision rejected");
    require(document.apply(command(AddEntity{{child, root, "Cube", {}}})).ok(), "Add child");
    checkRejected(AddEntity{{child, {}, "Duplicate", {}}}, SceneError::DuplicateEntity, "Duplicate identity rejected");
    checkRejected(AddEntity{{Guid{}, {}, "Empty ID", {}}}, SceneError::InvalidIdentity, "Null identity rejected");
    checkRejected(ReparentEntity{root, child}, SceneError::HierarchyCycle, "Parent cycle rejected");
    checkRejected(ReparentEntity{child, Guid::generate()}, SceneError::MissingParent, "Dangling parent rejected");
    checkRejected(RemoveEntity{root}, SceneError::HasChildren, "Parent deletion preserves children");
    checkRejected(RemoveEntity{Guid::generate()}, SceneError::MissingEntity, "Missing target rejected");
    checkRejected(RenameEntity{child, " \t"}, SceneError::InvalidName, "Blank entity name rejected");

    Transform transform;
    transform.positionMeters = {5.0, 0.0, 1.0};
    transform.rotationDegrees = {0.0, 25.0, 0.0};
    transform.scale = {1.0, 2.0, 1.0};
    auto invalid = transform;
    invalid.positionMeters[0] = std::numeric_limits<double>::quiet_NaN();
    checkRejected(SetTransform{child, invalid}, SceneError::InvalidTransform, "NaN transform rejected");
    invalid = transform;
    invalid.scale[1] = 0;
    checkRejected(SetTransform{child, invalid}, SceneError::InvalidTransform, "Singular scale rejected");
    auto wrongScene = command(SetTransform{child, transform});
    wrongScene.scene = Guid::generate();
    require(document.apply(wrongScene).error == SceneError::WrongDocument, "Wrong scene rejected");
    auto futureEdit = command(SetTransform{child, transform});
    ++futureEdit.schemaVersion;
    require(document.apply(futureEdit).error == SceneError::UnsupportedSchema, "Unknown command schema rejected");

    auto change = document.apply(command(SetTransform{child, transform}));
    require(change.ok() && change.inverse && document.find(child)->transform == transform, "Set transform");
    auto undo = document.apply(command(*change.inverse));
    require(undo.ok() && undo.inverse && document.find(child)->transform == Transform{}, "Undo with inverse");
    require(document.apply(command(*undo.inverse)).ok() && document.find(child)->transform == transform, "Redo with inverse");
    require(document.apply(command(RenameEntity{child, "First Light Cube"})).ok(), "Rename preserves ID");

    const auto snapshot = document.snapshot();
    auto reopened = SceneDocument::open(snapshot);
    require(std::holds_alternative<SceneDocument>(reopened)
            && std::get<SceneDocument>(reopened).snapshot() == snapshot, "Snapshot reopen preserves the full scene");
    require(std::get<SceneDocument>(reopened).find(child)->parent == root, "Parent ID survives reopen");
    auto futureSnapshot = snapshot;
    ++futureSnapshot.schemaVersion;
    require(std::get<SceneError>(SceneDocument::open(futureSnapshot)) == SceneError::UnsupportedSchema,
            "Unknown scene schema rejected");
    const auto removed = document.apply(command(RemoveEntity{child}));
    require(removed.ok() && removed.inverse && !document.find(child), "Delete leaf");
    require(document.apply(command(*removed.inverse)).ok() && document.find(child)->transform == transform, "Undo restores deleted entity");

    auto cyclic = snapshot;
    for (auto &entity : cyclic.entities) {
        if (entity.id == root)
            entity.parent = child;
    }
    require(std::get<SceneError>(SceneDocument::open(cyclic)) == SceneError::HierarchyCycle, "Cyclic snapshot rejected");
    auto missingParent = snapshot;
    for (auto &entity : missingParent.entities) {
        if (entity.id == child)
            entity.parent = Guid::generate();
    }
    require(std::get<SceneError>(SceneDocument::open(missingParent)) == SceneError::MissingParent, "Dangling snapshot rejected");
    auto exhausted = snapshot;
    exhausted.revision = std::numeric_limits<Revision>::max();
    auto endOfRevisions = std::get<SceneDocument>(SceneDocument::open(exhausted));
    require(endOfRevisions.apply({Guid::generate(), project, scene, exhausted.revision, RemoveEntity{child}}).error
        == SceneError::RevisionExhausted, "Revision overflow rejected");
    std::cout << "Scene document contracts passed\n";
}
