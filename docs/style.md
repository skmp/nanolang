# Organic Assembler — Style Guide

This document covers the visual, structural, and API design style of the Organic Assembler
project. It addresses both the C++ code style and the language/editor design aesthetic.

## Code Formatting

### Indentation and Braces

4-space indentation. Opening brace on the same line as the statement:

```cpp
if (condition) {
    do_something();
} else {
    do_other();
}

for (auto& node : graph.nodes) {
    process(node);
}

struct FlowArg2 : std::enable_shared_from_this<FlowArg2> {
    // members
};
```

### Single-Line Expressions

Short accessor methods and inline helpers written on one line:

```cpp
ArgKind kind() const { return kind_; }
bool is(ArgKind k) const { return kind_ == k; }
const PortDesc2* port() const { return port_; }
bool eof() const { return pos >= src.size(); }
char peek() const { return eof() ? 0 : src[pos]; }
char advance() { return eof() ? 0 : src[pos++]; }
```

### Switch Statements

Case labels aligned with the switch, body indented:

```cpp
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
```

For longer cases, braces around each case body:

```cpp
switch (kind) {
case TypeKind::Scalar: {
    auto name = scalar_name(t.scalar);
    return name;
}
case TypeKind::Container: {
    auto inner = type_to_string(t.value_type);
    return container_name(t.container) + "<" + inner + ">";
}
default:
    return "void";
}
```

### Blank Lines

One blank line between:
- Function definitions
- Logical sections within a function
- Struct members of different categories

No blank lines between:
- Tightly related one-liners
- Consecutive case labels

### Section Comments

ASCII box-drawing characters for major sections within a file:

```cpp
// ─── Forward declarations ───

// ─── FlowArg2: base class for all pin/arg types ───

// ─── Concrete arg types ───
```

## API Design Style

### Accessor Pairs

Getters and setters share the same name. Getter is const, setter takes const ref:

```cpp
const FlowNodeBuilderPtr& node() const;
void node(const FlowNodeBuilderPtr& n);

const NetBuilderPtr& net() const;
void net(const NetBuilderPtr& w);

const PortDesc2* port() const { return port_; }
void port(const PortDesc2* p) { port_ = p; }
```

### Boolean Queries

Prefixed with `is_` or `has_`:

```cpp
bool is(ArgKind k) const;
bool is_remap() const;
bool is_dirty() const;
bool is_numeric(const TypePtr& t);
bool is_integer(const TypePtr& t);
bool is_float(const TypePtr& t);
bool is_generic(const TypePtr& t);
bool is_category_sigil(char c);
bool has_error() const;
```

### Factory Methods

Object creation through named factory methods on the owning container:

```cpp
// GraphBuilder owns creation of all arg types
auto arg = gb->build_arg_net(...);
auto arg = gb->build_arg_number(...);
auto arg = gb->build_arg_string(...);
auto arg = gb->build_arg_expr(...);

// Private constructors prevent external creation
struct ArgNet2 : FlowArg2 {
    friend struct GraphBuilder;
private:
    ArgNet2(const std::shared_ptr<GraphBuilder>& owner);
};
```

### Conversion Methods

`as_*` for type-safe downcasting:

```cpp
std::shared_ptr<ArgNet2> as_net();
std::shared_ptr<ArgNumber2> as_number();
std::shared_ptr<ArgString2> as_string();
std::shared_ptr<ArgExpr2> as_expr();

FlowNodeBuilder* as_node();
NetBuilder* as_net();
```

### Computed Properties

Methods that derive values from state use descriptive names:

```cpp
std::string fq_name() const;        // fully-qualified name: "node.port_name"
std::string name() const;           // short name: "port_name" or "va_name[idx]"
unsigned remap_idx() const;          // index in remaps array
unsigned input_pin_idx() const;      // index in parsed_args
unsigned output_pin_idx() const;     // index in outputs
unsigned input_pin_va_idx() const;   // index in parsed_va_args
unsigned output_pin_va_idx() const;  // index in outputs_va_args
```

## Type System Style

### Type String Conventions

Types are represented as strings for serialization and display. Canonical formats:

```
literal<T,V>          # no spaces around comma
symbol<name,type>     # no spaces
type<T>               # metatype wrapper
vector<f32>           # container with element type
map<string, f32>      # container with key,value (space after comma)
(x:f32 y:f32)->f32    # function type (space between args)
{x:f32 y:f32}         # struct type (space between fields)
{x:1.0f, y:2.0f}      # struct literal (comma = runtime)
array<f32, 4, 4>       # fixed-size array
```

### Category Sigils

Value categories are prefix sigils on wire types:

| Sigil | Category  | Example      |
|-------|-----------|--------------|
| `%`   | Data      | `%f32`       |
| `&`   | Reference | `&f32`       |
| `^`   | Iterator  | `^vector<f32>` |
| `@`   | Lambda    | `@(f32)->f32`  |
| `#`   | Enum      | `#my_enum`   |
| `!`   | Bang      | `!`          |
| `~`   | Event     | `~on_key`    |

Data (`%`) is the default and typically omitted.

### Pin Reference Style

In expressions, pin references use `$N` (numeric only):

```
$0 + $1 * sin($2)
$0.field_name
$0[$1]
```

Lambda references use `@N`:

```
@0($1, $2)
```

Variable references are bare identifiers:

```
my_var + 1.0f
```

## Editor Visual Style

### Style Struct

All visual constants centralized in a style struct `S`:

