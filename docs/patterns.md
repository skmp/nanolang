# Organic Assembler — Design Patterns

This document catalogs the recurring design patterns, idioms, and structural motifs
found throughout the Organic Assembler codebase.

## 1. Sentinel Pattern

**Problem**: Many graph operations return references to nodes or nets that might not
exist. Checking for null at every call site is error-prone and verbose.

**Solution**: Pre-allocate sentinel objects (`$empty`, `$unconnected`) that serve as
always-valid stand-ins for missing entities.

```cpp
// Sentinels created once at graph builder initialization
void ensure_sentinels() {
    // $empty — default node (returned when no real node is assigned)
    // $unconnected — default net (returned when no real net is assigned)
}

// Every FlowArg2 always has valid references
struct FlowArg2 {
    FlowNodeBuilderPtr node_;   // always valid ($empty if unassigned)
    NetBuilderPtr net_;         // always valid ($unconnected if unassigned)
};
```

**Usage**: Code can unconditionally call `arg->node()->id()` without null checks.
The sentinel returns safe defaults (empty strings, zero positions, etc.).

**When to use**: Any time an object reference is "optional" but accessed frequently.
Sentinels eliminate the most common source of null pointer bugs.

**Trade-off**: Requires `is_sentinel()` checks in the few places where the distinction
matters (serialization, rendering).

---

## 2. Observer Pattern (via Interfaces)

**Problem**: The graph model is shared between the compiler and editor. The editor
needs to react to graph changes (re-render, re-layout), but the model shouldn't
depend on UI code.

**Solution**: Abstract observer interfaces defined in the core library, implemented
by the editor.

```cpp
// Core library defines the interface
struct IGraphEditor {
    virtual std::shared_ptr<INodeEditor> node_added(
        const FlowNodeBuilderPtr& node) = 0;
    virtual void node_removed(const NodeId& id) = 0;
    virtual std::shared_ptr<INetEditor> net_added(
        const NetBuilderPtr& net) = 0;
    virtual void net_removed(const NodeId& id) = 0;
};

struct INodeEditor {
    virtual void on_dirty() = 0;
    virtual void on_layout_dirty() = 0;
};

struct INetEditor {
    // ...
};
```

```cpp
// Editor implements the interface
class Editor2Pane : public IGraphEditor {
    std::shared_ptr<INodeEditor> node_added(
        const FlowNodeBuilderPtr& node) override {
        // Create visual representation, cache layout
        return std::make_shared<NodeEditorImpl>(node);
    }
};
```

**Registration**: Editors register via weak pointers to avoid preventing cleanup:

```cpp
void GraphBuilder::add_editor(std::weak_ptr<IGraphEditor> editor);
```

**When to use**: Whenever a core data structure needs to notify UI or other
subsystems of changes without depending on them.

---

## 3. Mutation Batching

**Problem**: A single user action (connecting a wire, deleting a node) may require
multiple internal mutations. Firing observer notifications after each micro-mutation
causes flickering, inconsistent state, and redundant re-inference.

**Solution**: Bracket multi-step operations with `edit_start()` / `edit_commit()`.
All observer callbacks are queued and fired in order only on commit.

```cpp
void GraphBuilder::edit_start() {
    if (!mutations_.empty())
        throw std::logic_error("edit_start: uncommitted mutations");
}

void GraphBuilder::add_mutation_call(void* ptr, std::function<void()>&& fn) {
    mutations_.emplace_back(ptr, std::move(fn));
}

void GraphBuilder::edit_commit() {
    auto batch = std::move(mutations_);
    for (auto& [_, fn] : batch)
        fn();
}
```

**Usage**:
```cpp
gb->edit_start();
gb->add_node(...);         // queues notification
gb->add_link(...);         // queues notification
gb->edit_commit();         // fires both notifications
```

**When to use**: Any time multiple related changes should appear atomic to observers.

**Invariant**: `edit_start()` throws if there are uncommitted mutations — this catches
cases where a previous edit was abandoned without committing.

---

## 4. Dirty Tracking (Three-Level Cascade)

**Problem**: The editor needs to know *what* changed to minimize re-rendering and
re-inference work. A global "something changed" flag is too coarse.

**Solution**: Three levels of dirty flags that cascade upward:

```
FlowArg2 (pin/arg) ──dirty──> FlowNodeBuilder (node) ──dirty──> GraphBuilder (graph)
```

