#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <cstdio>

// Value category sigils
enum class TypeCategory { Data, Reference, Iterator, Lambda, Enum, Bang, Event };

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

inline bool is_category_sigil(char c) {
    return c == '%' || c == '&' || c == '^' || c == '@' || c == '#' || c == '!' || c == '~';
}

// Type kinds
enum class TypeKind {
    Void,       // void
    Bool,       // bool
    String,     // string (UTF-8)
    Mutex,      // mutex (non-copyable, reference-only)
    Scalar,     // u8, s8, u16, ..., f32, f64
    Named,      // reference to a decl_type
    Container,  // map, set, vector, list, queue, ordered_map, ordered_set
    ContainerIterator, // map_iterator, set_iterator, etc.
    Array,      // array<Vt, dim1, dim2, ...>
    Tensor,     // tensor<Vt>
    Function,   // (arg:type ...) -> ret
    Struct,     // inline struct (fields from decl_type)
    Symbol,     // compile-time symbol (defined in symbol table)
    UndefinedSymbol, // compile-time symbol (not yet in symbol table)
    MetaType,   // type<T> — a type as a first-class compile-time value
};

// Scalar subtypes
enum class ScalarType { U8, S8, U16, S16, U32, S32, U64, S64, F32, F64 };

inline bool parse_scalar(const std::string& s, ScalarType& out) {
    if (s == "u8")  { out = ScalarType::U8;  return true; }
    if (s == "s8")  { out = ScalarType::S8;  return true; }
    if (s == "u16") { out = ScalarType::U16; return true; }
    if (s == "s16") { out = ScalarType::S16; return true; }
    if (s == "u32") { out = ScalarType::U32; return true; }
    if (s == "s32") { out = ScalarType::S32; return true; }
    if (s == "u64") { out = ScalarType::U64; return true; }
    if (s == "s64") { out = ScalarType::S64; return true; }
    if (s == "f32") { out = ScalarType::F32; return true; }
    if (s == "f64") { out = ScalarType::F64; return true; }
    return false;
}

// Container subtypes
enum class ContainerKind {
    Map, OrderedMap, Set, OrderedSet, List, Queue, Vector
};

enum class IteratorKind {
    Map, OrderedMap, Set, OrderedSet, List, Vector
};

struct TypeExpr;
using TypePtr = std::shared_ptr<TypeExpr>;

// Function argument
struct FuncArg {
    std::string name;
    TypePtr type;
};

// A parsed type expression
struct TypeExpr {
    TypeKind kind = TypeKind::Void;
    TypeCategory category = TypeCategory::Data;
    bool is_generic = false;          // true for unresolved type parameters (e.g., 0 could be u8/u32/f32, vector<?>)
    bool is_unvalued_literal = false;  // true for literals whose value isn't provided yet (e.g., decl_import pin: literal<string,?>)

    // Scalar
    ScalarType scalar = ScalarType::U8;

    // Named type reference
    std::string named_ref;

    // Container
    ContainerKind container = ContainerKind::Vector;
    TypePtr key_type;    // for map types
    TypePtr value_type;  // for all containers

    // Iterator
    IteratorKind iterator = IteratorKind::Vector;
    // reuses key_type/value_type

    // Array dimensions (compile-time constants)
    std::vector<int> dimensions;

    // Function
    std::vector<FuncArg> func_args;
    TypePtr return_type;

    // Struct fields (from decl_type)
    std::vector<FuncArg> fields; // reuse FuncArg as name:type pair

    // Symbol name (for Symbol/UndefinedSymbol kinds)
    std::string symbol_name;

    // MetaType: the wrapped type T in type<T>
    TypePtr wrapped_type;

    // Literal value (for display in literal<T, V> — empty if not a literal)
    std::string literal_value;
    bool literal_signed = false;  // true for signed integer literals
};

// Type parser
// Parses a type string like "u32", "vector<f32>", "map<u64, osc_def>",
// "(x:f32 y:f32) -> f32", "array<f32, 4, 4>", etc.
struct TypeParser {
    std::string src;
    size_t pos = 0;
    std::string error;

