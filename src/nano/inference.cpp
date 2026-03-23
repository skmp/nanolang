#include "inference.h"

std::vector<std::string> GraphInference::run(FlowGraph& graph) {
    std::vector<std::string> all_errors;

    // Phase 1: Clear all resolved types
    clear_all(graph);

    // Phase 2: Build type registry from decl_type nodes
    build_registry(graph);

    // Phase 3: Build inference context (var_types, named_types)
    build_context(graph);

    // Phase 4: Resolve pin types from type_name strings
    resolve_pin_type_names(graph);

    // Phase 5: Fixed-point propagation
    for (int iter = 0; iter < 10; iter++) {
        bool changed = false;

        // 5.1: Propagate across connections
        changed |= propagate_connections(graph);

        // 5.2: Infer expression nodes
        changed |= infer_expr_nodes(graph);

        // 5.3: Resolve lambda types
        changed |= resolve_lambdas(graph);

        if (!changed) break;
    }

    // Phase 6: Check link type compatibility
    for (auto& link : graph.links) {
        if (!link.error.empty()) continue; // already has an error from lambda validation
        FlowPin* from_pin = nullptr;
        FlowPin* to_pin = nullptr;
        for (auto& node : graph.nodes) {
            for (auto& p : node.outputs) if (p.id == link.from_pin) from_pin = &p;
            for (auto& p : node.bang_outputs) if (p.id == link.from_pin) from_pin = &p;
            if (node.lambda_grab.id == link.from_pin) from_pin = &node.lambda_grab;
            if (node.bang_pin.id == link.from_pin) from_pin = &node.bang_pin;
            for (auto& p : node.inputs) if (p.id == link.to_pin) to_pin = &p;
            for (auto& p : node.bang_inputs) if (p.id == link.to_pin) to_pin = &p;
        }
        if (from_pin && to_pin &&
            from_pin->resolved_type && to_pin->resolved_type &&
            !from_pin->resolved_type->is_generic && !to_pin->resolved_type->is_generic &&
            !types_compatible(from_pin->resolved_type, to_pin->resolved_type)) {
            link.error = "Type mismatch: " +
                type_to_string(from_pin->resolved_type) + " vs " +
                type_to_string(to_pin->resolved_type);
        }
    }

    // Collect all errors
    for (auto& node : graph.nodes) {
        if (!node.error.empty())
            all_errors.push_back(node.guid + ": " + node.error);
    }
    for (auto& link : graph.links) {
        if (!link.error.empty())
            all_errors.push_back("link [" + link.from_pin.substr(0, 8) + "->...]: " + link.error);
    }
    return all_errors;
}

void GraphInference::clear_all(FlowGraph& graph) {
    for (auto& node : graph.nodes) {
        for (auto& p : node.inputs) p.resolved_type = nullptr;
        for (auto& p : node.outputs) p.resolved_type = nullptr;
        for (auto& p : node.bang_inputs) p.resolved_type = nullptr;
        for (auto& p : node.bang_outputs) p.resolved_type = nullptr;
        node.lambda_grab.resolved_type = nullptr;
        node.bang_pin.resolved_type = nullptr;
        for (auto& e : node.parsed_exprs) clear_expr_types(e);
    }
    for (auto& link : graph.links) link.error.clear();
}

void GraphInference::build_registry(FlowGraph& graph) {
    registry.clear();
    for (auto& node : graph.nodes) {
        if (node.type != "decl_type") continue;
        auto tokens = tokenize_args(node.args, false);
        if (tokens.size() < 2) continue;
        std::string def;
        for (size_t i = 1; i < tokens.size(); i++) {
            if (!def.empty()) def += " ";
            def += tokens[i];
        }
        // Determine if this is a type alias, function type, or struct with fields
        int decl_class = classify_decl_type(tokens);
        if (decl_class == 0 || decl_class == 1) { // alias or function type
            // Type alias (e.g. "osc_list list<osc_def>") or
            // Function type (e.g. "gen_fn (id:u64) -> osc_res")
            registry.register_type(tokens[0], def);
        } else {
            // Struct with fields — register as placeholder, fields validated separately
            registry.register_type(tokens[0], "void");
        }
    }
    registry.resolve_all();
}

void GraphInference::build_context(FlowGraph& graph) {
    ctx.var_types.clear();
    ctx.named_types.clear();

    for (auto& node : graph.nodes) {
        if (node.type == "decl_var") {
            auto tokens = tokenize_args(node.args, false);
            if (tokens.size() >= 2) {
                // Join all tokens after the name as the type (handles "map<u32, f32>")
                std::string type_str;
                for (size_t i = 1; i < tokens.size(); i++) {
                    if (!type_str.empty()) type_str += " ";
                    type_str += tokens[i];
                }
                ctx.var_types[tokens[0]] = pool.intern(type_str);
            }
        }
    }
    // Register FFI functions as global variables with function types
    for (auto& node : graph.nodes) {
        if (node.type == "ffi") {
            auto tokens = tokenize_args(node.args, false);
            if (tokens.size() >= 2) {
                std::string type_str;
                for (size_t i = 1; i < tokens.size(); i++) {
                    if (!type_str.empty()) type_str += " ";
                    type_str += tokens[i];
                }
                auto fn_type = pool.intern(type_str);
                if (fn_type && fn_type->kind == TypeKind::Function) {
                    ctx.var_types[tokens[0]] = fn_type;
                } else {
                    node.error = "ffi: type must be a function type (got " + type_str + ")";
                }
            } else {
                node.error = "ffi: requires <name> <function_type>";
            }
        }
        if (node.type == "decl_import") {
            auto tokens = tokenize_args(node.args, false);
            if (tokens.empty()) {
                node.error = "decl_import: requires a path (e.g. std/math)";
            } else if (tokens[0].substr(0, 4) != "std/") {
                node.error = "decl_import: only std/ imports are currently supported (got " + tokens[0] + ")";
            }
        }
    }
    for (auto& node : graph.nodes) {
        if (node.type != "decl_type") continue;
        auto tokens = tokenize_args(node.args, false);
        if (tokens.size() < 2) continue;
        auto fields = parse_type_fields(node);
        if (!fields.empty()) {
            // Struct type with fields
            auto struct_type = std::make_shared<TypeExpr>();
            struct_type->kind = TypeKind::Struct;
            for (auto& f : fields)
                struct_type->fields.push_back({f.name, pool.intern(f.type_name)});
            ctx.named_types[tokens[0]] = struct_type;
        }
        // Type aliases (no fields) are resolved via registry.parsed
    }
}

