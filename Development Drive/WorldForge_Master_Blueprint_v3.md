# WorldForge Engine & Studio

## Complete Product, AI-Native Engine, AAA Open-World & Engineering Development Specification

**Revision:** 3.0 (proposed architecture baseline)  
**Date:** 10 November 2026  
**Target systems:** Windows, macOS and Linux desktop editor; independent game runtime and build tools  
**Primary audience:** founder; engineering, product, design, technical art, AI/ML, test, security and production teams  
**Classification:** development planning and requirements, **not evidence of implemented functionality**  
**Brand status:** WorldForge is a working codename pending trademark/domain clearance  
**Status owner:** founder / technical lead (to be appointed)

> **IMPORTANT SCOPE STATEMENT.** WorldForge is a proposed independent native game engine and a comprehensive creation suite. Its ambition is to support photorealistic, GTA-scale and ultimately more advanced original open-world games, and to allow AI to plan, create, edit, test and optimize work across the whole production pipeline. That is a research and product target, **not** a present fact or a guarantee that a creator can effortlessly reproduce an existing proprietary game. Existing screenshots are design concepts. The first real engineering gate is a Qt/C++ editor with a Vulkan viewport and a scene that can be changed, saved, reopened and launched independently.

---

## Document control and how to use this specification

The document is intended as a durable handoff to new engineers and other AI assistants. Each statement belongs to one of four categories: **DECIDED** (architecture direction agreed in this discussion), **PROPOSED** (candidate implementation), **EXPERIMENTAL** (benchmark or research hypothesis), and **FUTURE** (long-term capability). A design mockup is not an implemented feature. An acceptance test is an agreed verification target until an actual build and test report exists.

**Authority order for conflicts:** (1) approved product goals and intellectual-property/security constraints; (2) this version's architecture decision records; (3) stable command and file schemas; (4) individual screen requirements; (5) visual mockups. Higher-level requirements may be revised only by a recorded architecture decision. This document does **not** establish final API binary compatibility, licensing entitlement, hardware minimums, or external platform approval.

### Executive product definition

WorldForge comprises six products sharing one technology core:

1. **WorldForge Studio:** native creator application; professional authoring, inspection, debugging, AI assistance and release tools.
2. **WorldForge Engine:** independent C++ runtime; scene, rendering, assets, physics, motion, audio, gameplay and networking modules.
3. **WorldForge Player:** minimal packaged executable/game runtime that has **no Qt editor dependency**.
4. **WorldForge Build:** headless importing, asset cooking, validation, compilation, packaging and repeatable release orchestration.
5. **Forge Intelligence:** project-aware optional AI, retrieval, multimodal generation and supervised editing through typed engine tools.
6. **Forge Services:** optional cloud inference, collaboration, distributed build farm, marketplace and account infrastructure; local editing remains usable without sign-in.

The product should combine the **breadth of an end-to-end game-production workflow** with the **depth of professional editable tools**. It aims to outperform competitors on *measurable* task outcomes, not feature-count advertising. No company currently owns the design claim or an advantage without implemented benchmarks.

### End-state user outcomes

- A game creator can produce a complete original 3D game in one coherent project: concept, world, mesh, sculpt, texture, materials, character, rig, animation, code, AI/NPC, VFX, physics, audio, cinematics, UI, simulation, testing, optimization and distributable build.
- A team can collaboratively work on **large, streamable worlds** with stable entity/asset IDs, versioned source data, reproducible builds and managed permissions.
- Forge Intelligence can retrieve *correct* context, plan a multi-studio task, modify real engine state through permissioned commands, preserve editability, run verification, produce an audit trail and escalate failures for human review.
- WorldForge can eventually support **AAA-scale original open worlds** with sophisticated vehicles, populated districts, weather, cinematics and reactive gameplay, with explicitly specified performance and quality targets.
- Developers retain manual-first workflows even when AI features are disabled or unavailable.

### Non-goals and guardrails

WorldForge will not start by (a) recreating all Unreal/Unity/Blender/Substance/Premiere tools from scratch, (b) training a proprietary foundation model, (c) claiming 4K photorealism on arbitrary hardware, (d) promising identical copies of Grand Theft Auto VI, its game data or Rockstar intellectual property, or (e) requiring cloud AI for local editing. Outsourced/third-party assets and libraries require explicit licensing review. A screen named after a concept does not make the corresponding technology ready.

---

# PART I - Product strategy and user experience

## 1. Product positioning and competitive thesis

**Category:** independent game engine + professional digital content creation studio + AI production operating layer.  
**First target creator:** solo technical artists and small PC game studios needing a coherent 3D creation-to-export workflow.  
**Later targets:** mid-market teams, AAA departments, film/virtual production, architecture/simulation and academic users.  
**North-star metric:** *time and defect rate from a user goal to a verified, editable, shippable game feature*, measured on identical hardware and comparable tasks.

### 1.1 Four possible sources of durable differentiation

**Unified dependency-aware authorship (WorldGraph).** Editing a mesh can signal the exact materials, rigs, collisions, instances and tests needing revalidation. The graph is a stable system, not an LLM-generated list.

**Production-grade AI execution (Forge Intelligence).** AI is not just a prompt window or one-shot generated image. It operates through the same validated command bus available to human editors and captures exact before/after effects, provenance and test evidence.

**Performance-aware authoring.** The engine exposes concrete constraints during modeling, texture generation, rigging, effect creation and world building: triangle budgets, material variants, resident asset budgets, frame timing and platform capability tiers.

**High-quality rendering and simulation.** Develop a measurable series of rendering/simulation innovations. Avoid declaring victory over Unreal or other engines before controlled, independent benchmarks support a narrow claim.

### 1.2 Product success measures

| Measure | Baseline / methodology | Future acceptance principle |
|:--|:--|:--|
| Creation throughput | Timed user test with the same assets and quality criteria | Faster complete workflows without higher defect rate |
| Scene and asset correctness | Referential integrity, import/reimport, undo/redo and build tests | No missing/incorrect dependency references |
| Rendering quality | Paired blinded scene reviews and documented settings | Better or comparable visual quality at measured frame budget |
| Runtime performance | Frame-time trace (CPU/GPU), 1% low, VRAM and loading | Explicit targets met on named hardware profiles |
| AI tool reliability | Valid proposed commands, approvals, tests, postconditions | High completion rate with rollback and low unsafe-action rate |
| Open-world scale | Streaming hitch rate, load times, entity residency and traversal | Seamless traversal within defined budgets |
| Quality and stability | Crash-free sessions, recovery, migrations, regression tests | No critical data-loss or release blockers |
| Team adoption | Independent published projects, retention, creator interviews | Real creators repeatedly finish work using the engine |

### 1.3 Value proposition for a GTA-scale reference target

The flagship benchmark is an **original, fictional 'Living City' project**, not a GTA clone or reproduction. It tests the technical difficulty of a detailed city, enterable interior, pedestrian systems, traffic, vehicles, police-like response systems, missions, dialogue, climate, streaming and cinematic quality. WorldForge should reduce *production friction* and automate repetitive work; human teams remain responsible for creative direction, licensed content, final polish, nuanced game design and verification.

## 2. Product architecture and navigation rules

**There are 40 functional screen specifications, not 40 permanent navigation buttons.** The native desktop shell should use an icon-only global rail (roughly 44-56 logical pixels), a compact top menu/action bar, a small set of configurable pinned workspace tabs, a searchable More/command palette, dockable contextual Outliner/Inspector/Content panels and a **single horizontal status strip at the bottom**. The central editing surface should dominate, not a list of categories. Status indicators are measured values, not decorative always-green indicators.

Qt 6 Widgets provides a native app shell with `QMainWindow`, `QDockWidget`, toolbars and persisted layouts. Each specialized editor may use custom Qt painting and a Vulkan surface. Its docking is not a substitute for project data synchronization; all panels subscribe to core selection and asset-change events.

### 2.1 Twelve navigation groups

| Group | Included screens | Placement principle |
|:--|:--|:--|
| Project & setup | WF-01, WF-38, WF-40 | Hub, settings dialog and project wizard |
| World & rendering | WF-02, WF-29, WF-30, WF-31, WF-35 | World modes, inspectors and scene tabs |
| Assets & geometry | WF-03, WF-04, WF-05, WF-06, WF-24, WF-28, WF-36 | Asset editors in the Create workspace |
| Characters & motion | WF-07, WF-08, WF-09 | Shared character/rig/animation authoring context |
| VFX & simulation | WF-10, WF-19, WF-34 | Effects and simulation toolset |
| Audio & cinematics | WF-11, WF-12, WF-33 | Media workspace; spatial audio is a mode |
| Gameplay & intelligence | WF-13, WF-14, WF-15, WF-32 | Code, graph, NPC behavior and creator AI |
| Multiplayer | WF-16 | Gameplay tools; advanced server mode |
| UI, playtest & quality | WF-17, WF-20, WF-21, WF-22, WF-23, WF-39 | Design, Play/Analyze, bottom console |
| Build & distribution | WF-18, WF-27 | Build and release separate stages |
| Team & revisioning | WF-25, WF-26 | Source control rail, Team workspace |
| Tools & extensions | WF-37 | Tools menu and package management |

### 2.2 Shell contract

**Top:** app and project identity, File/Edit/View/Assets/Tools/Build/Window/Help, active document tabs, Save/Undo/Redo/Play/Simulate/Build, target profile, current branch and AI activity indicator.  
**Center:** main editor viewport / waveform / sequencer / node graph / code / report.  
**Context:** Outliner, toolbox or library left; Inspector right; asset browser, timeline, console or profile dock below (hidden when not needed).  
**Bottom single line:** project, save/dirty state, current scene, active selection, background tasks, revision state, build target, measured FPS, CPU/GPU frame times, RAM/VRAM (only when available), warnings.  
**Multi-monitor:** tabs can detach, with documented re-docking/restoration.  
**High-DPI:** Qt logical coordinates separate from Vulkan framebuffer pixels; verify scaling at 100/125/150/200% and mixed-DPI displays.

### 2.3 Shared interaction invariants

Every workspace must use the same command names and ID types for Create, Duplicate, Delete, Rename, Select, Focus, Undo/Redo, Save, Import, Reimport, Validate and Inspect Dependency. Persistent edits are transactional, serialized or explicitly marked non-persistent, and reflected in all open editors. Inspector changes must not mutate engine memory behind the command/audit layer. Dialogs include cancel, validation feedback and clear destructive-action confirmation. Search, keyboard navigation, focus visibility, tooltips, accessible labels and localization hooks are required across the shell.

### 2.4 Screen states, accessibility and error behavior

All screens implement loading, empty, ready, focused, multi-select, dirty, saving, validation error, permissions denied, missing asset, offline, low disk space, AI unavailable and device-lost states where relevant. Use progress and cancellation for expensive imports, cooking, baking and model inference. A GPU/AI/import crash should not erase user edits. On macOS obey app menu conventions; on Linux test multiple window systems and input methods. Offer manual alternatives when AI operations cannot run.



# PART II - Technology, code architecture and contracts

## 3. Language policy, libraries and engineering environment

### 3.1 Minimal final language policy

**DECIDED core:** **C++20** for editor integration, runtime, renderer, core tooling and public native SDK. **Shader development:** **GLSL -> SPIR-V** for the initial Vulkan path. **FUTURE scripting:** **C#** (only after core reflection/serialization/embedding and gameplay APIs are stable). **OPTIONAL tools/services:** Python for Blender automation, evaluation notebooks and model-serving adapters; not a mandatory engine runtime dependency. SQL is a query/storage language for metadata, not a second gameplay language. TypeScript is optional for a later marketplace/web portal and is not required to build the native desktop application.

There is no architectural benefit to requiring all creator tool developers to know all of C++, Python, C#, TypeScript and Rust. The C++ plugin boundary and stable command contracts should make extensions possible without fragmenting the engine. **Visual scripting** is an engine-authoring feature that compiles to a validated graph representation; it is not a separate engine implementation language.

### 3.2 Libraries and adoption gates

| Subsystem | Preferred candidate | Decision / constraint | Earliest stage |
|:--|:--|:--|:--|
| UI and dock management | Qt 6 Widgets | Use QMainWindow/QDockWidget; audit LGPL/commercial terms and each module | F0 |
| GPU backend | Vulkan SDK | RHI boundary, validation layers, device feature negotiation | F0 |
| Vulkan memory | Vulkan Memory Allocator (VMA) | Device allocator only; engine resource lifetime still owned by WorldForge | F0 |
| Core math | GLM | Explicit coordinate/handedness/unit conventions and serialized precision | F0 |
| ECS | EnTT | Runtime components; authoring hierarchy separate from storage layout | F0 |
| Project metadata | nlohmann/json + SQLite | JSON interchange, SQLite transactions/indexes; schema versioning | F0 |
| Logs/profiling | spdlog or equivalent; Tracy | Pinned dependencies; distinguish engine logging from editor UI | F0/F1 |
| Source model loading | fastgltf | Strictly validate glTF 2.0; preserve editable original files | F1 |
| Image IO | stb_image or platform-appropriate libraries | Security review on untrusted decoders, color management required | F1 |
| Physics | Jolt Physics | Native C++; multithreading, deterministic limitations documented | F2 |
| Audio | miniaudio | Playback/mixing starting point, not full DAW/HRTF solution | F2 |
| Unit/integration tests | Catch2 or GoogleTest + CTest | Unit, serialization, interface and renderer smoke tests | F0 |
| Dependency management | vcpkg manifest + CMake/Ninja | Pin baseline; allow approved internal registries | F0 |
| Version control | Git + Git LFS | Source/text files, binaries; later evaluate per-asset locking | F0 |
| Image/model inference | ONNX Runtime, provider adapters | Optional capability negotiation; model/asset license check | F2 |
| Search/retrieval | SQLite FTS5; embedding adapter | Local-first keyword/metadata; add vector index only when justified | F2 |
| Team vector service | Qdrant or alternative | Evaluate hybrid search, operational cost, exportability | F3 |
| Advanced scene composition | OpenUSD (research) | For layers/references/variants; do not replace engine format blindly | F3 |
| GPU debugging | RenderDoc + Vulkan validation | Compatibility/driver checks | F0 |
| Compiler/build debug | MSVC, Clang, GCC; sanitizers | Reproducible configs and symbolized crash dumps | F0 |

The library list is not an assumption of license compatibility. Maintain an SPDX-style dependency inventory, module-level license review, contributor/IP policy, source-asset rights and redistribution notices. At implementation time, pin current supported versions after reproducible tests rather than copying release numbers from a planning document.

### 3.3 Build workstation and software

**Windows:** Visual Studio Community/Build Tools with C++ toolchain, Windows SDK, Qt 6, Vulkan SDK, CMake, Ninja, Python only for optional tooling, Git/Git LFS, debugger and RenderDoc.  
**macOS:** supported Xcode + macOS SDK, Apple Clang, Qt, CMake/Ninja, Apple GPU testing. The Vulkan/Metal strategy is **unresolved until prototype qualification**; a portability layer may provide an initial route, but an RHI contract must allow native Metal if required.  
**Linux:** Clang/GCC, Qt 6, Vulkan loader/SDK, CMake/Ninja, GPU drivers, Wayland/X11 testing, AppImage/Flatpak or distro packages selected later.

Suggested development computer: modern multi-core CPU; **16 GB RAM is a practical lower prototype floor, 32 GB preferred**; current Vulkan-capable graphics processor, SSD and substantial free disk. These are developer workstation recommendations, **not finished-game minimum requirements**. AAA realism requires multiple tiered GPU test machines.

### 3.4 Software engineering policy

Use a monorepo initially. C++ formatting with `clang-format`, static analysis (`clang-tidy`), address/undefined sanitizers where supported, strict compiler warnings, dependency scans, fuzz tests for untrusted assets, signed releases, CI builds and reproducible version manifests. Pull requests require tests, docs changes for public API changes and a design/ADR reference. Avoid adding a second UI framework while Qt remains adequate. Separate public stable APIs from internal experimental renderer/AI modules.

## 4. Process architecture and dependency rules

```text
+-------------------------- WorldForge Studio -------------------------+
| Qt native shell / docked workspaces / command palette / AI approval |
| Editor adapters: Outliner, Inspector, asset browser, 3D viewport     |
+----------------------------- | ------------------------------------+
                              v
                   Editor Application Services
        Project | Document | Command/Undo | Selection | Asset | AI Gateway
                              |
         +--------------------+--------------------+
         v                    v                    v
   EngineCore              Content Pipeline      Task/Evaluation
   IDs/ECS/scene           import/cook/cache      diagnostics/QA
   events/jobs             build/package          AI plan approval
         |
     +---+------------+-------------+----------+-----------+
     v                v             v          v           v
   Renderer         Physics       Animation    Audio     Gameplay/Net
   RHI/Vulkan       Jolt adapter  skeletons    mixers    scripting
     |                                                        |
     +------------------ WorldForgePlayer ----------------------+
                              |
                         Packaged Game

   WorldForgeBuild: headless reuse of EngineCore + import/cook/validation.
   Forge AI workers: isolated process/local/cloud -> typed Editor command API.
```

### 4.1 Three primary executables plus optional workers

