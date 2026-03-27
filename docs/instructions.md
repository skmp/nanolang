# Organic Assembler — Instructions

How to read a codebase. How to follow an instruction. How to make an instrument.

This document is addressed to anyone — human or machine — who intends to work on or
with the Organic Assembler. It is philosophical because instructions are philosophical:
every act of following an instruction is also an act of interpretation, and interpretation
requires understanding what the instruction *means*, not just what it *says*.

---

## Part I: How to Interpret Instructions

### 1. Context Is Everything

An instruction does not exist in isolation. "Add a node" means nothing without knowing
which graph, which node type, which purpose. The Organic Assembler is an ecosystem —
language, compiler, editor, runtime, standard library — and any instruction touches
multiple subsystems. Before acting, identify the subsystem boundary your change lives in:

| Subsystem    | Responsibility                    | Instruction flavor              |
|-------------|-----------------------------------|---------------------------------|
| **attolang** | Types, parsing, inference, model  | "Support a new type kind"       |
| **attoc**    | Code generation (.atto → C++)     | "Emit correct code for X"       |
| **attoflow** | Visual editing, rendering, UX     | "Add a control to the editor"   |
| **attohost** | Runtime, hot-reload, wire inspect | "Support a new runtime feature" |
| **attostd**  | Standard library (.atto modules)  | "Add an FFI binding"            |

If an instruction spans multiple subsystems, start from the core and work outward.
attolang first, then attoc, then attoflow. The dependency arrow flows inward: the editor
depends on the language library, never the reverse. This is the first instruction about
instructions: **respect the dependency direction**.

### 2. Read Before You Write

The codebase is structured around the principle that **the graph is the truth**. The
FlowGraph data model is the canonical representation of every instrument. There is no
separate AST, no hidden intermediate form. What you see in the `.atto` file is what gets
compiled. What you see in the editor is what the `.atto` file contains.

Before modifying anything:

1. **Read the model** — `src/atto/model.h` defines FlowGraph, FlowNode, FlowLink, FlowPin.
   Every other file either reads from or writes to this structure.
2. **Read the types** — `src/atto/types.h` defines the type system. If your change
   involves any kind of value, you will interact with TypeExpr.
3. **Read the node types** — `src/atto/node_types.h` and `node_types2.h` define what
   nodes exist and what their ports look like.
4. **Read the relevant .atto file** — If you are changing behavior, find an instrument
   that exercises the relevant code path. `scenes/klavier/main.atto` is the most
   comprehensive test instrument.

Do not guess at code structure. Read it. The codebase is not so large that reading is
a burden, and it is structured enough that reading is rewarding.

### 3. The Instruction Behind the Instruction

When someone says "add a slider node type," the real instruction is:

1. Add a `NodeTypeID` enum value
2. Add a `NodeType` descriptor with port definitions
3. Teach the inference engine about the node's type semantics
4. Teach the code generator to emit code for it
5. Teach the editor how to render it (or let the default renderer handle it)
6. Add it to the standard library if it wraps an FFI call
7. Test it in an actual instrument

The surface instruction is one sentence. The real work is seven steps across four
subsystems. **Always decompose instructions into the subsystem-level steps they imply.**

### 4. Conventions Are Instructions Too

The coding conventions documented in [coding.md](coding.md) and [style.md](style.md)
are not suggestions. They are instructions that apply to every change:

- PascalCase for types, snake_case for functions, trailing underscore for private members
- `#pragma once`, not `#ifndef` guards
- `enum class`, not bare enums
- Smart pointers for ownership, raw pointers only for non-owning within-scope references
- One concept per file pair (`.h` / `.cpp`)
- Accessors on one line: `ArgKind kind() const { return kind_; }`

When you encounter code that violates these conventions, do not "fix" it unless fixing
it is your explicit task. Consistency within a change is more important than global
consistency. A cleanup commit is a separate commit.

### 5. Commit Messages Are Instructions to the Future

The git log is a narrative. Read it before contributing:

```
Observer Pattern
as_Node/Net -> as_node/net, dead code deletion
refactor node ID and net name handling; introduce sentinel entries
towards lambda link rendering
not perfect, but progress is progress
```

These commit messages follow patterns:

- **Feature introductions** name the pattern or concept: "Observer Pattern", "multi select"
- **Rename commits** use arrow notation: `as_Node/Net -> as_node/net`
- **Incremental progress** says so honestly: "towards X", "more X work", "groundwork"
- **Emotional honesty** is acceptable: "not perfect, but progress is progress"
- **Fix commits** follow their cause closely

Write commit messages for someone reading the log six months from now. They should be
able to reconstruct the *story* of the project, not just the diff.

