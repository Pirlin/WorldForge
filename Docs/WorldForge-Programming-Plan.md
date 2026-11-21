# WorldForge Programming

The Programming workspace uses the same scene selection and project assets as World Editor. A behavior is reusable across objects. Its attachment identifies the object whose gameplay state the behavior acts on.

| Phase | Implementation | Acceptance |
| --- | --- | --- |
| 1. Object workflow | Show the selected object and its attachment in Programming. Find objects by name, type, or group. Open, assign, or remove a behavior there. Keep the Scene Outliner and Details in sync. | Switch between two objects with different attachments. Search for a mission marker. Undo an attachment change and reopen the project. |
| 2. Gameplay library | Provide typed C++ helpers for lifecycle events, interaction, mission conditions, scene effects, transforms, and physics. Keep one runtime implementation for Editor Play and Player. Start with beacon, raise, and launch functions. | Build a project using the shipped headers. Run the compiled rule against a real physics object in both runtimes. Verify an incompatible object produces a useful error. |
| 3. Behavior components | Attach an ordered list of behavior assets. Give each attachment an enabled flag, an optional scene target, and an optional typed value in seconds, meters, or meters per second. Read existing single attachments as one component. | Attach a beacon and door lift to one object. Give two doors different lift heights. Preserve a renamed target and the ordered list through save, undo, and export. |
| 4. Native gameplay modules | Define a versioned C-compatible host interface with world and object handles, component lookup, and lifecycle callbacks. Start with transform, input, time, and diagnostics. Add Visual Studio attach and package the exact same module for Player. | Run a user-authored C++ component through Editor Play and a standalone build. Verify source breakpoints, ABI mismatch diagnostics, and clean Windows startup. |
| 5. Reload and broader APIs | Stage updated modules at a safe simulation boundary. Keep a usable prior build on compiler or load failure. Expand into spawning, triggers, physics queries, UI, audio, animation, saves, and replication. | Exercise reload with a live object, a failed build, and a changed component schema. Add one end-to-end sample and Player test per library. |
| 6. Visual programming | Author event and function graphs against the same typed component APIs. Add graph validation, parameter editing, action previews, and per-instance runtime traces. | Build the same interaction in code and in a graph. Verify equivalent runtime state, readable errors, and packaged behavior. |

## Native runtime, engine APIs, and graphs

| Order | Deliverable | Release check |
| --- | --- | --- |
| 4A | Windows C++ project, versioned host interface, Play and fixed-update callbacks, debugger symbols, and explicit standalone game packaging. Expose input, logging, the player, and physics bodies identified by stable scene IDs. | Build with Visual Studio 2022, run a module that moves a real body in both runtimes, and verify a plain scene open executes only its built-in rules. |
| 4B | Replace modules between fixed updates after checking the new module's ABI. Keep the current module active when the build or validation fails. Start replacement module state fresh while retaining live world state. | Replace a module in the same Play session, check retained transforms, reject a bad version, and verify debugger source breakpoints in Editor and Player. |
| 5A | Add instance-scoped components, typed properties, stable object and asset references, transform APIs, entity spawning and destruction, input actions, triggers, queries, and profiling. | Exercise a reusable door, spawned object, and trigger in Editor and Player with deterministic fixtures. |
| 5B | Extend the runtime APIs to audio, animation, UI, saves, and multiplayer. Define ownership, lifecycle, thread, and replication contracts as each subsystem lands. | Package and test one complete gameplay flow per subsystem, including save migration and two-process replication where applicable. |
| 6A | Introduce a typed graph format and operation registry, an event-to-action graph editor, reusable functions, validation, and a trace for the selected object instance. | Build one graph equivalent to a C++ interaction and verify matching runtime state, errors, and exported behavior. |
| 6B | Add graphs for more engine APIs, structured control flow, graph breakpoints, state inspection, migration, and release validation. | Step through a graph in Play, verify repeatable outputs and old projects, and package a clean Windows build. |

Each module ABI version defines its data size and callback contract. Pass fixed-size values and opaque handles across the boundary. The runtime owns the scene and physics objects. Native code must come from a trusted build or a game package the player explicitly launches.

## High-priority implementation sequence

