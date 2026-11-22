# WorldForge Editor

WorldForge is a native C++20 and Qt 6 Widgets desktop editor with 24 destinations. World Editor, Modeling, Sculpting, Texture, and Material edit project assets directly. Other studio workspaces include preview controls and sample data.

## Run on Windows

Use the **Modeling**, **Sculpting**, **Texture**, **Material**, **Rigging**, or **Animation** tab to open that workspace inside the editor. The current workspace stays visible until Studio is ready. **World Editor** returns to the level and keeps the Studio document open. **File → Close Authoring Session** closes the Studio document and returns to World Editor. In the Modeling viewport, use **Window → UV Editing**. Open and save editable documents as `.wfasset`. Use **File → Import → Blender Project (.blend)** for an existing Blender project.

Studio ships its Essentials brush and node libraries as `.wflib` binary data in `StudioSource/blender/assets`. Browse the brushes in Sculpting and the node groups in the Asset Browser's Essentials library. The library loader can resolve older built-in `.blend` references to the installed `.wflib` files.

To work on a project asset, use **Content Browser → Add → New Studio Asset**, right-click an existing mesh and choose **Edit in Studio**, or select a mesh and use **File → Edit Selected Mesh in Studio**. In Studio, use **File → Publish to WorldForge** to prepare the active mesh and its editable `.wfasset`, then choose **Apply Studio Update** above the viewport. The update keeps the mesh's existing level placements and can be undone in WorldForge. Reopen the selected mesh in Studio to edit the published source again. The first exchange profile supports static meshes with one UV set and no WorldForge modifier stack.

Open `dist/WorldForge/bin/WorldForgeEditor.exe` and keep it in its distribution folder so Qt can find its libraries and plugins.