    TypeParser(const std::string& s) : src(s) {}

    bool eof() const { return pos >= src.size(); }
    char peek() const { return eof() ? 0 : src[pos]; }
    char advance() { return eof() ? 0 : src[pos++]; }

    void skip_ws() {
        while (!eof() && (src[pos] == ' ' || src[pos] == '\t')) pos++;
    }

    std::string read_ident() {
        skip_ws();
        size_t start = pos;
        while (!eof() && (std::isalnum(src[pos]) || src[pos] == '_')) pos++;
        return src.substr(start, pos - start);
    }

    bool expect(char c) {
        skip_ws();
        if (peek() == c) { advance(); return true; }
        error = std::string("Expected '") + c + "' at position " + std::to_string(pos);
        return false;
    }

    int read_int() {
        skip_ws();
        int val = 0;
        bool found = false;
        while (!eof() && std::isdigit(src[pos])) {
            val = val * 10 + (src[pos] - '0');
            pos++;
            found = true;
        }
        if (!found) error = "Expected integer at position " + std::to_string(pos);
        return val;
    }

    TypePtr parse();
    TypePtr parse_function(TypeCategory cat);
};

// Parse a type string, returning nullptr on error
TypePtr parse_type(const std::string& s, std::string& error);

// Type registry: resolves named types and checks for circular references
struct TypeRegistry {
    // Named type definitions: name -> type fields string
    std::map<std::string, std::string> type_defs;
    // Parsed cache
    std::map<std::string, TypePtr> parsed;
    // Errors per type name
    std::map<std::string, std::string> errors;

    void clear();
    void register_type(const std::string& name, const std::string& def);
    void resolve_all();
    bool validate_type(const std::string& type_str, std::string& error);

private:
    TypePtr resolve(const std::string& name, std::set<std::string>& visited);
    bool check_refs(const TypePtr& t, std::set<std::string>& visited, std::string& error);
};

// Decay a symbol to its wrapped type. If not a symbol, returns as-is.
inline TypePtr decay_symbol(const TypePtr& t) {
    if (!t) return t;
    if (t->kind == TypeKind::Symbol && t->wrapped_type) return t->wrapped_type;
    return t;
}

// --- Type utility functions ---
// Note: these all auto-decay symbols to their wrapped types,
// so symbol<x,f32> passes is_numeric() etc.

inline bool is_numeric(const TypePtr& t) {
    auto d = decay_symbol(t);
    return d && d->kind == TypeKind::Scalar;
}

inline bool is_integer(const TypePtr& t) {
    auto d = decay_symbol(t);
    if (!d || d->kind != TypeKind::Scalar) return false;
    switch (d->scalar) {
    case ScalarType::U8: case ScalarType::S8:
    case ScalarType::U16: case ScalarType::S16:
    case ScalarType::U32: case ScalarType::S32:
    case ScalarType::U64: case ScalarType::S64:
        return true;
    default: return false;
    }
}

inline bool is_unsigned(const TypePtr& t) {
    auto d = decay_symbol(t);
    if (!d || d->kind != TypeKind::Scalar) return false;
    switch (d->scalar) {
    case ScalarType::U8: case ScalarType::U16: case ScalarType::U32: case ScalarType::U64: return true;
    default: return false;
    }
}

inline bool is_signed_int(const TypePtr& t) {
    auto d = decay_symbol(t);
    if (!d || d->kind != TypeKind::Scalar) return false;
    switch (d->scalar) {
    case ScalarType::S8: case ScalarType::S16: case ScalarType::S32: case ScalarType::S64: return true;
    default: return false;
    }
}

inline bool is_float(const TypePtr& t) {
    auto d = decay_symbol(t);
    if (!d || d->kind != TypeKind::Scalar) return false;
    return d->scalar == ScalarType::F32 || d->scalar == ScalarType::F64;
}