- **WorldForgeEditor:** authoring application, Qt dependency permitted; editor-only assets, timeline widgets, source code and project databases.
- **WorldForgePlayer:** standalone game runtime; loads cooked content without Qt, Python, remote service or any Studio binary. Game AI requiring optional cloud services must degrade gracefully per game configuration.
- **WorldForgeBuild:** CLI executable for deterministic import/cook/compile/package, able to run in CI without the GUI.
- **ForgeAIBroker / isolated AI worker:** optional separate process with a restricted filesystem/API boundary, timeout, cancellation, logging, budgets, explicit network policy and user approval events. Never load arbitrary model-supplied C++ code into the editor process to execute it.

### 4.2 Module ownership and dependency direction

`EngineCore` owns fundamental ID and handle types, ECS/document model, events, jobs, file I/O and errors; it must not import Qt or model provider SDKs. `EditorServices` owns editing documents, commands, transactions, selection and source asset lifecycle. `Renderer` exposes `IRenderDevice`/`IRenderGraph` contracts and owns GPU memory/synchronization. `Physics`, `Animation`, `Audio`, `AI Runtime` and `Networking` use engine/public component APIs rather than touching Qt view models. `Studio` uses adapters to project data; the Inspector must never own authoritative component values. `BuildTools` uses shared importers and versioned cooker recipes.

### 4.3 Threading and lifetime rules

UI thread handles Qt events and view-model updates; render thread owns Vulkan queue/device operations; job workers handle bounded CPU imports and cooking; IO tasks return cancellation-aware futures; model inference runs in a worker/service. Cross-thread messages are immutable or synchronized; no UI callback should retain a raw pointer to a deleted entity. Entity and asset handles use ID+generation or equivalent stale-handle detection. Apply edits at a safe scene/document boundary, not while physics/render loops mutate the same storage. Record clocks/timestamps used for diagnostics in monotonic time where relevant.

## 5. WorldGraph: canonical document and asset data model

### 5.1 Primary resource types

| Entity/resource | Minimum canonical fields |
|:--|:--|
| ProjectManifest | project GUID, schema version, engine compatibility, primary scene, units, coordinate system, target platforms, plugins |
| SceneDocument | GUID, revision, child entities, data layers, world settings, dependency references |
| Entity | GUID, generation, parent GUID, name, component set, authoring layer, optional prefab link |
| Component | stable type ID, schema version, typed fields, validation rules, serialization rules |
| Asset | GUID, type, editable source path, import settings, derivatives, content hash, license/provenance, dependency edges |
| DocumentRevision | revision ID, parent revision, author, timestamp, applicable schema, change summary |
| AIArtifact | prompt/goal reference, model/provider version, input IDs+revisions, generation settings, rights metadata, output hashes, evaluation result |
| BuildArtifact | target platform, profile, project revision, cooked manifest hash, build toolchain, license report and signatures |
| TestResult | test ID, scene/revision, hardware, trace/log references, actual assertions, result and confidence level |

### 5.2 Identity, reference and dependency invariants

**Never use display names or mutable paths as canonical identity.** Store stable GUIDs with type validation; record transitive dependencies and reverse references. Each importing operation can create multiple cooked derivatives while preserving the original authoring file. A change to the original should invalidate only affected derivatives/tests (incremental build), not blindly rebuild all content. Missing dependencies should surface as explicit errors with repair choices. Deleting a referenced asset must present its consumer list and require a safe replacement or explicit broken-reference approval.

### 5.3 Document command contract

Every persistent editor action has: `command_type`, `command_schema_version`, `command_id`, `project_id`, `target_ids`, `expected_revision`, `author_id`, validated parameters, capability/permission scope, `preview()`, `apply()`, effect summary, inverse/compensating operation or explanation why not reversible, and deterministic failure semantics where practical. Commands are grouped in transactions for complex actions such as AI-generated scene creation. Logs must capture an audit record but must not silently duplicate sensitive asset content into remote telemetry.

```json
{
  "schema_version": 1,
  "command_id": "cmd-uuid",
  "project_id": "prj-uuid",
  "expected_revision": 42,
  "operation": "entity.transform.set",
  "target_entity_id": "ent-uuid",
  "args": {"position_m": [5.0, 0.0, 1.0]},
  "mode": "preview",
  "requested_by": "user-or-approved-agent-id"
}
```

**Concurrency rule:** mismatched `expected_revision` yields a conflict for review, not a forced overwrite. A multi-operation AI plan is staged against an immutable snapshot, validated, previewed, then committed as one transaction where feasible. Long-running bake/import/build jobs record partial outputs in temporary directories, promote artifacts atomically and are cancelable or resumable as documented.

### 5.4 Project file formats and storage

`*.wfproject` is a versioned project manifest; `*.wfscene` stores authoring scene structure; `*.wfasset` stores asset metadata/recipe; large geometry/textures/timelines are binary or interchange sidecars with content hashes. JSON is acceptable for early authoring metadata, but runtime uses cooked, platform-specific binary packs. SQLite can hold searchable indices, transactions, FTS and local job/event metadata; avoid putting the only copy of huge binary source assets in SQLite. Define migration tests before changing schemas. Export glTF/GLB for interchange and preserve full-fidelity source; consider USD for layered scene composition later, with a compatibility/translation plan rather than assuming one format solves every problem.

### 5.5 Asset processing lifecycle

`source -> validation -> semantic/metadata extraction -> import -> editable document/derivative -> dependency graph -> cook/LOD/texture formats -> platform package -> runtime load`. Each stage has IDs, versions, hashes, errors, statistics, cancellation and logs. Use a quarantine process for untrusted importers and generated scripts. Record authoring-to-cooked correlation so an optimization report can point back to a source material or mesh.

### 5.6 Collaboration and versioning

Git+LFS is the initial code/asset history, with review and conflict guidance. Large studios may later need asset locks or a Perforce-style centralized workflow. **Editor undo is not Git revert;** the two remain separate but auditably linked. Online collaboration requires an authorization layer, revision conflicts, offline reconciliation and per-asset owner semantics; real-time multiuser co-editing is future work, not a simple button.



# PART III - Forge Intelligence: AI-native production operating layer

## 6. AI system charter

The project-wide AI architecture is a **first-class engine/editor subsystem**, not an independent chatbot pasted onto every screen. It must (1) know the exact project state through WorldGraph and safe queries, (2) retrieve contextual information by both lexical and semantic methods, (3) plan work across editors, (4) invoke versioned editor/build/test tools, (5) create ordinary editable assets rather than opaque blobs, (6) check results using actual engine tools, (7) allow cancellation, preview, undo and audit, and (8) remain optional so professional creators retain normal manual workflows. The AI is **not an authoritative source of asset identity or correctness**; the engine's validated project state is.

### 6.1 Separate three kinds of AI

**Creator AI:** development-time code, mesh, materials, VFX, dialogue, content, testing, QA and build assistance. This operates in the editor and has strong tool permissions only with approval.  
**Runtime NPC AI:** in-game navigation, perception, utility/behavior trees, planning, simulation and optional bounded language-driven dialogue. It must respect strict frame budgets and deterministic gameplay constraints.  
**Engine intelligence:** internal non-creative optimization such as asset similarity, anomaly detection, intelligent cache/streaming hints and automatic QA prioritization. None of these categories shares implicit privileges with another.

### 6.2 Six cooperating services

| Service | Owns | Must never assume |
|:--|:--|:--|
| Forge Director | Goal decomposition, scoped plan, task DAG, model/tool routing | Unlimited permission or unlimited context |
| Forge Memory | Document indexing, embeddings, search, prompt context, provenance | Embedding similarity proves identity |
| WorldGraph API | Exact entities/assets/dependencies, revisions, impact analysis | LLM can overwrite canonical graph |
| Forge Generators | Image/3D/texture/audio/motion/code models via adapters | Generated output is production-ready |
| Forge Tool Gateway | Type-checked/authorized engine commands, sandbox and budgets | A prompt is permission to execute |
| Forge Validator | Asset checks, builds, simulation, image comparisons, gameplay tests | Model's self-evaluation equals passing tests |

### 6.3 Model routing and deployment

Use **pluggable model adapters**, not a single exclusive provider. A task capability descriptor advertises input/output modalities (text/image/audio/video/mesh/motion), supported formats, expected hardware, local/cloud availability, model rights, latency band, cost, maximum context and content safety constraints. Choose a model based on user policy, available compute, commercial license and task-specific evaluation results. Support local inference (for suitable small models), company-hosted inference and third-party APIs as optional configurations; offline manual authoring must continue when none is available.

**Minimum model categories:** language/reasoning, embedding, image/texture, image-to-3D/mesh, geometry/retopology, motion, speech/transcription, sound/music where legally licensed, vision QA and optional video understanding. It is not necessary to train foundation models to ship useful creator AI. Specialized procedural algorithms often outperform generation for deterministic geometry, UV layout, physical constraints or collision creation.

### 6.4 RAG and search architecture

**Problem:** a project may contain millions of lines of code, hundreds of thousands of assets and thousands of design decisions. Putting everything in an LLM context window is slow, expensive and error-prone. RAG retrieves only relevant, authorized context.

**Candidate corpus:** project manifest; docs and design briefs; code symbols/ASTs and comments; scene and asset metadata; materials and shader descriptions; character rigs and animation tags; dialogue/sound metadata; test failures; performance samples; completed AI change logs. Store only permitted material; exclude API secrets and private files unless explicitly authorized.

**Initial storage:** SQLite records for file IDs, revisions, ownership, searchable metadata, FTS5 keyword search and index jobs. Persist embeddings locally only when a model/license policy allows it. With scale, evaluate a vector search engine such as Qdrant, but keep the canonical graph outside the vector database. Index updates must track source revision and content hash to avoid stale embeddings.

**Query pipeline (recommended):**

1. Interpret user request and current selection with permission scope.
2. Resolve exact IDs, names, paths and code symbols using deterministic metadata/lexical search.
3. Retrieve semantically relevant docs/assets with text/image embeddings only where helpful.
4. Filter by project, permissions, content licensing, type, platform, asset version and timestamp.
5. Combine/rerank lexical, vector and graph-neighbor results; attach source IDs and revision numbers.
6. Query WorldGraph for **exact** dependency relationships and current asset state.
7. Assemble bounded context, explicitly differentiating retrieved text from authority.
8. Generate a structured plan with citations to actual project resources, validate it and show the affected entities.
9. Refresh or reject stale context at execution time if the project revision has changed.

**Retrieval evaluation:** a benchmark dataset of real questions with known correct asset IDs; assess recall@k, exact-target accuracy, stale-context rate, permission leakage, p95 retrieval time and the value of vector search vs FTS alone. Avoid expensive vector infrastructure before these experiments justify it.

### 6.5 Knowledge graph and memory types

Use three layers rather than an unconstrained AI memory blob:

- **Factual project memory:** canonical engine graph and versioned metadata: scene contains entity; entity references mesh; mesh has material slots; rig drives animation; script consumes event; test covers component.
- **Authoring memory:** approved project style guides, prompt constraints, tasks, engineering decisions and creator preferences, all versioned/attributed and editable.
- **Ephemeral task memory:** a short-lived plan, tool responses, observations and pending approvals. Do not silently promote unreliable AI speculation into the canonical project graph.

A request such as 'change the castle bridge stone to damp limestone' requires semantic similarity to locate candidates, **then exact entity IDs** to target the correct bridge, validated material asset references and impact analysis on linked shader variants and cooked assets. Users should see *why* an asset was selected and be able to correct it.

### 6.6 Tool gateway: permissioned editor operations

AI can ask the engine to perform operations, but cannot directly write arbitrary project storage or shell commands. Implement typed APIs such as:

`project.read_summary`, `scene.list_entities`, `scene.query_component`, `entity.create`, `entity.transform.set`, `entity.component.add`, `material.instance.create`, `material.parameters.set`, `mesh.import`, `animation.clip.attach`, `script.propose_patch`, `test.run`, `profiler.capture`, `build.validate`, `build.package`.

Each tool declares JSON-schema-like typed arguments, capability/permission flags, read/write effects, maximum scope, time/cost budget, preview response, prerequisites, output identifiers, undo/compensation path and error taxonomy. API results are data, not instructions. Reject unrecognized fields and assets outside the active project. The LLM should not be given raw pointers, unrestricted C++ APIs, developer credentials or direct root filesystem access.

### 6.7 AI change lifecycle and state machine

`REQUESTED -> CONTEXT_RETRIEVED -> PROPOSED -> VALIDATED -> PREVIEWED -> APPROVED -> EXECUTING -> VERIFYING -> COMMITTED` with explicit failure/cancel/rollback branches. The UI displays affected assets, proposed commands, model/provider, estimated time/cost range when available, permissions, confidence and human-review checkpoints. 'COMMITTED' signifies edits applied, not that the game feature is production-ready. Only tests can establish the latter within their coverage limits.

**Approval tiers:**

- *Read:* look up assets, summarize logs, inspect source; no write rights.
- *Assist:* stage reversible edits; every transaction requires user review.
- *Bounded Autopilot:* execute preapproved low-risk operations in a sandbox; cap writes/compute/network spending; pause on novel risks.
- *High-risk actions:* file deletion, package publication, external messages, paid operations, secrets, plugin installation, code execution and broad refactors require a fresh explicit approval regardless of Autopilot status.

### 6.8 Sample AI plan contract

```json
{
  "plan_id": "plan-uuid",
  "project_id": "prj-uuid",
  "source_revision": 42,
  "goal": "Create a small playable blacksmith encounter",
  "constraints": {
    "max_asset_changes": 25,
    "max_cloud_cost_usd": 2.00,
    "allow_external_publish": false,
    "require_human_approval": true
  },
  "steps": [
    {"id":"s1","tool":"asset.search","mode":"read", "depends_on":[]},
    {"id":"s2","tool":"scene.entity.create","mode":"write", "depends_on":["s1"]},
    {"id":"s3","tool":"script.propose_patch","mode":"staged", "depends_on":["s2"]},
    {"id":"s4","tool":"test.run","mode":"read", "depends_on":["s3"]}
  ],
  "validation": ["scene_references", "gameplay_smoke", "performance_budget"],
  "status": "awaiting_review"
}
```

This is a **design example**, not a final public API or operational proof. It deliberately lacks executable code, secrets and unrestricted file paths.

### 6.9 AI outputs by production studio

| Studio | Generative / assistive outputs | Quality and editability requirements |
|:--|:--|:--|
| World/Terrain | Terrain height fields, road layout, biome placement, worldbuilding plan | Editable splines/layers; water/traffic/navigation validation |
| Modeling/Sculpt | Mesh drafts, low-poly variants, topology suggestions, masks | Editable topology, mesh manifold checks, UVs and artist override |
| Texture/Material | PBR texture channels, procedural layer stacks, shader nodes | Color-space correctness, baking tests, no baked-in lighting unless intentional |
| Character/Rig/Animation | Character meshes, skinning candidates, skeletal pose/motion clips | Rig compatibility, deformations, root motion, pose continuity |
| Code/Visual Graph | Typed component code, node graph, tests, documentation | Actual compile/graph validation, static analysis, user approval |
| VFX/Audio/Cinematic | Effect graphs, sound layers, shot lists, camera tracks | Timing, loudness and GPU budget checks, rights metadata |
| NPC/Multiplayer | Behavior trees, dialogue rules, net simulation test plans | Rule safety, deterministic baseline, security and latency checks |
| UI/QA/Build | Menus, accessibility proposals, tests, optimization suggestions | Contrast/keyboard tests, measured regressions, signed build gates |

**Do not claim generated media is owned by the platform or automatically clear of third-party rights.** Store licensing provenance and user authorizations; follow provider terms and jurisdiction-specific requirements.

### 6.10 Security threats and defenses

**Risks:** prompt injection in retrieved docs/assets, arbitrary tool calls, exfiltration of source projects or API keys, unsafe generated scripts/plugins, malicious model artifacts, dependency confusion, hidden remote costs, permission escalation, prompt memory poisoning, stale-revision overwrites and AI-caused data loss.

**Controls:** sanitize and label untrusted sources; validate tool arguments outside the model; permit only scoped capabilities; isolate script/AI workers; no secrets in prompts/logs; approval per high-risk action; checkout/checkpoint prior to edits; transaction logs; code execution sandbox and network restrictions; generated assets undergo import validation; end-to-end adversarial tests and rate/cost limits. Model self-review, 'system prompts', keyword blocking and friendly UI disclaimers are insufficient controls on their own.

### 6.11 AI benchmarking and release gates

Use a versioned evaluation suite of human-authored tasks, including an unknown project, corrupted assets, context conflicts, ambiguous target names, oversized content, missing permissions, offline mode and failed builds. Metrics: correct asset targeting, executable command validity, compile/test pass rate, visual consistency, user interventions, p95 latency, cloud cost/task, post-commit defects, accidental writes and successful rollback. QA should blind-review generated artifacts against professional criteria and compare the same workflow without AI and in competitor tools. Ship Autopilot only after real safeguards and demonstrated reliability at its supported task scope.

## 7. AI-powered end-to-end game creation workflow

**Example goal:** 'Build an original photorealistic coastal city block with a playable driving mission, traffic, pedestrians, weather, ambient 3D audio and a 60-FPS target.'

