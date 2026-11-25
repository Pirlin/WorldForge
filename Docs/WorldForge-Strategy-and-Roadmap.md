# WorldForge strategy and roadmap

## The goal

WorldForge should be one application that covers the whole production of an original 3D game:

- world building, modeling, sculpting, texturing and image editing;
- characters and animation;
- gameplay code, NPCs, effects, audio and UI;
- testing, and a shippable build.

AI should be able to do any of this work through the same tools a person uses. The graphics should be as realistic as the hardware allows, and realism is the top priority.

This document explains how to get there. It sits above the detailed plans:

- [Next development plan](WorldForge-Next-Development-Plan.md)
- [Programming plan](WorldForge-Programming-Plan.md)
- [AI game creation plan](WorldForge-AI-Game-Creation-Plan.md)
- the blueprint in `Development Drive/`

## An honest starting point

Unreal, Blender, Maya, Unity and Photoshop each represent decades of work by hundreds to thousands of engineers. WorldForge cannot rewrite all of them and then go beyond them. A plan that tries will never ship.

What WorldForge can do:

1. **Integrate** the best existing tools.
2. **Own what connects them**: one project, one undo history, one dependency graph, one verification system and one AI interface.
3. **Build itself only the parts where it can be better than everyone else**: the renderer, the runtime, the connected workflow and the verification.

The project already works this way. Studio is a Blender fork that runs inside the editor, physics uses Jolt, and mesh booleans use manifold. This direction should be kept and stated plainly, not treated as a temporary shortcut.

**AI expectations.** "AI that does more than a human" is realistic for volume, speed, consistency and checking. For example, AI can place ten thousand props, play-test every mission after every change, or write and test gameplay code. Taste, direction and final approval stay with the creator. The product should be built so that AI gets better automatically as models improve.

## Where the project stands

**Working today:**

- editor, embedded Studio with publish review, and Forge AI with typed and verified edits;
- Asset Hub, behaviors and native C++ modules;
- the Playable Street running in both the editor and the standalone Player.

**Graphics are the weakest area relative to the goal:**

| Area | Today | Where it is |
| --- | --- | --- |
| Lighting | One sun. GGX specular with Lambert diffuse. | `Components/Shaders/Scene.frag` |
| Shadows | One 3×3 PCF shadow map covering the whole scene. | `Components/Shaders/Lighting.glsl` |
| Ambient | A flat constant times ambient occlusion. No image-based lighting and no global illumination. | `Scene.frag` |
| Sky and fog | Gradient and physical skies, shadowed height fog, and one local mist sphere. | `Sky.frag`, `Lighting.glsl` |
| Color | No HDR target, exposure or tone mapping. The Player presents to an 8-bit swap chain. | `Player/main.cpp` |
| Post-processing | None: no anti-aliasing resolve, bloom, depth of field or motion blur. | n/a |
| Renderers | Two hand-maintained copies: Vulkan in the editor and Direct3D 11 in the Player. | `LiveViewport.cpp`, `Player/WorldShaders.h` |

**Development machine.** The GTX 1650 (4 GB) has Vulkan 1.4 and `VK_EXT_mesh_shader`, but **no ray tracing extensions**. Hardware ray tracing can't be built or tested on this machine. It stays the minimum-spec test machine. High-end features need an RTX-class GPU with at least 12 GB.

## Seven strategic decisions

### 1. Integrate the best tools and own the connections

| Need | Approach |
| --- | --- |
| Modeling, sculpting, UVs, rigging, animation | Studio (the Blender fork), already integrated. |
| Texture painting and image editing | Studio texture painting first. Add an embedded image editor later as a separate process, the same way Studio is integrated. |
| Physics | Jolt. |
| Mesh booleans and remeshing | manifold. |
| Rendering, runtime, gameplay, project, verification, AI | Built by WorldForge. |

WorldForge stands out through how these pieces work together: dependency tracking, publish review, one undo step, play verification, and AI that uses the same commands.

### 2. One command layer for people, scripts and AI, exposed over MCP

Every edit is already a typed command that checks the revision and can be undone (`ProjectStore`, Forge AI commands). The next steps:

1. Make the command registry complete, so every editor action is a registered command.
2. Expose the registry through a local **MCP server**. Any capable model (Claude or others) can then create and change games with the same commands, permissions and review a person uses.
3. Keep the current rule-based planner as an offline fallback.

This is how AI keeps getting better without WorldForge training its own models.

### 3. Verification is part of the product

Every change, whether made by a person or by AI, should be able to prove it worked:

- **Play tests**, as the Playable Street audit already does.
- **Performance budgets** on real hardware.
- **Image tests** (new, see decision 5).

AI must pass these checks before a change can be committed.

### 4. One renderer for the editor and the game

1. Move the renderer out of `LiveViewport.cpp` into a shared engine library built on Vulkan with a render graph.
2. Use it in both the editor and the Player.
3. Retire the Direct3D 11 copy.

Every graphics improvement then lands in the game and the editor at once, and the editor shows exactly what players will see.

### 5. Realism that can be measured

The **Cycles reference check** is a feature no other engine offers. Studio already contains Blender's path tracer, Cycles, and it can be used as the source of truth:

1. Render the same camera view of a scene in Cycles and in the real-time renderer.
2. Compare the two images with NVIDIA's FLIP metric (BSD-licensed).
3. Report the difference per scene as a **realism score**.

Every rendering change in the roadmap below is accepted only if it improves this score without breaking the frame budget. AI can use the same score to check lighting and material changes.

### 6. A safe script language for AI-written gameplay

The three gameplay layers each have a role:

- **Behavior graphs** for designers.
- **Luau scripts** for most gameplay. Luau is small, fast, typed, sandboxed and MIT-licensed, which makes it safe for AI-written code.
- **Native C++ modules** for performance-critical systems.

All three call the same typed engine API, and AI-written scripts must pass a play test.

### 7. Keep the license boundary clean

