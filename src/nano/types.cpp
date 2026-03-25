#include "types.h"

// --- TypeParser method bodies ---

TypePtr TypeParser::parse() {
    skip_ws();
    if (eof()) { error = "Empty type"; return nullptr; }

    auto result = std::make_shared<TypeExpr>();

    // Check for category sigil
    if (is_category_sigil(peek())) {
        result->category = parse_category(advance());
    }

    // Check for function type
    if (peek() == '(') {
        return parse_function(result->category);
    }

    // Check for struct type: {field:type field:type ...}
    if (peek() == '{') {
        advance(); // consume '{'
        auto st = std::make_shared<TypeExpr>();
        st->kind = TypeKind::Struct;
        st->category = result->category;
        skip_ws();
        while (peek() != '}' && !eof()) {
            std::string field_name = read_ident();
            if (field_name.empty()) { error = "Expected field name in struct type"; return nullptr; }
            if (!expect(':')) return nullptr;
            auto field_type = parse();
            if (!field_type) return nullptr;
            st->fields.push_back({field_name, field_type});
            skip_ws();
        }
        if (st->fields.empty()) { error = "Struct type must have at least one field"; return nullptr; }
        if (!expect('}')) return nullptr;
        return st;
    }

    std::string name = read_ident();
    if (name.empty()) { error = "Expected type name at position " + std::to_string(pos); return nullptr; }

    // void
    if (name == "void") { result->kind = TypeKind::Void; return result; }
    // bool
    if (name == "bool") { result->kind = TypeKind::Bool; return result; }
    // string
    if (name == "string") { result->kind = TypeKind::String; return result; }
    if (name == "mutex") { result->kind = TypeKind::Mutex; return result; }

    // Scalar
    ScalarType st;
    if (parse_scalar(name, st)) {
        result->kind = TypeKind::Scalar;
        result->scalar = st;
        return result;
    }

    // Container types
    struct ContainerInfo { const char* name; ContainerKind kind; int params; };
    static const ContainerInfo containers[] = {
        {"map", ContainerKind::Map, 2},
        {"ordered_map", ContainerKind::OrderedMap, 2},
        {"set", ContainerKind::Set, 1},
        {"ordered_set", ContainerKind::OrderedSet, 1},
        {"list", ContainerKind::List, 1},
        {"queue", ContainerKind::Queue, 1},
        {"vector", ContainerKind::Vector, 1},
    };
    for (auto& ci : containers) {
        if (name == ci.name) {
            result->kind = TypeKind::Container;
            result->container = ci.kind;
            if (!expect('<')) return nullptr;
            if (ci.params == 2) {
                result->key_type = parse();
                if (!result->key_type) return nullptr;
                skip_ws();
                if (!expect(',')) return nullptr;
            }
            result->value_type = parse();
            if (!result->value_type) return nullptr;
            skip_ws();
            if (!expect('>')) return nullptr;
            return result;
        }
    }

    // Iterator types
    struct IterInfo { const char* name; IteratorKind kind; int params; };
    static const IterInfo iterators[] = {
        {"map_iterator", IteratorKind::Map, 2},
        {"ordered_map_iterator", IteratorKind::OrderedMap, 2},
        {"set_iterator", IteratorKind::Set, 1},
        {"ordered_set_iterator", IteratorKind::OrderedSet, 1},
        {"list_iterator", IteratorKind::List, 1},
        {"vector_iterator", IteratorKind::Vector, 1},
    };
    for (auto& ii : iterators) {
        if (name == ii.name) {
            result->kind = TypeKind::ContainerIterator;
            result->iterator = ii.kind;
            if (!expect('<')) return nullptr;
            if (ii.params == 2) {
                result->key_type = parse();
                if (!result->key_type) return nullptr;
                skip_ws();
                if (!expect(',')) return nullptr;
            }
            result->value_type = parse();
            if (!result->value_type) return nullptr;
            skip_ws();
            if (!expect('>')) return nullptr;
            return result;
        }
    }

    // array<Vt, dim1, dim2, ...>
    if (name == "array") {
        result->kind = TypeKind::Array;
        if (!expect('<')) return nullptr;
        result->value_type = parse();
        if (!result->value_type) return nullptr;
        // Read dimensions (at least one)
        while (true) {
            skip_ws();
            if (peek() == '>') break;
            if (!expect(',')) return nullptr;
            int dim = read_int();
            if (!error.empty()) return nullptr;
            result->dimensions.push_back(dim);
        }
        if (result->dimensions.empty()) {
            error = "array must have at least one dimension";
            return nullptr;
        }
        if (!expect('>')) return nullptr;
        return result;
    }

    // tensor<Vt>
    if (name == "tensor") {
        result->kind = TypeKind::Tensor;
        if (!expect('<')) return nullptr;
        result->value_type = parse();
        if (!result->value_type) return nullptr;
        skip_ws();
        if (!expect('>')) return nullptr;
        return result;
    }

    // type<T> — metatype
    if (name == "type") {
        skip_ws();
        if (peek() == '<') {
            advance();
            auto inner = parse();
            if (!inner) return nullptr;
            skip_ws();
            if (!expect('>')) return nullptr;
            result->kind = TypeKind::MetaType;
            result->wrapped_type = inner;
            return result;
        }
        // bare "type" without <> is a named type reference
    }

    // symbol / undefined_symbol as type names
    if (name == "symbol") {
        result->kind = TypeKind::Symbol;
        return result;
    }
    if (name == "undefined_symbol") {
        result->kind = TypeKind::UndefinedSymbol;
        return result;
    }

    // Named type reference
    result->kind = TypeKind::Named;
    result->named_ref = name;
    return result;
}

