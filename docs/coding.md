# Organic Assembler — Coding Conventions

This document describes the C++ coding conventions, idioms, and practices used throughout
the Organic Assembler codebase.

## Language Standard

The project uses **C++20** (`CMAKE_CXX_STANDARD 20`). Features actively used:

- `std::variant<>` for algebraic data types
- `std::shared_ptr<>` / `std::weak_ptr<>` / `std::unique_ptr<>` for ownership
- `std::enable_shared_from_this` for safe self-references
- Range-based for loops everywhere
- Lambda expressions with captures for callbacks and inline logic
- `constexpr` for compile-time computation
- Default member initializers
- `auto` for type deduction (used judiciously)
- Fold expressions: `((id == ids) || ...)` for variadic checks
- Scoped enums (`enum class`) exclusively

Features intentionally **not** used:
- RTTI / `dynamic_cast` on raw pointers (only on `shared_ptr` casts)
- Exceptions as control flow (used only for invariant violations)
- Concepts / constraints
- Modules (C++20)
- Coroutines
- `std::optional<>` (error strings or variants preferred)
- Structured bindings (rarely used)

## Naming Conventions

### Types and Classes
```cpp
// PascalCase for all type names
struct FlowGraph;
struct GraphBuilder;
struct TypeExpr;
class CodeGenerator;
enum class NodeTypeID;
enum class TypeKind;
```

### Functions and Methods
```cpp
// snake_case for all functions and methods
void add_node();
void mark_dirty();
void rebuild_pin_ids();
TypePtr resolve_type();
bool is_numeric(const TypePtr& t);
```

### Variables
```cpp
// snake_case for locals and parameters
int result;
FlowNode* source_node;
int temp_counter;

// snake_case with trailing underscore for private members
ArgKind kind_;
std::shared_ptr<GraphBuilder> owner_;
FlowNodeBuilderPtr node_;
bool dirty_ = false;

// snake_case without underscore for public members
std::vector<FlowNode> nodes;
std::vector<FlowLink> links;
std::map<NodeId, BuilderEntryPtr> entries;
```

### Enum Values
```cpp
// PascalCase for enum values
enum class TypeKind { Void, Bool, String, Scalar, Named, Container, ... };
enum class NodeTypeID : uint8_t { Expr, New, Dup, Str, Select, ... };
enum class ArgKind : uint8_t { Net, Number, String, Expr };
enum class IdCategory { Node, Net };
```

### Type Aliases
```cpp
// PascalCase with descriptive suffixes
using TypePtr = std::shared_ptr<TypeExpr>;
using ExprPtr = std::shared_ptr<ExprNode>;
using FlowArg2Ptr = std::shared_ptr<FlowArg2>;
using FlowNodeBuilderPtr = std::shared_ptr<FlowNodeBuilder>;
using NetBuilderPtr = std::shared_ptr<NetBuilder>;
using BuilderEntryWeak = std::weak_ptr<BuilderEntry>;
using NodeId = std::string;
using BuilderError = std::string;
using Remaps = std::vector<std::pair<std::string, std::string>>;
```

### Constants and Statics
```cpp
// Static arrays for descriptors
static const NodeType NODE_TYPES[] = { ... };
static constexpr int NUM_NODE_TYPES = sizeof(...) / sizeof(...);

// Inline helpers in headers
inline bool is_numeric(const TypePtr& t);
inline TypeCategory parse_category(char c);
```

## Header Guards

Always `#pragma once`. No `#ifndef` guards are used anywhere in the codebase.

```cpp
#pragma once
#include <string>
#include <vector>
// ...
```

## Include Order

1. Standard library headers
2. Project headers

```cpp
#pragma once
#include "model.h"
#include "types.h"
#include "node_types.h"
#include "graph_editor_interfaces.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <variant>
#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <functional>
#include <set>
```

Note: project headers often come first, then standard headers. This is the established
convention — not the Google style of standard-first.

## Memory Management

### Ownership Model

The codebase uses smart pointers consistently. Raw pointers are only for non-owning
references within the same scope.

```cpp
// Shared ownership for graph entries
std::shared_ptr<FlowNodeBuilder> node;
std::shared_ptr<NetBuilder> net;
std::shared_ptr<TypeExpr> type;

// Unique ownership for pins
std::unique_ptr<FlowPin> pin;

// Weak references for cycle breaking
std::weak_ptr<IGraphEditor> editor;
std::vector<std::weak_ptr<IArgNetEditor>> net_editors;

// Raw pointers only for non-owning, same-scope references
const PortDesc2* port = nullptr;  // borrowed from node type descriptor
FlowNode* node_ptr;               // temporary within a function
```

