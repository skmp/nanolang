#include "codegen.h"
#include "args.h"
#include "node_types.h"
#include "type_utils.h"
#include <filesystem>
#include <algorithm>

void CodeGenerator::collect_lambda_pins(FlowNode& node, std::vector<FlowPin*>& params,
                                        std::set<std::string>& visited) {
    if (visited.count(node.guid)) return;
    visited.insert(node.guid);
    // Only follow DATA connections (not bang chains) to find lambda parameters.
    // Bang chains are handled separately by emit_node.
    for (auto& inp : node.inputs) {
        // Skip Lambda inputs — they define inner lambda boundaries
        if (inp.direction == FlowPin::Lambda) continue;
        std::string source = find_source_pin(inp.id);
        if (source.empty()) {
            params.push_back(&inp);
        } else {
            // Don't recurse through as_lambda (LambdaGrab) pins — they are lambda boundaries
            auto* src_node = find_source_node(inp.id);
            if (src_node) {
                bool is_lambda_grab = (source == src_node->lambda_grab.id);
                if (!is_lambda_grab)
                    collect_lambda_pins(*src_node, params, visited);
            }
        }
    }
}

void CodeGenerator::collect_stored_lambda_params(FlowNode& root,
                                                   std::vector<LambdaParamInfo>& params,
                                                   std::set<std::string>& visited) {
    // Traverse data + bang chains, collecting unconnected input pins grouped by index
    std::map<int, LambdaParamInfo> param_map;

    std::function<void(FlowNode&)> collect = [&](FlowNode& node) {
        if (visited.count(node.guid)) return;
        visited.insert(node.guid);

        for (auto& inp : node.inputs) {
            if (inp.direction == FlowPin::Lambda) continue;
            std::string src = find_source_pin(inp.id);
            if (src.empty()) {
                // Unconnected pin — determine parameter index from pin name
                int idx = -1;
                try { idx = std::stoi(inp.name); } catch (...) {}
                // Also accept "argN" names (from lock forwarded params)
                if (idx < 0 && inp.name.substr(0, 3) == "arg") {
                    try { idx = std::stoi(inp.name.substr(3)); } catch (...) {}
                }
                if (idx >= 0) {
                    param_map[idx].index = idx;
                    param_map[idx].pins.push_back(&inp);
                    param_map[idx].nodes.push_back(&node);
                }
            } else if (src.find(".as_lambda") == std::string::npos) {
                auto* src_node = find_source_node(inp.id);
                if (src_node) collect(*src_node);
            }
        }
        // Follow bang chains
        auto bt = follow_bang_from(node.bang_pin.id);
        for (auto* t : bt) collect(*t);
        for (auto& bout : node.bang_outputs) {
            auto bt2 = follow_bang_from(bout.id);
            for (auto* t : bt2) collect(*t);
        }
    };
    collect(root);

    // Sort by index and return
    params.clear();
    for (auto& [idx, info] : param_map)
        params.push_back(std::move(info));
}

LambdaScope CodeGenerator::enter_lambda_scope() {
    LambdaScope scope;
    scope.saved_pin_to_value = pin_to_value;
    scope.saved_materialized = materialized;
    scope.saved_emitted_bang_nodes = emitted_bang_nodes_;
    // Clear per-lambda state but keep pin_to_value (globals are still accessible via [&])
    materialized.clear();
    emitted_bang_nodes_.clear();
    return scope;
}

void CodeGenerator::exit_lambda_scope(LambdaScope& scope) {
    pin_to_value = std::move(scope.saved_pin_to_value);
    materialized = std::move(scope.saved_materialized);
    emitted_bang_nodes_ = std::move(scope.saved_emitted_bang_nodes);
}

void CodeGenerator::emit_stored_lambda(FlowNode& store_node, FlowNode& lambda_root,
                                        const std::string& target, std::ostringstream& out, int indent) {
    std::string ind = indent_str(indent);

    // Determine function type from the target variable's type
    TypePtr fn_type;
    if (!store_node.parsed_exprs.empty() && store_node.parsed_exprs[0]) {
        fn_type = store_node.parsed_exprs[0]->resolved_type;
        // Resolve named type aliases
        while (fn_type && fn_type->kind == TypeKind::Named) {
            auto it = pool.cache.find(fn_type->named_ref);
            if (it != pool.cache.end() && it->second.get() != fn_type.get())
                fn_type = it->second;
            else break;
        }
    }
    if (!fn_type || fn_type->kind != TypeKind::Function) {
        out << ind << "// WARNING: could not determine function type for stored lambda\n";
        return;
    }

    // Collect grouped lambda parameters
    std::set<std::string> lam_visited;
    std::vector<LambdaParamInfo> grouped_params;
    collect_stored_lambda_params(lambda_root, grouped_params, lam_visited);

    // Enter lambda scope
    auto scope = enter_lambda_scope();

    // Emit lambda signature
    std::string ret = fn_type->return_type ? type_to_cpp(fn_type->return_type) : "void";
    out << ind << target << " = [&](";
    for (size_t i = 0; i < fn_type->func_args.size(); i++) {
        if (i > 0) out << ", ";
        std::string ptype = fn_type->func_args[i].type ? type_to_cpp(fn_type->func_args[i].type) : "auto";
        out << ptype << " " << fn_type->func_args[i].name;
    }
    out << ") -> " << ret << " {\n";

    // Register all parameter pins with their function arg names
    for (auto& param_info : grouped_params) {
        if (param_info.index >= (int)fn_type->func_args.size()) continue;
        std::string param_name = fn_type->func_args[param_info.index].name;

        for (size_t j = 0; j < param_info.pins.size(); j++) {
            FlowPin* pp = param_info.pins[j];
            FlowNode* pnode = param_info.nodes[j];

            pin_to_value[pp->id] = param_name;

            // Map input[k] -> output[k] for the owning node
            for (int oi = 0; oi < (int)pnode->inputs.size(); oi++) {
                if (pnode->inputs[oi].id == pp->id && oi < (int)pnode->outputs.size()) {
                    pin_to_value[pnode->outputs[oi].id] = param_name;
                }
            }
        }
    }

    // Mark fully-registered parameter nodes as materialized
    // (but never the lambda root itself — it needs proper materialization)
    std::set<std::string> param_node_guids;
    for (auto& pi : grouped_params)
        for (auto* n : pi.nodes)
            if (n->guid != lambda_root.guid)
                param_node_guids.insert(n->guid);
    for (auto& guid : param_node_guids) {
        auto* pnode = find_node_by_guid(guid);
        if (!pnode) continue;
        bool all_registered = true;
        for (auto& o : pnode->outputs) {
            if (pin_to_value.find(o.id) == pin_to_value.end()) {
                all_registered = false;
                break;
            }
        }
        if (all_registered) materialized.insert(guid);
    }

    // Materialize lambda body (the lambda root node)
    std::string result = materialize_node(lambda_root, out, indent + 1);

    // Emit bang chains from all visited nodes in the lambda subgraph
    for (auto& guid : lam_visited) {
        auto* n = find_node_by_guid(guid);
        if (!n) continue;
        auto bt = follow_bang_from(n->bang_pin.id);
        for (auto* t : bt)
            if (!emitted_bang_nodes_.count(t->guid))
                emit_node(*t, out, indent + 1);
    }

    // Return value if non-void
    if (fn_type->return_type && fn_type->return_type->kind != TypeKind::Void) {
        out << indent_str(indent + 1) << "return " << result << ";\n";
    }

    out << ind << "};\n";

    // Exit lambda scope
    exit_lambda_scope(scope);
}

std::string CodeGenerator::indent_str(int level) {
    return std::string(level * 4, ' ');
}

std::string CodeGenerator::fresh_var(const std::string& prefix) {
    return prefix + "_" + std::to_string(temp_counter++);
}

// --- Type conversion ---

std::string CodeGenerator::type_to_cpp(const TypePtr& t) {
    if (!t) return "auto";
    if (t->is_generic) return "auto";

    // Handle reference category
    if (t->category == TypeCategory::Reference) {
        auto copy = std::make_shared<TypeExpr>(*t);
        copy->category = TypeCategory::Data;
        return type_to_cpp(copy) + "&";
    }

    switch (t->kind) {
    case TypeKind::Void: return "void";
    case TypeKind::Bool: return "bool";
    case TypeKind::String: return "std::string";
    case TypeKind::Mutex: return "std::mutex";
    case TypeKind::Scalar: {
        static const char* names[] = {"u8","s8","u16","s16","u32","s32","u64","s64","f32","f64"};
        return names[(int)t->scalar];
    }
    case TypeKind::Named: return t->named_ref;
    case TypeKind::Container: {
        std::string vt = type_to_cpp(t->value_type);
        switch (t->container) {
        case ContainerKind::Vector: return "std::vector<" + vt + ">";
        case ContainerKind::List: return "std::list<" + vt + ">";
        case ContainerKind::Queue: return "std::queue<" + vt + ">";
        case ContainerKind::Map: return "std::unordered_map<" + type_to_cpp(t->key_type) + ", " + vt + ">";
        case ContainerKind::OrderedMap: return "std::map<" + type_to_cpp(t->key_type) + ", " + vt + ">";
        case ContainerKind::Set: return "std::unordered_set<" + vt + ">";
        case ContainerKind::OrderedSet: return "std::set<" + vt + ">";
        }
        break;
    }
    case TypeKind::ContainerIterator: {
        std::string vt = type_to_cpp(t->value_type);
        switch (t->iterator) {
        case IteratorKind::Vector: return "std::vector<" + vt + ">::iterator";
        case IteratorKind::List: return "std::list<" + vt + ">::iterator";
        case IteratorKind::Map: return "std::unordered_map<" + type_to_cpp(t->key_type) + ", " + vt + ">::iterator";
        case IteratorKind::OrderedMap: return "std::map<" + type_to_cpp(t->key_type) + ", " + vt + ">::iterator";
        case IteratorKind::Set: return "std::unordered_set<" + vt + ">::iterator";
        case IteratorKind::OrderedSet: return "std::set<" + vt + ">::iterator";
        }
        break;
    }
    case TypeKind::Array: {
        std::string s = "std::array<" + type_to_cpp(t->value_type);
        for (int d : t->dimensions) s += ", " + std::to_string(d);
        return s + ">";
    }
    case TypeKind::Tensor: return "std::vector<" + type_to_cpp(t->value_type) + ">";
    case TypeKind::Function: {
        std::string s = "std::function<" + type_to_cpp(t->return_type) + "(";
        for (size_t i = 0; i < t->func_args.size(); i++) {
            if (i > 0) s += ", ";
            s += type_to_cpp(t->func_args[i].type);
        }
        return s + ")>";
    }
    case TypeKind::Struct: return "auto";
    }
    return "auto";
}