- **Create New Project** opens maximized with the eight-card template catalog. Pick a template, then select **Empty Scene**, **Rolling Hills with Trees**, or **Physics Lab** as the actual starter content. **Graphics Quality** controls dynamic sun shadows. **Recent Projects** opens or manages local projects.
- **Scene Outliner** shows saved scene objects and supports searching, visibility, and selection. Its context menu creates directional lights, point lights, player starts, empty objects, or folders. The scene filter can reveal original demo placeholders, which are hidden by default. **World Settings → Remove demo objects** removes those unchanged records as an undoable edit.
- **Content Browser** lists the project's actual mesh assets. Double-click an asset to place another instance. Instances of the same asset share mesh geometry.
- **World Editor** opens a live Vulkan viewport. **Add Cube** creates a mesh and instance. Select a visible mesh with a left click and edit its transform in Details. Right-drag to orbit, middle-drag to pan, scroll to zoom, and press **F** in the viewport to frame the scene.
- **Terrain → Add Rolling Hills with Trees** creates an outdoor scene and frames it near ground level. Sculpt the heightfield and use **Terrain → Paint Material Layer** to paint Grass, Rock, or Soil. Automatic shading blends colors by slope and location. Set the tree count, foliage seed, or wind strength in Details. Rendering streams terrain and foliage in chunks. Physics loads terrain collision tiles near dynamic bodies.
- **Sky & Fog** saves height fog, sun angle, shadow settings, and world gravity. Enable a spherical fog volume by setting its center, radius, and density. The same sun shadow map attenuates both direct surface light and sunlight in the fog.
- **Color, light units and exposure.** The viewport and the Player light the scene in linear color with high dynamic range, then convert it for the display. Lights use real-world units:
  - The **Intensity** of a directional light is illuminance in lux. A clear midday sun is about 100,000 lux and an overcast sky about 10,000.
  - The **Intensity** of a point light is luminous intensity in candela. A **Spot Light** works the same way, with its light in a cone: aim it with its rotation (it points down by default) and set **Cone angle** and **Cone blend** in Details, as in Blender. A scene can hold up to 1024 lights; each pixel shades only the ones whose light reaches it, so a street of lamps stays fast. The eight lamps nearest the camera cast shadows.
  - Projects made before these units keep their look: the old intensity 1 is 123,532 lux.
  - The sky lights the scene. Its horizon and zenith colors and the sun glow are the light that fills shadows and shows in reflections, blurred for rough surfaces as Cycles blurs them, so a dark night sky gives dark shadows. Every surface is lit per pixel with physically based shading, the same in the editor and the Player and the same model as Blender's Principled BSDF in Cycles, so rough ground seen toward a low sun shows the same broad glare.
  - Sun shadows use four cascades fitted to what the camera sees, from about 1 cm per texel nearby to coarser ones far away, and reach up to 250 m. Shadow edges soften with distance from the object that casts them, as the sun's 0.53° disc does: sharp where a box meets the ground, blurred at the far end of a building's shadow.
  - **Sky & Fog → Sky model** is **Gradient** (the horizon and zenith colors, as before) or **Physical atmosphere**. The physical sky scatters sunlight the way air does: blue overhead, white toward the horizon, a glow around the sun, and an orange sun and sky when the sun is low. The sunlight itself passes through the air, so a low sun lights the scene dimmer and warmer. The authored intensity is the sunlight with the sun overhead. Pair a low sun with **Auto** exposure or a lower EV100, as a camera would need. The sun may also set (elevation down to −20°): it stops lighting the scene and the physical sky shows twilight. The Direct3D 11 fallback shows the gradient.
  - **Sky & Fog → Bloom** (on by default) spreads a little light around bright areas, as a camera lens does: a glow around the sun, lamps and bright reflections.
  - **Sky & Fog → Depth of field** blurs what is nearer or farther than the **Focus distance**, as a camera lens does; a smaller **Aperture** f-number blurs more. **Motion blur** smears the image along the camera's movement for the **Shutter**'s share of each frame (0.5 is a 180° film shutter). Both are off by default.
  - **Sky & Fog → Ambient occlusion** (on by default) darkens sky light in corners, creases and openings that the sky can't reach, such as a shop doorway or the ground under an object. It is computed each frame from the depth of the view (ground-truth ambient occlusion, GTAO, within 1.5 m) and averaged by TAA. The same search also adds bounce light: ground beside a sunlit wall picks up its warm light, and a white box lights the ground next to it. It only sees what is on screen. Sunlight and lamps keep their own shadows. The same setting also turns on large-scale sky occlusion. The scene is seen from 16 directions across the sky, and each surface counts only the directions that reach the sky, so the inside of a building seen through its doorway, the space under an awning and a narrow alley are darker, as in Cycles. The maps cover up to 150 m around the scene's center at about a third of a meter per texel, so this occlusion is soft; the screen-space part adds the fine detail. The Direct3D 11 fallback has no ambient occlusion.

  **Sky & Fog → Camera exposure** is either **Manual** or **Auto**:
  - **Manual** takes an exposure value at ISO 100 (EV100): about 15 for a sunny day, 12 overcast, 7 indoors and 4 for a lit street at night. Each step of 1 halves the light the camera takes in.
  - **Auto** meters the view each frame and adapts like an eye: quickly to brightness, more slowly to darkness. It ignores the brightest and darkest tenth of the view. **Exposure compensation** brightens or darkens its choice in stops. The Direct3D 11 fallback uses the manual exposure.

  **View transform** defaults to **AgX**, Blender's default, so assets look the same in Studio and in the game. Bright light rolls off instead of clipping to white; **Standard** shows colors without that roll-off. Color pickers and base-color and emission maps use sRGB, and glTF color factors are converted on import and export. Modeling views use Standard.
  **Sky & Fog → Anti-aliasing** defaults to **TAA** (temporal anti-aliasing). Each frame renders with the camera shifted by a fraction of a pixel, and the frames blend into smooth edges:
  - In the editor, a still view averages 16 shifted frames into an exact supersampled image. Any change starts the average again.
  - In Play, Simulate and the Player, the history follows the camera and is clamped to what each pixel shows now, so revealed surfaces don't smear.
  - Moving objects rely on that clamp, since there are no per-object motion vectors yet.
  - **Off** shows single frames.

