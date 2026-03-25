#include "type_utils.h"
#include "node_types.h"
#include "types.h"
#include "expr.h"
#include <sstream>
#include <map>

// Helper: find the shadow node connected to a specific input pin of a node
static const FlowNode* find_shadow_source(const FlowNode& node, const std::string& pin_name, const FlowGraph& graph) {
    // Find the pin
    std::string pin_id;
    for (auto& p : node.inputs) {
        if (p->name == pin_name) { pin_id = p->id; break; }
    }
    if (pin_id.empty()) return nullptr;
    // Find the link TO this pin
    for (auto& link : graph.links) {
        if (link.to_pin == pin_id) {
            // Find the source node
            auto dot = link.from_pin.find('.');
            if (dot == std::string::npos) continue;
            std::string src_guid = link.from_pin.substr(0, dot);
            for (auto& n : graph.nodes) {
                if (n.guid == src_guid) return &n;
            }
        }
    }
    return nullptr;
}

// Helper: extract a symbol name from a shadow expr node's parsed expression or args
static std::string extract_symbol_from_node(const FlowNode* shadow) {
    if (!shadow) return "";
    // Check parsed_exprs first
    if (!shadow->parsed_exprs.empty() && shadow->parsed_exprs[0]) {
        auto& e = shadow->parsed_exprs[0];
        if (e->kind == ExprKind::SymbolRef) return e->symbol_name;
        if (e->kind == ExprKind::Literal && e->literal_kind == LiteralKind::String) return e->string_value;
    }
    // Fallback: raw args
    if (!shadow->args.empty()) return shadow->args;
    return "";
}

std::string get_decl_name(const FlowNode& node, const FlowGraph& graph) {
    // 1. Check parsed_exprs (may survive shadow generation)
    if (!node.parsed_exprs.empty() && node.parsed_exprs[0] &&
        node.parsed_exprs[0]->kind == ExprKind::SymbolRef)
        return node.parsed_exprs[0]->symbol_name;
    // 2. Check shadow node connected to "name" or "path" pin
    auto* shadow = find_shadow_source(node, "name", graph);
    if (!shadow) shadow = find_shadow_source(node, "path", graph);
    if (shadow) return extract_symbol_from_node(shadow);
    // 3. Fallback: raw args tokenization
    auto tokens = tokenize_args(node.args, false);
    return tokens.empty() ? "" : tokens[0];
}

std::string get_decl_type_str(const FlowNode& node, const FlowGraph& graph) {
    // 1. Check shadow node connected to "type" pin
    auto* shadow = find_shadow_source(node, "type", graph);
    if (shadow) return extract_symbol_from_node(shadow);
    // 2. Fallback: raw args — join tokens[1:]
    auto tokens = tokenize_args(node.args, false);
    if (tokens.size() < 2) return "";
    std::string type_str;
    for (size_t i = 1; i < tokens.size(); i++) {
        if (!type_str.empty()) type_str += " ";
        type_str += tokens[i];
    }
    return type_str;
}

std::vector<TypeField> parse_type_fields(const FlowNode& type_node) {
    std::vector<TypeField> fields;
    auto tokens = tokenize_args(type_node.args, false);
    // Skip token[0] (the type name)
    for (size_t i = 1; i < tokens.size(); i++) {
        auto& tok = tokens[i];
        if (tok.empty()) continue;
        if (tok[0] == '(' || tok == "->") continue;
        auto colon = tok.find(':');
        if (colon == std::string::npos) continue;
        fields.push_back({tok.substr(0, colon), tok.substr(colon + 1)});
    }
    return fields;
}

const FlowNode* find_type_node(const FlowGraph& graph, const std::string& type_name) {
    for (auto& n : graph.nodes) {
        if (n.type_id != NodeTypeID::DeclType) continue;
        if (get_decl_name(n, graph) == type_name) return &n;
    }
    return nullptr;
}

const FlowNode* find_event_node(const FlowGraph& graph, const std::string& event_name) {
    std::string name = event_name;
    if (!name.empty() && name[0] == '~') name = name.substr(1);
    for (auto& n : graph.nodes) {
        if (n.type_id != NodeTypeID::DeclEvent) continue;
        if (get_decl_name(n, graph) == name) return &n;
    }
    return nullptr;
}

