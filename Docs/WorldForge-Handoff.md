# WorldForge handoff

Read this first if you are continuing the work. The strategy is in `WorldForge-Strategy-and-Roadmap.md`, and each finished rendering feature has a row in its R1 status table.

## Conventions
- Commit and push straight to `master` (github.com/Pirlin/WorldForge). Don't create feature branches.
- If a large HTTPS push drops with "remote end hung up", push one commit at a time: `git push origin <sha>:refs/heads/master`.
- Verify every rendering change with the audits below. After an intended visual change, re-record the goldens with `WORLDFORGE_UPDATE_GOLDEN=1`.
- Update the R1 table in the roadmap and the user-facing settings in `README.md` as features land.
- Check every scripted edit with `git diff`. Multi-line perl substitutions often fail without any error, and a `|` delimiter clashes with `||` in C++ patterns (it once pasted text at the top of a file). Prefer exact edits. Before committing, read `git diff --cached`.
- PowerShell variable names ignore case: a `$a` assigned inside a script overwrites a `[string]$A` parameter. `powershell.exe` launched from Bash cannot run `.ps1` files here (execution policy); run them through the PowerShell tool.
- To debug a shader, temporarily color pixels by category (for example screen-space reflection hits by how far they are from the start) behind an environment variable, read both the editor and the Player captures from the rendering audit, then remove it.

## Build (Windows, GTX 1650 dev machine)
1. In PowerShell, run VS 2022's `Launch-VsDevShell.ps1 -Arch amd64 -HostArch amd64 -SkipAutomaticLocation`. The "vswhere.exe not recognized" message it prints is harmless.
2. Build with `cmake --build build/audit --parallel 6`. `build/audit` is a Ninja Release build with the editor audit enabled.
3. `ctest` only works inside that dev shell.
- Python files in `Workspaces/` are Qt resources, so rebuild the editor after editing them.
- A running audit locks the editor binary, so a rebuild fails until the audit ends.
- To compile one shader on its own:
  ```
  %LOCALAPPDATA%/WorldForge/Vulkan/glslang-16.6.0/bin/glslang -V -I. -IEngine/Render/Shaders -IComponents/Shaders <file>
  ```

## Audits
Put Qt on PATH first (`%LOCALAPPDATA%/WorldForge/Qt/6.8.3/msvc2022_64/bin`), then run `build/audit/WorldForgeEditorAudit.exe <outDir> <repoDir> [mode]`.

| Mode | What it checks |
|---|---|
| (none) | Full audit, about 1365 PASS rows, roughly 6–10 minutes. |
| `--rendering` | Street views in the editor and the Player (`WorldForgePlayer --capture`) against `Tests/Golden`. |
| `--realism` | The Player against Cycles (rendered through Studio), scored with LDR-FLIP against `Tests/Golden/realism.json`. Results are in `controls.csv` as "Realism score". |
| `--street` | Needs `WORLDFORGE_STUDIO_TEST_FORGE=1`. Reports frame times. |

- If the full audit stops writing `controls.csv`, a modal dialog is open outside `withDialog`.
- The Authoring check "Sun casts visible geometry shadows" is flaky. Rerun it before treating a failure as real.
- Run one audit at a time, with a visible window. Two audits at once, or a leftover audit process, slow the GPU and time each other out. An audit started with a hidden window (for example `Start-Process -WindowStyle Hidden`) fails the rendering check "The street renders" with "No Vulkan frame", because its viewport is never exposed.
- Timings depend on the Windows power plan. Before blaming a change, measure the previous commit in the same session.