| Stage | Deliverable | Required verification |
| --- | --- | --- |
| 1. Native components | Register component types and property schemas from the gameplay DLL. Attach instances to scene objects and expose Boolean, integer, real, object reference, and typed asset reference fields. Store stable type, property, object, and asset IDs. Apply component edits through scene undo. | Attach the same type to two objects with different settings. Check rename, undo and redo, reopen, defaults, missing references, and equivalent Editor Play and Player results. Reject a reload that cannot satisfy the live component schema. |
| 2. Runtime object API | Introduce runtime object handles separate from physics handles. Add component lookup, transforms and parenting, prefab spawn, and deferred destruction. Publish a typed operation registry shared with graphs. | Exercise multiple spawned objects, stale handles, parent transforms, deferred destruction, and deterministic playback in both runtimes. |
| 3. Physics and input | Add named input actions, overlap and trigger enter/exit events, ray and shape queries, layers and filters, and collision callbacks. Define fixed-update ordering and command budgets. | Run door, interaction, projectile, and trigger fixtures with rebinding, collision filtering, and saved scene references. |
| 4. Game services | Extend the registry with scene audio, animation parameters and events, UI properties and events, and versioned save data. Build a working example for each service. | Verify audio and animation callbacks, UI input and focus, save/reload, missing assets, and migration in a packaged game. |
| 5. Multiplayer | Add authority, ownership, replicated properties, remote calls, and object spawn/despawn replication. Keep local, replicated, and save properties explicit. | Verify two actual processes, late join, reconnect, invalid ownership, conflicting actions, and replay of replicated state. |
| 6. Visual graph runtime | Build an editable typed graph with free wiring, variable scopes, branches, reusable functions, schema validation, serialization, and the shared operation registry. Add stable node identifiers and execution budgets before loops or recursion. | Round-trip graph assets and compare C++ and graph versions of an interaction. Test invalid connections, branch paths, function parameters and returns, and deployment. |
| 7. Debugging and release | Add per-instance graph breakpoints, step-through, variable inspection and trace filtering. Verify Visual Studio source breakpoints, module reload, graph migrations, and fresh-machine packaging. | Stop at an actual breakpoint in each editor, resume and step, inspect two instances independently, run approved regression fixtures, and validate a clean PC build. |

Stages 2 and 3 provide the initial shared operations for stage 6. Graph authoring can advance alongside game services after that contract is stable. Multiplayer replication builds on stable runtime object and component identities, deferred scene operations, and versioned property names.

### Native component workflow

Launch the [Native Components Editor](../dist/WorldForge-Native-Components/bin/WorldForgeEditor.exe). Open **Programming → Native C++ → C++ Gameplay Module** and build and activate the generated project. Select a scene object, then use **Native Components** in Details or **Edit Selected Object's Native Components** in Programming. Choose a type, edit its fields, and attach it. It is one undoable object edit. On the generated sample, **Launch Body** can act on the selected object or a referenced physics body. Use **Upward speed** and **Speed percent** to give two instances different results.

Game code adds a `WfGameGetComponentsV1` export using `WorldForgeNativeComponents.h`. The optional export adds instance callbacks to the existing module API. Each type declares stable property IDs, field kinds, defaults, and lifecycle callbacks. The editor installs the new header alongside existing project source. For an existing V1-only project, add the component export to your own source. New projects include the Launch Body sample.

Object, mesh, material, and texture refs carry stable IDs. A native callback can read these IDs and look up a physics body by object ID. Asset loading and streaming are separate APIs in the roadmap. Unset fields use the DLL schema defaults. Play and Export validate enabled components and their explicit references. Removing an active type or changing an overridden property's kind rejects the new module. Authored component values use project manifest schema 6 and packaged component scenes use playable format v13. Existing V1-only modules continue to run in scenes without native components.

All 16 [native gameplay and component checks](../build/audit/native-components-release-native-gameplay/controls.csv) passed. The packaged [v13 Player fixture](../build/audit/native-components-release-native-gameplay/native-components-player.json) moved the two active crates by 0.79 and 4.89 meters vertically and recorded no upward movement on the third. The checks covered reference kinds and IDs, integer precision, rename, undo and redo, reopen, reload rejection, and a missing reference blocking export.

The [Programming](../build/audit/native-components-release-programming/controls.csv) and [engine session](../build/audit/native-components-release-engine-session/controls.csv) suites passed 35 checks. Three core and standalone tests passed. The new Player also completed the [v12 fixture with a legacy DLL](../build/audit/native-components-release-v12.json).

### Windows native gameplay workflow

Launch the [Native Gameplay Editor](../dist/WorldForge-Native-Gameplay/bin/WorldForgeEditor.exe).

1. Open **Programming → Native C++ → C++ Gameplay Module** and choose **Open in Visual Studio**. The project generates `NativeGameplay.cpp`, `NativeGameplay.vcxproj`, and a copy of the SDK header. For the sample launch, select a Dynamic physics mesh and copy the selected object ID into `targetId` in the generated source.
2. Choose **Build and Activate**, then start **World Editor → Play**. Native code receives `start`, `fixed_update`, and `stop` callbacks. The Editor Output Log displays messages from the module. A compile error or an unsupported ABI leaves the last working module active.
3. While Play is running, open **Tools → C++ Gameplay Module** and build again. The next fixed update stops the previous module and starts the replacement. Scene transforms and physics state stay live. The replacement starts with fresh C++ static state.
4. Choose **Export C++ Game** and launch `WorldForgePlayer.exe` from the generated folder. It loads that package's scene and DLL. Opening or passing a standalone scene file uses its built-in rules. The Player also accepts `--module <path>` after an explicit scene path. Packages include the redistributable installer and engine license notices when present in the Editor distribution.
5. In Visual Studio, attach the **Native** debugger to `WorldForgeEditor.exe` before starting Play or rebuilding. For Player startup breakpoints, set the generated C++ project's debugger command to the packaged `WorldForgePlayer.exe` and start it from Visual Studio. The package includes `Gameplay.pdb` when available, and the host copies matching symbols next to its loaded DLL copy.

