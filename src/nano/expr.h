#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include "args.h"
#include <cmath>
#include <cctype>
#include <cassert>
#include "types.h"

// --- Expression AST ---

struct ExprNode;
using ExprPtr = std::shared_ptr<ExprNode>;

enum class ExprKind {
    IntLiteral,     // 42 (type deferred) — legacy, kept for backward compat
    F32Literal,     // 1.0f — legacy
    F64Literal,     // 1.0 — legacy
    BoolLiteral,    // true, false — legacy
    StringLiteral,  // "hello" — legacy
    PinRef,         // $0, $1, ... (only $N with digits)
    VarRef,         // $name — legacy (kept for backward compat during transition)
    BinaryOp,       // +, -, *, /, ==, !=, <, >, <=, >=, <=>
    UnaryMinus,     // -expr
    FieldAccess,    // expr.field
    Index,          // expr[expr]
    QueryIndex,     // expr?[expr]
    Slice,          // expr[start:end]
    FuncCall,       // fn(args) — function/constructor/lambda calls
    Ref,            // &expr — reference/iterator creation (top-level only)
    Deref,          // *expr — dereference iterator to value (inserted by inference)
    // --- New kinds for type system redesign ---
    Literal,        // Unified compile-time literal (int, float, string, bool)
    SymbolRef,      // Bare identifier — resolves via symbol table
    StructLiteral,  // {name:value, name:value, ...} — runtime struct construction
    StructType,     // {name:type name:type ...} — compile-time struct type
    NamespaceAccess,// a::b — namespace resolution
};

enum class BinOp { Add, Sub, Mul, Div, Eq, Ne, Lt, Gt, Le, Ge, Spaceship };

// Literal domain — the T in literal<T, V>
enum class LiteralKind {
    Integer,          // literal<integer<?>, V> — unresolved integer
    Unsigned,         // literal<unsigned<?>, V> — non-negative integer
    Signed,           // literal<signed<?>, V> — negative integer
    F32,              // literal<f32, V>
    F64,              // literal<f64, V>
    String,           // literal<string, V>
    Bool,             // literal<bool, V>
};

enum class BuiltinFunc {
    None,  // not a builtin (lambda call)
    Sin, Cos, Pow, Exp, Log,
    Or, Xor, And, Not, Mod, Rand,
};

struct PinRefInfo {
    char sigil = '$';   // $, %, &, ^, @, #, !, ~
    int index = 0;
    std::string name;   // optional :name on first use
};

// How a value is accessed — set by inference, consumed by codegen
enum class ValueAccess : uint8_t {
    Value,      // plain value (e.g. f32, s32)
    Field,      // struct field access (use '.')
    Iterator,   // iterator (use '->' for field access, '*' for deref)
    Reference,  // reference (use '.' for field access)
};

struct ExprNode {
    ExprKind kind;
    TypePtr resolved_type; // filled during inference
    ValueAccess access = ValueAccess::Value; // set by inference based on resolved_type

    // Literals
    int64_t int_value = 0;
    double float_value = 0.0;
    bool bool_value = false;
    std::string string_value;

    // PinRef
    PinRefInfo pin_ref;

    // VarRef
    std::string var_name;
    bool is_dollar_var = false; // true if came from $name, false if bare identifier

    // BinaryOp
    BinOp bin_op = BinOp::Add;

    // FieldAccess
    std::string field_name;

    // FuncCall
    std::string func_name;
    BuiltinFunc builtin = BuiltinFunc::None;

    // Literal (unified) — used when kind == Literal
    LiteralKind literal_kind = LiteralKind::Integer;

    // SymbolRef — used when kind == SymbolRef
    std::string symbol_name;

    // StructLiteral / StructType — field names (values/types are in children)
    std::vector<std::string> struct_field_names;

    // Children (meaning varies by kind)
    std::vector<ExprPtr> children;
};

inline ExprPtr make_expr(ExprKind k) {
    auto e = std::make_shared<ExprNode>();
    e->kind = k;
    return e;
}

// --- Expression Tokenizer ---

enum class ExprTokenKind {
    Int, Float, Ident, String, PinRef,
    Plus, Minus, Star, Slash,
    Eq, Ne, Lt, Gt, Le, Ge, Spaceship,
    Dot, LBrack, RBrack, LParen, RParen,
    Colon, Question, Comma, Ampersand,
    LBrace, RBrace,     // { }
    ColonColon,          // ::
    Eof, Error,
};

struct ExprToken {
    ExprTokenKind kind = ExprTokenKind::Eof;
    std::string text;
    // For numeric tokens
    int64_t int_val = 0;
    double float_val = 0.0;
    bool is_f32 = false; // true if ends with 'f'
    // For PinRef
    PinRefInfo pin_ref;
};

struct ExprTokenizer {
    std::string src;
    size_t pos = 0;
    std::string error;

    ExprTokenizer(const std::string& s) : src(s) {}

    bool eof() const { return pos >= src.size(); }
    char peek() const { return eof() ? 0 : src[pos]; }
    char peek2() const { return (pos + 1 < src.size()) ? src[pos + 1] : 0; }
    char advance() { return eof() ? 0 : src[pos++]; }

