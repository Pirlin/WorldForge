# WorldForge Studio implementation plan

## Product architecture

WorldForge Studio will be a Windows asset-authoring application built from the complete Blender source tree. It will ship as part of WorldForge with its own executable, W icon, installer identity, dark navy and purple palette, typography, splash art, and workspace defaults. The WorldForge launcher will offer **New Asset** and **Edit in Studio**. Studio will use Blender's integrated interactive tools, editable data model, dependency graph, and undo system.

WorldForge Editor owns levels, placed instances, terrain, gameplay configuration, Play, and Simulate. Studio owns the editable source for Studio-managed assets. WorldForge Player loads published game data through the engine's packaging pipeline. On Windows the top authoring tabs open their corresponding workspaces in one hosted Studio process. **World Editor** returns to the level and keeps the Studio session open.

The end-to-end workflow is **select an asset → edit in Studio → save `.wfasset` → preview changes → publish → verify in World Editor and Player**. Saving an asset creates an authoring revision. Publishing updates the revision used by the game.

## Execution checklist

Mark a phase complete when its build or deliverable exists and its exit checks have evidence. Record the build ID, fixture, hardware where relevant, and test result beside the phase. Fix a failed exit check in the phase that owns it. Where phases can overlap, release them in dependency order.

| Phase | Depends on | Deliverable and evidence |
| --- | --- | --- |
| [x] 0. Upstream baseline | — | Windows x64 Release built from the pinned source. The mesh edit, bevel evaluation, save and reopen fixture passed, and the source-built UI rendered the fixture on the GTX 1650. |
| [x] 1. Branded Studio preview | 0 | ZIP and MSI built, extracted, and passed the save/reopen fixture. Studio ran side by side in windowed and maximized layouts on the GTX 1650. Mixed-monitor DPI and a clean-PC installation are release checks. |
| [x] 2. Native asset package | 1 | The packaged Release build opens `.wfasset` by filename and passed the portable asset, concurrent write, interrupted write, missing dependency, and forced-exit recovery fixtures. Windows file association is a release check. |
| [x] 3. First complete publishing workflow | 2 | Published a mesh from the embedded packaged Studio UI and saved it in World Editor. The publishing audit and standalone Player passed with a two-material fixture. |
| [ ] 4A. Modeling and UV Editing | 3 | Verify the branded authoring workflow and publish a model with validated topology, UVs, and material slots. |
| [ ] 4B. Sculpting | 4A | Reopen an editable sculpt and publish a game-resolution mesh. Verify reduction, normals, UVs, and preview on the fixture. |
| [ ] 4C. Texture Painting and Materials | 4A | Reopen painted textures and the editable graph. Verify baked PBR maps in Editor and Player. |
| [ ] 4D. Rigging and Animation | 4A | Reopen an editable rig and clip. Verify skeleton, weights, and animation from Studio through the engine and Player. |
| [ ] 4E. Geometry Nodes and scene tools | 4A | Reopen a procedural graph. Publish supported evaluated geometry and verify diagnostics for unsupported output. |
| [ ] 5. Forge AI | 3 and the relevant 4-series workflow | Preview and apply a supported Studio command. Verify undo, stale-edit rejection, cancellation, and worker failure recovery. |
| [ ] 6. Windows release | 0–5 | Release installer and corresponding source package. Verify a clean Windows install, upgrades, recovery, and the packaged Editor-to-Player workflow. |

Phase 3 is the first integration milestone. Package it for an internal walkthrough before widening scope. The 4-series phases share the same save and publish pipeline. Each can be verified and handed off independently once its dependencies pass. Phase 5 can validate commands against workflows as they become available. Phase 6 covers the full scope listed above.

## Phase 0 — Pin and build upstream

**Verified November 14, 2026.** The Windows x64 Release build used MSVC 14.44.35207, Windows SDK 10.0.26100.0, CMake 3.31.6 and Ninja 1.12.1. The source-built executable preserved the mesh edit and editable bevel through save and reopen. The fixture evaluated to 98 polygons and reopened in the Vulkan viewport on the GTX 1650. The build directory contains `phase0-build-evidence.json`.

