# Organic Assembler — Design Thinking

This document captures the design philosophy, trade-offs, and reasoning behind the
key decisions in the Organic Assembler project. Understanding *why* things are built
this way is essential for making coherent future decisions.

## The Core Metaphor: Operating System for Instruments

Organic Assembler is not just a language or an editor. It is an **Operating System for
Instruments**. This framing drives every major architectural decision:

- **Instruments** (not "programs") are the unit of authorship. Each `.atto` file is a
  self-contained instrument — potentially audio, visual, interactive, or all three.
- **The OS** is the ecosystem: compiler, editor, runtime, standard library. It manages
  the instrument lifecycle from creation through editing, compilation, execution, and
  hot-reload.
- **attolang** is the language, but the language is subservient to the OS concept. The
  language exists to make instruments expressible, not as an end in itself.

This is why the project includes an editor, a runtime with audio/video callbacks, and
FFI bindings for GUI and audio. A language-only project would stop at the compiler.

## Why a Visual Dataflow Language?

Instruments are inherently about **signal flow**. Audio signals, control signals, event
triggers, and UI state all flow between processing nodes. A textual language forces the
programmer to name intermediate values and linearize a fundamentally parallel, graph-shaped
computation.

A visual dataflow graph:
1. **Shows topology directly** — you see what connects to what
2. **Eliminates naming burden** — wires *are* the variables
3. **Supports spatial reasoning** — related nodes are placed near each other
4. **Makes parallelism obvious** — unconnected subgraphs run independently
5. **Enables live editing** — change a connection, see the result immediately

The trade-off is that text is better for complex expressions. That's why attolang supports
inline expressions within nodes (`$0 + $1 * sin($2)`). The graph handles topology, the
expressions handle math.

## Why Compile to C++?

The decision to compile `.atto` → C++ → native binary (rather than interpret or compile
to a custom IR) was driven by pragmatism:

1. **Performance**: Audio processing at 48kHz with low latency requires native speed.
   An interpreter would add unacceptable overhead for real-time audio.
2. **Ecosystem**: C++ gives access to every audio/graphics library. No FFI bridge needed
   for SDL, ImGui, or custom DSP code.
3. **Debugging**: Standard C++ debuggers (MSVC, GDB, LLDB) work on the output. No custom
   debug tooling needed initially.
4. **Simplicity**: The codegen is a straightforward tree walk. No register allocation, no
   instruction selection, no optimization passes. C++ handles all of that.

The cost is compilation latency (seconds instead of milliseconds). The hot-reload
architecture (DLL swap) mitigates this for the edit-test cycle.

### Why Not LLVM?

LLVM would give faster compilation and avoid the C++ intermediate step. But:
- LLVM is a massive dependency (~100MB+ binary)
- LLVM's API changes frequently between versions
- The generated C++ is human-readable, which aids debugging
- LLVM doesn't help with web deployment (Emscripten compiles C++)

If compilation latency becomes a bottleneck, a WASM codegen backend is the planned
escape hatch — but that's future work.

### Why Not Interpret?

An interpreter was considered (and would be the fastest path to "runs in browser").
The blocker is audio: at 48kHz sample rate, the audio callback runs every ~21
microseconds per sample. An interpreter would need to be extremely fast, and any
garbage collection pause would cause audio glitches.

For non-audio instruments (pure GUI tools, data processing), interpretation might
be viable in the future.

## The Type System Philosophy

### Why Bidirectional Inference?

Unidirectional inference (forward-only, Hindley-Milner-style) doesn't work well for
dataflow graphs because:

1. **Producers and consumers are peers** — in a graph, there's no clear "this comes first"
   ordering. A node's output type might depend on its input types (forward), but its input
   types might also depend on what its outputs connect to (backward).

2. **Literal types need context** — `0` could be u8, u16, u32, u64, f32, or f64. The
   correct type comes from the *consumer*. Without backpropagation, every literal would
   need explicit type annotation.

3. **Generic containers need both directions** — `vector<?>` resolves when either a
   producer provides `vector<f32>` (forward) or a consumer expects `f32` elements (backward).

The multi-pass fixed-point approach (iterate until no types change) handles mutual
dependencies gracefully.

### Why literal<T,V> Instead of Constant Folding?

Traditional compilers fold constants at the IR level. attolang makes literals a *type-level*
concept for several reasons:

1. **Type resolution**: `42` in `$0 + 42` needs to resolve to the same type as `$0`. If `42`
   is just an integer, the type system can't help. As `literal<unsigned<?>,42>`, the type
   system resolves `unsigned<?>` from context.

2. **Compile-time metaprogramming**: Literal types flow through the graph as type information.
   A `decl_type` node receives field names as `literal<string,?>` inputs. This makes the
   type system the compile-time evaluation mechanism.

3. **No separate constant-folding pass**: The type system handles what would otherwise require
   a separate optimization pass. Less code, fewer passes, more unified semantics.

