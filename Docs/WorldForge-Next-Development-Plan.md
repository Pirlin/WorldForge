# WorldForge next development plan

## Eight-hour engine development session

**Goal.** Make Content Browser assets open their own Details and editor, then use the existing Playable Street as the test project for an engine workflow. Author one repair-shop interaction, run it in Editor Play and the standalone Player, and capture the result in Testing.

**Starting point.** Content Browser already offers **Edit in Studio** from a mesh tile's context menu. Activating the tile currently calls **Place Mesh**. Playable Street already runs a mission, traffic, and an interaction input in both runtimes. Forge AI already stages supported creator edits. The seven workspaces still need individual authoring and runtime checks. The [completed AI plan](WorldForge-AI-Game-Creation-Plan.md) and [Asset Search audit](../build/audit/studio-asset-search/controls.csv) record the baseline. The asset-opening interaction follows the type-specific editor pattern documented by Epic. [Epic asset activation reference](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/ContentBrowser) · [Epic asset editor example](https://dev.epicgames.com/documentation/en-us/unreal-engine/animation-sequence-editor-in-unreal-engine)

| Elapsed time | Work | Verification before moving on |
| --- | --- | --- |
| 00:00–00:25 | **Baseline.** Build the Editor and Player, open a disposable Playable Street project, and inventory the existing controls in the seven engine tabs. Confirm current Content Browser behavior and run the street fixture. | Record current failures, the build revision, and one reproducible test project. |
| 00:25–01:55 | **Content Browser and Asset Details (WF-24).** Route double-click, Enter, and **Open Asset** to one action using the asset ID and type. Start with static meshes. Open mesh-specific Details with its preview, material slots, geometry and source information, and **Edit in Studio**. Keep explicit **Add to Level** for placement. Reuse the handler in Search Assets where the same asset is available. | Open two different meshes, including an unplaced asset. Confirm each opens its own Details, opening twice focuses the same asset editor, and opening creates zero scene instances. Add to Level creates one. Check closing, stale IDs, and existing Studio drafts. |
| 01:55–03:05 | **Programming (WF-13).** Author one typed game rule in the Programming workspace using existing mission events and asset references. Use **On Repair Completed → Activate Shop Beacon** as the first rule. Save and export its event, action, and target. | In Editor Play and the Player, the event activates the correct target once. Verify undo, reopen, and a readable error for a missing target. |
| 03:05–03:50 | **UI (WF-20).** Bind a simple mission objective and completion message to live mission state. Expose the supported binding in the UI workspace. | See the objective change in Editor Play and the packaged Player. Check restart and two window sizes. |
| 03:50–04:35 | **VFX (WF-10).** Author a small repair completion effect with color, duration, and a scene attachment. Start with one short beacon pulse using the available runtime rendering path. | Preview it, trigger it from the mission rule, verify it ends, and check the saved values in the Player. |
| 04:35–05:20 | **Cinematic (WF-12).** Author one short camera shot using two timed keys. Add preview and scrub for this shot, then trigger it at repair completion. | Check the start and end views, save and reopen the keys, and return control to the player if the runtime shot is ready. |
| 05:20–06:05 | **Gameplay AI (WF-15).** Author one NPC route with a waypoint, wait, and resume state. Surface the active state in the AI tab and reuse the fixed-step runtime where possible. | Observe transitions in Play, validate a missing waypoint, and verify the saved behavior after reopening. Record Player coverage separately. |
| 06:05–06:45 | **Multiplayer (WF-16).** Verify a loopback transport path with two local instances. Start with connection status, stable player IDs, and server ownership of one interaction. If transport needs its own implementation block, record the interface and the failing two-process fixture for the next session. | Capture an actual local connection and state change or the exact gap in the fixture. A Multiplayer tab with sample values does not count as a pass. |
| 06:45–08:00 | **Testing and package (WF-22).** Run a visible Playable Street test from Testing and record per-step results. Verify asset opening, the mission rule, the HUD, and every runtime slice that landed. Package the Editor with Studio and Player and rerun the baseline regressions. | Hand off the package path, test project, pass/fail results, logs, and each unverified area. Verify the Editor and Player on this machine. Track clean Windows verification as a release check. |