### Move Semantics

FlowGraph and similar containers are movable but non-copyable:

```cpp
FlowGraph(FlowGraph&&) = default;
FlowGraph& operator=(FlowGraph&&) = default;
FlowGraph(const FlowGraph&) = delete;
FlowGraph& operator=(const FlowGraph&) = delete;
```

### enable_shared_from_this

Used on objects that need to hand out shared pointers to themselves:

```cpp
struct FlowArg2 : std::enable_shared_from_this<FlowArg2> {
    // Can safely call shared_from_this() in methods
};

struct BuilderEntry : std::enable_shared_from_this<BuilderEntry> {
    // Entries can refer to themselves in observer callbacks
};
```

## Error Handling

### Variant-Based Results (Primary Pattern)

Functions that can fail return a variant of success/error:

```cpp
using BuilderResult = std::variant<std::pair<NodeId, FlowNodeBuilder>, BuilderError>;
using ParseAttoResult = std::variant<std::shared_ptr<GraphBuilder>, BuilderError>;
using ParseResult = std::variant<std::shared_ptr<ParsedArgs2>, std::string>;
using SplitResult = std::variant<std::vector<std::string>, std::string>;
```

Usage:
```cpp
auto result = parse_atto(stream);
if (auto* err = std::get_if<BuilderError>(&result)) {
    // handle error
} else {
    auto& gb = std::get<std::shared_ptr<GraphBuilder>>(result);
    // use graph builder
}
```

### Error Strings on Objects

Objects that can be in an error state carry an error string:

```cpp
struct FlowNode {
    std::string error;  // non-empty if node has a type error
};

struct FlowLink {
    std::string error;  // non-empty if connection is invalid
};
```

### Exceptions for Invariant Violations

Exceptions are reserved for programmer errors, never for expected failures:

```cpp
if (!entry_) throw std::logic_error("ArgNet2: entry must not be null");
if (!owner_) throw std::logic_error("FlowArg2: owner must not be null");
```

These should never fire in correct code — they guard against impossible states.

### Early Returns

Functions validate preconditions and return early:

```cpp
bool validate_type(const std::string& type_str, std::string& error) {
    if (type_str.empty()) {
        error = "empty type string";
        return false;
    }
    // ...
    return true;
}
```

## Enums

Always `enum class` (scoped), never plain `enum`:

```cpp
enum class TypeKind {
    Void, Bool, String, Mutex, Scalar, Named, Container,
    ContainerIterator, Array, Tensor, Function, Struct,
    Symbol, UndefinedSymbol, MetaType
};

enum class TypeCategory { Data, Reference, Iterator, Lambda, Enum, Bang, Event };
enum class ScalarType { U8, S8, U16, S16, U32, S32, U64, S64, F32, F64 };
enum class ContainerKind { Map, OrderedMap, Set, OrderedSet, List, Queue, Vector };
enum class ArgKind : uint8_t { Net, Number, String, Expr };
enum class NodeTypeID : uint8_t { /* 38 values */ };
```

Underlying types (`: uint8_t`) specified when the enum is stored in compact structures.

## Discriminated Unions

Two approaches are used, depending on context:

### std::variant (for simple unions)
```cpp
using FlowArg = std::variant<ArgPortRef, ArgLambdaRef, ArgVariable, ...>;
using HoverItem = std::variant<std::monostate, BuilderEntryPtr, FlowArg2Ptr, AddPinHover>;
```

### Kind enum + as_* methods (for complex hierarchies)
```cpp
struct FlowArg2 {
    ArgKind kind() const { return kind_; }
    bool is(ArgKind k) const { return kind_ == k; }

    std::shared_ptr<ArgNet2> as_net();
    std::shared_ptr<ArgNumber2> as_number();
    std::shared_ptr<ArgString2> as_string();
    std::shared_ptr<ArgExpr2> as_expr();
};
```

The kind-based approach is preferred when the types share substantial base behavior
and need `shared_from_this()`.

## String Handling