Blender is GPL. Keep Studio a separate program that talks to the editor through files, as it does now, and never link Blender code into the engine, editor or Player. Shipped games must contain only WorldForge runtime code and permissively licensed libraries. Have a lawyer review this before any commercial release.

## Realism roadmap

Each stage ends with three checks:

- the Cycles realism score for the Playable Street improves;
- golden images pass;
- the street stays within budget on the GTX 1650: 60 fps at 1080p, with the high-end tier excepted.

### R0: Renderer foundation

- **One renderer.** Extract the shared Vulkan renderer library and move the Player onto it.
- **Render graph.** Add a render graph with transient resources.
- **HDR pipeline.** Render to a 16-bit float target.
- **Physical light units.** Sun in lux, sky in nits, camera exposure in EV100, plus auto exposure.
- **Matching color.** Tone map with **AgX**, Blender's default view transform, so assets look the same in Studio and in the game.
- **Anti-aliasing.** Add TAA with motion vectors.
- **Image tests.** Add a headless golden-image test harness, and the first Cycles comparison of the street.

**R0 status (2026-11-20).**

| Item | State |
| --- | --- |
| Linear color | Done in the editor viewport and the Player. Authored colors and base-color and emission maps are decoded from sRGB. glTF color factors are converted on import and export. |
| HDR target | Done. A 16-bit float scene target in both renderers. Editor lines and overlays draw after the display transform, so their colors are unchanged. |
| Exposure and AgX | Done. Exposure (EV) and the view transform (AgX or Standard) are saved per project and exported in Player format 14. |
| AgX matches Studio | Fitted to Studio's own output by `Tests/FitAgx.py`. Greys are within 0.1 of an 8-bit level. Colors average under 1 level; fully saturated primaries reach 16. The core tests check this. |
| Golden images | Done. The `--rendering` audit checks three street views in the editor and in the Player against `Tests/Golden`, and checks that the two match. On the GTX 1650 they differ by 0.05 of an 8-bit level on average, only at edges and shadow borders. |
| One renderer | Mostly done. A Qt-free Vulkan core in `Engine/Render/Vulkan` holds the loader and the HDR frame pass, used by both the editor viewport and the Player. Sky, shadow, fog, color and display shaders live in `Engine/Render/Shaders` and are shared. Two differences remain. First, the editor still lights scene geometry on the CPU while the Player uses `PlayScene` on the GPU, and R1 replaces both with one per-pixel shader. Second, Direct3D 11 stays as a fallback without a Vulkan driver and for CI on GitHub runners without a GPU, with its own HLSL copy. Retiring it needs a software Vulkan driver for CI.  (Update: R1 replaced both with one per-pixel shader; see R1 status.) |
| Cycles comparison | Done. `WorldForgeEditorAudit --realism` renders three street views in the Player and in Cycles (`Tests/CyclesReference.py`, run by Studio) and scores each pair with LDR-FLIP (`Engine/Render/Flip.cpp`, following NVIDIA's reference implementation). Both render without fog. The first scores are 0.216 (shop front), 0.293 (street corner) and 0.215 (aerial): a **realism score of 75.9 of 100**. The audit fails if a view gets more than 0.01 worse than `Tests/Golden/realism.json`. Each view takes 22–31 s on the CPU (Studio has no GPU rendering), denoised, and Cycles gives the same image every run. |
| Physical light units | Done. Directional lights are authored in lux and point lights in candela, and camera exposure in EV100. Renderers keep working in scene units pre-exposed for EV100 15 (one unit is 39,322 nits, the brightest luminance that camera records), which keeps sunlight inside 16-bit float range. The old unitless intensity 1 equals 123,532 lux, so older projects look the same. The sky is still authored as colors, and its brightness in nits comes with the physical atmosphere in R1. Until then, the ambient term scales with the brightness of the sky colors relative to the default sky (exactly 1 for the default), so night scenes with a dark sky are no longer lit by a constant daylight ambient. |
| Auto exposure | Done in the editor and the Vulkan Player. The display pass counts every fourth pixel's luminance into a 64-bin histogram with GPU atomics, so there is no extra pass. The CPU reads it back a frame later, averages the 10th to 90th percentile, and adapts toward showing that average as middle grey, with exposure compensation. Captures settle at once. The `--rendering` audit checks that auto exposure rescues a far-too-dark manual exposure, that compensation works, and that the Player and editor choose the same exposure. The Direct3D 11 fallback uses the manual exposure. |
| TAA | Done in the editor and the Vulkan Player. The frame is now a scene pass followed by a display pass with two subpasses: a TAA resolve into ping-pong HDR history targets, then the display step. Editor lines and overlays still draw after the view transform, unjittered. The scene uses a 16-sample Halton(2,3) jitter. The history is reprojected from depth and the camera matrices, and clipped to the 3×3 neighbourhood's colour variance in YCoCg. A still editor view averages exactly 16 frames, so captures are deterministic, and the Player's captures do the same. On the street, TAA changes 0.6% of pixels, all on edges, and editor and Player still agree within 0.1 of a level. Not yet done: per-object motion vectors (moving cars rely on the clamp), and the Direct3D 11 fallback has no TAA. |

What the first comparison shows, in order of size:

1. **No sky light.** The engine adds a constant grey ambient light. In Cycles the blue sky lights the scene, so shadows and the ground turn blue-grey and the sky is blocked where geometry hides it.
2. **No occlusion.** The open shop doorway is lit in the engine and black in Cycles. Wall bases and the ground near objects are too bright.
3. **Back-lit walls.** With the low sun (14 degrees), walls facing away from it are much darker in Cycles. The engine's ambient keeps them bright.
4. **No specular on most surfaces.** The street uses the simple vertex-lit path, which has no reflections. Cycles shows the road and ground reflecting the sky at grazing angles.

R1's sky-driven image-based lighting, ambient occlusion and per-pixel PBR target exactly these differences.

The reference reads engine light intensity as "a white surface facing the light reflects that much light", which is what the engine's lighting assumes. Physical light units (lux and nits) will replace that convention, and the reference script will change with it.

The default fog (0.024 per meter) is far denser than real air. Linear blending makes that visible: distant buildings fade more than before. The physically based atmosphere in R1 replaces it.

### R1: Physically based lighting

- **Atmosphere.** A physically based sky and atmosphere (Hillaire 2020 method) drives the sky, the sun color and the ambient light.
- **Image-based lighting.** Split-sum prefiltered specular plus spherical-harmonic irradiance, updated when time of day changes.
- **Shadows.** Four cascaded shadow maps with soft contact hardening (PCSS) and contact shadows.
- **Many lights.** Clustered lighting for hundreds of street lights, signs and car lights.
- **Occlusion and reflections.** Ground-truth ambient occlusion (GTAO), specular occlusion and screen-space reflections.
- **Volumetrics.** Shadowed height fog, with a froxel grid planned for more local volumes.
- **Camera effects.** Bloom, physically based depth of field, motion blur and lens effects.

**R1 status (2026-11-22).**

| Item | State |
| --- | --- |
| One shading model | Done for Vulkan. Every surface in the editor's world views and in the Player is lit per pixel by `Engine/Render/Shaders/Shading.glsl`: GGX specular, Lambert diffuse, the sun with its shadow map, up to 16 other lights with inverse-square falloff, and the sky as image-based light. The editor no longer bakes lighting into vertices; modeling views keep their studio light. Editor and Player match within 0.22 of an 8-bit level. The Direct3D 11 fallback keeps the earlier lighting. |
| Image-based lighting from the sky | First step done. `Engine/Render/SkyLight.h` projects the visible sky (gradient and sun glow) onto nine spherical-harmonic coefficients of irradiance. Below the horizon it uses a flat ground of 25% albedo lit by the sun and sky. Reflections look up the sky in the reflected direction, blurred toward the irradiance with roughness, and use the split-sum environment BRDF. The core tests check the irradiance of a uniform sky on floors and walls. |
| Realism | The score rose from **75.9 to 90.0** (mean FLIP 0.241 → 0.100; shop front 0.215 → 0.096, street corner 0.292 → 0.157, aerial 0.215 → 0.048). |
| Ground-truth ambient occlusion | Done for Vulkan. `Engine/Render/Shaders/Ambient.frag` computes GTAO (Jimenez 2016) in world space from the scene depth: 2 slices of 6 steps per frame within 1.5 m, turned 1/16 of their spacing each TAA frame so a still view averages 32 evenly spaced directions. The scene pass writes its sky-light part and diffuse albedo to a second target, and the TAA resolve takes the occluded share out of it (the multi-bounce fit used at first was replaced by explicit bounce light; see below). Three details keep open surfaces unbiased: a 16-bit float target, so noisy frames above 1 are not clamped before they average; samples at texel centers; and a 0.06 rad angle bias against normals tilted by depth quantization. **Sky & Fog → Ambient occlusion** turns it off. The realism score holds at **90.0** (mean FLIP 0.1002 → 0.1001; shop front 0.0959 → 0.0952, street corner 0.1566 → 0.1557, aerial 0.0482 → 0.0494). Contact darkening at object bases matches Cycles in shadow, but in the sun Cycles brightens those spots with light bounced from the sunlit faces, which the engine does not have yet. |
| Cascaded shadows with PCSS | Done for Vulkan. `Engine/Render/ShadowCascades.h` fits four cascades to the view, between the nearest and farthest scene points it sees (at most 250 m), with splits blended 70% logarithmic. Each is a 1536² tile of a 3072² atlas, fitted to a sphere around its slice and moved in whole texels, so shadows neither change size nor shimmer as the camera turns or moves; the nearest one is about 1 cm per texel at street level. `Lighting.glsl` uses the finest cascade that holds a point, dithered into the next across the last 8% of a tile, with a normal offset scaled by the texel size. Soft lookups use PCSS: a 16-sample blocker search, then 16 filter samples over the penumbra a 0.53° sun casts at the blocker distance, turned each TAA frame. The stair-stepped edges are gone, and editor and Player now match within 0.07 of an 8-bit level. The realism score stays at **90.0**, since shadow edges are a small share of the pixels. A core test checks coverage, ordering, caster depth and whole-texel movement. |
| Large-scale sky occlusion | Done for Vulkan. `Engine/Render/SkyVisibility.h` renders the scene's depth from 16 directions over the sky (a Fibonacci spiral above 4.6° elevation) into 512² tiles of a 2048² atlas, covering the scene bounds up to 150 m from their center. `skyOpenness` in `Shading.glsl` weights each direction by its sky luminance times the cosine to the normal (plus 0.05, so downward faces still see the sky around them) and counts it only if its map shows nothing above the point. The lookup moves within a texel each TAA frame, softening the coarse maps. The result scales the sky light, both diffuse and reflected, before GTAO; the Ambient occlusion setting turns both on. The shop doorway is now dark inside, as in Cycles. Realism **90.0 → 90.1** (shop front 0.0952 → 0.0924, street corner 0.1556 → 0.1544, aerial 0.0496 → 0.0506, where ground beside walls darkens more than Cycles, which brightens it with bounce light). One set of maps serves all frames. It is re-rendered (16 draws of the scene) when the scene bounds move, after an edit, and at most every 30 frames while a running game moves objects. |
| Screen-space bounce light | Done for Vulkan. The GTAO search in `Ambient.frag` also gathers one bounce: when a sample raises a slice's horizon, the arc of the cosine lobe it newly hides sees that sample's lit color in this frame's scene instead of the sky. The sum, weighted like GTAO's visibility, is the radiance a white surface would reflect. The scene pass writes each surface's diffuse albedo and fog transmission to a third target (RGBA8), and the TAA resolve adds albedo × bounce × transmission while taking out the occluded sky light. The occlusion target is RGBA16F: bounce in RGB, visibility in alpha. Only what is on screen within 1.5 m bounces. Realism **90.1 → 90.2** (shop front 0.0924 → 0.0917, street corner 0.1544 → 0.1531, aerial 0.0506 → 0.0484). The street averages about 7 ms a frame on the GTX 1650 (p95 about 10–13 ms), mostly from per-pixel sky and bounce lookups. |
| Specular model matched to Cycles | Done. The comparison sheets showed the engine missing the broad sun glare that rough roads show when seen toward a low sun. The cause was the visibility term. The engine used the UE4 Smith approximation with roughness remapped to k = (r+1)²/8, which darkens grazing light on purpose. Cycles' Principled BSDF uses the exact height-correlated Smith term with α = r². At a 14° sun seen from 9° the two differ by about 2.5×. `directLight` now uses the height-correlated term, plus multiple-scattering compensation (Turquin 2019, 1 + F0(1/E − 1), with E from the split-sum fit) on direct and image-based specular. Realism **90.2 → 94.3** (shop front 0.0917 → 0.0665, street corner 0.1531 → 0.0647, aerial 0.0484 → 0.0394). |
| Shadow edges in place | Done. The sun shadow lookup used to move off the surface by up to 2.5 texels along the normal. Under a low sun that moved shadow edges toward their casters by about four times the lift, so the street-corner shadow ended a few pixels short of Cycles'. The lookup now moves half a texel along the normal and the rest toward the sun, which changes only the compared depth. Edges land within a pixel of the reference, and lit ground is no noisier than Cycles'. Realism **94.3 → 94.4**. |
| Prefiltered sky reflections | Done. `color::skyLight` tabulates the sky prefiltered with the GGX lobe for 8 roughness levels by 16 reflected elevations (512 importance samples each, split-sum with Karis' NdotL weighting); the gradient sky depends on elevation only, so the table is exact for it. Upward reflections leave out the part of the lobe below the horizon, which on ground and roofs lies under the surface and never reaches the ground. `skyReflection` in `Shading.glsl` interpolates the table and uses the sky itself for mirror-like surfaces. It replaces the analytic blur toward the irradiance. Realism **94.4 → 95.1** (shop front 0.0664 → 0.0567, street corner 0.0627 → 0.0555, aerial 0.0391 → 0.0334). |
| Physical atmosphere | Done for Vulkan (**Sky & Fog → Sky model → Physical atmosphere**). `Engine/Render/Atmosphere.h` implements Hillaire 2020 on the CPU: Rayleigh, Mie (Cornette-Shanks, g = 0.8) and ozone over an Earth-sized planet, a transmittance table, and the multiple-scattering table (ψ_ms = L₂ / (1 − f_ms)). Sky radiance is ray-marched in 32 steps with analytic per-step integration. It is computed when the sun changes, about 0.1–0.3 s. The authored sun intensity is sunlight with the sun overhead. A low sun reaches the ground through more air, dimmer and warmer, and the sky's brightness follows from the same sunlight above the atmosphere. Both skies now go through one environment (`color::skyLightFrom`): a 128 × 64 radiance map with rows closer near the horizon, GGX-prefiltered levels for roughness ⅓, ⅔ and 1 at 64 × 32, and SH irradiance projected from the map. It is uploaded as a storage buffer (binding 5) only when it changes; it replaced the elevation-only reflection table, which cannot hold a sky that depends on azimuth. `Sky.frag` draws the physical sky from the map with a sun disc tinted by the air. The Cycles reference receives the same sky as a 2048 × 1024 equirectangular `sky.hdr` and the tinted sun, and the realism audit now uses the physical sky: **95.9** (shop front 0.0409, street corner 0.0569, aerial 0.0261). The rendering audit checks that editor and Player match with it (0.07 levels). The gradient sky through the map scores 95.2. |
| Clustered lights | Done for Vulkan. `Engine/Render/LightClusters.h` replaces the limit of 16 lights. Up to 1024 lights go in a storage buffer (binding 6), and the view is cut into 16 × 9 screen tiles by 24 depth slices, spaced exponentially from 0.1 m to 1 km. Each frame the CPU projects every point light's sphere of influence to the clusters it touches and lists it there. `shadeSurface` shades the directional lights and the lights of its pixel's cluster. A light's range is where it falls to 2 × 10⁻⁶ scene units, invisible even at night exposures (a 2000 cd lamp reaches about 100 m). Within it the falloff is the inverse square, faded to zero over the last part. The sun may now set (elevation down to −20°): it then gives no direct light, and the physical sky shows twilight. The realism audit adds a **night** view: the street at −6° with six 2000 cd lamps on 5 m posts at EV100 5, scoring FLIP 0.114 against Cycles. Its largest error is lamp light reaching inside the shop through walls, since point lights cast no shadows yet. |
| Point light shadows | Done for Vulkan. The 8 point lights nearest the camera, among those on screen, each get a shadow cube: six 90° faces of 256² out to the light's range, in a shared 2048 × 1536 depth atlas (binding 7). Their matrices are in the cluster buffer, and a light's data carries its slot. `pointShadow` in `Lighting.glsl` picks the face, offsets off the surface by about a texel, and filters with four samples. The cubes re-render when the shadowed lights change, after an edit, and every 30 frames while a game runs. Night realism FLIP **0.114 → 0.078**: lamps no longer light the shop through its walls. Overall **95.0** over four views. |
| Bloom | Done for Vulkan (**Sky & Fog → Bloom**, on by default; Player format 16 stores it when off). Bright light spreads into a soft glow, as a lens scatters it. Before the display pass, `HdrFrame` builds a chain of six images from half size down, starting from the previous frame's TAA result: one frame late, which motion hides and still views converge past. The downsample is Jimenez 2014's 13-tap filter, with a Karis average on the first level so the sun's disc cannot flicker (`BloomDown.frag`); the upsample is a 3 × 3 tent that adds each level into the one above (`BloomUp.frag`). The display step mixes 4% of the blurred light into the scene before exposure, which keeps the total light. Captures render at least two frames so bloom has a previous frame. The Cycles reference renders without bloom, since Cycles has no lens. The rendering audit checks that bloom changes the sun-facing street a little and only spread out (0.62 levels), and that editor and Player still match. |
| Spot lights | Done for Vulkan. **Add → Spot Light** places a light aimed by its rotation (straight down by default), with **Cone angle** and **Cone blend** in Details, as Blender's Spot Size and Blend (default 45° and 0.15). Its intensity is in candela on the axis, like a point light. Lights now take four vec4 in the cluster buffer, adding the spot direction, cos(cone/2) and the scale for Cycles' own falloff, `smoothstep(0, 1, (cos θ − cos(cone/2)) / ((1 − cos(cone/2)) · blend))`. Spot lights share the point lights' shadow cubes. Player format 17 stores the cones, and the Cycles reference makes real SPOT lights. The night view now uses 120° downward spots for its four outer street lamps (FLIP 0.080). |
| Specular occlusion | Done (committed with spot lights in 97e2373, verified after). Sky reflections lose more light in creases than diffuse sky light does, since they come from a narrower cone: `SO = saturate((N·V + AO)^exp2(−16·roughness − 1) − 1 + AO)` (Lagarde and de Rousiers 2014). AO is the material's occlusion times the large-scale sky openness. Diffuse sky light keeps plain AO. For rough surfaces (the street's are all 0.65) it equals AO, so the street's images and scores are unchanged; it matters for glossy materials. |
| Frame passes split | Done. TAA now resolves in its own pass instead of a subpass of the display pass, so later steps can sample the finished frame. Order: scene, occlusion, bloom (from the previous frame), TAA, camera effects, display; the editor draws its overlays in the display pass. The images are unchanged (0.00 levels against the goldens). |
| Depth of field and motion blur | Done for Vulkan (**Sky & Fog → Depth of field**, with **Focus distance** and **Aperture**; **Motion blur**, with **Shutter**; both off by default; Player format 18). Depth of field models a thin lens with the focal length of the field of view on a 24 mm sensor. It gathers 32 samples in a spiral over each pixel's circle of confusion, counting a sample only if its own circle reaches back, so sharp subjects don't bleed into the blur behind them (`DepthOfField.frag`). Motion blur smears each pixel along the camera's screen motion since the last frame (from its depth and the two frames' cameras) for the shutter's share of the frame, in 12 samples (`MotionBlur.frag`). Moving objects are blurred only by camera motion, since there are no per-object motion vectors yet. The rendering audit checks that depth of field blurs the street beyond a 4 m focus at f/1.4, alike in editor and Player (0.03 levels), and that motion blur leaves a still view unchanged. |
| Volumetric height fog | Implemented for Vulkan. `Lighting.glsl` integrates exponential height fog along each view ray to the visible surface or sky. Up to 8 jittered samples of the sun shadow atlas darken the in-scattered sunlight where geometry blocks it; TAA turns the jitter each frame, so a still view averages 128. On the GTX 1650 the shadowed fog costs about 2.8 ms on the street (16 samples cost about 5 ms and changed the images by under 0.1 levels). The sky is now fogged by the same integral as surfaces, so with the street's dense default fog the sky turns the fog color. Fog beyond the atlas still attenuates the background and scatters light. The rendering audit checks that an overhead slab darkens fog against open sky and that the editor and Player match. |
| Screen-space reflections | Done for Vulkan (**Sky & Fog → Screen-space reflections**, on by default; Player format 19 when off). Glossy surfaces (roughness below 0.55, fading out from 0.35) trace their reflection through the previous frame's TAA result in the scene shader (`screenReflection` in `Shading.glsl`): TAA keeps each pixel's depth along the camera's forward axis in the alpha of its result (written by the scene shaders into the sky-light target's alpha, 0 for sky), and the ray is projected with the previous frame's unjittered camera, so it needs no extra pass, copy or G-buffer. 28 steps growing along the ray, then 5 bisection steps; the reflection direction is a GGX sample per pixel, so TAA blurs rough reflections like the lobe. A hit replaces the sky reflection, without specular occlusion. Rays start 3 pixels out and must pass behind a surface by more than 2.5 pixels' worth of their own depth change: each stored depth comes from a jittered sample, and on a floor seen at a grazing angle half a pixel is a large step in depth, which otherwise reads as the ray hitting the surface it left. Off screen, behind another object, or within 3 pixels of a contact, the sky reflection shows instead. The renderers bind the previous frame (`HdrFrame::previousFrame`, binding 8) every frame; Player captures render one warm-up frame so every averaged frame has one. The rendering audit checks that a mirror-like floor shows a block's reflection (3.8 levels) and nothing changes above it, alike in the editor and the Player (0.35 levels). The street's surfaces are rough (0.65), so its images, the realism score (94.9) and its frame time (about 0.5 ms within noise) are unchanged. |
| Contact shadows | Done for Vulkan (**Sky & Fog → Contact shadows**, on by default with sun shadows; Player format 20 when off). `contactShadow` in `Shading.glsl` marches a 0.5 m ray from each sunlit point toward the sun through the previous frame's depth, in 12 jittered steps, like the screen-space reflections, and takes the sun's visibility to 0 where the ray passes behind something up to 30 cm thick. It finds what the shadow map is too coarse for (contacts, gaps under ledges); rays shorter than 4 pixels on screen are skipped. To keep open ground from shadowing itself, a hit needs the ray behind the surface by more than 2.5 pixels' worth of its own depth change plus 0.4% of the depth (the stored depth is 16-bit, and a ray toward a low sun runs close above the ground). The rendering audit checks that a box on a slab darkens around its base (0.65 levels) and leaves open ground unchanged (0.00), alike in the editor and the Player (0.27 levels). The street's goldens and realism score (94.9) are unchanged, since its shadow map already resolves its shadows; the street's frame time rose by about 0.5 ms. The editor now treats the first frame after a camera move or edit as a warm-up and restarts the TAA average after it, as Player captures do, so every averaged frame traces a previous frame of the same view. Open question: the editor's stored depths showed larger jitter errors than the Player's in the same views (self-hits on flat floors that the Player did not have); the slack covers both, but the cause was not found. |
| Per-object motion vectors | Done for Vulkan. The scene pass writes each surface's own motion to a fourth target (`HdrFrame::velocityFormat`, RG16F): the vertex shaders (`SceneMotion.glsl`) project each vertex's previous position and its current one through the previous frame's camera, and the fragment writes their difference on screen, 0 for static geometry and sky. TAA adds it to the camera reprojection (at the nearest-depth neighbour), so moving objects no longer smear their history, and motion blur adds it to the camera's motion. Previous positions: the Player keeps its per-frame world-space vertex buffers in a ring of three and binds the previous one as a second vertex stream (no copying; when the vertex count changed, the current one, so no motion); the editor uploads the previous frame's triangle positions as a second stream on the first frame of each new geometry revision (Play and Simulate move objects by rebuilding geometry). Wind sway uses the previous frame's wind time, so swaying foliage carries its motion too. Motion blur became a simplified reconstruction filter (after McGuire et al. 2012): each pixel blurs along the strongest motion in a 5 x 5 neighbourhood, and a sample counts only where the motion covers the pixel during the shutter, so a moving object smears over the still background beside it while still background stays sharp (before, only the object's inside blurred and its outline stayed sharp). The rendering audit drops a box in Simulate: paused, its top edge's sharpest step is 0.23; one physics step later, with the camera still, 0.07 (smeared over about 7 pixels). Not yet covered by images: the Player's vertex ring (only the editor runs Simulate in the audit; the street audit's Player run with traffic exercises it without image checks). |