std::string CodeGenerator::type_to_cpp_str(const std::string& type_str) {
    return type_to_cpp(pool.intern(type_str));
}

// --- Expression codegen ---

std::string CodeGenerator::expr_to_cpp(const ExprPtr& e, FlowNode* ctx_node) {
    if (!e) throw std::runtime_error("codegen: null expression");
    switch (e->kind) {
    case ExprKind::IntLiteral: {
        // If the resolved type is f32/f64, emit as float to avoid narrowing warnings
        if (e->resolved_type && e->resolved_type->kind == TypeKind::Scalar) {
            if (e->resolved_type->scalar == ScalarType::F32)
                return std::to_string((float)e->int_value) + "f";
            if (e->resolved_type->scalar == ScalarType::F64)
                return std::to_string((double)e->int_value);
        }
        return std::to_string(e->int_value);
    }
    case ExprKind::F32Literal: return std::to_string(e->float_value) + "f";
    case ExprKind::F64Literal: return std::to_string(e->float_value);
    case ExprKind::BoolLiteral: return e->bool_value ? "true" : "false";
    case ExprKind::StringLiteral: return "\"" + e->string_value + "\"";
    case ExprKind::PinRef: {
        if (ctx_node) return resolve_pin_value(*ctx_node, e->pin_ref.index);
        throw std::runtime_error("codegen: PinRef without node context");
    }
    case ExprKind::VarRef: {
        if (e->is_dollar_var) return e->var_name;
        if (e->var_name == "pi") return "nano_pi";
        if (e->var_name == "e") return "nano_e";
        if (e->var_name == "tau") return "nano_tau";
        return e->var_name;
    }
    case ExprKind::UnaryMinus:
        return "-(" + expr_to_cpp(e->children[0], ctx_node) + ")";
    case ExprKind::BinaryOp: {
        static const char* ops[] = {"+","-","*","/","==","!=","<",">","<=",">=","<=>"};
        return "(" + expr_to_cpp(e->children[0], ctx_node) + " " + ops[(int)e->bin_op] + " " + expr_to_cpp(e->children[1], ctx_node) + ")";
    }
    case ExprKind::FieldAccess: {
        std::string obj = expr_to_cpp(e->children[0], ctx_node);
        // If the object is an iterator type, use -> for auto-deref
        auto obj_type = e->children[0]->resolved_type;
        if (obj_type && obj_type->kind == TypeKind::ContainerIterator)
            return obj + "->" + e->field_name;
        return obj + "." + e->field_name;
    }
    case ExprKind::Index:
        return expr_to_cpp(e->children[0], ctx_node) + "[" + expr_to_cpp(e->children[1], ctx_node) + "]";
    case ExprKind::QueryIndex:
        return "(" + expr_to_cpp(e->children[0], ctx_node) + ".count(" + expr_to_cpp(e->children[1], ctx_node) + ") > 0)";
    case ExprKind::Slice:
        throw std::runtime_error("codegen: slice not yet supported");
    case ExprKind::FuncCall: {
        std::string callee;
        size_t arg_start = 1;
        bool is_method_call = false;
        std::string method_self;
        TypePtr callee_resolved;
        if (e->builtin != BuiltinFunc::None) {
            switch (e->builtin) {
            case BuiltinFunc::Sin: callee = "std::sin"; break;
            case BuiltinFunc::Cos: callee = "std::cos"; break;
            case BuiltinFunc::Pow: callee = "std::pow"; break;
            case BuiltinFunc::Exp: callee = "std::exp"; break;
            case BuiltinFunc::Log: callee = "std::log"; break;
            case BuiltinFunc::Or:
                return "(" + expr_to_cpp(e->children[1], ctx_node) + " | " + expr_to_cpp(e->children[2], ctx_node) + ")";
            case BuiltinFunc::Xor:
                return "(" + expr_to_cpp(e->children[1], ctx_node) + " ^ " + expr_to_cpp(e->children[2], ctx_node) + ")";
            case BuiltinFunc::And:
                return "(" + expr_to_cpp(e->children[1], ctx_node) + " & " + expr_to_cpp(e->children[2], ctx_node) + ")";
            case BuiltinFunc::Not:
                return "(~" + expr_to_cpp(e->children[1], ctx_node) + ")";
            case BuiltinFunc::Mod:
                return "(" + expr_to_cpp(e->children[1], ctx_node) + " % " + expr_to_cpp(e->children[2], ctx_node) + ")";
            case BuiltinFunc::Rand: {
                auto a = expr_to_cpp(e->children[1], ctx_node);
                auto b = expr_to_cpp(e->children[2], ctx_node);
                // Determine if float or int based on resolved type
                bool is_float = e->resolved_type && e->resolved_type->kind == TypeKind::Scalar &&
                    (e->resolved_type->scalar == ScalarType::F32 || e->resolved_type->scalar == ScalarType::F64);
                if (is_float) {
                    return "nano_rand_float(" + a + ", " + b + ")";
                } else {
                    return "nano_rand_int(" + a + ", " + b + ")";
                }
            }
            default: throw std::runtime_error("codegen: unknown builtin function"); break;
            }
        } else {
            // Check for method-call pattern: expr.field() where field is a function
            // If the callee is a FieldAccess and the field resolves to a function type,
            // pass the object as implicit first argument
            auto& callee_expr = e->children[0];
            if (callee_expr && callee_expr->kind == ExprKind::FieldAccess) {
                auto obj_type = callee_expr->children[0]->resolved_type;
                if (obj_type && (obj_type->kind == TypeKind::ContainerIterator ||
                    obj_type->kind == TypeKind::Named || obj_type->kind == TypeKind::Struct)) {
                    // The field is accessed on a struct/iterator — check if the field type is a function
                    auto field_type = callee_expr->resolved_type;
                    if (field_type && field_type->kind == TypeKind::Function) {
                        is_method_call = true;
                        method_self = expr_to_cpp(callee_expr->children[0], ctx_node);
                    }
                }
            }
            callee = expr_to_cpp(callee_expr, ctx_node);
            callee_resolved = callee_expr->resolved_type;
        }
        // Get resolved function type for argument deref
        auto fn_resolved = callee_resolved;
        if (!fn_resolved && e->children[0]->resolved_type)
            fn_resolved = e->children[0]->resolved_type;

        // Helper: dereference an arg if it's an iterator but the param expects a value/ref
        auto maybe_deref_arg = [&](const std::string& arg_code, const ExprPtr& arg_expr, size_t param_idx) -> std::string {
            if (!arg_expr || !arg_expr->resolved_type) return arg_code;
            if (arg_expr->resolved_type->kind != TypeKind::ContainerIterator) return arg_code;
            // Iterator arg — check if param expects a non-iterator type
            if (fn_resolved && fn_resolved->kind == TypeKind::Function &&
                param_idx < fn_resolved->func_args.size()) {
                auto param_t = fn_resolved->func_args[param_idx].type;
                if (param_t && param_t->kind != TypeKind::ContainerIterator)
                    return "(*" + arg_code + ")";
            }
            return "(*" + arg_code + ")"; // default: deref iterators
        };

        std::string s = callee + "(";
        size_t param_offset = 0;
        if (is_method_call) {
            s += maybe_deref_arg(method_self, e->children[0]->children[0], 0);
            param_offset = 1;
            if (e->children.size() > arg_start) s += ", ";
        }
        for (size_t i = arg_start; i < e->children.size(); i++) {
            if (i > arg_start) s += ", ";
            std::string arg_code = expr_to_cpp(e->children[i], ctx_node);
            s += maybe_deref_arg(arg_code, e->children[i], (i - arg_start) + param_offset);
        }
        return s + ")";
    }
    case ExprKind::Ref:
        return "&(" + expr_to_cpp(e->children[0], ctx_node) + ")";
    }
    throw std::runtime_error("codegen: unknown expression kind");
}

// --- Pin value resolution ---

std::string CodeGenerator::resolve_pin_value(FlowNode& node, int pin_index) {
    // Find the input pin
    if (pin_index >= (int)node.inputs.size())
        throw std::runtime_error("codegen: pin index " + std::to_string(pin_index) + " out of range for node " + node.guid);

    auto& pin = node.inputs[pin_index];

    // Check if the pin itself is registered (lambda parameter or pre-set value)
    auto self_it = pin_to_value.find(pin.id);
    if (self_it != pin_to_value.end()) return self_it->second;

    // Check if connected
    std::string source_pin = find_source_pin(pin.id);
    if (source_pin.empty()) {
        throw std::runtime_error("codegen: unconnected pin " + pin.id + " on node " + std::string(node_type_str(node.type_id)) + " [" + node.guid.substr(0,8) + "]");
    }

    // Check if we already know the value for this source pin
    auto it = pin_to_value.find(source_pin);
    if (it != pin_to_value.end()) return it->second;

    // Find the source node
    auto* source_node = find_source_node(pin.id);
    if (!source_node)
        throw std::runtime_error("codegen: cannot find source node for " + pin.id);

    // Determine the pin name from the source pin ID
    auto dot = source_pin.find('.');
    std::string pin_name = (dot != std::string::npos) ? source_pin.substr(dot + 1) : source_pin;

    // If source is an event output, use the parameter name directly
    if (source_node->type_id == NodeTypeID::EventBang) {
        pin_to_value[source_pin] = pin_name;
        return pin_name;
    }

    // Try on-demand materialization
    if (!materialized.count(source_node->guid) && current_out_) {
        materialize_node(*source_node, *current_out_, current_indent_);
        auto it2 = pin_to_value.find(source_pin);
        if (it2 != pin_to_value.end()) return it2->second;
    }

    throw std::runtime_error("codegen: cannot resolve pin value from " + std::string(node_type_str(source_node->type_id)) +
                           " [" + source_node->guid.substr(0, 8) + "] pin " + pin_name +
                           " (not materialized — needs pre-materialization in calling context)");
}

