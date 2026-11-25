# WorldForge handoff

Read [WorldForge-Development-Playbook.md](WorldForge-Development-Playbook.md) first: it is the working method (verification, review checklist, pitfalls) and the detailed plan of what to do next. Then read this handoff for the build, the frame architecture and the current status. The strategy is in `WorldForge-Strategy-and-Roadmap.md`, and each finished rendering feature has a row in its stage's status table.

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
| `--r3-performance` | The street mission in the Player with R3 lighting off, then on. |
| `--district` | Generates the City District (R4 test scene), flies the Player's `--verify-district` route, and records frame, CPU and per-pass GPU times, draw calls, triangles and memory. |
| `--aerial-perspective` | The district in clear air, haze and fog: contrast must fall with distance, near buildings stay readable, editor and Player match. |

`Tests/Run-Verification.ps1` builds and runs any of these one after another and prints a summary (see the playbook, section 4).

- If the full audit stops writing `controls.csv`, a modal dialog is open outside `withDialog`.
- The Authoring check "Sun casts visible geometry shadows" is flaky. Rerun it before treating a failure as real.
- Run one audit at a time, with a visible window. Two audits at once, or a leftover audit process, slow the GPU and time each other out. An audit started with a hidden window (for example `Start-Process -WindowStyle Hidden`) fails the rendering check "The street renders" with "No Vulkan frame", because its viewport is never exposed.
- Timings depend on the Windows power plan. Before blaming a change, measure the previous commit in the same session.