What the comparison shows next, in order:

1. **Bounce light beyond the screen and beyond 1.5 m.** Ground beside walls is still darker than in Cycles; R3's global illumination addresses this.

### R2: Materials close to film quality

- **Surface model.** Extend the material model toward **OpenPBR**, which maps closely to Blender's Principled BSDF:
  - clear coat for car paint;
  - sheen for cloth;
  - transmission and thin glass for windows;
  - subsurface scattering for skin and foliage;
  - anisotropy for brushed metal.
- **Import.** Bring these properties in through the glTF material extensions Blender exports.
- **Streets.** Wet-surface and puddle layers, decals, and parallax occlusion.

| R2 item | State |
| --- | --- |
| Clear coat | An independent GGX reflection layer over the base material, with Fresnel attenuation, roughness, weight, and its own normal maps. Playable format 21 introduces the factors. |
| Sheen and thin glass | A Charlie sheen lobe with integrated base attenuation. Thin glass filters the scene by the pane's base color and the index of refraction controls Fresnel reflection. Transparent surfaces draw behind-to-front over the opaque geometry. Playable formats 22 and 23 introduce these factors. |
| Subsurface and anisotropy | Local subsurface scattering uses thickness and scattering distance to shape backlighting. Anisotropic GGX controls brushed-metal reflections with a direction. Playable format 24 introduces these factors. |
| glTF and texture maps | GLB interchange supports the Khronos clear coat, sheen, transmission, IOR, and anisotropy extensions and seven embedded layer maps. The Vulkan editor and Player sample the maps using mesh UVs. Playable format 25 introduces the maps and coat normal scale. |
| Street materials | Wet surfaces and masked puddles use the reflective coating. Parallax occlusion marches through the packed height map. Alpha-masked mesh decals composite over the scene. Playable format 26 introduces these parameters. |