```cpp
// Arg level: any mutation marks the arg dirty
void FlowArg2::mark_dirty() {
    // Mark owning node dirty
    node_->mark_dirty();
}

// Node level: dirty means "re-render this node"
void FlowNodeBuilder::mark_dirty() {
    dirty_ = true;
    // Notify node editor
    if (auto ed = editor_.lock())
        ed->on_dirty();
}

// Layout-only dirty: position changed but semantics didn't
void FlowNodeBuilder::mark_layout_dirty() {
    layout_dirty_ = true;
    // Does NOT cascade to graph (no re-inference needed)
    if (auto ed = editor_.lock())
        ed->on_layout_dirty();
}
```

**When to use**: Any system with hierarchical state where changes at one level
affect ancestors.

**Key insight**: Layout changes (dragging a node) are tracked separately from
semantic changes (modifying an expression). This prevents node movement from
triggering expensive type re-inference.

---

## 5. Discriminated Union (Kind Enum + as_* Methods)

**Problem**: A set of related types share a common base but have different fields
and behaviors. `std::variant` works for simple cases but doesn't support
`shared_from_this()` or deep inheritance.

**Solution**: A base class with a `kind()` enum and `as_*()` downcasting methods.

```cpp
enum class ArgKind : uint8_t { Net, Number, String, Expr };

struct FlowArg2 : std::enable_shared_from_this<FlowArg2> {
    ArgKind kind() const { return kind_; }
    bool is(ArgKind k) const { return kind_ == k; }

    std::shared_ptr<ArgNet2> as_net();
    std::shared_ptr<ArgNumber2> as_number();
    std::shared_ptr<ArgString2> as_string();
    std::shared_ptr<ArgExpr2> as_expr();

protected:
    FlowArg2(ArgKind kind, ...);
private:
    ArgKind kind_;
};

// Concrete types inherit from the base
struct ArgNet2 : FlowArg2 { /* net-specific fields */ };
struct ArgNumber2 : FlowArg2 { /* number-specific fields */ };
```

