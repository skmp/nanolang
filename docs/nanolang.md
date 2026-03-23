# nanolang Language Specification

## Type System

### Value Categories

A value in NanoProg has a **category** indicated by a prefix sigil:

| Sigil | Category   | Description                         |
|-------|-----------|--------------------------------------|
| `%`   | Data      | Plain value                          |
| `&`   | Reference | Reference to a value                 |
| `^`   | Iterator  | Iterator into a container            |
| `@`   | Lambda    | Callable function reference          |
| `#`   | Enum      | Enumeration value                    |
| `!`   | Bang      | Trigger signal (no data)             |
| `~`   | Event     | Event source                         |

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

### Special Types

| Type     | Description                                    |
|----------|------------------------------------------------|
| `void`   | Empty type, only valid as function return type |
| `bool`   | Boolean (true/false), 1-bit logical value      |
| `string` | UTF-8 string, first-class value type           |
| `mutex`  | Mutual exclusion lock (non-copyable, reference-only) |

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

### Function Types

```
(argname:type anotherarg:type) -> return_type
```

Arguments are named with `name:type` pairs. Return type follows `->`.
`void` is a valid return type (no return value).

Example: `(x:f32 y:f32) -> f32`

### Named Types (Type Declarations)

Types can be named using `decl_type`:

```
decl_type type_name field1:type1 field2:type2 ...
```

Named types can be used anywhere a type is expected.
Circular type references are not allowed.

**Struct types must have at least one field.** A `decl_type` with a single token after the name and no `:` is a type alias. A `decl_type` with `(` as the second token is a function type. Any other `decl_type` is a struct and must contain at least one `name:type` field.

Example:
```
decl_type osc_def gen:gen_fn stop:stop_fn p:f32 pstep:f32 a:f32 astep:f32
```

## Node Types

### Declaration Nodes

These are structural — they define types and variables but have no runtime pins.

- `decl_type <name> <fields...>` — Declare a named type
- `decl_var <name> <type>` — Declare a global variable (name must not start with `$`)
- `decl_local <name> <type> <initial_value>` — Declare a local variable (bang in + bang out, no side bang). Declared on the execution path where the input bang arrives, passes bang through. Name must not start with `$`. Type must be valid. Initial value must be compatible with the type. Output pin is `&type` (a reference to the local). The variable is registered for downstream `$name` references.
- `decl_event <name> <fn_type>` — Declare an event with a function signature
- `decl_import <path>` — Import declarations from a module. Only `std/` prefix is supported (resolves to `nanostd/<module>.nano`). Non-`std/` paths are reserved for future package/local lib support.
- `ffi <name> <fn_type>` — Declare an external (FFI) function. The type must be a function type. Registers the function as a global callable via `$name`. In codegen, emits an `extern` declaration.

#### Available Standard Modules

| Module | Import | Description |
|--------|--------|-------------|
| ImGui  | `decl_import std/imgui` | ImGui bindings — window management, text, buttons, sliders, trees, tables, popups, plotting |

### Expression Nodes

- `expr <expression>` — Evaluate expression, inputs from `$N` refs
- `select <cond> <if_true> <if_false>` — Select value by boolean condition. Condition must be `bool`. Both branches must have compatible types.
- `new <type_name>` — Instantiate a declared type
- `dup <value>` — Pass through (duplicate) a value
- `erase <collection> <key>` — Erase from collection (no bangs). Same validation rules as `erase!`. Returns an iterator pointing to the next element.
- `next <iterator>` — Advance an iterator to the next element. Input must be a container iterator. Returns the same iterator type, advanced by one position. Equivalent to `std::next(it)` in C++.
- `lock <mutex> <fn>` — Execute lambda while holding mutex lock. Mutex auto-decays to reference. Lambda takes no args: `() -> T`. If T is non-void, produces a data output. The node's post_bang fires **inside** the lock scope (all chained operations run under the lock).
- `call <fn> [args...]` — Call a function. First arg is the function reference (`$name`). Input pins are dynamically created from the function's argument list. Output pin created from return type (omitted if void). Has lambda handle and post_bang (side bang).

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
- `event! ~<name>` — Event source. Name must be prefixed with `~`. Outputs derived from `decl_event` function args. Return type must be void.
- `output_mix! <value>` — Mix into audio output (bang in)
- `on_key_down!` — Klavier key press event
- `on_key_up!` — Klavier key release event

## Inline Expressions

All non-expr nodes support **inline expressions** in their arguments. Each space-separated arg token replaces the corresponding descriptor input. If an arg is an inline expression (a literal, variable reference, or complex expression), that input slot is "filled" and does not require a pin connection. Only `$N`/`@N` references within inline expressions create actual input pins.

### Rules

1. Each arg token (space-separated, respecting parentheses and quotes) maps to a descriptor input left-to-right
2. The number of arg tokens must not exceed the node's descriptor input count (error otherwise)
3. `$N` references within inline args create input pins; `$name` variable references do not
4. Pin indices must be contiguous starting from 0 — gaps (e.g. `$0` and `$2` without `$1`) are errors
5. Descriptor inputs beyond the number of inline args remain as pin connections