```cpp
struct Editor2Style {
    // Colors
    ImVec4 node_bg;
    ImVec4 node_border;
    ImVec4 wire_color;
    ImVec4 hover_highlight;
    ImVec4 selection_color;
    ImVec4 error_color;
    ImVec4 grid_color;

    // Sizes
    float node_padding;
    float pin_radius;
    float wire_thickness;
    float font_size;
    float grid_spacing;
    float scroll_pan_speed;

    // Thresholds
    float hover_distance;
    float snap_distance;
    float zoom_min;
    float zoom_max;
};
```

### Pin Shape Language

Pin shapes communicate type categories at a glance:

| Shape    | Meaning              | Used For                   |
|----------|----------------------|----------------------------|
| Circle   | Data value           | Standard typed pins        |
| Square   | Trigger signal       | Bang pins (!-category)     |
| Triangle | Function reference   | Lambda pins (@-category)   |
| Diamond  | Extensible/optional  | Variadic args, optional inputs |

### Wire Drawing

Wires use bezier curves with consistent control point offsets:

```
Source pin ───╮
              │  (horizontal offset based on distance)
              ╰──── Target pin
```

Wire hover detection uses `WireInfo` structs storing precomputed bezier points
for efficient hit testing.

### Node Layout

Nodes are rectangles with:
- Header bar (node type name, background tinted by category)
- Input pins on the left edge
- Output pins on the right edge
- Inline arg values displayed next to pins
- Side-bang on the left (for banged nodes)
- Lambda grab handle on the left (for lambda-capturing nodes)
- Error indicator (red border or error text)

### Hover Priority

When multiple elements overlap, hover priority is:
1. Pins (highest priority — biased toward selection)
2. Wires
3. Nodes (lowest priority)

This ensures pins are always selectable even when overlapping with their parent node.

## Serialization Style

### .atto File Format

TOML-like with specific conventions:

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

[[node]]
guid = "e073eb5950485587"
type = "new"
args = ["osc_def"]
outputs = ["$auto-df6e4aa3d0d8d2bc-out0"]
position = [1746.02, 2025.51]

[[link]]
from = "$auto-df6e4aa3d0d8d2bc-out0"
to = "$auto-831e483b4e4602dc_s1-out0"
net_name = "$osc-signal"
```

### ID Format

- Node GUIDs: 16 hex characters, generated from `mt19937_64`
- Auto-generated pin IDs: `$auto-<guid>-<pin_name>`
- Compact IDs after import: `$a-N` (hex counter)
- Named nets: `$name` prefix for system nets, bare names for user nets
- Sentinels: `$empty`, `$unconnected`

### Version Markers

Version strings follow the pattern `format@version`:
- `nanoprog@0` — original format
- `attoprog@1` — post-rename with extended pins
- `instrument@atto:0` — current formal specification

The serializer always writes the latest version. The deserializer auto-migrates
from any known version.

## Naming Conventions in the Language

### Node Types

Node type names are lowercase with underscores, banged variants have `!` suffix:

```
expr        select       new         cast
store!      append!      erase!      iterate!
lock!       select!      event!      output_mix!
decl_type   decl_var     decl_event  decl_import
on_key_down!  on_key_up!
call        call!
```

The `!` suffix indicates the node participates in bang chains (has execution ordering).

### Standard Library FFI

FFI function names use snake_case with module prefix:

```
av_create_window    av_audio_tick    av_video_tick
imgui_begin         imgui_end        imgui_text
imgui_button        imgui_slider_float
imgui_begin_child   imgui_same_line
```

### Scalar Type Names

Scalar types follow a terse, lowercase convention:

```
u8  u16  u32  u64    # unsigned integers
s8  s16  s32  s64    # signed integers
f32  f64              # floating point
```

This mirrors Rust's naming convention and avoids C++'s verbose `uint8_t` / `int32_t`.

## Comment Style

### No Excessive Documentation

The codebase uses comments sparingly. Code should be self-documenting through:
- Clear naming
- Small functions
- Obvious data flow

Comments are used when:
- The logic is genuinely non-obvious
- A design decision needs explanation
- A workaround or hack is present

```cpp
// Struct types must have at least one field
// (empty struct would be ambiguous with void)
if (fields.empty()) {
    error = "struct must have at least one field";
    return nullptr;
}
```

### Section Dividers

ASCII line dividers (not Doxygen-style blocks):

```cpp
// ─── Type parsing ───

// ─── Inference phases ───

// ─── Code generation ───
```

### No Doxygen

The project does not use Doxygen comments (`///`, `/** */`). Function documentation,
when needed, is a plain `//` comment above the declaration.

## Error Message Style

Error messages are lowercase, descriptive, and include context:

```
"empty type string"
"struct must have at least one field"
"ArgNet2: entry must not be null"
"unknown node type: <type>"
"type mismatch: expected <expected>, got <actual>"
```

No error codes — all errors are human-readable strings.

## Build System Style

### CMake Conventions

- `project()` at top level only
- Targets named after their subsystem: `attolang`, `attoc`, `attoflow`
- `target_include_directories` with PRIVATE/PUBLIC correctly scoped
- Platform-specific logic via `if(WIN32)` / `else()`
- Optional features gated behind `option()` flags

### Dependency Management

- Windows: vcpkg (`vcpkg.json` manifest mode)
- Linux/macOS: CMake `FetchContent` for SDL3 and ImGui
- No vendored dependencies in the repo (all fetched at build time)
- Core library has zero external dependencies