void GraphInference::resolve_pin_type_names(FlowGraph& graph) {
    auto resolve = [&](FlowPin& p) {
        if (p.type_name.empty() || p.type_name == "value") return;
        if (p.type_name == "bang") { p.resolved_type = pool.t_bang; return; }
        if (p.type_name == "lambda") return;
        p.resolved_type = pool.intern(p.type_name);
    };
    for (auto& node : graph.nodes) {
        for (auto& p : node.bang_inputs) resolve(p);
        for (auto& p : node.inputs) resolve(p);
        for (auto& p : node.outputs) resolve(p);
        for (auto& p : node.bang_outputs) resolve(p);
        resolve(node.lambda_grab);
        resolve(node.bang_pin);
    }
}

bool GraphInference::propagate_connections(FlowGraph& graph) {
    bool changed = false;
    for (auto& link : graph.links) {
        auto* from_pin = graph.find_pin(link.from_pin);
        auto* to_pin = graph.find_pin(link.to_pin);
        if (from_pin && to_pin && from_pin->resolved_type) {
            if (!to_pin->resolved_type || (to_pin->resolved_type->is_generic && !from_pin->resolved_type->is_generic)) {
                to_pin->resolved_type = from_pin->resolved_type;
                changed = true;
            }
        }
    }
    return changed;
}

