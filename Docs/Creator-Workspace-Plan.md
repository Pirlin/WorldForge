# WorldForge creation workspaces

## Frontend status

The previous Windows audit compared 17 supplied reference images with editor renders at the references’ native sizes. Every pair differed. Modeling averaged 28.67 RGB levels of error per channel, Sculpting 26.56, Texture 28.89, Material 36.58, Rigging 31.68, and Animation 40.06, on a 0–255 scale. Home, Physics, UI Designer, Profiler, Asset Hub, Team, and Settings had no dedicated reference. The audit traversed 24 destinations and recorded 1,346 passing assertions and 132 preview-only activations. Some passes verified widget signals. Native geometry, paint, deformation, and animation workflows still need end-to-end tests once their backends exist. See [the development audit](Development-Audit.md) and [pixel results](../build/audit/qa/pixels.csv).

The 1917 × 1021 Modeling screenshot supplied for this review shows a viewport about 310 pixels tall inside a studio area about 795 pixels tall. A full-width lower dock displays the asset tree, thumbnails, and a UV preview while a separate UV Editor tab sits beside it. Three rows compete for viewport controls. The cockpit view shows Object highlighted alongside geometry editing tools and a preselected edge overlay. The original 1672 × 941 Modeling concept also dedicates permanent space to the browser and UV preview. The new design below proposes a different default layout for the studio interior, using the approved World Editor shell for shared navigation, typography, icon styling, and status.

## Blender findings

