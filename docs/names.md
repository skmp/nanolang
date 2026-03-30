# Organic Assembler — On Names

Naming things is one of the two hard problems in computer science. The other is cache
invalidation. This project has both (see: TypePool interning and dirty tracking), but
this document is about the first.

Names in the Organic Assembler are not labels slapped on after the fact. They are
*decisions* — small acts of design that accumulate into the character of the system.
A bad name is a lie that everyone agrees to believe. A good name is a truth that makes
other truths easier to see.

---

## Part I: The Names That Were

### The Great Rename

The project was born as **nanolang**. The editor was **nanoflow**. The compiler was
**nanoc**. The runtime was **nanoruntime**. The file format was `nanoprog@0`.

Then the project grew, and "nano" stopped fitting. The language was not nano — it had
a rich type system, bidirectional inference, lambda capture, compile-time metaprogramming.
The editor was not nano — it had mutation batching, dirty tracking, observer patterns,
three-layer architecture. Nothing about the system was small.

The rename to **atto** happened in a single phase (Phase 2b, March 24). Every file,
every variable, every comment, every format string. `nanolang` → `attolang`.
`nanoflow` → `attoflow`. `nanoc` → `attoc`. `nanoprog@0` → `attoprog@0`.

The version markers tell the story:
```
nanoprog@0  →  nanoprog@1  →  attoprog@0  →  attoprog@1  →  instrument@atto:0
```

Each migration step is a function in the serializer. The old names are not forgotten —
they live in the migration code, translating the past into the present. The system can
still load a `nanoprog@0` file. It just calls it `instrument@atto:0` when it saves.

"Atto" means 10⁻¹⁸. It is smaller than "nano" (10⁻⁹) by nine orders of magnitude.
The irony is intentional: as the project grew more complex, its name became more humble.
The best names have a sense of humor about themselves.

### The Wire That Became a Net

In early commits, connections between nodes were called **wires**. A wire connected an
output pin to an input pin. Simple, physical, intuitive.

Then named connections appeared — the ability to give a name to a signal so that distant
nodes could share it without visual routing. These were not wires anymore; they were
**nets**. The word comes from electronics: a net is a set of electrically connected
points, regardless of the physical routing.

The rename is visible in the git log:
```
wire -> net in graph_builder
```

This single commit changed the vocabulary of the entire builder layer. `WireBuilder`
became `NetBuilder`. `wire_id` became `net_name`. The old word persisted in the
visual layer — we still *draw* wires on the canvas — but the semantic layer speaks
of nets.

The lesson: **when the concept changes, the name must change with it**. A wire implies
point-to-point. A net implies broadcast. The old name would have been a lie.

### BangInput → BangTrigger, BangOutput → BangNext

Pin directions were originally named from the perspective of the pin itself:
`BangInput` (a pin that receives a bang) and `BangOutput` (a pin that sends a bang).

But this created confusion when reading node definitions. Does "BangInput" mean "the
input that carries the bang signal" or "the bang that inputs to this node"? Both
readings are valid, and that ambiguity is a naming failure.

The rename to `BangTrigger` and `BangNext` resolved the ambiguity by naming what
the pin *does*:
- **BangTrigger** — this pin *triggers* the node's execution
- **BangNext** — this pin fires *next*, continuing the bang chain

The new names are verbs, not nouns. They describe action, not identity. This is
a pattern throughout the codebase: when a name causes confusion, rename it to
describe behavior rather than structure.

### as_Node/Net → as_node/net

A small rename, but it illustrates a principle: **naming conventions are not
optional**. The C++ convention in this project is snake_case for functions. `as_Node`
violates that convention. `as_node` does not. The rename happened in a dedicated
commit with dead code deletion, because naming fixes deserve their own commits.

### graphbuilder → graph_builder

Another convention fix. Compound names use underscores. `graphbuilder` looks like
a single word; `graph_builder` reveals its structure. The underscore is not
punctuation — it is *meaning*. It says: this is two concepts composed.

### editor → window

The top-level editor class was originally called `editor.cpp` / `editor.h`. But it
managed tabs, file browsing, build toolbars, and child processes — it was a *window*,
not an editor. The actual editing happened in `Editor2Pane`. The rename aligned
the filename with its responsibility.

---

## Part II: The Names That Are

### Project and Subsystem Names

