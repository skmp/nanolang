# Introducing the Organic Assembler: An Operating System for Instruments

## What Is the Organic Assembler?

The Organic Assembler is an operating system for instruments. Not applications, not programs, not sketches -- instruments. That word choice is deliberate and load-bearing. A musical instrument is played, not executed. A scientific instrument measures, not computes. An instrument has a purpose -- it does one thing well. An instrument is expressive -- the same instrument in different hands produces different results.

Every instrument in the Organic Assembler is a self-contained `.atto` file that defines some interactive, real-time behavior -- typically audio synthesis, visual display, or both. The system compiles these programs, runs them with hot-reload support, and provides a visual node-graph editor for authoring them. The project's repository is called "orgasm" -- short for Organic Assembler. Memorable, irreverent, honest about the creative pleasure of building instruments.

The "Organic" in the name reflects how instruments grow. You don't write an instrument top-to-bottom like a text program. You grow it organically: place a node, connect a wire, hear the result, add another node, adjust a parameter, watch types propagate through the graph in real time. The "Assembler" reflects what the system does: it assembles your graph of nodes into a running, real-time program.

## The Core Idea: Visual Dataflow for Real-Time Systems

The fundamental insight behind the Organic Assembler is that instruments are inherently about signal flow. Audio signals, control signals, event triggers, and UI state all flow between processing nodes. A textual programming language forces you to name intermediate values and linearize a fundamentally parallel, graph-shaped computation. A visual dataflow graph shows topology directly -- you see what connects to what. Wires are the variables. Related nodes are placed near each other. Unconnected subgraphs run independently. You change a connection, you see (and hear) the result immediately.

But pure visual programming has a weakness: complex mathematical expressions are awkward as node graphs. Nobody wants to wire up `a + b * sin(c)` as six separate nodes. So the Organic Assembler supports inline expressions within nodes. You can type `$0 + $1 * sin($2)` directly inside an expression node, where `$0`, `$1`, `$2` refer to the node's input pins. The graph handles topology and signal routing; inline expressions handle the math. It's a hybrid that takes the best of both worlds.

## The Language: attolang

The language is called attolang. "Atto" means 10 to the minus 18 -- smaller than "nano" (10 to the minus 9) by nine orders of magnitude. The project was originally called "nanolang," but as the system grew more complex, its name became more humble. The best names have a sense of humor about themselves.

attolang has a rich type system with two orthogonal dimensions. The first dimension is TypeKind -- what a value IS. Scalars like u8 through s64, f32 and f64. Booleans, strings. Containers like vector, map, list, set, queue. Fixed-size arrays and tensors. Functions and structs. Symbols and meta-types for compile-time abstractions.

The second dimension is TypeCategory -- HOW a value is accessed. This is where it gets interesting. There are seven categories, each with its own single-character sigil:

- Percent sign for Data -- plain values, the default
- Ampersand for Reference -- mutable locations, borrowed from C++ syntax
- Caret for Iterator -- a position into a container, pointing "up" to the current element
- At-sign for Lambda -- callable function references, a function "at" an address
- Hash for Enum -- enumerated, numbered items
- Exclamation mark for Bang -- trigger signals carrying no data, an imperative command: "do this!"
- Tilde for Event -- something that comes and goes, like a wave

These two dimensions are orthogonal. An ampersand-vector-of-f32 is a mutable reference to a vector of floats. An at-sign-f32-arrow-f32 is a callable that takes and returns a float. An exclamation mark is a bang trigger. This grid of kinds and categories is the DNA of every value in the system.

## The Bang Chain: Making Dataflow Imperative

One of the most distinctive features of attolang is the bang chain. In a pure dataflow language, everything happens simultaneously -- data flows through the graph, and every node computes when its inputs are ready. But real-time instruments need side effects: storing values to variables, appending to collections, mixing audio output. Side effects need ordering. You need to say "do this, THEN do that."

Bang nodes -- nodes postfixed with an exclamation mark, like `store!`, `append!`, `iterate!`, `lock!` -- have explicit execution ordering via bang signals. A bang signal flows from one node's "next" output to another node's "trigger" input, creating a chain. This chain is a sequential program embedded in a parallel graph.