### Examples (store! has 2 descriptor inputs: target, value)

| Node text | Pins | Explanation |
|-----------|------|-------------|
| `store!` | target, value | No inline args — both inputs are pins |
| `store! $oscs` | value | target filled by `$oscs` (variable ref) |
| `store! $oscs 42` | (none) | Both filled inline |
| `store! $oscs $0` | $0 | target = variable, value = pin $0 |
| `store! $1 $0` | $0, $1 | Both inline but reference pins |
| `store! $0 $1 $2` | error | Too many args (store! takes 2) |
| `store! $0 $2` | error | Missing pin $1 |

### Nodes that do NOT support inline expressions

- Declaration nodes (`decl_type`, `decl_var`, `decl_local`, `decl_event`) — args are type/field definitions
- `new` — args are type names
- `event!` — args are event names
- `label` — args are display text

## Expression Language

Expressions appear in `expr` nodes (and inline in other node args). They operate on typed inputs and produce typed outputs.

### Operators

**Arithmetic:** `+`, `-`, `*`, `/`

**Unary minus:** `-expr` — negation (works on numeric types)

**Comparison:** `==`, `!=`, `<`, `>`, `<=`, `>=` — return `bool`. Operands must be the same type (or upcastable). Supports array manipulation (element-wise comparison produces a collection of `bool`).

**Spaceship:** `<=>` — three-way comparison, returns `s32` (-1, 0, or 1). Operands must be the same type (or upcastable). Supports array manipulation (produces a collection of `s32`).

**Parentheses:** `(subexpr)` — grouping, arbitrary nesting

**Reference operator:** `&expr` — creates a reference or iterator. Top-level only (cannot appear inside other expressions like `$0 + &$name`).

| Form | Result |
|------|--------|
| `&$name` | `&T` — reference to the variable's type |
| `&$name[expr]` on `vector<V>` | `vector_iterator<V>` |
| `&$name[expr]` on `map<K,V>` | `map_iterator<K,V>` |
| `&$name[expr]` on `ordered_map<K,V>` | `ordered_map_iterator<K,V>` |
| `&$name[expr]` on array/tensor | Error — cannot reference array/tensor elements |
| `&$name.field` | Error — cannot reference fields |
| `&$name[expr].field` | Error — not allowed |
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

**Slice:** `$var[start:end]` — pythonic slice semantics (negative indices wrap from end). `start` and `end` must be the same type. Supports array manipulation (see below).

**Field access:** `.field` — universal field/member access. Works on:

- **Struct/named types:** access declared fields (e.g. `$pos.x` where `pos : vec2`)
- **Non-map iterators** (`vector_iterator<V>`, `list_iterator<V>`, `set_iterator<V>`, `ordered_set_iterator<V>`): auto-dereference to the value type. Fields of `V` are accessed directly (e.g. `$it.p` where `it : vector_iterator<osc_def>` accesses `osc_def.p`).
- **Map iterators** (`map_iterator<K,V>`, `ordered_map_iterator<K,V>`): have `.key` (returns `K`) and `.value` (returns `V`). No auto-dereference — use `.value.field` to access value fields.

**Logical/bitwise (function-call syntax only):** `or(a,b)`, `xor(a,b)`, `and(a,b)`, `not(a)`, `mod(a,b)` — these are **not** infix operators, only callable as functions. Operate on integer types (`u8`/`s8`/`u16`/`s16`/`u32`/`s32`/`u64`/`s64`).

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

Note: `or`, `and`, `xor`, `not`, `mod` are functions, not operators — they do not appear in the precedence table.

### Built-in Functions

Called as `fn(arg1, arg2, ...)`.

| Function | Accepts     | Returns   |
|----------|-------------|-----------|
| `sin`    | `f32`/`f64` | same      |
| `cos`    | `f32`/`f64` | same      |
| `pow`    | `f32`/`f64` | same      |
| `exp`    | `f32`/`f64` | same      |
| `log`    | `f32`/`f64` | same      |
| `or`     | integer     | same      |
| `xor`    | integer     | same      |
| `and`    | integer     | same      |
| `not`    | integer     | same      |
| `mod`    | integer     | same      |

### Lambda Calls

Any expression that resolves to a lambda can be called: `expr_resolving_to_lambda(arg1, arg2, ...)`. The result type is the return type of the lambda. Bangs are lambdas that take no arguments and return void.

### Numeric Literals

- **Integers:** `1`, `42`, `0` — unresolved integer type (`int?`). The concrete type is inferred from context:
  - `$0 + 1` where `$0 : u8` → `1` becomes `u8`
  - Integer literals also coerce to `f32` or `f64` from context, but only if the value can be represented exactly (f32: |v| <= 2^24 = 16777216, f64: |v| <= 2^53). Values that exceed these limits produce an error.
  - If no context resolves the type, it remains `int?` (unresolved). In a complete program all types must be resolved — an unresolved `int?` is a compile error.
- **f32 floats:** `1.0f`, `3.14f`
- **f64 floats:** `1.0`, `3.14`
- **Boolean:** `true`, `false` — `bool` type

### Built-in Constants