bool GraphInference::infer_expr_nodes(FlowGraph& graph) {
    bool changed = false;
    for (auto& node : graph.nodes) {
        bool is_expr = (node.type == "expr" || node.type == "expr!");
        auto* nt = find_node_type(node.type.c_str());

        // Skip nodes that don't have inline expressions:
        // - declarations (decl_type, decl_var, etc.)
        // - type-based nodes where args are type names, not expressions (new, event!)
        // - nodes with no args
        bool needs_type_propagation = (node.type == "dup" || node.type == "select" || node.type == "next" || node.type == "void" || node.type == "discard" || node.type == "str");
        bool has_custom_output = needs_type_propagation ||
            node.type == "append!" || node.type == "erase" || node.type == "erase!" ||
            node.type == "decl_local" || node.type == "call" || node.type == "call!" ||
            node.type == "cast";
        if (!is_expr) {
            if (!nt || nt->is_declaration) continue;
            if (node.type == "new" || node.type == "event!") continue;
            if (node.type == "label") continue;
            if (node.args.empty() && !needs_type_propagation && !has_custom_output) continue;
            if (nt->inputs == 0 && !needs_type_propagation && !has_custom_output) continue;
        }

        // Skip expression parsing for nodes whose args aren't expressions
        bool skip_expr_parse = (node.type == "decl_local" || node.type == "void" || node.type == "cast");

        // Parse expression(s) if not cached.
        // For expr nodes, each space-separated token is a separate expression/output.
        // For non-expr nodes, each token is an inline expression for a descriptor input.
        if (!skip_expr_parse && node.args != node.last_parsed_args) {
            node.parsed_exprs.clear();
            if (!node.args.empty()) {
                auto tokens = tokenize_args(node.args, false);
                for (auto& tok : tokens) {
                    auto result = parse_expression(tok);
                    if (result.root && result.error.empty())
                        node.parsed_exprs.push_back(result.root);
                    else {
                        if (!result.error.empty() && node.error.empty())
                            node.error = "In '" + tok + "': " + result.error;
                        node.parsed_exprs.push_back(nullptr); // placeholder
                    }
                }
            }
            node.last_parsed_args = node.args;
        }

        if (node.parsed_exprs.empty() && !needs_type_propagation && !has_custom_output) continue;

        // Clear expression type cache so inference uses updated pin types
        for (auto& e : node.parsed_exprs) clear_expr_types(e);

        // Build input pin type map
        ctx.input_pin_types.clear();
        for (int i = 0; i < (int)node.inputs.size(); i++) {
            if (node.inputs[i].resolved_type)
                ctx.input_pin_types[i] = node.inputs[i].resolved_type;
        }

        // Run forward inference for each expression → each output pin
        for (int ei = 0; ei < (int)node.parsed_exprs.size(); ei++) {
            auto& expr = node.parsed_exprs[ei];
            if (!expr) continue;

            ctx.errors.clear();
            auto result_type = ctx.infer(expr);

            // Surface inference errors
            if (!ctx.errors.empty() && node.error.empty())
                node.error = ctx.errors[0];

            // Assign to corresponding output pin (skip for nodes with custom output logic)
            if (!has_custom_output && ei < (int)node.outputs.size()) {
                auto& out = node.outputs[ei];
                if (result_type && (!out.resolved_type || out.resolved_type->is_generic)) {
                    if (out.resolved_type != result_type) {
                        out.resolved_type = result_type;
                        changed = true;
                    }
                }
            }

            // Backward pass: resolve generic literals
            if (result_type)
                ctx.resolve_int_literals(expr, result_type);
        }

        // Propagate PinRef resolved types back to input pins
        propagate_pin_ref_types(node, changed);

        // --- Node-specific validation ---

        // --- Passthrough nodes: propagate input type to output ---

        if (node.type == "dup") {
            // dup: output = input type
            // Input can be from a connection OR from inline expression
            TypePtr input_type = nullptr;
            if (!node.inputs.empty() && node.inputs[0].resolved_type)
                input_type = node.inputs[0].resolved_type;
            else if (!node.parsed_exprs.empty() && node.parsed_exprs[0] &&
                     node.parsed_exprs[0]->resolved_type)
                input_type = node.parsed_exprs[0]->resolved_type;
            if (input_type && !node.outputs.empty()) {
                if (!node.outputs[0].resolved_type || node.outputs[0].resolved_type->is_generic) {
                    if (node.outputs[0].resolved_type != input_type) {
                        node.outputs[0].resolved_type = input_type;
                        changed = true;
                    }
                }
            }
        }

        if (node.type == "void") {
            if (!node.outputs.empty() && !node.outputs[0].resolved_type) {
                node.outputs[0].resolved_type = pool.t_void;
                changed = true;
            }
        }

        if (node.type == "str") {
            if (!node.outputs.empty() && !node.outputs[0].resolved_type) {
                node.outputs[0].resolved_type = pool.intern("string");
                changed = true;
            }
        }

        if (node.type == "cast") {
            // Output type is the destination type from args
            if (!node.outputs.empty() && !node.outputs[0].resolved_type && !node.args.empty()) {
                auto dest_type = pool.intern(node.args);
                if (dest_type) {
                    node.outputs[0].resolved_type = dest_type;
                    changed = true;
                }
            }
        }

        if (node.type == "next") {
            // next: input must be an iterator, output = same iterator type
            TypePtr input_type = nullptr;
            if (!node.inputs.empty() && node.inputs[0].resolved_type)
                input_type = node.inputs[0].resolved_type;
            else if (!node.parsed_exprs.empty() && node.parsed_exprs[0] &&
                     node.parsed_exprs[0]->resolved_type)
                input_type = node.parsed_exprs[0]->resolved_type;

            if (input_type && !input_type->is_generic) {
                auto resolved = ctx.resolve_type(input_type);
                if (resolved && resolved->kind != TypeKind::ContainerIterator) {
                    if (node.error.empty())
                        node.error = "next requires an iterator (got " + type_to_string(input_type) + ")";
                }
            }
            // Output = same type as input (advanced iterator)
            if (input_type && !node.outputs.empty()) {
                if (!node.outputs[0].resolved_type || node.outputs[0].resolved_type->is_generic) {
                    if (node.outputs[0].resolved_type != input_type) {
                        node.outputs[0].resolved_type = input_type;
                        changed = true;
                    }
                }
            }
        }

        if (node.type == "decl_local") {
            // decl_local <name> <type>
            // Validate args: must have 2 tokens (name, type)
            auto tokens = tokenize_args(node.args, false);
            if (tokens.size() < 2) {
                if (node.error.empty())
                    node.error = "decl_local requires: name type";
            } else {
                // Name must not start with $
                if (!tokens[0].empty() && tokens[0][0] == '$') {
                    if (node.error.empty())
                        node.error = "Local variable name should not start with $";
                }
                // Type must be valid
                std::string type_str;
                for (size_t i = 1; i < tokens.size(); i++) {
                    if (!type_str.empty()) type_str += " ";
                    type_str += tokens[i];
                }
                TypePtr local_type = pool.intern(type_str);
                std::string err;
                if (!registry.validate_type(type_str, err)) {
                    if (node.error.empty())
                        node.error = "Invalid type: " + err;
                }

                // Set output type to a reference to the declared type
                if (local_type && !node.outputs.empty()) {
                    if (!node.outputs[0].resolved_type || node.outputs[0].resolved_type->is_generic) {
                        auto ref_type = std::make_shared<TypeExpr>(*local_type);
                        ref_type->category = TypeCategory::Reference;
                        node.outputs[0].resolved_type = ref_type;
                        changed = true;
                    }
                }

                // Register in var_types for downstream inference
                ctx.var_types[tokens[0]] = local_type;

                // Validate initial value type compatibility
                // Parse the last token as an expression to get its type
                std::string init_str = tokens.back();
                auto init_parsed = parse_expression(init_str);
                if (init_parsed.root && init_parsed.error.empty()) {
                    ctx.errors.clear();
                    auto init_type = ctx.infer(init_parsed.root);
                    if (init_type && local_type &&
                        !init_type->is_generic && !local_type->is_generic &&
                        !types_compatible(init_type, local_type)) {
                        if (node.error.empty())
                            node.error = "Initial value type " +
                                type_to_string(init_type) +
                                " not compatible with " + type_to_string(local_type);
                    }
                }
            }
        }

        if (node.type == "select") {
            // Resolve types from inline exprs or input pins
            auto get_arg_type = [&](int idx) -> TypePtr {
                if (idx < (int)node.parsed_exprs.size() && node.parsed_exprs[idx] &&
                    node.parsed_exprs[idx]->resolved_type)
                    return node.parsed_exprs[idx]->resolved_type;
                if (idx < (int)node.inputs.size() && node.inputs[idx].resolved_type)
                    return node.inputs[idx].resolved_type;
                return nullptr;
            };

            auto cond_type = get_arg_type(0);
            auto true_type = get_arg_type(1);
            auto false_type = get_arg_type(2);

            // Condition must be bool
            if (cond_type && !cond_type->is_generic && cond_type->kind != TypeKind::Bool) {
                if (node.error.empty())
                    node.error = "select condition must be bool, got " + type_to_string(cond_type);
            }

            // if_true and if_false must be compatible
            if (true_type && false_type &&
                !true_type->is_generic && !false_type->is_generic &&
                !types_compatible(true_type, false_type)) {
                if (node.error.empty())
                    node.error = "select branches must have compatible types: " +
                        type_to_string(true_type) + " vs " + type_to_string(false_type);
            }

            // Output type = whichever branch has a concrete type
            TypePtr result_type = nullptr;
            if (true_type && !true_type->is_generic) result_type = true_type;
            else if (false_type && !false_type->is_generic) result_type = false_type;
            else if (true_type) result_type = true_type;
            else result_type = false_type;

            if (result_type && !node.outputs.empty()) {
                if (!node.outputs[0].resolved_type || node.outputs[0].resolved_type->is_generic) {
                    if (node.outputs[0].resolved_type != result_type) {
                        node.outputs[0].resolved_type = result_type;
                        changed = true;
                    }
                }
            }
        }

        // --- Node-specific validation ---

        if ((node.type == "store!" || node.type == "store") && node.parsed_exprs.size() >= 2) {
            // First arg must be an lvalue
            if (node.parsed_exprs[0] && !is_lvalue(node.parsed_exprs[0])) {
                if (node.error.empty())
                    node.error = "store! target must be assignable (variable, field access, or index)";
            }
            // Check type compatibility between target and value
            if (node.parsed_exprs[0] && node.parsed_exprs[1] &&
                node.parsed_exprs[0]->resolved_type && node.parsed_exprs[1]->resolved_type &&
                !node.parsed_exprs[0]->resolved_type->is_generic &&
                !node.parsed_exprs[1]->resolved_type->is_generic) {
                if (!types_compatible(node.parsed_exprs[1]->resolved_type,
                                     node.parsed_exprs[0]->resolved_type)) {
                    if (node.error.empty())
                        node.error = "store! type mismatch: cannot store " +
                            type_to_string(node.parsed_exprs[1]->resolved_type) +
                            " into " + type_to_string(node.parsed_exprs[0]->resolved_type);
                }
            }
        }

        // For store!/store: if the value comes from as_lambda, set the value pin's
        // resolved_type to the target's function type so lambda validation can run
        if ((node.type == "store!" || node.type == "store") && node.parsed_exprs.size() >= 1) {
            auto& target_expr = node.parsed_exprs[0];
            if (target_expr && target_expr->resolved_type && !target_expr->resolved_type->is_generic) {
                auto target_type = ctx.resolve_type(target_expr->resolved_type);
                if (target_type && target_type->kind == TypeKind::Function) {
                    // Find the value input pin and set its expected type
                    for (auto& inp : node.inputs) {
                        // Check if this pin is connected from an as_lambda
                        for (auto& l : graph.links) {
                            if (l.to_pin == inp.id && l.from_pin.find(".as_lambda") != std::string::npos) {
                                if (!inp.resolved_type || inp.resolved_type->is_generic) {
                                    inp.resolved_type = target_type;
                                    changed = true;
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }

        if ((node.type == "erase" || node.type == "erase!") && node.parsed_exprs.size() >= 2) {
            auto& target_expr = node.parsed_exprs[0];
            auto& key_expr = node.parsed_exprs[1];
            if (target_expr && target_expr->resolved_type && !target_expr->resolved_type->is_generic) {
                auto target_resolved = ctx.resolve_type(target_expr->resolved_type);
                if (target_resolved && target_resolved->kind == TypeKind::Container) {
                    auto key_type = key_expr ? key_expr->resolved_type : nullptr;
                    auto key_resolved = key_type ? ctx.resolve_type(key_type) : nullptr;

                    if (key_resolved && !key_resolved->is_generic) {
                        bool valid = false;
                        // Check if key is a matching iterator type
                        if (key_resolved->kind == TypeKind::ContainerIterator) {
                            // Iterator kind must match container kind
                            static const std::map<ContainerKind, IteratorKind> iter_map = {
                                {ContainerKind::Vector, IteratorKind::Vector},
                                {ContainerKind::List, IteratorKind::List},
                                {ContainerKind::Map, IteratorKind::Map},
                                {ContainerKind::OrderedMap, IteratorKind::OrderedMap},
                                {ContainerKind::Set, IteratorKind::Set},
                                {ContainerKind::OrderedSet, IteratorKind::OrderedSet},
                            };
                            auto it = iter_map.find(target_resolved->container);
                            if (it != iter_map.end() && key_resolved->iterator == it->second)
                                valid = true;
                        }
                        // For map/ordered_map: key type is also valid
                        if (!valid && (target_resolved->container == ContainerKind::Map ||
                                      target_resolved->container == ContainerKind::OrderedMap)) {
                            if (target_resolved->key_type && types_compatible(key_resolved, target_resolved->key_type))
                                valid = true;
                        }
                        // For set/ordered_set: value type is also valid
                        if (!valid && (target_resolved->container == ContainerKind::Set ||
                                      target_resolved->container == ContainerKind::OrderedSet)) {
                            if (target_resolved->value_type && types_compatible(key_resolved, target_resolved->value_type))
                                valid = true;
                        }
                        // For vector: integer index is also valid
                        if (!valid && target_resolved->container == ContainerKind::Vector) {
                            if (key_resolved->kind == TypeKind::Scalar && is_integer(key_resolved))
                                valid = true;
                        }
                        // For list: only iterator is valid (no key-based erase)
                        if (!valid) {
                            if (node.error.empty())
                                node.error = "erase: invalid key/iterator type " +
                                    type_to_string(key_type) + " for " + type_to_string(target_expr->resolved_type);
                        }
                    }
                    // Set output to matching iterator type
                    if (!node.outputs.empty()) {
                        static const std::map<ContainerKind, IteratorKind> iter_map = {
                            {ContainerKind::Vector, IteratorKind::Vector},
                            {ContainerKind::List, IteratorKind::List},
                            {ContainerKind::Map, IteratorKind::Map},
                            {ContainerKind::OrderedMap, IteratorKind::OrderedMap},
                            {ContainerKind::Set, IteratorKind::Set},
                            {ContainerKind::OrderedSet, IteratorKind::OrderedSet},
                        };
                        auto it_kind = iter_map.find(target_resolved->container);
                        if (it_kind != iter_map.end()) {
                            auto iter_type = std::make_shared<TypeExpr>();
                            iter_type->kind = TypeKind::ContainerIterator;
                            iter_type->iterator = it_kind->second;
                            iter_type->value_type = target_resolved->value_type;
                            iter_type->key_type = target_resolved->key_type;
                            if (!node.outputs[0].resolved_type || node.outputs[0].resolved_type->is_generic) {
                                node.outputs[0].resolved_type = iter_type;
                                changed = true;
                            }
                        }
                    }
                } else if (target_resolved) {
                    if (node.error.empty())
                        node.error = "erase: target must be a collection (got " +
                            type_to_string(target_expr->resolved_type) + ")";
                }
            }
            // Target must be an lvalue
            if (target_expr && !is_lvalue(target_expr)) {
                if (node.error.empty())
                    node.error = "erase: target must be assignable";
            }
        }

        if ((node.type == "iterate!" || node.type == "iterate") && node.parsed_exprs.size() >= 1) {
            auto& target_expr = node.parsed_exprs[0];
            if (target_expr && target_expr->resolved_type && !target_expr->resolved_type->is_generic) {
                auto target_resolved = ctx.resolve_type(target_expr->resolved_type);
                TypePtr lambda_param_type = nullptr;

                if (target_resolved && target_resolved->kind == TypeKind::Container) {
                    // Iterable containers: lambda gets ^iterator_type
                    auto iter_type = std::make_shared<TypeExpr>();
                    iter_type->kind = TypeKind::ContainerIterator;
                    iter_type->category = TypeCategory::Iterator;
                    iter_type->value_type = target_resolved->value_type;
                    iter_type->key_type = target_resolved->key_type;
                    switch (target_resolved->container) {
                    case ContainerKind::Vector:     iter_type->iterator = IteratorKind::Vector; break;
                    case ContainerKind::List:       iter_type->iterator = IteratorKind::List; break;
                    case ContainerKind::Set:        iter_type->iterator = IteratorKind::Set; break;
                    case ContainerKind::OrderedSet: iter_type->iterator = IteratorKind::OrderedSet; break;
                    case ContainerKind::Map:        iter_type->iterator = IteratorKind::Map; break;
                    case ContainerKind::OrderedMap: iter_type->iterator = IteratorKind::OrderedMap; break;
                    case ContainerKind::Queue:      iter_type->iterator = IteratorKind::Vector; break; // queue uses vector-like iteration
                    }
                    lambda_param_type = iter_type;
                } else if (target_resolved && (target_resolved->kind == TypeKind::Array || target_resolved->kind == TypeKind::Tensor)) {
                    // Array/tensor: lambda gets &value_type
                    if (target_resolved->value_type) {
                        auto ref_type = std::make_shared<TypeExpr>(*target_resolved->value_type);
                        ref_type->category = TypeCategory::Reference;
                        lambda_param_type = ref_type;
                    }
                } else if (target_resolved && target_resolved->kind == TypeKind::Scalar) {
                    // Scalar: runs once, lambda gets &scalar_type
                    auto ref_type = std::make_shared<TypeExpr>(*target_resolved);
                    ref_type->category = TypeCategory::Reference;
                    lambda_param_type = ref_type;
                } else if (target_resolved) {
                    if (node.error.empty())
                        node.error = "iterate! target must be a collection, array, tensor, or scalar (got " +
                            type_to_string(target_expr->resolved_type) + ")";
                }

                // Set expected function type on the lambda input pin
                // so that lambda resolution can assign parameter types
                if (lambda_param_type) {
                    // Containers return the iterator (to continue from), arrays/scalars return void
                    bool returns_iterator = (target_resolved && target_resolved->kind == TypeKind::Container);
                    auto fn_type = std::make_shared<TypeExpr>();
                    fn_type->kind = TypeKind::Function;
                    fn_type->func_args.push_back({"it", lambda_param_type});
                    fn_type->return_type = returns_iterator ? lambda_param_type : pool.t_void;

                    // Find the lambda input pin (either from inline @0 or remaining descriptor pin)
                    for (auto& inp : node.inputs) {
                        if (inp.direction == FlowPin::Lambda) {
                            if (!inp.resolved_type || inp.resolved_type->is_generic) {
                                inp.resolved_type = fn_type;
                                changed = true;
                            }
                            break;
                        }
                    }
                }
            }
            // Target must be an lvalue
            if (target_expr && !is_lvalue(target_expr)) {
                if (node.error.empty())
                    node.error = "iterate! target must be a variable";
            }
        }

        if ((node.type == "lock" || node.type == "lock!") && node.parsed_exprs.size() >= 1) {
            auto& mutex_expr = node.parsed_exprs[0];
            if (mutex_expr && mutex_expr->resolved_type && !mutex_expr->resolved_type->is_generic) {
                auto mutex_resolved = ctx.resolve_type(mutex_expr->resolved_type);
                // Validate: first arg must be mutex (auto-decays to reference)
                if (!mutex_resolved || mutex_resolved->kind != TypeKind::Mutex) {
                    if (node.error.empty())
                        node.error = std::string(node.type) + ": first argument must be a mutex (got " +
                            type_to_string(mutex_expr->resolved_type) + ")";
                }

                // Find the lambda root and collect its params to determine the function signature
                FlowNode* lambda_root = nullptr;
                for (auto& inp : node.inputs) {
                    if (inp.direction != FlowPin::Lambda) continue;
                    for (auto& link : graph.links) {
                        if (link.to_pin == inp.id) {
                            for (auto& src : graph.nodes) {
                                if (src.lambda_grab.id == link.from_pin) {
                                    lambda_root = &src;
                                    break;
                                }
                            }
                            break;
                        }
                    }
                    if (lambda_root) break;
                }

                // Collect lambda params to determine how many extra inputs lock needs
                std::vector<FlowPin*> lambda_params;
                if (lambda_root) {
                    std::set<std::string> visited;
                    collect_lambda_params(graph, *lambda_root, lambda_params, visited);
                }

                // Build expected function type from lambda params
                auto fn_type = std::make_shared<TypeExpr>();
                fn_type->kind = TypeKind::Function;
                fn_type->return_type = pool.t_unknown;
                // Add params — these will be forwarded through extra lock inputs
                for (auto* p : lambda_params) {
                    fn_type->func_args.push_back({"", p->resolved_type ? p->resolved_type : pool.t_unknown});
                }

                // Dynamically add data input pins for lambda params (after mutex + fn)
                // Count existing non-Lambda, non-mutex data inputs
                int existing_extra = 0;
                for (auto& inp : node.inputs) {
                    if (inp.direction != FlowPin::Lambda && inp.name != "mutex")
                        existing_extra++;
                }
                int needed_extra = (int)lambda_params.size();
                if (needed_extra != existing_extra) {
                    // Remove old extra pins and add new ones
                    std::vector<FlowPin> new_inputs;
                    for (auto& inp : node.inputs) {
                        if (inp.direction == FlowPin::Lambda || inp.name == "mutex")
                            new_inputs.push_back(inp);
                    }
                    for (int pi = 0; pi < needed_extra; pi++) {
                        std::string pname = "arg" + std::to_string(pi);
                        std::string ptype = lambda_params[pi]->resolved_type ?
                            type_to_string(lambda_params[pi]->resolved_type) : "value";
                        new_inputs.push_back({"", pname, ptype, nullptr, FlowPin::Input});
                    }
                    node.inputs = std::move(new_inputs);
                    node.rebuild_pin_ids();
                    changed = true;
                }
                // Set types on extra input pins
                for (int pi = 0; pi < needed_extra; pi++) {
                    for (auto& inp : node.inputs) {
                        if (inp.name == "arg" + std::to_string(pi) && lambda_params[pi]->resolved_type) {
                            if (!inp.resolved_type || inp.resolved_type->is_generic) {
                                inp.resolved_type = lambda_params[pi]->resolved_type;
                                changed = true;
                            }
                        }
                    }
                }

                // Set expected type on lambda input pin
                for (auto& inp : node.inputs) {
                    if (inp.direction == FlowPin::Lambda) {
                        if (!inp.resolved_type || inp.resolved_type->is_generic) {
                            inp.resolved_type = fn_type;
                            changed = true;
                        }

                        // Resolve return type from the lambda root's output
                        TypePtr lambda_ret_type;
                        if (lambda_root && !lambda_root->outputs.empty() &&
                            lambda_root->outputs[0].resolved_type &&
                            !lambda_root->outputs[0].resolved_type->is_generic) {
                            lambda_ret_type = lambda_root->outputs[0].resolved_type;
                        }

                        if (lambda_ret_type && lambda_ret_type->kind != TypeKind::Void) {
                            if (node.outputs.empty()) {
                                node.outputs.push_back({"", "result", "", nullptr, FlowPin::Output});
                                node.rebuild_pin_ids();
                            }
                            if (!node.outputs[0].resolved_type || node.outputs[0].resolved_type->is_generic) {
                                node.outputs[0].resolved_type = lambda_ret_type;
                                changed = true;
                            }
                        } else if (lambda_ret_type && lambda_ret_type->kind == TypeKind::Void) {
                            if (!node.outputs.empty()) {
                                node.outputs.clear();
                                node.rebuild_pin_ids();
                            }
                        }
                        break;
                    }
                }
            }
            // Mutex must be an lvalue
            if (mutex_expr && !is_lvalue(mutex_expr)) {
                if (node.error.empty())
                    node.error = std::string(node.type) + ": mutex must be a variable reference";
            }
        }

        if ((node.type == "call" || node.type == "call!") && node.parsed_exprs.size() >= 1) {
            // First arg is the function reference
            auto& fn_expr = node.parsed_exprs[0];
            if (fn_expr && fn_expr->resolved_type && !fn_expr->resolved_type->is_generic) {
                auto fn_resolved = ctx.resolve_type(fn_expr->resolved_type);
                if (!fn_resolved || fn_resolved->kind != TypeKind::Function) {
                    if (node.error.empty())
                        node.error = std::string(node.type) + ": first argument must be a function (got " +
                            type_to_string(fn_expr->resolved_type) + ")";
                } else {
                    // Set input pin types from function args.
                    // $N ref pins map to specific inline arg positions, not to fn_arg[0..N].
                    // Build a map from pin name to the fn arg index it corresponds to.
                    {
                        auto tokens = tokenize_args(node.args, false);
                        for (int ai = 0; ai < (int)tokens.size() - 1 && ai < (int)fn_resolved->func_args.size(); ai++) {
                            auto& tok = tokens[ai + 1]; // skip fn name
                            // Check for bare $N ref
                            if (tok.size() >= 2 && tok[0] == '$' && tok[1] >= '0' && tok[1] <= '9') {
                                size_t end = 1;
                                while (end < tok.size() && tok[end] >= '0' && tok[end] <= '9') end++;
                                if (end == tok.size()) {
                                    // Bare $N — find the pin and set its type
                                    std::string pin_name = tok.substr(1);
                                    for (auto& p : node.inputs) {
                                        if (p.name == pin_name && fn_resolved->func_args[ai].type) {
                                            if (!p.resolved_type || p.resolved_type->is_generic) {
                                                p.resolved_type = fn_resolved->func_args[ai].type;
                                                changed = true;
                                            }
                                        }
                                    }
                                }
                                // $N.field etc.: don't set pin type from fn arg
                            }
                        }
                        // Also set types for remaining descriptor pins (after inline args)
                        int num_inline = (int)tokens.size() - 1;
                        int slot_count = (int)scan_slots(node.args).slots.size();
                        for (int pi = slot_count; pi < (int)node.inputs.size(); pi++) {
                            int fn_arg_idx = num_inline + (pi - slot_count);
                            if (fn_arg_idx < (int)fn_resolved->func_args.size() && fn_resolved->func_args[fn_arg_idx].type) {
                                if (!node.inputs[pi].resolved_type || node.inputs[pi].resolved_type->is_generic) {
                                    node.inputs[pi].resolved_type = fn_resolved->func_args[fn_arg_idx].type;
                                    changed = true;
                                }
                            }
                        }
                    }
                    // Set output type from return type
                    if (fn_resolved->return_type && fn_resolved->return_type->kind != TypeKind::Void) {
                        if (!node.outputs.empty()) {
                            if (!node.outputs[0].resolved_type || node.outputs[0].resolved_type->is_generic) {
                                node.outputs[0].resolved_type = fn_resolved->return_type;
                                changed = true;
                            }
                        }
                    }
                    // Check too many inline args.
                    // num_inline_args already includes $N pin refs (which are also
                    // represented as input pins), so don't add node.inputs.size()
                    // for those — subtract them to avoid double-counting.
                    int num_inline_args = (int)node.parsed_exprs.size() - 1; // exclude fn ref
                    int expected_args = (int)fn_resolved->func_args.size();
                    int total_args = num_inline_args + (int)node.inputs.size() - (int)scan_slots(node.args).slots.size();
                    if (total_args > expected_args) {
                        if (node.error.empty())
                            node.error = std::string(node.type) + ": too many arguments (" +
                                std::to_string(total_args) + " given, " +
                                std::to_string(expected_args) + " expected)";
                    }

                    // Validate argument types from inline expressions
                    for (int j = 1; j < (int)node.parsed_exprs.size() && (j-1) < (int)fn_resolved->func_args.size(); j++) {
                        auto& arg_expr = node.parsed_exprs[j];
                        if (arg_expr && arg_expr->resolved_type && !arg_expr->resolved_type->is_generic &&
                            fn_resolved->func_args[j-1].type && !fn_resolved->func_args[j-1].type->is_generic) {
                            if (!types_compatible(arg_expr->resolved_type, fn_resolved->func_args[j-1].type)) {
                                if (node.error.empty())
                                    node.error = std::string(node.type) + ": argument '" +
                                        fn_resolved->func_args[j-1].name + "' type mismatch: " +
                                        type_to_string(arg_expr->resolved_type) + " vs expected " +
                                        type_to_string(fn_resolved->func_args[j-1].type);
                            }
                        }
                    }
                }
            }
        }

        if ((node.type == "append!" || node.type == "append") && node.parsed_exprs.size() >= 1) {
            // First arg is the target collection
            auto& target_expr = node.parsed_exprs[0];
            if (target_expr && target_expr->resolved_type && !target_expr->resolved_type->is_generic) {
                auto target_resolved = ctx.resolve_type(target_expr->resolved_type);
                bool valid_target = false;
                TypePtr elem_type;
                if (target_resolved && target_resolved->kind == TypeKind::Container) {
                    switch (target_resolved->container) {
                    case ContainerKind::Vector:
                    case ContainerKind::List:
                    case ContainerKind::Queue:
                        valid_target = true;
                        elem_type = target_resolved->value_type;
                        break;
                    default: break;
                    }
                }
                if (!valid_target) {
                    if (node.error.empty())
                        node.error = "append! target must be a vector, list, or queue (got " +
                            type_to_string(target_expr->resolved_type) + ")";
                }
                // Set output type to iterator of the collection
                if (valid_target && !node.outputs.empty()) {
                    auto iter_type = std::make_shared<TypeExpr>();
                    iter_type->kind = TypeKind::ContainerIterator;
                    iter_type->value_type = target_resolved->value_type;
                    switch (target_resolved->container) {
                    case ContainerKind::Vector: iter_type->iterator = IteratorKind::Vector; break;
                    case ContainerKind::List:   iter_type->iterator = IteratorKind::List; break;
                    case ContainerKind::Queue:  iter_type->iterator = IteratorKind::Vector; break;
                    default: break;
                    }
                    // Always set — append output is always an iterator, not the container type
                    if (!node.outputs[0].resolved_type ||
                        node.outputs[0].resolved_type->kind != TypeKind::ContainerIterator) {
                        node.outputs[0].resolved_type = iter_type;
                        changed = true;
                    }
                }
                // Check value type compatibility
                if (valid_target && elem_type && node.parsed_exprs.size() >= 2) {
                    auto& value_expr = node.parsed_exprs[1];
                    if (value_expr && value_expr->resolved_type &&
                        !value_expr->resolved_type->is_generic && !elem_type->is_generic) {
                        if (!types_compatible(value_expr->resolved_type, elem_type)) {
                            if (node.error.empty())
                                node.error = "append! type mismatch: cannot append " +
                                    type_to_string(value_expr->resolved_type) +
                                    " to " + type_to_string(target_expr->resolved_type) +
                                    " (element type: " + type_to_string(elem_type) + ")";
                        }
                    }
                }
            }
            // Target must be an lvalue
            if (target_expr && !is_lvalue(target_expr)) {
                if (node.error.empty())
                    node.error = "append! target must be assignable (variable or indexed variable)";
            }
        }

        if (node.type == "select!") {
            // Condition input must be bool
            auto get_cond_type = [&]() -> TypePtr {
                if (!node.parsed_exprs.empty() && node.parsed_exprs[0] &&
                    node.parsed_exprs[0]->resolved_type)
                    return node.parsed_exprs[0]->resolved_type;
                if (!node.inputs.empty() && node.inputs[0].resolved_type)
                    return node.inputs[0].resolved_type;
                return nullptr;
            };
            auto cond_type = get_cond_type();
            if (cond_type && !cond_type->is_generic) {
                auto resolved = ctx.resolve_type(cond_type);
                if (!resolved || resolved->kind != TypeKind::Bool) {
                    if (node.error.empty())
                        node.error = "select! condition must be bool (got " + type_to_string(cond_type) + ")";
                }
            }
        }

        if (node.type == "output_mix!") {
            // Input must be f32
            auto get_input_type = [&]() -> TypePtr {
                if (!node.parsed_exprs.empty() && node.parsed_exprs[0] &&
                    node.parsed_exprs[0]->resolved_type)
                    return node.parsed_exprs[0]->resolved_type;
                if (!node.inputs.empty() && node.inputs[0].resolved_type)
                    return node.inputs[0].resolved_type;
                return nullptr;
            };
            auto input_type = get_input_type();
            if (input_type && !input_type->is_generic) {
                auto resolved = ctx.resolve_type(input_type);
                if (!resolved || resolved->kind != TypeKind::Scalar || resolved->scalar != ScalarType::F32) {
                    if (node.error.empty())
                        node.error = "output_mix! input must be f32 (got " + type_to_string(input_type) + ")";
                }
            }
        }
    }
    return changed;
}

void GraphInference::propagate_pin_ref_types(FlowNode& node, bool& changed) {
    if (node.parsed_exprs.empty()) return;
    std::function<void(const ExprPtr&)> walk = [&](const ExprPtr& e) {
        if (!e) return;
        if (e->kind == ExprKind::PinRef && e->pin_ref.index >= 0 &&
            e->pin_ref.index < (int)node.inputs.size()) {
            auto& pin = node.inputs[e->pin_ref.index];
            if (e->resolved_type && !e->resolved_type->is_generic &&
                (!pin.resolved_type || pin.resolved_type->is_generic)) {
                pin.resolved_type = e->resolved_type;
                changed = true;
            }
        }
        for (auto& child : e->children) walk(child);
    };
    for (auto& expr : node.parsed_exprs) walk(expr);
}

bool GraphInference::resolve_lambdas(FlowGraph& graph) {
    bool changed = false;
    for (auto& node : graph.nodes) {
        for (auto& link : graph.links) {
            if (link.from_pin != node.lambda_grab.id) continue;
            auto* target_pin = graph.find_pin(link.to_pin);
            if (!target_pin || !target_pin->resolved_type) continue;

            // Resolve through Named type aliases
            auto expected = target_pin->resolved_type;
            while (expected && expected->kind == TypeKind::Named) {
                auto it = registry.parsed.find(expected->named_ref);
                if (it != registry.parsed.end() && it->second.get() != expected.get())
                    expected = it->second;
                else break;
            }
            if (!expected || expected->kind != TypeKind::Function) continue;

            // Recursively collect unconnected input pins (lambda parameters)
            std::vector<FlowPin*> params;
            std::set<std::string> visited;
            collect_lambda_params(graph, node, params, visited);

            // Assign parameter types
            for (size_t i = 0; i < params.size() && i < expected->func_args.size(); i++) {
                if (!params[i]->resolved_type && expected->func_args[i].type) {
                    params[i]->resolved_type = expected->func_args[i].type;
                    changed = true;
                }
            }

            // Assign return type
            if (expected->return_type && expected->return_type->kind != TypeKind::Void) {
                for (auto& out : node.outputs) {
                    if (!out.resolved_type) {
                        out.resolved_type = expected->return_type;
                        changed = true;
                    }
                }
            }

            // For stored lambdas (target is store!/store), use deep collection
            // that crosses Lambda boundaries to find all params in the full subgraph
            // Validate (store error on the link, not the node)
            validate_lambda(node, params, expected, link);
            break; // only process the first valid lambda connection
        }
    }
    return changed;
}

FlowNode* GraphInference::find_node_by_pin(FlowGraph& graph, const std::string& pin_id) {
    auto dot = pin_id.find('.');
    if (dot == std::string::npos) return nullptr;
    std::string guid = pin_id.substr(0, dot);
    for (auto& n : graph.nodes)
        if (n.guid == guid) return &n;
    return nullptr;
}

void GraphInference::follow_bang_chain(FlowGraph& graph, const std::string& from_pin_id,
                      std::vector<FlowPin*>& params, std::set<std::string>& visited) {
    for (auto& l : graph.links) {
        if (l.from_pin != from_pin_id) continue;
        auto* target_node = find_node_by_pin(graph, l.to_pin);
        if (target_node) collect_lambda_params(graph, *target_node, params, visited);
    }
}

void GraphInference::collect_lambda_params(FlowGraph& graph, FlowNode& node,
                           std::vector<FlowPin*>& params, std::set<std::string>& visited) {
    if (visited.count(node.guid)) return;
    visited.insert(node.guid);

    // 1. Data inputs: recurse into connected sources, collect unconnected as params
    //    Skip Lambda inputs — they define inner lambda boundaries
    for (auto& inp : node.inputs) {
        if (inp.direction == FlowPin::Lambda) continue;
        std::string source_pin_id;
        for (auto& l : graph.links) {
            if (l.to_pin == inp.id) { source_pin_id = l.from_pin; break; }
        }
        if (source_pin_id.empty()) {
            params.push_back(&inp);
        } else {
            // Don't recurse through as_lambda (LambdaGrab) pins — they are lambda boundaries
            auto* src_node = find_node_by_pin(graph, source_pin_id);
            if (src_node) {
                bool is_lambda_grab = (source_pin_id == src_node->lambda_grab.id);
                if (!is_lambda_grab)
                    collect_lambda_params(graph, *src_node, params, visited);
            }
        }
    }

    // 2. Side bang (post_bang): follow chain to downstream nodes
    follow_bang_chain(graph, node.bang_pin.id, params, visited);

    // 3. Output bangs (left to right): follow each bang output's chain
    for (auto& bout : node.bang_outputs) {
        follow_bang_chain(graph, bout.id, params, visited);
    }
}

void GraphInference::validate_lambda(FlowNode& node, const std::vector<FlowPin*>& params,
                     const TypePtr& expected, FlowLink& link) {
    // Build the actual lambda type from collected params and outputs
    auto lambda_type = std::make_shared<TypeExpr>();
    lambda_type->kind = TypeKind::Function;
    for (auto* p : params)
        lambda_type->func_args.push_back({"", p->resolved_type});
    if (!node.outputs.empty() && node.outputs[0].resolved_type)
        lambda_type->return_type = node.outputs[0].resolved_type;
    else
        lambda_type->return_type = pool.t_void;
    node.lambda_grab.resolved_type = lambda_type;

    // Validate parameter count.
    // If params > expected, the extras are captures from an outer lambda scope — not an error.
    // If params < expected, it's a genuine mismatch.
    if (params.size() < expected->func_args.size()) {
        link.error = "Lambda has " + std::to_string(params.size()) +
            " parameter(s), expected " + std::to_string(expected->func_args.size());
        return;
    }
    for (size_t i = 0; i < expected->func_args.size(); i++) {
        if (params[i]->resolved_type && expected->func_args[i].type &&
            !params[i]->resolved_type->is_generic && !expected->func_args[i].type->is_generic &&
            !types_compatible(params[i]->resolved_type, expected->func_args[i].type)) {
            link.error = "Lambda param " + std::to_string(i) + " type mismatch: " +
                type_to_string(params[i]->resolved_type) + " vs expected " +
                type_to_string(expected->func_args[i].type);
            return;
        }
    }
    if (lambda_type->return_type && expected->return_type &&
        !lambda_type->return_type->is_generic && !expected->return_type->is_generic &&
        !types_compatible(lambda_type->return_type, expected->return_type)) {
        link.error = "Lambda return type mismatch: " +
            type_to_string(lambda_type->return_type) + " vs expected " +
            type_to_string(expected->return_type);
    }
}
