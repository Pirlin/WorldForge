# WorldForge development audit

## Handoff

The Development Drive contains the v3 master blueprint in Markdown, PDF, and Word, a machine-readable specification for 40 screens, a capability matrix, an engineering backlog, and the approved shell image. The approved image is byte-identical to the World Editor image already used by the application. The Markdown blueprint is the editable authority. The backlog CSV and blueprint assign some different ticket numbers after the early milestones, so this report identifies those steps by name.

The 40 IDs specify workflows organized into 12 groups. The current build exposes 24 prototype destinations. A destination, reference image, or successful UI signal does not satisfy the workflow's acceptance criteria.

## Visual audit

All 17 supplied images were compared pixel by pixel with an application render at each image's native dimensions. The mean absolute RGB difference is the average per channel on a 0–255 scale. Lower values are closer. Exact equality would be 0.

| Workspace | Mean RGB difference |
|---|---:|
| World Editor | 16.47 |
| Sculpting | 26.56 |
| Modeling | 28.67 |
| Texture | 28.89 |
| Programming | 30.04 |
| Rigging | 31.68 |
| Visual Scripting | 33.47 |
| Build | 34.03 |
| Material | 36.58 |
| Audio | 37.01 |
| VFX | 37.72 |
| AI | 37.73 |
| Multiplayer | 38.23 |
| Animation | 40.06 |
| Character | 40.79 |
| Testing | 44.19 |
| Cinematic | 49.03 |

Home, Physics, UI Designer, Profiler, Asset Hub, Team, and Settings have no dedicated comparison image. The HUD screenshot is mapped to Testing because it shows UI Testing Mode. Visual parity remains open across the 17 pairs. Typical gaps include inspector spacing, toolbar icons and offsets, artwork framing, and timeline and graph presentation. The highest measured differences are in Cinematic, Testing, Character, and Animation.

Generated evidence is under `build/audit/qa` for this run. It contains the per-pixel results and, for each reference, the actual render and a difference image with RGB deltas amplified 4×. The full Windows editor audit ran at 100% effective Qt scaling.

## Control coverage

The audit traverses all 24 destinations and their tabs, expands inspector sections, exercises exposed widgets and menu actions, and verifies panel close/reopen. It separately checks project creation, rename, save, reopening, deletion confirmation, scene selection, rename, transform undo/redo, and importing a demo record in an isolated profile. The final run recorded 1,346 passing assertions and 132 preview-only control activations, with zero failed assertions. A pass can represent either a verified state change or a control-level signal, as identified in the report. Both the core and editor CTest targets passed.

Remaining acceptance work includes keyboard-only and accessibility coverage, real mouse and multi-monitor docking at mixed DPI, all selection-dependent/error states, and real end-to-end results for preview controls. Geometry import, rendering, mesh and texture editing, graph execution, audio output, testing, compilation and packaging still need backend implementations. Some studio settings and code buffers remain session-local. Sample performance and renderer labels are not measured runtime values.

## First Light

The first implementation slice adds a Qt-independent C++20 core, canonical GUIDs, a versioned and revision-checked scene document, hierarchy validation, and reversible typed commands for adding, removing, renaming, reparenting, and transforming entities. A snapshot round trip validates the in-memory scene. The current editor still uses its prototype JSON store. Build presets, a pinned empty vcpkg manifest, core CTest coverage, and a Windows/Linux CI workflow are present. The local Windows core and editor builds passed, including the Visual Studio generator used by the Windows CI job. This folder has no Git metadata, so the hosted workflow has yet to run.

| Step | Owner and verification |
|---|---|
| Wire the editor to the scene core | WF-02 and WF-24. Map existing stable prototype IDs to GUIDs without changing user data. Route Outliner and Inspector changes through the typed edit contract. Test selection synchronization, stale revisions, and undo/redo. |
| Implement versioned files and recovery | WF-01, WF-02, and WF-40. Define and migrate `.wfproject` and `.wfscene`; preserve IDs, hierarchy, transforms, and legacy data. Test two disk reopen cycles, invalid schema rejection, and an interrupted save. |
| Render First Light on Vulkan | WF-02 and WF-39. Bring up a validation-enabled surface and swapchain, draw a lit cube, then connect camera, resize, selection, and transforms to scene data. Confirm validation on the target GPU and actionable errors on failure. |
| Close the F0 gate | Prove a clean checkout can build, run, edit, save, and reopen the sample. Run hosted CI and complete the required DPI and accessibility checks. Move GLB import and the independent Player to F1. |

The Windows host reports Vulkan devices for NVIDIA GeForce GTX 1650 and Intel UHD Graphics. The Vulkan SDK headers and validation layer still need to be provisioned and verified before the renderer gate.