### R3: Global illumination on GPUs without ray tracing

- **Probe GI.** Dynamic probe GI (DDGI-style) using compute-shader ray tracing against distance fields or a GPU BVH. This runs on the GTX 1650 at a reduced update rate.
- **Fine detail.** Screen-space GI.
- **Reflections.** Parallax-corrected reflection probes.

#### R3 implementation checklist

- [x] Build a world-space triangle BVH and verify Vulkan probe tracing on the GTX 1650.
- [x] Integrate a bounded, visibility-weighted probe field in the editor and Player. Verify off-screen diffuse bounce and scene export.
- [x] Add short-range screen-space GI and verify visible color transfer independently of ambient occlusion.
- [x] Add parallax-corrected reflection probes and verify an off-screen reflection in both renderers.
- [x] Run combined lighting, compatibility, rendering, and performance checks.

The final rendering audit passed 45 checks. With all R3 lighting enabled, the GTX 1650 completed the Playable Street mission at 15.74 ms p95 and a peak working set of 201 MiB. The same benchmark measured 15.42 ms p95 with R3 off.

### Realism pass before city scaling

Use the Playable Street for fixed clear-day, low-sun, twilight, and lamp-lit night views. Use the same camera and exposure in the editor and Player. Judge the atmosphere across those views and while moving through the scene. Keep the existing Cycles comparisons for clear-air lighting and add reference checks for haze and fog as those features develop.