std::string CodeGenerator::resolve_inline_arg(FlowNode& node, int arg_index) {
    // If inline expression exists, use it
    if (!node.parsed_exprs.empty() && arg_index < (int)node.parsed_exprs.size() && node.parsed_exprs[arg_index])
        return expr_to_cpp(node.parsed_exprs[arg_index], &node);

    // Fall back to connected pin value.
    // The pin index depends on how many inline args filled descriptor slots.
    // Inline args fill descriptor inputs left-to-right.
    // Remaining descriptor inputs become pins, indexed after any $N ref pins.
    auto* nt = find_node_type(node.type_id);
    int descriptor_inputs = nt ? nt->inputs : 0;
    auto info = compute_inline_args(node.args, descriptor_inputs);
    int ref_pins = (info.pin_slots.max_slot >= 0) ? (info.pin_slots.max_slot + 1) : 0;

    // arg_index maps to descriptor input `arg_index`.
    // If arg_index >= num_inline_args, it's a remaining descriptor pin.
    // Remaining pins start after ref_pins in node.inputs.
    int remaining_pin_offset = arg_index - info.num_inline_args;
    int pin_index = ref_pins + remaining_pin_offset;

    if (pin_index >= 0 && pin_index < (int)node.inputs.size()) {
        std::string src = find_source_pin(node.inputs[pin_index].id);
        if (!src.empty()) {
            // Try to materialize the source node if not yet done
            auto* src_node = find_source_node(node.inputs[pin_index].id);
            if (src_node && !materialized.count(src_node->guid) && src_node->type_id != NodeTypeID::EventBang && current_out_) {
                materialize_node(*src_node, *current_out_, current_indent_);
            }
            auto it = pin_to_value.find(src);
            if (it != pin_to_value.end()) return it->second;
        }
    }

    // Also try searching all input pins by name matching the descriptor
    if (nt && nt->input_ports && arg_index < descriptor_inputs) {
        std::string expected_name = nt->input_ports[arg_index].name;
        for (auto& inp : node.inputs) {
            if (inp.name == expected_name) {
                std::string src = find_source_pin(inp.id);
                if (!src.empty()) {
                    auto* src_node = find_source_node(inp.id);
                    if (src_node && !materialized.count(src_node->guid) && src_node->type_id != NodeTypeID::EventBang && current_out_) {
                        materialize_node(*src_node, *current_out_, current_indent_);
                    }
                    auto it = pin_to_value.find(src);
                    if (it != pin_to_value.end()) return it->second;
                }
            }
        }
    }

    throw std::runtime_error("codegen: missing arg " + std::to_string(arg_index) + " on node " + std::string(node_type_str(node.type_id)) + " [" + node.guid.substr(0, 8) + "]");
}

// --- Materialize a data-producing node as a local variable ---