    void skip_ws() {
        while (!eof() && (src[pos] == ' ' || src[pos] == '\t')) pos++;
    }

    bool is_sigil(char c) {
        // Only $ is a pin ref sigil now. Other sigils (%, &, ^, @, #, !, ~)
        // are no longer used for pin references — values are accessed as plain symbols.
        return c == '$';
    }

    ExprToken next();
};

// --- Slot info collected during parsing ---

struct ExprSlotInfo {
    std::map<int, char> slots;    // index → sigil char
    std::map<int, std::string> slot_names; // index → name (from first :name occurrence)
    int max_slot = -1;
    bool has_any_args = false;    // true if args string was non-empty

    void add(int index, char sigil, const std::string& name = "") {
        if (index < 0) return; // var refs don't count
        slots[index] = sigil;
        if (!name.empty() && slot_names.find(index) == slot_names.end())
            slot_names[index] = name;
        if (index > max_slot) max_slot = index;
    }

    bool is_lambda_slot(int n) const {
        auto it = slots.find(n);
        return it != slots.end() && it->second == '@';
    }

    int total_pin_count(int type_default) const {
        if (max_slot >= 0) return max_slot + 1;
        if (has_any_args) return 0; // args exist but no $N/@N refs
        return type_default;
    }
};

// --- Expression Parser (recursive descent) ---

struct ExprParser {
    ExprTokenizer tokenizer;
    ExprToken current;
    std::string error;
    ExprSlotInfo slot_info;
    std::vector<std::string> var_refs; // collected $name references

    ExprParser(const std::string& src);

    void advance();
    bool check(ExprTokenKind k) const;
    bool expect(ExprTokenKind k);

    ExprPtr parse();
    ExprPtr parse_expr();
    ExprPtr parse_equality();
    ExprPtr parse_comparison();
    ExprPtr parse_additive();
    ExprPtr parse_multiplicative();
    ExprPtr parse_unary();
    ExprPtr parse_postfix();
    ExprPtr parse_primary();
    ExprPtr parse_struct_expr(); // { ... } — struct literal or struct type

    static BuiltinFunc lookup_builtin(const std::string& name);
};

// --- Convenience: parse expression string ---

struct ParsedExpr {
    ExprPtr root;
    ExprSlotInfo slots;
    std::vector<std::string> var_refs;
    std::string error;
};

ParsedExpr parse_expression(const std::string& src);

// --- NodeArgs: unified parse result ---

struct NodeArgs {
    std::vector<ExprPtr> exprs;         // parsed ASTs (one per expression token)
    ExprSlotInfo slots;                 // unified slot info across all args
    std::vector<std::string> var_refs;  // collected variable references
    std::string error;
    bool has_any_args = false;
};

ExprSlotInfo scan_slots(const std::string& s);
NodeArgs parse_node_expr(const std::string& args_str);

// --- Inline expression support for non-expr nodes ---

struct InlineArgInfo {
    int num_inline_args = 0;        // how many descriptor inputs are filled by inline args
    ExprSlotInfo pin_slots;         // $N/@N refs found across all inline args
    int remaining_descriptor_inputs = 0; // descriptor inputs not covered by args
    int total_pins = 0;             // pin_slots.max_slot+1 + remaining_descriptor_inputs
    std::string error;              // validation error (too many args, gaps in $N, etc.)
    std::vector<std::string> tokens; // tokenized arg strings
};

InlineArgInfo compute_inline_args(const std::string& args, int descriptor_inputs);

void clear_expr_types(const ExprPtr& expr);
bool is_lvalue(const ExprPtr& e);
void collect_slots(const ExprPtr& expr, ExprSlotInfo& info);
std::string expr_to_string(const ExprPtr& e);

// --- Type Inference Engine ---

struct SymbolTable; // forward declaration

struct TypeInferenceContext {
    TypePool& pool;
    TypeRegistry& registry;
    SymbolTable* symbol_table = nullptr; // for resolving bare identifiers

    // Pin types: pin_index → resolved type (for the current node's inputs)
    std::map<int, TypePtr> input_pin_types;
    // Variable types: name → resolved type (from decl_var nodes)
    std::map<std::string, TypePtr> var_types;
    // Named type definitions: name → resolved struct type
    std::map<std::string, TypePtr> named_types;
    // Errors collected during inference
    std::vector<std::string> errors;

    TypeInferenceContext(TypePool& p, TypeRegistry& r) : pool(p), registry(r) {}

    void add_error(const std::string& msg);
    TypePtr resolve_named(const std::string& name);
    TypePtr resolve_type(const TypePtr& t);
    TypePtr find_field_type(const TypePtr& obj_type, const std::string& field_name);
    TypePtr infer(const ExprPtr& expr);
    TypePtr infer_binary_op(const ExprPtr& expr);
    TypePtr infer_scalar_binop(const TypePtr& left_t, const TypePtr& right_t, bool is_comparison, BinOp op);
    TypePtr make_collection_of(const TypePtr& collection, const TypePtr& elem_type);
    TypePtr infer_ref(const ExprPtr& expr);
    TypePtr infer_func_call(const ExprPtr& expr);
    TypePtr infer_builtin_call(const ExprPtr& expr);
    void resolve_int_literals(const ExprPtr& expr, const TypePtr& expected);
};
