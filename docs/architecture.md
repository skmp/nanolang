# Organic Assembler — Architecture

## Overview

Organic Assembler (orgasm) is an **Operating System for Instruments**. An "instrument" is a
multimodal dataflow program authored as a node graph in a `.atto` file using the attolang
language. The system comprises four major subsystems: the core language library, the compiler,
the visual editor, and the runtime. Each is independently buildable and has well-defined
boundaries.

```
┌─────────────────────────────────────────────────────────┐
│                    User / Developer                     │
│                                                         │
│   ┌─────────────┐    ┌─────────────┐    ┌────────────┐  │
│   │  attoflow   │    │   attoc     │    │ attohost   │  │
│   │  (editor)   │───>│  (compiler) │───>│ (runtime)  │  │
│   └──────┬──────┘    └──────┬──────┘    └──────┬─────┘  │
│          │                  │                  │        │
│          └──────────────────┼──────────────────┘        │
│                             │                           │
│                    ┌────────┴────────┐                  │
│                    │    attolang     │                  │
│                    │ (core library)  │                  │
│                    └─────────────────┘                  │
└─────────────────────────────────────────────────────────┘
```

## Build Targets

The project uses CMake 3.25+ with C++20. There are four build targets:

| Target            | Type       | Description                              | Dependencies         |
|-------------------|------------|------------------------------------------|----------------------|
| `attolang`        | Static lib | Core language: types, parsing, inference | C++20 stdlib only    |
| `attoc`           | Executable | Standalone compiler (.atto → C++)        | attolang             |
| `test_inference`  | Executable | Unit tests for type inference            | attolang             |
| `attoflow`        | Executable | Visual node editor (optional)            | attolang, SDL3, ImGui|

The editor (`attoflow`) is optional and gated behind `ATTOLANG_BUILD_EDITOR`. The core
library and compiler have zero external dependencies beyond the C++20 standard library.

## Directory Layout

```
orgasm/
├── CMakeLists.txt                 # Top-level build configuration
├── cmake/AttoDeps.cmake           # SDL3 + ImGui fetch (vcpkg on Windows, FetchContent otherwise)
├── vcpkg.json                     # Windows dependency manifest
│
├── src/
│   ├── atto/                      # Core language library (attolang)
│   │   ├── model.h                #   FlowGraph, FlowNode, FlowLink, FlowPin
│   │   ├── types.h / types.cpp    #   Type system: TypeExpr, TypeParser, TypePool
│   │   ├── expr.h / expr.cpp      #   Expression AST: tokenizer, parser, ExprNode
│   │   ├── inference.h / .cpp     #   Bidirectional type inference engine
│   │   ├── graph_builder.h / .cpp #   Structured graph editing with dirty tracking
│   │   ├── graph_index.h / .cpp   #   Fast pin/node lookup by ID
│   │   ├── serial.h / serial.cpp  #   .atto file serialization (TOML-like)
│   │   ├── shadow.h / shadow.cpp  #   Shadow expression nodes (deref, internal)
│   │   ├── symbol_table.h / .cpp  #   Compile-time symbol resolution
│   │   ├── type_utils.h / .cpp    #   Type compatibility, upcasting rules
│   │   ├── args.h / args.cpp      #   Argument tokenization utilities
│   │   ├── node_types.h / .cpp    #   Node type descriptors (v1)
│   │   ├── node_types2.h          #   Extended node type descriptors (v2)
│   │   └── graph_editor_interfaces.h  # Observer interfaces for editor integration
│   │
│   ├── attoc/                     # Standalone compiler
│   │   ├── main.cpp               #   Entry point: load → infer → codegen
│   │   ├── codegen.h / .cpp       #   C++ code generation (types, header, impl, cmake)
│   │
│   ├── attoflow/                  # Visual editor
│   │   ├── main.cpp               #   SDL3 window init, 60 FPS event loop
│   │   ├── window.h / .cpp        #   Tab management, project browser, build toolbar
│   │   ├── editor2.h / .cpp       #   Editor2Pane: wraps GraphBuilder, wire connection
│   │   ├── visual_editor.h / .cpp #   Canvas: pan/zoom, grid, hover detection
│   │   ├── node_renderer.h / .cpp #   Node visualization: pins, args, errors
│   │   ├── nets_editor.h / .cpp   #   Named nets panel
│   │   ├── editor_style.h / .cpp  #   Colors, fonts, spacing constants
│   │   ├── tooltip_renderer.h/.cpp#   Hover tooltip rendering
│   │   ├── sdl_imgui_window.h     #   SDL3 + ImGui integration layer
│   │   └── fonts/                 #   Liberation Mono (embedded via CMake hex read)
│   │
│   ├── attoruntime/               # Instrument runtime
│   │   ├── attoruntime.h          #   Runtime API declarations
│   │   ├── atto_gui.cpp           #   GUI bindings (imgui calls)
│   │   ├── nano_imgui.cpp         #   High-level ImGui wrappers
│   │   └── main.cpp               #   Stub entry point
│   │
│   └── legacy/                    # Deprecated editor1 code
│
├── attostd/                       # Standard library (.atto modules)
│   ├── gui.atto                   #   AV/audio FFI: av_create_window, audio_tick
│   └── imgui.atto                 #   ImGui FFI: buttons, sliders, text, windows
│
├── scenes/                        # Example instruments
│   ├── klavier/main.atto          #   Piano/synthesizer (29KB, 100+ nodes)
│   └── multifader/main.atto       #   Fader mixing instrument (25KB)
│
├── tests/
│   └── test_inference.cpp         # Type inference unit tests
│
└── docs/
    └── attolang.md                # Full language specification (44KB)
```