- **Details → Physics** assigns static or dynamic box, sphere, or Z-axis capsule collision, with mass, friction, bounce, and initial spin. Select a second collider to connect a fixed, hinge, or distance joint. A hinge accepts an axis and pivot offset. Runtime steps at 120 Hz and supports up to 256 scene bodies plus the player.
- **Simulate** runs physics through the editor camera. **Play** starts a controllable player at a Player Start object from the Outliner context menu, or at the world origin if none is present. Use **WASD** to move and **Space** to jump. **Pause** and **Step** control the clock, and **Esc** or **Stop** discards the runtime copy.
- **File → Export Playable Scene…** writes a `.wfplay` package. Open `dist/WorldForge/bin/WorldForgePlayer.exe` and choose that file, or pass its path on the command line. The Windows Player renders with Vulkan through the same frame pass and shaders as the editor viewport, and needs the Microsoft Visual C++ runtime. On a computer without a Vulkan driver it falls back to Direct3D 11; set `WORLDFORGE_PLAYER_RENDERER=d3d11` to choose that renderer. `WorldForgePlayer --capture <scene> <png> <width> <height> <eye x y z> <target x y z>` renders one view to a PNG file with Vulkan. `--reference` takes the same arguments with a folder in place of the PNG and also writes the frame's geometry, materials, lights and camera for the Cycles realism check. Use **WASD**, **Space**, right-drag, and **Esc** to move, jump, orbit, and exit. Mission names and goals are exported as up to 120 bytes of UTF-8 text, and completion messages as up to 150 bytes. That is 120 Latin letters or about 40 Chinese or Japanese characters. Longer text is shortened at a character boundary.
- **Status bar and Profiler** show Windows CPU and RAM measurements and usage for the viewport's GPU. Discrete adapters report dedicated memory. Integrated adapters report shared memory. Unavailable counters are shown as a dash.
- **Window → Output Log** shows editor activity, renderer messages, and runtime events. **Message Log** collects warnings and errors. **Terminal** opens the local Command Prompt with output, history, and a stop control.
- **Content Browser → Import** accepts static GLB 2.0 meshes with triangle geometry, node transforms, normals, the first UV set, and embedded PNG/JPEG maps for base color, normal, metallic/roughness, occlusion, and emission. Modeling's Material Preview uses these maps. This import path targets uncompressed, static meshes.
- **Modeling** opens the selected mesh for component editing. Use **Edit** to show **Vertex**, **Edge**, and **Face**. Click to select, drag a box, use Shift to extend, Ctrl to toggle, or Alt to subtract. **X-Ray** lets box selection reach hidden components. Box selection uses the projected vertex, edge midpoint, or face center.
- **Move**, **Rotate**, and **Scale** preview a component transform. Drag in the viewport or enter XYZ values. Choose an axis, orientation and pivot in the tool bar. For Move, choose grid or vertex snapping during a drag. Apply or press Enter to commit one undoable edit, or press Escape to cancel. Use **G**, **R**, or **S** from the focused viewport. **Ortho** toggles projection, and **1**, **3**, and **7** align front, right, and top views.
- **Face tools** include **Extrude Face**, **Bevel Perimeter**, and a **Loop Cut** for connected quads. **Bevel Edges** supports adjoining selections on closed corners with convex planar faces. **Voxel Remesh** builds a new surface from a closed mesh and previews it before applying. **Mirror** accepts either side of a crossing mesh. **Add Instance** shares the mesh asset, so applied edits affect both instances. **Reimport Source** prompts before replacing local geometry edits.
- **Primitives**, the Outliner context menu, and **Select** provide sized primitives, independent or linked copies, Make Unique, join, separate, origin tools, and component selection by adjacency. **Mesh** and **Face Operations** add region extrusion, inset, bevel, slide, weld, fill, bridge, dissolve, and subdivision. **Knife** previews connected cuts across faces. Click each crossing edge and press **Enter** to apply. **Retopology** places quads on a reference surface and extends boundary edges.
- **UV Tools** marks seams, unwraps and packs islands, reports overlap, and moves, rotates, or scales the active island. **Pin UV Corners** anchors UV coordinates during unwrap and packing. **UV Sets** duplicates and switches named layouts with their pins. **Modifiers** evaluates an ordered stack with a separate editable base mesh. **Shrinkwrap** and **Boolean** use evaluated targets in mesh-local coordinates. Boolean supports Difference, Union and Intersection on closed solids with an explicit cutter offset. **Export GLB** writes the evaluated static mesh with the active UV set, normals, and material factors.

In **Sculpting**, select a mesh or add a sculpt sphere, choose Draw, Smooth, Inflate, or Flatten, and drag on its surface. Radius is in local mesh units. Shift switches to Smooth and Ctrl inverts Draw and Inflate. Escape discards the stroke and each committed stroke is one undo entry. Sculpting edits the shared base mesh, so linked instances update together.

In **Modeling**, selecting vertices, edges, or faces shows the active transform gizmo. Move is the default. Drag an axis handle to commit on release, or use the numeric controls for precision. Switching tools or component modes commits a valid pending edit. Escape discards it. The viewport has direct Wireframe (4), Solid (5), Material Preview (6), and Shaded + Wire buttons. Use **Material Preview Settings** in Details to assign a PNG or JPEG and edit the metallic and roughness of a material slot. The preview uses the active UVs with repeat wrapping and studio lighting. Image data is saved with the project and included in evaluated GLB export.

**Spin** previews a sweep of selected boundary edges around a local axis and pivot. Partial sweeps attach to the boundary. Full turns weld the swept seam and keep the source faces separate. **Measure** reports distance between two mesh surface clicks or the angle across three clicks, including object scale. **Annotate** draws strokes on the mesh and saves them with the object. Click **Erase Annotation** in Details to remove a stroke or **Clear Annotations** to remove them all. Both support Undo.