std::vector<TypeField> parse_event_args(const FlowNode& event_decl, const FlowGraph& graph) {
    std::vector<TypeField> result;
    auto tokens = tokenize_args(event_decl.args, false);
    if (tokens.size() < 2) return result;

    // Check if it's an inline function type: starts with (
    if (tokens[1][0] == '(') {
        // Collect everything between ( and ) across tokens, then split by space
        std::string paren_content;
        for (size_t i = 1; i < tokens.size(); i++) {
            auto& tok = tokens[i];
            if (tok == "->") break;
            std::string s = tok;
            if (!s.empty() && s[0] == '(') s = s.substr(1);
            if (!s.empty() && s.back() == ')') s.pop_back();
            if (!s.empty()) {
                if (!paren_content.empty()) paren_content += " ";
                paren_content += s;
            }
        }
        // Split by space and parse each arg
        std::istringstream iss(paren_content);
        std::string arg;
        while (iss >> arg) {
            auto colon = arg.find(':');
            if (colon != std::string::npos) {
                result.push_back({arg.substr(0, colon), arg.substr(colon + 1)});
            }
        }
    } else {
        // It's a reference to a decl_type that is a function type
        auto* type_node = find_type_node(graph, tokens[1]);
        if (type_node) {
            auto type_tokens = tokenize_args(type_node->args, false);
            std::string paren_content;
            for (size_t i = 1; i < type_tokens.size(); i++) {
                auto& tok = type_tokens[i];
                if (tok == "->") break;
                std::string s = tok;
                if (!s.empty() && s[0] == '(') s = s.substr(1);
                if (!s.empty() && s.back() == ')') s.pop_back();
                if (!s.empty()) {
                    if (!paren_content.empty()) paren_content += " ";
                    paren_content += s;
                }
            }
            std::istringstream iss(paren_content);
            std::string arg;
            while (iss >> arg) {
                auto colon = arg.find(':');
                if (colon != std::string::npos) {
                    result.push_back({arg.substr(0, colon), arg.substr(colon + 1)});
                }
            }
        }
    }
    return result;
}

void reconcile_pins(PinVec& pins,
                    const std::vector<DesiredPinDesc>& desired,
                    const std::string& node_guid, bool is_output,
                    std::vector<FlowLink>& links) {
    std::map<std::string, int> existing;
    for (int i = 0; i < (int)pins.size(); i++)
        existing[pins[i]->name] = i;

    PinVec new_pins;
    for (auto& d : desired) {
        auto it = existing.find(d.name);
        if (it != existing.end()) {
            auto pin = std::move(pins[it->second]);
            pin->direction = d.dir;
            pin->type_name = d.type_name;
            if (d.resolved) pin->resolved_type = d.resolved;
            new_pins.push_back(std::move(pin));
            existing.erase(it);
        } else {
            std::string id = node_guid + "." + d.name;
            new_pins.push_back(make_pin(id, d.name, d.type_name, d.resolved, d.dir));
        }
    }

    // Remove links for pins that no longer exist
    for (auto& [name, idx] : existing) {
        auto& old_pin = pins[idx];
        if (is_output)
            std::erase_if(links, [&](auto& l) { return l.from_pin == old_pin->id; });
        else
            std::erase_if(links, [&](auto& l) { return l.to_pin == old_pin->id; });
    }

    pins = std::move(new_pins);
}