TypePtr TypeParser::parse_function(TypeCategory cat) {
    auto result = std::make_shared<TypeExpr>();
    result->kind = TypeKind::Function;
    result->category = cat;

    if (!expect('(')) return nullptr;
    skip_ws();
    while (peek() != ')' && !eof()) {
        std::string arg_name = read_ident();
        if (arg_name.empty()) { error = "Expected argument name"; return nullptr; }
        if (!expect(':')) return nullptr;
        auto arg_type = parse();
        if (!arg_type) return nullptr;
        result->func_args.push_back({arg_name, arg_type});
        skip_ws();
    }
    if (!expect(')')) return nullptr;
    skip_ws();

    // -> return type
    if (pos + 1 < src.size() && src[pos] == '-' && src[pos + 1] == '>') {
        pos += 2;
        result->return_type = parse();
        if (!result->return_type) return nullptr;
    } else {
        // Default void return
        result->return_type = std::make_shared<TypeExpr>();
        result->return_type->kind = TypeKind::Void;
    }

    return result;
}

// --- parse_type ---

TypePtr parse_type(const std::string& s, std::string& error) {
    TypeParser p(s);
    auto result = p.parse();
    if (!result || !p.error.empty()) {
        error = p.error.empty() ? "Failed to parse type" : p.error;
        return nullptr;
    }
    // Check for trailing garbage
    p.skip_ws();
    if (!p.eof()) {
        error = "Unexpected trailing text: " + s.substr(p.pos);
        return nullptr;
    }
    return result;
}

// --- TypeRegistry method bodies ---

void TypeRegistry::clear() { type_defs.clear(); parsed.clear(); errors.clear(); }

void TypeRegistry::register_type(const std::string& name, const std::string& def) {
    type_defs[name] = def;
}

void TypeRegistry::resolve_all() {
    parsed.clear();
    errors.clear();
    for (auto& [name, def] : type_defs) {
        std::set<std::string> visited;
        resolve(name, visited);
    }
}

bool TypeRegistry::validate_type(const std::string& type_str, std::string& error) {
    auto t = parse_type(type_str, error);
    if (!t) return false;
    std::set<std::string> visited;
    return check_refs(t, visited, error);
}