## Subsystem Deep Dives

### 1. attolang — Core Language Library

The core library is a static C++ library with **zero external dependencies**. It defines
the data model, type system, expression parser, inference engine, serialization, and
graph builder. Everything in `src/atto/` belongs here.

#### Data Model (`model.h`)

The fundamental representation is a **FlowGraph**: a directed graph of nodes connected
by typed links.

```
FlowGraph
├── nodes: vector<FlowNode>
│   ├── guid: string          (stable identifier)
│   ├── type: NodeTypeID      (one of 38 built-in types)
│   ├── args: string          (inline arguments, legacy)
│   ├── position: vec2        (editor layout)
│   ├── inputs: vector<string>  (pin IDs for incoming connections)
│   └── outputs: vector<string> (pin IDs for outgoing connections)
│
└── links: vector<FlowLink>
    ├── from: string          (output pin ID)
    ├── to: string            (input pin ID)
    └── net_name: string      (optional named net)
```

Nodes are passive data containers. All semantic meaning comes from the node's `type`
field, which indexes into the node type registry.

#### Type System (`types.h`, `type_utils.h`)

The type system is the backbone of attolang. Every value has a **TypeExpr** with two
orthogonal dimensions:

1. **TypeKind** — what the value is (Void, Bool, Scalar, Container, Function, Symbol, etc.)
2. **TypeCategory** — how the value is accessed (Data, Reference, Iterator, Lambda, etc.)

Categories are denoted by sigils in the wire format:

| Sigil | Category   | Meaning                           |
|-------|------------|-----------------------------------|
| `%`   | Data       | Plain value (default)             |
| `&`   | Reference  | Reference to a mutable location   |
| `^`   | Iterator   | Iterator into a container         |
| `@`   | Lambda     | Callable function reference       |
| `#`   | Enum       | Enumeration value                 |
| `!`   | Bang       | Trigger signal (carries no data)  |
| `~`   | Event      | Event source                      |

Type expressions are parsed by `TypeParser` (recursive descent) and interned in
`TypePool` for deduplication. The pool caches every parsed type string → `TypePtr`
mapping.

Key type features:
- **Literal types**: `literal<T,V>` — compile-time constants with type domain T and value V
- **Symbol types**: `symbol<name,type>` — first-class named references with decay
- **Meta types**: `type<T>` — types as compile-time values
- **Generic types**: `unsigned<?>`, `float<T:?>` — resolved via backpropagation
- **Automatic upcasting**: u8 → u16 → u32 → u64, s8 → s32 → s64, integer → float

#### Expression Parser (`expr.h`, `expr.cpp`)

Expressions are inline code within nodes (e.g., `$0 + $1 * sin($2)`). The parser is a
recursive-descent parser producing an `ExprNode` AST.