void resolve_type_based_pins(FlowGraph& graph) {
    for (auto& node : graph.nodes) {
        if (node.type_id == NodeTypeID::New) {
            auto tokens = tokenize_args(node.args, false);
            std::string inst_type_name = tokens.empty() ? "" : tokens[0];
            auto* type_node = find_type_node(graph, inst_type_name);
            if (type_node) {
                auto fields = parse_type_fields(*type_node);
                std::vector<DesiredPinDesc> desired;
                for (auto& field : fields)
                    desired.push_back({field.name, field.type_name, FlowPin::Input, field.resolved});
                reconcile_pins(node.inputs, desired, node.guid, false, graph.links);
                // Set output type to the instantiated type
                for (auto& p : node.outputs) p->type_name = inst_type_name;
                node.rebuild_pin_ids();
            }
        }
        if (node.type_id == NodeTypeID::EventBang) {
            auto tokens = tokenize_args(node.args, false);
            std::string event_name = tokens.empty() ? "" : tokens[0];
            auto* event_decl = find_event_node(graph, event_name);
            if (event_decl) {
                auto args = parse_event_args(*event_decl, graph);
                std::vector<DesiredPinDesc> desired;
                for (auto& arg : args)
                    desired.push_back({arg.name, arg.type_name, FlowPin::Output, arg.resolved});
                reconcile_pins(node.outputs, desired, node.guid, true, graph.links);
                node.rebuild_pin_ids();
            }
        }
        if (is_any_of(node.type_id, NodeTypeID::Call, NodeTypeID::CallBang)) {
            auto tokens = tokenize_args(node.args, false);
            if (tokens.empty()) continue;
            // First token is the function reference (e.g. "$sin" or "$imgui_begin")
            std::string fn_ref = tokens[0];
            // Strip $ prefix to get the name
            std::string fn_name = fn_ref;
            if (!fn_name.empty() && fn_name[0] == '$') fn_name = fn_name.substr(1);

            // Look up the function type from ffi or decl_var nodes
            TypePtr fn_type;
            TypePool pool;
            for (auto& other : graph.nodes) {
                if (other.type_id == NodeTypeID::Ffi) {
                    auto ftokens = tokenize_args(other.args, false);
                    if (!ftokens.empty() && ftokens[0] == fn_name) {
                        std::string type_str;
                        for (size_t i = 1; i < ftokens.size(); i++) {
                            if (!type_str.empty()) type_str += " ";
                            type_str += ftokens[i];
                        }
                        fn_type = pool.intern(type_str);
                        break;
                    }
                }
                if (other.type_id == NodeTypeID::DeclVar) {
                    auto ftokens = tokenize_args(other.args, false);
                    if (ftokens.size() >= 2 && ftokens[0] == fn_name) {
                        std::string type_str;
                        for (size_t i = 1; i < ftokens.size(); i++) {
                            if (!type_str.empty()) type_str += " ";
                            type_str += ftokens[i];
                        }
                        fn_type = pool.intern(type_str);
                        break;
                    }
                }
            }

            if (fn_type && fn_type->kind == TypeKind::Function) {
                // Inline args after the function name (tokens[1..]) cover function args left-to-right.
                // Only create input pins for function args NOT covered by inline expressions.
                int num_inline_args = (int)tokens.size() - 1; // exclude the fn ref token

                // Build desired pins: first keep $N ref pins from inline args,
                // then add remaining function args as named pins.
                std::vector<DesiredPinDesc> desired_inputs;

                // Preserve existing $N/@N ref pins (created by scan_slots during loading)
                for (auto& p : node.inputs) {
                    desired_inputs.push_back({p->name, p->type_name, p->direction, p->resolved_type});
                }

                // Add remaining function args not covered by inline expressions
                for (int ai = num_inline_args; ai < (int)fn_type->func_args.size(); ai++) {
                    auto& arg = fn_type->func_args[ai];
                    std::string type_str = arg.type ? type_to_string(arg.type) : "value";
                    desired_inputs.push_back({arg.name, type_str, FlowPin::Input, arg.type});
                }
                reconcile_pins(node.inputs, desired_inputs, node.guid, false, graph.links);

                // Set type info on existing pins from inline $N refs.
                // Token[i+1] is inline arg i; if it's a $N ref, pin "N" gets fn_arg[i]'s type.
                for (int ai = 0; ai < num_inline_args && ai < (int)fn_type->func_args.size(); ai++) {
                    int tok_idx = ai + 1; // skip fn name token
                    if (tok_idx >= (int)tokens.size()) break;
                    auto& tok = tokens[tok_idx];
                    // Check if this token is a bare $N pin ref (not $N.field or $N+expr)
                    if (tok.size() >= 2 && tok[0] == '$' && tok[1] >= '0' && tok[1] <= '9') {
                        // Extract pin number and check it's the entire token
                        size_t end = 1;
                        while (end < tok.size() && tok[end] >= '0' && tok[end] <= '9') end++;
                        if (end == tok.size()) {
                            // Bare $N — set pin type from function arg
                            std::string pin_name = tok.substr(1);
                            for (auto& p : node.inputs) {
                                if (p->name == pin_name && fn_type->func_args[ai].type) {
                                    p->type_name = type_to_string(fn_type->func_args[ai].type);
                                    p->resolved_type = fn_type->func_args[ai].type;
                                }
                            }
                        }
                        // $N.field, $N[i], etc.: pin type can't be derived from fn arg
                    }
                }

                // Create output pin from return type (if non-void)
                if (fn_type->return_type && fn_type->return_type->kind != TypeKind::Void) {
                    std::string ret_str = type_to_string(fn_type->return_type);
                    std::vector<DesiredPinDesc> desired_outputs;
                    desired_outputs.push_back({"result", ret_str, FlowPin::Output, fn_type->return_type});
                    reconcile_pins(node.outputs, desired_outputs, node.guid, true, graph.links);
                } else {
                    // Void return: no outputs
                    node.outputs.clear();
                }
                node.rebuild_pin_ids();
            }
        }
    }
}