### Completion criteria and scope

- **Primary deliverables.** Finish the Content Browser asset-opening path and one saved gameplay rule that can be exercised in the packaged Player. Keep the final validation block available even if an earlier task overruns.
- **Engine integration.** Persist edits by stable project and asset IDs, use the existing undo path for authoring changes, and share supported gameplay evaluation between Editor Play and Player. Start with the existing street fixture and the smallest asset type each workspace can support.
- **Remaining blocks.** Each timebox targets one verifiable workflow. If a runtime dependency takes longer, record the failing fixture, finish the work it unblocks, and continue with independent blocks. Record a local prototype as such. Full particle graphs, Sequencer parity, native C++ gameplay modules and hot reload, general NPC behavior tools, multiplayer replication coverage, and a full UI designer are follow-up development.
- **Evidence.** Keep the test scene, actual output from each check, and the packaged build together. Run core tests and the existing Studio publishing, Forge AI, and Playable Street audits for affected behavior. Follow the [Windows build instructions](../README.md#build-from-source) to bundle Studio.

### Implementation and local verification

The first engine workflow is in the [WorldForge Engine Session package](../dist/WorldForge-Engine-Session/bin/WorldForgeEditor.exe). The [engine session audit](../build/audit/engine-session-final/controls.csv) passed 13 checks on the GTX 1650 and passed again [at 125% scaling](../build/audit/engine-session-final-dpi125/controls.csv). The scene fixture is [engine-street.wfplay](../build/audit/engine-session-final/engine-street.wfplay), with a matching [standalone Player report](../build/audit/engine-session-final/engine-street-player.json).

1. Open **World Editor** and double-click a static mesh in Content Browser. Check its preview, material slots, source, and placement count in Asset Details. Use **Edit in Studio** or **Add to Level** from that panel when needed.
2. Create or open a **Playable Street** project. In **Programming**, bind Repair Run to the Repair Counter beacon and apply it. In **VFX**, set the color and duration. In **Cinematic**, pick a target and scrub between the two camera keys before applying. In **UI**, set the completion message and enable the live objective.
3. In **AI**, choose two scene markers, preview the states, and apply the patrol. In **Multiplayer**, run the two-client loopback check to confirm connection, authority for one interaction, and matching beacon state. The check exercises the local protocol with separate client processes.
4. Save the project. In **Testing**, run **Run Playable Street Check**. The check reports mission, shop access, traffic, beacon expiry, camera completion, and patrol progress. It then exports the test scene and runs the installed standalone Player.

The Editor exports playable format v6 for the authored gameplay settings. The new Player also completed the previous [v5 street fixture](../build/audit/engine-session-first/legacy-v5-player.json). Invalid beacon and camera targets stop export with a panel-specific message. The packaged [Studio publishing](../build/audit/engine-session-regression-publish/controls.csv), [Forge AI](../build/audit/engine-session-regression-forge/controls.csv), and [Playable Street](../build/audit/engine-session-regression-street/controls.csv) regressions passed 48 checks combined. Clean Windows release verification remains open.

## Follow-up roadmap

### Reusable base-color assets

The [Surface Library build](../dist/WorldForge-Surface-Library/bin/WorldForgeEditor.exe) adds project-local PNG and JPEG textures and reusable base-color materials. In **Content Browser → Add** or **Search Assets**, choose **Import Texture** or **New Material**. Open Texture Details for a preview and size. In Material Details, choose a texture and tint, **Save**, then **Assign to Mesh** and select a material slot. A material edit updates each linked slot in one undoable operation.

The [surface library audit](../build/audit/project-surface-library/controls.csv) verified two linked meshes, undo and redo, reopen after deleting the source files, Content Browser and Search Assets, and texture export to the [standalone Player](../build/audit/project-surface-library/surface-library-player.json). Geometry publishing keeps links when Studio returns the same unique material names. Publishing rejects a missing or ambiguous material match. Refreshing materials inside older editable Studio packages needs a separate round-trip check.

| Priority | Work | Check before calling it complete |
| --- | --- | --- |
| 1 | **Asset Hub and reimport.** Search by usage, preview source changes, list affected slots and instances, and handle missing files. | Reimport while preserving asset IDs and placement. Verify the selected local-edit policy with modified and corrupt sources. |
| 2 | **Reusable PBR maps.** Extend the material library to normal, metallic/roughness, occlusion, and emissive maps. | Validate known color and data fixtures across the Editor, Studio handoff, and Player. |
| 3 | **Modeling and UV round trips.** Verify named UV sets, material slots, normals, and a supported modifier stack on an original hard-surface prop. Add diagnostics for UV, LOD, and collision changes. | Reopen editable source, compare evaluated output, and verify shared and unique copies, undo, and Player export. |
| 4 | **Forge AI viewport preview.** Show the staged mesh, material, and position before applying. | Apply, cancel, and undo the preview. Change the target while the preview is open and verify stale results are rejected. |
| 5 | **Sculpt-to-game.** Publish an optimized mesh from a saved sculpt, with reduction, normal, UV, and bake checks. | Reopen both forms and verify silhouette, baked maps, and triangle budget in the Player. |
| 6 | **Character, Rigging, and Animation.** Carry one skeleton, skinned mesh, and authored clip from Studio into the runtime. | Bind and pose, reopen, then compare playback at known frames in Editor and Player. Verify an invalid rig is rejected. |
| 7 | **Procedural assets and scene tools.** Publish one Geometry Nodes asset with evaluated geometry and dependency diagnostics. Build one usable VFX or Cinematic workflow on that handoff. | Reopen the source, change a parameter, and confirm placed instances update to the expected geometry. |
| 8 | **Forge AI language-model integration.** Connect a provider to the typed command interface. Add target clarification, editable plans, and an applied-command record. | Exercise ordinary, ambiguous, invalid, and interrupted requests. Approved edits apply and undo together. |
| 9 | **Playable Street to Living City.** Expand the original street to a compact city block with two interiors, connected traffic, a pedestrian route, and a mission across the block. Expand streaming and simulation based on measured bottlenecks. | Run a repeatable Player route and compare frame time and memory to the GTX 1650 baseline. Verify traffic, interiors, and mission progress after reopen. |
| Release | **Windows and workspace QA.** Produce a reproducible Editor, Studio, and Player package with matching source and notices. Test install, upgrade, recovery, offline startup, and uninstall on an independent machine. | Record Windows and GPU details, package identity, Editor and Player outputs, keyboard and DPI behavior, and any unresolved reproduction steps. |

### Asset Hub, materials, and sculpt delivery

Open the [WorldForge Asset Hub build](../dist/WorldForge-Asset-Hub/bin/WorldForgeEditor.exe).

- **Asset Hub.** Search names, source filenames, and references. A texture lists the project materials and mesh slots that use it. A mesh lists its level placements. **Preview Reimport** shows the current and proposed source, evaluates retained modifiers and LODs, and reviews collision changes. The edit preserves asset IDs and placed instances. Replacing local mesh edits requires a separate confirmation and can be undone.
- **Materials.** Material Details accepts base color, normal, packed metallic and roughness, occlusion, and emissive textures. Packed metallic uses blue, roughness uses green, and occlusion uses red. One edit updates linked slots. World Editor and Player render these maps. The playable format is now v7 and still reads older exports. The Studio GLB handoff preserves the five image channels and factors.
- **Modeling delivery.** Mesh reimport retains named UV sets, modifier and LOD settings, and linked instances. A topology change flags affected inactive UV sets for rebuilding. Synchronizing older editable Studio sources with newer library materials still needs separate exchange work.
- **Forge AI.** After **Generate Preview** on a barrier plan, select **Preview in Level** to inspect the staged placement, materials, and changes to existing copies. Close the preview to continue reviewing. **Apply as One Change** commits once and supports undo. If the asset or entrance changes during review, generate a new preview.
- **Sculpt delivery.** In embedded Sculpting, choose **Publish Game Mesh**, enter a triangle budget, and enable **Bake Sculpt Detail** when the mesh has usable UVs. Studio produces the evaluated game mesh and preserves the editable sculpt separately. The bake uses a 512 × 512 tangent-space normal map and a Principled material. Project-linked materials retain their registered maps. The editor warns when a project material would replace a Studio normal map.

The [asset, modeling, material, and sculpt audit](../build/audit/asset-hub-final-surface/controls.csv) covers reimport, saved material links, and the exported Player scenes. The [Studio sculpt fixture](../build/audit/asset-hub-sculpt-bake/result.json) reduced a four-level sculpt from 3,072 to 600 triangles, baked the normal map, and reopened the editable source. The [Forge AI preview audit](../build/audit/asset-hub-full-forge-ai/controls.csv) checks viewport rendering, cancellation, stale targets, apply, and undo. The [Studio publishing](../build/audit/asset-hub-full-studio-publishing/controls.csv), [Playable Street](../build/audit/asset-hub-full-street/controls.csv), and [engine session](../build/audit/asset-hub-engine-session/controls.csv) regressions ran on the GTX 1650 development PC.

The [authoring audit](../build/audit/asset-hub-final-authoring/controls.csv) passed 176 checks, including the embedded Modeling entry point, native sculpt cancellation and undo, terrain, asset delivery, and Player scene export. The standalone Player also passed its software-rendering smoke test.

### Named UV layouts and character playback

Open the [Studio Exchange build](../dist/WorldForge-Studio-Exchange/bin/WorldForgeEditor.exe).

- **Named UV layouts.** Open a static mesh in Studio, edit its named UVs, and choose **File → Publish to WorldForge**. Apply the staged update from the Editor toolbar. The render UV layer drives material sampling in World and Player. Other named layouts remain editable. If a published mesh leaves out a previous layer, Asset Details lists it as **Rebuild required**. Apply WorldForge modifiers before opening the native mesh in Studio.
- **Character playback.** Select the weighted mesh of a rig in Studio, assign one active Action to its armature, and use **File → Publish Character Clip**. Apply the staged update, return to World Editor, and press **Play**. The editable Studio source stores the armature, weights, and Action. WorldForge interpolates the sampled positions and normals and loops the clip in Editor Play and Player. This profile supports one armature, one active Action, 2–120 sampled frames, and a bounded sample budget. Runtime retargeting, blending, and root-motion control remain separate work.
- **Playable Scene.** Version 8 stores sampled animation. The Player continues to read earlier scene versions.

The [named UV audit](../build/audit/exchange-studio-uv/controls.csv) includes a texture layout, a lightmap layout, a trim material, two placed instances, and an intentionally omitted layout. The [character audit](../build/audit/exchange-studio-character/controls.csv) verifies weight validation, project and Studio reopen, two Editor Play poses, and loop playback in the [standalone Player](../build/audit/exchange-studio-character/studio-character-player.json).

The package passed 10 new checks and 256 regression checks on the GTX 1650 test PC. The regressions cover [authoring](../build/audit/exchange-authoring/controls.csv), material and sculpt delivery, engine gameplay, Forge AI, Playable Street, and Studio publishing. The packaged Player also completed the earlier [version 7 playable scene](../build/audit/exchange-legacy-v7-player.json).

Geometry Nodes workflows, a language-model provider, the Living City expansion, and clean Windows release verification follow in roadmap order.

### Programming workspace

See the [Programming plan](WorldForge-Programming-Plan.md) for object selection, the gameplay library, and the remaining component and runtime work.

Launch the [Programming build](../dist/WorldForge-Programming/bin/WorldForgeEditor.exe) and open **Programming → Behaviors**.

- Create a **Behavior** asset in Programming or **Content Browser → Add → New Behavior**. Choose **Play started**, **Mission completed**, or **Interact nearby**, then optionally require a completed mission. Select an object and use **Details → Behavior** to attach the asset. Multiple objects can share one asset.
- **Pulse beacon** places a timed effect on the attached object. **Raise object** lifts a root mesh and its Static collider by a specified number of meters. Use a mesh without physics joints or child colliders. Each attachment can fire once per Play session. An interaction blocked by a mission condition can be retried once the mission completes.
- Run **World Editor → Play** and inspect **Output Log** for each action or failed condition. The **Mission Beacon** tab retains the original built-in mission form. Playable format v9 carries attached behaviors to the standalone Player.
- **Programming → C++ Behavior Project** generates `Behavior.vcxproj`, `Behavior.cpp`, and `WorldForgeBehavior.h` under the project’s Programming folder. Open the project in Visual Studio 2022 for editing and debugger breakpoints in the authoring program. **Build and Preview C++ Behavior** compiles and runs it to produce a typed rule. Review it, apply it as an asset, then attach the asset. Editor Play and Player run the resulting rule. Runtime-native game modules and source breakpoints during game execution are further development.

The [Programming audit](../build/audit/programming-programming/controls.csv) checked the asset editor, project reopen, undo and redo, C++ build and apply, failed build recovery, runtime event logging, and door collision. The [standalone Player report](../build/audit/programming-programming/programming-street-player.json) recorded all three behavior attachments firing on the GTX 1650.

All 11 Programming checks and 266 regression checks passed across the engine session, surface library, Studio UV and character exchange, Forge AI, Playable Street, Studio publishing, and authoring suites. The three core and standalone CTest cases also passed. The [v8 compatibility test](../build/audit/programming-legacy-v8-player.json) completed the mission and looped the character animation using the updated Player.

### Additional project tracks

- **Game UI and accessibility.** Build one mission objective HUD and rebindable controls for the street. Verify keyboard navigation, scaling, persistence of bindings, and updates as mission state changes in the Player.
- **Testing and build automation.** Export the same street from a known project revision, validate its assets, and archive the playable package with its test report. Verify the build explains a missing dependency and never replaces a valid output with an incomplete package.
- **Multiplayer.** After the single-player mission and persistence contracts stabilize, prototype two players entering a shared street and interacting with one replicated object. Test join, reconnect, and conflicting interactions.
- **Collaboration.** Build on stable asset IDs and revisions to show which assets changed and who affected them. Exercise concurrent material edits and a recoverable conflict on a shared test project.

## Source plans

| Document | Role in this schedule |
| --- | --- |
| [AI Game Creation Plan](WorldForge-AI-Game-Creation-Plan.md) | Completed barrier and street scope. Current AI, preview, and exchange follow-ups. |
| [Studio Implementation Plan](WorldForge-Studio-Implementation-Plan.md) | Authoring round trips, runtime handoff, and Windows release. |
| [Modeling Production Plan](Modeling-Production-Plan.md) and [Modeling Source Review](Modeling-Source-Review.md) | Geometry and UV fixtures, Blender and Maya workflow comparisons, and modeling performance. |
| [Creator Workspace Plan](Creator-Workspace-Plan.md) | Workspace layout, shared asset discovery, and interaction acceptance. |
| [Development Audit](Development-Audit.md) | Historical visual, runtime, and control test evidence. |
| [Development Drive handoff](../Development%20Drive/START_HERE.md) and [master blueprint](../Development%20Drive/WorldForge_Master_Blueprint_v3.md) | Long-term product scope, screen IDs, the capability matrix, and the engineering backlog. |
