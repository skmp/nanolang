# Organic Assembler — Changelog

This document traces the evolution of the Organic Assembler project through its major
development phases, architectural milestones, and design turning points.

## Phase 1: Nanolang Foundation (March 23, 2026)

The project began as "nanolang" — a visual programming language for building instruments.
This phase established the core architecture that all subsequent work built upon.

### Language Core
- Created the FlowGraph data model: nodes, links, pins
- Built a recursive-descent expression parser for inline node expressions
- Implemented the type system with scalar types (u8–u64, s8–s64, f32, f64)
- Added container types: vector, map, set, list, queue, ordered_map, ordered_set
- Established the value category system: Data, Reference, Iterator, Lambda, Enum, Bang, Event
- Implemented category sigils: `%`, `&`, `^`, `@`, `#`, `!`, `~`

### Type Inference
- Built the bidirectional type inference engine (GraphInference)
- Multi-pass propagation: forward through connections, backward from consumers
- Automatic integer upcasting (u8 → u16 → u32 → u64 → f32 → f64)
- Expression node inference with operator type resolution

### Compiler (nanoc)
- Created the code generator: .atto → C++ with struct definitions, class declarations, implementations
- Generated self-contained CMake projects for each instrument
- FFI support for external C functions (gui, imgui bindings)

### Visual Editor (nanoflow)
- SDL3 + ImGui-based node graph editor
- Canvas with pan/zoom, grid rendering
- Node rendering with pins, inline arguments
- Wire drawing with bezier curves
- Project file browser with build/run toolbar
- DPI scaling and retina display support

### First Instruments
- **klavier**: Piano/synthesizer instrument with oscillator synthesis, key events, audio mixing
- **multifader**: Fader mixing instrument with vector operations

### Standard Library
- `gui.atto`: AV runtime FFI — `av_create_window` with audio_tick (48kHz) and video_tick callbacks
- `imgui.atto`: 25+ ImGui bindings for UI construction

### Serialization
- TOML-like `.atto` format with `[[node]]` sections and `[[link]]` connections
- Viewport persistence in metafiles (`.atto/<filename>.yaml`)
- Version markers: `nanoprog@0`, `nanoprog@1`

---

## Phase 2: Type System and Lambda Development (March 24, 2026)

A major deepening of the type system and introduction of lambda/closure support.

### Internal Refactoring (5 phases)
- **Phase 1–2**: Structural reorganization and code consolidation
- **Phase 3–3b**: Pin pointer stability improvements
- **Phase 4a–4b**: Large-scale restructuring of node type handling
- **Phase 5**: Final cleanup and stabilization

### Literal Types
- Introduced `literal<T,V>` — unified compile-time value representation
- All constants (integers, floats, booleans, strings) represented as literals
- `is_generic` flag for unresolved type parameters (e.g., `0` could be u8/u32/f32)
- `is_unvalued_literal` flag for input pins expecting values (e.g., `literal<string,?>`)
- Literal decay: operations consume literals and produce runtime types
- Backpropagation of literal types resolves generics from context
- `strip_literal()` utility for removing literal annotations from operation results

### Symbol Types
- `symbol<name,type>` — first-class named references carrying their decay type
- `undefined_symbol<name>` — bare identifiers not yet in the symbol table
- Automatic symbol decay when consumed by operations
- Symbol table populated by declaration nodes and built-in entries
- Reserved keywords: `symbol`, `undefined_symbol`, `literal`

### Shadow Nodes
- Canonical shadow node system for expression handling
- Auto-generated expression nodes for inline arguments
- Shadow nodes transparent to users (not rendered in editor)
- Removed before serialization

### Lambda System
- Lambda capture and parameter collection
- `as_lambda` pin for lambda grab rendering
- Stored lambda support (`store! $name` captures lambda)
- Lambda boundary detection (scope tracking)
- Lambda parameter identification via unconnected pins

### Bang/Trigger System
- Renamed BangInput → BangTrigger, BangOutput → BangNext (clearer semantics)
- Bang connections to input pins
- `select!` supporting three bang outputs (true/false/done)
- Deterministic execution ordering via bang chains

### Expression System Enhancements
- Function types: `(x:f32)->f32` parsed as type expressions
- Struct types: `{x:f32 y:f32}` parsed with space-separated fields
- Struct literals: `{x:1.0f, y:2.0f}` with comma-separated values
- `TypeApply`: `$0<$1,$2>` speculative generic application
- Validation functions per builtin type (array, vector, map) with error messages