1. Read project settings and approved style guide, selected hardware tier, current world cells and content budget.
2. Retrieve relevant modular streets, road rules, props, vehicle archetypes, road materials and scene conventions; exact-match WorldGraph IDs.
3. Draft a proposal with generated/assembled assets, scope, cost, scene changes, licensing notes and test plan.
4. Generate editable road splines and blocks; use existing licensed content where possible; produce missing props with generator adapters.
5. Derive traffic/nav lane data and collision; create behavior profiles and spawn budgets; generate test scenarios.
6. Generate first playable mission and basic UI; stage code or visual graph edits and require compilation.
7. Create audio emitter zones, cinematic camera track and streaming cells; mark authoring/cooked dependencies.
8. Execute incremental import, cook, shader compile and automated mission checks; profile streaming and frame time.
9. Present a review report: what succeeded, what failed, what is approximate, what's artist-polish work, where content rights are uncertain.
10. Apply approved revisions; maintain audit/provenance; ask creator to sign off before packaging/publishing.

**Expected result at initial AI milestone:** a small *structured scene modification* with real undo. **Expected result at mature milestone:** a *tested scene/block or gameplay feature*, with known limitations. Automatic creation of an entire AAA game from one sentence is an unsolved long-horizon production challenge and is **not** a committed release promise.



# PART IV - Photorealistic AAA engine and Living City reference project

## 8. Reality Renderer: advanced graphics program

The user's north-star target is **more realistic and more powerful than contemporary AAA engines, including the class of game represented by GTA VI**. Treat this as a **measurable research agenda**: visual fidelity, temporal stability, asset scale, creator iteration time, performance-per-watt/memory, stability and platform coverage. It is not one feature switch. No individual effect ensures photorealism; the art direction, scanning/authoring, geometry, material models, lighting, animation, sound, streaming and post-processing must work together.

### 8.1 Rendering hardware interface (RHI)

Build WorldForge's RHI between engine code and Vulkan, with concepts for adapter capability discovery, command buffers, fences, resource lifetime, descriptor/pipeline management, render graph, memory budgeting and device-loss handling. Initially target **Windows/Linux via Vulkan**; qualify a macOS portability route and/or Metal backend before claiming a native-supported release. Shader compiler pipelines must support validated SPIR-V and predictable cross-platform feature degradation. Expose explicit capability tiers rather than silently dropping unsupported features.

### 8.2 Rendering stages and exit gates

| Renderer stage | Core features | Release gate |
|:--|:--|:--|
| R0 - First Light | Swapchain, triangle/cube, depth buffer, camera, resize, validation | Stable render loop with no blocking Vulkan validation issues |
| R1 - Foundational 3D | Indexed meshes, instancing basics, PBR metallic/roughness, directional light, tone mapper | Assets render with correct transforms and color-space handling |
| R2 - Real game scenes | Shadow maps, IBL, postprocess chain, transparency baseline, TAA testing, material variants | Consistent images and measurable performance on test scenes |
| R3 - Environment quality | Volumetric fog, atmospheric sky, foliage/wind, reflections, terrain, lighting authoring | Reference environment meets documented quality/frame budget |
| R4 - Scalable worlds | GPU culling, HLOD, streaming, virtualized texture/geometry research, async compute where justified | No major hitches along standardized traversal route |
| R5 - Advanced realism | Hardware ray-tracing/reflection/GI prototypes, denoising, path-traced offline/still mode | Feature validated on supported GPU with measurable gains/costs |
| R6 - Research beyond | Dynamic scene neural methods, reconstruction, physically richer materials/simulation | Controlled benchmarks, reproducibility and graceful fallback |

Use Vulkan's established ray-tracing/ray-query APIs when devices support them; provide robust raster/compute alternatives. A path-traced film render mode and a real-time 4K mode have different performance constraints and should not be conflated. Do not promise that all effects run at native 4K/60 on ordinary PCs.

### 8.3 The 16.67 ms frame budget

For 60 FPS, the **total frame period is ~16.67 ms**. CPU and GPU pipelines overlap, so summing budgets across independent threads is not the same as measuring final frame time. A planning *GPU* allocation for a particular high-end reference scene might begin with: visibility/geometry 3.5 ms; shadows/GI/reflections 4.5 ms; materials/lighting 3.5 ms; postprocessing/reconstruction 2.0 ms; particles/transparency 1.5 ms; reserve 1.67 ms. These are **illustrative targets, not actual achieved metrics**, and need profiling on named hardware. Record CPU game thread, render thread, GPU passes, frame pacing, VRAM residency, shader hitch rate and image quality. Permit 30/60/120 FPS quality profiles as appropriate.

### 8.4 Photorealistic material/lighting requirements

A production PBR pipeline must define linear lighting and color management (sRGB/linear source handling, HDR transfer, tone mapping, gamut/white point), material energy conservation, normal/tangent conventions, roughness/metalness, physically plausible light units, reflection probes/GI options, lightmaps where relevant, skin subsurface scattering, eye shader, hair anisotropy, cloth BRDF, water interaction and high-quality atmospheric scattering. Advanced authoring demands layer-based materials, texture baking, masks, decals, procedural material graphs, shader permutation management and material complexity reports. Always preserve adjustable layers and asset provenance.

### 8.5 Geometry and texture scale

Large worlds require clustered/instanced rendering, visibility culling, HLOD, vertex/mesh LOD, impostors, streaming tile caches, texture mip residency, anisotropic filtering, sampler caches and resource eviction. 'Infinite polygons' is not a credible product specification. Use measured screen-space error and GPU budgets. Keep collision and navigation meshes separate from render geometry as needed. Procedural content recipes must be seedable/versioned to support deterministic rebuilds and team collaboration.

### 8.6 Character realism

Support skeletal meshes, animation compression, blend spaces, root motion, retargeting, IK/FK, pose correction, morph targets and facial rigs before attempting production-scale digital humans. Advanced work includes cloth/hair simulation, skin shading, eyes/teeth, speech sync, performance capture integration, muscle/soft-tissue deformation, secondary motion and animation-aware LOD. Model/animation generation must preserve the artist's editability and verify animation foot planting, hand contacts, clipping and skeleton compatibility.

### 8.7 Image reconstruction and optional neural methods

Consider temporal upscaling, frame generation where appropriate, high-quality denoisers, radiance caching and neural material/texture compression. Their licenses, hardware support, image artifacts, temporal lag and frame pacing require independent evaluation. Neural output must have conventional fallbacks and must not compromise deterministic editor operations or offline authoring.

## 9. WorldStream: large-world engine architecture

### 9.1 World coordinates and partitioning

Use high-precision authoring coordinates or origin-rebasing strategies with explicit unit/coordinate conversion to GPU-local precision. Define spatial partition cells, streaming sources (camera/player/mission), priority, data layers, entity persistence, occlusion, cross-cell references and live editing. Support asset references across cells without eager-loading the whole world. Separate authoring logical geography from physical cooked cell layout. Build world-edit diff tools, cell ownership and dependency reports for collaboration.

### 9.2 Streaming pipeline

`view/player position -> cell priority -> asynchronous manifest load -> dependency prefetch -> decompression/decode -> GPU upload -> safe activation -> simulation tier choice -> eviction/save`. A streaming task must be cancelable and non-blocking; asset residency and pending requests are visible in the profiler. Explicitly specify memory caps and eviction policy, IO queue priorities, multiple-player/fast-vehicle prefetch and asset re-use. Validate worst-case transitions such as sudden teleport, high-speed driving, weather change and cinematics.

### 9.3 Hierarchical level of detail and occlusion

Build HLOD for distant buildings, terrain and crowds; material atlas/instancing where suitable; occlusion/visibility solutions appropriate to target hardware. Maintain collision/nav proxies for unloaded cells when game logic needs them. An entity outside the camera may still matter for a quest or traffic simulation; visibility and simulation lifetime must be separate systems.

### 9.4 City population and simulation LOD

Not every citizen in a huge city can run a fully detailed neural NPC, rigid-body cloth and facial animation at 60 FPS. Use **simulation tiers**:

- **Tier A (near/interactive):** full animation, collision, perception, nav, gameplay and relevant dialogue.
- **Tier B (nearby/background):** simplified AI, crowd locomotion and animation LOD, fewer physics events.
- **Tier C (distant/offscreen persistent):** coarse schedules, statistical traffic/population state and event simulation.
- **Tier D (unloaded):** compact persistent state or procedural regeneration with explicit narrative exceptions.

Promote/demote tiers according to distance, importance, line of sight, mission state and platform budget; preserve continuity for persistent characters and vehicles. Be transparent that crowd complexity is an engineering balancing act, not unlimited 'living minds'.

## 10. Forge Traffic, vehicle and mission systems

### 10.1 Roads and vehicle simulation

Represent roads with lanes, traffic signs/lights, permitted turns, speed rules, intersection graph, pedestrian crossings, parking/spawn zones, navigation constraints and temporary closures. Build a deterministic lane graph and decouple it from render meshes. Vehicle simulation integrates chassis/wheels/suspension, collisions, damage/dents (progressively), traction/surface wetness and basic recovery from traffic deadlocks. AI vehicle controllers must obey valid lane rules and safely degrade when regions unload. Test junction congestion, high-speed player approaches and weather changes.

### 10.2 Pedestrian behavior and NPCs

Create reusable NPC archetypes and state machines with schedule/event hooks, perception budgets, locomotion/nav queries and safe interactions. Add crowds with variation in clothing/animations, group behaviors, spatial avoidance and remote simulation. Optional LLM-driven high-level conversations may be interesting for named NPCs but must respect gameplay state, cost, response time, moderation and narrative constraints. Mission progression and real-time locomotion should remain under deterministic code.

### 10.3 Missions and systemic interactions

Use structured quest state graphs with typed events, preconditions, triggers, save/load transitions, checkpoints, cutscenes and failure/retry behavior. Support vehicle enter/exit, inventory, shops, NPC interactions, law-enforcement response as a gameplay system (not a production assumption), combat framework and accessibility. Authors need inspectors, debug overlays, visualization and assertions. AI-generated missions require state-space tests; narrative uniqueness and quality still need human editorial review.

### 10.4 Advanced physics and environmental simulation

Integrate rigid body/collision first. Research soft bodies, cloth, hair, ragdolls, fracture, destructible terrain, particles, fluids and water with cost-tiers and deterministic fallback. Use separate precision/performance profiles for cinematic simulation versus 60-FPS interactive play. Every high-cost effect needs a budget, culling/LOD and recovery behavior.

## 11. Living City: official AAA benchmark/reference game

**Reference project name:** *WorldForge Living City* (original city, characters, vehicles, story and intellectual property).  
**Purpose:** prove end-to-end production capability and benchmark the editor, runtime, AI tools and content pipeline.  
**Not the requirement:** copy GTA VI or deliver an effortless exact clone.  
**Initial reference geography:** one dense coastal urban block; expand to neighborhoods, a traversable district and ultimately an original metro region only after gates pass.

### 11.1 Reference game feature ladder

| Vertical slice | Included game systems | Verification criteria |
|:--|:--|:--|
| LC-00: Street scene | Single street, 4-8 buildings, drivable route, dynamic sun | Load/play on PC, no missing assets; visual benchmark captures |
| LC-01: Playable block | One car, one on-foot character, ambient audio, one short mission, collision | Mission completable; saved/reloaded with state correct |
| LC-02: Populated block | Pedestrians, traffic lane graph, weather/time changes, simple shops | No stuck traffic on test route; no serious streaming hitch |
| LC-03: Dense district | Multiple cells/interiors, vehicle switching, AI population tiers, mission chain | Repeated traversal and 1% low frame-time tests |
| LC-04: Metropolitan region | District streaming, longer missions, deeper traffic, cinematic content | Multi-hour sessions; content build, crash and memory qualification |
| LC-05: Next-gen research | Advanced material/lighting and long-horizon agent-assisted world creation | Benchmarked quality/time/performance improvement against baselines |

**First demo after First Light:** a third-person character walks/drives a short original street to complete a package-delivery mission. The road, vehicle, pedestrians, collisions, audio, UI, mission logic and build are authored using WorldForge's own tools wherever those tools exist; early stages may legitimately import Blender-made/third-party licensed assets.

### 11.2 Acceptance scenario LC-01 (detailed)

Given an editable project, a creator opens the city scene, selects one road segment and sees its editable lane splines. The creator places a licensed vehicle prefab, sets a mission destination and adds a pedestrian crowd archetype. The player can drive from spawn to a drop-off point, interact with the mission trigger, receive a UI acknowledgment and save/reload progress. The build system generates a Windows standalone executable. The profiler captures average/1% frame times on the test hardware and the test suite exercises player spawn, driving route, mission completion, reload and source asset reimport. No external editor is required to *play* the exported game. This is a future acceptance test, not an achieved milestone.

### 11.3 AI production scenario LC-AI-01 (detailed)

Given a selected city block, a style guide and a fixed technical budget, the creator requests 'add a weathered market square with pedestrian access and rain ambience'. Forge Intelligence searches existing licensed assets, proposes a street layout change, generates or selects props, creates material variants, attaches a spatial audio zone, updates nav/crowd graphs, checks lane continuity and runs a test drive. All changes appear in one reviewable transaction with provenance and cost. The user can accept individual substeps or revert the whole change. **Release gate:** at least a defined sample of independent evaluators must complete the task with lower defect/repair burden than a manual baseline.

### 11.4 Suggested benchmark scene and hardware tiers

Use *named measured machines*, not anonymous labels in marketing:

- **Tier D / developer minimum (to be qualified):** simple scenes, low texture/light/visibility settings, editor usability.
- **Tier C / mainstream PC:** 1080p or 1440p, upscaling if required; ordinary gameplay profiles.
- **Tier B / enthusiast:** high-quality rendering, dense block, 60 FPS target where achieved.
- **Tier A / flagship GPU:** 4K output high-quality mode, measured frame-time and stability; ray-traced/path-traced features selectively supported.

No target is finalized until actual benchmark hardware, quality settings and images are published. Record *render resolution vs output resolution*, CPU/game frame time, GPU frame time, frame pacing, VRAM, RAM, streaming spikes, load duration, camera route, weather, crowd density, asset version and build revision. Do not compare WorldForge running a lighter scene to Unreal rendering a more complex one.

### 11.5 'More advanced than Unreal' proof framework

Make narrowly falsifiable claims such as 'Given identical assets, an equivalent final image quality score and test machine, task X takes 30% fewer verified user actions' or 'Our streaming cell change creates fewer frame spikes on the same traversal workload'. **Do not preselect a superiority metric after running tests.** Publish hardware and build settings, include regressions and failure modes, have external artists evaluate output quality, compare tooling ergonomics and quantify cost of AI generation. The goal is original measured breakthroughs over time, not a slogan mistaken for an accomplished benchmark.



# PART V - Complete 40-screen / editor-mode product specification

## 12. Screen requirements index and general acceptance contract

Every WF-ID remains a **stable requirements identifier**, even where several tools are consolidated inside one dockable workspace. Each record defines purpose, placement, central surface, panels, user actions, canonical data, cross-workspace links, engine services, AI hooks, and acceptance gate. Requirements are progressive: a future-mode specification does not authorize a working control in the first release.

**Universal screen criteria:** consistent shell/theme, keyboard shortcuts, selection synchronization, non-destructive/undo-aware persistence, scope-safe error states, accessibility labels, localization-ready text, high-DPI layout, open-in-next-workspace links, and measured—not fictional—status values.

### 12.1 Master inventory

| ID | Workspace / mode | Main group | Planned stage |
|:--|:--|:--|:--|
| WF-01 | Home / Project Hub | Project & setup | F0 |
| WF-02 | World Editor | World & rendering | F0 then F1 |
| WF-03 | 3D Modeling Studio | Assets & geometry | F1/F2 |
| WF-04 | Sculpting Studio | Assets & geometry | F2/F3 |
| WF-05 | Texture Studio | Assets & geometry | F2 |
| WF-06 | Material Studio | Assets & geometry | F1/F2 |
| WF-07 | Character Studio | Characters & motion | F2/F3 |
| WF-08 | Rigging Studio | Characters & motion | F2 |
| WF-09 | Animation Studio | Characters & motion | F1/F2 |
| WF-10 | VFX Studio / Particle Graph | VFX & simulation | F2/F3 |
| WF-11 | Audio Studio | Audio & cinematics | F1/F2 |
| WF-12 | Cinematic Studio | Audio & cinematics | F2/F3 |
| WF-13 | Programming Studio (C++) | Gameplay & intelligence | F1 |
| WF-14 | Visual Scripting (Blueprint-style) | Gameplay & intelligence | F2 |
| WF-15 | NPC AI / Behavior Tools | Gameplay & intelligence | F2 |
| WF-16 | Multiplayer Studio | Multiplayer | F3 |
| WF-17 | UI Testing Mode | UI, playtest & quality | F2 |
| WF-18 | Build System | Build & distribution | F1 |
| WF-19 | Physics Studio | VFX & simulation | F1/F2 |
| WF-20 | UI Designer | UI, playtest & quality | F2 |
| WF-21 | Playtest Studio | UI, playtest & quality | F1/F2 |
| WF-22 | Testing & QA Studio | UI, playtest & quality | F1/F2 |
| WF-23 | Performance Profiler | UI, playtest & quality | F1/F2 |
| WF-24 | Asset Hub / Content Browser | Assets & geometry | F0/F1 |
| WF-25 | Version Control | Team & revisioning | F0/F1 |
| WF-26 | Team & Collaboration | Team & revisioning | F2/F3 |
| WF-27 | Publishing Studio | Build & distribution | F2 |
| WF-28 | Shader Editor | Assets & geometry | F2 |
| WF-29 | Lighting Studio | World & rendering | F1/F3 |
| WF-30 | Terrain & Procedural Worlds | World & rendering | F2/F3 |
| WF-31 | Render Settings / Graphics Quality | World & rendering | F1/F3 |
| WF-32 | Forge AI Creation Assistant | Gameplay & intelligence | F2/F3 |
| WF-33 | Spatial Audio / 360 Sound | Audio & cinematics | F2/F3 |
| WF-34 | Simulation Studio (fluids, cloth, destruction) | VFX & simulation | F3 |
| WF-35 | Navigation & NPC World Tools | World & rendering | F2 |
| WF-36 | Import & Export Pipeline | Assets & geometry | F1/F2 |
| WF-37 | Plugin & Tools Manager | Tools & extensions | F2/F3 |
| WF-38 | Project & Engine Settings | Project & setup | F0/F1 |
| WF-39 | Console & Diagnostics | UI, playtest & quality | F0 |
| WF-40 | Project Creation Wizard | Project & setup | F0 |