| Name  | Value                    | Type  |
|-------|--------------------------|-------|
| `pi`  | 3.14159265358979323846   | `f64` |
| `e`   | 2.71828182845904523536   | `f64` |
| `tau` | 6.28318530717958647692   | `f64` |

These are bare identifiers (no `$` prefix). Their type is unresolved float (`float?`) and coerces to `f32` or `f64` from context (e.g. `pi * $0` where `$0 : f32` → `pi` becomes `f32`). If no context resolves the type, it remains `float?` (unresolved). In a complete program all types must be resolved — an unresolved `float?` is a compile error. Any other bare identifier that is not a built-in function name, constant, or `true`/`false` is an error.

### Input References (Pin Sigils)

Inputs to an expression are referenced by sigil + index. The sigil indicates the expected value category:

| Syntax | Category   | Description                     |
|--------|------------|--------------------------------|
| `$N`   | Value      | Generic value (any category)   |
| `%N`   | Data       | Data value (plain)             |
| `&N`   | Reference  | Reference to a value           |
| `^N`   | Iterator   | Iterator into a container      |
| `@N`   | Lambda     | Callable function reference    |
| `#N`   | Enum       | Enumeration value              |
| `!N`   | Bang       | Trigger signal                 |
| `~N`   | Event      | Event handle (opaque for now)  |

**Naming:** The first occurrence of a pin index can include a name: `$0:my_input`. Subsequent uses must use `$0` directly (no re-naming).

### Array Manipulation (Broadcasting)

When an operator or built-in function is applied between a scalar and a collection type (`map`, `ordered_map`, `set`, `ordered_set`, `list`, `queue`, `vector`, `array`, `tensor`), the operation is applied element-wise:

- The result is a new collection of the same type and size
- For maps, the operation applies to values (keys are preserved)
- The scalar's type must be compatible with the element type

Example: `sin($0)` where `$0 : vector<f32>` → `vector<f32>` with sin applied to each element.

**Exception:** If the function natively accepts the collection type as input, it operates on the collection directly (no broadcasting). Broadcasting only applies when the function signature doesn't match the collection type.

### Type Inference

NanoProg uses **bidirectional type inference**:

1. **Forward inference:** Input types propagate through operators and functions to determine the output type.
2. **Backward inference:** If a downstream consumer expects a specific type, that constraint propagates back to resolve unknown input types.

**Rules:**
- Unknown input types are assumed valid as long as the rest of the expression type-checks. They are resolved when the connected pin's type becomes known.
- In a complete program, all types must be statically resolved — no unknowns remain.
- **No silent type conversions** except:
  - Integer upcasts within the same signedness family (`u8` → `u16` → `u32` → `u64`, `s8` → `s16` → `s32` → `s64`)
  - **Iterator-to-reference decay**: a `^iterator<T>` automatically decays to `&T` or `T` when passed to a function. This allows passing iterators directly to functions that operate on element references (e.g. `$it.stop($it)` where `stop` expects `&osc_def` and `$it` is `^list_iterator<osc_def>`).
- **Function call validation**: argument count and types are checked against the function signature. Generic numeric literals (`int?`, `float?`) cannot be used as struct, Named, or container types.
- Connections between incompatible types are errors and render in red.

### Lambda Construction

When an `expr` node has a **lambda grab** handle (`as_lambda`):

1. The node **must** have outputs (or the lambda returns `void`).
2. **Connected inputs are captures** — their values are static at the point of lambda construction (they belong to the caller's graph, not the callee's). Captures are visually indicated in the editor.
3. **Unconnected inputs become lambda parameters**, ordered left to right.
4. **Recursive parameter collection:** If a connected input's own inputs are unconnected, those bubble up as lambda parameters too, recursively, in left-to-right order. The key distinction is whether a value has already been computed in the caller's graph (capture) or needs to be provided by the callee at call time (parameter).

**Example:** A lambda node has 3 inputs. Input `$0` is connected to a node whose own 2 inputs are unconnected. Input `$1` is connected to a node with 1 unconnected input. Input `$2` is unconnected. The resulting lambda has **4 parameters**: `$0`'s two (left to right), `$1`'s one, then `$2`.

**Inbound type inference for lambdas:** If a downstream node expects a lambda of type `(u32, u32) -> u32`, the lambda's parameters are typed `u32, u32` and the output must resolve to `u32`.

## File Format (.nano)

TOML-like format:

```
version = "nanoprog@0"

[[node]]
guid = "a3f7c1b2e9d04856"
type = "expr"
args = ["$0+$1"]
position = [100, 200]
connections = ["a3f7c1b2e9d04856.out0->b4c8d9e0f1a23456.0"]
```

### Connection Format

Connections use pin IDs: `"<guid>.<pin_name>-><guid>.<pin_name>"`

Pin names:
- Data/lambda inputs: `0`, `1`, `2`, ... or named (e.g. `gen`, `stop`)
- Bang inputs: `bang_in0`, `bang_in1`, ...
- Data outputs: `out0`, `out1`, ...
- Bang outputs: `bang0`, `bang1`, ...
- Lambda grab: `as_lambda`
- Post-bang: `post_bang`