The naming is perfect. An exclamation mark IS a bang. And the pin names tell you exactly what they do: BangTrigger triggers the node's execution; BangNext fires next, continuing the chain. These are verbs, not nouns -- they describe action, not identity.

## Bidirectional Type Inference

attolang's type inference engine is bidirectional, which is unusual and powerful. In most languages, types flow in one direction: the producer determines the type, and the consumer must accept it. But in a visual dataflow graph, producers and consumers are peers. There's no clear "this comes first" ordering.

Consider a literal `0` in an expression. It could be u8, u16, u32, u64, f32, or f64. The correct type comes from the CONSUMER -- whatever that zero flows into determines what type it should be. Without backpropagation from consumer to producer, every literal would need an explicit type annotation.

The inference engine runs multiple passes in a fixed-point loop: forward-propagate types through wires, infer expression nodes, backpropagate from consumers -- and repeat until nothing changes. Types only become more specific, never less, which guarantees convergence. Most graphs converge in two or three iterations.

This is what makes the editing experience feel alive. As you connect nodes, types resolve in real time. You watch pin types update live in the editor. The type system is your development partner, catching errors at edit time rather than at runtime.

## Literal Types and Symbols: Compile-Time Power

attolang makes compile-time values first-class citizens of the type system. `literal<T,V>` represents a compile-time constant with type domain T and value V. The number `42` isn't just "an integer" -- it's `literal<unsigned<?>,42>`, where the question mark means "the exact unsigned type will be resolved from context." This means the type system knows which values are compile-time constants, enabling optimizations and metaprogramming that would otherwise require a separate constant-folding pass.

Symbols work similarly. When you type `sin` in an expression, the result isn't immediately "the sine function." It's `symbol<sin,(f32)->f32>` -- a first-class named reference that carries both its name and what it resolves to. An identifier that isn't in the symbol table becomes `undefined_symbol<name>` -- it doesn't error immediately, it errors only when something tries to evaluate it. This means partially-written instruments don't explode with cascading errors. You can have unfinished nodes and the system stays calm.

## The Four Subsystems

The Organic Assembler has four major subsystems, each independently buildable with well-defined boundaries.

**attolang** is the core language library. It defines the data model, type system, expression parser, inference engine, serialization, and graph builder. It has zero external dependencies -- just the C++20 standard library. This is a deliberate constraint: the core is pure computation, no I/O, no GUI, no platform dependencies.

**attoc** is the compiler. It transforms `.atto` files into self-contained C++ projects. The pipeline loads the file, runs type inference, validates, then generates C++ code: struct definitions from `decl_type` nodes, a program class with event handlers, and a complete CMakeLists.txt. Each compiled instrument is an independent project.

The decision to compile to C++ rather than interpret or compile to LLVM IR was pragmatic. Audio processing at 48kHz with low latency demands native speed -- an interpreter would add unacceptable overhead. C++ gives access to every audio and graphics library without FFI bridges. Standard debuggers work on the output. And the generated code is human-readable, which aids debugging. When something goes wrong at runtime, you read the generated C++ to understand what happened.

**attoflow** is the visual node editor, built on SDL3 and Dear ImGui. It has a three-layer architecture: VisualEditor handles canvas rendering (pan, zoom, hover detection, drawing primitives), Editor2Pane wraps the GraphBuilder and implements graph editing semantics (wire connections, node manipulation), and FlowEditorWindow manages the application chrome (tabs, file browser, build toolbar, child processes). This layering means each layer could be reused independently.

**attohost** is the instrument runtime. It runs instruments in a separate process from the editor, so a crashing instrument doesn't take down your editing session. It supports hot-reload: the compiler generates a DLL, the host loads it, and when you edit and recompile, the host unloads the old DLL and loads the new one. Communication between editor and host happens via named pipes for live value inspection.

## The Graph as Truth

A fundamental principle: the FlowGraph is the canonical representation. There is no separate abstract syntax tree, no hidden intermediate form, no shadow copy. What you see in the editor is what gets compiled. What you read in the `.atto` file is what the editor displays.