### 12.2 Individual screen engineering records

### WF-01 - Home / Project Hub

**Navigation:** Home icon; launcher, not persistent editing tab. **Delivery:** F0 (foundation); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** The entry point for opening, creating, recovering and learning about projects; must never imply active engine features that do not exist.

**Primary surface:** Project list/card grid with thumbnails, recent files, version/compatibility indicators and a compact new/open/import action strip. No giant banner once a project is open.

**Docked panels and options:** Recent projects, templates, recovery sessions, engine/runtime selection, update notices, tutorials and verified plugin compatibility.

**Core user actions:** Create, open, clone, restore, rename, reveal in file manager; search and sort; confirm conversion when a project schema is older.

**Authoritative data / file types:** ProjectManifest, recent-project cache, template catalogue, recovery snapshots and engine capability manifest.

**Connections and data flow:** Opens P40 wizard, P38 settings and P24 assets; launch enters P02 World or saved last workspace.

**Engine/backend services required:** ProjectManager, filesystem, version migration detector, template installer, recents index, crash recovery.

**Forge Intelligence integration:** Search templates and explain missing SDK/plugin requirements; suggest recovery options without modifying projects.

**Detailed acceptance scenario:** Open/recover a project on an unfamiliar machine. **Required evidence:** A valid new project opens and can be reloaded; a corrupt or unsupported project gives actionable errors without changing files.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-02 - World Editor

**Navigation:** Primary World workspace. **Delivery:** F0 then F1 (foundation); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Assemble gameplay worlds with a scene hierarchy and real renderer, while preserving direct access to terrain, lighting, physics and scripting.

**Primary surface:** Large perspective/orthographic Vulkan viewport with selection, translate/rotate/scale gizmos, snapping, camera controls, debug overlays and Play/Simulate.

**Docked panels and options:** Scene Outliner; content dock; details/Inspector; World Settings; contextual Terrain, Foliage, Lighting, Layers and Navigation submodes.

**Core user actions:** Create/delete/duplicate/reparent entities; multi-select; group, hide, lock, align, spawn prefab, drag asset into level; save/reopen.

**Authoritative data / file types:** SceneDocument, entity IDs, Transform, MeshRenderer, Camera, Light and world/streaming settings.

**Connections and data flow:** Meshes from P03/P04; materials P05/P06; lights P29; terrain P30; physics P19; AI/nav P35; playtest P21.

**Engine/backend services required:** ECS+SceneGraph, Vulkan renderer, selection picking, spatial index, command bus, serialization, undo and asset registry.

**Forge Intelligence integration:** Plan terrain/prop placement; preview entity transforms, asset dependencies, world cells and validation checks.

**Detailed acceptance scenario:** Modify one visible entity and persist it. **Required evidence:** Moving an entity updates viewport, Outliner, Inspector and serialized scene; reopening preserves hierarchy, transformations and assets.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-03 - 3D Modeling Studio

**Navigation:** Create > Modeling; mesh asset editor. **Delivery:** F1/F2 (progressive); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Create editable polygonal props and game-ready mesh assets without leaving the editor, eventually matching professional topology workflows.

**Primary surface:** Main mesh-edit viewport with vertex/edge/face selections, shaded/wireframe overlays, normals, transform handles and LOD statistics.

**Docked panels and options:** Object tree, modifier stack, mesh data inspector, UV tab, topology analysis, material slots and modeling history.

**Core user actions:** Primitives, extrude, inset, bevel, loop cut, bridge, merge, split, mirror, arrays, booleans, triangulate, UV unwrap and apply modifier.

**Authoritative data / file types:** Editable MeshAsset, mesh topology ID mapping, modifiers/history, material assignments, UV sets, LOD metadata.

**Connections and data flow:** Shares editable mesh with P04 Sculpt, P05 Texture, P06 Material, P07 Character, P19 Physics and World P02.

**Engine/backend services required:** Geometry kernels, half-edge or equivalent topology representation, CPU/GPU mesh editing, spatial queries, import/export.

**Forge Intelligence integration:** Propose editable topology/LOD variants; validate manifoldness, UVs and mesh scale before material/rig export.

**Detailed acceptance scenario:** Edit a vertex/edge/face and update asset references. **Required evidence:** A user edits geometry, performs undo/redo, saves and sees updated mesh in an existing scene with references intact.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-04 - Sculpting Studio

**Navigation:** Create > Sculpt; mesh asset mode. **Delivery:** F2/F3 (progressive); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Perform high-resolution non-destructive surface authoring for characters, environments and hero assets.

**Primary surface:** Dominant clay/material preview with brush cursor, symmetry visualization, masks, detail levels and interactive navigation.

**Docked panels and options:** Brush shelf, multiresolution/remesh controls, sculpt object tree, layer stack, alpha library, topology/polygon count, masks.

**Core user actions:** Draw, clay, crease, smooth, grab, inflate, flatten, scrape, symmetry, alpha stamps, mask, voxel remesh, dynamic topology and bake.

**Authoritative data / file types:** SculptAsset, stroke journal or layers, mesh levels, masks, brush settings and high-poly/low-poly relationships.

**Connections and data flow:** Model P03 provides base mesh; Texture P05 receives baked details; Character P07 receives shapes; Asset Hub P24 tracks results.

**Engine/backend services required:** Spatial brush acceleration, subdivision/remeshing, mesh multiresolution, safe GPU preview and memory budgeting.

**Forge Intelligence integration:** Suggest non-destructive brushes, masks, remesh/retopo tasks and preserve editable layers.

**Detailed acceptance scenario:** Sculpt one mesh at multiple subdivision levels. **Required evidence:** Strokes are reproducible and undoable; topology operations preserve meaningful asset links or explicitly report required rebakes.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-05 - Texture Studio

**Navigation:** Create > Texture; material texture-set mode. **Delivery:** F2 (indie production); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Paint, layer, bake and export physically based texture maps while viewing the final material on the target asset.

**Primary surface:** Dual 3D model preview and 2D UV/texture canvas with selectable channel (base color, roughness, metalness, normal, AO, height, emissive).

**Docked panels and options:** Texture set list, brush/stencil shelf, paint layers, masks, smart materials, baking settings, channel thumbnails, export presets.

**Core user actions:** Paint, projection, fill, erase, mask, clone, smart wear/dirt, bake normal/AO/curvature, adjust channel bit depth and reimport.

**Authoritative data / file types:** TextureSet, layer stack, source images, masks, UV relationships, color-space metadata, bake dependencies.

**Connections and data flow:** UVs from P03; high-poly normals from P04; P06 material graph consumes texture outputs; World P02 updates in situ.

**Engine/backend services required:** Brush engine, GPU paint surfaces, image codecs, bake pipeline, color management, tile/UDIM design, undo transactions.

**Forge Intelligence integration:** Generate licensed PBR map candidates, seam-aware paint tasks and channel/color-space validation.

**Detailed acceptance scenario:** Paint a PBR mask and inspect multiple maps. **Required evidence:** A changed paint layer updates a linked material and reopens without destructive flattening; game export uses baked textures.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-06 - Material Studio

**Navigation:** Create > Materials; opens by material asset. **Delivery:** F1/F2 (progressive); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Build reusable surface appearances using PBR parameters and a validated graph rather than hard-coded shader values.

**Primary surface:** Realtime material preview on sphere/mesh/scene with environment lighting plus an optional central node graph editor.

**Docked panels and options:** Material library, node palette, graph canvas, texture channels, static/dynamic parameters, blend modes, instances and compile errors.

**Core user actions:** Create material, change parameters, wire nodes, create instance, preview shader permutations, inspect compilation and reset values.

**Authoritative data / file types:** MaterialGraph, MaterialInstance, TextureRef, ShaderVariant, render-state and compile metadata.

**Connections and data flow:** P05 produces maps; P28 owns advanced shader code/graph; P29 controls preview lighting; P02 and P07 use instances.

**Engine/backend services required:** Material expression graph, shader compiler, parameter binding, variant caching, render-state validation and hot reload.

**Forge Intelligence integration:** Generate material parameter variants and shader graph proposals; run compile and visual comparisons.

**Detailed acceptance scenario:** Create a reusable material instance. **Required evidence:** Changing a material instance changes referenced objects without altering its parent; shader errors leave the last valid material visible.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-07 - Character Studio

**Navigation:** Character workspace; character asset editor. **Delivery:** F2/F3 (progressive); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Assemble game-ready characters from geometry, shape controls, materials, garments and equipment in a traceable workflow.

**Primary surface:** Turntable 3D character preview with front/side/back views and optional wireframe, hair, skin, UV and rig diagnostic panes.

**Docked panels and options:** Character/preset library, body and facial morphs, wardrobe/equipment slots, expression thumbnails, hair/groom, LOD and validation.

**Core user actions:** Choose base archetype, edit morphs, attach armor, assign materials, inspect LOD, generate a character prefab and send to rigging.

**Authoritative data / file types:** CharacterAsset, body/face morphs, slot attachment refs, materials, meshes, skeleton link and game-ready validation report.

**Connections and data flow:** Models P03, sculpt P04 and texture P05 feed character; rig P08 and animation P09 consume; physics P19 cloth later.

**Engine/backend services required:** Character assembly and dependency graph, morph evaluation, skinning binding, LOD generation/validation, prefab system.

**Forge Intelligence integration:** Draft character variants, outfits and LODs using project style constraints and licensing provenance.

**Detailed acceptance scenario:** Create a character variant linked to a rig. **Required evidence:** A character created from interchangeable assets retains valid rig/material references and can be placed as a prefab in P02.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-08 - Rigging Studio

**Navigation:** Character > Rig; specialized rig asset editor. **Delivery:** F2 (indie production); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Create, skin and inspect skeletal rigs for animation, procedural control and runtime posing.

**Primary surface:** Character in T/A pose with color-coded skeleton bones, IK/FK manipulators, gizmos and weight-paint diagnostic overlay.

**Docked panels and options:** Bone hierarchy, controls/constraints list, IK setup, weight brush settings, influence heatmaps, pose library and retarget mapping.

**Core user actions:** Add/reparent/orient/mirror bones, bind skin, paint/normalize weights, configure control rig and IK, validate hierarchy and retarget.

**Authoritative data / file types:** SkeletonAsset, BindPose, SkinWeights, ControlRig, Constraints, RetargetProfile and bone GUID mapping.

**Connections and data flow:** P07 sources the character; P09 consumes rig for clips; P19 ragdoll maps physics bodies to bones; P12 cinematics pose rig.

**Engine/backend services required:** Skeletal skinning, constraint graph, IK solvers, vertex influences, bone mapping and non-destructive import.

**Forge Intelligence integration:** Suggest bone hierarchy/weights/IK; detect deformation, rig mismatch and animation-retarget impact.

**Detailed acceptance scenario:** Retarget a skeleton and verify deformation. **Required evidence:** A skinned character animates without unexplained deformation; bone changes invalidate dependent clips with clear migration guidance.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-09 - Animation Studio

**Navigation:** Animation workspace; clip/state-machine asset tabs. **Delivery:** F1/F2 (progressive); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Author clips and movement logic for skeletal, prop, camera and event animation across gameplay and cinematics.

**Primary surface:** Dominant character/scene preview above resizable timeline, dope sheet and curve editor; optional animation state graph.

**Docked panels and options:** Animation asset tree, clip/take list, layers, IK controls, retargeting, animation notifies, compression, root-motion inspector.

**Core user actions:** Keyframe, scrub, adjust tangents, layer, blend, retarget, attach event, set transitions, preview controller and export clips.

**Authoritative data / file types:** AnimationClip, keyframe tracks, curve tangents, layer stack, state machine, bone references, notifies and compression settings.

**Connections and data flow:** Consumes P08 rigs; drives P02 actors and P21 playtest; triggers P11 audio and P10 VFX; P12 sequences reuse same clips.

**Engine/backend services required:** Interpolation and blend trees, skeleton evaluation, retargeter, event dispatcher, deterministic preview, optimized runtime playback.

**Forge Intelligence integration:** Propose keyframes/retargeted actions/blend transitions; test root motion and foot planting.

**Detailed acceptance scenario:** Blend two animations and play in the world. **Required evidence:** Selected clip updates its bound preview rig; notifies fire at the same logical frames in playtest and packaged runtime.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-10 - VFX Studio / Particle Graph

**Navigation:** VFX workspace; Niagara-like original graph. **Delivery:** F2/F3 (progressive); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Author high-impact real-time particles and effects using WorldForge-owned node graphs and a predictable GPU/CPU preview.

**Primary surface:** Effect preview viewport with particles, bounds and debug counters next to graph, curves and multi-emitter timeline.

**Docked panels and options:** Emitter library, node graph, spawn/render modules, system inspector, material slots, event tracks, variation thumbnails, performance.

**Core user actions:** Build emitter, connect modules, author spawn/lifetime/forces/color curves, collision, trails, events, preview, profile and set LOD.

**Authoritative data / file types:** VFXGraph, Emitter, ParticleModule, ResourceBinding, MaterialRef, GPU program and event descriptors.

**Connections and data flow:** P06 shaders/materials, P09 notifies, P12 cinematics, P19 physics and P02 scene placement all consume VFX assets.

**Engine/backend services required:** Typed graph compiler, GPU simulation buffers, resource binding, deterministic seed management, effect bounds/scalability.

**Forge Intelligence integration:** Plan effects graphs, emitter budgets and consistent variants; profile GPU/CPU and particle lifetime.

**Detailed acceptance scenario:** Create an effect from a reusable graph. **Required evidence:** An effect can be edited and reloaded while in a scene; invalid graph connections are rejected with explanatory errors.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-11 - Audio Studio

**Navigation:** Audio workspace; audio asset/mix editor. **Delivery:** F1/F2 (progressive); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Arrange and mix sound effects, dialogue, ambience and adaptive music for a game, retaining a connection to in-world emitters.

**Primary surface:** Multitrack waveform/timeline and mixer, with an embedded video or 3D viewport preview for synchronized sound design.

**Docked panels and options:** Asset library, tracks, buses, FX rack, fades, markers, automation, event graph, meters, ducking and loudness/peak analysis.

**Core user actions:** Import, trim, fade, loop, mix, route, normalize, create event cues, add bus effects, preview and prepare streamed/compressed output.

**Authoritative data / file types:** AudioAsset, Clip, Track, MixBus, AutomationCurve, EventCue, ImportMetadata and spatial emitter reference.

**Connections and data flow:** P33 spatial tab handles 3D positioning; P12 sequences reference cues; P09 notifies trigger SFX; P21 tests runtime sound.

**Engine/backend services required:** miniaudio backend initially, sample-accurate graph/mixer later, decoding and resampling, streaming and real-time-safe audio threads.

**Forge Intelligence integration:** Find sound assets, propose mix/effect chains and check clipping/loudness/rights.

**Detailed acceptance scenario:** Place and mix multiple sound sources. **Required evidence:** Audio cue playback works in editor and runtime; playback is synchronized and no UI-thread operation blocks the real-time audio callback.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-12 - Cinematic Studio

**Navigation:** Cinematic workspace; Sequencer editor. **Delivery:** F2/F3 (progressive); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Create cameras, shots, cutscenes, dialogue and presentation sequences using the actual game scene and assets.

**Primary surface:** Camera viewport with safe frames and cinematic framing plus multi-camera preview, shot list and large dockable sequencer.

**Docked panels and options:** Sequence browser, camera/lens inspector, camera cuts, actor/light/audio/VFX tracks, color controls, film/render queue.

**Core user actions:** Create shot, place camera, keyframe lens/transform, edit cuts, sync dialogue, apply color look, test cutscene and render preview.

**Authoritative data / file types:** CinematicSequence, Shot, CameraRig, Track, AssetRefs, RenderQueueJob, ColorLook settings.

**Connections and data flow:** Shares P02 scenes, P07 characters, P09 animation, P10 VFX and P11 audio; output can feed P27 trailer assets.

**Engine/backend services required:** Timeline sequencer, frame-accurate event scheduler, camera model, offline frame capture/encoder pipeline later.

**Forge Intelligence integration:** Suggest shot list, camera positions, continuity, cuts and timing; leave tracks editable.

**Detailed acceptance scenario:** Create a shot sequence and render a preview. **Required evidence:** Sequence playback reproduces camera cuts and animation/audio events; saving does not create disconnected copies of actors.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-13 - Programming Studio (C++)

**Navigation:** Gameplay > Code; source-file tabs. **Delivery:** F1 (creator alpha); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Provide a real C++ programming and debugging environment tightly linked to engine objects, without forcing visual scripting.

