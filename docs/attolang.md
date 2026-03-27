# attolang Language Specification

## Type System

### Value Categories

A value in AttoProg has a **category**:

| Category   | Description                         |
|-----------|--------------------------------------|
| Data      | Plain value                          |
| Reference | Reference to a value (`&T`)          |
| Iterator  | Iterator into a container (`^T`)     |
| Lambda    | Callable function reference          |
| Enum      | Enumeration value                    |
| Bang      | Trigger signal (no data)             |
| Event     | Event source                         |

Value categories are part of the type system but are not indicated by prefix sigils during access. The only prefix syntax retained is `$N` for pin references in expressions (see [Input References](#input-references)).

### Literal Type

`literal<T,V>` is a unified compile-time value type, parameterized by a type domain `T` and a compile-time value `V`. All compile-time constants — numbers, booleans, and strings — are represented as literals. The canonical format uses no spaces: `literal<T,V>`.

| Expression | Type | Notes |
|---|---|---|
| `0` | `literal<unsigned<?>,0>` | Type-generic: resolves to concrete unsigned type from context |
| `-1` | `literal<signed<?>,-1>` | Type-generic: resolves to concrete signed type from context |
| `42` | `literal<unsigned<?>,42>` | Type-generic: resolves to concrete unsigned type from context |
| `3.14f` | `literal<f32,3.140000f>` | Concrete f32 literal |
| `3.14` | `literal<f64,3.140000>` | Concrete f64 literal |
| `true` | `literal<bool,true>` | Parsed directly as a boolean literal |
| `false` | `literal<bool,false>` | Parsed directly as a boolean literal |
| `"hello"` | `literal<string,"hello">` | String literal |

`true` and `false` are parsed as boolean literals by the expression parser — they are not symbols.

Bare identifiers (`sin`, `pi`, `myvar`, etc.) produce **symbol types**, not literals — see [Symbol Types](#symbol-types).

#### Literal Typing

Literal types have two flags:

- **`is_generic`** — The **type** is unresolved (e.g., `0` could be `u8`, `u16`, `u32`, `f32`, etc.). Resolved via backpropagation from context.
- **`is_unvalued_literal`** — The **value** is not yet provided (used for input pins that expect a literal). Displayed as `literal<T,?>` (e.g., `literal<string,?>`).

| Example | `is_generic` | `is_unvalued_literal` | Display |
|---|---|---|---|
| `0` (unresolved int) | true | false | `literal<unsigned<?>,0>` |
| `1.0f` (concrete) | false | false | `literal<f32,1.000000f>` |
| input pin expecting string | false | true | `literal<string,?>` |
| `vector<?>` | true | false | `vector<?>` |

#### Literal Decay

Literals decay to their resolved types when consumed by operations:

- **Operations** (binary ops, function calls, `select`, builtins) on literals produce **non-literal runtime types**. `0 + 1` produces `literal<unsigned<?>,?>`, not a literal. `sin(1.0f)` produces `f32`, not `literal<f32,...>`.
- **Connections and passthrough** (dup, wire connections) preserve literal types — literals flow as-is through wires.
- **Type-generic literals** (`unsigned<?>`, `float<?>`) resolve via backpropagation when a concrete type is known from context.

Integer literals coerce to `f32` or `f64` from context, but only if the value can be represented exactly (f32: |v| <= 2^24 = 16777216, f64: |v| <= 2^53). Values that exceed these limits produce an error.

#### Literal Syntax in Expressions

`literal<T,V>` can be typed directly in expression nodes:

```
expr literal<string,"abc">
expr literal<unsigned<?>,42>
expr literal<signed<?>,-5>
expr literal<bool,true>
```

This produces the same result as the shorthand forms (`"abc"`, `42`, `-5`, `true`).

`literal`, `symbol`, and `undefined_symbol` are **reserved keywords** in the expression parser — they cannot be used as identifiers.

### Symbol Types

Bare identifiers in expressions produce **symbol types**, not literals. Symbols are first-class values that carry both a name and a decay type.

- **`symbol<name,type>`** — A defined symbol found in the symbol table. `name` is the symbol name, `type` is the decay type (what it resolves to when consumed). Example: `expr sin` produces `symbol<sin,(f32)->f32>`. `expr myvar` (where `myvar` is declared as `f32`) produces `symbol<myvar,f32>`.
- **`undefined_symbol<name>`** — A bare identifier not (yet) in the symbol table. Valid to pass around at compile time (e.g., as a name input to `decl_var`). Does not produce an error at inference time. Errors only if something tries to **evaluate** it (emit runtime code).

#### Symbol Decay

Symbols decay automatically when **consumed** by operations:

- **Operations** (binary ops, function calls, builtins, store, select, field access, indexing, unary ops) decay symbols to their wrapped type before processing.
- **Connections** decay symbols when propagating through wires — the receiving pin gets the decayed type.
- **Non-expr nodes** (store!, iterate!, append!, etc.) decay all symbol types from inline expressions before validation.
- **Expr node outputs** preserve symbol types — `expr sin` outputs `symbol<sin,(f32)->f32>`, not the decayed function type.
- **Type utility functions** (`is_numeric`, `is_float`, `is_collection`, `types_compatible`, etc.) auto-decay symbols transparently.

#### Symbol Table

The symbol table maps symbol names to their meanings. It is populated by:

1. **Built-in entries** (predefined):

| Symbol | Decays to |
|--------|-----------|
| `sin` | `(float<T:?>) -> float<T>` |
| `cos` | `(float<T:?>) -> float<T>` |
| `pow` | `(float<T:?>, float<T>) -> float<T>` or `(literal<integer, 2>, unsigned<T:?>) -> unsigned<T>` |
| `exp` | `(float<T:?>) -> float<T>` |
| `log` | `(float<T:?>) -> float<T>` |
| `or` | `(integer<T:?>, integer<T>) -> integer<T>` |
| `xor` | `(integer<T:?>, integer<T>) -> integer<T>` |
| `and` | `(integer<T:?>, integer<T>) -> integer<T>` |
| `not` | `(integer<T:?>) -> integer<T>` |
| `mod` | `(numeric<T:?>, numeric<T>) -> numeric<T>` |
| `rand` | `(numeric<T:?>, numeric<T>) -> numeric<T>` |
| `pi` | `float<?>` (value 3.14159265358979323846) |
| `e` | `float<?>` (value 2.71828182845904523536) |
| `tau` | `float<?>` (value 6.28318530717958647692) |
| `true` | `bool` |
| `false` | `bool` |
| `f32` | `type<f32>` |
| `f64` | `type<f64>` |
| `u8` | `type<u8>` |
| `u16` | `type<u16>` |
| `u32` | `type<u32>` |
| `u64` | `type<u64>` |
| `s8` | `type<s8>` |
| `s16` | `type<s16>` |
| `s32` | `type<s32>` |
| `s64` | `type<s64>` |
| `bool` | `type<bool>` |
| `string` | `type<string>` |
| `void` | `type<void>` |
| `mutex` | `type<mutex>` |
| `vector` | `type<vector<?>>` |
| `map` | `type<map<?, ?>>` |
| `set` | `type<set<?>>` |
| `list` | `type<list<?>>` |
| `queue` | `type<queue<?>>` |
| `ordered_map` | `type<ordered_map<?, ?>>` |
| `ordered_set` | `type<ordered_set<?>>` |
| `array` | `type<array<?, ...>>` |
| `tensor` | `type<tensor<?>>` |

2. **Declaration nodes** — `decl_type`, `decl_var`, `decl_import`, `decl_event`, `ffi` add entries during the compile-time phase.

An `undefined_symbol` is promoted to `symbol` when a declaration node adds it to the table.

### Namespace Operator

The `::` operator is used for namespace access within symbol names:

- `std::imgui::Button` — nested namespace lookup
- `mymodule::MyType` — module-qualified symbol

`.` is always field access on values. `::` is always namespace resolution on symbols. The two never overlap.

### Scalar Types

| Type  | Description              |
|-------|--------------------------|
| `u8`  | Unsigned 8-bit integer   |
| `s8`  | Signed 8-bit integer     |
| `u16` | Unsigned 16-bit integer  |
| `s16` | Signed 16-bit integer    |
| `u32` | Unsigned 32-bit integer  |
| `s32` | Signed 32-bit integer    |
| `u64` | Unsigned 64-bit integer  |
| `s64` | Signed 64-bit integer    |
| `f32` | 32-bit float (float)     |
| `f64` | 64-bit float (double)    |

### Type Categories

| Category   | Types                          | Description                  |
|------------|--------------------------------|------------------------------|
| signed     | `s8`, `s16`, `s32`, `s64`      | Signed integers              |
| unsigned   | `u8`, `u16`, `u32`, `u64`      | Unsigned integers            |
| integer    | signed ∪ unsigned              | All integer types            |
| float      | `f32`, `f64`                   | Floating-point types         |
| numeric    | integer ∪ float                | All numeric types            |

These categories are generic type constructors and can be parameterized:

#### Generic Type Notation

`?` is the unresolved type indicator. `|` is the combination (union) type indicator. Named bindings allow referencing the same resolved type across a signature.

| Notation | Meaning |
|----------|---------|
| `signed<s32>` | Concrete signed type `s32` |
| `integer<u64>` | Concrete integer type `u64` |
| `numeric<s32>` | Concrete numeric type `s32` |
| `signed<?>` | Any signed type (`s8`, `s16`, `s32`, or `s64`) |
| `float<?>` | Any float type (`f32` or `f64`) |
| `signed<s8\|s32>` | Union — matches `s8` or `s32` |
| `signed<T:?>` | Named generic — `T` binds to whichever signed type is resolved, and can be referenced elsewhere |
| `numeric<T:?>` | Named generic — `T` binds to whichever numeric type is resolved |

A generic type written without `<>` implies `<?>`. For example, `numeric` in a type descriptor is equivalent to `numeric<?>` — it matches any numeric type.

Named generics express constraints across arguments and return types. For example, a function signature `(a:numeric<T:?>, b:numeric<T>) -> numeric<T>` requires both args and the return type to resolve to the same concrete numeric type.

### Special Types

| Type     | Description                                    |
|----------|------------------------------------------------|
| `void`   | Empty type, only valid as function return type |
| `bool`   | Boolean (true/false), 1-bit logical value      |
| `string` | UTF-8 string, first-class value type           |
| `mutex`  | Mutual exclusion lock (non-copyable, reference-only) |
| `symbol` | Symbolic name, compile-time only               |
| `type<T>`| Type as a first-class compile-time value       |

### Container Types

All containers are parameterized with element/key types.

| Container                    | Description                        | C++ Equivalent              |
|-----------------------------|------------------------------------|-----------------------------|
| `map<Kt, Kv>`              | Unordered associative container    | `std::unordered_map<Kt,Kv>`|
| `ordered_map<Kt, Kv>`      | Ordered associative container      | `std::map<Kt,Kv>`          |
| `set<Vt>`                  | Unordered unique collection        | `std::unordered_set<Vt>`   |
| `ordered_set<Vt>`          | Ordered unique collection          | `std::set<Vt>`             |
| `list<Vt>`                 | Doubly-linked list                 | `std::list<Vt>`            |
| `queue<Vt>`                | FIFO queue                         | `std::queue<Vt>`           |
| `vector<Vt>`               | Dynamic array                      | `std::vector<Vt>`          |

### Iterator Types

Each container has a corresponding iterator type:

| Iterator                        | Iterates over          |
|--------------------------------|------------------------|
| `map_iterator<Kt, Kv>`        | `map<Kt, Kv>`         |
| `ordered_map_iterator<Kt, Kv>`| `ordered_map<Kt, Kv>` |
| `set_iterator<Vt>`            | `set<Vt>`             |
| `ordered_set_iterator<Vt>`    | `ordered_set<Vt>`     |
| `list_iterator<Vt>`           | `list<Vt>`            |
| `vector_iterator<Vt>`         | `vector<Vt>`          |

### Fixed-Size Arrays

```
array<Vt, dim1, dim2, ...>
```

Must have at least one dimension. Dimensions are compile-time constants.
Example: `array<f32, 4, 4>` is a 4x4 matrix of floats.

### Tensors

```
tensor<Vt>
```

Like array but dimensions are determined at runtime.

### Type Expressions

Type expressions are syntactically distinct from value expressions. The parser can always determine whether an expression is a type or a value.

#### Function Types

```
(argname:type anotherarg:type)->return_type
```

Identified by the `->` token. Arguments are named with `name:type` pairs (space-separated). Return type follows `->`. `void` is a valid return type.

Example: `(x:f32 y:f32)->f32`

#### Struct Types

Struct types use `{}` with space-separated `name:type` fields:

```
{field1:type1 field2:type2 ...}
```

Example: `{x:f32 y:f32}`, `{gen:gen_fn stop:stop_fn p:f32 pstep:f32 a:f32 astep:f32}`

Struct types must have at least one field.

#### Struct Literals

Struct literals also use `{}` but with **comma-separated** `name:value` pairs:

```
{field1:value1, field2:value2, ...}
```

Example: `{x: 1.0f, y: 2.0f}`

The comma is the disambiguator: commas = runtime struct literal, spaces only = type definition.

Struct literals produce a value of an **anonymous struct type**. The type is inferred from the field names and value types. To construct a named type from a struct literal, use the type constructor explicitly:

```
osc_def({gen: $0, stop: $1, p: 440.0f, pstep: 0.0f, a: 1.0f, astep: 0.0f})
```

Both positional and named construction are supported:
- `osc_def($0, $1, 440.0f, 0.0f, 1.0f, 0.0f)` — positional, arguments match field order
- `osc_def({gen: $0, stop: $1, p: 440.0f, ...})` — named fields via struct literal

#### Type Construction (Calling Types)

Any `type<T>` value can be called to construct a value of that type:

- **Named struct types:** `osc_def(gen_val, stop_val, 440.0f, 0.0f, 1.0f, 0.0f)` — positional arguments matching field order.
- **Scalar types:** `f32(42)` — casts the argument to `f32`. `string(123)` — converts to string.
- **Container types:** `vector<f32>(1.0f, 2.0f, 3.0f)` — constructs with initial elements (future).

This is a regular function call on a symbol that decays to `type<T>`. The call operator on `type<T>` decays to the appropriate constructor function. This replaces the need for dedicated `new`, `cast`, and `str` nodes.

#### Parameterized Types

Types can be parameterized with `<>`:

```
vector<f32>
map<u64, osc_def>
array<f32, 4, 4>
```

#### Type Aliases

A `decl_type` with a symbol and a single type argument (no `{}`) is a type alias:

```
decl_type gen_fn (x:f32)->f32
```

### Named Types (Type Declarations)

Types can be declared using `decl_type` (see [Declaration Nodes](#declaration-nodes-compile-time)):

```
decl_type osc_def {gen:gen_fn stop:stop_fn p:f32 pstep:f32 a:f32 astep:f32}
```

Named types can be used anywhere a type is expected.
Circular type references are not allowed.

`decl_type` adds the type name to the symbol table mapping to `type<T>`. When called, the type decays to a constructor function with positional arguments matching the struct's field order.

## Compile-Time and Runtime Phases

AttoProg has two distinct execution phases with separate bang chains:

### Compile-Time Phase

The compile-time phase establishes the symbol table and type definitions. It is driven by the `decl` node, which is the compile-time entry point (at most one per graph). The `decl` node's bang output chains to declaration nodes (`decl_type`, `decl_var`, `decl_import`, `decl_event`, `ffi`).

Compile-time bang chains:
- Only connect declaration nodes
- Only pass around compile-time values: literals, symbols, types
- No runtime evaluation or side effects
- Establish the symbol table before any runtime code executes

The declaration order is explicit — determined by the bang chain. Symbols must be declared before they are referenced by downstream nodes.

### Runtime Phase

The runtime phase is event-driven, starting from event nodes (`event!`, `on_key_down!`, etc.). Runtime bang chains execute imperative code: storing values, iterating, branching, calling functions.

`decl_var` on a runtime bang chain declares a **local variable** scoped to that execution path (see below).

The two phases never cross — no bang wire from a compile-time node to a runtime node or vice versa.

## Node Types

### Declaration Nodes (Compile-Time)

These nodes live on the compile-time bang chain rooted at `decl`. They define types, variables, and imports. All declaration nodes take a bang input and produce a bang output.

- **`decl`** — Compile-time entry point. At most one per graph. No inputs. Outputs: bang. Starts the compile-time bang chain.

- **`decl_type <name> <type>`** — Declare a named type.
  - Inputs: bang, symbol (name), type (a struct type `{...}`, function type `(...)->T`, or existing type for aliasing)
  - Outputs: bang, `type<T>` (the declared type)
  - The symbol input is an `undefined_symbol` that gets added to the symbol table.
  - The `type<T>` output can be wired directly to other nodes that need the type (e.g., `decl_var`, `new`).

- **`decl_var <name> <type> [initial_value]`** — Declare a variable.
  - Inputs: bang, symbol (name), type, optional initial value
  - Outputs: bang, `&T` (reference to the variable)
  - On the **compile-time** bang chain: declares a **global** variable.
  - On a **runtime** bang chain: declares a **local** variable scoped to the execution path.
  - Initial value is optional. If omitted, the variable is default/zero-initialized.
  - The symbol is added to the symbol table, mapping to `&T`.

- **`decl_event <name> <fn_type>`** — Declare an event with a function signature.
  - Inputs: bang, symbol (name), type (must be a function type with void return)
  - Outputs: bang

- **`decl_import <module>`** — Import declarations from a module.
  - Inputs: bang, string literal (module path, e.g., `"std/imgui"`)
  - Outputs: bang
  - The module path is a string literal, not a symbol — the module doesn't exist in the symbol table before the import. The input pin type is `literal<string,?>` (an unvalued string literal).
  - Populates the symbol table with all exported symbols from the module.

- **`ffi <name> <fn_type>`** — Declare an external (FFI) function.
  - Inputs: bang, symbol (name), type (must be a function type)
  - Outputs: bang
  - Registers the function in the symbol table. In codegen, emits an `extern` declaration.

#### Available Standard Modules

| Module | Import | Description |
|--------|--------|-------------|
| ImGui  | `decl_import "std/imgui"` | ImGui bindings — window management, text, buttons, sliders, trees, tables, popups, plotting |

### Expression Nodes

- `expr <expression>` — Evaluate expression, inputs from `$N` refs. This is the universal expression node. Type construction (`osc_def($0, $1, ...)`), type casting (`f32($0)`), and string conversion (`string($0)`) are all regular expressions using type constructor calls.
- `select <cond> <if_true> <if_false>` — Select value by boolean condition. Condition must be `bool`. Both branches must have compatible types.
- `new <type>` — Visual sugar for type construction. `new osc_def` with input pins is equivalent to `expr osc_def($0, $1, ...)`. Takes a `type<T>` input and creates input pins for each field/argument.
- `str <value>` — Visual sugar for `expr string($0)`. Input is any type, output is always `string`.
- `cast <dest_type>` — Visual sugar for `expr T($0)`. Takes a `type<T>` and a value input, outputs `T`.
- `dup <value>` — Pass through (duplicate) a value
- `erase <collection> <key>` — Erase from collection (no bangs). Same validation rules as `erase!`. Returns an iterator pointing to the next element.
- `next <iterator>` — Advance an iterator to the next element. Input must be a container iterator. Returns the same iterator type, advanced by one position. Equivalent to `std::next(it)` in C++.
- `lock <mutex> <fn>` — Execute lambda while holding mutex lock. Mutex auto-decays to reference. Lambda takes no args: `() -> T`. If T is non-void, produces a data output. The node's post_bang fires **inside** the lock scope (all chained operations run under the lock).
- `call <fn> [args...]` — Call a function. First arg is the function reference. Input pins are dynamically created from the function's argument list. Output pin created from return type (omitted if void). Has lambda handle and post_bang (side bang).

### Bang Nodes (postfixed with `!`)

Nodes with input or output bangs:

- `store! <target> <value>` — Store value (bang in + bang out). Target must be an lvalue (variable, field access, or indexed variable). Value type must be compatible with target type.
- `append! <collection> <value>` — Append to collection (bang in). Collection must be `vector`, `list`, or `queue`. Value must be compatible with the collection's element type. Returns an iterator pointing to the appended element.
- `expr! <expression>` — Evaluate on bang trigger (bang in + bang out)
- `select! <condition>` — Branch: fires `true` or `false` bang output
- `erase! <collection> <key>` — Erase from collection (bang in + bang out + output). Accepts: matching iterator type for any container, key type for map/ordered_map, value type for set/ordered_set, integer index for vector. Returns an iterator pointing to the next element.
- `iterate! <collection> <fn>` — Iterator loop (bang in + bang out). The lambda signature depends on the collection:

  | Collection type | Lambda signature | Notes |
  |----------------|-----------------|-------|
  | `vector<V>` | `(^vector_iterator<V>) -> ^vector_iterator<V>` | Returns next iterator |
  | `list<V>` | `(^list_iterator<V>) -> ^list_iterator<V>` | Returns next iterator |
  | `set<V>` | `(^set_iterator<V>) -> ^set_iterator<V>` | Returns next iterator |
  | `ordered_set<V>` | `(^ordered_set_iterator<V>) -> ^ordered_set_iterator<V>` | Returns next iterator |
  | `map<K,V>` | `(^map_iterator<K,V>) -> ^map_iterator<K,V>` | Returns next iterator |
  | `ordered_map<K,V>` | `(^ordered_map_iterator<K,V>) -> ^ordered_map_iterator<K,V>` | Returns next iterator |
  | `queue<V>` | `(^vector_iterator<V>) -> ^vector_iterator<V>` | Returns next iterator |
  | `array<V,...>` | `(&V) -> void` | No iterator, visits each element |
  | `tensor<V>` | `(&V) -> void` | No iterator, visits each element |
  | scalar `T` | `(&T) -> void` | Runs once |
- `lock! <mutex> <fn>` — Execute lambda while holding mutex lock (bang in + bang out). Mutex auto-decays to reference. Lambda takes no args: `() -> T`. If T is non-void, produces a data output. Bang output fires **after** the lock is released.
- `call! <fn> [args...]` — Call a function (bang in + bang out). Same as `call` but with explicit bang control flow. Input/output pins dynamically created from function signature.
- `event! <name>` — Event source. Name is a symbol referencing a declared event. Outputs derived from `decl_event` function args. Return type must be void.
- `resize! <collection> <size>` — Resize a vector (bang in + bang out). First arg is the target vector, second arg is the new size (integer). If the size arg is not `u64`, a `static_cast<size_t>()` is emitted.
- `output_mix! <value>` — Mix into audio output (bang in)
- `on_key_down!` — Klavier key press event
- `on_key_up!` — Klavier key release event

## Inline Expressions

All non-declaration nodes support **inline expressions** in their arguments. Each arg maps 1:1 to a descriptor input port. An arg can be:

- **Net reference** (`$net-name`): connects to a named net — produces a visible input pin
- **Expression** (`sin($0)+1`): inline expression — displayed in node text, no pin
- **Literal** (`42`, `"hello"`, `true`): inline constant — displayed in node text, no pin
- **Symbol** (`oscs`, `sin`): bare identifier resolved via symbol table — displayed in text, no pin

Only net references produce visible input pins. All other arg types fill the slot inline.

### Rules

1. Each arg maps to a descriptor input left-to-right
2. The number of args must not exceed the node's descriptor input count (plus va-args if applicable)
3. `$N` references within expressions create **remap pins** (mapped via the `remaps` array)
4. Remap indices must be contiguous starting from 0 — gaps produce errors
5. `$name` (non-numeric, starting with `$`) references a named net and produces a visible pin
6. Bare names (no `$` prefix) are symbols resolved via the symbol table, not pins

### Examples (store! has 3 descriptor inputs: bang_in, target, value)

| Args | Visible pins | Explanation |
|------|-------------|-------------|
| `["$bang-src", "$var-ref", "$val-net"]` | 3 pins | All net refs — all visible |
| `["$bang-src", "oscs", "$0"]` | 2 pins (bang + remap $0) | target filled by symbol `oscs` |
| `["$bang-src", "oscs", "42"]` | 1 pin (bang only) | Both target and value filled inline |

## Expression Language

Expressions appear in `expr` nodes (and inline in other node args). They operate on typed inputs and produce typed outputs.

### Operators

**Arithmetic:** `+`, `-`, `*`, `/`

**Unary minus:** `-expr` — negation (works on numeric types)

**Comparison:** `==`, `!=`, `<`, `>`, `<=`, `>=` — return `bool`. Operands must be the same type (or upcastable). Supports array manipulation (element-wise comparison produces a collection of `bool`).

**Spaceship:** `<=>` — three-way comparison, returns `s32` (-1, 0, or 1). Operands must be the same type (or upcastable). Supports array manipulation (produces a collection of `s32`).

**Parentheses:** `(subexpr)` — grouping, arbitrary nesting

**Reference operator:** `&expr` — creates a reference or iterator. Top-level only (cannot appear inside other expressions like `$0 + &myvar`).

| Form | Result |
|------|--------|
| `&name` | `&T` — reference to the variable's type |
| `&name[expr]` on `vector<V>` | `vector_iterator<V>` |
| `&name[expr]` on `map<K,V>` | `map_iterator<K,V>` |
| `&name[expr]` on `ordered_map<K,V>` | `ordered_map_iterator<K,V>` |
| `&name[expr]` on array/tensor | Error — cannot reference array/tensor elements |
| `&name.field` | Error — cannot reference fields |
| `&name[expr].field` | Error — not allowed |
| `&(expr)`, `&literal` | Error — can only reference variables or indexed containers |

**Indexing:** `expr[expr]` — index into a collection. Supported types and return values:

| Type | Index type | Returns |
|------|-----------|---------|
| `vector<V>` | integer | `V` |
| `map<K, V>` | `K` | `V` |
| `ordered_map<K, V>` | `K` | `V` |
| `array<V, ...>` | integer | `V` (multi-dimensional: chain `[i][j][k]`) |
| `tensor<V>` | integer | `V` (multi-dimensional: chain `[i][j]`, dimension count checked at runtime) |
| `string` | integer | `u8` (byte value) |

Not indexable: `list`, `queue`, `set`, `ordered_set` (use iterators or `?[]` instead).

**Query indexing:** `expr?[expr]` — returns `bool`: `true` if the index/key exists, `false` otherwise. Supported on: `map`, `ordered_map`, `set`, `ordered_set`. Not supported on `vector`, `list`, `queue`, `array`, `tensor`.

**Slice:** `var[start:end]` — pythonic slice semantics (negative indices wrap from end). `start` and `end` must be the same type. Supports array manipulation (see below).

**Field access:** `.field` — universal field/member access. Works on:

- **Struct/named types:** access declared fields (e.g. `pos.x` where `pos : vec2`)
- **Non-map iterators** (`vector_iterator<V>`, `list_iterator<V>`, `set_iterator<V>`, `ordered_set_iterator<V>`): auto-dereference to the value type. Fields of `V` are accessed directly (e.g. `$0.p` where `$0 : vector_iterator<osc_def>` accesses `osc_def.p`).
- **Map iterators** (`map_iterator<K,V>`, `ordered_map_iterator<K,V>`): have `.key` (returns `K`) and `.value` (returns `V`). No auto-dereference — use `.value.field` to access value fields.

**Namespace access:** `::` — namespace resolution on symbols. `std::imgui::Button` resolves to a symbol in the `std::imgui` namespace. Never used for field access.

**String concatenation:** `string + string` — the `+` operator concatenates two strings. Both operands must be `string`. To concatenate a non-string value, convert it first with `str`.

### Operator Precedence (highest to lowest)

| Precedence | Operators                     | Associativity |
|-----------|-------------------------------|---------------|
| 1         | Unary `-`                     | Right         |
| 2         | `.` (field access)            | Left          |
| 3         | `[expr]`, `?[expr]`, `[a:b]` | Left (postfix)|
| 4         | `(args)` (function call)      | Left (postfix)|
| 5         | `*`, `/`                      | Left          |
| 6         | `+`, `-`                      | Left          |
| 7         | `<`, `>`, `<=`, `>=`, `<=>`   | Left          |
| 8         | `==`, `!=`                    | Left          |

Note: `or`, `and`, `xor`, `not`, `mod` are symbols that decay to functions — they do not appear in the precedence table.

### Function Calls

Any expression that resolves to a callable type can be called: `expr(arg1, arg2, ...)`. This includes:

- **Symbols that decay to functions:** `sin($0)`, `rand(0, 100)`
- **Pin references to lambdas:** `$0($1)`
- **Symbols connected via wires:** `expr sin` outputs `symbol<sin,(f32)->f32>`, which can be wired to another node's `$0` input, then called as `$0($1)` — equivalent to `sin($1)`.

The result type is the return type of the function/lambda. Bangs are lambdas that take no arguments and return void.

### Input References

Inputs to an expression are referenced by `$N` where `N` is a numeric pin index:

| Syntax | Description |
|--------|------------|
| `$N` | Input pin N (any value category) |

Only numeric indices are valid. `$name` syntax is **not supported** and produces a parse error — use bare names instead (resolved via the symbol table).

**Naming:** The first occurrence of a pin index can include a name: `$0:my_input`. Subsequent uses must use `$0` directly (no re-naming).

All other values (variables, functions, constants) are accessed as plain symbols resolved through the symbol table. No sigil prefix is needed.

### Array Manipulation (Broadcasting)

When an operator or function is applied between a scalar and a collection type (`map`, `ordered_map`, `set`, `ordered_set`, `list`, `queue`, `vector`, `array`, `tensor`), the operation is applied element-wise:

- The result is a new collection of the same type and size
- For maps, the operation applies to values (keys are preserved)
- The scalar's type must be compatible with the element type

Example: `sin($0)` where `$0 : vector<f32>` → `vector<f32>` with sin applied to each element.

**Exception:** If the function natively accepts the collection type as input, it operates on the collection directly (no broadcasting). Broadcasting only applies when the function signature doesn't match the collection type.

### Type Inference

AttoProg uses **bidirectional type inference**:

1. **Forward inference:** Input types propagate through operators and functions to determine the output type.
2. **Backward inference:** If a downstream consumer expects a specific type, that constraint propagates back to resolve unknown input types.

**Rules:**
- Unknown input types are assumed valid as long as the rest of the expression type-checks. They are resolved when the connected pin's type becomes known.
- In a complete program, all types must be statically resolved — no unknowns remain.
- **No silent type conversions** except:
  - Integer upcasts within the same signedness family (`u8` → `u16` → `u32` → `u64`, `s8` → `s16` → `s32` → `s64`)
  - **Iterator-to-reference decay**: a `^iterator<T>` automatically decays to `&T` or `T` when passed to a function. This allows passing iterators directly to functions that operate on element references.
- **Function call validation**: argument count and types are checked against the function signature. Generic numeric literals cannot be used as struct, Named, or container types.
- Connections between incompatible types are errors and render in red.

### Lambda Construction

When an `expr` node has a **lambda grab** handle (`as_lambda`):

1. The node **must** have outputs (or the lambda returns `void`).
2. **Connected inputs are captures** — their values are static at the point of lambda construction (they belong to the caller's graph, not the callee's). Captures are visually indicated in the editor.
3. **Unconnected inputs become lambda parameters**, ordered left to right.
4. **Recursive parameter collection:** If a connected input's own inputs are unconnected, those bubble up as lambda parameters too, recursively, in left-to-right order. The key distinction is whether a value has already been computed in the caller's graph (capture) or needs to be provided by the callee at call time (parameter).

**Example:** A lambda node has 3 inputs. Input `$0` is connected to a node whose own 2 inputs are unconnected. Input `$1` is connected to a node with 1 unconnected input. Input `$2` is unconnected. The resulting lambda has **4 parameters**: `$0`'s two (left to right), `$1`'s one, then `$2`.

**Caller scope and captures:** The lambda capture point is the node that receives the `as_lambda` connection (e.g., `iterate!`, `store!`, `lock!`). All nodes that are in the **ancestral execution flow** before this capture point are in the **caller scope**. This includes:

- Nodes reachable backward via the bang chain (trigger connections) from the capture node
- Nodes reachable backward via data connections from those bang-chain ancestors

When a lambda's data dependency traces back to a node in the caller scope, that dependency is a **capture** (already evaluated before the lambda was constructed), not a lambda parameter. The recursive parameter collection stops at caller-scope boundaries — it does not enter the caller's subgraph.

**Inbound type inference for lambdas:** If a downstream node expects a lambda of type `(u32, u32) -> u32`, the lambda's parameters are typed `u32, u32` and the output must resolve to `u32`.

### Bang Pins

Bang pins represent `() -> void` callable connections for control flow:

- **BangTrigger** (top square): The node's callable entry point. When invoked, the node executes. Typed as `() -> void`. Can be used as a value source — connecting a BangTrigger to a data Input passes the `() -> void` callable as a value (e.g., to store it in a variable).
- **BangNext** (bottom square): The node's continuation output. After execution, the node calls whatever is connected here. Typed as `() -> void`. Links go FROM BangNext TO BangTrigger. The first output pin on bang nodes is always a `BangNext` named `next` — this replaces the old `post_bang` pseudo-pin and is rendered at the same visual position.

**Link direction:** BangNext → BangTrigger. The "next" pin calls the "trigger" pin.

**Multiple connections:** BangTrigger pins accept multiple incoming connections if the owning node has no captured data inputs (pure `() -> void` callable). Lambda pins accept multiple connections if the lambda root has no captures. Otherwise, inference reports an error.

**Bidirectional BangTrigger:** A BangTrigger pin can be both a link destination (receiving bang chain flow from BangNext) and a link source (providing its `() -> void` value to a data Input pin).

## File Format (instrument@atto:0)

TOML-like format with named nets instead of explicit pin-to-pin connections.

```
# version instrument@atto:0

[[node]]
id = "$gen-expr"
type = "expr"
args = ["sin($0.p)*$1/32.f"]
remaps = ["$iter-item", "$iter-amp"]
position = [1866.25, 1443.11]

[[node]]
id = "$store-p"
type = "store!"
args = ["$0.p"]
remaps = ["$iter-item"]
position = [2133.84, 1421.55]
```

### Version Header

First line: `# version instrument@atto:0` (comment-style).

Legacy formats (`nanoprog@0`, `nanoprog@1`, `attoprog@0`, `attoprog@1`) are loaded via a legacy parser and auto-migrated. Saving always writes `instrument@atto:0`.

### Node IDs and Net Names

Node IDs and net names share the same namespace:
- Format: `$[a-zA-Z_-][a-zA-Z0-9_-]*`
- Auto-generated: `$auto-<guid>` for unnamed entries
- `$0`, `$1`, ... `$N` are reserved for expression pin inputs
- `$unconnected` is a reserved sentinel for unconnected pins
- The `$` prefix is stored in the file

### Node Structure

```toml
[[node]]
id = "$my-node"          # human-readable identifier
type = "store!"          # node type name
args = ["oscs", "$0"]    # inline arguments (expressions, literals, net refs)
remaps = ["$data-net"]   # $N → net mapping for expression pin inputs
position = [100, 200]    # canvas coordinates
```

### Arguments (`args`)

Each entry in the `args` array is a singular expression (space-delimited in the source, already split in the file). Arguments map 1:1 to the node's descriptor input ports.

An argument can be:
- **Net reference** (`$name`): connects to a named net — produces a visible input pin
- **Expression** (`sin($0)+1`): inline expression with `$N` pin refs — displayed in node text, not a pin
- **Number** (`42`, `3.14f`): inline constant — displayed in node text
- **String** (`"hello"`): inline string literal — displayed in node text

Only `ArgNet2` (net reference) entries produce visible input pins. Inline values are displayed in the node's label text.

### Remaps (`remaps`)

The `remaps` array maps `$N` expression pin inputs to named nets:

```toml
remaps = ["$iter-item", "$iter-amp"]
```

- `remaps[0]` = net for `$0`, `remaps[1]` = net for `$1`, etc.
- `$unconnected` for unconnected expression inputs
- Remaps are always net references

### Pin Model

#### Input pins (top of node, left to right)

Only net reference (`$name`) arguments produce visible pins. The visible pin count is:

| Section | Visible pins | Source |
|---|---|---|
| **Base args** | Only net refs in `args` | 1:1 with descriptor input ports |
| **Va-args** | Only net refs in va-args | Named `{template}_0`, `_1`, ... |
| **Remaps** | All entries | `$0`, `$1`, ... from expressions |

#### Output pins (bottom of node)

All descriptor output ports are visible. Exception: `expr`/`expr!` output count equals `args` count.

#### Pin kinds

| Kind | Visual | Description |
|---|---|---|
| `BangTrigger` | Square (top) | Trigger input |
| `Data` | Circle | Data value |
| `Lambda` | Triangle | Lambda capture (accepts node refs or net refs) |
| `BangNext` | Square (bottom) | Bang continuation output |

### Lambda Captures via Node ID

When a `$id` in an argument resolves to a **node** (not a net), it is a lambda capture. The referenced node's subgraph becomes the lambda body.

- `find_node(id)` → lambda capture
- `find_net(id)` → data wire
- Both `Lambda` and `Data` pins can accept lambda captures
- `Lambda` pins can ONLY accept lambdas (node refs)

### Va-args

Some node types accept a variable number of additional inputs (e.g., `new` for struct fields, `call` for function arguments, `lock` for lambda parameters). The va-args template is defined on the node type descriptor. Va-args pins are named `{template_name}_0`, `{template_name}_1`, etc.

| Node | Va-args template | Description |
|---|---|---|
| `new` | `field` | Constructor fields (`field_0`, `field_1`, ...) |
| `call` / `call!` | `arg` | Function arguments (`arg_0`, `arg_1`, ...) |
| `lock` / `lock!` | `param` | Lambda parameters (`param_0`, `param_1`, ...) |

### Viewport (Meta File)

Viewport state is stored in `.atto/<filename>.yaml`, not in the `.atto` file:

```yaml
# Editor metadata for main.atto
viewport_x: -1504.32
viewport_y: -551.573
viewport_zoom: 4.17725
```

The `.atto/` directory is gitignored. Node positions remain in the `.atto` file.

### Labels and Errors

```toml
[[node]]
id = "$lbl-types"
type = "label"
args = ["Types"]
position = [766, 335]
```

Labels have exactly 1 argument (the display text). Error nodes are the same — they display the original args when parsing failed.