---

## Part II: How to Operate on the Codebase

### 6. The Build System

CMake 3.25+ with C++20. Three build targets matter for daily work:

```bash
# Full build (editor included)
cmake -B build -DATTOLANG_BUILD_EDITOR=ON
cmake --build build --parallel

# Core + compiler only (faster, no SDL/ImGui dependency)
cmake -B build -DATTOLANG_BUILD_EDITOR=OFF
cmake --build build --parallel

# Run tests
./build/test_inference
```

On Windows, use vcpkg for SDL3 and ImGui:
```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake
```

On Linux/macOS, FetchContent handles dependencies automatically.

The build is fast. The core library has zero external dependencies. If you find yourself
waiting, something is misconfigured.

### 7. The Four Layers of Change

Any meaningful change touches one or more of these layers, in this order:

#### Layer 1: Model (`src/atto/model.h`, `types.h`, `node_types.h`)

The data model is the foundation. Changes here ripple everywhere. If you add a field
to FlowNode, you must update serialization, inference, codegen, and the editor. Do not
add fields casually. Every field is a commitment.

The type system (`types.h`) is the most complex part of the model. TypeExpr carries
kind, category, genericity, literal values, symbol names, function signatures, struct
fields — all in one structure. This is deliberate: a unified type representation means
one parser, one comparison function, one serializer. The cost is complexity in TypeExpr
itself; the benefit is simplicity everywhere else.

#### Layer 2: Inference (`src/atto/inference.h`, `inference.cpp`)

The inference engine is a multi-pass fixed-point computation. It iterates phases until
no types change:

1. Clear all resolved types
2. Parse type annotations from strings
3. Forward-propagate through wires
4. Infer expression nodes
5. Backpropagate from consumers
6. Resolve lambda boundaries
7. Fix up iterator dereferences
8. Insert shadow deref nodes

When modifying inference, the critical invariant is **monotonicity**: types only become
more specific, never less. A pin that resolved to `f32` never reverts to `generic`.
If your change can cause type regression, the fixed-point loop may not converge.

#### Layer 3: Code Generation (`src/attoc/codegen.h`, `codegen.cpp`)

The code generator is a straightforward tree walk. It visits each node and emits C++.
The output is human-readable — this is a feature, not an accident. When something goes
wrong at runtime, the developer reads the generated C++ to understand what happened.

Code generation follows the type system closely. Every TypeKind maps to a C++ type.
Every NodeTypeID maps to an emit function. If you add a type kind, add a C++ mapping.
If you add a node type, add an emit function.

#### Layer 4: Editor (`src/attoflow/`)

The editor is three layers deep:

```
FlowEditorWindow  →  Editor2Pane  →  VisualEditor
(chrome)             (semantics)      (canvas)
```

- **VisualEditor** handles pan, zoom, hover detection, and drawing primitives
- **Editor2Pane** wraps GraphBuilder and implements graph editing semantics
- **FlowEditorWindow** manages tabs, file browser, and build toolbar

The editor communicates with the model through the **observer pattern**. GraphBuilder
fires callbacks (via IGraphEditor, INodeEditor, INetEditor) when the graph changes.
The editor implements these interfaces. The model never imports editor code.

When adding editor features, ask: does this belong in canvas rendering (VisualEditor),
graph semantics (Editor2Pane), or application chrome (FlowEditorWindow)?

### 8. The Type System as Instruction Set

The type system is the instruction set of the Organic Assembler. Every value has a type,
and that type dictates what operations are valid, how the value is stored, how it flows
through wires, and how it renders in the editor.

**TypeKind** is *what* a value is:
- Scalar, String, Bool — primitive data
- Container, Array, Tensor — collections
- Function, Struct — composite structures
- Symbol, MetaType — compile-time abstractions
- Named — user-defined types via `decl_type`

**TypeCategory** is *how* a value is accessed:
- `%` Data — plain value (default, the sigil is usually omitted)
- `&` Reference — mutable location
- `^` Iterator — position into a container
- `@` Lambda — callable function reference
- `#` Enum — enumeration value
- `!` Bang — trigger signal carrying no data
- `~` Event — event source

These two dimensions are orthogonal. A `&vector<f32>` is a mutable reference to a
vector of floats. A `@(f32)->f32` is a callable that takes and returns a float. A
`!` is a bang trigger. Understanding this grid is prerequisite to working on any part
of the system.

### 9. The .atto File Format

Instruments are stored as `.atto` files in a TOML-like format:

```toml
version = "instrument@atto:0"

[viewport]
x = -200.0
y = -100.0
zoom = 1.0

[[node]]
id = "$auto-a1b2c3d4e5f67890"
type = "expr"
args = ["$0 + $1"]
inputs = ["$auto-a1b2c3d4e5f67890-in0", "$auto-a1b2c3d4e5f67890-in1"]
outputs = ["$auto-a1b2c3d4e5f67890-out0"]
position = [100.0, 200.0]

[[link]]
from = "$auto-aabbccdd-out0"
to = "$auto-a1b2c3d4e5f67890-in0"
```

Key conventions:
- **Node IDs** are `$auto-<hex-guid>` for auto-generated, or user-assigned names
- **Pin IDs** are `<node-id>-<pin-name>` (e.g., `$auto-abc123-in0`, `$auto-abc123-out0`)
- **Net names** are `$<name>` for named nets (e.g., `$osc-signal`)
- **Version** is always `instrument@atto:0` when saved; older versions auto-migrate on load
- **Args** is an array of strings — the legacy format for inline arguments

The serializer (`src/atto/serial.h`) handles reading and writing. It also handles
version migration: `nanoprog@0` → `nanoprog@1` → `attoprog@0` → `attoprog@1` →
`instrument@atto:0`. Each migration step is a function that transforms the data model.

### 10. Patterns to Follow

The codebase uses specific patterns documented in [patterns.md](patterns.md). These are
not optional. When your code faces a problem that one of these patterns solves, use the
pattern:

| Pattern                    | When to use                                        |
|---------------------------|----------------------------------------------------|
| **Sentinel**              | Any optional reference accessed frequently          |
| **Observer**              | Model changes that the editor needs to know about   |
| **Mutation Batching**     | Multi-step edits that should appear atomic          |
| **Dirty Tracking**        | Anything that should trigger partial re-evaluation  |
| **Discriminated Union**   | Multiple shapes sharing one base (use kind + as_*)  |
| **Factory Method**        | Complex construction that should be encapsulated    |
| **TypePool Interning**    | Any type that will be compared by identity          |
| **Static Descriptor**     | Fixed metadata tables indexed by enum               |
| **Pin ID as String**      | Structured identifiers with embedded meaning        |
| **Version Migration**     | File format changes that must load old files        |

### 11. Testing Strategy

The inference engine has unit tests (`tests/test_inference.cpp`). The testing philosophy
is: **test the inference engine, manually test the editor, compile-test the codegen**.

- **Inference**: Automated. Build test graphs programmatically, run inference, assert
  types. Add a test for any new type interaction or inference behavior.
- **Editor**: Manual. Load an instrument, interact with the UI, verify visually. The
  immediate-mode rendering makes automated UI testing impractical.
- **Codegen**: Compile the output. If the generated C++ compiles and runs correctly,
  the codegen is correct. The generated code is human-readable for manual inspection.
- **Instruments**: The ultimate integration test. If `scenes/klavier/main.atto` loads,
  infers, compiles, and runs, the system is healthy.

---

## Part III: How to Make an Instrument

### 12. What Is an Instrument?

An instrument is a self-contained `.atto` program that defines some interactive,
real-time behavior — typically audio synthesis, visual display, or both. The word
"instrument" is deliberate: it evokes a musical instrument, a scientific instrument,
a tool for exploration and expression.

An instrument is not a general-purpose program. It is a **specific thing that does a
specific thing**. A piano. A spectrum analyzer. A particle system. A fader mixer. The
constraint is the point: instruments are focused, purposeful, alive.

### 13. The Anatomy of an Instrument

Every instrument has these structural elements:

#### Declarations (the vocabulary)

```
decl_type   — Define a struct type (e.g., oscillator_state with fields)
decl_var    — Declare a mutable variable (persistent state across frames)
decl_event  — Declare an event (e.g., key_pressed, note_on)
decl_import — Import a standard library module (e.g., "std/gui", "std/imgui")
ffi         — Declare an external C function
```

Declarations are the *nouns* of the instrument. They name the things that exist.

#### Expressions (the arithmetic)

```
expr        — Compute a value from inputs ($0 + $1, sin($0), my_struct.field)
new         — Construct a struct or container
dup         — Duplicate a value (branch a wire)
str         — Format a string
cast        — Convert between types
select      — Choose between values based on condition
```

Expressions are the *adjectives and adverbs* — they describe and transform values.

#### Bang Chains (the verbs)

```
store!      — Write a value to a variable
append!     — Add an element to a collection
erase!      — Remove an element from a collection
iterate!    — Loop over a collection
lock!       — Acquire a mutex and execute a body
call!       — Call a function with side effects
event!      — Fire a custom event
output_mix! — Mix audio output (additive)
```