## Frame architecture (`Engine/Render/Vulkan/HdrFrame.*`, shared by the editor and the Player)
1. **Scene pass.** Writes three color targets (scene; indirect sky light with the depth along the camera's forward axis in alpha; albedo plus fog transmission) and depth. Shading is in `Shaders/Shading.glsl` and `Lighting.glsl`. Screen-space reflections and contact shadows trace the previous frame's TAA result here (binding 8, `HdrFrame::previousFrame`), which the renderers rebind every frame. The editor restarts the TAA average one frame after a camera move or edit, so averaged frames never trace another view.
2. **Occlusion pass.** GTAO plus screen-space bounce light (`Ambient.frag`).
3. **Bloom.** Built from the previous frame's history (`BloomDown`/`BloomUp.frag`).
4. **TAA.** Runs in its own pass (`Temporal.frag`) and writes `history_[parity]`: light in RGB, this frame's depth in alpha.
5. **Optional post passes.** Depth of field and motion blur (`DepthOfField.frag`, `MotionBlur.frag`) ping-pong between `post_[0/1]`.
6. **Display pass.** Exposure and AgX (`Display.frag`). Editor overlays draw in the same subpass.

The CPU side of the lighting lives in these headers under `Engine/Render/`:

| Header | Feature |
|---|---|
| `SkyLight.h` | Sky environment map with GGX levels and SH |
| `Atmosphere.h` | Physical atmosphere (Hillaire) |
| `SkyVisibility.h` | Sky occlusion maps |
| `ShadowCascades.h` | Sun cascades with PCSS |
| `LightClusters.h` | Clustered point and spot lights; shadow cubes for the 8 nearest |

Settings flow through these files:
- Sky & Fog dialog: `App/MainWindow.cpp`
- `WorldAtmosphere`: `State/WorldScene.*`
- Export: `State/WorldRuntime.cpp`
- Player format, now v18: `Engine/World/PlayableScene.*`
- Player `View`: `Player/PlayerRender.h`, which must stay a multiple of 16 bytes for D3D11

## Status (2026-11-22)
**Realism** is 94.9 over 4 views (mean FLIP 0.0508). The Cycles comparison runs with fog disabled.

**Street frame time** (`--street`, GTX 1650 laptop on the Windows "Power saver" plan) is 13.6–14.1 ms on average with fog, screen-space reflections and contact shadows (15.6–15.8 ms at p95; the budget is 16.7 ms, so the margin is thin on this plan; 13.2–13.5 ms before contact shadows, 12.7–13.0 ms with fog alone). The same machine and plan measured 10.0 ms before fog, so the fog costs about 2.8 ms. The plan matters: before fog the street measured about 7 ms on a faster plan. Compare timings only within one run of measurements.

**R1 done:**
- HDR, EV100 exposure and AgX tonemapping
- TAA and GTAO
- Cascaded sun shadows with PCSS
- Sky occlusion and screen-space bounce light
- A specular model that matches Cycles
- Physical sky
- Clustered point and spot lights, with point light shadows
- Bloom and specular occlusion
- TAA split into its own pass
- Depth of field and motion blur
- Volumetric height fog
- Screen-space reflections
- Contact shadows

## Next, in order
1. **Per-object motion vectors.** Today TAA reprojection and motion blur use only the camera's motion, so moving cars ghost under TAA. Step A is done: the scene pass has a fourth target, `velocity` (`HdrFrame::velocityFormat`, RG16F, cleared to 0), which `Temporal.frag` (at the nearest-depth neighbour) and `MotionBlur.frag` already add to the camera's motion; the scene shaders write 0 for now, so images are unchanged. Remaining steps:
   - Both renderers rebuild moving objects' world-space vertices on the CPU each frame (`Player/main.cpp` `vertices()`, the editor's `Geometry::triangles`). Keep the previous frame's positions as a second, tightly packed vertex stream (one `Point3` per vertex, binding 1), uploaded with each frame. When the vertex count or order changed since the last frame, upload the current positions instead (no object motion for that frame).
   - The scene vertex shaders pass two clip positions, both through the previous frame's camera (`lighting.previousViewProjection`, which must then always hold the previous camera, with separate on flags): the previous position and the current one. The fragment writes their screen-space difference (object motion only) to a fourth scene target, `velocity` (R16G16_SFLOAT, cleared to 0, so sky and static geometry need nothing).
   - Notes from step A: the scene vertex shaders cannot include `Lighting.glsl` (it uses `gl_FragCoord`). Declare a second block on binding 1 that reads only the end of the Lighting block, for example `layout(std140, set = 0, binding = 1) uniform LightingMotion { layout(offset = 1904) mat4 previousViewProjection; vec4 reflectionSettings; } motion;`, and give binding 1 the vertex stage in the editor's layout (`WorldLightingVulkan.cpp`; the Player's already has it). Wind sway moves foliage every frame (`Scene.vert` uses `camera.eye.w`, `PlayScene.vert` uses `view.time.x`), so pass the previous frame's wind time too (for example in `reflectionSettings.w`). The rendering audit's "Motion blur leaves a still view unchanged" will then see swaying foliage blur, if the view has any: exclude wind or expect it.
   - Test: in the street audit's Player run with traffic, or a rendering audit case that renders two frames with a moved object and checks the blur direction.
2. **R2 materials:**
   - clear coat, sheen, transmission and glass
   - subsurface and anisotropy
   - glTF material extensions
   - wet and puddle layers
   - decals and parallax occlusion
3. **R3 GI.** Bounce light from beyond the screen is the largest remaining gap against Cycles.