**Primary surface:** Tabbed code editor with syntax diagnostics, quick navigation, breakpoints and source map, complemented by logs and debugger docks.

**Docked panels and options:** Project tree, symbol outline, problems, build output, call stack, watches, variables, search, revision info and documentation.

**Core user actions:** Create/open source, navigate definitions, compile, run player, debug breakpoints, inspect variables and open owning scene/entity.

**Authoritative data / file types:** C++ source files, compile_commands.json, module manifest, build graph, diagnostics, debugger symbol references.

**Connections and data flow:** Creates gameplay components used by P02 World and P21 Playtest; P14 Visual Scripting later invokes exposed APIs; P18 builds.

**Engine/backend services required:** CMake compiler integration, LSP/clangd, debugger adapter, safe build process, module reload strategy and reflection metadata.

**Forge Intelligence integration:** Read code symbols via RAG, propose C++ patches with diff review, compilation and tests.

**Detailed acceptance scenario:** Edit/compile a C++ component and review diagnostics. **Required evidence:** Code compiles into a loadable game module with diagnostics linked to lines; debugger pauses game execution and inspects state.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-14 - Visual Scripting (Blueprint-style)

**Navigation:** Gameplay > Graph; graph asset editor. **Delivery:** F2 (indie production); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Let creators build typed gameplay logic using original visual graph tooling without relying on Unreal technology or copying its APIs.

**Primary surface:** Pan/zoom node canvas with typed pins, wires, comments, reroutes and breakpoint highlights, plus runtime preview.

**Docked panels and options:** Node palette, asset tree, variables/functions/events, component list, Inspector, compiler results, graph debugger and preview.

**Core user actions:** Add events/functions, wire pins, define variables, compile graph, step through execution, watch values and refactor subgraphs.

**Authoritative data / file types:** GameplayGraph, NodeID, PinID, FunctionSignature, VariableMetadata, Bytecode/compiled plan, debug mapping.

**Connections and data flow:** Can attach graphs to P02 entities; graph nodes expose native C++ components from P13; supports P21 and P22 debugging/tests.

**Engine/backend services required:** Typed graph schema, compiler/interpreter, reflection registry, runtime graph scheduler, cycle/type validation and trace support.

**Forge Intelligence integration:** Generate typed visual graphs; verify pin types, cycle restrictions, determinism and runtime semantics.

**Detailed acceptance scenario:** Construct/validate a visual script behavior. **Required evidence:** A user can create input -> movement behavior, inspect validation errors and replay behavior in a standalone player.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-15 - NPC AI / Behavior Tools

**Navigation:** Gameplay > NPC AI; behavior tree and nav tabs. **Delivery:** F2 (indie production); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Author in-game agent behavior rather than generic text generation: perception, states, navigation, decisions and debugging.

**Primary surface:** AI simulation viewport with perception cones, paths and agent labels plus behavior-tree or state-machine graph.

**Docked panels and options:** NPC library, behavior tree, blackboard, EQS/query visualizer, perception settings, navmesh and agent state/events.

**Core user actions:** Create behavior, define priorities, connect blackboard keys, configure sight/hearing, debug transitions and validate nav paths.

**Authoritative data / file types:** AIController, BehaviorTree, Blackboard, PerceptionConfig, NavQuery, AgentRuntimeTelemetry.

**Connections and data flow:** Depends on P35 navigation, P02 world and P13 gameplay; debug in P21; automate scenarios through P22 testing.

**Engine/backend services required:** Behavior tree interpreter, perception queries, pathfinding, navmesh runtime, agent scheduler and state-recording debugger.

**Forge Intelligence integration:** Generate constrained NPC state machine/behavior tree and dialogue rules; test perceptual responses.

**Detailed acceptance scenario:** Make guard NPC patrol, notice and pursue. **Required evidence:** Given a simple patrol scenario, the same NPC follows routes, reacts to a stimulus and recovers to patrol reproducibly.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-16 - Multiplayer Studio

**Navigation:** Gameplay > Multiplayer; dedicated sub-editor. **Delivery:** F3 (advanced/AAA research); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Manage sessions, replication, client/server simulation, networking budgets and multiplayer validation.

**Primary surface:** Multi-client gameplay preview plus replication graph and latency/bandwidth charts with independent server/client consoles.

**Docked panels and options:** Session configs, replication rules, client table, dedicated server profiles, join/match flow, prediction diagnostics, network emulation.

**Core user actions:** Host/join locally, launch N clients, inspect replicated fields, induce latency/loss, record desync and package server build.

**Authoritative data / file types:** SessionConfig, NetworkProfile, ReplicationPolicy, NetworkEntityID, TransportSettings, CapturedTrace.

**Connections and data flow:** P13 components expose serializable state; P21 launches sessions; P22 runs tests; P23 profiles network; P18 builds headless server.

**Engine/backend services required:** Transport abstraction, authoritative simulation, state serialization, interpolation/prediction, interest management, security.

**Forge Intelligence integration:** Propose replicated properties and server validation checks; generate latency/packet-loss test cases.

**Detailed acceptance scenario:** Simulate latency and inspect replication. **Required evidence:** Two players interact with the same object consistently under simulated latency; server authority rejects invalid client edits.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-17 - UI Testing Mode

**Navigation:** Testing > UI; launched from UI Designer. **Delivery:** F2 (indie production); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Validate game user interfaces across resolutions, inputs, localization and accessibility, without duplicating the UI authoring workspace.

**Primary surface:** Game UI test viewport with bounds, focus order and visual-diff overlays alongside test and recorder docks.

**Docked panels and options:** Widget hierarchy, smoke tests, input recorder, assertion list, baseline/current image comparison, keyboard/gamepad navigation.

**Core user actions:** Record action sequence, simulate input, assert text/focus/state, compare screenshots, export failure evidence and rerun tests.

**Authoritative data / file types:** UITestCase, InputTrace, ExpectedWidgetState, ScreenshotBaseline, AccessibilityResults.

**Connections and data flow:** UI layouts from P20; P21 runtime events; P22 QA suite; P27 release validation gate.

**Engine/backend services required:** Headless UI runner, test hooks, screenshot comparator, accessibility inspector, deterministic UI state serializer.

**Forge Intelligence integration:** Generate keyboard/responsive/accessibility test cases and evidence-based visual-diff reports.

**Detailed acceptance scenario:** Verify a HUD in 1080p and 4K with accessibility. **Required evidence:** A Continue button test passes by keyboard and mouse, fails with meaningful evidence when the button is missing or disabled.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-18 - Build System

**Navigation:** Build workspace or Build menu. **Delivery:** F1 (creator alpha); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Compile code and cook project resources reproducibly into a launchable game artifact with inspectable logs and profiles.

**Primary surface:** Build configuration editor, target selector, asset-cooking status, build queue, console log, artifacts table and size budget charts.

**Docked panels and options:** Platform profiles, optimization levels, output directories, incremental/full build, release symbols, asset-cook per-type progress.

**Core user actions:** Validate, queue/cancel/retry build, inspect failed step, open artifact, compare sizes, run player and generate signed manifests later.

**Authoritative data / file types:** BuildProfile, CookGraph, BuildJob, ToolchainManifest, Artifact, BuildReport, ContentHashCache.

**Connections and data flow:** Consumes all project assets; reports failures to P22 QA and P39 Console; successful artifacts transfer to P27 Publishing.

**Engine/backend services required:** Headless WorldForgeBuild executable, dependency graph, content hashing/cook cache, native compiler, packager and target profiles.

**Forge Intelligence integration:** Explain build errors, propose settings changes and run authorized, reproducible cook/package tests.

**Detailed acceptance scenario:** Create reproducible Windows build artifacts. **Required evidence:** From a clean checkout, a specified Windows project builds deterministically enough to run and produces a machine-readable report.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-19 - Physics Studio

**Navigation:** Effects/Simulation > Physics; world context or physics asset. **Delivery:** F1/F2 (progressive); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Create rigid-body and collision behavior, then extend toward vehicles, cloth, ragdolls and destruction.

**Primary surface:** Simulation viewport with collision shapes, contact points, bounds, forces, selectable bodies and fixed-step playback controls.

**Docked panels and options:** Physics asset tree, body/shape inspector, constraints graph, materials, debug overlays, live profiler and simulation timeline.

**Core user actions:** Add body/collider, assign mass/friction/restitution, link joints, simulate/pause/step/reset, inspect contacts and save presets.

**Authoritative data / file types:** PhysicsBody, ColliderShape, Joint, PhysicsMaterial, WorldPhysicsSettings and collision-layer matrix.

**Connections and data flow:** Mesh colliders from P03, ragdolls P08, world entities P02, destruction/VFX P10/P34 and runtime P21.

**Engine/backend services required:** Jolt adapter, shape cooking, fixed-step simulation, debug draw, collision filtering and serialization.

**Forge Intelligence integration:** Suggest collider shapes/joints/mass; run bounded simulation checks and report stability.

**Detailed acceptance scenario:** Drop a crate on a destructible prop safely. **Required evidence:** A box reliably falls onto a floor in Editor preview and WorldForgePlayer; collision and transform changes remain synchronized.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-20 - UI Designer

**Navigation:** UI workspace; UI layout asset. **Delivery:** F2 (indie production); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Design menus, HUDs and interactive game interfaces visually with keyboard/gamepad/accessibility support.

**Primary surface:** WYSIWYG screen canvas with guides, anchors, breakpoints and responsive-device preview; selected widget handles visible.

**Docked panels and options:** Widget tree, palette, properties, themes/style guide, state preview, bindings, animation timeline, localization and a11y checks.

**Core user actions:** Drag controls, set anchors, theme states, bind gameplay data, design focus navigation, preview at aspect ratios and save layouts.

**Authoritative data / file types:** UILayout, WidgetTree, StyleSheet, AnimationTrack, DataBinding, LocalizationKey and FocusNavigationGraph.

**Connections and data flow:** P17 tests UI, P21 displays it in game, P13/P14 supply data/actions, P25 versions layouts and P27 packages localizations.

**Engine/backend services required:** Runtime UI tree, DPI-scale layout engine, input/focus routing, style renderer, binding validation and localization support.

**Forge Intelligence integration:** Generate editable HUD/menu layouts using theme tokens; validate focus, safe areas and localization.

**Detailed acceptance scenario:** Author responsive HUD/menu widget. **Required evidence:** A menu adapts to 16:9 and ultrawide previews without clipped text, and button navigation works on keyboard/controller.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-21 - Playtest Studio

**Navigation:** Playtest workspace; also F5 from World. **Delivery:** F1/F2 (progressive); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Run the actual game in an embedded or standalone session with safe tools for debugging and repeating gameplay situations.

**Primary surface:** Dominant live game viewport with optional gamepad overlays, pause/stop/possess controls, FPS/perf HUD and session identifiers.

**Docked panels and options:** Session browser, runtime entity tree, variable watches, test presets, logs, recordings, bookmarks, input diagnostics, stats.

**Core user actions:** Start/stop/restart/eject, possess actor, spawn controlled test entities, set time/weather, bookmark frame, record inputs and replay.

**Authoritative data / file types:** PlaytestSession, RuntimeSnapshot, InputTrace, DebugCommand, Bookmark, CrashArtifact and PerfCapture.

**Connections and data flow:** Uses P02 scenes, P13 code, P14 graph, P15 NPC AI, P17 UI tests and P16 network simulation; feeds P22 and P23.

**Engine/backend services required:** Separate runtime context/process where feasible, entity/asset hot reload rules, debugger bridge, input capture and reliable cleanup.

**Forge Intelligence integration:** Guide or automate limited gameplay routes; capture controller inputs, replay traces and defects.

**Detailed acceptance scenario:** Run a saved playtest route. **Required evidence:** Launching and stopping repeatedly does not corrupt the editing scene; a packaged player matches essential gameplay behavior.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-22 - Testing & QA Studio

**Navigation:** Analyze > Testing; UI Testing tab is P17. **Delivery:** F1/F2 (progressive); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Organize deterministic automated and manual quality checks for the editor, runtime, assets, accessibility and platform release.

**Primary surface:** Results dashboard with suite counts, failures, trends, search/filter and selected-test evidence.

**Docked panels and options:** Test explorer, execution configs, coverage, logs, crash cases, defect tracker, CI status, reports and regression history.

**Core user actions:** Define suite, run selected tests, rerun failure, compare builds, attach reproduction, mark severity and export report.

**Authoritative data / file types:** TestSuite, TestCase, TestRun, Assertion, Evidence, BugReport, CrashReport and CIJob.

**Connections and data flow:** Invokes P17 UI checks, P21 playtests, P16 network tests and P18 build gates; shares failure traces with P39 Console.

**Engine/backend services required:** Test orchestration, Catch2/CTest bridge, game automation hooks, report aggregation, test evidence storage, crash symbols.

**Forge Intelligence integration:** Generate tests from requirements, triage crashes and deduplicate defects with project-specific evidence.

**Detailed acceptance scenario:** Diagnose a known failing regression. **Required evidence:** A deliberately broken gameplay case is reported with exact assertion, log and reproduction; release can be blocked by gate policy.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-23 - Performance Profiler

**Navigation:** Analyze > Profiler; dockable capture view. **Delivery:** F1/F2 (progressive); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Reveal CPU, GPU, memory, asset streaming and frame-time bottlenecks without resorting to speculative visual judgments.

**Primary surface:** Frame-time timeline with CPU/GPU lanes, profiler tree and capture timeline; optional synchronized game screenshot.

**Docked panels and options:** Capture sessions, call tree, render passes, memory allocation chart, shader hot spots, streaming IO, regressions and budgets.

**Core user actions:** Start capture, pause, select frame, search cost centers, compare baselines, set frame budget, export capture and link offender.

**Authoritative data / file types:** ProfilerCapture, MetricSample, FrameMarker, CallTree, GPUEvent, MemoryAllocationEvent and RegressionBaseline.

**Connections and data flow:** P02/P21/P18 provide sessions; P31 render settings and P30 terrain streaming respond to findings; P22 enforces budgets.

**Engine/backend services required:** Tracy instrumentation, GPU timestamp queries, memory labels, render graph instrumentation, capture file format and comparators.

**Forge Intelligence integration:** Explain frame traces and rank optimization candidates; accept only measured improvements.

**Detailed acceptance scenario:** Find slow GPU/CPU pass and compare traces. **Required evidence:** A sample bottleneck can be traced from frame spike to a named render pass or function with repeatable metrics on defined hardware.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-24 - Asset Hub / Content Browser

**Navigation:** Content rail / bottom dock; full view on demand. **Delivery:** F0/F1 (progressive); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Provide authoritative search, import, dependency and lifecycle tools for every asset in a WorldForge project.

**Primary surface:** Resizable tree/grid/list browser with thumbnails, type filters, metadata, dependency graph, previews and import tasks.

**Docked panels and options:** Favorites/collections, project folders, tags, import status, referencers, unused asset report, source provenance and asset health.

**Core user actions:** Import, rename, move, duplicate, reimport, find references, replace reference, validate, bulk tag, migrate and reveal file.

**Authoritative data / file types:** AssetRegistry, AssetID, source URI, content hash, dependency edges, import settings, cooked cache and provenance metadata.

**Connections and data flow:** Every authoring page creates/consumes assets through P24; P36 import workflow writes registry; P25 versions source files.

**Engine/backend services required:** Filesystem watcher, local asset-index service/SQLite, dependency graph, thumbnail generator, importer jobs, atomic moves.

**Forge Intelligence integration:** Hybrid RAG/semantic and exact-ID asset search; deduplication and provenance warnings.

**Detailed acceptance scenario:** Locate asset by exact name and visual similarity. **Required evidence:** Renaming or moving an asset preserves references; changing its source triggers only necessary rebuilds and marks invalid dependents.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-25 - Version Control

**Navigation:** Source Control icon; history/diff dock. **Delivery:** F0/F1 (progressive); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Expose project revision history, working-tree changes and safe collaborative branches in editor terms.

**Primary surface:** Changes list, diff/preview, commit editor, branch graph, conflict details and submit history with binary asset indicators.

**Docked panels and options:** Git status, staging, commit, fetch/push/pull, branch switch, file-lock and large-asset state, review comments.

**Core user actions:** Stage, commit, inspect diff, stash changes, resolve text conflict, inspect file history, open affected asset; request locks later.

**Authoritative data / file types:** RepositoryState, RevisionID, FileChange, AssetGUID mapping, ConflictRecord and optional lock metadata.

**Connections and data flow:** Cross-links to P24 assets, P02 scenes, P26 reviews/tasks and P18 release source revision.

**Engine/backend services required:** Git CLI/libgit2 evaluation, Git LFS, project conflict adapters and source-control credentials in OS credential store.

**Forge Intelligence integration:** Summarize diffs and potential conflicts; no autonomous merge/deletion without scoped approval.

**Detailed acceptance scenario:** Review and revert a scene change. **Required evidence:** A user can commit a scene change, reopen earlier version and resolve an edited metadata file without corrupting unrelated assets.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-26 - Team & Collaboration

**Navigation:** Team icon; dockable project/issue view. **Delivery:** F2/F3 (progressive); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Coordinate task ownership, reviews, asset approvals and permission-based teamwork, without requiring realtime editing on day one.

**Primary surface:** Project overview with people, tasks/kanban, scene/asset reviews, comments, release milestone calendar and activity.