- [x] Improve atmospheric transmittance near the horizon and antialias the physical sun disc at the angular size used for shadows. Check grazing light in the core tests and review a fixed low-sun view.
- [x] Match height fog and local mist to the physical sky and sunlight. Verify shadowed fog, dusk brightness, and editor and Player parity.
- [x] Verify the sun crossing the horizon, twilight colors, and exposure during moving sunrise and sunset shots.
  - **Result** (2026-11-25): `--sunrise` and `--sunset` animate the sun from −6° to 12° and back in both renderers. No step between consecutive frames exceeds 0.5 stops of auto-exposure, the zenith stays bluer than the horizon, and the editor and Player match (3/3 checks each).
- [x] Refine evening and night lighting. Verify warm and cool lamps, shadow falloff, light in mist, and exposure transitions from the street into lit interiors.
  - **Result** (2026-11-25, `--rendering`): a 2000 cd lamp 4 m over a slab lights the ground at display levels 0.43, 0.29 and 0.15 at 1.5, 3.5 and 6.5 m, and a 1.5 m block leaves the ground beyond it black (0.21 on the open side); the editor and Player match within 0.05 levels. Walking from the sunlit street into a closed room lit by a lamp, auto exposure opens by about 3.2 stops with no step over 0.11 stops between readings 50 ms apart and settles within 0.03 stops. Warm and cool lamps in mist were already checked; the mist's lamp light now comes from the fog volume and has a golden image (`lamp-mist.png`).
  - **Bounce light** (2026-11-25 night): in the night Cycles comparison the walls were 3–7 levels darker than Cycles, and Cycles with no diffuse bounces is darker by the same amount, so the gap was bounce light. The probe field now has a coarse grid (8 × 8 × 4 probes at 6 m) beside the fine one; probe rays light their hits with the sky (where a ray toward it is open) and the 32 nearest lamps; and where probes reach a point, its sky term leaves out the atmosphere's ground below the horizon, which the probes replace with the real ground (without that, daylight counted the ground twice: +11 levels on a shaded box). The walls now match Cycles within about one level, and probe GI is on in the realism views: FLIP shop front 0.0386, street corner 0.0577, aerial 0.0250, night 0.0708 (without probe GI 0.0411, 0.0579, 0.0250, 0.0750); score 95.2.