### Why Symbols?

Bare identifiers (`sin`, `my_var`, `f32`) produce symbol types rather than immediately
resolving to values. This two-phase approach (parse → symbol, then symbol → value on
consumption) enables:

1. **Deferred resolution**: `decl_var` receives a *name* as input, not a value. The name is
   an `undefined_symbol<name>` until the declaration is processed. This lets the declaration
   system work without special-casing the expression parser.

2. **First-class type references**: `f32` as a symbol decays to `type<f32>`. This means type
   names and value names live in the same expression language. No separate "type expression"
   syntax needed (though struct types `{...}` and function types `(...)→...` do have special
   syntax because they contain embedded type references).

3. **Graceful error handling**: An `undefined_symbol<name>` doesn't error at parse time. It
   errors only when something tries to *evaluate* it. This means partial programs (still
   being edited) don't explode with cascading errors.

## The Graph Builder Philosophy

### Why Two Layers (Model + Builder)?

The original `FlowGraph` (model layer) is a simple, serializable data structure. It was
designed for file I/O and compiler consumption. But the editor needs:

- Dirty tracking (what changed since last render?)
- Observer notifications (tell the UI when something mutates)
- Structured access (not just raw string arrays)
- Transactional edits (batch multiple changes atomically)
- Sentinels (never-null references for safe UI code)

Rather than adding all this to the model (making it complex and slow for the compiler),
a second layer (GraphBuilder) was created. The builder wraps the model and adds
editor-specific concerns.