std::string CodeGenerator::materialize_node(FlowNode& node, std::ostringstream& out, int indent) {
    current_out_ = &out;
    current_indent_ = indent;
    // Already materialized?
    if (materialized.count(node.guid)) {
        // Find the var name we assigned
        for (auto& [pin_id, val] : pin_to_value)
            if (pin_id.find(node.guid) == 0) return val;
        throw std::runtime_error("codegen: node " + node.guid + " materialized but no var found");
    }
    materialized.insert(node.guid);

    std::string ind = indent_str(indent);

    if (is_any_of(node.type_id, NodeTypeID::Expr, NodeTypeID::ExprBang)) {
        // Materialize each output as a local variable
        // First materialize any data dependencies
        for (auto& inp : node.inputs) {
            std::string src = find_source_pin(inp.id);
            if (!src.empty()) {
                auto* src_node = find_source_node(inp.id);
                if (src_node && !materialized.count(src_node->guid) &&
                    !is_any_of(src_node->type_id, NodeTypeID::EventBang, NodeTypeID::Dup, NodeTypeID::Next)) {
                    materialize_node(*src_node, out, indent);
                }
            }
        }

        for (int i = 0; i < (int)node.parsed_exprs.size() && i < (int)node.outputs.size(); i++) {
            if (!node.parsed_exprs[i]) continue;
            std::string expr = expr_to_cpp(node.parsed_exprs[i], &node);
            bool is_void_expr = node.outputs[i].resolved_type &&
                                node.outputs[i].resolved_type->kind == TypeKind::Void;
            if (is_void_expr) {
                // Void expression — emit as statement, no variable
                out << ind << expr << ";\n";
                pin_to_value[node.outputs[i].id] = "void()";
            } else {
                std::string var = fresh_var("val");
                std::string type_str = node.outputs[i].resolved_type ? type_to_cpp(node.outputs[i].resolved_type) : "auto";
                out << ind << type_str << " " << var << " = " << expr << ";\n";
                pin_to_value[node.outputs[i].id] = var;
            }
        }

        // Emit post_bang chain (side effects triggered after materialization)
        auto bang_targets = follow_bang_from(node.bang_pin.id);
        for (auto* bt : bang_targets)
            emit_node(*bt, out, indent);

        return pin_to_value.count(node.outputs[0].id) ? pin_to_value[node.outputs[0].id] : "/* no output */";
    }

    if (node.type_id == NodeTypeID::Dup) {
        // Dup: output = input, just alias
        if (!node.inputs.empty()) {
            // Check if the input pin itself is already registered (lambda parameter)
            auto pin_it = pin_to_value.find(node.inputs[0].id);
            if (pin_it != pin_to_value.end()) {
                for (auto& o : node.outputs)
                    pin_to_value[o.id] = pin_it->second;
                return pin_it->second;
            }
            // Check if connected
            std::string src = find_source_pin(node.inputs[0].id);
            if (!src.empty()) {
                auto* src_node = find_source_node(node.inputs[0].id);
                if (src_node && !materialized.count(src_node->guid) && src_node->type_id != NodeTypeID::EventBang) {
                    materialize_node(*src_node, out, indent);
                }
                auto it = pin_to_value.find(src);
                if (it != pin_to_value.end()) {
                    for (auto& o : node.outputs)
                        pin_to_value[o.id] = it->second;
                    return it->second;
                }
            }
        }
        throw std::runtime_error("codegen: dup node " + node.guid + " has no connected input or registered value");
    }

    if (node.type_id == NodeTypeID::Str) {
        // Materialize input, wrap in std::to_string
        std::string input_val;
        if (!node.inputs.empty()) {
            auto pin_it = pin_to_value.find(node.inputs[0].id);
            if (pin_it != pin_to_value.end()) {
                input_val = pin_it->second;
            } else {
                std::string src = find_source_pin(node.inputs[0].id);
                if (!src.empty()) {
                    auto* src_node = find_source_node(node.inputs[0].id);
                    if (src_node && !materialized.count(src_node->guid) && src_node->type_id != NodeTypeID::EventBang)
                        materialize_node(*src_node, out, indent);
                    auto it = pin_to_value.find(src);
                    if (it != pin_to_value.end()) input_val = it->second;
                }
            }
        }
        if (input_val.empty())
            throw std::runtime_error("codegen: str node " + node.guid + " has no connected input");
        std::string var = fresh_var("str");
        out << ind << "std::string " << var << " = std::to_string(" << input_val << ");\n";
        for (auto& o : node.outputs)
            pin_to_value[o.id] = var;

        auto bang_targets = follow_bang_from(node.bang_pin.id);
        for (auto* bt : bang_targets)
            emit_node(*bt, out, indent);

        return var;
    }

    if (node.type_id == NodeTypeID::DeclLocal) {
        // decl_local is emitted via emit_node in bang chain.
        // If materialized, the variable was already declared — just return its name.
        auto tokens = tokenize_args(node.args, false);
        if (tokens.size() >= 1) {
            std::string var_name = tokens[0];
            for (auto& o : node.outputs)
                pin_to_value[o.id] = var_name;
            return var_name;
        }
        throw std::runtime_error("codegen: decl_local has no name");
    }

    if (node.type_id == NodeTypeID::Next) {
        // next: output = std::next(input)
        if (!node.inputs.empty()) {
            auto pin_it = pin_to_value.find(node.inputs[0].id);
            if (pin_it != pin_to_value.end()) {
                std::string var = fresh_var("next_it");
                out << ind << "auto " << var << " = std::next(" << pin_it->second << ");\n";
                for (auto& o : node.outputs)
                    pin_to_value[o.id] = var;
                return var;
            }
            std::string src = find_source_pin(node.inputs[0].id);
            if (!src.empty()) {
                auto* src_node = find_source_node(node.inputs[0].id);
                if (src_node && !materialized.count(src_node->guid) && src_node->type_id != NodeTypeID::EventBang) {
                    materialize_node(*src_node, out, indent);
                }
                auto it = pin_to_value.find(src);
                if (it != pin_to_value.end()) {
                    std::string var = fresh_var("next_it");
                    out << ind << "auto " << var << " = std::next(" << it->second << ");\n";
                    for (auto& o : node.outputs)
                        pin_to_value[o.id] = var;
                    return var;
                }
            }
        }
        throw std::runtime_error("codegen: next node " + node.guid + " has no connected input");
    }

    if (node.type_id == NodeTypeID::Lock) {
        std::string mutex_var = resolve_inline_arg(node, 0);
        std::string lock_var = fresh_var("lock_guard");

        // Determine if lambda returns a value — check outputs first, then lambda pin type
        bool has_return = !node.outputs.empty();
        std::string ret_type_str = "void";
        if (has_return && node.outputs[0].resolved_type &&
            node.outputs[0].resolved_type->kind != TypeKind::Void) {
            ret_type_str = type_to_cpp(node.outputs[0].resolved_type);
        }
        if (!has_return) for (auto& inp : node.inputs) {
            if (inp.direction == FlowPin::Lambda && inp.resolved_type &&
                inp.resolved_type->kind == TypeKind::Function && inp.resolved_type->return_type &&
                inp.resolved_type->return_type->kind != TypeKind::Void) {
                has_return = true;
                ret_type_str = type_to_cpp(inp.resolved_type->return_type);
            }
        }

        std::string result_var;
        if (has_return) {
            result_var = fresh_var("lock_result");
            out << ind << ret_type_str << " " << result_var << "{};\n";
        }

        // Open lock scope
        out << ind << "{\n";
        out << indent_str(indent+1) << "std::lock_guard<std::mutex> " << lock_var << "(" << mutex_var << ");\n";

        // Find and emit lambda body
        FlowNode* lambda_root = nullptr;
        for (auto& inp : node.inputs) {
            if (inp.direction == FlowPin::Lambda) {
                auto* src = find_source_node(inp.id);
                if (src) lambda_root = src;
            }
        }

        if (lambda_root) {
            // Forward lock's extra input pins (argN) to the inner lambda's params
            std::set<std::string> inner_visited;
            std::vector<FlowPin*> inner_params;
            collect_lambda_pins(*lambda_root, inner_params, inner_visited);
            for (int pi = 0; pi < (int)inner_params.size(); pi++) {
                std::string arg_name = "arg" + std::to_string(pi);
                // Find the lock's argN pin value
                for (auto& inp : node.inputs) {
                    if (inp.name == arg_name) {
                        std::string arg_val;
                        // Check pin_to_value for the pin itself (forwarded from outer scope)
                        auto it = pin_to_value.find(inp.id);
                        if (it != pin_to_value.end()) arg_val = it->second;
                        else {
                            // Try connected source
                            std::string src = find_source_pin(inp.id);
                            if (!src.empty()) {
                                auto it2 = pin_to_value.find(src);
                                if (it2 != pin_to_value.end()) arg_val = it2->second;
                            }
                        }
                        if (!arg_val.empty()) {
                            pin_to_value[inner_params[pi]->id] = arg_val;
                            // Also register outputs of the param's node
                            auto dot = inner_params[pi]->id.find('.');
                            if (dot != std::string::npos) {
                                std::string pguid = inner_params[pi]->id.substr(0, dot);
                                auto* pnode = find_node_by_guid(pguid);
                                if (pnode) {
                                    for (int oi = 0; oi < (int)pnode->inputs.size(); oi++) {
                                        if (pnode->inputs[oi].id == inner_params[pi]->id &&
                                            oi < (int)pnode->outputs.size()) {
                                            pin_to_value[pnode->outputs[oi].id] = arg_val;
                                        }
                                    }
                                }
                            }
                        }
                        break;
                    }
                }
            }
            // Mark fully-forwarded param nodes as materialized
            for (auto* pp : inner_params) {
                auto dot = pp->id.find('.');
                if (dot == std::string::npos) continue;
                auto* pnode = find_node_by_guid(pp->id.substr(0, dot));
                if (!pnode) continue;
                bool all = true;
                for (auto& o : pnode->outputs)
                    if (pin_to_value.find(o.id) == pin_to_value.end()) { all = false; break; }
                if (all) materialized.insert(pnode->guid);
            }

            std::string result = materialize_node(*lambda_root, out, indent + 1);
            if (has_return) {
                out << indent_str(indent+1) << result_var << " = " << result << ";\n";
                // Register result BEFORE post_bang so downstream nodes can use it
                for (auto& o : node.outputs)
                    pin_to_value[o.id] = result_var;
            }
        }

        // post_bang fires INSIDE the lock scope
        auto bang_targets = follow_bang_from(node.bang_pin.id);
        for (auto* bt : bang_targets)
            emit_node(*bt, out, indent + 1);

        // Close lock scope
        out << ind << "}\n";

        if (has_return) {
            return result_var;
        }
        return "/* lock void */";
    }

    if (node.type_id == NodeTypeID::Cast) {
        // Cast node: currently only supports array -> vector
        // Materialize input dependency
        if (!node.inputs.empty()) {
            std::string src = find_source_pin(node.inputs[0].id);
            if (!src.empty()) {
                auto* src_node = find_source_node(node.inputs[0].id);
                if (src_node && !materialized.count(src_node->guid) && src_node->type_id != NodeTypeID::EventBang) {
                    materialize_node(*src_node, out, indent);
                }
            }
        }

        std::string dest_type = type_to_cpp_str(node.args);
        std::string input_val = resolve_pin_value(node, 0);
        std::string var = fresh_var("cast");
        out << ind << dest_type << " " << var << "(" << input_val << ".begin(), " << input_val << ".end());\n";
        if (!node.outputs.empty())
            pin_to_value[node.outputs[0].id] = var;

        auto bang_targets = follow_bang_from(node.bang_pin.id);
        for (auto* bt : bang_targets)
            emit_node(*bt, out, indent);

        return var;
    }

    if (node.type_id == NodeTypeID::New) {
        // Materialize non-lambda dependencies first
        // Skip inputs connected via as_lambda (those are handled as inline lambdas below)
        for (auto& inp : node.inputs) {
            std::string src = find_source_pin(inp.id);
            if (src.empty()) continue;
            // Check if source is an as_lambda pin
            if (src.find(".as_lambda") != std::string::npos) continue;
            auto* src_node = find_source_node(inp.id);
            if (src_node && !materialized.count(src_node->guid) && src_node->type_id != NodeTypeID::EventBang) {
                materialize_node(*src_node, out, indent);
            }
        }

        auto tokens = tokenize_args(node.args, false);
        if (tokens.empty()) throw std::runtime_error("codegen: new node has no type name");
        std::string type_name = tokens[0];
        std::string var = fresh_var(type_name);

        std::string ind1 = indent_str(indent + 1);
        std::string ind2 = indent_str(indent + 2);
        out << ind << type_name << " " << var << " = {\n";
        for (int i = 0; i < (int)node.inputs.size(); i++) {
            std::string field_name = node.inputs[i].name;
            std::string src = find_source_pin(node.inputs[i].id);
            if (!src.empty()) {
                auto pit = pin_to_value.find(src);
                if (pit != pin_to_value.end()) {
                    // Check if we need a cast to avoid narrowing (e.g. int -> f32)
                    auto* src_node = find_source_node(node.inputs[i].id);
                    bool needs_cast = false;
                    if (node.inputs[i].resolved_type && !node.inputs[i].resolved_type->is_generic &&
                        node.inputs[i].resolved_type->kind == TypeKind::Scalar &&
                        src_node && !src_node->outputs.empty() &&
                        src_node->outputs[0].resolved_type &&
                        src_node->outputs[0].resolved_type->is_generic) {
                        needs_cast = true;
                    }
                    if (needs_cast) {
                        out << ind1 << "." << field_name << " = static_cast<" << type_to_cpp(node.inputs[i].resolved_type) << ">(" << pit->second << "),\n";
                    } else {
                        out << ind1 << "." << field_name << " = " << pit->second << ",\n";
                    }
                } else {
                    // Source is likely a lambda (as_lambda connection)
                    auto* src_node = find_source_node(node.inputs[i].id);
                    if (src_node) {
                        // Generate inline lambda
                        // Find the lambda's parameters and return type
                        std::set<std::string> lam_visited;
                        std::vector<FlowPin*> lam_params;
                        collect_lambda_pins(*src_node, lam_params, lam_visited);

                        // Determine the expected function type from this input's resolved_type
                        // Try the type_name first (it might be a named alias like "gen_fn")
                        TypePtr fn_type = nullptr;
                        if (node.inputs[i].resolved_type) {
                            fn_type = node.inputs[i].resolved_type;
                            // If it's still Named, look up the struct field type
                            if (fn_type && fn_type->kind == TypeKind::Named) {
                                // Find the actual type from the decl_type node
                                auto* type_node = find_type_node(graph, fn_type->named_ref);
                                if (type_node) {
                                    // It's a type alias — parse the definition
                                    auto tokens = tokenize_args(type_node->args, false);
                                    if (classify_decl_type(tokens) == 1) {
                                        // Function type alias
                                        std::string def;
                                        for (size_t ti = 1; ti < tokens.size(); ti++) {
                                            if (!def.empty()) def += " ";
                                            def += tokens[ti];
                                        }
                                        std::string err;
                                        auto parsed = parse_type(def, err);
                                        if (parsed) fn_type = parsed;
                                    }
                                }
                            }
                        }
                        if (!fn_type || fn_type->kind != TypeKind::Function)
                            fn_type = node.inputs[i].resolved_type;

                        // Build lambda signature
                        out << ind1 << "." << field_name << " = [&](";
                        for (size_t pi = 0; pi < lam_params.size(); pi++) {
                            if (pi > 0) out << ", ";
                            // Get param type from function type if available
                            std::string param_type = "auto";
                            std::string param_name = "lam_p" + std::to_string(pi);
                            if (fn_type && fn_type->kind == TypeKind::Function && pi < fn_type->func_args.size()) {
                                param_type = type_to_cpp(fn_type->func_args[pi].type);
                                if (!fn_type->func_args[pi].name.empty())
                                    param_name = fn_type->func_args[pi].name;
                            }
                            out << param_type << " " << param_name;

                            // Register the parameter pin's value
                            pin_to_value[lam_params[pi]->id] = param_name;
                            // For the parameter node, register its outputs
                            // But DON'T mark as materialized — let materialize_node handle multi-output
                            auto pdot = lam_params[pi]->id.find('.');
                            if (pdot != std::string::npos) {
                                std::string pguid = lam_params[pi]->id.substr(0, pdot);
                                auto* pnode = find_node_by_guid(pguid);
                                if (pnode && pnode->outputs.size() == 1) {
                                    // Single-output: alias output to param
                                    pin_to_value[pnode->outputs[0].id] = param_name;
                                    materialized.insert(pguid);
                                }
                                // Multi-output: will be materialized later with proper expressions
                            }
                        }

                        // Return type
                        std::string ret_type = "void";
                        if (fn_type && fn_type->kind == TypeKind::Function && fn_type->return_type)
                            ret_type = type_to_cpp(fn_type->return_type);

                        out << ") -> " << ret_type << " {\n";

                        // Emit lambda body
                        // If the root is a data-producing node, materialize it
                        // If it's a bang node (store!, etc.), emit it as a statement
                        auto* root_nt = find_node_type(src_node->type_id);
                        bool is_bang_root = root_nt && (root_nt->bang_inputs > 0 || root_nt->bang_outputs > 0);

                        std::string result;
                        if (!is_bang_root) {
                            std::ostringstream lam_body;
                            result = materialize_node(*src_node, lam_body, indent + 2);
                            out << lam_body.str();
                        } else {
                            // Bang node as lambda root — emit directly
                            emit_node(*src_node, out, indent + 2);
                        }

                        // Emit bang chains within the lambda
                        for (auto& lg : lam_visited) {
                            auto* ln = find_node_by_guid(lg);
                            if (!ln) continue;
                            auto bt = follow_bang_from(ln->bang_pin.id);
                            for (auto* t : bt) emit_node(*t, out, indent + 2);
                        }

                        if (ret_type != "void" && !result.empty())
                            out << indent_str(indent + 2) << "return " << result << ";\n";
                        out << ind1 << "},\n";
                    } else {
                        out << ind1 << "." << field_name << " = {},\n";
                    }
                }
            } else {
                out << ind1 << "." << field_name << " = {},\n";
            }
        }
        out << ind << "};\n";

        for (auto& o : node.outputs)
            pin_to_value[o.id] = var;
        return var;
    }

    if (node.type_id == NodeTypeID::Select) {
        // Materialize condition dependency only (not branches — they may have side effects)
        if (!node.inputs.empty()) {
            std::string src = find_source_pin(node.inputs[0].id);
            if (!src.empty()) {
                auto* src_node = find_source_node(node.inputs[0].id);
                if (src_node && !materialized.count(src_node->guid) && src_node->type_id != NodeTypeID::EventBang) {
                    materialize_node(*src_node, out, indent);
                }
            }
        }

        std::string var = fresh_var("sel");
        std::string cond = resolve_inline_arg(node, 0);

        // Determine output type
        std::string type_str = "auto";
        bool is_void_select = false;
        if (!node.outputs.empty() && node.outputs[0].resolved_type) {
            if (node.outputs[0].resolved_type->kind == TypeKind::Void)
                is_void_select = true;
            else
                type_str = type_to_cpp(node.outputs[0].resolved_type);
        }

        // Lazy evaluation: use if/else to avoid side effects in unused branch
        if (!is_void_select)
            out << ind << type_str << " " << var << ";\n";
        out << ind << "if (" << cond << ") {\n";

        // Save state before branches — select branches are independent scopes
        auto saved_emitted = emitted_bang_nodes_;
        auto saved_materialized = materialized;

        // True branch: materialize all deps referenced by the true expression
        for (auto& inp : node.inputs) {
            if (inp.direction == FlowPin::Lambda) continue;
            std::string src = find_source_pin(inp.id);
            if (!src.empty() && pin_to_value.find(src) == pin_to_value.end()) {
                auto* src_node = find_source_node(inp.id);
                if (src_node && !materialized.count(src_node->guid) && src_node->type_id != NodeTypeID::EventBang) {
                    // Only materialize in true branch if referenced by arg 1
                    // We'll let resolve handle it — just ensure it's available
                }
            }
        }
        // Materialize deps for true expression's pin refs
        if (node.parsed_exprs.size() > 1 && node.parsed_exprs[1]) {
            // Find which pins arg 1 references and materialize their sources
            std::function<void(const ExprPtr&)> mat_deps = [&](const ExprPtr& e) {
                if (!e) return;
                if (e->kind == ExprKind::PinRef && e->pin_ref.index >= 0 &&
                    e->pin_ref.index < (int)node.inputs.size()) {
                    std::string src = find_source_pin(node.inputs[e->pin_ref.index].id);
                    if (!src.empty() && pin_to_value.find(src) == pin_to_value.end()) {
                        auto* src_node = find_source_node(node.inputs[e->pin_ref.index].id);
                        if (src_node && !materialized.count(src_node->guid) && src_node->type_id != NodeTypeID::EventBang)
                            materialize_node(*src_node, out, indent + 1);
                    }
                }
                for (auto& c : e->children) mat_deps(c);
            };
            mat_deps(node.parsed_exprs[1]);
        }
        current_indent_ = indent + 1;
        std::string true_val = resolve_inline_arg(node, 1);
        if (!is_void_select)
            out << indent_str(indent + 1) << var << " = " << true_val << ";\n";

        // Save true branch state, restore for false branch
        auto true_emitted = emitted_bang_nodes_;
        auto true_materialized = materialized;
        emitted_bang_nodes_ = saved_emitted;
        materialized = saved_materialized;

        // False branch: emit to temporary stream to check if non-empty
        std::ostringstream false_out;
        if (node.parsed_exprs.size() > 2 && node.parsed_exprs[2]) {
            std::function<void(const ExprPtr&)> mat_deps = [&](const ExprPtr& e) {
                if (!e) return;
                if (e->kind == ExprKind::PinRef && e->pin_ref.index >= 0 &&
                    e->pin_ref.index < (int)node.inputs.size()) {
                    std::string src = find_source_pin(node.inputs[e->pin_ref.index].id);
                    if (!src.empty() && pin_to_value.find(src) == pin_to_value.end()) {
                        auto* src_node = find_source_node(node.inputs[e->pin_ref.index].id);
                        if (src_node && !materialized.count(src_node->guid) && src_node->type_id != NodeTypeID::EventBang)
                            materialize_node(*src_node, false_out, indent + 1);
                    }
                }
                for (auto& c : e->children) mat_deps(c);
            };
            mat_deps(node.parsed_exprs[2]);
        }
        current_indent_ = indent + 1;
        std::string false_val = resolve_inline_arg(node, 2);
        if (!is_void_select)
            false_out << indent_str(indent + 1) << var << " = " << false_val << ";\n";

        current_indent_ = indent;
        std::string false_body = false_out.str();
        if (!false_body.empty()) {
            out << ind << "} else {\n";
            out << false_body;
        }
        out << ind << "}\n";

        // Merge emitted/materialized from both branches — anything emitted in either
        // branch should be considered emitted to prevent re-emission
        emitted_bang_nodes_.insert(true_emitted.begin(), true_emitted.end());
        materialized.insert(true_materialized.begin(), true_materialized.end());

        if (!is_void_select) {
            for (auto& o : node.outputs)
                pin_to_value[o.id] = var;
        }

        // Emit post_bang chain (side effects after select completes)
        auto bang_targets = follow_bang_from(node.bang_pin.id);
        for (auto* bt : bang_targets)
            emit_node(*bt, out, indent);

        return var;
    }

    if (node.type_id == NodeTypeID::Append) {
        // Non-bang append, returns iterator to appended item
        std::string target = resolve_inline_arg(node, 0);
        std::string value = resolve_inline_arg(node, 1);
        std::string var = fresh_var("append_it");
        out << ind << target << ".push_back(" << value << ");\n";
        out << ind << "auto " << var << " = std::prev(" << target << ".end());\n";
        for (auto& o : node.outputs)
            pin_to_value[o.id] = var;

        // Emit post_bang chain (side effects after append)
        auto bang_targets = follow_bang_from(node.bang_pin.id);
        for (auto* bt : bang_targets)
            emit_node(*bt, out, indent);

        return var;
    }

    if (node.type_id == NodeTypeID::Iterate) {
        // Non-bang iterate — same loop logic as iterate! but materialized as a data node
        std::string collection = resolve_inline_arg(node, 0);
        std::string it_var = fresh_var("it");

        out << ind << "for (auto " << it_var << " = " << collection << ".begin(); "
            << it_var << " != " << collection << ".end(); ) {\n";

        // Find lambda root
        FlowNode* lambda_root = nullptr;
        for (auto& inp : node.inputs) {
            if (inp.direction == FlowPin::Lambda) {
                auto* src = find_source_node(inp.id);
                if (src) lambda_root = src;
            }
        }

        if (lambda_root) {
            std::set<std::string> visited_lambda;
            std::vector<FlowPin*> lambda_params;
            collect_lambda_pins(*lambda_root, lambda_params, visited_lambda);

            // Register iterator variable for lambda params
            for (auto* param_pin : lambda_params) {
                auto dot = param_pin->id.find('.');
                if (dot != std::string::npos) {
                    std::string param_guid = param_pin->id.substr(0, dot);
                    auto* param_node = find_node_by_guid(param_guid);
                    if (param_node) {
                        for (auto& o : param_node->outputs)
                            pin_to_value[o.id] = it_var;
                        pin_to_value[param_pin->id] = it_var;
                    }
                }
            }

            std::string result = materialize_node(*lambda_root, out, indent + 1);

            // Emit bang chains from lambda subgraph
            for (auto& guid : visited_lambda) {
                auto* n = find_node_by_guid(guid);
                if (!n) continue;
                auto bang_targets = follow_bang_from(n->bang_pin.id);
                for (auto* bt : bang_targets)
                    if (!emitted_bang_nodes_.count(bt->guid))
                        emit_node(*bt, out, indent + 1);
            }

            out << indent_str(indent + 1) << it_var << " = " << result << ";\n";
        } else {
            out << indent_str(indent + 1) << "++" << it_var << ";\n";
        }

        out << ind << "}\n";

        // Emit post_bang chain (side effects after iterate completes)
        auto bang_targets = follow_bang_from(node.bang_pin.id);
        for (auto* bt : bang_targets)
            emit_node(*bt, out, indent);

        return "void()";
    }

    if (node.type_id == NodeTypeID::Call) {
        // Non-bang call: resolve function ref and args
        std::string fn_name = resolve_inline_arg(node, 0);
        std::ostringstream call_expr;
        call_expr << fn_name << "(";
        auto tokens = tokenize_args(node.args, false);
        int num_inline_args = (int)tokens.size() - 1;
        int total_args = num_inline_args + (int)node.inputs.size() - (int)scan_slots(node.args).slots.size();
        for (int i = 0; i < total_args; i++) {
            if (i > 0) call_expr << ", ";
            if (i < num_inline_args) {
                call_expr << resolve_inline_arg(node, i + 1);
            } else {
                int pin_idx = i - num_inline_args;
                if (pin_idx < (int)node.inputs.size()) {
                    std::string src = find_source_pin(node.inputs[pin_idx].id);
                    if (!src.empty()) {
                        auto it = pin_to_value.find(src);
                        if (it != pin_to_value.end()) {
                            call_expr << it->second;
                        } else {
                            auto* src_node = find_source_node(node.inputs[pin_idx].id);
                            if (src_node && !materialized.count(src_node->guid))
                                materialize_node(*src_node, out, indent);
                            auto it2 = pin_to_value.find(src);
                            call_expr << (it2 != pin_to_value.end() ? it2->second : "/* unresolved */");
                        }
                    } else {
                        call_expr << "/* missing arg */";
                    }
                }
            }
        }
        call_expr << ")";

        bool has_return = !node.outputs.empty();
        if (has_return) {
            std::string var = fresh_var("call_result");
            std::string type_str = node.outputs[0].resolved_type ? type_to_cpp(node.outputs[0].resolved_type) : "auto";
            out << ind << type_str << " " << var << " = " << call_expr.str() << ";\n";
            for (auto& o : node.outputs)
                pin_to_value[o.id] = var;

            // Post_bang fires after the call
            auto bang_targets = follow_bang_from(node.bang_pin.id);
            for (auto* bt : bang_targets)
                emit_node(*bt, out, indent);

            return var;
        } else {
            out << ind << call_expr.str() << ";\n";

            auto bang_targets = follow_bang_from(node.bang_pin.id);
            for (auto* bt : bang_targets)
                emit_node(*bt, out, indent);

            return "void()";
        }
    }

    if (node.type_id == NodeTypeID::LockBang) {
        // lock! is a bang node but may be reached via data dependency (when it has an output)
        // Delegate to emit_node which handles the full lock! logic
        emit_node(node, out, indent);
        if (!node.outputs.empty()) {
            auto it = pin_to_value.find(node.outputs[0].id);
            if (it != pin_to_value.end()) return it->second;
        }
        return "void()";
    }

    if (node.type_id == NodeTypeID::Store) {
        std::string target = resolve_inline_arg(node, 0);

        // Check if value comes from as_lambda (stored lambda)
        FlowNode* lambda_src = nullptr;
        for (auto& inp : node.inputs) {
            std::string src = find_source_pin(inp.id);
            if (!src.empty() && src.find(".as_lambda") != std::string::npos) {
                lambda_src = find_source_node(inp.id);
                break;
            }
        }

        if (lambda_src) {
            emit_stored_lambda(node, *lambda_src, target, out, indent);
        } else {
            std::string value = resolve_inline_arg(node, 1);
            out << ind << target << " = " << value << ";\n";
        }

        // Post_bang (side bang)
        auto bang_targets = follow_bang_from(node.bang_pin.id);
        for (auto* bt : bang_targets)
            emit_node(*bt, out, indent);

        return "void()";
    }

    if (node.type_id == NodeTypeID::Void) {
        // No-op, returns void
        for (auto& o : node.outputs)
            pin_to_value[o.id] = "void()";

        // Follow post_bang chain (e.g. when void is used as a lambda root for lock!)
        auto bang_targets = follow_bang_from(node.bang_pin.id);
        for (auto* bt : bang_targets)
            emit_node(*bt, out, indent);

        return "void()";
    }

    if (node.type_id == NodeTypeID::Discard) {
        // Evaluate inputs but discard — just materialize dependencies for side effects
        for (auto& inp : node.inputs) {
            std::string src = find_source_pin(inp.id);
            if (!src.empty() && pin_to_value.find(src) == pin_to_value.end()) {
                auto* src_node = find_source_node(inp.id);
                if (src_node && !materialized.count(src_node->guid))
                    materialize_node(*src_node, out, indent);
            }
        }
        // Post_bang
        auto bang_targets = follow_bang_from(node.bang_pin.id);
        for (auto* bt : bang_targets)
            emit_node(*bt, out, indent);
        return "void()";
    }

    if (node.type_id == NodeTypeID::Erase) {
        // Non-bang erase, returns iterator
        std::string target = resolve_inline_arg(node, 0);
        std::string key = resolve_inline_arg(node, 1);
        std::string var = fresh_var("erase_it");
        out << ind << "auto " << var << " = " << target << ".erase(" << key << ");\n";
        for (auto& o : node.outputs)
            pin_to_value[o.id] = var;
        return var;
    }

    if (is_any_of(node.type_id, NodeTypeID::CallBang, NodeTypeID::Call)) {
        // Bang call nodes with outputs (e.g. imgui_slider_int returns bool):
        // delegate to emit_node which handles call! and sets pin_to_value for outputs.
        emit_node(node, out, indent);
        for (auto& o : node.outputs)
            if (pin_to_value.count(o.id)) return pin_to_value[o.id];
        throw std::runtime_error("codegen: call node " + node.guid.substr(0, 8) + " materialized but produced no output");
    }

    throw std::runtime_error("codegen: cannot materialize node type " + std::string(node_type_str(node.type_id)) + " [" + node.guid.substr(0, 8) + "]");
}