**Docked panels and options:** Members/roles, assignments, review queue, notifications, asset owners, project activity and synchronized repository status.

**Core user actions:** Create task, assign owner, request review, comment on asset revision, link defect, approve change and track milestone.

**Authoritative data / file types:** TeamMember, Role, WorkItem, Review, Comment, Milestone and RevisionLink.

**Connections and data flow:** Links bugs P22, source changes P25, assets P24 and release gates P27; online features remain optional.

**Engine/backend services required:** Future collaboration service, access control, audit log, offline queue/merge and secure project permissions.

**Forge Intelligence integration:** Turn milestones into tasks and identify dependency risks; honor project/team authorization.

**Detailed acceptance scenario:** Assign and review tasks with permissions. **Required evidence:** A reviewer can inspect the exact revision attached to a task and approve/reject it; offline projects remain usable.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-27 - Publishing Studio

**Navigation:** Build > Publish; post-artifact release workspace. **Delivery:** F2 (indie production); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Turn verified build artifacts into release candidates with human approvals and destination-specific compliance checks.

**Primary surface:** Release checklist, store destination cards, metadata/cover art, build artifact selection, validation results and submission status.

**Docked panels and options:** Version/changelog, platform package requirements, screenshots/trailers, dependency/license audits, signing and rollback plans.

**Core user actions:** Select successful build, fill metadata, run validations, create release bundle, upload through authorized integration and stage rollout.

**Authoritative data / file types:** ReleaseCandidate, BuildArtifactRef, DestinationProfile, StoreMetadata, ValidationGate, SubmissionRecord.

**Connections and data flow:** Consumes P18 successful builds, P22 QA reports, P23 performance budgets and P12 cinematic exports; revisions from P25.

**Engine/backend services required:** Release orchestrator, platform adapters, secure credentials, metadata validators, checksums, signing, external store APIs later.

**Forge Intelligence integration:** Review release compliance, descriptions, missing licenses and checks; never auto-publish without authorization.

**Detailed acceptance scenario:** Prepare a store release candidate. **Required evidence:** A release cannot be marked ready when required test, license or signing checks fail; publishing actions require explicit confirmation.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-28 - Shader Editor

**Navigation:** Materials > Advanced Shader or graph asset. **Delivery:** F2 (indie production); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Expose lower-level shader graphs and source editing with compile diagnostics, hardware resource checks and preview.

**Primary surface:** Split node/source editing canvas and preview, with syntax-aware inspector and shader permutation/variant metrics.

**Docked panels and options:** Shader file tree, function library, code compiler output, reflection uniforms, graph pin types, performance costs.

**Core user actions:** Edit graph/source, validate types, compile SPIR-V, preview PBR result, change variants, link shader to material and debug errors.

**Authoritative data / file types:** ShaderSource, ShaderGraph, SPIRVModule, ShaderReflection, PipelineVariant and CompileDiagnostic.

**Connections and data flow:** P06 Material is the consumer; P31 renderer settings affect variants; P23 profiles shaders; P10 VFX consumes GPU shaders.

**Engine/backend services required:** glslang/shaderc or vetted compiler, SPIR-V validation/optimization, hot-reload staging, shader cache and pipeline reflection.

**Forge Intelligence integration:** Suggest graph changes and shaders with compile tests and performance estimates.

**Detailed acceptance scenario:** Edit material graph and verify shader compile. **Required evidence:** A shader can be edited and recompiled; if compilation fails, the previous valid shader remains active and errors point to nodes/lines.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-29 - Lighting Studio

**Navigation:** World > Lighting mode; separate asset tabs. **Delivery:** F1/F3 (progressive); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Set believable lighting and environment conditions with understandable trade-offs between quality and frame cost.

**Primary surface:** World viewport with light gizmos, exposure histogram, probes, shadow/volume overlays and before/after comparison.

**Docked panels and options:** Light list, sun/sky, HDRI, reflection probes, exposure, shadow cascades, atmosphere, fog, baked/real-time GI settings.

**Core user actions:** Create lights/probes, position light rigs, tune color/intensity, adjust exposure/volumetrics, preview GI and capture reference frame.

**Authoritative data / file types:** LightComponent, EnvironmentAsset, LightProbe, ExposureProfile, ShadowConfig, GISettings.

**Connections and data flow:** P02 live scene, P06 material preview, P12 cinematics and P31 renderer/scalability modes share lighting data.

**Engine/backend services required:** Lighting and shadow passes, tone mapping, HDR pipeline, probe baking/import, dynamic GI research for later renderer releases.

**Forge Intelligence integration:** Suggest lighting setups and quality/performance presets with side-by-side measured comparisons.

**Detailed acceptance scenario:** Add volumetric lighting without frame spikes. **Required evidence:** Light changes affect the same world used by Playtest; an unsupported GI mode gives capability guidance instead of a fake result.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-30 - Terrain & Procedural Worlds

**Navigation:** World > Terrain/Foliage mode. **Delivery:** F2/F3 (progressive); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Author large landscapes, biomes, roads, foliage and procedural distributions with scalable streaming and clear memory budgets.

**Primary surface:** Terrain viewport with paint/sculpt brushes, chunk grid, height/slope masks, biome preview and world-partition bounds.

**Docked panels and options:** Terrain layers, sculpt/erosion, procedural rules, roads/rivers, scatter graph, foliage types, chunk/streaming stats.

**Core user actions:** Sculpt heightfield, paint layers, scatter vegetation, bake masks, generate roads, stream tiles and inspect LOD.

**Authoritative data / file types:** TerrainAsset, Heightfield, LayerWeightmap, BiomeGraph, InstanceCluster, WorldTile and StreamConfig.

**Connections and data flow:** P02 scene contains terrain; P05 produces splat materials; P29 lighting and P35 nav mesh process world tiles; P23 profiles.

**Engine/backend services required:** Terrain meshing, virtual texturing later, instance rendering, world streaming, data layer systems and cook pipeline.

**Forge Intelligence integration:** Draft biome, road and spline systems constrained by terrain and streaming dependencies.

**Detailed acceptance scenario:** Generate terrain road and foliage layers. **Required evidence:** Terrain edits survive reload; only affected tiles rebuild; playable camera transitions between tiles without unacceptable hitching.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-31 - Render Settings / Graphics Quality

**Navigation:** World > Rendering/Project Settings, not always its own tab. **Delivery:** F1/F3 (progressive); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Expose renderer capabilities, quality budgets and platform fallbacks without expecting every developer to understand GPU internals.

**Primary surface:** Scalability profile editor with a scene preview, capability matrix, GPU feature warnings, quality-performance comparison.

**Docked panels and options:** API/backend selection, display resolution, dynamic resolution, anti-aliasing, shadows, GI/reflections, post effects, texture budgets.

**Core user actions:** Set quality tier, configure renderer flags, compare output, generate shader variants, apply default to project or platform.

**Authoritative data / file types:** GraphicsProfile, RenderFeatureSet, ShaderPermutationPolicy, QualityPreset and HardwareCapabilityReport.

**Connections and data flow:** P29 Lighting, P28 Shader, P23 Profiler and P18 platform builds consume these settings; P38 stores project defaults.

**Engine/backend services required:** RHI feature negotiation, renderer configuration, shader/pipeline variant cache and target compatibility validation.

**Forge Intelligence integration:** Recommend measured quality-tier settings, unsupported-feature fallback and capability tests.

**Detailed acceptance scenario:** Save portable graphics quality profile. **Required evidence:** Unsupported features are disabled with explanation; quality presets result in real measurable changes in frame cost and visual output.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-32 - Forge AI Creation Assistant

**Navigation:** Global AI utility pane; full plan/review editor on demand. **Delivery:** F2/F3 (progressive); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Help creators edit scenes, code, materials, rigs and test cases using real project context and explicitly approved operations.

**Primary surface:** Task conversation plus project/asset context picker, structured change plan, affected assets, diff preview, risks and approvals.

**Docked panels and options:** Assistant catalog, plan editor, permission scope, suggested tools, preview, tests, audit log, model/usage configuration.

**Core user actions:** Describe goal, scope selected assets, review proposed commands, edit plan, approve, apply, validate, undo and compare.

**Authoritative data / file types:** AIPrompt, ContextSnapshot, ToolSchema, ProposedCommandBatch, Diff, Approval, AuditEntry and CostBudget.

**Connections and data flow:** Every editor has exposed safe command APIs; P22 verifies outcomes; P39 logs execution; P24 tracks generated asset provenance.

**Engine/backend services required:** Model-provider adapter, constrained tools, structured JSON schema, command sandbox, validation/evaluation suite and opt-in networking.

**Forge Intelligence integration:** Forge Director planning, RAG context citations, tool approval, cost/permissions, audit and rollback.

**Detailed acceptance scenario:** Approve AI proposal and successfully roll back. **Required evidence:** A request to place three lights proposes exactly identified objects, shows a diff, applies only after approval and supports rollback.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-33 - Spatial Audio / 360 Sound

**Navigation:** Audio > Spatial mode (not separate permanent page). **Delivery:** F2/F3 (progressive); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Position and debug game sound in 3D space, from positional emitters to binaural rendering and ambisonic environments later.

**Primary surface:** 3D world preview with emitters, listener/head orientation, attenuation spheres, occlusion rays, room zones and live meters.

**Docked panels and options:** Emitter list, HRTF/binaural profile, channel/output layout, distance falloff, Doppler, reverb zones, obstruction and acoustic rays.

**Core user actions:** Place emitter, edit falloff/cones, listen from virtual player, toggle occlusion, assign room impulse and test headphones.

**Authoritative data / file types:** SpatialAudioEmitter, Listener, AttenuationCurve, AcousticZone, ReverbSend, AmbisonicAsset and OutputProfile.

**Connections and data flow:** P11 Audio supplies cues and buses; P02/world supplies geometry; P19 collision is optional occlusion source; P21 playtest listens.

**Engine/backend services required:** Audio spatializer, channel panning, HRTF/DSP integration, acoustic tracing/occlusion and real-time mixer performance budgets.

**Forge Intelligence integration:** Propose spatial placement, HRTF/occlusion/reverb zones with audible/technical validation.

**Detailed acceptance scenario:** Configure sound occlusion in rooms. **Required evidence:** A moving listener produces predictable positional changes; unsupported output formats fall back safely and report capabilities.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-34 - Simulation Studio (fluids, cloth, destruction)

**Navigation:** Effects/Simulation > Advanced Simulation modes. **Delivery:** F3 (advanced/AAA research); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Give artists controlled simulation and baking workflows without conflating fluid/fire/hair solvers with basic rigid-body collision.

**Primary surface:** Domain-specific scene viewport with solver states, caches, playback time, constraints and collision visualization.

**Docked panels and options:** Simulation domains, emitters, solvers, cache paths, bake queue, cloth/hair, volumetric smoke/fire and fracture controls.

**Core user actions:** Create fluid domain, configure cloth, set collision, run/bake cache, scrub, invalidate affected frames and export optimized runtime form.

**Authoritative data / file types:** SimulationGraph, SolverSettings, SimulationCache, BakeTask, DomainBounds, CollisionInputs and SourceAssetRefs.

**Connections and data flow:** P19 supplies rigid bodies, P10 VFX consumes baked effects, P12 cinematics previews simulations, P18 cooks runtime derivatives.

**Engine/backend services required:** Individual solver integrations/research, deterministic caching, frame asset storage, GPU compute jobs and cancellation/recovery.

**Forge Intelligence integration:** Assist simulations and caches with reproducible seeds, safety constraints and compute budgets.

**Detailed acceptance scenario:** Cache cloth/particle simulation with deterministic settings. **Required evidence:** Baking is resumable and reports memory/time; changing a source invalidates only dependent cache and never corrupts authoring data.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-35 - Navigation & NPC World Tools

**Navigation:** World > Navigation or Gameplay > NPC AI. **Delivery:** F2 (indie production); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Author pathfinding geometry, navigation zones and traversable links across the real scene.

**Primary surface:** Scene viewport with navmesh polygons, agent paths, obstacle volumes, heat maps and crowd visualizations.

**Docked panels and options:** Nav source list, bake settings, agent profiles, off-mesh links, obstacles, query debugger and world-tile cache.

**Core user actions:** Mark navigation geometry, bake/update mesh, define traversal costs, create links, query paths and visualize NPC state.

**Authoritative data / file types:** NavMeshAsset, AgentProfile, NavArea, OffMeshLink, NavBakeJob, PathQuery and NavTile.

**Connections and data flow:** World P02/P30 terrain is input; P15 NPC behavior queries paths; P21 debug and P22 test patrol/crowd scenarios.

**Engine/backend services required:** Navigation mesh generator/pathfinding runtime, tile cache, deterministic query debug and crowd update budget.

**Forge Intelligence integration:** Suggest navmesh/crowd tuning and inspect path failures with real simulation logs.

**Detailed acceptance scenario:** Fix a broken navigation route. **Required evidence:** A route updates after moving an obstacle or geometry; impossible destinations report failure rather than infinite agent loops.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-36 - Import & Export Pipeline

**Navigation:** File > Import; Asset Hub > Import/Export; task dock. **Delivery:** F1/F2 (progressive); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Convert external content into validated WorldForge assets while preserving editability, source provenance and error feedback.

**Primary surface:** Import wizard with source files, detected types, per-asset preview, options, dependency list, validation and queue progress.

**Docked panels and options:** Batch imports, glTF/GLB, images, WAV/OGG/other audio, unit conversion, coordinate orientation, licenses, export presets.

**Core user actions:** Drag source, configure scale/axes/compression/LOD, preview, validate, import, reimport and export supported interchange assets.

**Authoritative data / file types:** ImportJob, SourceAsset, ImportRecipe, ValidationResult, CookedDerivative, SourceLicense and FileHash.

**Connections and data flow:** P24 records imported assets; P03/P07/P09 receive content; P18 cooks; P25 stores source files/recipes.

**Engine/backend services required:** fastgltf, image/audio decoders, guarded file parsers, conversion jobs, async worker queue and progress/error reporting.

**Forge Intelligence integration:** Choose importer settings, report broken resources, propose conversions and verify source-to-cooked fidelity.

**Detailed acceptance scenario:** Reimport a modified GLB and update consumers. **Required evidence:** A valid Blender GLB imports with mesh/material hierarchy; bad paths or malformed source files fail safely with precise errors.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-37 - Plugin & Tools Manager

**Navigation:** Tools > Plugins; plugin details as document tabs. **Delivery:** F2/F3 (progressive); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Let third parties extend editor panels, importers, components, build steps and asset tools with permissions and compatibility rules.

**Primary surface:** Plugin catalog/list, compatibility matrix, details/permissions, installed/enabled state, error and dependency inspection.

**Docked panels and options:** Plugin SDK docs, module manifests, extension points, signing/trust, sandbox capabilities, update rollback and diagnostics.

**Core user actions:** Install approved package, inspect permissions, enable/disable, update, revert, report issue and open developer documentation.

**Authoritative data / file types:** PluginManifest, ExtensionPoint, CapabilityPermission, CompatibilityTarget, Signature and InstallRecord.

**Connections and data flow:** Extends editors P02-P36, contributes custom commands to P32 AI only after approval, and participates in P18 build packaging.

**Engine/backend services required:** Versioned C ABI or stable plugin boundary, loader, subprocess sandbox where feasible, signed package checks, crash isolation.

**Forge Intelligence integration:** Explain plugin permissions and compatibility; prohibit unreviewed installer or code execution.

**Detailed acceptance scenario:** Install a signed compatible extension under review. **Required evidence:** A faulty plugin does not corrupt a project or prevent safe-mode startup; incompatible versions are blocked with explanation.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-38 - Project & Engine Settings

**Navigation:** Project menu / Settings icon; category dialog. **Delivery:** F0/F1 (progressive); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Provide predictable configuration of project/runtime defaults, input, files, rendering, platforms, privacy and developer preferences.

**Primary surface:** Searchable settings navigator with per-category pages, modified-state markers, reset and validation hints.

**Docked panels and options:** Project identity, startup scene, input maps, frame target, physics timestep, graphics tier, asset paths, editor theme, plugins.

**Core user actions:** Change settings, compare against defaults, validate unsupported flags, save profile, restore defaults and export configuration.

**Authoritative data / file types:** ProjectSettings, UserPreferences, InputActions, PlatformProfile, PrivacyConsent and SettingsSchemaVersion.

**Connections and data flow:** Feeds all workspaces; P31 owns detailed rendering configuration; P18 consumes target settings; P40 seeds defaults.

**Engine/backend services required:** Versioned config schema, secure preference storage, constraints validator, configuration migration and platform capability checks.

**Forge Intelligence integration:** Explain settings impact and recommend safe profiles; never override permissions or platform gates.

**Detailed acceptance scenario:** Change render target settings safely. **Required evidence:** Settings persist after restart and invalid values are rejected or migrated without silent data loss; editor-only settings stay local.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-39 - Console & Diagnostics

**Navigation:** Bottom dock, command palette or Window > Console. **Delivery:** F0 (foundation); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Provide unified structured error reporting and safe diagnostics for engine, renderer, scripts, imports, AI, tests and builds.

**Primary surface:** Compact bottom-dock log with filter levels, search, timestamps, tags and source links; can expand into a large diagnostics view.

**Docked panels and options:** Output, warnings, errors, tasks, shader compile, asset import, runtime traces, build logs, crash records and reproduction links.