Expression kinds include:
- `Literal` — all constants (integers, floats, booleans, strings)
- `PinRef` — `$N` references to input pins
- `BinaryOp` — arithmetic, comparison, logical operators
- `UnaryOp` — negation, logical not
- `FunctionCall` — `name(args...)` including type constructors
- `FieldAccess` — `expr.field`
- `IndexOp` — `expr[index]`
- `TypeApply` — `$0<$1,$2>` speculative generic application

The tokenizer handles TOML string escaping, nested parentheses, and the distinction
between struct types (`{x:f32 y:f32}`) and struct literals (`{x:1.0f, y:2.0f}`) via
comma detection.

#### Inference Engine (`inference.h`, `inference.cpp`)

Type inference is **bidirectional** and **multi-pass**:

```
Phase 1: clear_all()              — Reset all pin types
Phase 2: resolve_pin_type_names() — Parse type annotations via TypePool
Phase 3: propagate_connections()  — Forward: flow types through wires
Phase 4: infer_expr_nodes()       — Recursive inference on expression ASTs
Phase 5: propagate_pin_ref_types()— Backward: backprop from consumers
Phase 6: resolve_lambdas()        — Validate lambda capture and parameters
Phase 7: fixup_expr_derefs()      — Insert Deref nodes for iterators
Phase 8: insert_deref_nodes()     — Materialize shadow deref nodes
```

The engine runs to a fixed point for phases 3-5, iterating until no types change.
This handles mutual dependencies between nodes (A depends on B depends on A).

#### Graph Builder (`graph_builder.h`, `graph_builder.cpp`)

The GraphBuilder is the **high-level editing API** layered over the raw FlowGraph model.
It provides:

- **Structured entries**: `FlowNodeBuilder` and `NetBuilder` with `BuilderEntry` base
- **Pin model**: `FlowArg2` hierarchy (ArgNet2, ArgNumber2, ArgString2, ArgExpr2)
- **Dirty tracking**: mutations bubble up from arg → node → graph
- **Observer pattern**: `IGraphEditor` / `INodeEditor` / `INetEditor` interfaces
- **Mutation batching**: `edit_start()` / `edit_commit()` for transactional edits
- **Sentinels**: `$empty` (node) and `$unconnected` (net) — always valid, never null

The builder also handles v0→v1 format migration, auto-generated IDs (`$auto-xxx` → `$a-N`),
and shadow node folding during import.

#### Serialization (`serial.h`, `serial.cpp`)

The `.atto` file format is TOML-like with section headers:

```toml
version = "instrument@atto:0"

[viewport]
x = -6131.11
y = -1999.86
zoom = 1.4641

[[node]]
guid = "934e3b98bb914e95"
type = "decl_type"
args = ["osc_res", "s:f32", "e:bool"]
position = [757.077, 266.729]

[[link]]
from = "$auto-df6e4aa3d0d8d2bc-out0"
to = "$auto-831e483b4e4602dc_s1-out0"
net_name = "$osc-signal"
```

Version history: `nanoprog@0` → `nanoprog@1` → `attoprog@0` → `attoprog@1` → `instrument@atto:0`.
Auto-migration on load handles all legacy formats.

### 2. attoc — Compiler

The compiler is a standalone executable that transforms `.atto` files into self-contained
C++ projects. The pipeline:

```
.atto file
    │
    ▼
load_atto() ──────────────── Parse TOML → FlowGraph
    │
    ▼
resolve_type_based_pins() ── Populate dynamic pins for new/event! nodes
    │
    ▼
GraphInference::run() ────── Multi-pass bidirectional type inference
    │
    ▼
validate() ──────────────── Collect type errors, pin mismatches
    │
    ▼
CodeGenerator::generate_*()
    ├── <name>_types.h ───── Struct definitions from decl_type nodes
    ├── <name>_program.h ─── Class declaration with event handlers
    ├── <name>_program.cpp ─ Implementation: node logic, expression eval
    ├── CMakeLists.txt ───── Build script for the instrument
    └── vcpkg.json ───────── Dependency manifest
```

The generated C++ is a complete, standalone project. Each instrument compiles
independently with its own CMakeLists.txt.

### 3. attoflow — Visual Editor

The editor has a three-layer architecture:

```
FlowEditorWindow (top-level)
├── Tab management (multiple .atto files)
├── Project file browser
├── Build/run toolbar
├── Child process management
│
└── Editor2Pane (per-tab)
    ├── Wraps GraphBuilder (semantic model)
    ├── Implements IGraphEditor observer
    ├── Wire connection logic (drag, connect, reconnect)
    ├── NodeEditorImpl per node (layout cache)
    ├── NetEditorImpl per net
    │
    └── VisualEditor (rendering base)
        ├── Canvas state (pan, zoom, grid)
        ├── Hover detection (wires → nodes → pins)
        ├── Node rendering (pins, inline args, errors)
        └── Wire drawing (bezier curves, WireInfo)
```

The hover system uses a distance-based priority: wires → nodes → pins, with pins
getting a priority bias for easier selection.

Pin shapes encode type categories:
- **Circle** — Data
- **Square** — Bang
- **Triangle** — Lambda
- **Diamond** — Variadic args / optional

### 4. attohost — Runtime (Planned)

The runtime architecture separates the editor from user program execution:

```
attoflow.exe                    attohost.exe
(editor process)                (host process)
     │                               │
     │  spawn + pipe name arg        │
     ├──────────────────────────────>│
     │                               │ LoadLibrary(instrument.dll)
     │                               │ resolve on_start()
     │  IPC: wire table              │ run on_start()
     │<──────────────────────────────┤
     │                               │
     │  IPC: dirty wire updates      │ per-frame/tick updates
     │<──────────────────────────────┤
     │                               │
     │  IPC: value override          │
     ├──────────────────────────────>│ inject values for debugging
     │                               │
     │  IPC: reload signal           │
     ├──────────────────────────────>│ unload dll, load new, restart
```

Key design decisions:
- **Process isolation**: user program crash doesn't take down the editor
- **Hot-reload**: unload DLL, load new one, call `on_start` again
- **wire<T>**: zero-cost in release (typedef to T), inspectable in debug mode
- **Named pipes**: editor is always the server, host is always the client

## Cross-Cutting Concerns

### Node Type System

There are 38 built-in node types organized into categories:

| Category      | Nodes                                                    |
|---------------|----------------------------------------------------------|
| Data          | expr, new, dup, str, select, cast, void                  |
| Collections   | append/append!, erase/erase!, iterate/iterate!, next     |
| Control       | select!, lock!, iterate!, store!, output_mix!            |
| Declarations  | decl_type, decl_var, decl_event, decl_import, ffi, decl  |
| Events        | on_key_down!, on_key_up!, event!                         |
| Special       | call/call!, lock/lock!, label, error                     |

Bang (`!`) nodes have execution ordering — they fire in the order of their bang chain,
providing deterministic side-effect sequencing in a dataflow graph.

### Standard Library

The `attostd/` directory contains `.atto` module files that declare FFI bindings:

- **gui.atto**: `av_create_window(title, audio_tick, sample_rate, channels, video_tick, width, height, on_close)` — creates a window with audio and video callbacks
- **imgui.atto**: 25+ ImGui bindings (buttons, sliders, text, windows, layout)

These are imported via `decl_import` nodes in user instruments.

### Web Target (Planned)

Future web deployment via a compile server:

```
Browser (Emscripten build of editor)
    │
    │  POST /compile  (sends .atto)
    ▼
Compile Server (hardened Pi)
    │  attoc → C++ → emcc → .wasm
    ▼
Browser loads result as side module
```

The server is containerized with no network access inside, 60s timeout, 512MB RAM limit,
read-only rootfs. Only `.atto` goes in (small attack surface), controlled C++ comes out.

## Design Principles

1. **Zero-dependency core**: attolang has no external dependencies. Only the editor needs SDL3/ImGui.
2. **Graph IS the program**: no separate AST. The FlowGraph is the canonical representation, serialized directly.
3. **Type-driven everything**: inference, validation, codegen, and editor rendering all flow from the type system.
4. **Bidirectional inference**: types propagate both forward (producers → consumers) and backward (consumers → producers).
5. **Observer not polling**: GraphBuilder notifies editors of changes via interfaces, not polling for dirty state.
6. **Sentinels over nulls**: `$empty` and `$unconnected` are always-valid stand-ins, eliminating null checks.
7. **Process isolation**: the editor and runtime are separate processes communicating via IPC.
8. **Instruments are self-contained**: each compiled instrument is a standalone C++ project with its own build system.
