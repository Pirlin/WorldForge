# WorldForge Engineering Handoff - v3.0

**Document date:** 10 November 2026  
**Status:** Proposed specification; not implemented product capabilities

## Read in this order

1. `WorldForge_Master_Blueprint_v3.pdf` - formatted 54-page engineering/product document.
2. `WorldForge_Master_Blueprint_v3.md` - authoritative editable source with all tables, URLs and complete technical detail.
3. `WorldForge_Master_Blueprint_v3.docx` - editable Word copy.
4. `WorldForge_40_Screen_Specifications_v3.json` - machine-readable description of all 40 editor screens: purpose, surface, panels, commands, data, cross-workspace links, backend, AI, acceptance.
5. `WorldForge_Engineering_Backlog_v3.csv` - ordered technical tickets for the first real engine milestones.
6. `WorldForge_Capability_Matrix_v3.json` - intended product stage definitions and sample release capabilities.
7. `Approved_UI_Shell_Reference.png` - existing concept image for compact native navigation and bottom single-line status strip; it is not a running engine capture.

## Critical product truths

- WorldForge is an **independent C++20 engine** with **Qt 6 Widgets native editor** and **Vulkan-first rendering**. The game player and headless build tool are separate executables without a Qt editor requirement.
- C++20 and GPU shaders are the initial language stack. Future C# for creator scripting is optional and deferred; Python may assist separate AI/asset services without entering the game runtime.
- The 40 screen IDs are **functionality specs**, consolidated into ~12 high-level groups; there is no permanent list of 40 page names in the UI.
- WorldGraph stores **authoritative scene/asset dependencies**. RAG/vector retrieval informs AI but cannot replace exact IDs, revision checks, tool permissions and test evidence.
- AI creator tooling and **in-game NPC AI** are different products with distinct permissions and runtime constraints.
- Photorealistic GTA-scale original open worlds are a research/end-state benchmark, **not current functionality or an effortless one-prompt guarantee**. Do not copy GTA VI intellectual property.
- The correct next step is `FirstLight`: native Qt editor, C++ engine core, Vulkan cube viewport, stable IDs, Inspector/Outliner, save/load/undo, standalone later.

## Contribution rule

Every proposed feature needs a WF screen ID, owning module, input/output data type, typed command contract, failure states, test plan and versioning/migration impact. Do not claim build/benchmark completion without a log and target hardware/build ID.