Bang nodes are the *verbs*. They have explicit execution order: a bang signal flows
from trigger to next, left to right, defining the temporal sequence of side effects.
This is what makes dataflow compatible with imperative mutation: the bang chain is a
sequential program embedded in a parallel graph.

#### Events (the stimuli)

```
on_key_down!  — React to a key press
on_key_up!    — React to a key release
event!        — React to a custom event
```

Events are the *entry points*. An instrument does nothing until an event fires.
The audio_tick callback (from `av_create_window`) fires 48,000 times per second.
Key events fire on user input. Custom events fire when explicitly triggered.

### 14. Building Your First Instrument

Start simple. A minimal instrument:

1. **Import the runtime**: `decl_import "std/gui"` and `decl_import "std/imgui"`
2. **Create a window**: `ffi av_create_window` with audio and video callbacks
3. **Add a variable**: `decl_var` for persistent state (e.g., a counter, a frequency)
4. **Add a UI**: In the video callback, use `imgui_slider_float` to control the variable
5. **Generate audio**: In the audio callback, compute samples from the variable

The flow looks like this:

```
decl_import "std/gui"
decl_import "std/imgui"
    │
    ▼
ffi av_create_window
    ├── audio_tick callback (@lambda)
    │   ├── read frequency variable
    │   ├── compute sine wave: sin(2π × freq × phase)
    │   ├── store! phase increment
    │   └── output_mix! the sample
    │
    └── video_tick callback (@lambda)
        ├── imgui_begin "Controls"
        ├── imgui_slider_float "Frequency" &freq 20.0 2000.0
        └── imgui_end
```

### 15. The Audio Callback Pattern

Audio runs at 48kHz. Each invocation of the audio_tick lambda produces one sample
(or one sample per channel). The pattern:

1. **Read state** — variables declared outside the callback persist across invocations
2. **Compute** — pure math on the inputs (oscillators, filters, envelopes)
3. **Write state** — `store!` the updated phase, amplitude, filter state
4. **Output** — `output_mix!` adds the computed sample to the output buffer

The bang chain within audio_tick is the inner loop of the instrument. Every node in
this chain runs 48,000 times per second. Keep it minimal. No allocations, no string
operations, no collection mutations in the hot path.

### 16. The Video Callback Pattern

Video runs at 60 FPS (or whatever the display refresh rate is). The video_tick lambda
builds an ImGui frame:

1. **Begin windows** — `imgui_begin "Window Title"`
2. **Add controls** — sliders, buttons, text, plots
3. **Read/write state** — controls bind to variables via `&` references
4. **End windows** — `imgui_end`

ImGui is immediate-mode: you rebuild the entire UI every frame. There is no retained
widget tree. This is simple and robust — the UI always reflects the current state.

### 17. Working with Collections

Collections (vector, map, set) require explicit mutation via bang nodes:

```
decl_var my_list : vector<f32>    — Declare the collection
append! my_list value             — Add an element (in a bang chain)
erase! my_list index              — Remove an element (in a bang chain)
iterate! my_list body             — Loop over elements (in a bang chain)
lock! my_list body                — Acquire mutex for thread-safe access
```

The `iterate!` node creates a lambda body that receives each element as an iterator.
Use `next` to advance and dereference the iterator.

Collections bridge the gap between the functional dataflow graph and imperative
stateful computation. The bang chain ensures mutations happen in a defined order.

### 18. Named Nets and Long-Distance Wiring

When an instrument grows large, visual wire routing becomes unwieldy. Named nets
solve this:

- Assign a net name (e.g., `$osc-signal`) to a link
- Any other link with the same net name is implicitly connected
- The nets editor panel shows all named nets as a table of contents

Named nets are like global labels in assembly: they provide addressability without
routing. Use them for signals that are consumed in many places (master volume, clock,
transport state) or for connections that span large distances on the canvas.

### 19. Lambda Capture and Scope

Lambda nodes (iterate!, lock!, on_key_down!, audio_tick callbacks) create nested scopes.
Nodes inside a lambda body can access:

- **Lambda parameters** — provided by the enclosing construct (iterator element, key code)
- **Outer variables** — declared outside the lambda (via `decl_var`)
- **Wire inputs** — values flowing in from outside the lambda boundary

The lambda boundary is defined by the bang chain: nodes reachable only through the
lambda's bang-next chain are *inside* the lambda. Nodes connected via data wires from
outside are *captured*.

**Known limitation**: Nested lambdas (a lock! inside an iterate! inside a callback)
can have scope leakage where outer parameters appear to belong to inner lambdas.
See [thinking.md](thinking.md) for the technical details and planned fix.