This single-source-of-truth principle eliminates an entire class of synchronization bugs. In many development environments, the visual representation can drift from the actual program. In the Organic Assembler, that's structurally impossible. The graph IS the program.

## The .atto File Format

Instruments are stored in a TOML-like format. Each file starts with a version marker (`instrument@atto:0`), followed by node definitions and link definitions. Node IDs are hex GUIDs prefixed with `$auto-`. Pin IDs embed their parent node's ID plus a pin name. Named nets use a dollar-sign prefix.

The version history tells a story of evolution: `nanoprog@0` to `nanoprog@1` to `attoprog@0` to `attoprog@1` to `instrument@atto:0`. Each step has a migration function in the serializer. The system can still load the very first format ever created -- it just saves in the latest format. Old names live on in the migration code, translating the past into the present.

## Anatomy of an Instrument

Every instrument has structural elements that map to linguistic categories.

Declarations are the nouns: `decl_type` defines a struct type, `decl_var` declares a mutable variable that persists across frames, `decl_event` declares an event, `decl_import` imports a standard library module, `ffi` declares an external C function.

Expressions are the adjectives and adverbs: `expr` computes values from inputs, `new` constructs structs, `dup` duplicates values to branch wires, `cast` converts types, `select` chooses between values based on conditions.

Bang nodes are the verbs: `store!` writes values, `append!` adds to collections, `iterate!` loops, `lock!` acquires mutexes, `call!` invokes functions with side effects, `output_mix!` writes audio output.

Events are the stimuli: `on_key_down!`, `on_key_up!`, custom events. An instrument does nothing until an event fires. The audio callback fires 48,000 times per second. Key events fire on user input.

## The Audio Callback: 48,000 Times Per Second

Audio runs at 48kHz. Each invocation of the audio callback produces one sample per channel. The pattern is: read state from variables declared outside the callback, compute pure math (oscillators, filters, envelopes), write updated state via `store!`, and output via `output_mix!`.

The bang chain within the audio callback is the inner loop of the instrument. Every node in this chain runs 48,000 times per second. This is where the real-time constraint bites hardest: no allocations, no string operations, no collection mutations in the hot path. Less than 21 microseconds per sample. Human ears detect latency above about 10 milliseconds as a distinct echo -- this is a physical requirement, not a soft goal.

The klavier instrument -- a piano synthesizer and the project's most comprehensive test case -- uses oscillator synthesis with per-key state, envelope generators, and additive audio mixing. It exercises complex types, nested lambdas, collections, events, audio, GUI, and FFI. If klavier loads, infers, compiles, and runs, the system is healthy.

## Design Patterns That Bear Load

The codebase uses specific design patterns that evolved from real problems.

The Sentinel pattern pre-allocates always-valid stand-in objects (`$empty` for nodes, `$unconnected` for nets) so that code can unconditionally access references without null checks. Every pin always has a valid node and net reference. This eliminates the most common source of null pointer bugs.

The Observer pattern connects the model to the editor without creating reverse dependencies. The core library defines interfaces (IGraphEditor, INodeEditor, INetEditor). The editor implements them. The model never imports editor code. When the graph changes, observers are notified.

Mutation Batching brackets multi-step operations with `edit_start()` and `edit_commit()`. All observer notifications are queued and fired only on commit, so observers always see consistent state. Without this, connecting a wire would fire partial notifications as each micro-step completes.

Dirty Tracking cascades through three levels: arg to node to graph. A pin mutation marks its owning node dirty, which marks the graph dirty. Layout changes (dragging a node) are tracked separately from semantic changes (editing an expression), so moving a node doesn't trigger expensive type re-inference.

The TypePool interns all parsed types so that type equality reduces to pointer comparison. No redundant parsing of the same type string. Common types like f32 and bool are always available without lookup.

## The Naming Philosophy

Names in the Organic Assembler are not labels slapped on after the fact. They are decisions -- small acts of design that accumulate into the character of the system. The project has a detailed philosophy about naming that permeates every level.