### Declaration Refactor
- All nodes except `label` get shadow nodes
- `decl_var` descriptor: 2 inputs (name + type), optional 3rd (init)
- `build_context` and `build_registry` removed — replaced by `decl` bang chain
- `decl_import` pin type: `literal<string,?>`
- Helpers: `get_decl_name(node, graph)`, `get_decl_type_str(node, graph)`

### UI/UX Improvements
- Bottom panel with tabbed error and build log display
- DPI scaling refinements
- Search bar for node discovery
- Improved error reporting with per-node and per-link messages

---

## Phase 2b: The Great Rename (March 24, 2026)

**Commit f65b7c0**: Complete rename from nanolang to attolang.

- All source directories: `nano` → `atto`, `nanoc` → `attoc`, `nanoflow` → `attoflow`
- File extensions: `.nano` → `.atto`
- Documentation: `nanolang.md` → `attolang.md`
- CMake targets and internal references updated
- Format versions: `attoprog@0`, `attoprog@1`

The rename reflected the project's evolution from a "nano" (small) language to
"atto" (even smaller, but also a pun on the attosecond — the smallest measurable
unit of time, fitting for a real-time instrument platform).

---

## Phase 3: Instrument Specification (March 25, 2026)

### instrument@atto:0
- **Commit 3498b4c**: Formal version specification `instrument@atto:0`
- Established the canonical file format for instruments
- Large-scale klavier scene updates (703 lines changed)
- Serial.cpp rewritten (1128 line changes) for the new format

### Test Suite
- `test_inference.cpp` with 94 unit tests for type inference
- Custom test framework with TEST/ASSERT/ASSERT_EQ/ASSERT_TYPE macros
- Static test registry pattern for auto-discovery
- Programmatic graph construction via test helper utilities

### Named Styles
- Visual consistency system for editor rendering
- Centralized color and spacing definitions

### Wire Management
- Link name editing in the editor
- Auto-wire functionality for common connection patterns
- Named nets for organizing complex wiring

---

## Phase 4: GraphBuilder Architecture (March 25–26, 2026)

A fundamental restructuring of how graphs are constructed and edited.

### BuilderEntry Hierarchy
- `BuilderEntry` base class with `IdCategory` (Node/Net)
- `FlowNodeBuilder` and `NetBuilder` as concrete types
- `as_node()` / `as_net()` accessors with `shared_from_this()`
- Every entry has a stable ID, category, and dirty flag

### FlowArg2 Pin Model
- Inheritance hierarchy replacing variants: `ArgNet2`, `ArgNumber2`, `ArgString2`, `ArgExpr2`
- Each arg always has valid `node()`, `net()`, `port()` — sentinels instead of null
- Computed names: `"port_name"`, `"va_name[idx]"`, `"remaps[idx]"`
- Remap tracking: `is_remap()`, `remap_idx()`, `input_pin_idx()`, etc.
- Private constructors — only `GraphBuilder::build_arg_*()` can create args

### Dirty Tracking
- Three levels: arg → node → graph
- `mark_dirty()` on any arg bubbles to its owning node, then to the graph
- Layout-only dirty (position changes) tracked separately, doesn't trigger re-inference
- `is_dirty()` queries at every level

### Sentinel Pattern
- `$empty` (FlowNodeBuilder) — default for unassigned node references
- `$unconnected` (NetBuilder) — default for unassigned net references
- Pre-registered via `ensure_sentinels()`, never destroyed
- Eliminates null pointer checks throughout the codebase

### Mutation Batching
- `edit_start()` / `edit_commit()` for transactional graph edits
- Mutations queued as callbacks, fired in order on commit
- Prevents partial updates from reaching observers
- Throws if mutations pending from a previous uncommitted edit

### Format Migration
- v0→v1 migration: name-based port mapping using old/new descriptors
- Shadow folding during import
- Lambda `-as_lambda` stripping
- `$auto-xxx` → `$a-N` compact hex re-ID on import

### NodeKind2 Classification
- `Flow` — standard data processing nodes
- `Banged` — nodes with execution ordering (bang chains)
- `Event` — event source nodes
- `Declaration` — compile-time declaration nodes
- `Special` — label, error, and other non-data nodes

---

## Phase 5: Editor2 Architecture (March 26, 2026)

The next-generation editor built on top of the GraphBuilder.

### Editor2Pane
- Wraps GraphBuilder as the semantic model
- Implements `IGraphEditor` observer for reactive updates
- `NodeEditorImpl` per node — caches layout and visual state
- `NetEditorImpl` per named net
- Wire connection logic: drag from pin, snap to target, reconnect