On the Modeling tool shelf, **Target Weld** drags one vertex or boundary edge onto another. **Connect** previews a cut between two vertices or edge midpoints on a shared face. **Loop Cut** previews a cut through a quad strip from a clicked edge, with segment count and slide controls. **Knife / Multi-Cut** can turn across one face. In the Mesh menu, **Retopology → Draw Quad Strip** creates a patch and keeps the new boundary edge ready for the next quad. Press Enter to apply each preview or Escape to cancel it.

**Revolve** creates a new polygon mesh from ordered radius and height points. The dialog previews the profile, with straight segments or rounded corners. Choose an axis, sweep angle and step count. Put an endpoint on the axis to close that end, and use 360° for a welded seam.

Choose **NURBS** in Revolve to edit rational spline control points, weights, degree, and sampling. Reopen the saved generator through **Mesh Data → Edit Revolve Profile**. A direct geometry edit converts the generated result to regular polygon geometry.

Use **Multi-Object → Capture Selected Components** on each mesh, then **Transform Captured Components** to apply a shared world-space move, rotation, or scale. Undo restores all affected meshes together.

The **Model Outliner** displays meshes under saved scene groups and exposes World Editor visibility. Right-click a mesh to move it to a named group. In **Details → Materials and Textures**, choose a slot, create a slot, assign selected faces, select a slot's faces, or edit the slot's base color and maps.

Open **Texture** to inspect a map with the selected slot's UV outline and a live material viewport. Scroll to zoom, middle-drag to pan, and use **Fit Image** to reset the view. **All Slots** shows the entire mesh's UV layout. Choose a channel to see its thumbnail, import a PNG or JPEG, or remove the map. In **Material**, **Link Existing Material** can replace a slot or add a new one. Use **Assign Slot to All Faces** for a whole mesh, or assign selected faces in Modeling Details. Edits to a linked material update both assets and undo together. **Make Material Independent** lets you edit one slot separately.

Drag a mesh onto another mesh in the Model Outliner or use **Set Parent**. Parenting and **Unparent** preserve world placement. In Object mode, use **Move**, **Rotate**, and **Scale** on a parent to transform its assembly. Details edits local transform channels and shows the resolved world transform for reference. Deleting a parent detaches its direct children in place and preserves their descendants.

Use **UV Set** to switch among the mesh's saved layouts. **Inspect UVs** reports islands, overlaps, degenerate faces, and faces outside the active UV tile. **Export UV Layout** writes a transparent 2048 × 2048 PNG of that tile. Use **All Slots** to include the whole mesh. **Export Map** saves the selected channel's original PNG or JPEG.

In **Texture**, select **Base Color** and enable **Paint**. Choose a color, diameter in texture pixels, and opacity, then drag on the image. The 3D viewport previews the stroke while you paint. Release to save one undoable stroke or press **Esc** to cancel. The first stroke on an empty slot starts with a white 1024 × 1024 image. Painting keeps the dimensions of an imported map and saves it as PNG.
- **Mesh Data** inspects topology and assigns material slots. **Generate LOD** creates one reduced mesh from a cell size and switch distance. Preview it in Modeling Object mode. World Editor selects the level by camera distance, and the Windows Player selects it by distance from the avatar. **Collision Proxy** configures local bounds for box, sphere, and capsule bodies.
- **Window** restores closed panels and resets the layout. Dock sizes, positions, and the active workspace are saved when the application closes.
- **Studios** remember which panels are open and where they are docked independently. Studio sample controls and code buffers stay available while switching workspaces in the current session. Edits to shared scene objects use the project save and undo history.
- **Studio references** in `Docs` capture the intended workspace layouts and tool sets.
- **Ctrl+S** saves the local projects. Scene edits also save on close. **Ctrl+Z** and **Ctrl+Y** undo and redo scene edits within the active project.

On Windows, the editor writes the recent-project index to `%APPDATA%\WorldForge\WorldForge Editor\projects.wfproject`. Projects created in the launcher store their versioned scenes and mesh data in `Scenes` under the chosen project folder. That folder also serves as the default export and terminal location. Older projects continue to use their profile storage. The editor checks each scene against its manifest and can open the prior complete save when the latest generation is damaged.

## Build from source