- [x] Add distance-dependent aerial perspective for streets, rooftops, and the far skyline. Verify that clear air, haze, and dense fog retain depth as the camera moves.
  - **Result** (2026-11-25, `--aerial-perspective`): on the 6 × 6 district, down the ring road and over the skyline, haze 6/km keeps 0.17–0.39 of clear air's far contrast against 0.34–0.74 near, and a nearly uniform fog keeps 0.32–0.38 far against 0.62–0.86 near. The editor and Player match within 0.3 levels.
- [x] Add cloud cover and weather transitions that change both the visible sky and the light reaching the scene. Check the transitions at street and rooftop height.
  - **Result** (2026-11-25, `--rendering`): the Player renders a clear-to-overcast transition at 0, 5, 10, 15 and 20 s of play (`WORLDFORGE_CAPTURE_SECONDS`). At street and rooftop height the view darkens one way only, by at most 34% of the change per quarter, and halfway it matches the editor holding the halfway coverage within 0.3 levels.
- [x] Profile and reduce the cost of local mist when the camera looks through the volume. Repeat the mission with both fog types enabled against the 16.7 ms p95 street budget.
  - **Result** (2026-11-25, `--r3-performance`): the street mission with all R3 lighting, height fog and a 9 m local mist runs at 16.3 ms p95 (it was 21.1 ms); with R3 lighting alone it is 14.0 ms. `--street-profile` found the cost in shadow lookups, not lamps. Under TAA the sun-shadow, sky-visibility, fog and mist lookups are split across frames and pixels, and the resolve averages them; without TAA every lookup is taken as before.
  - **Flicker:** the rendering audit pans through the misty street with the lookups split and with every lookup taken, including a pan that moves every frame. The split adds at most 0.02 levels of frame-to-frame change.
