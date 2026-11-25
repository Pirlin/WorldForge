# WorldForge development playbook

This is how WorldForge is developed, written so that any AI agent (Claude, ChatGPT/Codex, or another) works the same way and hands over cleanly. Follow it exactly. It complements:

- [WorldForge-Handoff.md](WorldForge-Handoff.md): build commands, the frame architecture, the current status and timings.
- [WorldForge-Strategy-and-Roadmap.md](WorldForge-Strategy-and-Roadmap.md): the goal, the R0–R5 rendering stages and their status tables.

Read this playbook first, then the handoff, then the roadmap section for the stage you are working on.

---

## 1. Ground rules

1. **One line of history.** Work on `master` and push to `origin/master` (github.com/Pirlin/WorldForge). Never create feature branches.
2. **Nothing unverified is committed.** A change is committed only after the checks in [section 5](#5-verification-matrix) pass, or after their failures are explained in the commit message and docs.
3. **Never overwrite another agent's work.** Before editing, check for uncommitted changes and recent edits by someone else ([section 2](#2-start-of-every-session)). If another session is still editing, stop and ask the user.
4. **Report honestly.** Give measured numbers. Say what failed, what was not checked, and why. Never describe a feature as done when its checks did not run.
5. **The editor and the Player must match.** Both render through the shared Vulkan code (`Engine/Render/Vulkan`, `Engine/Render/Shaders`). Every visual change goes into both, and the rendering audit compares them.
6. **Docs move with the code.** Each finished step updates the roadmap's status table, the handoff's status and "Next" list, and the README's user-facing settings.
7. **Keep going.** The user wants roadmap tasks chained without waiting for confirmation. After a task is verified, committed and pushed, start the next one in [section 10](#10-the-plan-what-to-do-next).

---

## 2. Start of every session

Run these before touching anything:

```bash
git status --short
git log --oneline -10
```

- **Uncommitted changes you did not make:** another agent worked here. Find out whether it is still active:
  - List the changed files' modification times. Edits in the last few minutes mean it may still be running.
  - Check for running `WorldForgeEditorAudit.exe`, `WorldForgePlayer.exe`, `ninja`, `cl.exe`, or agent processes (`tasklist`).
  - If it is active, do not edit or build. Tell the user and ask how to split the work.
  - If it is idle, treat its work as unreviewed: build, run every check in [section 5](#5-verification-matrix), review the diff ([section 6](#6-self-review-checklist)), fix what is wrong, and commit it with a message that says it was another agent's work and what you corrected.
- **A `WorldForgeEditor.exe` from `build\editor` may be running.** That is the user's own build; leave it alone. Audits use `build\audit`.
- Read the handoff's "Status" and "Next, in order" sections, then pick the first unfinished item.

---

## 3. The working loop

Every task goes through these steps in order.

### 3.1 Understand before editing

- Read the whole path a change touches, end to end:
  - the setting in the editor UI (`App/MainWindow.cpp`);
  - its state (`State/WorldScene.*`);
  - the export (`State/WorldRuntime.cpp`) and the playable format (`Engine/World/PlayableScene.*`);
  - the Player's view (`Player/main.cpp`, `Player/PlayerRender.h`) and renderer (`Player/VulkanRenderer.cpp`);
  - the editor renderer (`Components/LiveViewport.cpp`, `Components/WorldLightingVulkan.*`);
  - the shared frame (`Engine/Render/Vulkan/HdrFrame.*`) and shaders.
- Read existing tests for the area in `Tests/EditorAudit.cpp` (search for the feature's name).
- Find where a similar feature already exists and copy its pattern. For example, contact shadows copied the screen-space reflections plumbing step for step.

### 3.2 Design, and write the design down

- For anything larger than a few lines, write the design into the handoff's "Next, in order" entry **before** coding: data flow, files, the test that will prove it, and acceptance numbers.
- Split big work into steps that each build, pass every check, and can be committed on their own. Step A often adds infrastructure that changes nothing visible (for example, a velocity target the shaders first fill with zeros). Then prove the images are unchanged before adding behavior.

### 3.3 Implement

- Match the code style around you: compact C++20 with no spaces after commas, `const` locals, lambdas for local helpers, comments in full sentences that explain why.
- Shaders:
  - Shared code goes in `Engine/Render/Shaders/*.glsl`.
  - Register new include files in `worldforge_shader_includes` in `CMakeLists.txt`, or edits will not trigger a rebuild.
  - Compile each changed shader on its own before building ([section 4](#4-commands)).
- Never leave debug code. Tag any temporary lines with `// DEBUG` and grep for that tag before committing.

### 3.4 Verify

Run the checks for the change type ([section 5](#5-verification-matrix)) with `Tests/Run-Verification.ps1`. Then look at the images the audit wrote. Passing thresholds is not enough when something looks wrong.

### 3.5 Self-review

Go through [section 6](#6-self-review-checklist) for the diff. For large diffs, especially another agent's, also review line by line with the questions there.

### 3.6 Document

- **Roadmap:** in the stage's status table, describe how the feature works, its files, its tests and its measured numbers, and say "Not started" or "In progress" honestly.
- **Handoff:** update "Status" (realism score, frame times with the power plan) and "Next, in order" (remove what is done, add the next design), and add any new pitfall.
- **README:** document the user-facing setting: where it is, its default, and what it does.

### 3.7 Commit and push

- Stage only the files you mean to commit (`git add <paths>`), then check them with `git diff --cached --stat`.
- Check for leftovers: `git diff --cached | grep -c DEBUG` must print 0.
- Write a one-line subject in the imperative ("Add contact shadows"), then a body that explains what changed and how it was verified.
- Run `git push origin HEAD:refs/heads/master`. If a large push fails with "remote end hung up", push one commit at a time: `git push origin <sha>:refs/heads/master`.

### 3.8 Report and continue

Tell the user, in plain language:
- what is now true, with the numbers;
- what failed or was not checked;
- what comes next.

Then start the next task.

---

## 4. Commands

All commands run on Windows in PowerShell unless marked otherwise.

| Purpose | Command |
|---|---|
| Developer shell (needed for `cmake` and `ctest`) | `& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation` |
| Build | `cmake --build build/audit --parallel 6` |
| Build and run checks | `& .\Tests\Run-Verification.ps1 -Modes rendering,street -Tag mychange` |
| Everything before a commit | `& .\Tests\Run-Verification.ps1 -Modes rendering,street,realism,r3-performance,district,full -Tag mychange -CTest` |
| Timing variation | `& .\Tests\Run-Verification.ps1 -Modes street -Tag timing -Repeat 3` |
| Compile one shader (Bash) | `"$LOCALAPPDATA/WorldForge/Vulkan/glslang-16.6.0/bin/glslang" -V -I. -IEngine/Render/Shaders -IComponents/Shaders -o /tmp/x.spv <file>` |
| Unit and integration tests | `cd build\audit; ctest -E EditorAudit --output-on-failure` |
| Re-record golden images after an intended visual change | set `WORLDFORGE_UPDATE_GOLDEN=1`, run the `rendering` mode, then inspect and commit `Tests/Golden/*.png` |

Notes:
- Audit output goes to `build/audit/<mode>-<tag>/controls.csv`, with images beside it. The full audit takes 10–15 minutes, the others 3–10 minutes each.
- In the developer shell, the line "'vswhere.exe' is not recognized" is harmless.
- `ctest`'s `EditorAudit` test is the full audit. Run it as the `full` mode instead, so it does not time out against other runs.

---

## 5. Verification matrix

### 5.1 Which checks to run

| Change touches | Required checks |
|---|---|
| Any shader, `HdrFrame`, either renderer | `rendering`, `street`, `realism` |
| Materials, lighting, sky, fog | as above, plus a new rendering audit case for the feature |
| R3 lighting (probes, screen-space GI, reflection probes) | as above, plus `r3-performance` |
| Draw submission, culling, streaming, scene scale (R4) | as above, plus `district` |
| Playable format, export, project files | `ctest` (SceneDocumentTests, SurfaceLibrary, Player), `full` |
| Editor UI or workspaces | `full` |
| **Before every commit** | everything relevant above, plus `ctest` and `full` |

### 5.2 Acceptance thresholds

| Check | Threshold |
|---|---|
| Golden images (`Tests/Golden`) | Mean difference at most 1.5 levels, and at most 0.5% of pixels differing by more than 24 levels |
| Editor and Player alike | Mean at most 1 level for new feature cases (most cases measure 0.03–0.5) |
| Realism score (`--realism`, Cycles + FLIP) | Never below the recorded baseline: **94.9**, mean FLIP 0.0505 over 4 views. Fog, bloom and camera effects are off in the reference. |
| Street budget (`--street`, `--r3-performance`) | p95 frame under 16.7 ms and peak working set under 1 GiB on the GTX 1650 |
| District (`--district`) | Completes the route with Vulkan; memory under 1 GiB; p95 is recorded as the R4 baseline (INFO until R4 brings it under 16.7 ms) |
| Full audit | 0 FAIL. A few SKIP and PREVIEW rows are normal. |

### 5.3 A new feature needs its own audit case

Model it on an existing case in `renderingWorkflow` in `Tests/EditorAudit.cpp`:
- Build a small scene with `store.addCube()` and `store.updateObject(...)` at an unused spot (for example x = -150).
- Toggle the setting with `setDisplay({...})` and capture with `settled(shot)`.
- Compare the editor with the Player: `WorldRuntime::exportScene` into `playablePath`, then `playerCapture(shot, size)` and `difference(...)`.
- Measure the effect in a region where it must show (a non-trivial number of levels), and in a region where it must not show (about 0 levels).
- Undo every edit at the end with the `historyStart` loop.

### 5.4 Timing protocol (frame times are noisy)

The dev machine is a GTX 1650 **laptop**. Its timings depend on:
- **the Windows power plan:** the same build measured about 7 ms on a faster plan and 13–16 ms on "Power saver";
- **GPU temperature**, after long audit runs;
- **background CPU load:** Visual Studio indexing, the ChatGPT app.

So:
1. Compare only runs measured in the same session, back to back.
2. To test whether a change costs time, run an **alternating A/B**: old, new, old, new, each a full `street` run on the same build tree. The comparison of the TAA shader (2026-11-24) is an example: old 14.55/14.54 ms, new 15.10/14.48 ms, so no difference.
3. Record the power plan (`powercfg /getactivescheme`) next to every number in the docs.
4. A timing FAIL by itself does not reject a change. Rerun it, and A/B it before deciding.

---

## 6. Self-review checklist

Check every diff against these. Each item was a real bug at least once in this project.

**Data layout and formats**
- **The Lighting uniform block is std140, and three places must agree:** `Lighting.glsl`, `LightingUniform` (`Components/WorldLightingVulkan.h`) and `LightingBlock` (`Player/VulkanRenderer.cpp`).
  - Their sizes are pinned by `static_assert` (2016 bytes today).
  - `SceneMotion.glsl` reads `previousViewProjection` at byte offset **1904**.
  - Add new members **at the end** only.
- **Playable format (`Engine/World/PlayableScene.cpp`):**
  - The current version is 28.
  - A new field gets a new version, which is written only when the field differs from its default.
  - Raise the reader's `version > N` bound.
  - Write and read the field in the same order, under the same `version >= N` condition.
  - The writer's validation ranges must equal the reader's `readValue` ranges, or a saved game cannot be loaded.
  - Add a round-trip test to `Tests/SceneDocumentTests.cpp`.
- **Project files (`State/ProjectStore.cpp`):** every JSON key written must be read, with the same default.

**Vulkan**
- **Descriptor updates** must happen before the set is first bound in that command buffer (`vkUpdateDescriptorSets` after a bind invalidates the command buffer), and only on per-frame sets whose fence has signaled. Bind the previous-frame image (binding 8) every frame, because it alternates.
- **Cached handles:** never cache a `VkImageView` handle to skip descriptor writes. A recreated view can reuse a destroyed handle's value.
- **Synchronization:**
  - A compute write that fragment shaders read needs a `COMPUTE_SHADER` → `FRAGMENT_SHADER` barrier.
  - Render-pass dependencies cover attachment hazards.
  - Check both the read-after-write and the write-after-read direction.
- **Lifetimes:** resources used by in-flight frames live until those frames' fences signal. The pattern: the per-frame slot holds a `shared_ptr` to the resource.
- **Do not create pipelines per frame or per refresh.** Create them once and reuse them (see `ProbePipeline`).
- **Every pipeline in the scene pass** needs one blend state per scene color target (4 today: scene, indirect, albedo, velocity). The sky masks targets 1–3.

**Scene targets and their alpha channels**
- `indirect.a` is the depth along the camera's forward axis (0 for sky). TAA copies it into `history.a`, which screen-space reflections and contact shadows trace next frame.
- Glass passes must preserve it: blend alpha `ZERO`/`ONE`, and write 1 to the velocity target in the transmission pass.

**Parity and features**
- **Editor and Player:** every new uniform field, binding or vertex stream exists in both renderers. Captures (`VulkanRenderer::capture`) must match the live path, including warm-up frames for effects that read the previous frame.
- **Screen-space traces** (reflections, contact shadows) need jitter- and precision-aware slack. Stored depth is 16-bit and comes from jittered samples. Without slack, surfaces "hit" themselves at grazing angles.

**Hygiene**
- **Warnings:** build with no new compiler warnings.
- **Leftovers:** no `// DEBUG` lines, no temporary environment switches, no commented-out code.

---

## 7. Debugging techniques that worked

- **Look at the images.** Every audit writes PNGs next to `controls.csv` (for example `rendering-reflections.png` and its `-diff.png`). Diff images are scaled 8×.
- **Visualize with a temporary switch.** Add a `// DEBUG` branch, controlled by an environment variable that both renderers pass into an unused uniform slot. Paint pixels by category (for example red for hits nearer than the ray's start, green for farther), run the rendering audit, then read the editor and Player captures. This found the screen-space reflection self-hits in four runs.
- **Read pixel values, not impressions.** `Tests/Measure-ImageRegion.ps1` compares a rectangle of two images (mean and signed difference in levels), or with `-Column` prints brightness down a column across an edge. This is how the motion-blur silhouette problem was found: the object's inside blurred, its outline did not.
- **Bisect performance by switching one cost off,** in the same session (section 5.4). The fog shadow march was found this way: 15 ms with it, 10.1 ms without, 10.0 ms before fog.
- **Measure GPU time per pass.** `--verify-district` reports it (`gpuMs`): sun shadows, sky maps, point-light shadows, the scene pass, and post-processing.

---

## 8. Tool pitfalls on this machine

- **Visual Studio holds files open.** `sed -i`, `perl -i` and `cp` replace a file by renaming, which then fails ("Device or resource busy", "Permission denied"). Edit with an editing tool that writes in place, or edit a copy and write it back with `cat copy > file`. After a failed scripted edit, `grep` to see what partly applied before retrying, or the edit gets duplicated.
- **Scripted edits fail silently.** Multi-line `perl -0pi` patterns often do not match, and a `|` delimiter clashes with `||` in C++. Prefer exact string edits. Always confirm with `git diff` or `grep`.
- **PowerShell variable names ignore case:** `$a` overwrites a `[string]$A` parameter.
- **`powershell.exe -File` from Bash cannot run `.ps1` files** (execution policy). Run scripts from a PowerShell session.
- **Run one audit at a time, with a visible window.** Hidden windows (`-WindowStyle Hidden`) or two audits at once fail "The street renders" with "No Vulkan frame". After an interrupted run, check for leftover `ctest` or audit processes and stop them.
- **Rebuild the editor after editing `Workspaces/*.py`:** they are Qt resources.
- **Python is not on PATH.** Use PowerShell or Bash for scripts.
- **The Authoring check "Sun casts visible geometry shadows" is flaky.** Rerun it before investigating.

---

## 9. Architecture you must know before changing rendering

The handoff has the full frame description. These are the invariants:

**Frame order:**
1. Shadow cascades, sky visibility maps, point-light shadow cubes, then probe tracing when R3 probes are on.
2. Scene pass: opaque geometry, then the sky (drawn at the far plane), then glass from back to front.
3. Occlusion pass (GTAO plus screen-space GI, at half resolution when screen-space GI is on).
4. Bloom (from the previous frame).
5. TAA.
6. Camera effects (depth of field, motion blur).
7. Display.

**The scene pass writes 4 color targets and depth:**

| Target | Contents |
|---|---|
| scene | Lit HDR color |
| indirect | Sky light that ambient occlusion scales; alpha holds the depth along the camera's forward axis |
| albedo | Diffuse albedo; alpha holds fog transmission |
| velocity | The surface's own motion in previous-frame UV units (RG16F) |

**Lighting set (set 0) bindings:**

| Binding | Contents |
|---|---|
| 0 | Sun shadow atlas |
| 1 | Lighting uniforms |
| 2 | Editor material images (the Player uses set 1 for its 12 material textures) |
| 3 | The Player's view uniforms |
| 4 | Sky visibility maps |
| 5 | Sky environment |
| 6 | Light clusters |
| 7 | Point-light shadow atlas |
| 8 | Previous frame's TAA result |
| 9 | Light probes |

**Previous positions for motion vectors:**
- **Player:** a ring of 3 world-space vertex buffers, `vertexRing_`. The previous one is bound as vertex binding 1.
- **Editor:** `uploadMotion` writes previous triangle positions on the first frame of each geometry revision.

**The Player rebuilds every vertex in world space on the CPU each frame** (`Scene::vertices()` in `Player/main.cpp`) and uploads all of them. R4 replaces this with persistent static geometry and GPU-driven draws.

---

## 10. The plan: what to do next

Work through these in order. Each item names the files, the method, the test and the acceptance criteria. Update this section and the handoff as items finish.

### Latest verification

The GTX 1650 completed the 6 × 6 and 12 × 12 routes in Vulkan. Direct and indirect captures were pixel-identical in each renderer for both district sizes. Still frames now average history at the same pixel and authored camera previews share the Player's lens and clip planes. The editor and Player matched within 0.470 display levels on the 6 × 6 route and 0.314 levels on the 12 × 12 route.

| District | Mean rejected opaque triangles per frame | Scene GPU time, street / rooftops | Point-shadow GPU time, street / rooftops | p95 frame time, street / rooftops | Worst frame | Peak working set |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 6 × 6 | 2,355 | 9.09 / 22.93 ms | 0.04 / 0.05 ms | 22.87 / 47.11 ms | 119.35 ms | 217 MiB |
| 12 × 12 | 10,055 | 8.88 / 20.78 ms | 0.96 / 0.90 ms | 28.53 / 35.39 ms | 155.43 ms | 228 MiB |

The renderer retains the atlas slots of lamps that stay in range, replaces only the faces for changed lamps, and periodically refreshes shadow casters. The latest 12 × 12 route rejected an average of 754 opaque draws per frame and used compacted indirect draws for the sun shadows throughout all 1,500 measured frames. Both district sizes are below the memory limit. Street p95 still exceeds 16.7 ms. The table records one pass through each route under Power saver.

`--district-compare` runs the same export with direct and indirect submission in one audit. `--district-textured` uses the 12 × 12 layout with five generated 512 × 512 facade maps shared by its buildings. It checks texture export, route completion, memory, and render parity. `--sunrise` meters the physical sky at 73 evenly spaced points from −6° to 12° and compares both renderers' exposure readings and twilight colors.

### 10.0 Overnight plan (8 hours, started 2026-11-24 23:40)

The user is asleep. Work through these blocks in order without asking. Each block ends with its checks passing and a commit pushed to master, so an interruption loses at most one block. Tick the boxes and add measured numbers as you go. If a block cannot be finished, write down what is left, then move to the next block instead of stopping.

**Block 1 (about 1.5 h): verify and commit everything that is uncommitted.** **Done 2026-11-25 (commit 982400e):** ctest 9/9, rendering 50/50, realism 95.0, street 12/12, all district modes pass, full audit 1375 PASS.
- [ ] Build.
- [ ] Run `Tests/Run-Verification.ps1 -Modes rendering,realism,street,district,district-compare,district-occlusion,district-textured,district-streaming,district-meshlets,district-meshlets-indexed,sunrise,sunset,full -Tag night1 -CTest`.
- [ ] Review the new R4 code against section 6: `DrawCulling`, `OcclusionDepth`, `CullDraws.comp`, `ReduceDepth.comp`, `Meshlets.h`, `PlayMeshlets.mesh`, texture streaming, the point-shadow slot reuse, and the light-cluster changes. Fix what is wrong.
- [ ] Commit in logical commits where the files allow it (R2/R3; review fixes; district and GPU timing; culling and occlusion; meshlets; streaming; docs). Otherwise commit once, with a message that lists every part. Push.

**Block 2 (about 1 h): fix the sunrise exposure step.** **Done 2026-11-25:** `--sunrise` and `--sunset` pass 3/3 each.
- The `--sunrise` audit measured a 0.508-stop step against the 0.5-stop limit after the sun crosses the horizon.
- Find which frames step (print the per-frame log2 average) and whether the cause is the sun disc's visible fraction, skylight from the Earth's shadow edge, or the auto-exposure adaptation itself.
- Fix the cause, not the limit. Check `--sunset` too.
- Commit.

**Block 3 (about 1.5 h): remove the district's worst-frame hitches.** **Done 2026-11-25:** the worst frames on every district route are now 38–48 ms (they were 119–155 ms), under the 50 ms target.
- The latest routes recorded worst frames of 119 ms (6 × 6) and 155 ms (12 × 12).
- Log the frame index and per-pass GPU times of every frame over 50 ms in the `--verify-district` report. Find what runs on those frames: probe refresh, point-shadow refresh, sky map refresh, pipeline creation or texture upload.
- Spread or remove the work, for example a time budget per frame for refreshes, or creating pipelines up front.
- **Accept when** the worst frame is under 50 ms on both routes in the same session.

**Block 4 (about 2 h): bring the street-level half of the district under 16.7 ms p95.** **Done 2026-11-25 (commits ccb3ae3, ae69eab):** the district street p95 is 11.7–13.3 ms (it was 19–20.7 ms). Lamp ranges now stop where the current exposure can no longer show their light (an 1800 cd lamp reached 85 m by day; now about 3 m), and opaque ranges draw nearest first. Scene GPU time fell from 10.06 to 6.0 ms on the street and 25.77 to 14.5 ms on the rooftops. Tried and reverted: transforming the fog march ray once per cascade instead of per sample. It made fog 50% slower, most likely from register pressure in the scene shader. The rooftop p95 is still 21 ms; height fog is its largest cost (5.8 ms), and the separate street route stays near 17 ms p95 under Power saver, as it has all evening.
- Use `--district-profile` to measure the cost of each scene-shader feature.
- Work on the largest per-pixel cost, which the last profile showed is lamp shading. Options:
  - a tighter light range from an illuminance cutoff;
  - skipping shadow lookups for lamps that contribute almost nothing;
  - cheaper cluster lists from tile-sphere tests.
- Re-measure in the same session, and keep the rendering audit's images unchanged (or re-record goldens only for intended changes, with a reason).

**Block 5 (about 1 h): finish the R4 checklist items that have code but no recorded result.** **Done 2026-11-25:** all seven district audits pass. The roadmap now records the results, with R4.2 (culling and indirect draws), R4.3 (occlusion) and R4.5 (mip streaming) ticked. R4.4 stays open for cluster LOD; its meshlets and indexed fallback are done. One `--district-compare` run recorded an 88.6 ms worst frame, above block 3's 50 ms target; the other six routes stayed at 23–37 ms.
- For occlusion culling, meshlets (with the indexed fallback) and texture streaming, run their audits.
- Record the numbers in the roadmap's R4 checklist, ticking an item only when its acceptance criteria in section 10.3 are met.
- Write down what each unticked item still needs.

**Block 6 (about 1 h): documentation and a final combined run.** **Done 2026-11-25:** ctest 9/9, rendering 50/50, realism 95.0 (FLIP 0.0498), street 12/12 (12.5–13.0 ms p95), r3-performance 4/4, district 14.6 ms p95, textured district 16.4 ms p95, sunrise and sunset 3/3, full audit 1375 PASS. The roadmap, the handoff and this playbook are updated.

**What else the night did (after block 5):**
- `--street-profile` runs the street mission once per disabled feature, and the street report records GPU time per pass. The street's scene pass was 11.7 ms: sun shadows 6.9 ms, height fog 3.6 ms, ambient occlusion 3.4 ms (plus 2.8 ms in post-process).
- Under TAA, both renderers pass a TAA flag (`probeSettings.z`), and the costly lookups are split across pixels and frames for the resolve to average:
  - sky openness tests 8 of 16 directions per pixel;
  - PCSS takes 8 of its 16 blocker and 16 filter taps;
  - GTAO traces one slice per frame;
  - the height fog's eight segments share four shadow lookups, two per frame;
  - the local mist's segments alternate frames.
  Without TAA every lookup is taken as before. The street scene pass is now 7.8 ms.
- **Lessons** (keep them):
  - Two rewrites that added work or live values to the scene shader made it slower: transforming the fog ray once per cascade (+50% fog cost), and a second importance-sampling loop for the mist (+1.7 ms). The scene shader is large and register-bound, so extra registers cost more than the arithmetic they save.
  - A per-pixel checkerboard on a branch saves nothing: every warp still takes both sides. Split a lookup by changing a loop's start index (all pixels run the same count), or alternate whole frames.
  - Keying the split on a nonzero TAA sample index made every sixteenth frame take the full path, which set the p95. Use an explicit flag.

### 10.0.1 The next 8 hours

In order. The same rules as section 10.0: each block ends with its checks passing and a pushed commit.

1. **Temporal stability check (about 1.5 h).** **Done 2026-11-25:** the rendering audit pans through the misty sunset street with the lookups split and with every lookup taken (`WORLDFORGE_FULL_LOOKUPS=1`): once with 100 ms between steps, once moving the camera before every rendered frame. The split adds 0.02 and 0.01 levels of frame-to-frame change (0.34 versus 0.32; 0.14 versus 0.13), and no more pixels change by over 24 levels. Rendering 52/52. The criteria below were the plan; the audit uses the split-versus-full comparison instead, which separates flicker from motion.
   - Add a rendering-audit case that captures 16 consecutive frames of a still view with shadowed height fog and local mist, and one of a slow pan.
   - Measure the per-pixel standard deviation over the frames, in display levels.
   - Accept when the still view's mean deviation is under 1 level and the pan shows no pixel band brighter than 4 levels between frames. If it fails, lower the split (for example, keep two fog lookups per frame) and remeasure.
2. **Rooftop fog cost (about 2 h).** Height fog is still the largest share of the rooftop scene pass (2.1 of 9.5 ms).
   **Done 2026-11-25 (fog volume):** instead of a half-resolution pass after a depth prepass, a compute pass (`FogVolume.comp`, `Engine/Render/Vulkan/FogVolume.*`) fills a 160 × 90 × 64 froxel volume after the shadow maps. One invocation walks each froxel column from the eye, takes one sun-visibility lookup per slice (and the lamp light inside the local mist) and stores, for the height fog, its shadowed share per unit of opacity from the eye to each slice (one filtered read at the pixel's depth, times the pixel's own exact opacity), and, near the local mist, each froxel's sun visibility and lamp light (read at each step of the mist's per-pixel march). Set 0 binding 12 (`fogVolumeAt`). Height fog now costs 0.14 ms of the street's scene pass (it was 3.6 ms before the night, about 1.6 ms with 8 volume reads per pixel). Street with fog and mist 13.9–14.5 ms p95 (was 16.4); district 13.6 ms p95, rooftop scene pass 7.7 ms (was 9.5–10.2). Rendering 53/53 with a new golden for the misty sunset (`fog-sunset.png`), which caught a variable-shadowing bug that editor/Player parity could not. Lesson: precompute along view columns rather than adding cheaper lookups per pixel; eight volume reads per pixel still cost more than two shadow lookups on the rooftops. Second lesson: a running sum read between slice centers undercounts wherever the medium ends partway through a slice (the mist showed concentric rings, commit 26848ef, fixed the same day); store a share per unit of opacity, or point values, and let the pixel apply its own exact opacity. The misty lamp now has a golden image (`lamp-mist.png`). **Open:** with the mist reading point values (five reads per pixel), the street with fog and mist is 15.95 ms p95 and its scene pass 10.5 ms, against 9.0 ms for the ring-prone column version. Reading at fixed segment midpoints instead of jittered points saved only 0.2 ms, so cache misses are not the cause; time the compute pass on its own next (its own timestamp in `RenderStats`).
   - Plan and build a half-resolution volumetric pass: march height fog and mist into an RGBA target (in-scatter, transmission) after a depth prepass, then upsample with depth rejection in the scene shader.
   - This removes the fog and mist from the scene shader's register budget entirely. Compare against the current path in the same session; keep the editor and Player matching.
3. **Night lighting (realism pass item 2, about 1.5 h).** Warm and cool lamps in mist are checked. Still missing:
   - shadow falloff from a lamp;
   - exposure adapting from the street into a lit interior.
   Add both audit cases, fix what they find, then tick the roadmap item.
   **Done 2026-11-25:** the rendering audit lights a slab with a 2000 cd lamp beside a block (falloff 0.43 / 0.29 / 0.15 at 1.5 / 3.5 / 6.5 m; the block's shadow is black) and walks from the sunlit street into a lamp-lit room (auto exposure opens about 3.2 stops, largest step 0.11 stops per 50 ms, settles within 0.03). Audit traps found on the way: the history undo between cases also reverts the atmosphere (set `skyModel` again), the street has no ground at (-50, -50) (add a slab), and a camera looking straight down has no orientation.
   **Lead result:** Cycles with `WORLDFORGE_CYCLES_DIFFUSE_BOUNCES=0` is darker on the night walls by the same 3–7 levels the engine is, so the gap is bounce light. The probe trace now lights its hits with the sky and the nearest 32 lamps (`HitLighting`), but the probe grid spans about 10 m around the camera's focus and probe GI is off in the realism views (`WORLDFORGE_REALISM_PROBE_GI=1` / `WORLDFORGE_REALISM_SCREEN_GI=1` turn them on for experiments: probe GI left the night FLIP at 0.0750, screen-space GI made it 0.0765). Next: a wider or cascaded probe field, then enable probe GI in the realism views.
   - **Lead from 2026-11-25:** the night view is the weakest Cycles comparison (FLIP 0.075; the others are 0.025–0.058). Vertical walls render 7% darker than Cycles, and 22% on the face toward the twilight glow; open ground was up to 30% brighter. Part of the ground excess was the 9-coefficient sky irradiance, which overstated light on upward surfaces by 11% at twilight. Band 4 is now added (commit deeb36a; a core test keeps it within 6% of the integral). The walls are not explained by the spherical harmonics: on a horizontal normal they are within 2%. The likeliest cause is lamp light bounced off the street. Cycles bounces the 2000 cd lamps off the lit ground onto the walls; the probe GI injects only sunlit and emissive surfaces (R3), and the lower half of a wall's sky lobe sees the atmosphere's dark ground. Test it first: render the night view in Cycles with one bounce off. If the walls then match, inject clustered lamp light into the probe surfaces (`ProbeTrace.comp`), in both renderers. Sky openness for low directions is the second suspect: the 16 sky-visibility directions start at 4.6° elevation, and the twilight glow sits below that.
4. **Aerial perspective (realism pass item 3, about 1 h).** Add the skyline contrast-over-distance audit on the district at clear air, haze and dense fog.
   **Done 2026-11-25:** `--aerial-perspective` renders the 6 × 6 district down the ring road and over the skyline in clear air, haze 6/km and a nearly uniform fog (0.006, falloff 0.01). It measures RMS contrast around the vanishing point or skyline against the nearest facades or roofs, and checks that haze and fog take more from far than near, that near buildings stay readable, and that editor and Player match (0.27–0.28 levels). A steep ground fog seen from 40 m barely touches the skyline; that is physical, so the dense case uses uniform fog.
5. **Cloud transitions (realism pass item 4, about 1 h).** The 2D cloud layer exists and the overcast view matches across renderers. Add the transition check at street and rooftop height. This needs the Player to render at given play times.
   **Done 2026-11-25:** `WORLDFORGE_CAPTURE_SECONDS` sets the Player's play time for `--capture`. The rendering audit renders a clear-to-overcast transition at five moments at street and rooftop height: one-way, smooth, and halfway equal to the editor holding the halfway coverage (0.25–0.30 levels).
6. **R4.6 world partitions** (start only with the above done). The district is 52 k vertices and fits easily, so partitions need two things first: cell streaming from disk (playable format 30 with per-cell chunks) and content dense enough to show a gain. Write the design into section 10.3 before coding.
   **Progress 2026-11-25:** designed (section 10.3) and built for the Player: format 30 cells with proxies, `readPlayableIndex` / `readPlayableCell`, residency with worker-thread loads and batched layout rebuilds (`WORLDFORGE_CELL_STREAMING=1`), and `--district-cells` (three laps; 4/4 partition checks). Lessons: rebuilding the mesh layout for every finished cell cost 5 ms of CPU at p95 (batch the changes, 30 frames); hashing each mesh's facade PNG to find its texture on every rebuild was the largest part (materials now move with their meshes); a box proxy costs more than a box building (small meshes are their own proxy). Capping cell joins at four per rebuild made it worse (cells waited up to 12 s, memory grew) and did not remove the CPU spikes (31–35 ms worst), so their cause is still open. The frame-time check passed in one run (p95 18.9 against 17.6 ms) and missed in two (by 0.3 ms with a 78 ms frame; 55 ms worst), so treat it as unsettled. The Player's GPU timing now has a `fogVolume` pass: 0.46 ms on the misty street, so the mist's extra 1.5 ms is in the scene shader's reads. See the roadmap's R4 entry for what is still open.

### 10.0.2 The next 8 hours (planned 2026-11-25)

Same rules: each block ends with its checks passing and a pushed commit.

1. **Mist cost (about 1 h).** The street with fog and mist is 15.95 ms p95; its scene pass is 10.5 ms against 9.0 ms for the ring-prone column version. The fog-volume dispatch now has its own GPU timestamp (0.46 ms), so the 1.5 ms is in the mist's five reads per pixel; cut it (for example: one read per two segments, or lamps only in froxels the mist reaches). Keep `lamp-mist.png` and `fog-sunset.png` matching. Target: 14.5 ms p95.
2. **Night bounce light (about 2 h).** The night walls miss bounce light (Cycles with no diffuse bounces shows the same gap). Grow the probe field: a second, coarser grid (8 m spacing, about 50 m across) blended beyond the 2 m grid, both traced with `HitLighting`. Turn probe GI on in the realism views (the `WORLDFORGE_REALISM_PROBE_GI` switch does it for experiments) only if all four views improve or hold. Target: night FLIP under 0.065 and walls within 3 levels of Cycles.
3. **R4.6 follow-ups (about 2 h).**
   - Sun-shadow cost while streaming (1.6 against 0.7 ms): give each proxy its own draw range, or keep proxies out of the finer cascades.
   - Denser content: a district option with per-building detail (window insets, ledges, rooftop units) and a unique facade texture per building, so partitions have memory to save. Then record full residency against streaming on it.
   - Accept per section 10.3 item 5, then tick R4.6 for the Player; the editor's streaming and impostors stay open.
4. **R4.7 combined check (about 1 h).** Run district, street and realism together on the GTX 1650 and record sustained frame times, worst frames and memory in the roadmap.
5. **R4.4 cluster LOD design (about 1 h).** Write the design for a meshlet LOD hierarchy (cluster groups simplified per level, selected by projected error) into section 10.3 before coding.
6. **Realism review (about 1 h).** The realism pass's last item: record stills and moving shots on the GTX 1650 (settings, reference comparisons, editor/Player differences, frame-time percentiles, worst frames, peak memory) in the roadmap.

### 10.1 Immediately

- [ ] **Commit the uncommitted work if it is still uncommitted** (git status). It consists of:
  - the R2 materials, R3 lighting and atmosphere work done by ChatGPT, verified on 2026-11-24 (ctest 9/9, rendering 47/47, realism 94.9, full audit 1375 PASS);
  - Claude's review fixes from 2026-11-24:
    - the probe pipeline is created once and reused;
    - a failed probe refresh retries;
    - the Player refreshes probes only when geometry moves;
    - half-resolution screen-space GI keeps occlusion at depth edges;
  - the R4 district benchmark and GPU pass timing;
  - this playbook, `Tests/Run-Verification.ps1` and `Tests/Measure-ImageRegion.ps1`.

  A final combined run was started on 2026-11-24 with tag `final` (`build/audit/*-final/controls.csv`). Read those results, or rerun `Tests/Run-Verification.ps1 -Modes rendering,street,realism,r3-performance,district,full -Tag final -CTest`. Fix any failure, then commit and push. Street timing failures under "Power saver" are environmental (section 5.4).
- [ ] **Bring the district's scene pass down.** The district baseline (`--district`, 6 × 6 blocks, 375 meshes, 50 lamps, 4,500 triangles; 2026-11-24 on "Power saver") measured:

  | Measure | Result |
  | --- | --- |
  | Frame time | 24.7 ms average, 43.6 ms p95 (street-level half 23.3 ms p95, rooftop half 44.5 ms p95) |
  | CPU per frame | 0.5 ms |
  | Peak working set | 166 MiB |

  GPU time per pass:

  | Pass | Street level | Over the rooftops |
  | --- | --- | --- |
  | Sun shadows (including probe tracing) | 0.84 ms | 0.80 ms |
  | Sky visibility maps | 0.06 ms | 0.05 ms |
  | Point-light shadows | 0.13 ms | 0.22 ms |
  | Scene pass | **12.35 ms** | **32.46 ms** |
  | Post-processing | 3.04 ms | 3.59 ms |

  The scene pass is per-pixel shading cost, not geometry (4,500 triangles), so GPU-driven drawing (R4.2) would not help this yet. Find which part of the scene shader costs most:
  1. Add a breakdown to the `--district` audit: export the district several times, each with one feature off via the atmosphere details (`fogDensity` 0, `contactShadows` false, `reflections` false, `shadows` false), plus one export without the street lamps. Run the route for each and report the scene pass time per variant.
  2. Likely causes, from the shader:
     - **the clustered light loop:** 50 lamps of 1800 cd; every pixel seen from above falls in clusters that list many lamps, and the 8 shadowed lamps add 4 cube-map taps each;
     - **the fog's 8-step shadow march** on every pixel;
     - **contact shadows' 12-step ray** on every sunlit pixel;
     - **screen-space reflections** on the dark-glass facades (roughness 0.25).
  3. Fix the largest cost. For example, cap each light's range by where its illuminance falls below a threshold, so clusters list fewer lamps, or skip the fog shadow march where fog opacity along the ray is small. Then re-measure in the same session (section 5.4). The goal is the district under 16.7 ms p95 on the street-level half first.

### 10.2 Realism pass before city scaling (roadmap "Realism pass before city scaling")

1. **Moving sunrise and sunset.** Add an audit case that animates `sunElevation` from -6° to 12° over a fixed number of frames in the editor and the Player. Check that the sun disc crosses the horizon without a jump in exposure: the auto-exposure log2 average must change smoothly, with no step over 0.5 stops between consecutive frames. Check that the twilight sky keeps its hue order (zenith bluer than the horizon) and that the editor and Player match.
2. **Evening and night lighting.**
   - Lamps currently have no color (`PlayLight` holds intensity only). Add a light color end to end: details `lightColor` → `WorldRuntime` → playable format version 29 → the clustered light data (vec4 per light already has room) → both renderers.
   - Add audit cases: a warm lamp and a cool lamp; shadow falloff from a lamp; a lamp inside local mist; and exposure adapting from the street into a lit interior.
3. **Distance haze (aerial perspective).** Weather already has `haze` (aerosol extinction per km, `skySettings.w`). Verify that clear air, haze and dense fog keep depth cues across street, rooftop and skyline views. Add an audit case that measures contrast falling off with distance on the district skyline.
4. **Clouds and weather transitions.** `Weather` has cloud cover and transitions affecting sun transmission and sky irradiance. Add visible clouds to the sky. Start with a 2D cloud layer in `Sky.frag` sampled along view rays, its coverage tied to `cloudCover`. Check transitions at street and rooftop height in both renderers.
5. **Local mist cost.** With a 9 m mist volume at density 0.035, the street measured 19.5–20.5 ms p95, over budget. Profile it with the GPU pass timings, then reduce the march steps (as the fog shadow march went from 16 to 8 with TAA jitter) or march at half resolution. It must fit the street budget with both fog types on.
6. **Review on the GTX 1650.** Record the settings, the reference comparisons, editor/Player differences, frame-time percentiles, worst frames and peak memory in the roadmap before R4.

### 10.3 R4: scale for a whole city (roadmap "R4 implementation checklist")

**R4.1 District test scene and route.**
- Done in this session:
  - `State/CityDistrict.*` generates blocks, buildings, sidewalks and lamps, deterministically from a seed;
  - "City District" is in the new-project dialog;
  - the Player's `--verify-district <scene> <report> <inset> <rooftop>` flies a ring-road lap at eye height, then a rooftop orbit;
  - the report records frame, CPU and per-pass GPU times, draw calls, triangles and memory;
  - the editor audit's `--district` mode runs it.
- Remaining:
  - Add streaming stalls to the report once streaming exists (R4.6).
  - Use the existing 12 × 12 route alongside the 6 × 6 baseline when verifying the remaining scaling work.

**R4.2 GPU frustum culling and indirect drawing.**
- The Player caches fixed-layout vertex ranges in its persistent ring and updates moving geometry. Both renderers use a shared compute pass to test conservative opaque AABBs against the camera and sun cascades. The viewport and sun cascades use indirect counts on supported drivers, preserving draw order within each group. `WORLDFORGE_FIXED_INDIRECT=1` tests the fixed-size indirect path. `--direct` tests the ordinary draw path.
- Remaining acceptance work includes compaction by the Player's scene material buckets and the final district and street performance comparison.
- Replace the Player's per-frame CPU vertex rebuild:
  - Upload static meshes once into a persistent vertex buffer, re-uploading only when the scene changes. Keep moving meshes (bodies, clips) in a per-frame dynamic region.
  - Keep a per-mesh table of bounding sphere, first vertex, vertex count and material bucket.
- Add a compute pass that tests each mesh against the camera frustum (and each shadow cascade) and appends `VkDrawIndirectCommand`s per material bucket, with a count buffer.
- Draw with `vkCmdDrawIndirectCount` (core in Vulkan 1.2; add it to `VulkanApi.h`).
- Motion vectors: static geometry's previous position equals its current one, so bind the same buffer to binding 1; dynamic meshes keep the ring. Check that the motion and falling-box audit cases still pass.
- Then do the same in the editor's `LiveViewport` (`uploadMotion` and `upload`).
- **Accept when:**
  - the golden images are unchanged;
  - the editor and Player match;
  - the district reports far fewer scene draws or triangles from the street view than before;
  - the district p95 improves on the baseline recorded in section 10.1, measured in the same session (section 5.4);
  - the street does not regress.

**R4.3 Occlusion culling.**
- Build a hierarchical-Z pyramid from the previous frame's depth.
- Test culled-in meshes' bounds against it in the culling pass, using two-phase occlusion culling (draw last frame's visible set, build Hi-Z, test the rest).
- Test camera turns, fast movement and district boundaries. The district route already covers turns.
- **Accept when** no visible popping shows in the route frames (compare frames with culling off) and district street-level draws drop further.

**R4.4 Meshlets and cluster LOD.**
- Split meshes into meshlets of at most 64 vertices and 124 triangles, with a cone and bounds per meshlet.
- Use `VK_EXT_mesh_shader` where available (the GTX 1650 has it), with an indexed-draw fallback.
- Build cluster LOD by simplifying meshlet groups; `manifold` and the existing LOD code in `State/ProjectStore.cpp` `evaluateLod` are starting points.
- **Accept when** both paths match the goldens and the fallback is tested on a device without mesh shaders, or with the extension forced off.

**R4.5 Texture streaming.**
- Load mip tails first and stream higher mips by screen-space size.
- Evaluate virtual texturing against residency and upload budgets on the district with generated facade textures (`--district-textured`).
- **Accept when** memory stays under budget on the long route and the quality is restored within a set number of frames of a texture entering view.

**R4.6 Impostors and world partitions.**
- Divide the district into cells; stream cell geometry in and out around the camera.
- Show distant cells as impostors: an octahedral impostor per building, or a merged low-poly proxy per cell.
- Test long traversals and repeated boundary crossings for pops and memory growth: the working set must not climb over repeated laps.

Design (2026-11-25), in the order to build it:
1. **Playable format 30: cells.** The writer assigns every static mesh (no body, no clip) to a square cell of `cellSize` meters (64 by default) by the center of its bounds, and writes the meshes grouped by cell. A cell table follows the header: grid coordinates, bounds, first mesh and mesh count, and the byte offset and size of the cell's mesh block in the file. Dynamic meshes (bodies, clips), lights, the player and everything else stay global. `readPlayable` still reads everything, so every existing caller keeps working; `readPlayableIndex` reads the header, globals and cell table, and `readPlayableCell` reads one cell's meshes from its byte range. Core tests: round trip, cell assignment, one-cell reads equal the full read, and format 29 files still load.
2. **Proxies.** Each cell carries a merged low-poly proxy in the table, always resident: each mesh's LOD vertices where it has them, otherwise its bounding box (12 triangles) in its mean color. The Player draws the proxy for every cell not resident in full.
3. **Residency in the Player.** A cell streamer keeps cells within `fullRadius` (160 m) of the camera resident in full and unloads them past `fullRadius` + 32 m, so crossing a boundary back and forth does not thrash. Loads run on a worker thread (file read and texture decode); the main thread adds at most one finished cell per frame. When the resident set changes, the static vertex layout is rebuilt; if that hitches, give each cell its own slab in a GPU pool so a swap uploads only that cell.
4. **Measurement.** `--verify-district` gains resident cells, cell loads, the longest load-to-draw time and the working set per lap, and flies three laps. Content: the 14 × 14 textured district (the format's limit), where full residency is the baseline.
5. **Acceptance.** The working set stays within 5% from lap 2 to lap 3; no frame over 50 ms at a cell switch; p95 no worse than full residency; editor/Player parity and the rendering goldens unchanged (proxies only appear beyond `fullRadius`).

**R4.7 Combined check.**
- Run the district, the street and the realism audits together on the GTX 1650.
- Record sustained frame times, the worst frames and memory in the roadmap.

### 10.4 After R4

- **R5 (RTX-class GPUs):** hardware ray tracing, ReSTIR, a path-traced reference mode, upscalers, HDR output.
- **Outside rendering** (from the strategy doc):
  - verify release builds on a clean Windows PC;
  - put a real language model behind Forge AI;
  - Geometry Nodes publishing;
  - the Living City built on R4's district technology;
  - multiplayer replication.

---

## 11. Writing to the user

- Lead with what is now true, then the evidence (numbers), then what is not done.
- Use tables for before/after numbers.
- Name failures plainly, with the measured value and the threshold.
- Say what you did not verify.
- Write for a reader who was away: no unexplained shorthand, and say what "it" refers to.
