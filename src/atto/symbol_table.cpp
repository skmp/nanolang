#include "symbol_table.h"

// Helper: create a type<T> wrapper (MetaType)
static TypePtr make_meta_type(const TypePtr& inner) {
    auto t = std::make_shared<TypeExpr>();
    t->kind = TypeKind::MetaType;
    t->wrapped_type = inner;
    return t;
}

// Helper: create a function type
static TypePtr make_func_type(std::vector<FuncArg> args, TypePtr ret) {
    auto t = std::make_shared<TypeExpr>();
    t->kind = TypeKind::Function;
    t->func_args = std::move(args);
    t->return_type = std::move(ret);
    return t;
}

// Helper: create a generic float type (float<?>)
static TypePtr make_generic_float() {
    auto t = std::make_shared<TypeExpr>();
    t->kind = TypeKind::Scalar;
    t->scalar = ScalarType::F64;
    t->is_generic = true;
    return t;
}

// Helper: create a generic integer type (integer<?>)
static TypePtr make_generic_integer() {
    auto t = std::make_shared<TypeExpr>();
    t->kind = TypeKind::Scalar;
    t->is_generic = true;
    return t;
}

// Helper: create a generic numeric type (numeric<?>)
static TypePtr make_generic_numeric() {
    auto t = std::make_shared<TypeExpr>();
    t->kind = TypeKind::Scalar;
    t->is_generic = true;
    return t;
}

// Helper: create a container type<T> metatype with unresolved inner
static TypePtr make_container_meta(ContainerKind kind, int params) {
    auto inner = std::make_shared<TypeExpr>();
    inner->kind = TypeKind::Container;
    inner->container = kind;
    inner->is_generic = true;
    if (params == 2) {
        auto unknown = std::make_shared<TypeExpr>();
        unknown->kind = TypeKind::Void;
        unknown->is_generic = true;
        inner->key_type = unknown;
    }
    auto unknown = std::make_shared<TypeExpr>();
    unknown->kind = TypeKind::Void;
    unknown->is_generic = true;
    inner->value_type = unknown;
    return make_meta_type(inner);
}

void SymbolTable::populate_builtins(TypePool& pool) {
    auto add_builtin = [&](const std::string& name, TypePtr type) {
        entries[name] = {name, std::move(type), SymbolEntry::Builtin};
    };

    // --- Math functions: (float<T:?>) -> float<T> ---
    // Represented as generic function types
    auto float_unary = make_func_type({{"x", make_generic_float()}}, make_generic_float());
    add_builtin("sin", float_unary);
    add_builtin("cos", float_unary);
    add_builtin("exp", float_unary);
    add_builtin("log", float_unary);

    // pow: (float<T:?>, float<T>) -> float<T>  (integer overload handled in inference)
    auto pow_type = make_func_type({{"base", make_generic_float()}, {"exp", make_generic_float()}}, make_generic_float());
    add_builtin("pow", pow_type);

    // --- Logical/bitwise: (integer<T:?>, integer<T>) -> integer<T> ---
    auto int_binary = make_func_type({{"a", make_generic_integer()}, {"b", make_generic_integer()}}, make_generic_integer());
    add_builtin("or", int_binary);
    add_builtin("xor", int_binary);
    add_builtin("and", int_binary);

    auto int_unary = make_func_type({{"a", make_generic_integer()}}, make_generic_integer());
    add_builtin("not", int_unary);

    // mod, rand: (numeric<T:?>, numeric<T>) -> numeric<T>
    auto num_binary = make_func_type({{"a", make_generic_numeric()}, {"b", make_generic_numeric()}}, make_generic_numeric());
    add_builtin("mod", num_binary);
    add_builtin("rand", num_binary);

    // --- Constants ---
    add_builtin("pi", make_generic_float());
    add_builtin("e", make_generic_float());
    add_builtin("tau", make_generic_float());

    // --- Booleans ---
    {
        auto t_true = std::make_shared<TypeExpr>(*pool.t_bool);
        t_true->literal_value = "true";
        add_builtin("true", t_true);
        auto t_false = std::make_shared<TypeExpr>(*pool.t_bool);
        t_false->literal_value = "false";
        add_builtin("false", t_false);
    }

    // --- Scalar type symbols -> type<T> ---
    add_builtin("f32", make_meta_type(pool.t_f32));
    add_builtin("f64", make_meta_type(pool.t_f64));
    add_builtin("u8",  make_meta_type(pool.t_u8));
    add_builtin("u16", make_meta_type(pool.t_u16));
    add_builtin("u32", make_meta_type(pool.t_u32));
    add_builtin("u64", make_meta_type(pool.t_u64));
    add_builtin("s8",  make_meta_type(pool.t_s8));
    add_builtin("s16", make_meta_type(pool.t_s16));
    add_builtin("s32", make_meta_type(pool.t_s32));
    add_builtin("s64", make_meta_type(pool.t_s64));

    // --- Special type symbols ---
    add_builtin("bool",   make_meta_type(pool.t_bool));
    add_builtin("string", make_meta_type(pool.t_string));
    add_builtin("void",   make_meta_type(pool.t_void));
    add_builtin("mutex",  make_meta_type(pool.t_mutex));

    // --- Container type symbols -> type<container<?>> ---
    add_builtin("vector",      make_container_meta(ContainerKind::Vector, 1));
    add_builtin("map",         make_container_meta(ContainerKind::Map, 2));
    add_builtin("set",         make_container_meta(ContainerKind::Set, 1));
    add_builtin("list",        make_container_meta(ContainerKind::List, 1));
    add_builtin("queue",       make_container_meta(ContainerKind::Queue, 1));
    add_builtin("ordered_map", make_container_meta(ContainerKind::OrderedMap, 2));
    add_builtin("ordered_set", make_container_meta(ContainerKind::OrderedSet, 1));

    // --- Array and tensor type symbols ---
    {
        auto array_type = std::make_shared<TypeExpr>();
        array_type->kind = TypeKind::Array;
        array_type->value_type = pool.t_unknown;
        auto array_meta = std::make_shared<TypeExpr>();
        array_meta->kind = TypeKind::MetaType;
        array_meta->wrapped_type = array_type;
        add_builtin("array", array_meta);

        auto tensor_type = std::make_shared<TypeExpr>();
        tensor_type->kind = TypeKind::Tensor;
        tensor_type->value_type = pool.t_unknown;
        auto tensor_meta = std::make_shared<TypeExpr>();
        tensor_meta->kind = TypeKind::MetaType;
        tensor_meta->wrapped_type = tensor_type;
        add_builtin("tensor", tensor_meta);
    }
}

SymbolEntry* SymbolTable::lookup(const std::string& name) {
    auto it = entries.find(name);
    return it != entries.end() ? &it->second : nullptr;
}

const SymbolEntry* SymbolTable::lookup(const std::string& name) const {
    auto it = entries.find(name);
    return it != entries.end() ? &it->second : nullptr;
}

void SymbolTable::add(const std::string& name, TypePtr decay_type, SymbolEntry::Source src) {
    entries[name] = {name, std::move(decay_type), src};
}

bool SymbolTable::has(const std::string& name) const {
    return entries.count(name) > 0;
}

void SymbolTable::clear_declarations() {
    for (auto it = entries.begin(); it != entries.end(); ) {
        if (it->second.source == SymbolEntry::Declaration)
            it = entries.erase(it);
        else
            ++it;
    }
}