- [ ] Review stills and moving shots on the GTX 1650. Record scene settings, reference comparisons, editor and Player differences, frame-time percentiles, worst frames, and peak memory before starting city-scale rendering.

The Vulkan rendering audit passed all 47 checks. In the fixed Playable Street sunset view, height fog and local mist changed the image by 32.49 display levels on average. The editor and Player differed by 0.25 levels. The dusk fixture confirmed that local mist dims with the physical sky, and the updated shadow sampling reduced visible bands in the sunset view.

All four clear-air views passed the existing Cycles comparison thresholds at 256 samples. Mean LDR-FLIP was 0.0505 across the four views.

On the GTX 1650, the 5.5-second street mission with R3 lighting and physical height fog measured 16.04 ms p95. With a 9 m local mist volume at density 0.035, two runs measured 19.50 and 20.54 ms p95, with worst frames of 51.63 and 32.13 ms. The gradient sky with that same volume measured 18.52 ms p95. The combined fog setup needs further profiling against the street budget.

### R4: Scale for a whole city

#### R4 implementation checklist

- [x] Establish a repeatable district test scene and camera route. Record visible instances, draw count, CPU and GPU frame times, memory, and streaming stalls with the approved lighting settings.
  - **Scene:** the City District starter (`State/CityDistrict.*`, deterministic from a seed) lays out blocks with sidewalks, buildings of varied height, roads and a lamp at every intersection.
  - **Route:** the Player's `--verify-district` flies a ring-road lap at eye height, then a rooftop orbit.
  - **Report:** frame, CPU and per-pass GPU times (Vulkan timestamps), draw calls, triangles and memory. The editor audit's `--district` mode runs it.
  - **Baseline** (6 × 6 blocks, 2026-11-24, "Power saver"): 24.7 ms average, 43.6 ms p95, with the scene pass at 12.4 ms (street) and 32.5 ms (rooftops) of GPU time. It is per-pixel shading cost; geometry is only 4,500 triangles.
  - **Now** (2026-11-25, after the lamp-range, draw-order and TAA-split changes): 9.8 ms average, 14.6 ms p95 over the whole route (street and rooftops), under the 16.7 ms budget. Scene GPU time is 4.2 ms on the street and 9.5 ms on the rooftops; the textured 12 × 12 route is 16.4 ms p95. Streaming stalls are reported on the textured route (longest 22.1 ms).
  - **Next step:** see the [development playbook](WorldForge-Development-Playbook.md#101-immediately).
- [x] Move instance frustum culling and indirect drawing to Vulkan. Compare visible geometry and shadows with the current draw path in the editor and Player.
  - **Result** (6 × 6, 2026-11-25, "Power saver", `--district-compare`): 179 draws and 2,355 triangles rejected per frame. Route p95 is 21.5 ms indirect versus 26.1 ms direct (street 13.7 versus 15.9 ms). The sun cascades used compacted indirect draws on all 1,500 frames. Editor and Player images match (mean error 0.000 levels) and the rendering goldens are unchanged.
  - **Still open:** compacting the camera draws by material bucket. Camera ranges still bind their material one draw at a time.
- [x] Add occlusion culling and test it during camera turns, fast movement, and across district boundaries.
  - **Result** (`--district-occlusion`): the previous frame's depth pyramid rejects 19.8 more draws per frame than frustum culling alone (92 visible versus 112). The turn frames are pixel-identical with occlusion on and off. p95 is 20.5 ms versus 21.0 ms with frustum culling alone; the district is shading-bound, not geometry-bound.
- [ ] Add meshlets and cluster LOD. Enable the mesh shader path on supported devices and verify the indexed draw path on devices that lack it.
  - **Meshlets are done** (`--district-meshlets`, `--district-meshlets-indexed`): the mesh shader path (463,500 mesh workgroups over the route) and the indexed fallback, with the extension forced off (435,000 indexed draws), both match the editor and the goldens. Frame times are the same within noise (p95 20.8 versus 20.7 ms).
  - **Still open:** cluster LOD (simplified meshlet groups). It waits for meshes dense enough to need it: the district has 5 k triangles in view.
- [x] Stream texture mip levels and evaluate virtual textures against the district's residency and upload budgets. Verify quality when textures enter view.
  - **Result** (12 × 12 textured, `--district-streaming`): 5 promotions to full detail, each within 4 frames of the texture entering view. 7.5 MiB of detail is streamed, the working set peaks at 195 MiB, and the longest upload stall is 22.1 ms.
  - **Virtual textures:** not needed at this scale. All facade textures together are 8.7 MiB. Revisit when R4.6 streams partitions with unique textures per building.
- [ ] Add distant impostors and stream world partitions in the editor and Player. Test transitions, long traversals, and repeated boundary crossings for visual pops and memory growth.
  - **Progress** (2026-11-25, not ticked): the Player side works. Scenes with 256 or more static meshes export as playable format 30, with static meshes grouped into 64 m cells, a cell table holding each cell's bounds, byte range and proxy, and `readPlayableIndex` / `readPlayableCell` to load one cell at a time (core tests cover the round trip, the index skip and one-cell reads). With `WORLDFORGE_CELL_STREAMING=1` the Player keeps cells within 160 m of the camera loaded, reads the others on worker threads as the camera nears them, unloads them past 192 m, and draws the rest as proxies (each small mesh as itself, larger ones as boxes). `--district-cells` flies three laps of the 12 × 12 textured district: 662 cell loads, 638 unloads, none failed; committed memory 420.4 / 421.8 / 422.1 MiB per lap (steady); p95 18.9 ms against 17.6 ms fully resident, worst frame 41 ms.
  - **Frame time** (2026-11-26): streaming costs nothing measurable now: p95 13.4–13.9 ms against 13.8–14.1 ms fully resident, worst frame 33.5 ms. Proxy chunks draw as their own ranges, the texture cache samples its hash, and the periodic point-shadow refresh redraws one lamp at a time.
  - **Still open:** the district saves no memory yet (its buildings are boxes, so proxies cost what the meshes do, and its five facade textures are shared); it needs denser content with per-building detail. The editor does not stream, and there are no octahedral impostors yet.
- [ ] Run combined district rendering, lighting, streaming, and performance checks on the GTX 1650. Record sustained frame times and memory alongside the Playable Street regression results.

The Living City can't be built without this stage.

### R5: High-end tier (RTX-class GPU)

- **Hardware ray tracing.** Shadows, reflections and global illumination.
- **Many lights.** ReSTIR direct lighting for thousands of lights.
- **Path tracing.** A path-traced cinematic and reference mode inside the engine.
- **Upscaling.** DLSS, FSR and XeSS.
- **HDR displays.** HDR10 output (`VK_EXT_hdr_metadata` is already available).

### Later research

- Strand-based hair.
- Advanced eye and skin shading.
- Gaussian-splat capture of real places for backgrounds.
- Neural texture compression.

**Content matters as much as the renderer.** Calibrated lighting, scanned materials, correct scale and a color-managed pipeline account for much of the realism. These belong in the starter templates and the Asset Hub.

## Product roadmap

The tracks run in parallel. Within each track, the order matters.

| Track | Order of work |
| --- | --- |
| **Graphics** (top priority) | R0 → R1 → R2 → R3 → R4 → R5, as above. |
| **AI** | Complete the command registry → MCP server → model-backed Forge AI with permission tiers → AI-generated assets arriving through publish review → runtime NPC AI. |
| **Gameplay** | Luau runtime and typed engine API → behavior graph parity → AI-written scripts verified by play tests. |
| **World** | Playable Street → playable block → populated block with pedestrians and traffic → district streaming → Living City. Follows the Living City ladder LC-00 to LC-05 from the blueprint. |
| **Production** | Headless Build tool → clean-PC install test → audio, UI and effects editors → team collaboration → multiplayer replication. |

## What to avoid

- Rewriting what Blender, Jolt or manifold already do well.
- Keeping two renderers.
- Promising photorealism on the GTX 1650. The goal there is the best image within budget; the high-end tier is a separate target.
- AI that writes files directly instead of using typed commands with review.
- Copying another game's content or look. The Living City stays original.
- Adding features before the existing ones pass their verification.

## First four weeks: graphics first

| Week | Work | Done when |
| --- | --- | --- |
| 1 | Extract the shared Vulkan renderer and render graph. Add the headless golden-image harness for three fixed street views. | Editor and harness use the new library. Images match today's output. |
| 2 | Move the Player to the shared renderer. Add HDR, EV100 exposure, auto exposure and AgX tone mapping. | The Direct3D 11 Player is retired. Street fixture and Player smoke tests pass. Studio and the engine show the same colors. |
| 3 | Add the physical atmosphere, image-based lighting, cascaded shadows and TAA. | First Cycles comparison and realism score recorded. Street holds 60 fps at 1080p on the GTX 1650. |
| 4 | Add clustered lights, GTAO, bloom and a night-time street lighting test. | Realism score improves on the week 3 baseline. All 12 Playable Street checks pass. |