Use the Visual Studio x64 developer shell with MSVC, CMake 3.21 or later, Ninja, and a matching Qt 6.5 or later kit with Widgets and SVG. The Windows milestone was built with MSVC 2022 and Qt 6.8.3.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64"
cmake --build build
cmake --install build --prefix (Join-Path (Get-Location) 'dist/WorldForge')
```

Set `CMAKE_PREFIX_PATH` to your Qt kit's directory. The Windows install step bundles the Qt runtime and plugins with the executable. [Qt Windows deployment documentation](https://doc.qt.io/qt-6.8/windows-deployment.html)

To ship the embedded Studio workspaces with the Windows editor, also set `WORLDFORGE_STUDIO_RUNTIME` to the installed runtime directory that contains `WorldForgeStudio.exe` and its versioned data folder. The install step bundles it under `bin/Studio`. Use [Build-WorldForgeStudio.ps1](Tests/Build-WorldForgeStudio.ps1) to build the fork in `StudioSource/blender`.

The live viewport requires Vulkan headers and `glslang` or `glslangValidator` at build time. CMake finds them in the Vulkan SDK or through `Vulkan_INCLUDE_DIR` and `WORLDFORGE_GLSLANG`. On this PC the build also detects the project tooling under `%LOCALAPPDATA%\WorldForge\Vulkan`. The compiled shaders are embedded in the editor. Running the viewport requires a Vulkan-capable graphics driver.

CMake uses Git to fetch the pinned Jolt Physics 5.3.0 commit on the first configure. The Windows build also produces `WorldForgePlayer.exe`. [Jolt Physics source](https://github.com/jrouwe/JoltPhysics/tree/v5.3.0)

## First Light development

The scene core builds independently of the editor. From a developer shell with CMake and Ninja on `PATH`:

```powershell
cmake --preset core
cmake --build --preset core
ctest --preset core
```

To reproduce the editor audit, put the Qt kit's `bin` directory on `PATH` and configure the audit with the matching kit:

```powershell
cmake --preset editor-audit "-DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64"
cmake --build --preset editor-audit
ctest --preset editor-audit
```

The editor audit uses isolated sample data and writes a control inventory, pixel metrics, captures, and difference images to `build/editor-audit/qa`. Set `WORLDFORGE_AUDIT_CONTROLS_ONLY` to run the controls without producing images. The native Vulkan authoring test saves launcher and world captures plus an outdoor scene export to `build/editor-audit/authoring-qa`. The Windows Player tests read back a rendered frame and check movement and jumping. The Windows core-only preset also exercises the Player with Direct3D's WARP software device, without installing or building the Qt editor.

To check rendering, run `WorldForgeEditorAudit <output> <repo> --rendering`. It renders three fixed views of the Playable Street in the editor and in the Player, compares each with the images in `Tests/Golden`, and checks that the editor and the Player show each view alike (mean difference under one 8-bit level). The comparison runs only on the GPU that recorded those images; on another GPU the checks are skipped. After an intended visual change, set `WORLDFORGE_UPDATE_GOLDEN=1` to record new images. `Tests/FitAgx.py` and `Tests/GenerateAgxReference.py` measure Studio's AgX transform: the first derives the engine's AgX data in `Engine/Render`, the second produces the reference values the core tests check against.

To measure realism, run `WorldForgeEditorAudit <output> <repo> --realism` with WorldForge Studio installed. For three street views it renders the Player's image and a Cycles path-traced reference of the same frame (`Tests/CyclesReference.py`), both without fog and under the physical sky, which the Player bakes for Cycles as `sky.hdr`. It then scores each pair with LDR-FLIP, where 0 means identical, and saves the engine image, the Cycles image and the FLIP error map side by side as `realism-<view>.png`. The audit fails if a view scores more than 0.01 worse than `Tests/Golden/realism.json`. After an intended change, record new scores with `WORLDFORGE_UPDATE_GOLDEN=1`; `WORLDFORGE_REALISM_SAMPLES` sets the Cycles sample count (default 256). A run takes about two minutes on the CPU.

## Source layout

- `App` owns the shell, project manager, menus, workspace switching, and layout persistence.
- `Components` contains the Outliner, Inspector, Content Browser, and viewport.
- `State` owns project records, stable object IDs, selection, JSON persistence, and scene undo history.
- `Workspaces` contains the studio destinations and their native panel and preview widgets. `Data/studios.json` defines each studio's panel labels, properties, and sample content. `DesignSystem` contains the shared Qt theme and icons.
- `Engine/Core` and `Engine/Scene` contain the standalone GUID and scene command contracts. `Tests` contains the core contract tests and editor audit.

Continuing development: see [Docs/WorldForge-Handoff.md](Docs/WorldForge-Handoff.md).