- Verify the upstream tag and commit against the source and installed version cited in [Modeling-Source-Review.md](Modeling-Source-Review.md). Record the verified source commit, dependency versions, Windows SDK, compiler, build options, and source notices. Start with the reviewed release if it resolves and builds. Record any version change and repeat the baseline checks.
- Give the fork its own repository and Windows build pipeline. Track the upstream release separately from the WorldForge patches. Build the pristine baseline first using the documented Windows process, then pin the dependency inputs used by CI. Keep a local build path for Visual Studio Community 2022 if the selected upstream revision supports it. [Blender Windows build documentation](https://developer.blender.org/docs/handbook/building_blender/windows/)
- Trace the source entry points before editing them. Review application metadata, Windows resources and file associations, startup templates and theme definitions, Open/Save and recovery operators, and glTF export. Record the files touched by each patch. Keep Studio integration code in its own module where upstream extension points allow it.
- Preserve upstream copyright and license notices. Package license notices and make the corresponding fork source, including distributed modifications and required build scripts, available to recipients under the applicable GPL terms. Review the shipped Studio adapter and dependencies as part of release packaging. Use the WorldForge identity for the product and retain accurate upstream attribution in About and Third-Party Notices. [Blender Foundation rebranding FAQ](https://www.blender.org/support/faq/) · [Blender source license](https://github.com/blender/blender#license)

**Exit check.** Reproduce the baseline Windows x64 build from the recorded inputs. Launch it on the development PC, exercise a viewport edit, and save and reopen the upstream fixture.

The verified upstream pin is Blender `v5.2.3`, commit `e13b728c6880d3084d9d57c30031ed5df8b39dee`. Its Windows x64 dependency repository is pinned by the upstream tree to `60d6e96b917568278d400a4024c98da0fb777338`. Keep both checkouts under the same local build directory as siblings named `blender` and `windows-x64-deps`. Materialize the pinned Git LFS libraries and the upstream application resources before configuring. The baseline fixture uses its own generated mesh, so the large upstream test-data collection can be fetched separately when running Blender's full upstream suite.

For a fresh baseline on Windows, run these commands from the WorldForge repository. If an LFS transfer is interrupted, rerun the corresponding `git lfs pull` to reuse the local cache.

```powershell
$studio = "$env:LOCALAPPDATA/WorldForge/StudioSource"
New-Item -ItemType Directory -Path $studio -Force | Out-Null
$source = Join-Path $studio 'blender'
$deps = Join-Path $studio 'windows-x64-deps'
$env:GIT_LFS_SKIP_SMUDGE = '1'
try {
    git clone --depth 1 --branch v5.2.3 --single-branch https://github.com/blender/blender.git $source
    git -C $source remote add upstream https://projects.blender.org/blender/blender.git
    git init $deps
    git -C $deps remote add origin https://projects.blender.org/blender/lib-windows_x64.git
    git -C $deps fetch --depth 1 origin 60d6e96b917568278d400a4024c98da0fb777338
    git -C $deps checkout --detach FETCH_HEAD
} finally {
    Remove-Item Env:GIT_LFS_SKIP_SMUDGE
}
git -C $source lfs pull upstream --include 'assets/**,release/**,scripts/**,doc/**'
git -C $deps lfs pull origin --exclude '**/debug/**,**/*_d.dll,**/*_d.lib,**/*.pdb'
.\Tests\Build-StudioBaseline.ps1 -StudioRoot $studio
```

Run [Build-StudioBaseline.ps1](../Tests/Build-StudioBaseline.ps1) for the Visual Studio 2022 x64 Release build, local install, and [edit, save, and reopen fixture](../Tests/Verify-StudioBaseline.py). The script records the build and test logs, installed executable hash, tool versions, and fixture result in the local build directory. Verify the interactive viewport separately with the installed executable before marking Phase 0 complete.

The first source trace located these entry points in the pinned checkout.

| Area | Upstream files |
| --- | --- |
| Executable and installer | `source/creator/CMakeLists.txt`, `build_files/cmake/packaging.cmake`, `release/windows/icons/winblender.rc`, `release/windows/installer_wix/WIX.template` |
| Title, splash, and settings path | `source/blender/windowmanager/intern/wm_window.cc`, `source/blender/windowmanager/intern/wm_splash_screen.cc`, `source/blender/blenkernel/intern/appdir.cc` |
| Palette and workspaces | `release/datafiles/userdef/userdef_default_theme.c`, `scripts/presets/interface_theme/`, `scripts/startup/bl_app_templates_system/`, `scripts/startup/bl_ui/space_topbar.py` |
| Authoring and exchange | `source/blender/windowmanager/intern/wm_files.cc` for open, save, and recovery, and `scripts/addons_core/io_scene_gltf2/__init__.py` for glTF export |

## Phase 1 — Brand the Studio application

**Verified November 14, 2026.** The Release build of `codex/worldforge-studio` at `74dd9433a362` passed its launcher and save/reopen fixture. The fixture checked the purple selection color, authoring workspaces, and the separate WorldForge preferences directory. The branded splash, W icon and Modeling and UV Editing layouts were observed in the UI while the existing Blender and WorldForge Editor processes were open. A 2 m Z move in Edit Mode persisted across workspaces. Studio also reopened the fixture in a smaller window using Vulkan with the GTX 1650 explicitly selected and fallback disabled. Mixed-monitor DPI is unverified.

The ZIP and MSI both passed the Studio fixture from their extracted contents. Windows Installer verified the MSI through an administrative extraction to a test folder. The [source patch](WorldForge-Studio-Phase1.patch) applies to the Phase 0 pin. [Build-WorldForgeStudio.ps1](../Tests/Build-WorldForgeStudio.ps1) builds the executables, runs the launcher and asset fixture, and packages the ZIP preview and MSI. The script uses a short staging path for WiX because long paths to bundled Python test data caused MSI packaging to fail. Register `.wfasset` with the Phase 2 file-opening path so a double-click opens an actual asset package.

- Apply the W icon, WorldForge Studio name, application ID, product version, taskbar identity, splash artwork, About page, and Windows installer metadata. Register `.wfasset` Open and double-click handling. Use a Studio settings directory and keep settings isolated across installs and upgrades.
- Use the existing [WorldForge palette](../DesignSystem/WorldForgeTheme.qss) to define equivalent Studio theme colors for surfaces, text, active tools, focus, and selections. Set typography and spacing within Studio's own UI system. Check normal, maximized, and mixed-DPI window states.
- Use top workspace tabs for direct navigation and keep the viewport prominent. Modeling starts with the mode controls, left tool shelf, and Outliner above Properties. Sculpting adds the brush shelf. UV Editing uses a split UV and 3D view. Texture Painting exposes image and brush controls. Materials opens the shader graph with a preview. Rigging and Animation get context-specific panels and a timeline where needed. Preserve tool search, shortcuts, and advanced controls.

**Exit check.** Install and launch the branded executable side by side with the existing editor and an upstream install. Verify taskbar identity, menus, settings isolation, workspace switching, shortcuts, DPI scaling, and viewport operation on the GTX 1650.

## Phase 2 — Implement the WorldForge asset format

The editable fork lives in [StudioSource/blender](../StudioSource/blender) inside the WorldForge project. It retains its own Git history. The WorldForge repository carries the [Phase 1](WorldForge-Studio-Phase1.patch) and [Phase 2](WorldForge-Studio-Phase2.patch) changes for replay against the pinned upstream revision. Build the asset-enabled fork with `Tests/Build-WorldForgeStudio.ps1 -AssetDocuments`.

The Studio Integration preview opened a moved package from a native filename argument on the Windows development PC. It retained the editable bevel and packed texture. The asset audit tested stale and concurrent writes, interrupted replacement, corruption, newer format rejection, missing texture diagnostics, Save As, and Save a Copy. A separate Studio process saved a changed bevel to recovery and exited abruptly. A fresh process recovered the edit and the original asset's SHA-256 stayed identical. See the [asset audit](../build/audit/studio-asset-expanded.log) and [recovery results](../build/audit/studio-recovery-final.log).

Use the `.wfasset` name reserved in the [WorldForge blueprint](../Development%20Drive/WorldForge_Master_Blueprint_v3.md). Define a versioned package with a UTF-8 manifest and a supported set of payloads. Use an archive container with bounded reads and relative paths so projects can move between machines.

| Entry | Contents |
| --- | --- |
| `manifest.json` | Format version, stable asset ID, authoring revision, asset type, writer version, source and resource paths, and hashes for stored payloads |
| Editable source | Scene data written by the fork's upstream serializer, preserving editable objects, modifiers, nodes, UV sets, and animation data |
| Resources | Eligible packed textures and explicit package entries for supported dependencies |
| Preview | Optional thumbnail for the asset browser |

Implement **New**, **Open**, **Save**, **Save As**, **Save a Copy**, recent assets, autosave, and recovery through the Studio document lifecycle. Save As can move or rename the same asset. Save a Copy creates a new asset ID. Treat legacy `.blend` as an explicit import and save a Studio copy. Preserve the original file. Report missing or unsupported external libraries, caches, fonts, and media before declaring a package portable.

Write through a temporary file and replace the last valid save only after validation. Bound archive entry count, total expanded size, and path depth. Reject invalid paths, duplicate entries, checksum mismatches, and unsupported schema versions without replacing the original. Keep automatic script execution disabled when opening external asset data. Preserve recovery files separately from the last successful save.

**Exit check.** Move a textured fixture with a modifier stack to another folder and reopen it. Verify stable ID and full editability. Test concurrent saves, a broken archive, missing resources, an interrupted write, and opening a newer format in an older build. Verify that recovery can restore work after a forced close.

## Phase 3 — Publish static meshes into WorldForge

The packaged Editor hosted Studio while a mesh was edited, published, and saved to a test level. **Edit Selected Mesh in Studio** opens its published editable source when present. **Publish to WorldForge** stages the evaluated mesh and an editable `.wfasset` together. **Apply Studio Update** previews the mesh and checks the target project and revision before applying one undoable edit. The project stores the source by content hash so undo, redo, and reopening can refer to the correct source revision.

The packaged Studio passed the UV unwrap, two-material bevel, and editable source round trip. A reopened published source uses a working copy. Saving to a chosen location preserved the asset ID and left the staged source intact. The Editor publishing audit verified that the bevel geometry, UV coordinates, and material assignments survived import. It also verified three retained placements, undo and redo, stale revision rejection, source storage, and project reopen using actual Studio output. The standalone Player loaded that published scene on the development PC. See the [packaged Studio and Player results](../build/audit/studio-package-final.log) and the [publishing audit](../build/audit/studio-package-final-publishing/controls.csv).

1. Add **New Asset** and **Edit in Studio** to the asset browser and a **Publish to WorldForge** command in Studio. Pass an explicit project ID, asset ID, and expected published revision. Keep exchange files in a project-scoped staging directory.
2. Studio saves the authoring revision and prepares the evaluated static mesh and validation report. Use GLB for the first exchange profile. Verify scale, up axis, orientation, normals, UVs, material slots, and supported embedded PBR maps. Report omitted source features and any data outside the profile.
3. Extend [ProjectStore](../State/ProjectStore.h) to target publish by stable asset ID. The current GLB reimport identifies a source by path. Reuse its validation and undo infrastructure while making Studio publishing independent of filenames. Compare revisions before applying. Show a geometry and material summary and explain which local edits, LODs, or collision data need review.
4. Commit publishing as one undoable editor transaction. Preserve placed instances and their transforms. Keep the previous valid revision if export, validation, or commit fails. Detect stale publishes from simultaneous Studio sessions. Repeated publish of the same content should avoid duplicate updates.

**Exit check.** Publish a two-material mesh placed three times in a level. Verify that Studio can reopen the source and edit the original modifier stack, that all three instances update in place, and that undo, redo, save, reopen, and Player packaging work. Repeat with an invalid export, a stale revision, and an asset with native WorldForge edits.

## Phase 4 — Expand the authoring and publishing workflows

The shipped editor routes Modeling, Sculpting, Texture, Material, Rigging, and Animation to the embedded Studio. Modeling's Window menu opens UV Editing. The packaged navigation fixture switched workspaces without losing an unsaved bevel. The nine bundled brush libraries and six node libraries use their original `.blend` filenames. Studio resolves `.wflib` Essentials references in existing `.wfasset` documents to the installed `.blend` libraries. In Sculpt Mode, Add Sculpt Detail gives eligible low-poly meshes four levels of multiresolution subdivision and preserves the base mesh.

| Workspace | Interactive acceptance | Game handoff |
| --- | --- | --- |
| Modeling and UV Editing | Create, bevel, unwrap, edit a modifier, and reopen the same asset | Evaluated mesh, verified UVs, material slots, and topology-change diagnostics |
| Sculpting | Work with brushes, masks, symmetry, and remesh on a saved source | Publish a game-resolution mesh and report unsupported details or bake requirements |
| Texture Painting and Materials | Paint, undo, bake maps, and reopen editable shader data | Validate Base Color and Emissive color space, data maps, channel packing, and the resulting Editor and Player frames |
| Rigging and Animation | Create a skeleton, weight it, author a clip, and reopen the rig and curves | Add a separately verified engine import and runtime path for skeletons, skin weights, and clips before enabling animated publishing |
| Geometry Nodes and scene tools | Edit a procedural asset and reopen the graph | Publish supported evaluated output, with diagnostics for unsupported instancing, data types, or dependencies |

Keep upstream interactive capabilities available through the branded workspaces. Release each WorldForge publishing capability after its runtime and round-trip checks pass.

## Phase 5 — Add Forge AI

- Start with a small allowlist of typed Studio commands, such as bevel by numeric settings, UV pack, or batch export. Each request identifies the asset, expected revision, parameters, and preview output. Run headless-compatible operations on a working copy. Report operations that require an interactive viewport. [Blender Python API background execution](https://docs.blender.org/api/main/info_tips_and_tricks.html)
- Show the result and its validation report before applying. Keep automatic execution of scripts from imported assets off. Record the command and revision for undo and debugging. Support cancel, timeout, and recovery from a worker crash. Verify that a stale AI result cannot overwrite a newer manual edit.

**Exit check.** Preview a supported edit, apply it, undo it, then run the same operation against a stale revision and cancel an in-flight job. Confirm that cancellation, timeout, and worker failure preserve the last valid source and published assets.

## Phase 6 — Release and maintain the Windows build

- Package signed Windows builds when release signing is available. Test installation, upgrade, file associations, uninstall, crash recovery, offline startup, and side-by-side operation. Test the GTX 1650 development machine and a clean Windows PC. Verify the full asset journey through the packaged Editor and Player and record exact versions, hardware, and evidence.
- Track upstream fixes. For each upgrade, rebuild from the new pinned source, rerun saved-file and publishing fixtures, and test the WorldForge branding patches. Retain a prior installer and recovery instructions for users with newer asset revisions.

**Exit check.** On a clean Windows PC, install the release, open and save a `.wfasset`, publish an update, and verify it in the standalone Player. Check that the installer, third-party notices, and corresponding source match the tested release. Verify that uninstall preserves the user's project and asset files.

## Decisions and implementation order

The proposed defaults are a separate branded Studio executable, Windows x64 first, `.wfasset` as the authoring package, and static GLB as the first exchange profile. This uses upstream serialization internally. An independently designed scene serializer would be a different project with its own compatibility and migration cost.

The first implementation task is to verify the upstream revision recorded in the source review and build it on Windows. Keep the baseline log and saved fixture so later phases can trace regressions to the fork, file format, or publishing changes.