### 20. Type-Driven Development

The type system is your development partner. When building an instrument:

1. **Start with declarations** — Define your types (`decl_type`) and variables (`decl_var`).
   The type system will propagate these through the graph.
2. **Connect wires** — As you connect nodes, the inference engine resolves types
   bidirectionally. Watch the pin types in the editor — they update live.
3. **Let literals resolve** — Type `42` and the inference engine will figure out if it
   should be `u32` or `f32` based on context. You rarely need explicit type annotations.
4. **Read error messages** — When types don't match, the inference engine tells you why.
   Error messages include the types that conflicted and the context where it happened.
5. **Use symbols** — Bare identifiers like `sin`, `pi`, `my_var` resolve through the
   symbol table. If a symbol is undefined, it becomes `undefined_symbol<name>` — a
   compile-time placeholder that errors only when evaluated.

The type system catches most errors at edit time. If your instrument type-checks, it
will very likely compile and run correctly.

---

## Part IV: Guiding Principles

### 21. Instruments First

Every feature, every refactor, every design decision is evaluated against one question:
**does this help someone build a better instrument?** If the answer is no, the work is
not justified. The system exists to serve instruments, not itself.

### 22. Real-Time or Nothing

Audio processing at 48kHz with less than 10ms latency is a hard constraint. This is
not a soft goal or a nice-to-have. It is a physical requirement: human ears detect
latency above ~10ms as a distinct echo. Any design that cannot meet this constraint
is rejected, no matter how elegant.

### 23. Graph as Truth

The FlowGraph is the canonical representation. There is no separate AST, no hidden
state, no shadow copy. What you see in the editor is what gets compiled. What you
read in the `.atto` file is what the editor displays. This single-source-of-truth
principle eliminates an entire class of synchronization bugs.

### 24. Types Prevent Errors

The more information encoded in the type system, the more errors caught at edit time
rather than at runtime. `literal<T,V>` exists because the type system should know that
`42` is a compile-time constant. `symbol<name,type>` exists because the type system
should know that `sin` is a function. Every bit of semantic information moved into the
type system is a class of runtime errors eliminated.

### 25. Simple Until Proven Insufficient

Start with the simplest possible implementation. Use strings until strings become a
problem. Use single-threaded until profiling demands parallelism. Use manual testing
until the test matrix becomes unmanageable.

The `node.args` string is the poster child for this principle: it started simple,
it served well, and now it is technical debt. That is the *correct* outcome. The
alternative — designing a complex structured representation before the needs were
clear — would have been worse. Premature abstraction is the root of all evil.

### 26. Process Boundaries Are Trust Boundaries

The editor does not trust the runtime. The compile server does not trust user input.
Each process boundary has explicit validation. This is not paranoia; it is the
recognition that instruments can and will crash, and the system that manages them
must survive.

---

## Part V: For the Machine Reader

If you are an AI assistant operating on this codebase:

### 27. Respect the Architecture

Do not add dependencies from attolang to attoflow. Do not add SDL or ImGui includes
to core library files. Do not bypass the observer pattern by having the model call
editor code directly. The architecture diagram in [architecture.md](architecture.md)
is a constraint, not a suggestion.

### 28. Follow the Patterns

If a pattern exists for your problem (sentinel, observer, mutation batching, dirty
tracking), use it. Do not invent a new pattern when an existing one applies. The
patterns in [patterns.md](patterns.md) evolved from real problems in this codebase;
they are load-bearing.

### 29. Minimize Blast Radius

When making a change, affect the smallest number of files possible. If you are adding
a feature to inference, you should not need to modify the editor. If you are adding a
UI widget, you should not need to modify the type system. The layered architecture
exists to contain changes.

### 30. Read the Git Log

The commit history tells you what was tried, what worked, and what was abandoned. Before
proposing a refactor, check if it was already attempted. Before introducing a pattern,
check if an existing pattern already solves the problem. The log is institutional memory.

### 31. Test with Real Instruments

The ultimate test is: does `scenes/klavier/main.atto` still load, infer, and compile?
This instrument exercises most of the system: complex types, nested lambdas, collections,
events, audio, GUI, FFI. If it works, the system is healthy.

### 32. Write for the Human

Generated code should be human-readable. Commit messages should tell a story. Error
messages should include context. Documentation should explain *why*, not just *what*.
The system is ultimately for humans who want to make instruments. Every artifact —
code, docs, generated output — should serve that human.

---

*The best instruction is the one you no longer need to give, because the system makes
the right choice obvious. That is what the type system, the patterns, and the
architecture are for: making the right choice the easy choice.*