// Strip literal_value from a type — used when operations consume literals
// and produce runtime values. Returns the same pointer if no literal_value.
inline TypePtr strip_literal(const TypePtr& t) {
    if (!t || (t->literal_value.empty() && !t->literal_signed)) return t;
    auto r = std::make_shared<TypeExpr>(*t);
    r->literal_value.clear();
    r->literal_signed = false;
    return r;
}

inline bool is_collection(const TypePtr& t) {
    auto d = decay_symbol(t);
    if (!d) return false;
    return d->kind == TypeKind::Container || d->kind == TypeKind::Array || d->kind == TypeKind::Tensor;
}

inline TypePtr element_type(const TypePtr& t) {
    auto d = decay_symbol(t);
    if (!d) return nullptr;
    if (d->kind == TypeKind::Container || d->kind == TypeKind::Array || d->kind == TypeKind::Tensor)
        return d->value_type;
    return nullptr;
}

inline int scalar_rank(ScalarType s) {
    switch (s) {
    case ScalarType::U8: case ScalarType::S8:   return 1;
    case ScalarType::U16: case ScalarType::S16: return 2;
    case ScalarType::U32: case ScalarType::S32: return 3;
    case ScalarType::U64: case ScalarType::S64: return 4;
    case ScalarType::F32: return 5;
    case ScalarType::F64: return 6;
    }
    return 0;
}

inline bool can_upcast(ScalarType from, ScalarType to) {
    if (from == to) return true;
    bool from_u = (from == ScalarType::U8 || from == ScalarType::U16 || from == ScalarType::U32 || from == ScalarType::U64);
    bool to_u   = (to == ScalarType::U8 || to == ScalarType::U16 || to == ScalarType::U32 || to == ScalarType::U64);
    if (from_u && to_u) return scalar_rank(from) <= scalar_rank(to);
    bool from_s = (from == ScalarType::S8 || from == ScalarType::S16 || from == ScalarType::S32 || from == ScalarType::S64);
    bool to_s   = (to == ScalarType::S8 || to == ScalarType::S16 || to == ScalarType::S32 || to == ScalarType::S64);
    if (from_s && to_s) return scalar_rank(from) <= scalar_rank(to);
    return false;
}

std::string type_to_string(const TypePtr& t);
bool types_compatible(const TypePtr& from, const TypePtr& to);

// TypePool: singleton cache for common types
struct TypePool {
    TypePtr t_void, t_bool, t_string, t_mutex;
    TypePtr t_u8, t_s8, t_u16, t_s16, t_u32, t_s32, t_u64, t_s64, t_f32, t_f64;
    TypePtr t_int_literal;   // unresolved integer literal (is_generic)
    TypePtr t_float_literal; // unresolved float literal (is_generic, defaults to f64)
    TypePtr t_unknown;       // completely unresolved (is_generic)
    TypePtr t_bang;          // () -> void (bang type)
    TypePtr t_symbol;        // base symbol type
    TypePtr t_undefined_symbol; // base undefined symbol type

    std::map<std::string, TypePtr> cache;

    TypePool();

    TypePtr get_scalar(ScalarType st) {
        switch (st) {
        case ScalarType::U8: return t_u8; case ScalarType::S8: return t_s8;
        case ScalarType::U16: return t_u16; case ScalarType::S16: return t_s16;
        case ScalarType::U32: return t_u32; case ScalarType::S32: return t_s32;
        case ScalarType::U64: return t_u64; case ScalarType::S64: return t_s64;
        case ScalarType::F32: return t_f32; case ScalarType::F64: return t_f64;
        }
        return t_unknown;
    }

    TypePtr intern(const std::string& type_str) {
        if (type_str.empty() || type_str == "value") return t_unknown;
        auto it = cache.find(type_str);
        if (it != cache.end()) return it->second;
        std::string err;
        auto t = parse_type(type_str, err);
        if (!t) return t_unknown;
        cache[type_str] = t;
        return t;
    }
};
