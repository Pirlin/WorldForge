# WorldForge Editor

WorldForge is a native C++20 and Qt 6 Widgets desktop frontend with 24 destinations, including the Home project manager and World Editor. Each studio has its own navigator, editor surface, inspector, and tools. The studios use demo data and preview controls.

## Run on Windows

Open `dist/WorldForge/bin/WorldForgeEditor.exe` and keep it in its distribution folder so Qt can find its libraries and plugins.

- **Home** manages local demo projects. Select a project, then open World Editor.
- **Scene Outliner** supports searching, selection, adding objects, and deleting them from the context menu. **Details** edits object names, transforms, mesh and material assignments, and LOD settings. Object folder and visibility controls are available under Details settings.
- **Content Browser** filters by folder and search text and sorts by name or type. Selection is shared with the Outliner, Inspector, and other studios. For example, `Hero_Knight` retains the ID `hero-knight` across workspaces and edits.
- **World viewport** uses concept art with selectable demo markers and mock transform controls. The artwork is static. F5 toggles the simulated play preview.
- **Window** restores closed panels and resets the layout. Dock sizes, positions, and the active workspace are saved when the application closes.
- **Studios** remember which panels are open and where they are docked independently. Studio sample controls and code buffers stay available while switching workspaces in the current session. Edits to shared scene objects use the project save and undo history.
- **Studio references** in `Docs` guide the toolbar, hierarchy, inspector, and split-panel layouts. Modeling uses a modifier stack, mesh browser, and UV view. Texture uses synchronized brush controls, editable layer previews, channel thumbnails, and a UV atlas.
- **Ctrl+S** saves the local projects. Scene edits also save on close. **Ctrl+Z** and **Ctrl+Y** undo and redo scene edits within the active project.

On Windows, projects are stored by default in `%APPDATA%\WorldForge\WorldForge Editor\projects.json`.

## Build from source

Use the Visual Studio x64 developer shell with MSVC, CMake 3.21 or later, Ninja, and a matching Qt 6.5 or later kit with Widgets and SVG. The Windows milestone was built with MSVC 2022 and Qt 6.8.3.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64"
cmake --build build
cmake --install build --prefix (Join-Path (Get-Location) 'dist/WorldForge')
```

Set `CMAKE_PREFIX_PATH` to your Qt kit's directory. The Windows install step bundles the Qt runtime and plugins with the executable. [Qt Windows deployment documentation](https://doc.qt.io/qt-6.8/windows-deployment.html)

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

The audit uses isolated sample data and writes a control inventory, pixel metrics, captures, and difference images to `build/editor-audit/qa`. Set `WORLDFORGE_AUDIT_CONTROLS_ONLY` to run the controls without producing images. The included CI workflow runs core tests when hosted on GitHub.

## Source layout

- `App` owns the shell, project manager, menus, workspace switching, and layout persistence.
- `Components` contains the Outliner, Inspector, Content Browser, and viewport.
- `State` owns project records, stable object IDs, selection, JSON persistence, and scene undo history.
- `Workspaces` contains the studio destinations and their native panel and preview widgets. `Data/studios.json` defines each studio's panel labels, properties, and sample content. `DesignSystem` contains the shared Qt theme and icons.
- `Engine/Core` and `Engine/Scene` contain the standalone GUID and scene command contracts. `Tests` contains the core contract tests and editor audit.