// --- Node helpers ---

std::vector<FlowNode*> CodeGenerator::find_nodes(NodeTypeID type_id) {
    std::vector<FlowNode*> result;
    for (auto& n : graph.nodes)
        if (n.type_id == type_id) result.push_back(&n);
    return result;
}

FlowNode* CodeGenerator::find_node_by_guid(const std::string& guid) {
    return idx.find_node_by_guid(guid);
}

FlowNode* CodeGenerator::find_source_node(const std::string& to_pin_id) {
    FlowPin* pin = idx.find_pin(to_pin_id);
    if (!pin) return nullptr;
    return idx.source_node(pin);
}

std::string CodeGenerator::find_source_pin(const std::string& to_pin_id) {
    FlowPin* pin = idx.find_pin(to_pin_id);
    if (!pin) return "";
    FlowPin* src = idx.source_pin(pin);
    return src ? src->id : "";
}

std::vector<FlowNode*> CodeGenerator::follow_bang_from(const std::string& from_pin_id) {
    FlowPin* pin = idx.find_pin(from_pin_id);
    if (!pin) return {};
    return idx.follow_bang(pin);
}

// --- Type codegen ---

std::string CodeGenerator::generate_types() {
    std::ostringstream out;
    out << "#pragma once\n";
    out << "#include \"nanoruntime.h\"\n\n";
    out << "// Generated types from " << source_name << ".nano\n\n";

    // Forward declarations
    for (auto* node : find_nodes(NodeTypeID::DeclType)) {
        auto tokens = tokenize_args(node->args, false);
        if (tokens.empty()) continue;
        if (classify_decl_type(tokens) == 2)
            out << "struct " << tokens[0] << ";\n";
    }
    out << "\n";

    // Aliases and function types first
    for (auto* node : find_nodes(NodeTypeID::DeclType)) {
        auto tokens = tokenize_args(node->args, false);
        if (tokens.empty()) continue;
        int cls = classify_decl_type(tokens);
        if (cls == 0 || cls == 1) {
            std::string def;
            for (size_t i = 1; i < tokens.size(); i++) {
                if (!def.empty()) def += " ";
                def += tokens[i];
            }
            out << "using " << tokens[0] << " = " << type_to_cpp_str(def) << ";\n";
        }
    }
    out << "\n";

    // Struct definitions
    for (auto* node : find_nodes(NodeTypeID::DeclType)) {
        auto tokens = tokenize_args(node->args, false);
        if (tokens.empty()) continue;
        if (classify_decl_type(tokens) != 2) continue;

        auto fields = parse_type_fields(*node);
        out << "struct " << tokens[0] << " {\n";
        for (auto& f : fields)
            out << "    " << type_to_cpp_str(f.type_name) << " " << f.name << " = {};\n";
        out << "};\n\n";
    }

    return out.str();
}