- `std::string` for all text — no `char*` arrays
- Pass by `const std::string&` for inputs
- `std::move()` for ownership transfers
- String IDs for nodes and pins: `"guid.pin_name"` format

```cpp
// ID generation
inline std::string generate_guid() {
    static std::mt19937_64 rng(std::random_device{}());
    // hex from RNG
    return s;
}

// Tokenization with nesting awareness
std::vector<std::string> tokenize_args(const std::string& args);
std::vector<std::string> split_args(const std::string& args);
```

## Inline Functions

Type utility functions are `inline` in headers for zero-overhead abstraction:

```cpp
inline bool is_numeric(const TypePtr& t) { ... }
inline bool is_integer(const TypePtr& t) { ... }
inline bool is_float(const TypePtr& t) { ... }
inline TypePtr decay_symbol(const TypePtr& t) { ... }
inline TypePtr strip_literal(const TypePtr& t) { ... }
inline bool is_category_sigil(char c) { ... }
```

## Static Helpers in .cpp Files

Implementation-only helpers are `static` in the .cpp file, never in headers:

```cpp
// In serial.cpp
static std::string trim(std::string s);
static std::string unescape_toml(const std::string& s);
static std::string unquote(const std::string& s);
static std::vector<std::string> parse_toml_array(const std::string& val);
```

## File Organization

Each `.h` / `.cpp` pair covers a single concept:

| Pair                  | Concept                    |
|-----------------------|----------------------------|
| `types.h/cpp`         | Type system and parsing    |
| `expr.h/cpp`          | Expression AST and parsing |
| `inference.h/cpp`     | Type inference algorithm   |
| `graph_builder.h/cpp` | Structured graph editing   |
| `serial.h/cpp`        | .atto serialization        |
| `codegen.h/cpp`       | C++ code generation        |
| `shadow.h/cpp`        | Shadow expression nodes    |
| `symbol_table.h/cpp`  | Symbol resolution          |
| `type_utils.h/cpp`    | Type compatibility utils   |
| `model.h`             | Core data structures       |

Headers contain:
1. `#pragma once`
2. Includes (project, then standard)
3. Forward declarations
4. Type aliases
5. Struct/class definitions
6. Inline function implementations

Implementation files contain:
1. Header include
2. Additional includes
3. Static helper functions
4. Method implementations

## Templated Helpers

Variadic templates with fold expressions for type checks:

```cpp
template<typename... Ts>
constexpr bool is_any_of(NodeTypeID id, Ts... ids) {
    return ((id == ids) || ...);
}
```

## Macros

Macro usage is minimal and confined:

1. **Test framework** (only in test_inference.cpp):
   ```cpp
   #define TEST(name) static void test_##name()
   #define ASSERT(cond) if (!(cond)) { printf(...); tests_failed++; return; }
   #define ASSERT_EQ(a, b)
   #define ASSERT_TYPE(pin_ptr, expected_str)
   ```

2. **No preprocessor configuration** — all configuration is runtime state

## Constructor Patterns

### Private Constructors with Factory Methods
```cpp
struct ArgNet2 : FlowArg2 {
    friend struct GraphBuilder;  // only GraphBuilder can create
private:
    ArgNet2(const std::shared_ptr<GraphBuilder>& owner);
};

// Creation via factory
auto arg = gb->build_arg_net(...);
```

### Default Member Initialization
```cpp
struct TypeExpr {
    TypeKind kind = TypeKind::Void;
    TypeCategory category = TypeCategory::Data;
    bool is_generic = false;
    bool is_unvalued_literal = false;
    ScalarType scalar = ScalarType::U8;
    ContainerKind container = ContainerKind::Vector;
    const PortDesc2* port_ = nullptr;
    bool dirty_ = false;
};
```

## Container Usage

- `std::vector<>` — primary sequential container
- `std::map<>` — ordered associative (for deterministic iteration)
- `std::set<>` — ordered unique collection (for selections)
- `std::unordered_map<>` — rare, only for performance-critical lookups
- `std::erase_if()` — for filtered removal from containers

## const Correctness

- Input parameters: `const std::string&`, `const TypePtr&`
- Accessor methods: `const` qualified
- Mutable state: clearly non-const

```cpp
ArgKind kind() const { return kind_; }
const PortDesc2* port() const { return port_; }
const FlowNodeBuilderPtr& node() const;
void node(const FlowNodeBuilderPtr& n);  // setter is non-const
```