When connections between nodes changed from point-to-point to broadcast, "wire" was renamed to "net" -- because a net (from electronics) is a set of electrically connected points regardless of physical routing. The old word would have been a lie.

When pin directions caused confusion (does "BangInput" mean the input that carries a bang, or the bang that inputs to this node?), they were renamed to BangTrigger and BangNext -- verbs that describe what the pins DO, not what they ARE.

Error messages follow the same philosophy: lowercase, descriptive, context-included. "type mismatch: expected f32, got u8" names the problem in a way that suggests the fix. File names are architecture: putting a file in `src/atto/` is a promise it will never depend on SDL3. Sigils are the shortest possible names, working because the set is small, the context is constrained, and the mnemonics are strong.

## Differential Everything

The project has a deep relationship with the concept of differentiation -- rates of change, deltas, accumulation -- at every level.

At the audio level, an oscillator works by accumulating a phase delta each sample. The phase step is the derivative of the waveform position. The `store!` node performs numerical integration. Frequency IS the derivative of phase.

At the inference level, the fixed-point loop iterates until the delta between iterations is empty. Types move monotonically from generic to specific. The inference engine is structurally identical to forward-plus-reverse-mode automatic differentiation, with types playing the role of values and gradients.

At the editing level, dirty tracking computes the delta between "state before edit" and "state after edit" as a deduplicated set of changed items. The wire cache rebuilds only when the underlying model has a non-zero delta.

At the version level, each format migration encodes the difference between two versions. Loading a `nanoprog@0` file applies a chain of composed deltas.

Even the git commit history reflects differential development. "towards lambdas," "more graphbuilder work," "not perfect, but progress is progress" -- these are differential checkpoints capturing known-better intermediate states. The question marks in commit messages like "progress in literals?" reveal uncertainty about whether a delta is positive.

## The Development Story

The project was built in an intense five-day sprint. Day one (March 23, 2026): 16 commits establishing the foundation -- FlowGraph model, expression parser, type system, code generator, visual editor, first instruments. Day two: 28 commits deepening the type system with literal types, symbol types, shadow nodes, and lambda support. Day three: 36 commits at peak velocity, building out the type system, GraphBuilder, and Editor2. Day four: 72 commits -- the highest output day -- completing Editor2 features. Day five: 6 larger integration commits connecting everything.

The commit messages tell a story. Feature introductions name the concept: "Observer Pattern," "multi select." Rename commits use arrow notation: "wire -> net in graph_builder." Incremental progress is honest: "towards X," "more X work," "not perfect, but progress is progress." The emotional honesty is a feature, not a bug.

## The Web Vision

The future includes web deployment. The editor, built on SDL3 and ImGui, can be compiled to Emscripten for browser execution. A compile server on a hardened Raspberry Pi would accept `.atto` files, compile them through attoc to C++, then through Emscripten to WebAssembly, and send the result back to the browser.

The server would be containerized with no network access inside, a 60-second timeout, 512MB RAM limit, and read-only root filesystem. Only `.atto` goes in (small attack surface), controlled C++ comes out. This is the "process boundaries are trust boundaries" principle in action.

## Why It Matters

The Organic Assembler sits at an intersection that few projects occupy: a system that is both deeply technical (bidirectional type inference, compile-time metaprogramming, multi-pass fixed-point algorithms) and fundamentally creative (building instruments, making sound, exploring expression).

The type system catches errors at edit time so you can focus on the music, not the debugging. The visual editor shows you the topology of your signal flow so you can reason spatially. The real-time compilation means you hear changes as you make them. The bang chain gives you precise control over execution order without abandoning the parallel nature of dataflow.

Every feature, every refactor, every design decision is evaluated against one question: does this help someone build a better instrument? If the answer is no, the work is not justified. The system exists to serve instruments, not itself.

The best instruction is the one you no longer need to give, because the system makes the right choice obvious. That is what the type system, the patterns, and the architecture are for: making the right choice the easy choice.

---

*The Organic Assembler is MIT-licensed and available at github.com/nilware-io/orgasm.*