// --- Header codegen ---

std::string CodeGenerator::generate_header() {
    std::ostringstream out;
    out << "#pragma once\n";
    out << "#include \"" << source_name << "_types.h\"\n\n";
    out << "// Generated program from " << source_name << ".nano\n\n";

    for (auto* node : find_nodes(NodeTypeID::DeclVar)) {
        auto tokens = tokenize_args(node->args, false);
        if (tokens.size() < 2) continue;
        std::string type_str;
        for (size_t i = 1; i < tokens.size(); i++) {
            if (!type_str.empty()) type_str += " ";
            type_str += tokens[i];
        }
        out << "extern " << type_to_cpp_str(type_str) << " " << tokens[0] << ";\n";
    }
    out << "\n";

    // FFI declarations
    for (auto* node : find_nodes(NodeTypeID::Ffi)) {
        auto tokens = tokenize_args(node->args, false);
        if (tokens.size() < 2) continue;
        std::string type_str;
        for (size_t i = 1; i < tokens.size(); i++) {
            if (!type_str.empty()) type_str += " ";
            type_str += tokens[i];
        }
        auto fn_type = pool.intern(type_str);
        if (fn_type && fn_type->kind == TypeKind::Function) {
            std::string ret = fn_type->return_type ? type_to_cpp(fn_type->return_type) : "void";
            out << "extern " << ret << " " << tokens[0] << "(";
            for (size_t i = 0; i < fn_type->func_args.size(); i++) {
                if (i > 0) out << ", ";
                out << type_to_cpp(fn_type->func_args[i].type) << " " << fn_type->func_args[i].name;
            }
            out << ");\n";
        }
    }
    out << "\n";

    for (auto* node : find_nodes(NodeTypeID::DeclEvent)) {
        auto tokens = tokenize_args(node->args, false);
        if (tokens.empty()) continue;
        auto args = parse_event_args(*node, graph);
        out << "void on_" << tokens[0] << "(";
        for (size_t i = 0; i < args.size(); i++) {
            if (i > 0) out << ", ";
            out << type_to_cpp_str(args[i].type_name) << " " << args[i].name;
        }
        out << ");\n";
    }
    out << "\n";

    return out.str();
}

// --- Implementation codegen ---

std::string CodeGenerator::generate_impl() {
    idx.rebuild(graph);

    std::ostringstream out;
    out << "#include \"" << source_name << "_program.h\"\n\n";

    // Global variable definitions
    for (auto* node : find_nodes(NodeTypeID::DeclVar)) {
        auto tokens = tokenize_args(node->args, false);
        if (tokens.size() < 2) continue;
        std::string type_str;
        for (size_t i = 1; i < tokens.size(); i++) {
            if (!type_str.empty()) type_str += " ";
            type_str += tokens[i];
        }
        out << type_to_cpp_str(type_str) << " " << tokens[0] << " = {};\n";
    }
    out << "\n";

    // Event handlers
    for (auto* event_node : find_nodes(NodeTypeID::EventBang)) {
        auto tokens = tokenize_args(event_node->args, false);
        if (tokens.empty()) continue;
        std::string event_name = tokens[0];
        if (!event_name.empty() && event_name[0] == '~')
            event_name = event_name.substr(1);

        auto* decl = find_event_node(graph, tokens[0]);
        if (!decl) continue;
        auto args = parse_event_args(*decl, graph);

        std::vector<std::pair<std::string, std::string>> params;
        for (auto& a : args)
            params.push_back({type_to_cpp_str(a.type_name), a.name});

        emit_event_handler(*event_node, event_name, params, out);
    }

    return out.str();
}

void CodeGenerator::emit_event_handler(FlowNode& event_node, const std::string& event_name,
                                       const std::vector<std::pair<std::string, std::string>>& params,
                                       std::ostringstream& out) {
    // Reset per-handler context
    pin_to_value.clear();
    materialized.clear();
    emitted_bang_nodes_.clear();
    temp_counter = 0;

    // Register event output pins → parameter names
    for (auto& op : event_node.outputs) {
        pin_to_value[op.id] = op.name;
    }

    // Emit function signature
    out << "void on_" << event_name << "(";
    for (size_t i = 0; i < params.size(); i++) {
        if (i > 0) out << ", ";
        out << params[i].first << " " << params[i].second;
    }
    out << ") {\n";

    // Follow bang chain from event
    for (auto& bout : event_node.bang_outputs) {
        auto targets = follow_bang_from(bout.id);
        for (auto* target : targets)
            emit_node(*target, out, 1);
    }

    out << "}\n\n";
}