| Name | Meaning | Why this name |
|------|---------|---------------|
| **Organic Assembler** | The project as a whole | "Organic" because instruments grow organically from nodes; "Assembler" because the system assembles them into running programs |
| **orgasm** | Repository / directory name | The abbreviation. Memorable, irreverent, honest about the creative pleasure of building instruments |
| **attolang** | The core language library | "atto" (10⁻¹⁸) + "lang" (language). The language is a building block, deliberately small in scope |
| **attoflow** | The visual editor | "atto" + "flow" (dataflow). You author instruments by directing the flow of data |
| **attoc** | The compiler | "atto" + "c" (compiler). Following the Unix tradition: cc, gcc, rustc, attoc |
| **attohost** | The instrument runtime | "atto" + "host" (host process). The runtime hosts the instrument, providing it a world to live in |
| **attostd** | The standard library | "atto" + "std" (standard). Following the C++/Rust convention of a `std` module |

### Type System Names

| Name | Meaning | Why this name |
|------|---------|---------------|
| **TypeExpr** | A type expression | Not "Type" (too generic) or "TypeNode" (implies a tree). TypeExpr is a self-contained expression that fully describes a type |
| **TypeKind** | What a value *is* | "Kind" in the type-theory sense: the shape of a type. Not to be confused with "kind" in Haskell (types of types) |
| **TypeCategory** | How a value is *accessed* | "Category" as in grammatical category. Data, Reference, Iterator, Lambda — these are ways of being, not types of thing |
| **TypePool** | Intern cache for types | "Pool" as in object pool. Types are interned (deduplicated) so that pointer equality implies structural equality |
| **TypeParser** | Recursive-descent type parser | Self-documenting. Parses type strings into TypeExpr structures |
| **ScalarType** | Numeric primitive types | "Scalar" as opposed to "vector" or "composite". u8, s32, f64 are scalars |
| **literal\<T,V\>** | Compile-time constant | "Literal" as in "the literal value 42". T is the type domain, V is the value. The angle brackets echo C++ template syntax |
| **symbol\<name,type\>** | Named reference | "Symbol" as in symbol table. A name that refers to something, carrying both the name and what it resolves to |

### Graph Model Names

| Name | Meaning | Why this name |
|------|---------|---------------|
| **FlowGraph** | The top-level program representation | "Flow" because it is a dataflow graph. Not "Program" (too abstract) or "Scene" (too visual) |
| **FlowNode** | A computation step in the graph | "Node" is the standard graph-theory term. "Flow" prefix distinguishes it from GUI nodes |
| **FlowLink** | A connection between pins | "Link" rather than "edge" because links carry type information and net names — they are richer than mathematical edges |
| **FlowPin** | An input or output on a node | "Pin" from electronics: a connection point on a component. Familiar to anyone who has used a visual programming tool |
| **GraphBuilder** | High-level editing API | "Builder" as in builder pattern — it constructs and modifies graphs with structured operations, not raw mutations |
| **GraphIndex** | Fast lookup structure | "Index" as in database index — it accelerates queries without changing the underlying data |
| **GraphInference** | Type inference engine | Self-documenting. Runs inference over a graph |

### Editor Names

| Name | Meaning | Why this name |
|------|---------|---------------|
| **VisualEditor** | Canvas rendering layer | "Visual" because this layer knows only about drawing — coordinates, shapes, colors |
| **Editor2Pane** | Semantic editing layer | "Editor2" because it is the second-generation editor (replacing editor1). "Pane" because it is one panel in a tabbed window |
| **FlowEditorWindow** | Top-level application window | "Window" because it manages the OS window, tabs, toolbars, and child processes |
| **NodeEditorImpl** | Per-node editor state | "Impl" because it implements the INodeEditor interface. Each node in the graph has one |
| **NetEditorImpl** | Per-net editor state | Same pattern as NodeEditorImpl, but for named nets |

### Sentinel Names

| Name | Meaning | Why this name |
|------|---------|---------------|
| **$empty** | Node sentinel (no real node) | "Empty" because it has no content. The `$` prefix marks it as a system-generated ID |
| **$unconnected** | Net sentinel (no real net) | "Unconnected" because the pin has no wire. Descriptive of the situation, not the object |

### Pin and ID Names

| Name | Meaning | Why this name |
|------|---------|---------------|
| **$auto-\<hex\>** | Auto-generated node ID | "$auto" says "the system chose this". The hex GUID ensures uniqueness |
| **\<node-id\>-in0** | Input pin reference | The dash-separated format encodes structure: which node, which pin, which index |
| **\<node-id\>-out0** | Output pin reference | Same convention as input, for outputs |
| **\<node-id\>-bang0** | Bang trigger pin | Same convention, for bang triggers |
| **$\<name\>** | Named net | The `$` prefix distinguishes net names from node IDs. Short, scannable |

