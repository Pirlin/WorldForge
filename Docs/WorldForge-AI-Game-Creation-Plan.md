# WorldForge AI-Assisted Game Creation Plan

**Status: Complete.** All four phases were implemented and passed their automated checks on the GTX 1650 development machine. The delivered milestone covers one barrier and one playable street.

WorldForge should let a creator describe an asset or gameplay change, edit it by hand in Studio or World Editor, and test it in a playable game. This plan starts with one concrete road barrier and expands to one playable street. A large open-world game is the long-term benchmark. The street is the first test of whether the workflow scales across art, gameplay, and the runtime.

## Starting point

- The embedded Blender-based Studio already hosts the authoring workspaces. **Edit Selected Mesh in Studio**, **Publish to WorldForge**, and **Apply Studio Update** provide an existing static-mesh publishing path.
- Studio saves editable `.wfasset` revisions. The existing publishing audit covers two materials, UVs, multiple placed instances, undo and redo, stale revision rejection, and reopening. The barrier adds cross-workspace sculpting and texture painting to that path.
- The starter cube opens in Sculpting with detail ready to use. The barrier must exercise an asset selected in an actual project, including the low-poly preparation and the game mesh that gets published.
- The Forge AI barrier command uses one UV set and meshes without WorldForge modifiers. Manual Studio publishing can exchange named UV layouts.

See the [Studio implementation plan](WorldForge-Studio-Implementation-Plan.md) for the integration baseline and the [development audit](Development-Audit.md) for prior verification.

| Phase | Deliverable | Depends on |
| --- | --- | --- |
| 1 | One selected asset across Studio and World Editor, with a verified publish and reopen | Existing Studio integration |
| 2 | A short, selection-aware manual workflow for the barrier | Phase 1 |
| 3 | One prompt that produces a reviewable, undoable barrier edit | Phases 1 and 2 |
| 4 | A playable street that proves coordinated AI-assisted edits | Phase 3 |

## Completed work in build order

| Order | Work | Verified |
| --- | --- | --- |
| 1 · Phase 2 | **Bevel Edges** and **Set Pivot** in the Modeling header. | Modal bevel previews and Esc restores the mesh. An applied bevel undoes and redoes to the same vertices, and scaling uses the 3D cursor pivot. [Result](../build/audit/final-modeling-actions/result) |
| 2 · Phase 2 | **Draw**, **Smooth**, and **Add Sculpt Detail** in the Sculpting header, with a reason when detail cannot be added. Texture Paint names the material and image that will receive paint. | Adding detail keeps the 8 base vertices. A stroke and the added detail each undo and redo. The paint target follows the active image node, and painted pixels reopen from the published source. [Sculpt](../build/audit/final-sculpt-actions/result) · [Paint](../build/audit/final-paint-target/result) |
| 3 · Phase 2 | The publish review names the asset and each affected placement with its collider. It warns when the mesh changes size, when custom collision bounds no longer cover the mesh, and when a placement will not block the player. It blocks empty and invalid meshes. | 20 checks in the [publishing audit](../build/audit/final-studio-publishing2/controls.csv), including cancel, undo and redo, stale revisions, corrupt input, workspace switches in live Studio, and the barrier stopping the player. |
| 4 · Phase 3 | **Forge AI** in the World Editor toolbar plans a barrier request as four typed commands. It runs the bevel and stripe paint in a background Studio job on a working copy and applies the result as one undoable change. | 13 checks in the [Forge AI audit](../build/audit/final-forge-ai/controls.csv). See Phase 3. |
| 5 · Phase 4 | The **Playable Street** starter template, traffic and missions in editor Play and the standalone Player, base-color textures in the Player, and coordinated street requests in Forge AI. | 12 checks in the [street audit](../build/audit/phase4-street-final/controls.csv). See Phase 4. |

## Follow-up development and release verification

The [next development plan](WorldForge-Next-Development-Plan.md) schedules eight hours of Content Browser and engine development and records the reusable asset work that follows.