// --- Emit a single bang-triggered node ---

void CodeGenerator::emit_node(FlowNode& node, std::ostringstream& out, int indent) {
    // Guard against duplicate emission
    if (emitted_bang_nodes_.count(node.guid)) return;
    emitted_bang_nodes_.insert(node.guid);

    current_out_ = &out;
    current_indent_ = indent;
    std::string ind = indent_str(indent);

    // Materialize any data dependencies that haven't been emitted yet
    // Skip lambda inputs and as_lambda sources — they're handled by node-specific code
    for (auto& inp : node.inputs) {
        if (inp.direction == FlowPin::Lambda) continue;
        std::string src = find_source_pin(inp.id);
        if (!src.empty() && src.find(".as_lambda") != std::string::npos) continue; // lambda store
        if (!src.empty() && pin_to_value.find(src) == pin_to_value.end()) {
            auto* src_node = find_source_node(inp.id);
            if (src_node && src_node->type_id != NodeTypeID::EventBang && !materialized.count(src_node->guid)) {
                materialize_node(*src_node, out, indent);
            }
        }
    }

    if (is_any_of(node.type_id, NodeTypeID::StoreBang, NodeTypeID::Store)) {
        std::string target = resolve_inline_arg(node, 0);

        // Check if value comes from an as_lambda pin (storing a lambda as a variable)
        FlowNode* lambda_src = nullptr;
        for (auto& inp : node.inputs) {
            std::string src = find_source_pin(inp.id);
            if (!src.empty() && src.find(".as_lambda") != std::string::npos) {
                lambda_src = find_source_node(inp.id);
                break;
            }
        }

        if (lambda_src) {
            emit_stored_lambda(node, *lambda_src, target, out, indent);
        } else {
            std::string value = resolve_inline_arg(node, 1);
            out << ind << target << " = " << value << ";\n";
        }

        // Follow post_bang then bang outputs
        for (auto* t : follow_bang_from(node.bang_pin.id))
            emit_node(*t, out, indent);
        for (auto& bout : node.bang_outputs)
            for (auto* t : follow_bang_from(bout.id))
                emit_node(*t, out, indent);
    }
    else if (node.type_id == NodeTypeID::AppendBang) {
        std::string target = resolve_inline_arg(node, 0);
        std::string value;
        if (node.parsed_exprs.size() >= 2)
            value = resolve_inline_arg(node, 1);
        else
            throw std::runtime_error("codegen: append! missing value argument");

        // append returns iterator — materialize if output is connected
        bool has_output_connection = false;
        for (auto& o : node.outputs)
            for (auto& l : graph.links)
                if (l.from_pin == o.id) has_output_connection = true;

        if (has_output_connection) {
            std::string var = fresh_var("append_it");
            out << ind << target << ".push_back(" << value << ");\n";
            out << ind << "auto " << var << " = std::prev(" << target << ".end());\n";
            for (auto& o : node.outputs)
                pin_to_value[o.id] = var;
        } else {
            out << ind << target << ".push_back(" << value << ");\n";
        }

        for (auto* t : follow_bang_from(node.bang_pin.id))
            emit_node(*t, out, indent);
    }
    else if (node.type_id == NodeTypeID::ResizeBang) {
        std::string target = resolve_inline_arg(node, 0);
        std::string size = resolve_inline_arg(node, 1);
        // resize takes size_t; cast if the size arg isn't already u64/size_t
        bool needs_cast = true;
        if (node.inputs.size() >= 2 && node.inputs[1].resolved_type &&
            node.inputs[1].resolved_type->kind == TypeKind::Scalar &&
            node.inputs[1].resolved_type->scalar == ScalarType::U64)
            needs_cast = false;
        if (node.parsed_exprs.size() >= 2 && node.parsed_exprs[1] &&
            node.parsed_exprs[1]->resolved_type &&
            node.parsed_exprs[1]->resolved_type->kind == TypeKind::Scalar &&
            node.parsed_exprs[1]->resolved_type->scalar == ScalarType::U64)
            needs_cast = false;
        if (needs_cast)
            out << ind << target << ".resize(static_cast<size_t>(" << size << "));\n";
        else
            out << ind << target << ".resize(" << size << ");\n";

        for (auto* t : follow_bang_from(node.bang_pin.id))
            emit_node(*t, out, indent);
        for (auto& bout : node.bang_outputs)
            for (auto* t : follow_bang_from(bout.id))
                emit_node(*t, out, indent);
    }
    else if (node.type_id == NodeTypeID::EraseBang) {
        std::string target = resolve_inline_arg(node, 0);
        std::string key = resolve_inline_arg(node, 1);
        out << ind << target << ".erase(" << key << ");\n";

        for (auto* t : follow_bang_from(node.bang_pin.id))
            emit_node(*t, out, indent);
        for (auto& bout : node.bang_outputs)
            for (auto* t : follow_bang_from(bout.id))
                emit_node(*t, out, indent);
    }
    else if (node.type_id == NodeTypeID::SelectBang) {
        std::string cond = resolve_inline_arg(node, 0);
        out << ind << "if (" << cond << ") {\n";

        // Save state before true branch — cond branches are independent scopes
        auto saved_emitted = emitted_bang_nodes_;
        auto saved_materialized = materialized;

        if (!node.bang_outputs.empty())
            for (auto* t : follow_bang_from(node.bang_outputs[0].id))
                emit_node(*t, out, indent + 1);

        if (node.bang_outputs.size() > 1) {
            auto targets = follow_bang_from(node.bang_outputs[1].id);
            if (!targets.empty()) {
                // Restore state for false branch so shared nodes emit in both
                emitted_bang_nodes_ = saved_emitted;
                materialized = saved_materialized;
                out << ind << "} else {\n";
                for (auto* t : targets)
                    emit_node(*t, out, indent + 1);
            }
        }
        out << ind << "}\n";
    }
    else if (node.type_id == NodeTypeID::IterateBang) {
        std::string collection = resolve_inline_arg(node, 0);
        std::string it_var = fresh_var("it");

        out << ind << "for (auto " << it_var << " = " << collection << ".begin(); "
            << it_var << " != " << collection << ".end(); ) {\n";

        // Find the lambda root (connected to the fn/@0 pin)
        FlowNode* lambda_root = nullptr;
        for (auto& inp : node.inputs) {
            if (inp.direction == FlowPin::Lambda) {
                auto* src = find_source_node(inp.id);
                if (src) lambda_root = src;
            }
        }

        if (lambda_root) {
            // Find all unconnected inputs in the lambda subgraph
            // These are the lambda parameters (the iterator)
            // Use the same recursive collection as inference does
            std::set<std::string> visited_lambda;
            std::vector<FlowPin*> lambda_params;
            collect_lambda_pins(*lambda_root, lambda_params, visited_lambda);

            // Register iterator variable for all lambda parameter pins
            for (auto* param_pin : lambda_params) {
                // The pin's value is the dereferenced iterator
                std::string src_pin = param_pin->id;
                // Find all outgoing connections from this pin's node's output
                // Actually, the param pin is an INPUT pin. We need the node's OUTPUT
                // that carries the parameter value through.
                // For dup nodes: output = input, so register the dup's output
                auto dot = src_pin.find('.');
                if (dot != std::string::npos) {
                    std::string param_guid = src_pin.substr(0, dot);
                    auto* param_node = find_node_by_guid(param_guid);
                    if (param_node) {
                        // Register this node's outputs as the iterator
                        // (not dereferenced — field access uses -> for iterators)
                        for (auto& o : param_node->outputs)
                            pin_to_value[o.id] = it_var;
                        pin_to_value[src_pin] = it_var;
                        materialized.insert(param_guid);
                    }
                }
            }

            // Now materialize the lambda root's data dependencies and emit
            // The lambda root's output is the next iterator value
            std::string result = materialize_node(*lambda_root, out, indent + 1);

            // Emit bang chains from nodes in the lambda's data subgraph
            // (e.g. expr with post_bang -> output_mix! inside the loop)
            for (auto& guid : visited_lambda) {
                auto* n = find_node_by_guid(guid);
                if (!n) continue;
                auto bang_targets = follow_bang_from(n->bang_pin.id);
                for (auto* bt : bang_targets) {
                    // Only emit if the bang target isn't already materialized outside
                    if (!materialized.count(bt->guid))
                        emit_node(*bt, out, indent + 1);
                }
            }

            // Assign the result to the iterator
            out << indent_str(indent + 1) << it_var << " = " << result << ";\n";
        } else {
            out << indent_str(indent + 1) << "++" << it_var << ";\n";
        }

        out << ind << "}\n";

        for (auto& bout : node.bang_outputs)
            for (auto* t : follow_bang_from(bout.id))
                emit_node(*t, out, indent);
    }
    else if (node.type_id == NodeTypeID::LockBang) {
        std::string mutex_var = resolve_inline_arg(node, 0);
        std::string lock_var = fresh_var("lock_guard");

        // Find the lambda root first to determine return type
        FlowNode* lambda_root = nullptr;
        for (auto& inp : node.inputs) {
            if (inp.direction == FlowPin::Lambda) {
                auto* src = find_source_node(inp.id);
                if (src) lambda_root = src;
            }
        }

        // Determine if lambda returns a value
        bool has_return = !node.outputs.empty();

        std::string result_var;

        if (has_return) {
            // Use IIFE to capture result out of lock scope with auto type deduction
            result_var = fresh_var("lock_result");
            out << ind << "auto " << result_var << " = [&]() {\n";
            out << indent_str(indent+1) << "std::lock_guard<std::mutex> " << lock_var << "(" << mutex_var << ");\n";

            if (lambda_root) {
                std::string result = materialize_node(*lambda_root, out, indent + 1);
                out << indent_str(indent+1) << "return " << result << ";\n";
            }

            out << ind << "}();\n";
            for (auto& o : node.outputs)
                pin_to_value[o.id] = result_var;
        } else {
            // No return — simple scope
            out << ind << "{\n";
            out << indent_str(indent+1) << "std::lock_guard<std::mutex> " << lock_var << "(" << mutex_var << ");\n";

            if (lambda_root)
                materialize_node(*lambda_root, out, indent + 1);

            out << ind << "}\n";
        }

        // bang_out fires AFTER the lock is released
        for (auto& bout : node.bang_outputs)
            for (auto* t : follow_bang_from(bout.id))
                emit_node(*t, out, indent);
    }
    else if (node.type_id == NodeTypeID::ExprBang) {
        if (!node.parsed_exprs.empty()) {
            for (auto& expr : node.parsed_exprs) {
                if (expr) out << ind << expr_to_cpp(expr, &node) << ";\n";
            }
        }
        for (auto& bout : node.bang_outputs)
            for (auto* t : follow_bang_from(bout.id))
                emit_node(*t, out, indent);
    }
    else if (node.type_id == NodeTypeID::DiscardBang) {
        // Materialize input to trigger side effects, then discard.
        // The input may be void (e.g. from a void select), so don't require a value —
        // just ensure the source node is materialized for its side effects.
        if (!node.inputs.empty()) {
            std::string src = find_source_pin(node.inputs[0].id);
            if (!src.empty()) {
                auto* src_node = find_source_node(node.inputs[0].id);
                if (src_node && !materialized.count(src_node->guid))
                    materialize_node(*src_node, out, indent);
            }
        }
        // Just follow bang chain
        for (auto* t : follow_bang_from(node.bang_pin.id))
            emit_node(*t, out, indent);
        for (auto& bout : node.bang_outputs)
            for (auto* t : follow_bang_from(bout.id))
                emit_node(*t, out, indent);
    }
    else if (node.type_id == NodeTypeID::OutputMixBang) {
        std::string value = resolve_inline_arg(node, 0);
        out << ind << "output_mix(" << value << ");\n";
    }
    else if (node.type_id == NodeTypeID::CallBang) {
        // Bang call: resolve function ref and args
        std::string fn_name = resolve_inline_arg(node, 0);
        std::ostringstream call_expr;
        call_expr << fn_name << "(";
        auto tokens = tokenize_args(node.args, false);
        int num_inline_args = (int)tokens.size() - 1; // args after fn name
        // $N refs are already in num_inline_args AND in node.inputs — subtract to avoid duplication
        int total_args = num_inline_args + (int)node.inputs.size() - (int)scan_slots(node.args).slots.size();
        for (int i = 0; i < total_args; i++) {
            if (i > 0) call_expr << ", ";
            if (i < num_inline_args) {
                call_expr << resolve_inline_arg(node, i + 1);
            } else {
                int pin_idx = i - num_inline_args;
                if (pin_idx < (int)node.inputs.size()) {
                    std::string src = find_source_pin(node.inputs[pin_idx].id);
                    if (!src.empty()) {
                        auto it = pin_to_value.find(src);
                        if (it != pin_to_value.end()) {
                            call_expr << it->second;
                        } else {
                            auto* src_node = find_source_node(node.inputs[pin_idx].id);
                            if (src_node && !materialized.count(src_node->guid))
                                materialize_node(*src_node, out, indent);
                            auto it2 = pin_to_value.find(src);
                            call_expr << (it2 != pin_to_value.end() ? it2->second : "/* unresolved */");
                        }
                    } else {
                        call_expr << "/* missing arg */";
                    }
                }
            }
        }
        call_expr << ")";

        bool has_return = !node.outputs.empty();
        if (has_return) {
            std::string var = fresh_var("call_result");
            std::string type_str = node.outputs[0].resolved_type ? type_to_cpp(node.outputs[0].resolved_type) : "auto";
            out << ind << type_str << " " << var << " = " << call_expr.str() << ";\n";
            for (auto& o : node.outputs)
                pin_to_value[o.id] = var;
        } else {
            out << ind << call_expr.str() << ";\n";
        }

        // Post_bang and bang_out
        for (auto* t : follow_bang_from(node.bang_pin.id))
            emit_node(*t, out, indent);
        for (auto& bout : node.bang_outputs)
            for (auto* t : follow_bang_from(bout.id))
                emit_node(*t, out, indent);
    }
    else if (node.type_id == NodeTypeID::DeclLocal) {
        // decl_local <name> <type>
        auto tokens = tokenize_args(node.args, false);
        if (tokens.size() >= 2) {
            std::string var_name = tokens[0];
            std::string type_str;
            for (size_t i = 1; i < tokens.size(); i++) {
                if (!type_str.empty()) type_str += " ";
                type_str += tokens[i];
            }

            out << ind << type_to_cpp_str(type_str) << " " << var_name << " = {};\n";

            // Register the variable in pin_to_value for downstream references
            // The output pin carries a reference to this local
            for (auto& o : node.outputs)
                pin_to_value[o.id] = var_name;
        }

        // Follow bang output
        for (auto& bout : node.bang_outputs)
            for (auto* t : follow_bang_from(bout.id))
                emit_node(*t, out, indent);
    }
    else {
        throw std::runtime_error("codegen: unsupported bang node type " + std::string(node_type_str(node.type_id)) + " [" + node.guid.substr(0, 8) + "]");
    }
}