TypePtr TypeRegistry::resolve(const std::string& name, std::set<std::string>& visited) {
    // Already resolved
    auto it = parsed.find(name);
    if (it != parsed.end()) return it->second;

    // Check for cycle
    if (visited.count(name)) {
        errors[name] = "Circular type reference: " + name;
        return nullptr;
    }
    visited.insert(name);

    // Find definition
    auto def_it = type_defs.find(name);
    if (def_it == type_defs.end()) {
        errors[name] = "Unknown type: " + name;
        return nullptr;
    }

    // Parse the definition
    std::string err;
    auto t = parse_type(def_it->second, err);
    if (!t) {
        errors[name] = "Parse error in type " + name + ": " + err;
        return nullptr;
    }

    // Recursively resolve named references
    if (!check_refs(t, visited, err)) {
        errors[name] = err;
        return nullptr;
    }

    parsed[name] = t;
    visited.erase(name);
    return t;
}

bool TypeRegistry::check_refs(const TypePtr& t, std::set<std::string>& visited, std::string& error) {
    if (!t) return true;
    switch (t->kind) {
    case TypeKind::Named:
        if (type_defs.count(t->named_ref) == 0) {
            error = "Unknown type: " + t->named_ref;
            return false;
        }
        if (visited.count(t->named_ref)) {
            error = "Circular type reference: " + t->named_ref;
            return false;
        }
        {
            auto resolved = resolve(t->named_ref, visited);
            if (!resolved) {
                error = errors.count(t->named_ref) ? errors[t->named_ref] : "Failed to resolve " + t->named_ref;
                return false;
            }
        }
        return true;
    case TypeKind::Container:
    case TypeKind::ContainerIterator:
        if (t->key_type && !check_refs(t->key_type, visited, error)) return false;
        if (t->value_type && !check_refs(t->value_type, visited, error)) return false;
        return true;
    case TypeKind::Array:
    case TypeKind::Tensor:
        return t->value_type ? check_refs(t->value_type, visited, error) : true;
    case TypeKind::Function:
        for (auto& arg : t->func_args)
            if (!check_refs(arg.type, visited, error)) return false;
        return t->return_type ? check_refs(t->return_type, visited, error) : true;
    case TypeKind::Struct:
        for (auto& f : t->fields)
            if (!check_refs(f.type, visited, error)) return false;
        return true;
    case TypeKind::MetaType:
        return t->wrapped_type ? check_refs(t->wrapped_type, visited, error) : true;
    default:
        return true;
    }
}

// --- type_to_string ---