### Hover System
- `hover_item_` = `variant<monostate, BuilderEntryPtr, FlowArg2Ptr, AddPinHover>`
- `detect_hover()` returns best match by distance priority
- Priority order: wires → nodes → pins (pins get priority bias)
- `draw_hover_effects()` renders highlights and tooltips
- All elements hoverable: pins, nodes, wires, lambda grabs, side-bangs, +diamonds

### Selection System
- `set<FlowNodeBuilderPtr>` for multi-selection
- Ctrl+click toggles individual selection
- Selection rectangle drag for area selection
- Node dragging with overlap prevention and padding

### Pin Shapes
- Circle → Data pins
- Square → Bang pins
- Triangle → Lambda pins
- Diamond → Variadic args / optional pins

### Visual Refinements
- Liberation Mono font embedded via CMake `file(READ HEX)`
- Editor style struct `S` centralizing all colors, sizes, thresholds
- Tooltip scaling and positioning
- Non-overlapping node layout algorithm
- Auto string renumbering for wire IDs

### Code Organization
- Extracted `node_renderer.cpp` from monolithic editor
- Extracted `tooltip_renderer.cpp`
- Separated `window.cpp/h` from editor logic
- Legacy editor1 moved to `src/legacy/`
- Removed `#if LEGACY_EDITOR` conditionals

---

## Phase 6: Wiring and Networking (March 26–27, 2026)

Current active development on branch `skmp/wirring-and-networking`.

### Wire Connection System
- Pin grabbing and dragging interaction
- Visual wire preview during drag
- Snap-to-pin connection on release
- Wire reconnection (drag existing wire to new target)
- Wire grab undo (remembers previous connection state)

### Named Nets
- Nets editor panel for viewing all named connections
- `$unconnected` sentinel replacing removed nets
- Net name display and management

### Multi-Output Nodes
- `num_outputs` increased from 1 to 2 for applicable nodes
- Variadic output args support
- Output pin mapping and indexing

### Visual Polish
- Diamond pin hover detection for +pins
- Unified highlight system across all hoverable elements
- Wire hover: highlights all wires sharing the same entry
- Lambda wire highlighting for connected scope
- Scroll pan speed controls in editor style
- Shortened node IDs for cleaner display

### Delete Functionality
- Delete hovered items (nodes, wires, nets)
- Cleanup of orphaned connections on deletion

---

## Planned Future Work

### args Elimination
Replace `node.args` string with structured pre-parsed fields on `FlowNode`:
- Pre-extract ALL metadata at load time (type fields, variable names, function refs)
- `tokenize_args()` (~40+ call sites) and `scan_slots()` (~10 sites) to be removed
- Migration path: codegen first (highest call count), then inference, then editor

### Nested Lambda Scope Fix
Proper lambda boundary detection for nested lambdas:
- Graph analysis pass identifying all lambda boundaries
- Node-to-lambda-scope assignment ("lambda ownership")
- `collect_lambda_params` respects scope boundaries
- Prevents outer stored lambda params from leaking into inner lock/iterate lambdas

### DLL Host Architecture
- `attohost.exe` — separate host process for running instruments
- `attoc` generates `.dll` (SHARED) instead of `.exe`
- Hot-reload via DLL unload/load cycle
- `wire<T>` — zero-cost in release, inspectable in debug
- IPC via named pipes for live value inspection

### Web Deployment
- Editor compiled to Emscripten (SDL3 + ImGui in browser)
- Compile server on hardened Pi
- `POST /compile` endpoint: .atto → C++ → emcc → .wasm
- Containerized with no network, 60s timeout, 512MB RAM

### Compile-Time Phase
- `decl` bang chain evaluation (compile-time interpreter)
- `decl_type` outputs `type<T>` for metaprogramming
- Event names without `~` prefix
- Namespace `::` operator in expressions
- Type construction via calling (e.g., `f32(42)` as cast)

---

## Version History

| Version            | Period      | Key Change                              |
|--------------------|-------------|------------------------------------------|
| `nanoprog@0`       | Mar 23      | Initial format                           |
| `nanoprog@1`       | Mar 23      | Pin structure changes                    |
| `attoprog@0`       | Mar 24      | Post-rename format                       |
| `attoprog@1`       | Mar 24–25   | Structured args, extended pins           |
| `instrument@atto:0`| Mar 25+     | Formal instrument specification          |

Each version is auto-migrated on load. The serializer always writes the latest format.