Blender 5.2.3 LTS was inspected locally with factory settings. Its Modeling and Sculpting workspaces each contain one viewport occupying roughly 82% of the workspace width and the full editor height, with Outliner above Properties in a narrow column. UV Editing and Texture Paint split the main area between a 2D editor and a viewport. Animation gives the Dope Sheet a resizable strip beneath its views. Shading gives substantial space to the material preview and the node editor. Blender’s mode system changes the available menus, tools, and shortcuts with the task and selection. [Workspaces](https://docs.blender.org/manual/en/5.1/interface/window_system/workspaces.html), [Object Modes](https://docs.blender.org/manual/en/latest/editors/3dview/modes.html)

Blender’s source separates its evaluated Mesh data from BMesh, the topology representation used for editing. BMesh stores UVs at face corners and uses explicit vertex, edge, and face connectivity for local operations. The comparable WorldForge design needs an editable mesh with stable asset identity, topology revision checks, corner attributes for UV seams, and a derived render mesh that updates after committed edits. [Mesh design](https://developer.blender.org/docs/features/objects/mesh/mesh/), [BMesh source](https://github.com/blender/blender/blob/main/source/blender/bmesh/bmesh.hh)

A temporary GLB exported from the local Blender installation contained evaluated mesh geometry, normals, UVs, a material, and a translation animation. The original eight-vertex mesh and its bevel modifier remained editable in Blender. WorldForge’s F1 importer is the next consumer for this type of fixture. Keep Blender source files as source assets where applicable and use GLB for interchange. Direct reuse of Blender source would require a decision about GPL obligations for distribution and review of the exact files. [Blender glTF support](https://docs.blender.org/manual/en/5.2/addons/scene_gltf2.html), [Blender license](https://www.blender.org/about/license/)

## Modeling default

| Area | Proposed behavior |
|---|---|
| Main view | Give the viewport the full height below the studio header. Aim for about three-quarters or more of the usable studio width at the review resolutions when the right column is open. Provide a focus action that temporarily expands the viewport. |
| Viewport header | One row for Object/Edit mode, vertex/edge/face selection in Edit mode, snapping, and shading/overlays. Use menus for pivot, orientation, normal display, and rarely adjusted view options. |
| Tools | Use a short tool strip for selection and transforms. Show the active tool’s settings in one place. Find Extrude, Inset, Bevel, Loop Cut, Knife, Bridge, and Merge in Edit mode through contextual menus and command search. Add Blender-style shortcuts as an optional preset with text-entry and focus handling. |
| Right column | Stack a compact model Outliner above a selection-driven Properties panel. Object selection shows transforms, modifier stack, and material slots. Edit mode prioritizes mesh selection and active tool values. Collapse topology statistics and normals into panels or overlays. |
| Assets, UVs, history | Open the asset browser as a shared drawer or in Asset Hub. Give UV Editing a linked 2D/3D split view with synchronized selections, seams, unwrap, and pack. Put operation history and diagnostics in an on-demand panel. Save panel visibility and sizing per workspace and offer a reset. |
| Empty and unsupported states | Start empty with a clear Create or Import action when no mesh is open. Explain unavailable operations and their prerequisites. Show tool outcomes, geometry counts, and renderer status from actual project or renderer data. |

The first Modeling workflow should be a simple original hard-surface prop. Create or import the mesh, select a face in Edit mode, extrude it, undo and redo, save and reopen, and see the same asset update on an existing World Editor instance. Add inset, bevel, and a non-destructive Mirror modifier once that workflow holds. Verify generated geometry and attributes, not just the selected toolbar button.

## Workspace defaults and acceptance

| WorldForge workflow | Default editing view | First functional acceptance |
|---|---|---|
| Modeling and UV Editing (WF-03) | Large viewport for mesh editing. Opt-in UV split for a linked 2D view. | An extrude updates rendered geometry and keeps asset references through undo, redo, and reopen. Mark a seam and verify UV island selection maps to the correct faces. |
| Sculpting (WF-04) | Full-height clay view with radius, strength, and symmetry at hand. A compact brush shelf and optional Layers, Masks, and Remesh panels. | A pressure-sensitive sculpt stroke changes geometry, can be undone as one stroke, and survives reopen. Confirm symmetry and mask behavior on a test mesh. |
| Texture (WF-05) | 3D paint canvas with a switchable 2D view. Active texture set, channel, brush, and Layers stay visible. Baking and export open on demand. | Paint a layer on the prop, undo it, reopen the layer stack, and see the linked Material and World instance use the updated texture. Verify sRGB color channels and linear data channels. |
| Material (WF-06, WF-28) | Start with a live PBR preview and material parameters. Open the node graph for custom shaders. | Update a material instance across two consumers and keep the last valid preview when a graph fails to compile. |
| Character and Rigging (WF-07, WF-08) | Character assembly with a live preview. Rig Edit, Pose, and Weight Paint each expose relevant controls. Put retargeting in a dedicated workflow. | Bind a small skeleton, edit skin weights, pose and inspect deformation, and preserve stable bone references when reopening. |
| Animation (WF-09) | Live pose preview above one resizable editor switching between Dope Sheet and Curves. Keep transport, keying, and clip selection visible. Open blending, retargeting, and state graphs as needed. | Author and retime a short clip. Verify preview interpolation, undo, reopen, and eventual playback at the same frames in the Player. |
| VFX and Cinematic (WF-10, WF-12) | VFX preview and graph when graph editing is active. Cinematic camera preview above a sequencer with optional curves. | Preview a parameter-driven effect or camera cut, reopen it, and verify that the World placement or referenced clip updates. |
| Asset Hub and Import/Export (WF-24, WF-36) | One searchable project asset browser and import queue. Workspace actions open compatible assets directly. | Reimport a Blender GLB with mesh, material, and hierarchy changes. Verify stable asset references and actionable validation for invalid files. |

Use the same project asset ID when opening an asset in another workspace, and pass an explicit sub-selection such as a UV island, texture set, bone, or clip where it applies. Each editor consumes revisioned asset data and issues typed transactions. A mesh edit updates render geometry and marks affected bakes, LODs, or skin bindings for review. Use linked previews to show the effect in the World and a validation action to locate the affected assets.

## Implementation order

1. Prototype the viewport-first Modeling layout within the existing Qt shell. Establish a new visual baseline for Modeling with Object and Edit mode, empty selection, and the UV split. Use the old screenshot to preserve shared styling, with the layout above as the proposed studio baseline.
2. Complete the First Light scene path with a live Vulkan viewport, selection, commands, versioned save and reopen, and undo. Bring in the F1 GLB importer and asset registry to give studios real assets and diagnostics.
3. Ship a working Modeling edit and linked UV workflow against a shared project asset. Reuse the viewport, selection, command search, panel persistence, and undo patterns in the other creation workspaces as their backend features come online. Deliver basic PBR preview with the import path, then add layers, brush tools, and rig and animation capabilities in the relevant milestones.
4. Verify both layout and behavior. Capture at the original Modeling reference size and at the supplied screenshot size. Check smaller windows, 100%–200% scaling, keyboard focus, dock restoration, and selection-dependent states. For every shipped action, exercise successful edits, invalid or empty inputs, cancellation where relevant, undo and redo, reopen, and cross-workspace updates.
