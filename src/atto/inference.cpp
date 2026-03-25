#include "inference.h"
#include "type_utils.h"

std::vector<std::string> GraphInference::run(FlowGraph& graph) {
    std::vector<std::string> all_errors;

    // Phase 0: Wire symbol table into inference context
    ctx.symbol_table = &symbol_table;

    // Phase 1: Clear all resolved types
    clear_all(graph);

    // Phase 1.5: Clear declaration entries from symbol table (keep builtins)
    symbol_table.clear_declarations();

    // Phase 2: Resolve pin types from type_name strings
    resolve_pin_type_names(graph);

    // Phase 5: Fixed-point propagation
    // Build initial index (pin pointers are stable via unique_ptr)
    idx.rebuild(graph);

    for (int iter = 0; iter < 10; iter++) {
        bool changed = false;

        // 5.1: Propagate across connections
        changed |= propagate_connections(graph);

        // 5.2: Infer expression nodes (may modify pin vectors)
        changed |= infer_expr_nodes(graph);

        // Rebuild index if pins were added/renamed (IDs may have changed)
        if (changed) idx.rebuild(graph);

        // 5.3: Resolve lambda types
        changed |= resolve_lambdas(graph);

        if (!changed) break;
    }

    // Phase 6: Post-inference fixup — insert Deref nodes in expressions where
    // iterator args are passed to non-iterator params (may have been missed during
    // the fixed-point loop if types weren't resolved yet)
    fixup_expr_derefs(graph);

    // Phase 6b: Insert deref shadow nodes where iterators flow into non-iterator pins
    insert_deref_nodes(graph);

    // Phase 7: Pre-compute resolved data for codegen
    precompute_resolved_data(graph);

    // Phase 7: Check link type compatibility
    for (auto& link : graph.links) {
        if (!link.error.empty()) continue; // already has an error from lambda validation
        if (link.from && link.to &&
            link.from->resolved_type && link.to->resolved_type &&
            !link.from->resolved_type->is_generic && !link.to->resolved_type->is_generic &&
            !types_compatible(link.from->resolved_type, link.to->resolved_type)) {
            link.error = "Type mismatch: " +
                type_to_string(link.from->resolved_type) + " vs " +
                type_to_string(link.to->resolved_type);
        }
    }

    // Validate multi-connection pins (BangTrigger and Lambda)
    {
        // Count incoming links per to_pin
        std::unordered_map<std::string, int> to_pin_count;
        for (auto& link : graph.links)
            to_pin_count[link.to_pin]++;

        for (auto& link : graph.links) {
            if (!link.error.empty()) continue;
            if (to_pin_count[link.to_pin] <= 1) continue;

            // Multiple connections to this pin — check if allowed
            if (link.to && link.to->direction == FlowPin::BangTrigger) {
                // BangTrigger: allowed if owning node has no connected data inputs
                if (link.to_node) {
                    bool has_captures = false;
                    for (auto& inp : link.to_node->inputs) {
                        if (idx.source_pin(inp.get())) {
                            has_captures = true;
                            break;
                        }
                    }
                    if (has_captures)
                        link.error = "Cannot share trigger: node has captured inputs";
                }
            } else if (link.to && link.to->direction == FlowPin::Lambda) {
                // Lambda: allowed if lambda root has no captures
                if (link.to_node) {
                    // Find the lambda root (source node connected via as_lambda)
                    FlowNode* lambda_root = nullptr;
                    auto* src_pin = idx.source_pin(link.to);
                    if (src_pin && src_pin->direction == FlowPin::LambdaGrab) {
                        lambda_root = idx.source_node(link.to);
                    }
                    if (lambda_root) {
                        // Check if any inputs in the lambda subgraph are connected (captures)
                        std::set<std::string> visited;
                        std::vector<FlowPin*> params;
                        collect_lambda_params(graph, *lambda_root, params, visited);
                        // If ALL inputs in the subgraph are lambda params (unconnected), no captures
                        // But we need to check if there are CONNECTED inputs that aren't params
                        bool has_captures = false;
                        for (auto& n_guid : visited) {
                            for (auto& node : graph.nodes) {
                                if (node.guid != n_guid) continue;
                                for (auto& inp : node.inputs) {
                                    if (inp->direction == FlowPin::Lambda) continue;
                                    if (idx.source_pin(inp.get())) {
                                        // Connected input — check if it's from outside the lambda subgraph
                                        auto* src_node = idx.source_node(inp.get());
                                        if (src_node && !visited.count(src_node->guid)) {
                                            has_captures = true;
                                        }
                                    }
                                }
                            }
                        }
                        if (has_captures)
                            link.error = "Cannot share lambda: has captured inputs";
                    }
                }
            } else if (link.to && link.to->direction != FlowPin::BangTrigger && link.to->direction != FlowPin::Lambda) {
                // Other pin types: single connection only
                link.error = "Pin accepts only one connection";
            }
        }
    }

    // Check for unconnected required inputs on nodes in the execution flow
    for (auto& node : graph.nodes) {
        if (!node.error.empty() || node.shadow) continue;
        auto* nt = find_node_type(node.type_id);
        if (!nt || nt->is_declaration) continue;

        // Skip nodes not in any execution flow
        bool in_flow = false;
        for (auto& t : node.triggers)
            if (idx.source_pin(t.get())) { in_flow = true; break; }
        // Lambda roots have their inputs validated by the lambda type check,
        // not here — unconnected inputs are lambda parameters, not errors.
        {
            bool is_lambda_root = false;
            for (auto& link : graph.links)
                if (link.from_pin == node.lambda_grab.id) { is_lambda_root = true; break; }
            if (is_lambda_root) continue;
        }
        // Event nodes are always in flow
        if (nt->is_event) in_flow = true;
        // Data-dependency nodes: if any output feeds another node, this node is
        // consumed and should be checked — but only if it has at least one
        // connected descriptor input. Nodes with ALL inputs unconnected are
        // lambda parameter entry points, not missing-connection bugs.
        if (!in_flow) {
            bool has_any_connected_input = false;
            int desc_count = std::min((int)node.inputs.size(), nt->inputs);
            for (int i = 0; i < desc_count; i++) {
                if (node.inputs[i]->direction == FlowPin::Lambda) continue;
                if (idx.source_pin(node.inputs[i].get()) ||
                    (i < (int)node.parsed_exprs.size() && node.parsed_exprs[i]))
                    { has_any_connected_input = true; break; }
            }
            if (has_any_connected_input) {
                for (auto& out : node.outputs) {
                    for (auto& link : graph.links)
                        if (link.from_pin == out->id) { in_flow = true; break; }
                    if (in_flow) break;
                }
            }
        }
        if (!in_flow) continue;

        // Only check descriptor-defined inputs (up to nt->inputs count).
        // Pins beyond that are dynamically-added lambda parameters whose
        // connectivity is validated by the lambda type check instead.
        int check_count = std::min((int)node.inputs.size(), nt->inputs);
        for (int i = 0; i < check_count; i++) {
            auto& pin = node.inputs[i];
            if (pin->direction == FlowPin::Lambda) continue;

            bool has_source = false;
            if (i < (int)node.parsed_exprs.size() && node.parsed_exprs[i])
                has_source = true;
            else if (idx.source_pin(pin.get()))
                has_source = true;

            if (!has_source) {
                node.error = "Input '" + pin->name + "' is not connected";
                break;
            }
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
        for (auto& p : node.inputs) p->resolved_type = nullptr;
        for (auto& p : node.outputs) p->resolved_type = nullptr;
        for (auto& p : node.triggers) p->resolved_type = nullptr;
        for (auto& p : node.nexts) p->resolved_type = nullptr;
        node.lambda_grab.resolved_type = nullptr;
        node.bang_pin.resolved_type = nullptr;
        for (auto& e : node.parsed_exprs) clear_expr_types(e);
        node.error.clear();
    }
    for (auto& link : graph.links) link.error.clear();
}

void GraphInference::resolve_pin_type_names(FlowGraph& graph) {
    auto resolve = [&](FlowPin& p) {
        if (p.resolved_type) return; // already resolved (e.g. set directly by type_utils)
        // Bang and lambda pins don't need type resolution from type_name
        if (p.direction == FlowPin::BangTrigger || p.direction == FlowPin::BangNext) {
            p.resolved_type = pool.t_bang;
            return;
        }
        if (p.direction == FlowPin::Lambda || p.direction == FlowPin::LambdaGrab) return;
        // For data pins: resolve type_name string if it has a real type (not "value" placeholder)
        if (p.type_name.empty() || p.type_name == "value") return;
        p.resolved_type = pool.intern(p.type_name);
    };
    for (auto& node : graph.nodes) {
        for (auto& p : node.triggers) resolve(*p);
        for (auto& p : node.inputs) resolve(*p);
        for (auto& p : node.outputs) resolve(*p);
        for (auto& p : node.nexts) resolve(*p);
        resolve(node.lambda_grab);
        resolve(node.bang_pin);
    }
}

bool GraphInference::propagate_connections(FlowGraph& graph) {
    bool changed = false;
    for (auto& link : graph.links) {
        if (link.from && link.to && link.from->resolved_type) {
            if (!link.to->resolved_type || (link.to->resolved_type->is_generic && !link.from->resolved_type->is_generic)) {
                // Symbols decay when flowing through connections
                link.to->resolved_type = decay_symbol(link.from->resolved_type);
                changed = true;
            }
        }
    }
    return changed;
}

bool GraphInference::infer_expr_nodes(FlowGraph& graph) {
    bool changed = false;
    for (auto& node : graph.nodes) {
        bool is_expr = is_any_of(node.type_id, NodeTypeID::Expr, NodeTypeID::ExprBang);
        auto* nt = find_node_type(node.type_id);

        // Skip nodes that don't have inline expressions:
        // - declarations (decl_type, decl_var, etc.)
        // - type-based nodes where args are type names, not expressions (new, event!)
        // - nodes with no args
        bool needs_type_propagation = is_any_of(node.type_id, NodeTypeID::Dup, NodeTypeID::Select, NodeTypeID::Next, NodeTypeID::Void, NodeTypeID::Discard, NodeTypeID::Str);
        bool has_custom_output = needs_type_propagation ||
            is_any_of(node.type_id, NodeTypeID::AppendBang, NodeTypeID::Erase, NodeTypeID::EraseBang,
            NodeTypeID::Call, NodeTypeID::CallBang,
            NodeTypeID::Cast);
        if (!is_expr) {
            if (!nt) continue;
            // Declaration nodes: skip if no parsed_exprs to infer
            if (nt->is_declaration) {
                if (node.parsed_exprs.empty()) continue;
                // Fall through to expression inference for validation
            } else {
                if (is_any_of(node.type_id, NodeTypeID::New, NodeTypeID::EventBang)) continue;
                if (node.type_id == NodeTypeID::Label) continue;
                if (node.args.empty() && !needs_type_propagation && !has_custom_output) continue;
                if (nt->inputs == 0 && !needs_type_propagation && !has_custom_output) continue;
            }
        }

        // Skip expression parsing for nodes whose args aren't expressions
        // Expressions are pre-parsed at load time via FlowNode::parse_args().
        // No runtime parsing needed here.

        if (node.parsed_exprs.empty() && !needs_type_propagation && !has_custom_output) continue;

        // Clear expression type cache so inference uses updated pin types
        for (auto& e : node.parsed_exprs) clear_expr_types(e);

        // Build input pin type map
        ctx.input_pin_types.clear();
        for (int i = 0; i < (int)node.inputs.size(); i++) {
            if (node.inputs[i]->resolved_type)
                ctx.input_pin_types[i] = node.inputs[i]->resolved_type;
        }

        // Run forward inference for each expression → each output pin
        for (int ei = 0; ei < (int)node.parsed_exprs.size(); ei++) {
            auto& expr = node.parsed_exprs[ei];
            if (!expr) continue;

            ctx.errors.clear();
            auto result_type = ctx.infer(expr);

            // For non-expr nodes, decay symbols in expression types —
            // symbols are first-class in expr outputs but decay on use in other nodes
            if (!is_expr && result_type) {
                result_type = decay_symbol(result_type);
                // Also decay in the expression tree so downstream reads see decayed types
                std::function<void(const ExprPtr&)> decay_tree = [&](const ExprPtr& e) {
                    if (!e) return;
                    if (e->resolved_type) e->resolved_type = decay_symbol(e->resolved_type);
                    for (auto& c : e->children) decay_tree(c);
                };
                decay_tree(expr);
            }

            // Surface inference errors
            if (!ctx.errors.empty() && node.error.empty())
                node.error = ctx.errors[0];

            // Assign to corresponding output pin (skip for nodes with custom output logic)
            if (!has_custom_output && ei < (int)node.outputs.size()) {
                auto& out = *node.outputs[ei];
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

        // For store!/store: backpropagate target type to value expression
        // so that e.g. rand(200,12000) stored into f32 field resolves args as f32.
        // TODO: This backpropagation pattern should generalize to all assignment-like
        // contexts (e.g. struct field init in new, function call args), not just store.
        if (is_any_of(node.type_id, NodeTypeID::StoreBang, NodeTypeID::Store) && node.parsed_exprs.size() >= 2) {
            auto& target_expr = node.parsed_exprs[0];
            auto& value_expr = node.parsed_exprs[1];
            if (target_expr && value_expr && target_expr->resolved_type &&
                !target_expr->resolved_type->is_generic) {
                ctx.resolve_int_literals(value_expr, target_expr->resolved_type);
                // Re-infer value to pick up resolved literal types
                ctx.errors.clear();
                auto new_type = ctx.infer(value_expr);
                if (new_type && !new_type->is_generic && value_expr->resolved_type != new_type) {
                    value_expr->resolved_type = new_type;
                    changed = true;
                }
            }
        }

        // Propagate PinRef resolved types back to input pins
        propagate_pin_ref_types(node, changed);

        // --- Node-specific validation ---

        // --- Passthrough nodes: propagate input type to output ---

        if (node.type_id == NodeTypeID::Dup) {
            // dup: output = input type (decay symbols — dup is a value passthrough)
            TypePtr input_type = nullptr;
            if (!node.inputs.empty() && node.inputs[0]->resolved_type)
                input_type = decay_symbol(node.inputs[0]->resolved_type);
            else if (!node.parsed_exprs.empty() && node.parsed_exprs[0] &&
                     node.parsed_exprs[0]->resolved_type)
                input_type = decay_symbol(node.parsed_exprs[0]->resolved_type);
            if (input_type && !node.outputs.empty()) {
                if (!node.outputs[0]->resolved_type || node.outputs[0]->resolved_type->is_generic) {
                    if (node.outputs[0]->resolved_type != input_type) {
                        node.outputs[0]->resolved_type = input_type;
                        changed = true;
                    }
                }
            }
        }

        if (node.type_id == NodeTypeID::Void) {
            if (!node.outputs.empty() && !node.outputs[0]->resolved_type) {
                node.outputs[0]->resolved_type = pool.t_void;
                changed = true;
            }
        }

        if (node.type_id == NodeTypeID::Str) {
            if (!node.outputs.empty() && !node.outputs[0]->resolved_type) {
                node.outputs[0]->resolved_type = pool.intern("string");
                changed = true;
            }
        }

        if (node.type_id == NodeTypeID::Cast) {
            // Output type is the destination type from args
            if (!node.outputs.empty() && !node.outputs[0]->resolved_type && !node.args.empty()) {
                auto dest_type = pool.intern(node.args);
                if (dest_type) {
                    node.outputs[0]->resolved_type = dest_type;
                    changed = true;
                }
            }
        }

        if (node.type_id == NodeTypeID::Next) {
            // next: input must be an iterator, output = same iterator type
            TypePtr input_type = nullptr;
            if (!node.inputs.empty() && node.inputs[0]->resolved_type)
                input_type = node.inputs[0]->resolved_type;
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
                if (!node.outputs[0]->resolved_type || node.outputs[0]->resolved_type->is_generic) {
                    if (node.outputs[0]->resolved_type != input_type) {
                        node.outputs[0]->resolved_type = input_type;
                        changed = true;
                    }
                }
            }
        }

        // DeclVar output type inference is handled above (before is_declaration skip)

        if (node.type_id == NodeTypeID::Select) {
            // Resolve types from inline exprs or input pins
            auto get_arg_type = [&](int arg_idx) -> TypePtr {
                if (arg_idx < (int)node.parsed_exprs.size() && node.parsed_exprs[arg_idx] &&
                    node.parsed_exprs[arg_idx]->resolved_type)
                    return node.parsed_exprs[arg_idx]->resolved_type;
                if (arg_idx < (int)node.inputs.size() && node.inputs[arg_idx]->resolved_type)
                    return node.inputs[arg_idx]->resolved_type;
                return nullptr;
            };

            auto cond_type = decay_symbol(get_arg_type(0));
            auto true_type = decay_symbol(get_arg_type(1));
            auto false_type = decay_symbol(get_arg_type(2));

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

            // Select is a runtime operation — strip literal annotations
            if (result_type) result_type = strip_literal(result_type);
            if (result_type && !node.outputs.empty()) {
                if (!node.outputs[0]->resolved_type || node.outputs[0]->resolved_type->is_generic) {
                    if (node.outputs[0]->resolved_type != result_type) {
                        node.outputs[0]->resolved_type = result_type;
                        changed = true;
                    }
                }
            }
        }

        // --- Node-specific validation ---

        if (is_any_of(node.type_id, NodeTypeID::StoreBang, NodeTypeID::Store) && node.parsed_exprs.size() >= 2) {
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
        if (is_any_of(node.type_id, NodeTypeID::StoreBang, NodeTypeID::Store) && node.parsed_exprs.size() >= 1) {
            auto& target_expr = node.parsed_exprs[0];
            if (target_expr && target_expr->resolved_type && !target_expr->resolved_type->is_generic) {
                auto target_type = ctx.resolve_type(target_expr->resolved_type);
                if (target_type && target_type->kind == TypeKind::Function) {
                    // Find the value input pin and set its expected type
                    for (auto& inp : node.inputs) {
                        // Check if this pin is connected from an as_lambda
                        for (auto& l : graph.links) {
                            if (l.to_pin == inp->id && l.from_pin.find(".as_lambda") != std::string::npos) {
                                if (!inp->resolved_type || inp->resolved_type->is_generic) {
                                    inp->resolved_type = target_type;
                                    changed = true;
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }

        if (is_any_of(node.type_id, NodeTypeID::Erase, NodeTypeID::EraseBang) && node.parsed_exprs.size() >= 2) {
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
                            if (!node.outputs[0]->resolved_type || node.outputs[0]->resolved_type->is_generic) {
                                node.outputs[0]->resolved_type = iter_type;
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

        if (is_any_of(node.type_id, NodeTypeID::IterateBang, NodeTypeID::Iterate) && node.parsed_exprs.size() >= 1) {
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
                        if (inp->direction == FlowPin::Lambda) {
                            if (!inp->resolved_type || inp->resolved_type->is_generic) {
                                inp->resolved_type = fn_type;
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

        if (is_any_of(node.type_id, NodeTypeID::Lock, NodeTypeID::LockBang) && node.parsed_exprs.size() >= 1) {
            auto& mutex_expr = node.parsed_exprs[0];
            if (mutex_expr && mutex_expr->resolved_type && !mutex_expr->resolved_type->is_generic) {
                auto mutex_resolved = ctx.resolve_type(mutex_expr->resolved_type);
                // Validate: first arg must be mutex (auto-decays to reference)
                if (!mutex_resolved || mutex_resolved->kind != TypeKind::Mutex) {
                    if (node.error.empty())
                        node.error = std::string(node_type_str(node.type_id)) + ": first argument must be a mutex (got " +
                            type_to_string(mutex_expr->resolved_type) + ")";
                }

                // Find the lambda root and collect its params to determine the function signature
                FlowNode* lambda_root = nullptr;
                for (auto& inp : node.inputs) {
                    if (inp->direction != FlowPin::Lambda) continue;
                    for (auto& link : graph.links) {
                        if (link.to_pin == inp->id) {
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

                // Build caller scope: nodes in the execution ancestry before the lock.
                // For lock! (bang): walk backward from triggers.
                // For lock (non-bang): walk backward from whoever captures this node's as_lambda.
                std::set<std::string> caller_scope;
                {
                    std::vector<FlowNode*> work;
                    // Seed from triggers (lock! has triggers, lock doesn't)
                    for (auto& trig : node.triggers) {
                        auto* src_n = idx.source_node(trig.get());
                        if (src_n) work.push_back(src_n);
                    }
                    // Seed from the node that captures this node's as_lambda
                    for (auto& link : graph.links) {
                        if (link.from_pin == node.lambda_grab.id && link.to_node) {
                            // The capture node and its ancestors are in caller scope
                            work.push_back(link.to_node);
                        }
                    }
                    // Seed from non-lambda, non-as_lambda data inputs
                    for (auto& inp : node.inputs) {
                        if (inp->direction == FlowPin::Lambda) continue;
                        auto* src_pin = idx.source_pin(inp.get());
                        if (src_pin && src_pin->direction == FlowPin::LambdaGrab) continue;
                        auto* src_n = idx.source_node(inp.get());
                        if (src_n) work.push_back(src_n);
                    }
                    while (!work.empty()) {
                        auto* n = work.back(); work.pop_back();
                        if (caller_scope.count(n->guid)) continue;
                        caller_scope.insert(n->guid);
                        for (auto& trig : n->triggers) {
                            auto* src_n = idx.source_node(trig.get());
                            if (src_n) work.push_back(src_n);
                        }
                        for (auto& inp : n->inputs) {
                            if (inp->direction == FlowPin::Lambda) continue;
                            auto* src_pin = idx.source_pin(inp.get());
                            if (src_pin && src_pin->direction == FlowPin::LambdaGrab) continue;
                            auto* src_n = idx.source_node(inp.get());
                            if (src_n) work.push_back(src_n);
                        }
                    }
                }

                // Collect lambda params to determine how many extra inputs lock needs
                std::vector<FlowPin*> lambda_params;
                if (lambda_root) {
                    std::set<std::string> visited;
                    collect_lambda_params(graph, *lambda_root, lambda_params, visited, &caller_scope);
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
                    if (inp->direction != FlowPin::Lambda && inp->name != "mutex")
                        existing_extra++;
                }
                int needed_extra = (int)lambda_params.size();
                if (needed_extra != existing_extra) {
                    // Remove old extra pins and add new ones
                    PinVec new_inputs;
                    for (auto& inp : node.inputs) {
                        if (inp->direction == FlowPin::Lambda || inp->name == "mutex")
                            new_inputs.push_back(make_pin(inp->id, inp->name, inp->type_name, inp->resolved_type, inp->direction));
                    }
                    for (int pi = 0; pi < needed_extra; pi++) {
                        std::string pname = "arg" + std::to_string(pi);
                        std::string ptype = lambda_params[pi]->resolved_type ?
                            type_to_string(lambda_params[pi]->resolved_type) : "value";
                        new_inputs.push_back(make_pin("", pname, ptype, nullptr, FlowPin::Input));
                    }
                    node.inputs = std::move(new_inputs);
                    node.rebuild_pin_ids();
                    changed = true;
                }
                // Set types on extra input pins
                for (int pi = 0; pi < needed_extra; pi++) {
                    for (auto& inp : node.inputs) {
                        if (inp->name == "arg" + std::to_string(pi) && lambda_params[pi]->resolved_type) {
                            if (!inp->resolved_type || inp->resolved_type->is_generic) {
                                inp->resolved_type = lambda_params[pi]->resolved_type;
                                changed = true;
                            }
                        }
                    }
                }

                // Set expected type on lambda input pin
                for (auto& inp : node.inputs) {
                    if (inp->direction == FlowPin::Lambda) {
                        if (!inp->resolved_type || inp->resolved_type->is_generic) {
                            inp->resolved_type = fn_type;
                            changed = true;
                        }

                        // Resolve return type from the lambda root's output
                        TypePtr lambda_ret_type;
                        if (lambda_root && !lambda_root->outputs.empty() &&
                            lambda_root->outputs[0]->resolved_type &&
                            !lambda_root->outputs[0]->resolved_type->is_generic) {
                            lambda_ret_type = lambda_root->outputs[0]->resolved_type;
                        }

                        if (lambda_ret_type && lambda_ret_type->kind != TypeKind::Void) {
                            if (node.outputs.empty()) {
                                node.outputs.push_back(make_pin("", "result", "", nullptr, FlowPin::Output));
                                node.rebuild_pin_ids();
                            }
                            if (!node.outputs[0]->resolved_type || node.outputs[0]->resolved_type->is_generic) {
                                node.outputs[0]->resolved_type = lambda_ret_type;
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
                    node.error = std::string(node_type_str(node.type_id)) + ": mutex must be a variable reference";
            }
        }

        if (is_any_of(node.type_id, NodeTypeID::Call, NodeTypeID::CallBang) && node.parsed_exprs.size() >= 1) {
            // First arg is the function reference
            auto& fn_expr = node.parsed_exprs[0];
            if (fn_expr && fn_expr->resolved_type && !fn_expr->resolved_type->is_generic) {
                auto fn_resolved = ctx.resolve_type(fn_expr->resolved_type);
                if (!fn_resolved || fn_resolved->kind != TypeKind::Function) {
                    if (node.error.empty())
                        node.error = std::string(node_type_str(node.type_id)) + ": first argument must be a function (got " +
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
                                        if (p->name == pin_name && fn_resolved->func_args[ai].type) {
                                            if (!p->resolved_type || p->resolved_type->is_generic) {
                                                p->resolved_type = fn_resolved->func_args[ai].type;
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
                                if (!node.inputs[pi]->resolved_type || node.inputs[pi]->resolved_type->is_generic) {
                                    node.inputs[pi]->resolved_type = fn_resolved->func_args[fn_arg_idx].type;
                                    changed = true;
                                }
                            }
                        }
                    }
                    // Set output type from return type
                    if (fn_resolved->return_type && fn_resolved->return_type->kind != TypeKind::Void) {
                        if (!node.outputs.empty()) {
                            if (!node.outputs[0]->resolved_type || node.outputs[0]->resolved_type->is_generic) {
                                node.outputs[0]->resolved_type = fn_resolved->return_type;
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
                            node.error = std::string(node_type_str(node.type_id)) + ": too many arguments (" +
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
                                    node.error = std::string(node_type_str(node.type_id)) + ": argument '" +
                                        fn_resolved->func_args[j-1].name + "' type mismatch: " +
                                        type_to_string(arg_expr->resolved_type) + " vs expected " +
                                        type_to_string(fn_resolved->func_args[j-1].type);
                            }
                        }
                    }
                }
            }
        }

        if (is_any_of(node.type_id, NodeTypeID::AppendBang, NodeTypeID::Append) && node.parsed_exprs.size() >= 1) {
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
                    if (!node.outputs[0]->resolved_type ||
                        node.outputs[0]->resolved_type->kind != TypeKind::ContainerIterator) {
                        node.outputs[0]->resolved_type = iter_type;
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

        if (node.type_id == NodeTypeID::SelectBang) {
            // Condition input must be bool
            auto get_cond_type = [&]() -> TypePtr {
                if (!node.parsed_exprs.empty() && node.parsed_exprs[0] &&
                    node.parsed_exprs[0]->resolved_type)
                    return node.parsed_exprs[0]->resolved_type;
                if (!node.inputs.empty() && node.inputs[0]->resolved_type)
                    return node.inputs[0]->resolved_type;
                return nullptr;
            };
            auto cond_type = decay_symbol(get_cond_type());
            if (cond_type && !cond_type->is_generic) {
                auto resolved = ctx.resolve_type(cond_type);
                if (!resolved || resolved->kind != TypeKind::Bool) {
                    if (node.error.empty())
                        node.error = "select! condition must be bool (got " + type_to_string(cond_type) + ")";
                }
            }
        }

        if (node.type_id == NodeTypeID::OutputMixBang) {
            // Input must be f32
            auto get_input_type = [&]() -> TypePtr {
                if (!node.parsed_exprs.empty() && node.parsed_exprs[0] &&
                    node.parsed_exprs[0]->resolved_type)
                    return node.parsed_exprs[0]->resolved_type;
                if (!node.inputs.empty() && node.inputs[0]->resolved_type)
                    return node.inputs[0]->resolved_type;
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
    // HACK/TODO: Walk expression tree and propagate resolved types from PinRef nodes back
    // to pins. Skip PinRefs used as callees in FuncCall — their type is the function type,
    // not the value type for the pin. This is a workaround for call! nodes not having shadow
    // exprs (they're in the skip list because resolve_type_based_pins manages their pins).
    // The proper fix: make shadow exprs work for call! nodes, so each inline arg (including
    // $N(...) lambda calls) becomes its own expr node with independent type resolution.
    std::function<void(const ExprPtr&, bool)> walk = [&](const ExprPtr& e, bool is_callee) {
        if (!e) return;
        if (e->kind == ExprKind::PinRef && !is_callee &&
            e->pin_ref.index >= 0 && e->pin_ref.index < (int)node.inputs.size()) {
            auto& pin = *node.inputs[e->pin_ref.index];
            if (e->resolved_type && !e->resolved_type->is_generic &&
                (!pin.resolved_type || pin.resolved_type->is_generic)) {
                pin.resolved_type = e->resolved_type;
                changed = true;
            }
        }
        for (size_t i = 0; i < e->children.size(); i++) {
            // children[0] of a FuncCall is the callee — don't propagate its type to the pin
            bool child_is_callee = (e->kind == ExprKind::FuncCall && i == 0 && e->builtin == BuiltinFunc::None);
            walk(e->children[i], child_is_callee);
        }
    };
    for (auto& expr : node.parsed_exprs) walk(expr, false);
}

bool GraphInference::resolve_lambdas(FlowGraph& graph) {
    bool changed = false;
    for (auto& node : graph.nodes) {
        for (auto& link : graph.links) {
            if (link.from_pin != node.lambda_grab.id) continue;
            if (!link.to || !link.to->resolved_type) continue;
            auto* target_pin = link.to;

            // Resolve through Named type aliases
            auto expected = target_pin->resolved_type;
            while (expected && expected->kind == TypeKind::Named) {
                auto it = registry.parsed.find(expected->named_ref);
                if (it != registry.parsed.end() && it->second.get() != expected.get())
                    expected = it->second;
                else break;
            }
            if (!expected || expected->kind != TypeKind::Function) continue;

            // Build caller scope: all nodes in the bang chain ancestry before the
            // lambda capture point. These nodes are already materialized — their outputs
            // are captures, not lambda parameters.
            std::set<std::string> caller_scope;
            if (link.to_node) {
                // Walk backward from the capture node's ANCESTORS (not the capture node itself).
                // The capture node is the boundary — don't enter the lambda subgraph.
                std::vector<FlowNode*> work;
                // Seed with bang-chain ancestors of the capture node
                for (auto& trig : link.to_node->triggers) {
                    auto* src_n = idx.source_node(trig.get());
                    if (src_n) work.push_back(src_n);
                }
                // Seed with data input sources of the capture node (but NOT the lambda itself)
                for (auto& inp : link.to_node->inputs) {
                    if (inp->direction == FlowPin::Lambda) continue;
                    auto* src_pin = idx.source_pin(inp.get());
                    // Skip if the source is an as_lambda pin (that's the lambda, not a capture)
                    if (src_pin && src_pin->direction == FlowPin::LambdaGrab) continue;
                    auto* src_n = idx.source_node(inp.get());
                    if (src_n) work.push_back(src_n);
                }
                while (!work.empty()) {
                    auto* n = work.back(); work.pop_back();
                    if (caller_scope.count(n->guid)) continue;
                    caller_scope.insert(n->guid);
                    for (auto& trig : n->triggers) {
                        auto* src_n = idx.source_node(trig.get());
                        if (src_n) work.push_back(src_n);
                    }
                    for (auto& inp : n->inputs) {
                        if (inp->direction == FlowPin::Lambda) continue;
                        auto* src_n = idx.source_node(inp.get());
                        if (src_n) work.push_back(src_n);
                    }
                }
            }

            // Recursively collect unconnected input pins (lambda parameters)
            std::vector<FlowPin*> params;
            std::set<std::string> visited;
            collect_lambda_params(graph, node, params, visited, &caller_scope);

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
                    if (!out->resolved_type) {
                        out->resolved_type = expected->return_type;
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
    return idx.find_node_by_pin(pin_id);
}

void GraphInference::follow_bang_chain(FlowGraph& graph, const std::string& from_pin_id,
                      std::vector<FlowPin*>& params, std::set<std::string>& visited,
                      const std::set<std::string>* caller_scope) {
    auto* pin = idx.find_pin(from_pin_id);
    if (!pin) return;
    for (auto* target_node : idx.follow_bang(pin)) {
        collect_lambda_params(graph, *target_node, params, visited, caller_scope);
    }
}

void GraphInference::collect_lambda_params(FlowGraph& graph, FlowNode& node,
                           std::vector<FlowPin*>& params, std::set<std::string>& visited,
                           const std::set<std::string>* caller_scope) {
    if (visited.count(node.guid)) return;
    visited.insert(node.guid);

    // 1. Data inputs: recurse into connected sources, collect unconnected as params
    //    Skip Lambda inputs — they define inner lambda boundaries
    for (auto& inp : node.inputs) {
        if (inp->direction == FlowPin::Lambda) continue;
        auto* src_pin = idx.source_pin(inp.get());
        if (!src_pin) {
            params.push_back(inp.get());
        } else {
            // Don't recurse through as_lambda (LambdaGrab) pins — they are lambda boundaries
            auto* src_node = idx.source_node(inp.get());
            if (src_node) {
                bool is_lambda_grab = (src_pin->direction == FlowPin::LambdaGrab);
                if (is_lambda_grab) continue;

                // If the source node is in the caller scope (executed before lambda capture),
                // it's a capture — don't recurse into it
                if (caller_scope && caller_scope->count(src_node->guid)) continue;

                collect_lambda_params(graph, *src_node, params, visited, caller_scope);
            }
        }
    }

    // 2. Side bang (post_bang): follow chain to downstream nodes
    follow_bang_chain(graph, node.bang_pin.id, params, visited, caller_scope);

    // 3. Output bangs (left to right): follow each bang output's chain
    for (auto& bout : node.nexts) {
        follow_bang_chain(graph, bout->id, params, visited, caller_scope);
    }
}

void GraphInference::validate_lambda(FlowNode& node, const std::vector<FlowPin*>& params,
                     const TypePtr& expected, FlowLink& link) {
    // Build the actual lambda type from collected params and outputs
    auto lambda_type = std::make_shared<TypeExpr>();
    lambda_type->kind = TypeKind::Function;
    for (auto* p : params)
        lambda_type->func_args.push_back({"", p->resolved_type});
    if (!node.outputs.empty() && node.outputs[0]->resolved_type)
        lambda_type->return_type = node.outputs[0]->resolved_type;
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

// Recursively walk an expression tree and insert Deref nodes where an iterator
// is passed as a function argument but the param expects a non-iterator.
static void fixup_derefs_in_expr(const ExprPtr& expr, TypePool& pool) {
    if (!expr) return;

    // Recurse into children first (bottom-up)
    for (auto& child : expr->children)
        fixup_derefs_in_expr(child, pool);

    if (expr->kind != ExprKind::FuncCall) return;
    if (expr->children.empty()) return;

    // Determine function type from callee
    auto callee_type = expr->children[0]->resolved_type;
    // Resolve through named types
    while (callee_type && callee_type->kind == TypeKind::Named) {
        auto it = pool.cache.find(callee_type->named_ref);
        if (it != pool.cache.end() && it->second.get() != callee_type.get())
            callee_type = it->second;
        else break;
    }
    if (!callee_type || callee_type->kind != TypeKind::Function) return;

    size_t expected_args = callee_type->func_args.size();
    size_t actual_args = expr->children.size() - 1;

    for (size_t i = 0; i < std::min(actual_args, expected_args); i++) {
        auto& arg = expr->children[i + 1];
        if (!arg || !arg->resolved_type) continue;
        if (arg->kind == ExprKind::Deref) continue;

        if (arg->resolved_type->kind == TypeKind::ContainerIterator) {
            auto& param_type = callee_type->func_args[i].type;
            if (param_type && param_type->kind != TypeKind::ContainerIterator) {
                // Wrap in Deref
                auto deref = std::make_shared<ExprNode>();
                deref->kind = ExprKind::Deref;
                deref->children.push_back(arg);
                deref->resolved_type = arg->resolved_type->value_type;
                deref->access = ValueAccess::Value;
                arg = deref;
            }
        }
    }
}

void GraphInference::fixup_expr_derefs(FlowGraph& graph) {
    for (auto& node : graph.nodes) {
        // Fix derefs inside expressions (e.g. $obj.method($iter_arg))
        for (auto& expr : node.parsed_exprs) {
            fixup_derefs_in_expr(expr, pool);
        }

        // Fix derefs for call/call! inline args where the arg is an iterator
        // but the function param expects a non-iterator
        if (!is_any_of(node.type_id, NodeTypeID::Call, NodeTypeID::CallBang)) continue;
        if (node.parsed_exprs.size() < 2) continue;

        // First parsed_expr is the function ref — resolve its type through named aliases
        auto fn_type = node.parsed_exprs[0] ? node.parsed_exprs[0]->resolved_type : nullptr;
        while (fn_type && fn_type->kind == TypeKind::Named) {
            // Check both pool.cache and registry.parsed for named type resolution
            auto it = pool.cache.find(fn_type->named_ref);
            if (it != pool.cache.end() && it->second.get() != fn_type.get()) {
                fn_type = it->second;
            } else {
                auto rit = registry.parsed.find(fn_type->named_ref);
                if (rit != registry.parsed.end() && rit->second.get() != fn_type.get())
                    fn_type = rit->second;
                else break;
            }
        }
        if (!fn_type || fn_type->kind != TypeKind::Function) continue;

        // Check each arg (parsed_exprs[1..]) against fn params
        for (size_t i = 1; i < node.parsed_exprs.size() && (i - 1) < fn_type->func_args.size(); i++) {
            auto& arg_expr = node.parsed_exprs[i];
            if (!arg_expr || !arg_expr->resolved_type) continue;
            if (arg_expr->kind == ExprKind::Deref) continue; // already wrapped

            if (arg_expr->resolved_type->kind == TypeKind::ContainerIterator) {
                auto& param_type = fn_type->func_args[i - 1].type;
                if (param_type && param_type->kind != TypeKind::ContainerIterator) {
                    auto deref = std::make_shared<ExprNode>();
                    deref->kind = ExprKind::Deref;
                    deref->children.push_back(arg_expr);
                    deref->resolved_type = arg_expr->resolved_type->value_type;
                    deref->access = ValueAccess::Value;
                    arg_expr = deref;
                }
            }
        }
    }
}

void GraphInference::insert_deref_nodes(FlowGraph& graph) {
    idx.rebuild(graph);

    // Collect links that need a deref node inserted
    struct DerefTask {
        std::string from_pin_id;  // source output pin (iterator type)
        std::string to_pin_id;    // target input pin (expects value/ref)
        std::string from_guid;    // source node guid (for positioning)
        TypePtr elem_type;        // the dereferenced element type
    };
    std::vector<DerefTask> tasks;

    for (auto& link : graph.links) {
        if (!link.from || !link.to) continue;
        if (!link.from->resolved_type || !link.to->resolved_type) continue;

        auto from_t = link.from->resolved_type;
        auto to_t = link.to->resolved_type;

        // Skip if source is not an iterator
        if (from_t->kind != TypeKind::ContainerIterator) continue;
        // Skip if target also expects an iterator
        if (to_t->kind == TypeKind::ContainerIterator) continue;
        // Skip if target is generic/unknown (let it resolve naturally)
        if (to_t->is_generic) continue;

        // Extract element type from the iterator
        TypePtr elem_type = from_t->value_type;

        // Get source node guid for positioning
        std::string from_guid;
        if (link.from_node) from_guid = link.from_node->guid;

        tasks.push_back({link.from_pin, link.to_pin, from_guid, elem_type});
    }

    if (tasks.empty()) return;

    // Apply: for each task, create a deref node and rewire
    for (auto& task : tasks) {
        // Remove old link
        std::erase_if(graph.links, [&](auto& l) {
            return l.from_pin == task.from_pin_id && l.to_pin == task.to_pin_id;
        });

        // Create deref shadow node
        FlowNode deref;
        deref.id = graph.next_node_id();
        deref.guid = task.from_guid + "_deref_" + std::to_string(deref.id);
        deref.type_id = NodeTypeID::Deref;
        deref.shadow = true;

        // Position near source
        for (auto& n : graph.nodes) {
            if (n.guid == task.from_guid) {
                deref.position = {n.position.x + 50, n.position.y + 30};
                break;
            }
        }

        // Input: iterator
        deref.inputs.push_back(make_pin("", "value", "", nullptr, FlowPin::Input));
        // Output: dereferenced value
        deref.outputs.push_back(make_pin("", "out0", "", nullptr, FlowPin::Output));
        deref.rebuild_pin_ids();

        // Set resolved types
        deref.inputs[0]->resolved_type = task.elem_type; // input receives iterator (but we set elem for downstream)
        if (task.elem_type)
            deref.outputs[0]->resolved_type = task.elem_type;

        // Wire: source → deref.value, deref.out0 → target
        std::string deref_in = deref.pin_id("value");
        std::string deref_out = deref.pin_id("out0");

        graph.nodes.push_back(std::move(deref));
        graph.add_link(task.from_pin_id, deref_in);
        graph.add_link(deref_out, task.to_pin_id);
    }
}

void GraphInference::precompute_resolved_data(FlowGraph& graph) {
    // Rebuild index one last time to ensure it's current
    idx.rebuild(graph);

    for (auto& node : graph.nodes) {
        node.resolved_lambdas.clear();
        node.resolved_fn_type = nullptr;
        node.needs_narrowing_cast = false;

        // 1. Pre-compute lambda roots for nodes with Lambda-direction input pins
        for (auto& inp : node.inputs) {
            if (inp->direction != FlowPin::Lambda) continue;
            FlowNode::ResolvedLambda rl;
            rl.root = idx.source_node(inp.get());
            if (rl.root) {
                std::set<std::string> visited;
                collect_lambda_params(graph, *rl.root, rl.params, visited);
            }
            node.resolved_lambdas.push_back(std::move(rl));
        }

        // 2. Pre-compute store target function type (for stored lambdas)
        if (is_any_of(node.type_id, NodeTypeID::Store, NodeTypeID::StoreBang)) {
            if (!node.parsed_exprs.empty() && node.parsed_exprs[0]) {
                auto fn_type = node.parsed_exprs[0]->resolved_type;
                // Resolve Named type aliases
                while (fn_type && fn_type->kind == TypeKind::Named) {
                    auto it = pool.cache.find(fn_type->named_ref);
                    if (it != pool.cache.end() && it->second.get() != fn_type.get())
                        fn_type = it->second;
                    else break;
                }
                if (fn_type && fn_type->kind == TypeKind::Function)
                    node.resolved_fn_type = fn_type;
            }
        }

        // 3. Pre-compute narrowing cast flag for 'new' nodes
        if (node.type_id == NodeTypeID::New) {
            for (auto& inp : node.inputs) {
                if (inp->resolved_type && !inp->resolved_type->is_generic &&
                    inp->resolved_type->kind == TypeKind::Scalar) {
                    auto* src_node = idx.source_node(inp.get());
                    if (src_node && !src_node->outputs.empty() &&
                        src_node->outputs[0]->resolved_type &&
                        src_node->outputs[0]->resolved_type->is_generic) {
                        node.needs_narrowing_cast = true;
                        break;
                    }
                }
            }
        }
    }
}