**Core user actions:** Filter/search/copy/export logs, open source location, suppress known issue temporarily, attach report and clear local buffer.

**Authoritative data / file types:** LogEvent, ErrorCode, Severity, SourceLocation, TaskID, SessionID, DiagnosticBundle and TraceID.

**Connections and data flow:** Receives events from all modules; selecting an error opens the correct code P13, shader P28, build P18 or asset P24.

**Engine/backend services required:** Structured logger, bounded queues, redaction of secrets, crash dump upload opt-in, correlation IDs and portable symbols.

**Forge Intelligence integration:** Classify logs/diagnostics and suggest reproduction steps with links to relevant assets and code.

**Detailed acceptance scenario:** Resolve asset/import runtime error from log. **Required evidence:** A failed shader compile produces one searchable error linked to its originating shader; log collection never leaks credentials.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

### WF-40 - Project Creation Wizard

**Navigation:** File > New Project from P01; modal wizard. **Delivery:** F0 (foundation); the latest detailed status must be tracked in an issue/feature register.

**User purpose:** Create a valid, reproducible project from a small set of templates without requiring knowledge of engine internals.

**Primary surface:** Step-by-step dialog with template preview, project name/path, supported platform, renderer preset, sample content and summary.

**Docked panels and options:** Blank 3D, third-person starter, small simulation sample, future 2D/VR templates; toolchain validation and disk requirements.

**Core user actions:** Choose template, select path/name, set startup scene and quality preset, validate writable location, create and open project.

**Authoritative data / file types:** TemplateManifest, ProjectManifest, ToolchainRequirement, SceneBootstrap, ProjectSettings and CreationReport.

**Connections and data flow:** Invoked from P01; results open P02 and initialize P24 asset registry, P25 Git option and P38 settings.

**Engine/backend services required:** Template expansion, file atomicity, path security, project migration detection, source-control initialization and rollback on error.

**Forge Intelligence integration:** Recommend templates based on project goals and hardware; preview generated project requirements.

**Detailed acceptance scenario:** Create a project from a template and reopen it. **Required evidence:** A Blank 3D project is created with stable IDs and sample scene, then opens with no missing dependencies; failure leaves no half-project.

**Failure and safety conditions:** Missing, stale or incompatible assets must produce a recoverable error; data-affecting operations use a versioned command transaction; slow operations are cancellable where practical. AI cannot directly bypass editor validation. All changes that affect cooking/build output must invalidate the right dependent artifacts.

**Design review note:** The mock images sometimes show unsupported tools, unrealistic frame-rate indicators and inconsistent technical labels. Use the screen requirements, actual capabilities and approved layout shell as authority; the mockups are only appearance references.

# PART VI - Cross-workspace production pipelines

## 13. End-to-end workflows that must work across tools

### 13.1 From concept to production-ready character

**Input:** approved visual reference, scale/unit convention, geometry budget, LOD profiles, target platform, rights metadata. **Workflow:** Character Studio drafts design -> Modeling/Sculpting creates editable source geometry -> Texture Studio paints layered PBR maps -> Material Studio compiles material variants -> Rigging creates skeleton, skin weights and IK -> Animation defines locomotion/actions/root motion -> Code/Visual Scripting attaches gameplay behavior -> Audio adds timed sound events -> World places instances -> Testing validates deformation, collisions and input -> Build cooks the character and dependencies. **Critical invariants:** one character GUID; cross-workspace edits invalidate dependent LOD/collision/animation data; every generated variant has provenance; an engine restart does not lose edits. **End-to-end test:** change hand proportions and verify retarget/attachment diagnostics, not silent broken weapon placement.

### 13.2 From procedural terrain to playable streamed world

**Input:** world size, biome rules, climate, stream-cell plan, target frame/IO budgets. **Workflow:** World/Terrain builds layered height fields and roads -> Modeling supplies modular environment sets -> Materials supplies shader instances -> Lighting configures sky/weather -> Navigation builds walkable data -> Physics generates colliders -> Asset Cooker packages cell dependencies -> Player traverses cells -> Profiler inspects streaming stalls -> Testing checks teleport/high-speed traversal. **Invariants:** streaming does not lose persistent mission state; cell boundaries cannot break roads/terrain continuity; editor and runtime use compatible world manifest versions.

### 13.3 From mission specification to tested gameplay

**Input:** mission graph, success/failure criteria, references to vehicle/NPC assets. **Workflow:** Gameplay Code or Graph configures state machine -> NPC AI defines behaviors -> World inserts triggers -> UI Designer adds objective and failure states -> Audio/Cinematic add cues -> Playtest records mission route -> QA runs auto checks -> Build validates. **Tests:** initial spawn, objective trigger, player death/restart, abandon/reload, asset missing, networking when applicable. **AI option:** Forge Director proposes changes only through commands and provides a before/after test report.

### 13.4 From VFX to simulation and cinematic

**Input:** scene timing, effect references, compute budget and hardware targets. **Workflow:** VFX graph emits particles/lights -> Materials defines shader variants -> Physics/Simulation defines rigid bodies/cloth/volumes -> Audio ties events to impacts -> Cinematic Sequencer aligns cuts/time curves -> World embeds effect -> Profiler measures pass cost -> Build cooks caches. **Failure tests:** shader compilation error, missing texture, GPU device without feature, deterministic replay mismatch and poor frame budget.

### 13.5 From UI design to accessible game flow

**Input:** game UI style guide, target devices and localization strings. **Workflow:** UI Designer creates editable constraints/widgets -> Animation adds transitions -> Gameplay binds typed state values -> UI Testing checks focus, input and safe areas -> Playtest captures live behavior -> Testing verifies accessibility assertions -> Build packages fonts/assets with legal notices. **Tests:** 1080p/4K, gamepad, keyboard only, missing localization, oversized user fonts, reduced motion where supported.

### 13.6 From AI proposal to verified change

**Input:** user goal and selected entities. **Workflow:** permission-aware retrieval -> exact WorldGraph query -> deterministic preconditions -> structured plan -> cost/impact preview -> explicit approval -> transaction execution -> incremental import/compile/test -> measurable result summary -> commit/undo. **Special cases:** project revision conflict; source/model license unknown; external service outage; model proposed a non-existent component; dangerous script; token/cost cap reached. No tool silently escapes permission scope.

### 13.7 From team revision to packaged release

**Input:** reviewed source revision/branch, approved assets, build profile and release destination. **Workflow:** Source Control validates branch -> Build runs schema/importer and cooker checks -> tests and GPU smoke -> dependency and license manifest -> signed/hashed package -> Publishing performs store/platform-specific validation -> human approves distribution. **No source assets or secrets should be copied into public distributions unintentionally.** A released executable runs without the editor; debug builds/symbols are kept separate from shipping builds.

### 13.8 Crash/recovery across all workspaces

If the editor fails during AI generation, mesh import or large-world autosave, the creator must be able to reopen the project from last verified revision, inspect a recovery journal and choose safe recovery. Save with write-temp/fsync/atomic rename or an equivalent transaction strategy; use a separate recovery path and never call a partial write successful. QA must simulate power loss, read-only storage, low disk space, corrupted cached asset and interrupted network/tool operation.

---

# PART VII - Delivery, tests and production governance

## 14. Roadmap and delivery gates (F0-F5)

The full vision is company-scale and cannot be realized by a beginner in a few months. The roadmap is **dependency-based rather than fixed launch promises**. Phases can overlap for a capable team, but no phase can claim production readiness without evidence.

| Phase | Scope | Exit evidence |
|:--|:--|:--|
| F0 - First Light | CMake repo, C++ EngineCore, Qt shell, Vulkan viewport, cube camera, ECS IDs, Inspector/Outliner, save/load/undo | Another developer can build/open/edit/save/reopen scene; no critical crashes |
| F1 - Real 3D authoring | GLB importer, materials, cameras/lights, source asset registry, scene templates, first standalone Player, basic profiler | Playable small demo; correct source and cooked references |
| F2 - Indie production | Core physics, skeletal animation, basic audio, gameplay systems, UI and Windows packaging, first AI tool plans | One original small game created/tested/shipped using WorldForge tooling |
| F3 - Integrated creators | Basic native modeling/sculpt/texturing, VFX and cinematics, plugin SDK, better AI RAG/asset generation, Linux/macOS qualification | External creators complete projects with fewer broken handoffs |
| F4 - Large-world beta | Streaming/HLOD, navigation/traffic, crowds, advanced rendering/lighting, playable Living City district, team collaboration | Stable long-duration traversal/mission/publishing benchmarks |
| F5 - AAA research platform | Advanced path/ray tracing, high-end characters/simulation, large populations, mature AI orchestration/QA, broader target exports | Independent studio trials and documented quality/performance/reliability comparison |

### 14.1 First eight engineering milestones, precise acceptance

**M0 - Repo and toolchain:** CMake presets, C++ engine library, editor target, CI sample, pre-commit formatter, a simple log/test. Gate: clean build from a documented workstation and CI; no missing manual dependency steps.

**M1 - Native shell:** Qt main window, theme tokens, menus, tiny icon rail, editor docks, layout restore, one-line status bar, keyboard shortcuts. Gate: 1080p/4K scaling, minimum window sizes, restore/rearrange and accessibility smoke test pass.

**M2 - Vulkan first image:** Render triangle then lit cube with camera, resize and swapchain/device error paths. Gate: validation layers report no blocking lifetime/synchronization faults in standardized smoke scene.

**M3 - Scene entities:** GUIDs, scene model, transform, Camera, Light, mesh placeholder; Outliner + Inspector selection synchronization; picking. Gate: Add/Select/Move/Duplicate/Delete/Undo actions update views identically.

**M4 - Persistent document:** project schema, scene files, atomic save and migrations; one source asset. Gate: two clean reopen cycles preserve transforms, hierarchy, revisions and asset refs; crash injection recovers.

**M5 - Asset pipeline:** GLB mesh/material import, content hash, dependency graph, import diagnostics, basic material/texture. Gate: Blender-created original asset imports and reimports with correct scene references.

**M6 - Player and physics:** minimal standalone runtime executable plus first Jolt rigid body. Gate: sample scene loads on a clean compatible PC without Qt editor; box/floor interaction is repeatable within specified tolerance.

**M7 - Shippable prototype:** Windows package, tests, docs, known issues, SBOM/license inventory, a small complete reference game and contributor instructions. Gate: outside developer builds, plays, edits and packages sample unaided.

### 14.2 First 90 days, founder-friendly interpretation

The practical goal for a founder employed part-time is **M0-M2 or the beginning of M3**, depending on existing C++/graphics experience. Week 1-2: install toolchain, initialize repository, build Qt app and unit test. Week 3-4: build consistent desktop shell; preserve layout. Weeks 5-8: initialize Vulkan surface, validation, basic camera and cube. Weeks 9-12: entity GUIDs, Inspector/Outliner and save/undo prototype as capability allows. If the Vulkan integration takes longer, keep quality, do not claim completion or substitute a static image for a real renderer. Prioritize actual build reports over arbitrary dates.

### 14.3 Development epics and work packages

| Epic ID | Work package | Dependencies | Expected artifacts |
|:--|:--|:--|:--|
| EP-CORE | ID types, ECS wrappers, events, jobs, errors, serialization | M0 | EngineCore library + unit tests |
| EP-EDITOR | Native shell, theming, docks, selection, command palette | EP-CORE | Editor app, component library, UX tests |
| EP-RHI | Render device, Vulkan backend, resources, shaders, frames | EP-CORE | Render smoke/golden-image tests |
| EP-ASSET | Registry, importer, hashes, graph, cook and reimport | EP-CORE, EP-RHI | GLB pipeline + test data |
| EP-SCENE | Component types, scene documents, spatial hierarchies | EP-CORE, EP-RHI | Save/restore and entity editing |
| EP-PHYS | Jolt adapter, colliders, transforms, simulation runtime | EP-SCENE | Falling-box test |
| EP-ANIM | Skeleton, skinning, clips, retarget, blend tree | EP-ASSET, EP-SCENE | Playable animated character |
| EP-AUDIO | Audio graph, sound buses, events and spatial emitters | EP-SCENE | Sound-in-world demo |
| EP-PLAY | Gameplay components, input, runtime executable | EP-SCENE, EP-PHYS | Standalone demo |
| EP-AI | Tool gateway, retrieval, consent, audit, validators | EP-CORE, EP-ASSET, EP-EDITOR | Safe scene-edit AI demo |
| EP-WORLD | Terrain, spatial cells, streaming and HLOD | EP-RHI, EP-ASSET, EP-SCENE | Traversable streamed district |
| EP-TRAFFIC | Lanes, pedestrian AI, vehicles and schedules | EP-WORLD, EP-PHYS, EP-PLAY | Living City slice |
| EP-BUILD | Cooker, CLI, packaging, platform checks | EP-ASSET, EP-PLAY | Repeatable Windows build |
| EP-TEAM | Versioning UI, locking, review, collaboration | EP-ASSET | Multiuser review trial |

## 15. Quality engineering plan and test matrix

### 15.1 Test pyramid

**Unit:** type serialization, math, IDs, asset registry, invalidation, data migration, command preconditions, AI schema validation.  
**Component:** GLB importer, shader compile, device feature negotiation, physics integration, audio output, runtime scripting, RAG retrieval and permissions.  
**Integration:** selected entity ID across panels; scene save/load; material changes and cook; script compile and gameplay; AI apply+undo; build and standalone player.  
**System:** large asset set, offline/no GPU features, long playtest, high-speed streaming, recovery, multi-monitor UI, packaged game.  
**User acceptance:** external creator completes a reference feature and reports defects; time-on-task and subjective quality recorded.

### 15.2 Release quality gates

| Gate | Required evidence |
|:--|:--|
| No data loss | Transaction, forced-crash and project migration test logs |
| Renderer reliability | Vulkan validation, GPU golden tests, device reset, shader failure handling |
| Asset integrity | GUID uniqueness, dangling-reference report, license/provenance ledger |
| Gameplay correctness | Mission/test case assertion suite, saved-game compatibility |
| AI safety | Tool permission tests, prompt injection suite, rollback, cost caps |
| Performance | Named GPU/CPU profile, frame capture, 1% low, memory, scene complexity |
| Accessibility | Keyboard-only workflows, focus and labeling, contrast, scaling |
| Security | Importer fuzzing, plugin isolation, signed binaries, dependency scan |
| Packaging | Player runs without editor, manifest/reproducible build audit |
| Platform | Real target-OS hardware builds, not only successful cross-compilation |

### 15.3 Performance observability

Record per-frame CPU game/render/UI times, GPU passes, draw/dispatch counts, shader pipeline hitches, upload stalls, physical/logical memory, VRAM resident/pending, audio voice count/underruns, physics active bodies/step time, network latency/loss, simulation tiers, world-cell loads/evictions and AI inference duration/cost. Profiling must include source asset IDs and revision links to make the optimization actionable. Avoid an always-green 'All systems operational' status borrowed from concept art unless the real diagnostics justify it.

### 15.4 Cross-platform qualification

| Platform | First qualification | Advanced qualification |
|:--|:--|:--|
| Windows | Editor/app rendering, file IO, keyboard/mouse, Player packaging | High-DPI mixed monitors, multiple GPU vendors, signing |
| Linux | Vulkan windowing, file IO, Qt/Wayland/X11, audio | Multiple drivers/distributions, packaging and input |
| macOS | Supported CPU/GPU capability path, Qt docks, app bundle | Metal/Vulkan portability route, signing/notarization, Apple Silicon |
| Game export targets | Windows Player first | macOS/Linux; mobile/web/consoles only after dedicated engineering/SDK approvals |

### 15.5 Definition of 'production-ready'

A feature is production-ready only when: critical user journeys pass; editor interactions are undoable where appropriate; data is migration-safe; affected assets recook correctly; warnings are actionable; documentation and example assets exist; test coverage and performance profile meet the supported scope; security and licensing are reviewed; and external creators can use it without developer hand-holding. A concept image or working local demo alone does not meet this bar.

## 16. Security, privacy, safety and intellectual property

**Native plugins:** untrusted plugins need permission management, provenance and version compatibility. Native in-process code has broad powers; for untrusted components use a separate process or avoid installation. Signed packages do not automatically imply safety.  
**AI tooling:** retrieval context can contain adversarial instructions; tool calls must enforce permissions and argument schema outside the model. Code or binaries proposed by AI are treated as untrusted until reviewed and sandbox tested. Logs, API keys and private source code must not be transmitted without explicit user policy.  
**Networking:** server authority for competitive mechanics, input validation, encryption/transport protections as applicable, anti-cheat decisions, save-data and account security, privacy-law compliance.  
**Asset rights:** track third-party licenses for meshes, scanned content, music, fonts, model weights, generated media and reference input. Record rights/use restrictions in asset metadata.  
**Brand/IP:** WorldForge remains a codename pending clearance. 'Niagara-like' describes a visual-effect concept; do not ship competitor brands or copy their source code, UI assets or internal tools. GTA VI and future hypothetical GTA 10 are **quality and scope references**, not assets or IP to recreate.  
**Qt license:** select and audit LGPLv3 or commercial terms; check module-level licensing and relevant dynamic linking/redistribution obligations with qualified counsel before commercial release.  
**Platform approvals:** console and store SDKs, age ratings, privacy declarations, cryptographic signing and distribution policies require independent agreement and validation.

## 17. Team, funding and operating model

### 17.1 Team evolution