## Frame architecture (`Engine/Render/Vulkan/HdrFrame.*`, shared by the editor and the Player)
0. **Fog volume** (`FogVolume.comp`, `Vulkan/FogVolume.*`). After the shadow maps, a compute pass walks each of 160 × 90 froxel columns (64 log-spaced slices to `probeSettings.w` meters). It stores the height fog's shadowed share per unit of opacity (a pixel reads one texel and multiplies by its own opacity) and, near the local mist, each froxel's sun visibility and lamp light (read at each step of the mist's march). The scene and sky shaders read it at set 0 binding 12.
1. **Scene pass.** Writes four color targets (scene; indirect sky light with the depth along the camera's forward axis in alpha; albedo plus fog transmission; each surface's own motion, from previous vertex positions as a second vertex stream, see `SceneMotion.glsl`) and depth. Shading is in `Shaders/Shading.glsl` and `Lighting.glsl`. Screen-space reflections and contact shadows trace the previous frame's TAA result here (binding 8, `HdrFrame::previousFrame`), which the renderers rebind every frame. The editor restarts the TAA average one frame after a camera move or edit, so averaged frames never trace another view.
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
- Player format, now v21 for clear coat: `Engine/World/PlayableScene.*`
- Player `View`: `Player/PlayerRender.h`, which must stay a multiple of 16 bytes for D3D11

## Status (2026-11-25, day)
Read the playbook's section 10.0.3 for the next 10 hours (pending items listed there) and 10.0.4 for the 10 after.

**What changed today:**
- **Fog volume** (`FogVolume.comp`, `Engine/Render/Vulkan/FogVolume.*`): height fog's sun shadows come from a 160 × 90 × 64 froxel volume filled after the shadow maps, read once per pixel. Height fog now costs about 0.14 ms of the street's scene pass. The local mist reads the volume's point values at each step of its march.
- **Probe hit lighting:** probe rays light the surfaces they hit with the sky and the nearest 32 lamps (`ProbeGeometry.h` `HitLighting`). The night view's wall gap is bounce light (Cycles with no diffuse bounces shows it), but the probe grid is too small to reach those walls yet.
- **New checks:** `--aerial-perspective`; in `--rendering`, lamp falloff and shadow, auto exposure walking into a lit room, cloud transitions at five play times (`WORLDFORGE_CAPTURE_SECONDS`), and golden images `fog-sunset.png` and `lamp-mist.png`. The rendering ctest timeout is now 300 s.
- **World partitions (R4.6, Player side):** playable format 30 groups static meshes into 64 m cells (`readPlayableIndex`, `readPlayableCell`); the Player streams them with `WORLDFORGE_CELL_STREAMING=1`; `--district-cells` checks three laps.

**Frame times** (GTX 1650, Windows "Power saver"):
- The street is about 13 ms p95; with all R3 lighting about 13.9 ms. With height fog and a 9 m local mist it is 15.95 ms (16.4 last night).
- The 6 × 6 district is 13.6–14.2 ms p95; the textured 12 × 12 district is 17–18 ms p95 fully resident in this session, 18.9 ms streaming cells.

**Checks:** realism 95.2 with probe GI (FLIP 0.0480), rendering 62/62, aerial perspective 8/8, ctest 9/9, the full audit 1374–1375 PASS.

**Night:** probe GI now has a coarse 6 m grid and the sky term above the horizon where probes reach, which closed the night walls' bounce-light gap; probe GI is on in the realism views.

## Status (2026-11-25, morning)
**Frame times** (GTX 1650 laptop, Windows "Power saver"):
- The street runs at 11.4–11.7 ms average and 12.5–13.0 ms p95.
- With all R3 lighting it is 14.0 ms p95; with height fog and a 9 m local mist as well, 16.3 ms.
- The 6 × 6 district runs at 9.8 ms average and 14.6 ms p95 over the whole route. The textured 12 × 12 district is 16.4 ms p95.
- Every route is under the 16.7 ms budget.

**What changed:**
- **Lamp ranges** follow the exposure: an 1800 cd lamp no longer reaches 85 m by day.
- **Draw order:** opaque ranges draw nearest first.
- **Sky irradiance** adds spherical-harmonic band 4 (9 more coefficients at the end of the lighting block, 2160 bytes): upward surfaces at twilight were 11% too bright.
- **Split lookups under TAA:** sky openness, PCSS, GTAO, the height fog's shadow march and the local mist are split across pixels and frames. Both renderers pass the flag in `probeSettings.z`. Without TAA every lookup is taken as before. See playbook section 10.0 for the lessons, including what not to do in the scene shader.

**Checks:** realism is 95.0 (FLIP 0.0498), rendering 50/50, the full audit 1375 PASS, and `--street-profile` breaks the street's GPU time down by feature.

**Flicker:** the rendering audit compares pans with the split lookups and with every lookup taken (`WORLDFORGE_FULL_LOOKUPS=1`). The split adds at most 0.02 levels of change between frames.

## Status (2026-11-22)
**Realism** is 94.9 over 4 views (mean FLIP 0.0508). The Cycles comparison runs with fog disabled.

**Street frame time** (`--street`, GTX 1650 laptop on the Windows "Power saver" plan) is 13.6–14.1 ms on average with fog, screen-space reflections and contact shadows (15.6–15.8 ms at p95; the budget is 16.7 ms, so the margin is thin on this plan; 13.2–13.5 ms before contact shadows, 12.7–13.0 ms with fog alone). The same machine and plan measured 10.0 ms before fog, so the fog costs about 2.8 ms. The plan matters: before fog the street measured about 7 ms on a faster plan. Compare timings only within one run of measurements.
Later on 2026-11-22, after per-object motion vectors, the same street measured 9.5–9.9 ms on average (p95 15.0 ms, one run spiking to 18.7 ms) with the machine apparently on a faster power plan; compare only measurements taken together.

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
- Per-object motion vectors

**R2 materials:** The Vulkan editor and Player support clear coat, cloth sheen, tinted thin glass, local subsurface scattering, anisotropic reflection, seven material layer maps, wet surfaces, puddles, mesh decals, and parallax occlusion. GLB interchange supports five Khronos material extensions. Playable formats 21–26 introduce the R2 material parameters and retain support for earlier packages.

The final Vulkan rendering audit passed 42 checks. The surface-library audit passed 18 checks, including GLB and project round trips. The playable street audit passed 12 checks and measured 13.83 ms p95 on the GTX 1650. The street material comparisons ranged from 0.21 to 0.43 display levels between the editor and Player. The full editor audit passed 1,375 checks on retry, with three skips. Both Material tab navigation checks passed after the initial timeout.

**R3 lighting:** The Vulkan editor and Player use a 6 × 6 × 4 grid of world-space probes for single-bounce light from sunlit and emissive geometry. The grid follows the camera and refreshes as the scene moves. Screen-space GI traces at half resolution and rejects samples across depth edges at full resolution. Reflection probes use 8 × 8 directional captures with first-hit distances for parallax correction. The three controls are in Sky & Fog, start off by default, and export through playable format 27.

The GTX 1650 passed the GPU probe test and all 45 rendering checks. Editor and Player comparisons for the three effects differed by at most 0.05 display levels on average. Run the editor audit with `--r3-performance` to compare the street mission with all three controls off and on. The final benchmark measured 15.42 ms p95 off and 15.74 ms p95 on. With R3 on it peaked at 201 MiB working set and recorded a longest frame of 61.72 ms.

**Atmosphere before R4:** The physical sky uses a more precise lookup for grazing sunlight. The visible sun disc matches the angular size used by the shadow filter. Height fog and local mist now receive the same physical sunlight and skylight, and fog shadow samples vary across pixels and frames to reduce visible bands.

## Next, in order
The detailed, step-by-step version of this list, with files, methods, tests and acceptance criteria, is section 10 of the [development playbook](WorldForge-Development-Playbook.md#10-the-plan-what-to-do-next). Continue its next 8-hour plan (10.0.1); the temporal stability check is done, so the next item is the half-resolution volumetric pass. The paragraphs below are history.

The district profiling audit will reuse the same exported starter and camera route for the original settings and five single-feature variations. The Player's timestamps will give street and rooftop scene-pass times for each. Each run must complete on Vulkan. Compare the variations in the same session before changing the scene shaders. The street-level target is 16.7 ms p95.

The profile identified lamp shading as the largest cost. Refine the light sphere against the screen-tile sides within each depth slice, then reject out-of-range and back-facing lamp samples before shadow lookups on surfaces without a coat or subsurface scattering. Verify that valid lamp contributions stay in their clusters and recheck the renderings and district on the same power plan.

Lamp color will use the same scene-linear RGB in the editor's light clusters and the Player's version 29 package. The rendering audit will capture the same warm and cool lamp from an identical view with and without mist, requiring a visible color difference and an editor–Player difference at or below one display level.

The first draw-submission change will preserve the Player's existing vertex order and cache mesh ranges whose sizes stay fixed. The Vulkan vertex ring will refresh only moving ranges and the terrain and effects suffix after its first upload. Meshes with variable vertex counts keep the existing full upload until the culling and LOD work can handle them. The rendering and motion audits will check that the cache preserves the same images.

A shared Vulkan compute pass will test opaque world-space bounds against the camera and sun cascades and write indirect draws into each in-flight frame's buffers. Use the conservative winding bounds for foliage and preserve submission order. The direct path remains available for queues without compute or when indirect resources fail to allocate. The district will compare rendered images and report how much geometry the GPU rejected.

For the horizon crossing, use the visible fraction of the solar disc to drive direct illumination and the geometric horizon to clip the rendered disc. Keep the atmosphere and shadow filter in agreement on the disc's radius. Verify the sunlight across a sweep through the horizon before extending the moving-shot exposure audit.

The 12 × 12 route spends more than 6 ms in point-light shadows when the nearest-lamp set changes. Keep lamps that remain selected in their previous atlas slots. Rebuild only changed cubemaps between periodic geometry refreshes, using a load pass that preserves the other tiles. Compare direct and indirect district captures and remeasure the point-shadow pass on the same route.

The point-shadow atlas now keeps unchanged lamps in the same slots and redraws only changed cubemaps between periodic geometry refreshes. In the first 12 × 12 run with this change, it updated an average of 2.78 faces per frame and spent 1.10 ms on the street and 1.00 ms above the roofs. Still images now average temporal history at the same pixel. The 6 × 6 editor–Player comparison passed at 0.470 display levels.

The district report now counts draws and triangles that survive the GPU frustum test. The editor and the sun cascades can use compacted indirect draws. `WORLDFORGE_FIXED_INDIRECT=1` selects fixed-size indirect submission, and `--direct` selects ordinary draw commands. The `district-compare` mode runs a back-to-back comparison.

The `district-textured` audit exports a 12 × 12 district with five generated facade textures. The `sunrise` audit measures 73 exposure histograms in each renderer. Its first pass detected a 1.30-stop step after the horizon crossing. The physical atmosphere now includes diffuse skylight reflected from distant ground.

1. **Atmosphere and lighting realism.** Continue with moving sunrise and sunset shots, night lighting, distance haze, and weather. Use the [realism checklist](WorldForge-Strategy-and-Roadmap.md#realism-pass-before-city-scaling) to record visual and performance results on the Playable Street.
2. **R4 city scale.** Use the [R4 implementation checklist](WorldForge-Strategy-and-Roadmap.md#r4-implementation-checklist) to establish a district baseline and begin GPU-driven drawing after the realism pass.