// --- Main and CMake ---

std::string CodeGenerator::generate_cmake(const std::string& nanoruntime_path,
                                           const std::string& nanoc_path,
                                           const std::string& nano_project_path,
                                           const std::string& nano_source_path,
                                           const std::string& nanodeps_path) {
    std::string sn = source_name;
    std::ostringstream out;
    out << "cmake_minimum_required(VERSION 3.25)\n";
    out << "project(" << sn << " LANGUAGES C CXX)\n\n";
    out << "set(CMAKE_CXX_STANDARD 20)\n";
    out << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";
    out << "if(NOT WIN32)\n";
    out << "    find_program(CCACHE_PROGRAM ccache)\n";
    out << "    if(CCACHE_PROGRAM)\n";
    out << "        set(CMAKE_C_COMPILER_LAUNCHER \"${CCACHE_PROGRAM}\")\n";
    out << "        set(CMAKE_CXX_COMPILER_LAUNCHER \"${CCACHE_PROGRAM}\")\n";
    out << "    endif()\n";
    out << "endif()\n\n";

    // Check which std modules are imported
    bool uses_imgui = false;
    bool uses_gui = false;
    for (auto& n : graph.nodes) {
        if (n.type_id == NodeTypeID::DeclImport) {
            auto tokens = tokenize_args(n.args, false);
            if (!tokens.empty() && tokens[0] == "std/imgui")
                uses_imgui = true;
            if (!tokens.empty() && tokens[0] == "std/gui")
                uses_gui = true;
        }
    }

    // Fetch dependencies via NanoDeps.cmake (FetchContent on Linux/macOS, vcpkg on Windows)
    if (uses_imgui || uses_gui)
        out << "set(NANO_NEEDS_IMGUI ON)\n";
    out << "include(\"" << nanodeps_path << "\")\n\n";

    out << "set(NANORUNTIME_PATH \"" << nanoruntime_path << "\")\n";

    // Custom command: re-run nanoc when the .nano source changes
    out << "set(NANO_PROJECT \"" << nano_project_path << "\")\n";
    out << "set(NANO_SOURCE \"" << nano_source_path << "\")\n";
    out << "set(NANOC \"" << nanoc_path << "\")\n";
    out << "set(GENERATED_FILES\n";
    out << "    ${CMAKE_CURRENT_SOURCE_DIR}/" << sn << "_types.h\n";
    out << "    ${CMAKE_CURRENT_SOURCE_DIR}/" << sn << "_program.h\n";
    out << "    ${CMAKE_CURRENT_SOURCE_DIR}/" << sn << "_program.cpp\n";
    out << ")\n\n";

    out << "add_custom_command(\n";
    out << "    OUTPUT ${GENERATED_FILES}\n";
    out << "    COMMAND ${NANOC} ${NANO_PROJECT} -o ${CMAKE_CURRENT_SOURCE_DIR}\n";
    out << "    DEPENDS ${NANO_SOURCE}\n";
    out << "    COMMENT \"Compiling ${NANO_SOURCE} with nanoc\"\n";
    out << ")\n\n";

    out << "add_executable(" << sn << "\n";
    out << "    ${NANORUNTIME_PATH}/main.cpp\n";
    if (uses_imgui)
        out << "    ${NANORUNTIME_PATH}/nano_imgui.cpp\n";
    if (uses_gui)
        out << "    ${NANORUNTIME_PATH}/nano_gui.cpp\n";
    out << "    ${GENERATED_FILES}\n";
    out << ")\n\n";

    out << "set_source_files_properties(${GENERATED_FILES} PROPERTIES GENERATED TRUE)\n\n";

    out << "target_include_directories(" << sn << " PRIVATE\n";
    out << "    ${CMAKE_CURRENT_SOURCE_DIR}\n";
    out << "    ${NANORUNTIME_PATH}\n";
    out << ")\n\n";

    // Link libraries — platform-dependent target names
    if (uses_imgui || uses_gui) {
        out << "if(WIN32)\n";
        out << "    target_link_libraries(" << sn << " PRIVATE SDL3::SDL3 imgui::imgui)\n";
        out << "else()\n";
        out << "    target_link_libraries(" << sn << " PRIVATE SDL3::SDL3-static imgui_all)\n";
        out << "endif()\n";
    } else {
        out << "if(WIN32)\n";
        out << "    target_link_libraries(" << sn << " PRIVATE SDL3::SDL3)\n";
        out << "else()\n";
        out << "    target_link_libraries(" << sn << " PRIVATE SDL3::SDL3-static)\n";
        out << "endif()\n";
    }
    return out.str();
}

std::string CodeGenerator::generate_vcpkg() {
    bool uses_imgui = false;
    bool uses_gui = false;
    for (auto& n : graph.nodes) {
        if (n.type_id == NodeTypeID::DeclImport) {
            auto tokens = tokenize_args(n.args, false);
            if (!tokens.empty() && tokens[0] == "std/imgui")
                uses_imgui = true;
            if (!tokens.empty() && tokens[0] == "std/gui")
                uses_gui = true;
        }
    }

    std::ostringstream out;
    out << "{\n";
    out << "  \"name\": \"" << source_name << "\",\n";
    out << "  \"version-string\": \"0.1.0\",\n";
    if (uses_imgui || uses_gui)
        out << "  \"dependencies\": [\"sdl3\", {\"name\": \"imgui\", \"features\": [\"sdl3-binding\", \"sdl3-renderer-binding\"]}]\n";
    else
        out << "  \"dependencies\": [\"sdl3\"]\n";
    out << "}\n";
    return out.str();
}