---

## Part III: The Art of Naming

### Rule 1: Name the Concept, Not the Implementation

`TypeCategory` is better than `TypeAccessMode`. The concept is categorical (Data,
Reference, Iterator, Lambda are categories of being), not modal (they don't "switch
modes"). The name should survive a refactor of the implementation.

`FlowGraph` is better than `NodeArray`. The concept is a graph with dataflow semantics.
The implementation happens to use arrays, but that is incidental. If the implementation
changed to use a hash map, `FlowGraph` would still be correct; `NodeArray` would be a lie.

### Rule 2: Prefer Verbs for Actions, Nouns for Things

- `mark_dirty()` — verb, describes what happens
- `is_dirty()` — adjective (via "is"), describes a state
- `edit_start()` / `edit_commit()` — verbs, describe the operation
- `FlowGraph` — noun, names a thing
- `GraphBuilder` — noun (agent noun: "one who builds"), names a thing that acts

Do not use verbs for things or nouns for actions. `BuildGraph` is confusing: is it a
command (build the graph!) or a noun (a built graph)? `GraphBuilder` is unambiguous.

### Rule 3: Abbreviate Only When the Abbreviation Is More Familiar Than the Full Name

- `f32` > `float32` > `single_precision_float` — everyone knows f32
- `u8` > `uint8` > `unsigned_eight_bit_integer` — everyone knows u8
- `ffi` > `foreign_function_interface` — everyone knows FFI
- `gui` > `graphical_user_interface` — everyone knows GUI
- `expr` > `expression` — used so often that brevity matters
- `decl` > `declaration` — same reason

But:
- `FlowGraph` > `FG` — the abbreviation is meaningless
- `TypeCategory` > `TC` — the abbreviation is meaningless
- `GraphBuilder` > `GB` — the abbreviation is meaningless

The test: if someone seeing the abbreviation for the first time cannot guess its meaning,
spell it out.

### Rule 4: Use Prefixes for Namespacing, Suffixes for Classification

- `Flow` prefix: FlowGraph, FlowNode, FlowLink, FlowPin — all part of the flow model
- `Arg` prefix: ArgNet2, ArgNumber2, ArgString2, ArgExpr2 — all argument types
- `Node` prefix: NodeTypeID, NodeType, NodeEditorImpl — all node-related
- `Type` prefix: TypeExpr, TypeKind, TypeCategory, TypePool, TypeParser — all type-related
- `2` suffix: FlowArg2, Editor2Pane, node_types2.h — second generation (replaces v1)
- `Ptr` suffix: TypePtr, ExprPtr, FlowArg2Ptr — smart pointer aliases
- `Bang` suffix: StoreBang, CallBang, ExprBang — bang-chain variants of base nodes
- `Impl` suffix: NodeEditorImpl, NetEditorImpl — interface implementations

Prefixes group by domain. Suffixes classify within a domain. Together they create a
two-dimensional naming grid that is scannable and predictable.

### Rule 5: The Name Should Survive the Rename

Names that describe *what* something is are more stable than names that describe *where*
it lives or *when* it was created.

- `GraphBuilder` will still make sense if the file moves from `src/atto/` to `src/core/`
- `Editor2Pane` will stop making sense when Editor3 arrives (but by then it will be renamed)
- `FlowGraph` will still make sense if the serialization format changes
- `nanoprog@0` stopped making sense when the language was renamed (and was migrated)

When choosing a name, ask: "Will this name still be true after the next refactor?"

### Rule 6: Sigils Are Names Too

The type category sigils are single-character names:

| Sigil | Name | Mnemonic |
|-------|------|----------|
| `%` | Data | Percent — the "default" (as in "100% of values are data") |
| `&` | Reference | Ampersand — borrowed from C/C++ reference syntax |
| `^` | Iterator | Caret — points "up" to the current position |
| `@` | Lambda | At — a function "at" a callable address |
| `#` | Enum | Hash — enumerated, numbered items |
| `!` | Bang | Exclamation — an imperative command, "do this!" |
| `~` | Event | Tilde — a wave, something that comes and goes |

Each sigil was chosen because it has a visual or linguistic mnemonic. `!` for bang is
the most natural: an exclamation mark *is* a bang. `~` for event evokes a signal wave.
`&` for reference comes from C++ and needs no explanation.

Sigils are the shortest possible names. They work because the set is small (seven),
the context is constrained (type strings), and the mnemonics are strong. Do not add
new sigils without strong justification — the cognitive load scales faster than the
count.

### Rule 7: Error Messages Are Names for Problems

Error messages in the Organic Assembler follow a convention:
- Lowercase
- Descriptive
- Context-included

```
"type mismatch: expected f32, got u8"
"undefined symbol: foo"
"cannot iterate over non-collection type f32"
```

Each error message *names* the problem in a way that suggests the fix. "type mismatch:
expected f32, got u8" tells you exactly which types conflicted and which direction the
expectation flows. "undefined symbol: foo" tells you which name is missing. The error
message is a name for a specific situation, and like all names, it should be precise,
honest, and helpful.

### Rule 8: File Names Are Architecture

The file organization of the codebase is itself a naming system:

```
src/atto/         — core language (no external deps)
src/attoc/        — compiler (depends on atto)
src/attoflow/     — editor (depends on atto, SDL3, ImGui)
src/attoruntime/  — runtime (depends on atto)
attostd/          — standard library (.atto files)
scenes/           — example instruments
tests/            — automated tests
docs/             — documentation
```

Each directory name is a boundary declaration. `src/atto/` says: "everything in here
has zero external dependencies." `src/attoflow/` says: "everything in here can use
SDL3 and ImGui." The directory name is not just organization — it is a *constraint*
that the build system enforces.

When you create a new file, its location is its first name. Choose carefully: putting
a file in `src/atto/` is a promise that it will never depend on SDL3. Putting it in
`src/attoflow/` is a promise that it is editor-specific.

---

## Part IV: Naming as Practice

### The Naming Moment

Every variable, function, type, file, commit message, and error message is a naming
moment. Most naming moments are small and forgettable — `int i`, `auto& node`, `bool ok`.
These are fine. Not every name needs to be profound.

But some naming moments are *load-bearing*. The name `FlowGraph` shapes how everyone
thinks about the data model. The name `instrument` shapes how everyone thinks about
the programs. The name `bang` shapes how everyone thinks about execution order. These
names, once established, become the vocabulary of the project. Changing them requires
a Great Rename.

Recognize the load-bearing naming moments. Spend time on them. Sleep on them. The
name you choose today will be repeated ten thousand times in code, docs, conversations,
and commit messages. It will shape how people think about the concept it names.

### When to Rename

Rename when:
- The name is actively misleading (wire → net, when connections became broadcast)
- The name violates an established convention (as_Node → as_node)
- The concept has genuinely changed (editor → window, when the class became a window manager)

Do not rename when:
- You merely *prefer* a different name (preference is not justification)
- The name is "not perfect but fine" (perfection is the enemy of stability)
- The rename would touch 50+ files for marginal clarity (blast radius matters)

A rename is a coordinated migration. Do it in one commit, touch all references, update
docs and comments. Half-renamed code is worse than badly-named code — at least badly-named
code is consistent.

### The Compound Name Test

When a name becomes a compound (`GraphBuilderEntry`, `FlowNodeBuilderPtr`,
`INodeEditorImpl`), check if the compounds are load-bearing:

- `GraphBuilder` + `Entry` = a thing in the graph builder. Good: both parts carry meaning.
- `FlowNodeBuilder` + `Ptr` = a pointer to a flow node builder. Good: `Ptr` is a standard suffix.
- `I` + `NodeEditor` + `Impl` = an implementation of the node editor interface. Good: `I` prefix for interfaces, `Impl` suffix for implementations.

But beware compound names that merely concatenate context:
- `EditorGraphBuilderNodeArgNet2Ptr` — too long, too many levels of nesting
- If a name requires more than three components, the concept may need decomposition

### The Instrument Name

The most important name in the Organic Assembler is **instrument**. Not "program." Not
"project." Not "sketch" or "patch" or "scene."

"Instrument" carries specific connotations:
- A musical instrument is played, not executed
- A scientific instrument measures, not computes
- An instrument has a *purpose* — it does one thing well
- An instrument is *physical* — it exists in the world, not just in memory
- An instrument is *expressive* — the same instrument in different hands produces
  different results

Every `.atto` file is an instrument. The system is an operating system *for* instruments.
This name frames the entire project. It tells the user: you are not writing code, you
are building an instrument. Treat it with the care and intentionality that word implies.

---

*The name is the first line of documentation. It is the shortest possible explanation.
It is the thing that makes everything else easier to understand — or harder, if chosen
poorly. Name things right, and the code explains itself. Name things wrong, and no
amount of documentation can save you.*