std::string type_to_string(const TypePtr& t) {
    if (!t) return "?";
    std::string prefix;
    switch (t->category) {
    case TypeCategory::Reference: prefix = "&"; break;
    case TypeCategory::Iterator:  prefix = "^"; break;
    case TypeCategory::Lambda:    prefix = "@"; break;
    case TypeCategory::Enum:      prefix = "#"; break;
    case TypeCategory::Bang:      prefix = "!"; break;
    case TypeCategory::Event:     prefix = "~"; break;
    default: break;
    }
    if (t->is_generic) {
        if (t->kind == TypeKind::Scalar) {
            std::string domain = (t->scalar == ScalarType::F64 || t->scalar == ScalarType::F32)
                ? "float<?>" : "unsigned<?>";
            if (!t->literal_value.empty())
                return prefix + "literal<" + domain + ", " + t->literal_value + ">";
            return prefix + "literal<" + domain + ">";
        }
        return prefix + "?";
    }
    switch (t->kind) {
    case TypeKind::Void: return prefix + "void";
    case TypeKind::Bool: return prefix + "bool";
    case TypeKind::String: return prefix + "string";
    case TypeKind::Mutex: return prefix + "mutex";
    case TypeKind::Scalar: {
        static const char* names[] = {"u8","s8","u16","s16","u32","s32","u64","s64","f32","f64"};
        return prefix + names[(int)t->scalar];
    }
    case TypeKind::Named: return prefix + t->named_ref;
    case TypeKind::Container: {
        static const char* cnames[] = {"map","ordered_map","set","ordered_set","list","queue","vector"};
        std::string s = prefix + cnames[(int)t->container] + "<";
        if (t->key_type) s += type_to_string(t->key_type) + ", ";
        s += type_to_string(t->value_type) + ">";
        return s;
    }
    case TypeKind::ContainerIterator: {
        static const char* inames[] = {"map_iterator","ordered_map_iterator","set_iterator","ordered_set_iterator","list_iterator","vector_iterator"};
        std::string s = prefix + inames[(int)t->iterator] + "<";
        if (t->key_type) s += type_to_string(t->key_type) + ", ";
        s += type_to_string(t->value_type) + ">";
        return s;
    }
    case TypeKind::Array: {
        std::string s = prefix + "array<" + type_to_string(t->value_type);
        for (int d : t->dimensions) s += ", " + std::to_string(d);
        s += ">";
        return s;
    }
    case TypeKind::Tensor:
        return prefix + "tensor<" + type_to_string(t->value_type) + ">";
    case TypeKind::Function: {
        std::string s = prefix + "(";
        for (size_t i = 0; i < t->func_args.size(); i++) {
            if (i > 0) s += " ";
            s += t->func_args[i].name + ":" + type_to_string(t->func_args[i].type);
        }
        s += ") -> " + type_to_string(t->return_type);
        return s;
    }
    case TypeKind::Struct: {
        std::string s = prefix + "{";
        for (size_t i = 0; i < t->fields.size(); i++) {
            if (i > 0) s += " ";
            s += t->fields[i].name + ":" + type_to_string(t->fields[i].type);
        }
        s += "}";
        return s;
    }
    case TypeKind::Symbol:
        return prefix + "symbol<" + t->symbol_name + ">";
    case TypeKind::UndefinedSymbol:
        return prefix + "undefined_symbol<" + t->symbol_name + ">";
    case TypeKind::MetaType:
        return prefix + "type<" + type_to_string(t->wrapped_type) + ">";
    }
    return "?";
}

// --- types_compatible ---

bool types_compatible(const TypePtr& from, const TypePtr& to) {
    if (!from || !to) return true;
    if (from.get() == to.get()) return true;
    if (from->is_generic || to->is_generic) return true;
    // Named types: compatible with same name, or with any type (since we can't
    // resolve without registry — the inference engine validates the actual match)
    if (from->kind == TypeKind::Named || to->kind == TypeKind::Named) {
        if (from->kind == TypeKind::Named && to->kind == TypeKind::Named)
            return from->named_ref == to->named_ref;
        // One is Named, other is concrete — consider compatible (inference handles validation)
        return true;
    }
    // Iterator-to-reference decay: ^iterator<T> is compatible with &T or T
    // (iterators auto-decay to references when passed to functions)
    if (from->kind == TypeKind::ContainerIterator &&
        to->kind != TypeKind::ContainerIterator) {
        // Compare iterator's value_type against the target (ignoring category)
        if (from->value_type) {
            // Make a copy of 'to' without reference category for comparison
            auto to_data = std::make_shared<TypeExpr>(*to);
            to_data->category = TypeCategory::Data;
            auto from_val = std::make_shared<TypeExpr>(*from->value_type);
            from_val->category = TypeCategory::Data;
            return types_compatible(from_val, to_data);
        }
    }
    if (from->kind != to->kind) return false;
    switch (from->kind) {
    case TypeKind::Void:
    case TypeKind::Bool:
    case TypeKind::String:
    case TypeKind::Mutex:
        return true;
    case TypeKind::Scalar:
        return from->scalar == to->scalar || can_upcast(from->scalar, to->scalar);
    case TypeKind::Named:
        return from->named_ref == to->named_ref;
    case TypeKind::Container:
        if (from->container != to->container) return false;
        if (from->key_type && to->key_type && !types_compatible(from->key_type, to->key_type)) return false;
        return types_compatible(from->value_type, to->value_type);
    case TypeKind::ContainerIterator:
        if (from->iterator != to->iterator) return false;
        if (from->key_type && to->key_type && !types_compatible(from->key_type, to->key_type)) return false;
        return types_compatible(from->value_type, to->value_type);
    case TypeKind::Array:
        if (from->dimensions != to->dimensions) return false;
        return types_compatible(from->value_type, to->value_type);
    case TypeKind::Tensor:
        return types_compatible(from->value_type, to->value_type);
    case TypeKind::Function:
        if (from->func_args.size() != to->func_args.size()) return false;
        for (size_t i = 0; i < from->func_args.size(); i++)
            if (!types_compatible(from->func_args[i].type, to->func_args[i].type)) return false;
        return types_compatible(from->return_type, to->return_type);
    case TypeKind::Struct:
        if (from->fields.size() != to->fields.size()) return false;
        for (size_t i = 0; i < from->fields.size(); i++) {
            if (from->fields[i].name != to->fields[i].name) return false;
            if (!types_compatible(from->fields[i].type, to->fields[i].type)) return false;
        }
        return true;
    case TypeKind::Symbol:
    case TypeKind::UndefinedSymbol:
        return from->kind == to->kind && from->symbol_name == to->symbol_name;
    case TypeKind::MetaType:
        return types_compatible(from->wrapped_type, to->wrapped_type);
    }
    return false;
}