The first native API exposes the player, body lookup by scene object ID, position and dynamic-body queries, bounded velocity changes, fixed-step time, movement and interact input, and diagnostics. It runs on the simulation thread. The header keeps ownership inside the engine and C++ exceptions must stay inside the DLL. Custom dependencies and cross-platform builds are additional SDK work.

### Behavior graph workflow

In **Programming → Behaviors**, select a behavior asset and choose **Open Graph**. The editable Event, Condition, and Action nodes save the same behavior data used by the form and the C++ rule helpers. Attach the asset to a scene object and use **Output Log** to see its action or failed condition by object. This graph supports a single event, an optional mission condition, and one action. Free node wiring, branching, reusable graph functions, and graph breakpoints follow in phases 6A and 6B.

The [native runtime audit](../build/audit/native-release-final/controls.csv) covers Visual Studio build output, startup, fixed-update hot reload, bad ABI rejection, build errors, packaging, and the distinction between a game package and a plain scene open. The [Player report](../build/audit/native-release-final/native-player.json) measures module callbacks and the test crate's movement. The [Programming audit](../build/audit/native-release-programming/controls.csv) checks editing the graph and executing its saved action. Generated PDBs were verified locally. Interactive breakpoint binding in Visual Studio and execution on a clean Windows PC are release checks.

Unreal's Actor and Actor Component APIs and Unity's GameObject and Component APIs provide the reference for object-owned scripts and shared engine functions. [Unreal Gameplay Framework](https://dev.epicgames.com/documentation/unreal-engine/gameplay-framework-in-unreal-engine) · [Unity Component.GetComponent](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/Component.GetComponent.html)

Unreal instantiates an Actor's components per placed object and exposes instance properties through `UPROPERTY`. Unity attaches `MonoBehaviour` components to GameObjects and serializes exposed fields for each instance. WorldForge applies the same distinction between reusable asset definitions and per-object settings. Its current fields cover the supported behavior actions. General property reflection belongs with native component APIs and the graph editor. [Unreal component instancing](https://dev.epicgames.com/documentation/en-us/unreal-engine/basic-components-in-unreal-engine) · [Unreal property specifiers](https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-uproperties) · [Unity MonoBehaviour](https://docs.unity3d.com/2023.1/Documentation/ScriptReference/MonoBehaviour.html) · [Unity SerializeField](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/SerializeField.html)

## Using the Programming workspace

Launch the [Programming Components Editor](../dist/WorldForge-Programming-Components/bin/WorldForgeEditor.exe) and open **Programming → Behaviors**. Search by object name, type, or group. Selecting a result updates the Scene Outliner and Details. Open **Manage Components** from either workspace, select a behavior asset, and choose **Add Component**. Edit the selected component, move it up or down, then save the list as one undoable change. **Open Attached Behavior** edits the shared asset. Each attached object can override the asset's value.

Use **Action target** to control another scene object. For example, a nearby interaction can start at a switch and raise a door. Targets are saved by object ID and survive a rename. An enabled component pointing to a deleted or hidden target reports an error at Play or export. A disabled component is skipped.

Open **C++ Behavior Project** to generate or locate the Visual Studio project. Existing projects can switch their include from `WorldForgeBehavior.h` to `WorldForgeGameplay.h` to use the new helpers. Build and preview the rule, apply it to a behavior asset, and attach the asset to the intended object.

| Library | Available calls |
| --- | --- |
| Events | `events::playStarted()`, `events::interactNearby()`, `events::missionCompleted(markerId)` |
| Effects | `effects::pulseBeacon(seconds)` |
| Scene | `scene::raise(meters)` for an unparented Static physics mesh |
| Physics | `physics::launchUpward(metersPerSecond)` for an unparented Dynamic physics mesh |

Pass an optional completed mission marker ID as the fourth argument to `makeBehavior`. The C++ build produces the rule that Editor Play and Player execute. An ordered stack or value override uses project manifest schema 5. The Player reads prior formats and uses v11 when a launch action targets a different object from its component owner.

```cpp
#include "WorldForgeGameplay.h"

int main()
{
    using namespace worldforge;
    return publish(makeBehavior("Launch Crate", events::playStarted(), physics::launchUpward(8.0f)));
}
```

All 20 [Programming checks](../build/audit/programming-components-final-programming/controls.csv) passed. They covered the scene picker and Details, ordered components, undo and redo, project reopen, a renamed target, a disabled component, Visual Studio builds and diagnostics, and physics in both runtimes. The [Player report](../build/audit/programming-components-final-programming/programming-street-player.json) recorded a 3.14-meter rise and five fired behaviors on the GTX 1650. The updated Player also completed its v8, v9, and v10 compatibility runs.

The [engine session](../build/audit/programming-components-engine-session/controls.csv), [authoring](../build/audit/programming-components-final-authoring/controls.csv), and [Studio publishing](../build/audit/programming-components-studio-publishing/controls.csv) suites passed 198 checks. Three core and standalone tests also passed. Programming now rebuilds the Visual Studio behavior project before previewing it, so a source edit or compiler error is reflected in the preview.