**F0-F1:** founder/technical lead, senior C++/Vulkan advisor (if available), UI engineer and technical artist as contractors/collaborators; part-time QA/review. **F2-F3:** engine generalists, graphics specialist, tools/technical artist, runtime/gameplay/animation engineer, QA/build engineer and part-time ML integration owner. **F4-F5:** streaming/world systems, traffic/NPC simulation, rendering research, animation/DCC, physics/vehicles, online/networking, security/infrastructure, developer relations, business/legal and customer support. Each subsystem requires an accountable technical owner and test harness; more freelancers producing independent screens is not a substitute for architectural leadership.

### 17.2 Capital planning and realism

Free software and an existing computer can support **M0-M2**. The major costs are developer time, high-end GPU hardware and testing, contractors, purchased/licensed assets, model inference/GPU services, CI/device labs, security/legal work and later salaries. Full AAA parity is a multi-year, well-funded, multidisciplinary program—not a ₹10,000-₹1 lakh software build. Spend early money on validated technical milestones and creator research; keep a personal financial buffer and employment income. Paid cloud AI should use quotas and cost visibility from the first commercial feature.

### 17.3 Operating cadence

Weekly engineering demos with a real executable; prioritized backlog with impact/dependency tags; architecture decision review; monthly defect and benchmark reports; quarterly creator usability tests. Every feature should record its maturity level: Concept -> Prototype -> Internal Alpha -> Public Beta -> Production-Ready. Do not inflate version labels or test metrics from images into shipping claims.

## 18. Risk register and mitigation

| Risk | Severity | Trigger and mitigation |
|:--|:--|:--|
| Overbroad launch scope | Critical | Milestone slips: ship vertical slices, defer advanced suites |
| Vulkan editor integration | High | Unstable embedding/focus/overlays: prototype window ownership early |
| Asset/data corruption | Critical | Failed save/undo/migration: transaction journal and recovery tests |
| Insufficient renderer competitiveness | High | Poor quality/performance: measure reference scenes, focus on one research advantage |
| AI performs wrong edits | Critical | Wrong target, stale state: exact GUID checks, preview, revision guard, rollback |
| AI model cost/latency | High | Unexpected expense: task budgets, local provider alternatives, cached retrieval |
| Unlicensed assets/models | High | Provenance unknown: quarantine/deny export pending rights review |
| Large-world streaming hitches | High | Frame spikes during drive: async prefetch, HLOD, budget tuning |
| NPC/crowd CPU overload | High | High active AI count: simulation LOD, deterministic profiles |
| Mac/Linux support gaps | High | Build-only without test: qualify platforms on real machines |
| Qt/plugin license conflict | High | Commercial release planning: license inventory and legal review |
| Market moves rapidly | High | Competing AI/editor features: focus on verified workflow improvements |
| Solo founder burnout | High | Too many parallel epics: 12h/week sustainable schedule, demonstrate basics |



# PART VIII - Implementation assets and engineering handoff

## 19. Repository baseline and build/release structure

```text
WorldForge/
  CMakeLists.txt                 # superbuild entry
  CMakePresets.json             # Debug/RelWithDebInfo/Release
  vcpkg.json                    # versioned dependency manifest
  LICENSES/ THIRD_PARTY_NOTICES.md
  Docs/
    Product/                    # scope, 40-screen map, user stories
    ADR/                        # architecture decisions and evidence
    API/                        # C++ commands, components, AI schemas
    UX/                         # design tokens, icons, accessibility
    Benchmarks/                 # hardware, scenarios, results
  Engine/
    Core/                       # IDs, errors, event bus, jobs, math
    Scene/                      # ECS, hierarchy, components, layers
    Assets/                     # GUID registry, dependencies, cooking
    Renderer/
      RHI/                      # device/resource/graph abstractions
      Vulkan/                   # first backend
      Shaders/                  # source, compiler, reflection
    Physics/                    # Jolt adapter
    Animation/                  # rigs, skeleton, timeline, retarget
    Audio/                      # DSP/playback/spatial abstractions
    Gameplay/                   # input, scripting, quests
    Networking/                 # transport, replication, servers
    WorldStream/                # cells, HLOD, streaming IO
    AI/                         # runtime NPC AI (not Forge creator AI)
  Studio/
    App/ Theme/ Shell/          # Qt widgets, native menus, docking
    Services/                   # query/commands/undo/selection
    Editors/                    # mode implementations
    ForgeAI/                    # broker client, plan/approval UI
  Player/                       # game executable (NO Qt)
  BuildTools/                   # import, validate, cook, package
  AIWorkers/                    # optional isolated process/service
  Tests/                        # unit, contract, image, smoke, fuzz
  Samples/
    FirstLight/
    LostTemple/
    LivingCity/
  Tools/                        # codegen, platform packaging
  CI/                           # build matrices and artifact signing
```

### 19.1 CMake target/dependency rules

- `WorldForgeCore` cannot depend on `Qt::Widgets`, network API keys or Studio paths.
- `WorldForgeRenderVulkan` depends on `WorldForgeRHI`; engine-side renderers use generic handles.
- `WorldForgeEditor` links Qt Widgets and EngineCore/EditorServices; UI classes stay inside `Studio/`.
- `WorldForgePlayer` links EngineCore+runtime only; CI scans transitive libraries for forbidden editor dependencies.
- `WorldForgeBuild` is runnable on a headless test runner with no GPU for non-render cooking where supported.
- Every external dependency has a pin/lock and license metadata; the build records a bill of materials.

### 19.2 Minimal developer command flow (illustrative)

```bash
# Adapt vcpkg toolchain paths and Qt installation to the local machine.
cmake --preset windows-debug
cmake --build --preset windows-debug --target WorldForgeEditor
ctest --preset windows-debug --output-on-failure
# Later: WorldForgeBuild validate Samples/FirstLight/FirstLight.wfproject
```

Do not advertise these commands as already runnable; the repository targets/presets have not yet been implemented. The sample names specify the intended public developer experience.

## 20. Public contracts, API versioning and capability manifest

### 20.1 Stable API design principles

Public API versioning follows explicit compatibility rules: adding an optional field is generally additive; renaming component types, changing units/coordinate conventions, modifying the meaning of an asset type or removing a command requires migration and compatibility planning. Editor UI must not assume internal array order for entity IDs. Plugins declare required engine/API version, supported platforms and permissions. Keep serialized data separate from runtime-only pointers.

### 20.2 Engine capability manifest (example)

```json
{
  "engine": "WorldForge",
  "schema_version": 1,
  "build_id": "development-unverified",
  "features": {
    "renderer.vulkan": "prototype",
    "scene.serialization": "prototype",
    "asset.gltf_import": "not_implemented",
    "physics.rigid_body": "not_implemented",
    "ai.tool_gateway": "not_implemented",
    "world.streaming": "research"
  },
  "platforms": {
    "windows": "targeted_first",
    "linux": "planned_qualification",
    "macos": "planned_qualification"
  }
}
```

Screens consume this manifest to disable or explain unimplemented controls. This avoids a visual concept displaying 'All Systems Operational' or an imaginary engine version. Each feature's status is tested and signed off per build.

### 20.3 Editor command/event interface (illustrative C++)

```cpp
struct EditContext {
    ProjectId project;
    Revision expected_revision;
    UserOrAgentId actor;
    CapabilitySet permissions;
};

struct EditResult {
    bool success;
    Revision resulting_revision;
    std::vector<AssetId> affected_assets;
    std::vector<Diagnostic> diagnostics;
};

class IEditorCommand {
public:
    virtual ~IEditorCommand() = default;
    virtual ValidationResult validate(const ProjectSnapshot&) const = 0;
    virtual PreviewResult preview(const ProjectSnapshot&) const = 0;
    virtual EditResult apply(ProjectTransaction&, const EditContext&) = 0;
};
```

This pseudo-header shows intended relationships; it is **not compiled code** because types, ownership and ABI boundaries need actual definitions. The implementation must provide transactional behavior, thread-safe application, revision checks and audit records.

## 21. Architecture decision record register

| ADR | Proposed decision | Revisit trigger |
|:--|:--|:--|
| ADR-001 | Independent C++20 engine/runtime and Qt 6 desktop shell | UI integration/license tests fail or target changes |
| ADR-002 | Vulkan-first RHI; no Vulkan in EngineCore | Metal/Apple qualification and platform feature gaps |
| ADR-003 | EnTT runtime ECS + separate authoring hierarchy | Scale, reflection or serialization problems |
| ADR-004 | Stable GUID assets and versioned source->cooked pipeline | Team storage and large-scale project constraints |
| ADR-005 | Transactional commands, revisions, undo/recovery | Multiuser edit semantics and editor scale |
| ADR-006 | C++ runtime first; C# gameplay support deferred | Mature component APIs and creator demand |
| ADR-007 | Jolt/miniaudio as initial subsystem candidates | Proven missing needs, licensing or benchmark gaps |
| ADR-008 | AI model-agnostic broker with permissioned tools | Reliability and cost evaluation results |
| ADR-009 | RAG uses exact metadata+graph before vector similarity | Retrieval benchmark justifies changes |
| ADR-010 | Local-first workflow with optional cloud | Collaboration/service user needs |
| ADR-011 | 40 WF requirements within 12 desktop groups | Usability research contradicts navigation |
| ADR-012 | Windows first, macOS/Linux qualify separately | Shipping platform strategy changes |
| ADR-013 | 'Living City' is original IP and benchmark reference | Product positioning or licensing changes |
| ADR-014 | 4K photorealism framed as hardware-specific goal | Real performance/quality results |
| ADR-015 | AI approvals, audit and rollback for writes | Product security assessment and user trust |

Each ADR should record context, alternatives, consequences, owner, date, links to tests and the decision status (proposed/accepted/superseded). This table contains **proposed** decisions; sign-off happens in the actual repository.

## 22. Backlog starter: prioritized work items

| Ticket | Short description | Dependency | Done when |
|:--|:--|:--|:--|
| WF-ENG-001 | Bootstrap CMake + vcpkg + CI | None | Clean local and CI C++ build |
| WF-ENG-002 | Create Qt main window and design tokens | 001 | Docks work, layout persists |
| WF-ENG-003 | Vulkan instance/window/swapchain | 001,002 | Triangle and resize test |
| WF-ENG-004 | Entity GUID and scene/component model | 001 | Unit tests and hierarchy operations |
| WF-ENG-005 | Inspector/Outliner selection binding | 002,004 | Shared selected object across panels |
| WF-ENG-006 | Command bus + atomic undo/save | 004,005 | Reopen/save/recovery tests |
| WF-ENG-007 | Resource handles + basic cube/material | 003,004 | Editable rendered cube |
| WF-ENG-008 | GLB importer and asset registry | 006,007 | Blender file import + reimport |
| WF-ENG-009 | Standalone Player | 007,008 | Runtime works without editor |
| WF-ENG-010 | Jolt demo and basic gameplay | 009 | Gravity floor test |
| WF-AI-001 | Read-only project query API | 004,008 | Exact-ID safe query results |
| WF-AI-002 | Typed command schema + approval UI | 006,AI-001 | AI staged edit+undo works |
| WF-AI-003 | SQLite FTS5 indexing | 008,AI-001 | Correct retrieval on known test corpus |
| WF-AI-004 | Provider interface and model adapter | AI-002,AI-003 | One narrow task completed under cost/permission bounds |
| WF-LC-001 | Road/terrain source format | 008 | Basic editable block with road graph |
| WF-LC-002 | First car/mission demo | 010,LC-001 | Complete LC-01 gameplay route |

### 22.1 Definition of ready for implementation

A ticket is 'ready' only when its owning module, related WF screen ID, user goal, success/failure state, data/command contracts, test specification, example asset and integration dependency are identified. A beautiful screenshot alone does not satisfy ready-for-engineering.

## 23. Handoff checklist for another AI or engineering team

**Assume:** independent C++20 engine, Qt native editor, Vulkan-first rendering, single source of truth, 40 screen requirements consolidated into 12 groups, compact navigation/status line, AI permission gateway, local-first project and original Living City benchmark. **Do not assume:** complete engine exists, visuals are real live performance, model generation is production-ready, GTA-scale work is easy, or platform SDKs/licenses have been approved.

Before working on a feature:

1. Read product charter and architecture decisions; find the exact WF screen IDs and the canonical data types.
2. Identify code owners, project revision and supported platform capability.
3. Write the small end-to-end test first; define error and undo states.
4. Reuse common Qt controls, editor command bus, renderer and asset handles.
5. Keep engine/player/build CLI independent of Qt. Avoid embedding arbitrary scripts in the editor process.
6. If using AI, retrieve exact asset IDs, validate permissions and edits, show an approval plan, test and log results.
7. Record measured frame/time/memory impacts on named hardware when performance changes.
8. Update file schemas, docs, sample projects, license manifest and ADRs; attach executable evidence before claiming implementation.

**No AI may fabricate 'test passed', '4K at 60 FPS', 'production ready', 'shipped', asset licenses or market superiority**. If a tool or test was not run, state that explicitly.

## 24. Glossary

| Term | Definition |
|:--|:--|
| WorldForge Studio | Native desktop creation app with docked editing workspaces |
| WorldForge Engine | Independent C++ game runtime and shared subsystems |
| Player / Build | Standalone game execution / headless import-cook-package tools |
| WorldGraph | Canonical dependency and relationship model for scenes and assets |
| Forge Intelligence | AI retrieval, planning, generation, tool calls and validation subsystem |
| RAG | Retrieval-augmented generation; scoped project context selection |
| Vector embedding | Numerical representation for semantic similarity; not exact identity |
| Agent | Planner/controller with bounded tools, state and execution limits |
| Typed command | Versioned validated editor operation with declared effects |
| HLOD | Hierarchical level of detail, replacing detail of distant scenes |
| WorldStream | Large-world cell streaming, persistence and simulation tiers |
| PBR | Physically based rendering/material workflow |
| RHI | Rendering hardware abstraction over graphics APIs |
| glTF/GLB | Interchange/runtime-oriented 3D format, not full DCC authoring format |
| OpenUSD | Layered scene composition/interchange technology to evaluate |
| VRAM | Graphics memory; resident and budgeted usage matters |
| Frame pacing | Consistency of intervals between displayed frames |
| AI provenance | Model/input/version/license and tool history of generated assets |
| ADR | Architecture Decision Record |
| F0-F5 | Implementation phases from First Light to AAA research platform |
| LC-00..05 | Living City reference game/benchmark maturity levels |

## 25. Official references and licensing checkpoints

Sources below support *technical feasibility* and standards; they do **not** prove that WorldForge has implemented these features. Reverify versions, module licensing and platform availability when work begins.

1. Qt QMainWindow / QDockWidget, saved workspace layouts: https://doc.qt.io/qt-6/qmainwindow.html
2. Qt licensing and module obligations: https://doc.qt.io/qt-6/licensing.html
3. Microsoft vcpkg manifest mode: https://learn.microsoft.com/en-us/vcpkg/concepts/manifest-mode
4. Khronos glTF runtime/interchange standard: https://www.khronos.org/gltf/
5. glTF authoring vs delivery design goals: https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html
6. Vulkan ray tracing architecture: https://docs.vulkan.org/guide/latest/extensions/ray_tracing.html
7. Vulkan core/supported specification: https://registry.khronos.org/vulkan/
8. Jolt Physics (code and licensing): https://github.com/jrouwe/JoltPhysics
9. EnTT documentation: https://github.com/skypjack/entt
10. GLM documentation: https://github.com/g-truc/glm
11. Vulkan Memory Allocator: https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
12. fastgltf: https://github.com/spnda/fastgltf
13. miniaudio: https://github.com/mackron/miniaudio
14. SQLite FTS5: https://www.sqlite.org/fts5.html
15. Qdrant hybrid retrieval: https://qdrant.tech/documentation/search-tuning/hybrid-search/
16. ONNX Runtime execution providers: https://onnxruntime.ai/docs/execution-providers/
17. NVIDIA OpenUSD learning guide, layers: https://docs.nvidia.com/learn-openusd/latest/composition-basics/layers.html
18. Epic Unreal World Partition technical reference (comparative research only): https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition-in-unreal-engine
19. OWASP Prompt Injection Prevention: https://cheatsheetseries.owasp.org/cheatsheets/LLM_Prompt_Injection_Prevention_Cheat_Sheet.html
20. NIST Generative AI Risk Management Profile: https://www.nist.gov/publications/artificial-intelligence-risk-management-framework-generative-artificial-intelligence
21. CMake documentation: https://cmake.org/cmake/help/latest/
22. Khronos Vulkan SDK resources: https://vulkan.lunarg.com/
23. Git LFS: https://git-lfs.com/
24. Blender licensing FAQ (for optional integrations): https://www.blender.org/about/license/

## 26. Immediate next engineering artifact

**Implement the native WorldForgeEditor shell and `FirstLight` sample:** compiling Qt menu/dock shell; Vulkan cube viewport; IDs; Outliner/Inspector; reliable Save/Undo/Redo; startup/reopen tests. Then establish a safe Forge AI demo that can *only* inspect the scene and propose one reversible, validated `entity.transform.set` command. This provides a real engine/editor/AI connection from day one without pretending the advanced systems exist.

---

**END OF MASTER SPECIFICATION - REVISION 3.0**  
This specification is a proposal for a future production platform. Keep a release capability matrix and replace these statements with real test evidence as features are implemented.