- **Asset library follow-ups.** Search Assets and Content Browser now support reusable base-color textures and materials. The next asset workflow is Asset Hub usage tracking and reimport. Reusable PBR maps and older Studio source round trips are tracked in the [next development plan](WorldForge-Next-Development-Plan.md#reusable-base-color-assets).
- **Clean Windows PC.** The Editor-to-Player workflow has not been run on a clean Windows install. It needs a separate machine or VM. See the [Windows release checklist](WorldForge-Studio-Implementation-Plan.md#phase-6--release-and-maintain-the-windows-build).
- **Forge AI understands a fixed set of requests.** Its planner is rule-based. It reads barrier style requests and street requests that move the repair shop entrance or route traffic. Requests outside that set get an explanation of what is supported. No language model is connected yet. The typed commands are the interface a model would produce.
- **No viewport preview before apply.** The Forge AI preview shows the review text, the stripe texture, and the target position. It does not draw a ghost of the barrier in the viewport before it is applied.
- **Studio exchange limits.** Assets with several UV sets or WorldForge modifier stacks are still rejected by Studio editing and Forge AI jobs until their round trips are verified.

## Phase 1 — Carry one asset through every workspace

**Outcome.** Select a placed barrier in World Editor, edit the same asset in Modeling, Sculpting, and Texture Paint, then publish it back into the level.

- [x] Create a small QA level with two instances of one barrier asset at distinct transforms and one unrelated prop. Record their asset IDs, transforms, material assignments, and collision settings.
- [x] Route the selected mesh into the requested Studio workspace. Track the project, stable asset ID, and expected published revision through the session. Reuse the session when switching tabs for that asset. If the user selects another asset while edits are unsaved, give them a way to keep working or save before switching.
- [x] Make a small modeling edit, add sculpt detail and a visible stroke, then paint part of the surface. Keep the editable mesh and paint data in the same source revision. Verify what the runtime can represent, including UVs, material slots, supported texture maps, and evaluated geometry.
- [x] Preview and publish through the existing Studio exchange. Verify that every instance of the barrier updates in place. Preserve per-instance transforms and related scene references. Keep the existing collider settings and surface any collision bounds that need review.
- [x] Test undo and redo, save and reopen the project, reopen the editable source, and run Play. Exercise a stale revision, a missing texture, and an invalid mesh payload. Verify that the rejected publish leaves the last successful revision usable.

**Verified in the Phase 1 build.** Studio performed sculpt and Texture Paint brush strokes, packed the unsaved paint pixels, and reopened the editable source with the paint intact. The editor audit confirmed the same named asset across Studio tabs and rendered the published paint on both placements after reopening. Blender shortcuts were checked in the embedded viewport with Tab, an axis-constrained move, Ctrl+Z, and F3 search. In-editor Play used the World Editor base-color maps. Phase 4 added base-color map support to the standalone Player. See the [live paint test](../build/audit/phase1-studio-live-paint-a0fc5c/result) and [editor audit](../build/audit/phase1-live-paint-editor/controls.csv).

**Pass when** both barrier instances show the sculpt and texture after reopening, the original placements and unrelated prop are intact, the source can still be edited, and Play loads the published result.

## Phase 2 — Make the manual workflow easy to discover

**Outcome.** A creator can finish the barrier using a small set of actions that appear where they are needed.

**First action verified.** The Texture Paint viewport shows **Create Paint Layer** when the active material has no paint image and the mesh has UVs. It opens the base-color image setup and selects the resulting canvas. Modeling preserves the original polygon boundaries on World Editor handoff. **Restore Box Faces** repairs compatible cubes saved with the older triangulated topology. The repair passed undo and redo in the editor and republishing in the Studio fixture. See the [topology verification](../build/audit/studio-topology-test.log).

**UV setup verified.** In Texture Paint, **Set Up UVs** unwraps the active editable mesh when the active map is missing or all of its faces have collapsed UVs. It keeps an existing usable map, the current paint mode, and the mesh selection. The Studio fixture verified missing and collapsed maps and reopened the generated UVs from the published source.

**World Editor publishing.** **Apply Studio Update** appears beside Help while working on a Studio asset. The Vulkan selection stroke is dark purple and draws at four pixels wide at 100% display scaling. The audit verified equal outline coverage before and after GLB triangulation and confirmed that deselection removes the stroke. It also checked the toolbar while switching through Sculpting, Texture, and Modeling. See the [viewport capture](../build/audit/phase2-outline/triangulated-selection.png) and [publishing audit](../build/audit/phase2-outline/controls.csv).

| Workspace | First actions | Behavior to verify |
| --- | --- | --- |
| Modeling | Bevel Edges, Set Pivot | Use the current selection and preview dimensions or pivot placement before applying. |
| Sculpting | Add Sculpt Detail, Draw, Smooth | Explain when a mesh needs more detail. Preserve the base mesh when adding detail and verify undo and redo on the sculpted surface. |
| Texture Paint | Set Up UVs, Create Paint Layer | Reuse valid UVs where possible, show which material and image will receive paint, and verify that the saved paint data publishes. |
| World Editor | Preview Update, Publish, Play | Show which asset and instances will change. Check appearance and collision in the level. |

- [x] Give each action a predictable entry point, a short description, relevant controls, and a clear reason when it is unavailable. Keep Blender tool search and shortcuts available for advanced operations.
- [x] Back each action with a named operation that can be invoked by both the editor UI and the AI workflow. Define its inputs, prerequisites, result, and undo behavior. Reuse the Studio and World Editor operations that already implement the edit.
- [x] Walk through the same barrier from selection to Play. Cover no selection, an unsupported mesh, canceling a preview, undo and redo, and switching workspaces mid-edit. Record any steps that still require guessing which tool to use.

**Verified.** The header actions are `mesh.bevel`, `transform_pivot_point`, `sculpt.worldforge_brush`, `object.worldforge_add_sculpt_detail`, `mesh.worldforge_setup_uvs`, and `paint.add_texture_paint_slot`. Forge AI jobs call the same Blender operators. `reviewStudioPublish` in [StudioPublishReview.cpp](../State/StudioPublishReview.cpp) builds the publish review for both Apply Studio Update and Forge AI.

**Pass when** a creator can bevel, sculpt, paint, and publish the barrier from the visible workflow, and interrupted or canceled actions leave the asset in a predictable state.

## Phase 3 — Let AI run the same workflow

**Outcome.** A creator can ask Forge AI to make a worn barrier with faded yellow stripes and place it at a named road entrance.

- [x] Start with one prompt and a small set of supported commands. Include beveling, applying a bounded wear or paint operation, positioning an instance, and checking it in Play. Each command specifies its target IDs, expected revision, parameters, and result.
- [x] Show the plan and target before editing. If the barrier or road entrance is ambiguous, let the user choose. Run authoring operations on an editable working copy and stage placement changes for preview.
- [x] Preview the revised mesh, materials, and intended position. Let the user pause, take over manually, change parameters, or cancel. Apply the approved publish and placement as one undoable WorldForge change. Reuse revision checks so AI cannot overwrite edits made while its preview was open.
- [x] Check that the barrier appears at the entrance, has the expected material, and blocks the player in Play. Show what actually passed, what failed, and which change the user can revise.
- [x] Test the normal prompt, an ambiguous target, invalid command parameters, canceling, an intervening manual edit, and a failed Play check. Verify undo and redo across both the asset and the placement.

**Verified.** The commands are `bevel_edges`, `paint_stripes`, `place_instance`, and `check_play` ([ForgeAi.cpp](../State/ForgeAi.cpp)). [ForgeJob.py](../Workspaces/ForgeJob.py) runs in background Studio in about 4 seconds. It bevels sharp edges, paints worn stripes into a packed image, and publishes through **Publish to WorldForge**, so the result stays editable in Studio. Cancel, timeout, and a failing job leave the project unchanged. A manual edit to the barrier or a moved entrance blocks the stale result. Apply is one undo step, and the change survives reopening. The rendered stripes and the panel were captured in the [Forge AI audit](../build/audit/final-forge-ai). The planner is rule-based for now, and the preview shows text and the stripe texture rather than a viewport ghost.

**Pass when** the prompt produces a reviewable result that the user can edit by hand, approve and undo as one change, and verify in Play. A canceled or invalid run leaves the published project intact.

## Phase 4 — Expand to one playable street

**Outcome.** A small playable street with a road, props, one enterable building, basic traffic, and one mission becomes the test level for multi-system AI edits.

- [x] Build one short street with an enterable repair shop, a simple traffic route, and a mission that starts at a marker, leads the player to the shop, and completes through an interaction. Verify the baseline in both Play and the packaged Player.
- [x] Use the barrier workflow to populate the street. Extend the AI command set only as each new operation gains the same preview, validation, and undo behavior. First cover instance placement, a traffic route change, and a mission marker move.
- [x] Test one coordinated request such as moving the repair shop entrance to the side street, moving its mission marker, and routing traffic around the barrier. Review the proposed asset, level, and gameplay changes together before applying.
- [x] Verify traffic completes its route, the player can enter the shop, the mission can be completed, and the project reopens and packages successfully. Record frame time and memory on the GTX 1650 for a fixed route and compare against the baseline. Set the performance budget before widening the level.

**Verified.** Choose **Playable Street** under Starter Content when creating a project. Traffic, missions, and path planning live in [Gameplay.cpp](../Engine/World/Gameplay.cpp) and run in both editor Play (E interacts, and objectives appear in the status bar) and the standalone Player. The playable format is now version 5, which adds UVs, base-color maps, traffic routes, and missions. `WorldForgePlayer --verify-street <scene.wfplay> <report.json>` drives the mission with an autopilot and records frame time and memory.

| Run on the GTX 1650 | Mission | Frame avg | Frame p95 | Frame max | Peak working set |
| --- | --- | --- | --- | --- | --- |
| Baseline street | 5.5 s | 4.33 ms | 11.88 ms | 15.25 ms | 71 MiB |
| After the barrier and coordinated change, reopened | 7.6 s | 4.65 ms | 12.14 ms | 21.91 ms | 73 MiB |

**Performance budget** for widening the level: p95 frame time under 16.7 ms and peak working set under 1 GiB on the GTX 1650, measured with `--verify-street` and vsync off. The street audit enforces it.

Forge AI's street commands are `move_entrance`, `move_mission_marker`, `reroute_traffic`, and `check_street` ([PlayableStreet.cpp](../State/PlayableStreet.cpp)). After the striped barrier is placed in the eastbound lane, the street check reports traffic blocked. The coordinated request moves the entrance to the side street, moves the marker, and adds detour waypoints. Afterward the mission, shop access, and traffic all pass, in Play and in the packaged Player. Undo restores the original street, and redo replays the same result.

**Pass when** the street remains playable after the approved AI changes, the mission can be completed in the packaged Player, and the change can be undone and replayed with the expected result.

## How to track completion

Record the build tested, the fixture used, the automated results, and the manual checks run through the normal editor UI. Start the next phase after the current pass condition is met.