**Compared to std::variant**:
- Supports `shared_from_this()` (variants can't)
- Supports inheritance-based polymorphism
- More explicit control over memory layout
- Trade-off: manual dispatch instead of `std::visit`

**When to use**: When the types need shared ownership, self-references, or when
more than 4-5 alternative types make `std::variant` unwieldy.

---

## 6. Variant-Based Results

**Problem**: Functions that parse or construct objects can fail. Exceptions are too
heavy for expected failures. Error codes lose type information.

**Solution**: Return a `std::variant` of success type and error type.

```cpp
using BuilderResult = std::variant<
    std::pair<NodeId, FlowNodeBuilder>,
    BuilderError
>;

using ParseAttoResult = std::variant<
    std::shared_ptr<GraphBuilder>,
    BuilderError
>;

using ParseResult = std::variant<
    std::shared_ptr<ParsedArgs2>,
    std::string  // error message
>;
```

**Usage**:
```cpp
auto result = parse_atto(stream);
if (auto* err = std::get_if<BuilderError>(&result)) {
    report_error(*err);
    return;
}
auto& gb = std::get<std::shared_ptr<GraphBuilder>>(result);
```

**When to use**: Any function that can fail for expected reasons (parsing, validation,
I/O). Reserve exceptions for invariant violations (programmer errors).

---

## 7. Factory Method with Friend Access

**Problem**: Objects should only be created through a central manager (GraphBuilder)
to ensure proper registration, initialization, and tracking.

**Solution**: Private constructors with `friend` access for the factory.

```cpp
struct ArgNet2 : FlowArg2 {
    friend struct GraphBuilder;  // only GraphBuilder can construct

    // Public API
    const std::string& value() const;
    void set_value(const std::string& v);

private:
    ArgNet2(const std::shared_ptr<GraphBuilder>& owner);
};

// Factory method on GraphBuilder
std::shared_ptr<ArgNet2> GraphBuilder::build_arg_net(...) {
    auto arg = std::shared_ptr<ArgNet2>(new ArgNet2(shared_from_this()));
    // register in pin tracking, set up sentinels, etc.
    return arg;
}
```

**When to use**: When object creation involves side effects (registration, tracking)
that must happen consistently.

---

## 8. TypePool Interning

**Problem**: Type expressions are compared by value throughout the codebase (inference,
codegen, editor display). Creating identical TypeExpr objects wastes memory and makes
equality checks expensive.

**Solution**: A TypePool that parses type strings once and caches the result.

```cpp
struct TypePool {
    // Pre-cached common types
    TypePtr t_void, t_bool, t_string;
    TypePtr t_u8, t_u16, t_u32, t_u64;
    TypePtr t_s8, t_s16, t_s32, t_s64;
    TypePtr t_f32, t_f64;

    // Cache for all parsed types
    std::map<std::string, TypePtr> cache;

    TypePtr intern(const std::string& type_str) {
        auto it = cache.find(type_str);
        if (it != cache.end()) return it->second;

        TypeParser parser(type_str);
        auto type = parser.parse();
        if (type) cache[type_str] = type;
        return type;
    }
};
```

**Benefits**:
- Type equality reduces to pointer comparison (`t1 == t2`)
- No redundant parsing of the same type string
- Common types (f32, u32, bool) are always available without lookup

**When to use**: Any system with many small, frequently-compared objects
(types, symbols, identifiers).

---

## 9. Multi-Pass Fixed-Point Inference

**Problem**: In a dataflow graph, types flow in both directions. Node A's output
type depends on node B's input, but node B's input type depends on node A's output.
A single pass can't resolve all dependencies.

**Solution**: Iterate the inference passes until no types change (fixed point).

```cpp
void GraphInference::run() {
    clear_all();
    resolve_pin_type_names();

    bool changed = true;
    while (changed) {
        changed = false;
        changed |= propagate_connections();
        changed |= infer_expr_nodes();
        changed |= propagate_pin_ref_types();
    }

    resolve_lambdas();
    fixup_expr_derefs();
    insert_deref_nodes();
}
```

**Properties**:
- **Monotonic**: Types only become more specific, never less. This guarantees convergence.
- **Order-independent**: The fixed-point result is the same regardless of node ordering.
- **Efficient**: Most graphs converge in 2-3 iterations. Linear chains converge in 1.

**When to use**: Any constraint propagation system where dependencies are cyclic.

---

## 10. Shadow Node Pattern

**Problem**: Inline expressions on nodes (e.g., `store! $value` where `$value` is an
expression) need to participate in type inference and codegen, but they aren't
separate nodes in the user-visible graph.

**Solution**: Auto-generate hidden "shadow" expression nodes at load time.

```cpp
// User writes: store! $my_value
// System generates:
//   1. A shadow expr node with the inline expression
//   2. A link from the shadow node's output to the store! input
//   3. The shadow node is invisible in the editor
```

**Lifecycle**:
- Created at load time (`parse_args()`)
- Participate in inference and codegen
- Removed before serialization
- Invisible in the editor (not rendered)

**When to use**: When user-facing syntax is more compact than the internal
representation, and the translation is mechanical.

---

## 11. Two-Layer Model (Model + Builder)

**Problem**: The serialization format needs a simple, fast data structure. The editor
needs a rich, observable, transactional data structure. These are conflicting
requirements.

**Solution**: Two layers with different trade-offs.

```
FlowGraph (model layer)          GraphBuilder (builder layer)
├── Simple structs               ├── shared_ptr ownership
├── Vectors and strings          ├── Dirty tracking
├── Fast to serialize            ├── Observer interfaces
├── No smart pointers            ├── Mutation batching
├── No callbacks                 ├── Sentinel objects
└── Used by compiler             └── Used by editor
```

The builder wraps the model and adds editor-specific capabilities. The compiler
works directly with the model layer, avoiding builder overhead.

**When to use**: When the same data is consumed by systems with very different
performance and functionality requirements.

---

## 12. Recursive Descent Parsing

**Problem**: Type strings, expressions, and .atto files all need parsing. Each has
different grammar rules but similar parsing needs.

**Solution**: Hand-written recursive descent parsers with a consistent structure.

```cpp
struct TypeParser {
    std::string src;
    size_t pos = 0;
    std::string error;

    bool eof() const { return pos >= src.size(); }
    char peek() const { return eof() ? 0 : src[pos]; }
    char advance() { return eof() ? 0 : src[pos++]; }
    void skip_ws();
    std::string read_ident();

    TypePtr parse();           // entry point
    TypePtr parse_function();  // (args) -> ret
    TypePtr parse_struct();    // {fields}
    TypePtr parse_container(); // name<params>
    TypePtr parse_scalar();    // u8, f32, etc.
};
```

All parsers share this pattern:
- String source + position cursor
- `eof()`, `peek()`, `advance()`, `skip_ws()` primitives
- Error string for diagnostics
- Recursive methods for grammar rules

**Why not a parser generator?** The grammars are small enough that hand-written
parsers are clearer, faster, and easier to debug. Parser generators add a build
step and obscure the grammar.

---

## 13. Static Descriptor Tables

**Problem**: Node types have fixed properties (port names, port types, categories)
that are the same for every instance. Storing this per-node wastes memory.

**Solution**: Static arrays of node type descriptors, indexed by NodeTypeID.

```cpp
static const NodeType2 NODE_TYPES_V2[] = {
    { NodeTypeID::Expr,    "expr",    NodeKind2::Flow,
      { /* input ports */ }, { /* output ports */ },
      /* flags */ },
    { NodeTypeID::New,     "new",     NodeKind2::Flow,
      { /* input ports */ }, { /* output ports */ },
      /* flags */ },
    // ... 38 total
};
```

Each node instance stores only its `NodeTypeID`. All type-specific information
comes from the descriptor table.

**When to use**: When many instances share the same schema and the schema is
known at compile time.

---

## 14. Pin ID as Structured String

**Problem**: Pins need globally unique identifiers for serialization, linking, and
inspector subscriptions. Integer IDs would be compact but opaque.

**Solution**: Structured string IDs with embedded meaning.

```
"934e3b98bb914e95.out0"       — node GUID + output pin name
"$auto-df6e4aa3d0d8d2bc-out0" — auto-generated pin ID
"$a-3F"                       — compact re-ID after import
"$empty"                      — sentinel node
"$unconnected"                — sentinel net
```

Benefits:
- Human-readable (useful for debugging)
- Self-documenting (you can tell which node owns a pin)
- Stable across edits (GUID-based, not index-based)
- Prefixed sentinels ($) are easy to identify

Trade-off: Larger than integer IDs, slightly slower to compare. In practice,
the type pool interning pattern mitigates the comparison cost.

---

## 15. Category Sigil Dispatch

**Problem**: Type categories (Data, Reference, Iterator, Lambda, etc.) need to be
encoded in type strings and dispatched on in rendering, inference, and codegen.

**Solution**: Single-character sigils with parse/render helpers.

```cpp
// Parsing
inline TypeCategory parse_category(char c) {
    switch (c) {
    case '%': return TypeCategory::Data;
    case '&': return TypeCategory::Reference;
    case '^': return TypeCategory::Iterator;
    case '@': return TypeCategory::Lambda;
    case '#': return TypeCategory::Enum;
    case '!': return TypeCategory::Bang;
    case '~': return TypeCategory::Event;
    default:  return TypeCategory::Data;
    }
}

// Rendering (pin shapes)
switch (category) {
case TypeCategory::Data:     draw_circle(pos, radius);    break;
case TypeCategory::Bang:     draw_square(pos, size);      break;
case TypeCategory::Lambda:   draw_triangle(pos, size);    break;
// ...
}
```

The sigils serve triple duty:
1. **Serialization**: Compact type representation in .atto files
2. **Dispatch**: Switch on category for type-specific behavior
3. **Visual**: Pin shapes in the editor correspond to categories

---

## 16. Version Migration Chain

**Problem**: The .atto file format evolves over time. Old files need to be readable
by new versions of the editor and compiler.

**Solution**: A migration chain that upgrades files one version at a time.

```
nanoprog@0 → nanoprog@1 → attoprog@0 → attoprog@1 → instrument@atto:0
```

Each version step has a dedicated migration function:

```cpp
void migrate_v0_to_v1(FlowGraph& graph) {
    // Strip $ from variable references
    // Convert @N to $N in expressions
    // Rename ports using old→new descriptor mapping
    // Fold shadow nodes
}
```

The deserializer detects the version header, applies all necessary migrations in
sequence, and returns the result in the latest format. The serializer always writes
the latest version.

**When to use**: Any persistent format that evolves over time and has existing files
that must remain readable.

---

## Pattern Interactions

These patterns don't exist in isolation. They interact to form larger structures:

- **Sentinel + Observer**: Sentinels ensure observers always receive valid references
- **Dirty Tracking + Mutation Batching**: Dirty flags accumulate during a batch, observers fire on commit
- **Factory + Sentinel**: Factories ensure newly created objects start with sentinel defaults
- **TypePool + Inference**: Interned types enable efficient comparison during fixed-point iteration
- **Shadow Nodes + Two-Layer Model**: Shadows exist in the builder layer, stripped before model serialization
- **Discriminated Union + Descriptor Tables**: Node type descriptors define which arg kinds are valid
- **Version Migration + Serialization**: Migration chain runs between deserialization and builder construction

Understanding these interactions is key to making changes that work *with* the existing
architecture rather than against it.
