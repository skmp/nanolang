#include "shadow.h"
#include "args.h"
#include "expr.h"
#include "node_types.h"
#include <set>

// Nodes whose args are NOT expressions — skip shadow generation
static bool skip_shadow(NodeTypeID id) {
    return is_any_of(id,
        NodeTypeID::DeclType, NodeTypeID::DeclVar, NodeTypeID::DeclLocal,
        NodeTypeID::DeclEvent, NodeTypeID::DeclImport, NodeTypeID::Ffi,
        NodeTypeID::New, NodeTypeID::EventBang, NodeTypeID::Cast,
        NodeTypeID::Label,
        // Expr nodes already ARE expressions
        NodeTypeID::Expr, NodeTypeID::ExprBang,
        // Nodes with no meaningful inline args
        NodeTypeID::Dup, NodeTypeID::Str, NodeTypeID::Void, NodeTypeID::Discard,
        NodeTypeID::DiscardBang, NodeTypeID::Next,
        NodeTypeID::OnKeyDownBang, NodeTypeID::OnKeyUpBang,
        NodeTypeID::OutputMixBang);
}

void generate_shadow_nodes(FlowGraph& graph) {
    // Phase 1: Collect shadow generation tasks (don't modify graph yet)
    struct ShadowTask {
        std::string parent_guid;
        NodeTypeID parent_type_id;
        Vec2 parent_pos;
        std::vector<std::string> arg_tokens;
        bool is_call;
    };
    std::vector<ShadowTask> tasks;

    for (auto& node : graph.nodes) {
        if (node.shadow || node.args.empty()) continue;
        if (skip_shadow(node.type_id)) continue;

        auto* nt = find_node_type(node.type_id);
        if (!nt) continue;

        bool is_call = is_any_of(node.type_id, NodeTypeID::Call, NodeTypeID::CallBang);
        auto tokens = tokenize_args(node.args, false);
        int first_arg = is_call ? 1 : 0;
        if ((int)tokens.size() <= first_arg) continue;

        std::vector<std::string> arg_tokens(tokens.begin() + first_arg, tokens.end());
        tasks.push_back({node.guid, node.type_id, node.position, std::move(arg_tokens), is_call});
    }

    // Phase 2: Create shadow nodes and links (collected, then applied)
    struct PendingNode { FlowNode node; };
    struct PendingLink { std::string from_pin; std::string to_pin; };
    std::vector<PendingNode> pending_nodes;
    std::vector<PendingLink> pending_links;

    // Modifications to parent nodes
    struct ParentMod {
        std::string guid;
        std::string new_args;            // cleared or fn-ref only
        PinVec new_inputs;               // rebuilt inputs (descriptor pins only)
    };
    std::vector<ParentMod> parent_mods;

    for (auto& task : tasks) {
        auto* nt = find_node_type(task.parent_type_id);
        if (!nt) continue;

        ParentMod mod;
        mod.guid = task.parent_guid;

        // For call nodes, keep the function ref; for others, clear args
        if (task.is_call) {
            // Find the original first token (fn ref)
            FlowNode* parent = nullptr;
            for (auto& n : graph.nodes) if (n.guid == task.parent_guid) { parent = &n; break; }
            if (!parent) continue;
            auto all_tokens = tokenize_args(parent->args, false);
            mod.new_args = all_tokens.empty() ? "" : all_tokens[0];
        }

        // Create shadow nodes
        for (int ti = 0; ti < (int)task.arg_tokens.size(); ti++) {
            auto& tok = task.arg_tokens[ti];

            FlowNode shadow;
            shadow.id = graph.next_node_id();
            shadow.guid = task.parent_guid + "_s" + std::to_string(ti);
            shadow.type_id = NodeTypeID::Expr;
            shadow.args = tok;
            shadow.position = {task.parent_pos.x - 150, task.parent_pos.y - 50.0f * ti};
            shadow.shadow = true;

            // Input pins from $N refs
            auto slots = scan_slots(tok);
            int pin_count = slots.total_pin_count(0);
            for (int pi = 0; pi < pin_count; pi++) {
                bool is_lambda = slots.is_lambda_slot(pi);
                std::string pname = is_lambda ? ("@" + std::to_string(pi)) : std::to_string(pi);
                shadow.inputs.push_back(make_pin("", pname, "", nullptr,
                    is_lambda ? FlowPin::Lambda : FlowPin::Input));
            }

            // One output
            shadow.outputs.push_back(make_pin("", "out0", "", nullptr, FlowPin::Output));
            shadow.rebuild_pin_ids();

            // Determine target pin name on parent
            std::string target_pin_name;
            if (nt->input_ports && ti < nt->inputs) {
                target_pin_name = nt->input_ports[ti].name;
            } else {
                target_pin_name = std::to_string(ti);
            }

            // Ensure parent has this input pin
            // (built in new_inputs below)

            // Wire shadow output → parent input
            std::string from_id = shadow.guid + ".out0";
            std::string to_id = task.parent_guid + "." + target_pin_name;
            pending_links.push_back({from_id, to_id});

            // Re-route $N connections: if the parent had input pins for $N refs,
            // and those pins had incoming links, wire them to the shadow instead
            for (int pi = 0; pi < pin_count; pi++) {
                std::string parent_pin_id = task.parent_guid + "." + std::to_string(pi);
                std::string shadow_pin_name = slots.is_lambda_slot(pi)
                    ? ("@" + std::to_string(pi)) : std::to_string(pi);
                std::string shadow_pin_id = shadow.guid + "." + shadow_pin_name;

                for (auto& link : graph.links) {
                    if (link.to_pin == parent_pin_id) {
                        pending_links.push_back({link.from_pin, shadow_pin_id});
                    }
                }
            }

            pending_nodes.push_back({std::move(shadow)});
        }

        // Build new parent inputs: descriptor pins for shadow outputs to connect to
        for (int ti = 0; ti < (int)task.arg_tokens.size(); ti++) {
            std::string pin_name;
            if (nt->input_ports && ti < nt->inputs) {
                pin_name = nt->input_ports[ti].name;
            } else {
                pin_name = std::to_string(ti);
            }
            mod.new_inputs.push_back(make_pin("", pin_name, "", nullptr, FlowPin::Input));
        }

        parent_mods.push_back(std::move(mod));
    }

    // Phase 3: Apply modifications
    for (auto& mod : parent_mods) {
        for (auto& node : graph.nodes) {
            if (node.guid != mod.guid) continue;

            // Remove old links to $N ref pins (they're being replaced)
            for (auto& p : node.inputs) {
                if (p->name.empty()) continue;
                char c = p->name[0];
                if (c >= '0' && c <= '9') {
                    std::erase_if(graph.links, [&](auto& l) { return l.to_pin == p->id; });
                }
            }

            node.args = mod.new_args;
            node.inputs = std::move(mod.new_inputs);
            node.rebuild_pin_ids();
            break;
        }
    }

    for (auto& pn : pending_nodes) {
        graph.nodes.push_back(std::move(pn.node));
    }

    for (auto& pl : pending_links) {
        graph.add_link(pl.from_pin, pl.to_pin);
    }
}

void remove_shadow_nodes(FlowGraph& graph) {
    std::set<std::string> shadow_guids;
    for (auto& node : graph.nodes) {
        if (node.shadow) shadow_guids.insert(node.guid);
    }

    std::erase_if(graph.links, [&](auto& l) {
        auto dot1 = l.from_pin.find('.');
        auto dot2 = l.to_pin.find('.');
        if (dot1 != std::string::npos && shadow_guids.count(l.from_pin.substr(0, dot1))) return true;
        if (dot2 != std::string::npos && shadow_guids.count(l.to_pin.substr(0, dot2))) return true;
        return false;
    });

    std::erase_if(graph.nodes, [](auto& n) { return n.shadow; });
}