// --- TypePool constructor ---

TypePool::TypePool() {
    auto mk = [](TypeKind k) { auto t = std::make_shared<TypeExpr>(); t->kind = k; return t; };
    auto mk_scalar = [](ScalarType s) {
        auto t = std::make_shared<TypeExpr>();
        t->kind = TypeKind::Scalar; t->scalar = s;
        return t;
    };

    t_void = mk(TypeKind::Void);
    t_bool = mk(TypeKind::Bool);
    t_string = mk(TypeKind::String);
    t_mutex = mk(TypeKind::Mutex);
    t_u8  = mk_scalar(ScalarType::U8);  t_s8  = mk_scalar(ScalarType::S8);
    t_u16 = mk_scalar(ScalarType::U16); t_s16 = mk_scalar(ScalarType::S16);
    t_u32 = mk_scalar(ScalarType::U32); t_s32 = mk_scalar(ScalarType::S32);
    t_u64 = mk_scalar(ScalarType::U64); t_s64 = mk_scalar(ScalarType::S64);
    t_f32 = mk_scalar(ScalarType::F32); t_f64 = mk_scalar(ScalarType::F64);

    t_int_literal = std::make_shared<TypeExpr>();
    t_int_literal->kind = TypeKind::Scalar; t_int_literal->is_generic = true;
    t_float_literal = std::make_shared<TypeExpr>();
    t_float_literal->kind = TypeKind::Scalar; t_float_literal->scalar = ScalarType::F64; t_float_literal->is_generic = true;
    t_unknown = std::make_shared<TypeExpr>();
    t_unknown->kind = TypeKind::Void; t_unknown->is_generic = true;
    t_bang = std::make_shared<TypeExpr>();
    t_bang->kind = TypeKind::Function;
    t_bang->return_type = t_void;

    t_symbol = mk(TypeKind::Symbol);
    t_undefined_symbol = mk(TypeKind::UndefinedSymbol);

    cache["void"] = t_void; cache["bool"] = t_bool; cache["string"] = t_string; cache["mutex"] = t_mutex;
    cache["u8"] = t_u8; cache["s8"] = t_s8;
    cache["u16"] = t_u16; cache["s16"] = t_s16;
    cache["u32"] = t_u32; cache["s32"] = t_s32;
    cache["u64"] = t_u64; cache["s64"] = t_s64;
    cache["f32"] = t_f32; cache["f64"] = t_f64;
}