This separation means:
- The compiler never pays for editor overhead
- The model remains simple and fast to serialize
- The builder can evolve independently (new UI features don't affect codegen)
- Tests can use either layer (model for fast unit tests, builder for integration tests)

### Why Sentinels Over Nulls?

Every `FlowArg2` always has a valid `node()` and `net()`. If no real node/net is assigned,
sentinels (`$empty`, `$unconnected`) are used instead.

This eliminates an entire class of bugs: null pointer dereferences in UI code. When
rendering a pin, you can always call `arg->node()->id()` without checking for null.
The sentinel returns safe default values.

The trade-off is a small memory cost (two extra entries in the graph) and the need to
check `is_sentinel()` when the distinction matters. In practice, sentinel checks are
rare — most code just uses the values.

### Why Mutation Batching?

Graph edits often involve multiple changes that should appear atomic:
- Moving a node updates its position (one change) and may adjust connected wires (more changes)
- Connecting a wire creates a link and updates both endpoint pins
- Deleting a node removes the node, all its links, and all its pins

Without batching, each individual change would trigger observer notifications, causing
the UI to re-render partially updated state. This leads to visual glitches and, worse,
can trigger cascading re-inference on incomplete graphs.

`edit_start()` / `edit_commit()` brackets these multi-step operations. All observer
notifications are queued and fired only on commit, ensuring observers always see
consistent state.

## The Editor Philosophy

### Why SDL3 + ImGui?

SDL3 provides:
- Cross-platform windowing (Windows, macOS, Linux)
- Audio output with low-latency callbacks
- Input handling (keyboard, mouse, gamepad)
- Emscripten support for web deployment

ImGui provides:
- Immediate-mode UI rendering (no retained widget tree)
- Custom drawing primitives (lines, beziers, filled shapes)
- Text rendering with custom fonts
- Docking and multi-viewport support

Together they give a lightweight, fast, cross-platform rendering stack that can
run at 60 FPS while drawing complex node graphs. The immediate-mode approach means
the editor re-renders every frame from scratch — no stale cached widgets.

The alternative (Qt, GTK, etc.) would provide more built-in widgets but add a
massive dependency, slower build times, and less control over the rendering pipeline.
For a node graph editor where everything is custom-drawn anyway, ImGui is ideal.

### Why Three Editor Layers?

```
FlowEditorWindow  →  Editor2Pane  →  VisualEditor
(app chrome)         (semantics)      (rendering)
```

1. **VisualEditor** knows about canvas coordinates, zoom, hover detection, and drawing
   primitives. It doesn't know about graph semantics.

2. **Editor2Pane** knows about the GraphBuilder, node types, pin connections, and
   wire logic. It doesn't know about window management or file I/O.

3. **FlowEditorWindow** handles tabs, file browser, build toolbar, and child processes.
   It delegates all graph work to Editor2Pane instances.

This layering means:
- VisualEditor could be reused for a different graph system
- Editor2Pane could be embedded in a different window framework
- The window can manage multiple editors (tabs) without code duplication

### Why Distance-Based Hover Priority?

The hover system evaluates all elements (pins, wires, nodes) and returns the best
match by distance, with pins getting a priority bias.

This is better than z-order-based hit testing because:
1. Pins are small and hard to click — the bias helps
2. Wires can overlap nodes — distance sorting resolves ambiguity naturally
3. No need to maintain a z-order for every element
4. Works correctly at any zoom level (distances scale with zoom)

## The Wire Philosophy

### Why First-Class Wires?

In the original model, connections were arrays on nodes (`inputs`, `outputs`). Wires
had no identity — they were implicit relationships between pins.

Making wires first-class entities (with their own GUID, metadata, and editor properties)
enables:

1. **Wire selection**: Click a wire to inspect its type, rename it, add probes
2. **Stable inspector subscriptions**: A wire's GUID persists across recompiles, so
   the value inspector can track a specific wire across edit-compile-run cycles
3. **Wire metadata**: Color coding, logging toggles, probe flags, user-assigned names
4. **Decoupled ownership**: Wires don't belong to either endpoint — they're independent
   graph entities

### Why Named Nets?

Named nets (`$osc-signal`, `$freq`) are a higher-level abstraction over wires. A named
net connects all pins that share the same net name, without requiring explicit wire routing.

This is useful for:
1. **Long-distance connections**: Connecting nodes far apart without visual wire spaghetti
2. **Bus-like patterns**: Multiple consumers reading the same signal
3. **Modularity**: Reference a signal by name instead of by wire topology

Named nets are displayed in the nets editor panel, providing a table-of-contents view
of all named signals in the instrument.

## The Runtime Philosophy

### Why Process Isolation?

The editor and runtime run in separate processes (attoflow.exe and attohost.exe) for
one critical reason: **a crashing instrument must not crash the editor**.

Audio programming inevitably involves buffer overruns, division by zero, and infinite
loops. In a single-process model, any of these would kill the editor, losing unsaved work.
In the two-process model, the host crashes, the editor reports the error, and the user
can fix their instrument and try again.

### Why wire<T> Instead of Raw Values?

In release builds, `wire<T>` is a zero-cost typedef to `T`. In inspect builds, it wraps
the value with metadata (name, GUID) and notifies an IPC channel on assignment.

This dual-mode design means:
- **Release performance**: No overhead. The generated code runs as fast as hand-written C++.
- **Debug observability**: Every signal can be monitored, recorded, and overridden from
  the editor without modifying the instrument code.

The alternative (always wrapping, or separate debug/release codegen paths) would either
add permanent overhead or require maintaining two code generators.

## Trade-Offs and Known Limitations

### args String vs Structured Fields

`node.args` is still a string that gets tokenized at runtime in ~40+ call sites. This is
the single largest source of technical debt. The fix (structured pre-parsed fields) is
planned but is a large refactor touching all node type handling.

The original design used strings for simplicity: quick to serialize, easy to display,
simple to parse. But as the type system grew more sophisticated, the string became a
liability — it encodes semantic information (type names, variable names, field lists)
in an untyped format that requires error-prone tokenization.

### Nested Lambda Scope Leakage

When a `lock` node's lambda body shares nodes with an outer stored lambda, the
parameter collector incorrectly identifies outer parameters as belonging to the
inner lambda. This is because `collect_lambda_params` doesn't respect lambda
boundaries.

The proper fix requires a pre-inference "lambda ownership" pass that assigns each
node to its innermost enclosing lambda. This is non-trivial because nodes can be
shared between scopes (the graph is not a tree).

### Single-Threaded Inference

The inference engine runs single-threaded on the main thread. For large instruments
(100+ nodes), this can cause a noticeable pause. Moving inference to a background
thread would require making the graph immutable during inference (copy-on-write)
or using fine-grained locking.

### No Undo/Redo

The editor currently has no undo/redo system. The mutation batching infrastructure
(`edit_start`/`edit_commit`) provides the foundation for command-pattern undo, but
the actual command recording is not yet implemented.

### Web Compilation Latency

The planned web deployment relies on a remote compile server, introducing network
latency in the edit-run cycle. For audio instruments, this latency may be
unacceptable. The long-term fix is either a WASM codegen backend in attoc
(eliminating the server) or an in-browser interpreter for rapid prototyping with
a compile step for production.

## Guiding Principles

1. **Instruments first**: Every feature is evaluated by whether it helps people
   build better instruments.

2. **Real-time or nothing**: Audio processing at 48kHz with <10ms latency is a
   hard constraint. Any design that can't meet this is rejected.

3. **Graph as truth**: The FlowGraph is the canonical representation. There is no
   separate AST, no hidden state. What you see in the editor is what gets compiled.

4. **Types prevent errors**: The more information encoded in the type system, the
   more errors are caught at edit time rather than at runtime.

5. **Zero-cost abstractions**: Features like `wire<T>` that add development-time
   capabilities without production-time overhead.

6. **Simple until proven insufficient**: Start with strings, switch to structured
   types when the strings become a problem. Start with single-threaded, add
   parallelism when profiling demands it.

7. **Process boundaries are trust boundaries**: The editor doesn't trust the
   runtime. The compile server doesn't trust user input. Each boundary has
   explicit validation.
