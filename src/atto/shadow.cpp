#include "shadow.h"
#include "args.h"
#include "expr.h"
#include "node_types.h"
#include <set>
#include <map>

static bool skip_shadow(NodeTypeID id) {
    return is_any_of(id,
        NodeTypeID::Decl,
        NodeTypeID::New, NodeTypeID::EventBang, NodeTypeID::Cast,
        NodeTypeID::Label, NodeTypeID::Deref,
        NodeTypeID::Expr, NodeTypeID::ExprBang,
        // TODO: call! nodes are skipped because resolve_type_based_pins manages their pins.
        // Shadow exprs for call! args would fix $N(...) lambda call type resolution properly.
        NodeTypeID::Call, NodeTypeID::CallBang,
        NodeTypeID::Dup, NodeTypeID::Str, NodeTypeID::Void, NodeTypeID::Discard,
        NodeTypeID::DiscardBang, NodeTypeID::Next,
        NodeTypeID::OnKeyDownBang, NodeTypeID::OnKeyUpBang,
        NodeTypeID::OutputMixBang);
}

void generate_shadow_nodes(FlowGraph& graph) {
    //
    // After serial loading, a non-expr node with inline args has:
    //   args = "token0 token1 ..."
    //   inputs = [$N ref pins from inline args] ++ [remaining descriptor pins]
    //
    // compute_inline_args tells us:
    //   num_inline_args: how many tokens fill descriptor inputs
    //   pin_slots: which $N/@N refs appear in the tokens
    //   remaining_descriptor_inputs: descriptor inputs beyond the inline count
    //
    // The shadow pass creates one shadow expr node per inline arg token, wires
    // its output to a NEW descriptor-named pin on the parent, and re-routes
    // any $N connections from the parent to the shadow.
    //
    // After the pass:
    //   - Parent args are cleared
    //   - Parent inputs = [descriptor pins for inline-arg positions (connected from shadows)]
    //                    ++ [remaining descriptor pins (unchanged, keep their connections)]
    //   - $N ref pins are removed from parent
    //

    struct Task {
        std::string guid;
        NodeTypeID type_id;
        Vec2 pos;
        std::vector<std::string> tokens;
        int num_inline_args;
        int descriptor_inputs;
        std::map<std::string, std::string> ref_sources;
        int first_shadow_arg; // args before this index are lvalues — keep as inline
    };

    std::vector<Task> tasks;

    for (auto& node : graph.nodes) {
        if (node.shadow || node.args.empty()) continue;
        if (skip_shadow(node.type_id)) continue;

        // Skip nodes used as lambda roots — their inline args are part of the
        // lambda subgraph and shadow nodes would cross lambda boundaries
        bool is_lambda_root = false;
        for (auto& link : graph.links) {
            if (link.from_pin == node.lambda_grab.id) { is_lambda_root = true; break; }
        }
        if (is_lambda_root) continue;

        auto* nt = find_node_type(node.type_id);
        if (!nt) continue;

        int di = nt->inputs;
        auto info = compute_inline_args(node.args, di);
        if (info.num_inline_args == 0) continue;

        int first_shadow_arg = first_shadow_arg_for(node.type_id);

        if (info.num_inline_args <= first_shadow_arg) continue;

        auto tokens = tokenize_args(node.args, false);

        // Collect $N ref pin → source link
        std::map<std::string, std::string> ref_sources;
        for (auto& p : node.inputs) {
            if (p->name.empty()) continue;
            char c = p->name[0];
            if (!((c >= '0' && c <= '9') || c == '@')) continue;
            for (auto& link : graph.links) {
                if (link.to_pin == p->id) {
                    ref_sources[p->name] = link.from_pin;
                    break;
                }
            }
        }

        tasks.push_back({node.guid, node.type_id, node.position,
                         std::move(tokens), info.num_inline_args, di,
                         std::move(ref_sources), first_shadow_arg});
    }

    if (tasks.empty()) return;

    // Collect pending operations
    std::vector<FlowNode> pending_nodes;
    struct Link { std::string from, to; };
    std::vector<Link> pending_links;

    struct Mod {
        std::string guid;
        std::set<std::string> remove_pin_ids;
        struct NewPin { std::string name; FlowPin::Direction dir; };
        std::vector<NewPin> insert_pins;
        int first_shadow_arg = 0;  // args before this are lvalues kept in node.args
        std::vector<std::string> kept_tokens; // lvalue tokens to preserve

        Mod() = default;
        Mod(Mod&&) = default;
        Mod& operator=(Mod&&) = default;
    };
    std::vector<Mod> mods;

    for (auto& task : tasks) {
        auto* nt = find_node_type(task.type_id);
        if (!nt) continue;

        Mod mod;
        mod.guid = task.guid;
        mod.first_shadow_arg = task.first_shadow_arg;
        // Keep lvalue tokens
        for (int ai = 0; ai < task.first_shadow_arg && ai < (int)task.tokens.size(); ai++)
            mod.kept_tokens.push_back(task.tokens[ai]);

        // Find parent node
        FlowNode* parent = nullptr;
        for (auto& n : graph.nodes) if (n.guid == task.guid) { parent = &n; break; }
        if (!parent) continue;

        // Determine which $N refs are used ONLY in shadowed args (not in lvalue args)
        // Collect $N refs from lvalue args
        std::set<std::string> lvalue_refs;
        for (int ai = 0; ai < task.first_shadow_arg && ai < (int)task.tokens.size(); ai++) {
            auto slots = scan_slots(task.tokens[ai]);
            for (auto& [idx, sigil] : slots.slots) {
                lvalue_refs.insert(sigil == '@' ? ("@" + std::to_string(idx)) : std::to_string(idx));
            }
        }

        // Mark $N ref pins for removal — only if NOT used in lvalue args
        for (auto& p : parent->inputs) {
            if (p->name.empty()) continue;
            char c = p->name[0];
            if ((c >= '0' && c <= '9') || c == '@') {
                if (lvalue_refs.count(p->name) == 0) {
                    mod.remove_pin_ids.insert(p->id);
                }
            }
        }

        // Create shadow expr for each inline arg token (skip lvalue args)
        for (int ti = task.first_shadow_arg; ti < task.num_inline_args; ti++) {
            auto& tok = task.tokens[ti];

            FlowNode shadow;
            shadow.id = graph.next_node_id();
            shadow.guid = task.guid + "_s" + std::to_string(ti);
            shadow.type_id = NodeTypeID::Expr;
            shadow.args = tok;
            shadow.position = {task.pos.x - 200, task.pos.y - 60.0f * ti};
            shadow.shadow = true;

            // Create shadow input pins from $N/@N refs in this token
            auto slots = scan_slots(tok);
            int pin_count = slots.total_pin_count(0);
            for (int pi = 0; pi < pin_count; pi++) {
                bool is_lam = slots.is_lambda_slot(pi);
                std::string pn = is_lam ? ("@" + std::to_string(pi)) : std::to_string(pi);
                shadow.inputs.push_back(make_pin("", pn, "", nullptr,
                    is_lam ? FlowPin::Lambda : FlowPin::Input));
            }

            // One output
            shadow.outputs.push_back(make_pin("", "out0", "", nullptr, FlowPin::Output));
            shadow.rebuild_pin_ids();

            // Pre-parse the expression
            auto parse_result = parse_expression(tok);
            if (parse_result.root)
                shadow.parsed_exprs.push_back(parse_result.root);
            else
                shadow.parsed_exprs.push_back(nullptr);

            // Wire shadow's $N inputs from parent's $N sources
            for (int pi = 0; pi < pin_count; pi++) {
                std::string pn = slots.is_lambda_slot(pi)
                    ? ("@" + std::to_string(pi)) : std::to_string(pi);
                auto it = task.ref_sources.find(pn);
                if (it != task.ref_sources.end()) {
                    pending_links.push_back({it->second, shadow.guid + "." + pn});
                }
            }

            // Determine descriptor pin name for this inline arg position
            std::string desc_name;
            if (nt->input_ports && ti < nt->inputs) {
                desc_name = nt->input_ports[ti].name;
            } else {
                desc_name = "arg" + std::to_string(ti);
            }

            // Wire shadow output → new parent descriptor pin
            pending_links.push_back({shadow.guid + ".out0", task.guid + "." + desc_name});

            // Record that we need this pin on the parent
            bool is_lam = nt->input_ports && (nt->input_ports[ti].kind == PortKind::Lambda);
            mod.insert_pins.push_back({desc_name, is_lam ? FlowPin::Lambda : FlowPin::Input});

            pending_nodes.push_back(std::move(shadow));
        }

        mods.push_back(std::move(mod));
    }

    // Apply modifications
    for (auto& mod : mods) {
        // Remove links TO $N ref pins
        std::erase_if(graph.links, [&](auto& l) {
            return mod.remove_pin_ids.count(l.to_pin) > 0;
        });

        for (auto& node : graph.nodes) {
            if (node.guid != mod.guid) continue;

            // Remove $N ref pins from inputs
            std::erase_if(node.inputs, [&](auto& p) {
                return mod.remove_pin_ids.count(p->id) > 0;
            });

            // Rebuild inputs: existing pins first (preserving $N ref indexing),
            // then new descriptor pins for shadow connections at the end
            PinVec new_inputs;
            for (auto& p : node.inputs)
                new_inputs.push_back(std::move(p));
            for (auto& np : mod.insert_pins) {
                bool found = false;
                for (auto& p : new_inputs) {
                    if (p->name == np.name) { found = true; break; }
                }
                if (!found) {
                    new_inputs.push_back(make_pin("", np.name, "", nullptr, np.dir));
                }
            }

            node.inputs = std::move(new_inputs);
            // Keep only lvalue tokens in args
            std::string kept;
            for (auto& t : mod.kept_tokens) {
                if (!kept.empty()) kept += " ";
                kept += t;
            }
            node.args = kept;
            node.rebuild_pin_ids();
            node.parse_args(); // re-parse with only lvalue tokens
            break;
        }
    }

    for (auto& n : pending_nodes)
        graph.nodes.push_back(std::move(n));
    for (auto& l : pending_links) {
        graph.add_link(l.from, l.to);
    }

}

void remove_shadow_nodes(FlowGraph& graph) {
    std::set<std::string> shadow_guids;
    for (auto& n : graph.nodes)
        if (n.shadow) shadow_guids.insert(n.guid);

    std::erase_if(graph.links, [&](auto& l) {
        auto d1 = l.from_pin.find('.');
        auto d2 = l.to_pin.find('.');
        if (d1 != std::string::npos && shadow_guids.count(l.from_pin.substr(0, d1))) return true;
        if (d2 != std::string::npos && shadow_guids.count(l.to_pin.substr(0, d2))) return true;
        return false;
    });

    std::erase_if(graph.nodes, [](auto& n) { return n.shadow; });
}

int first_shadow_arg_for(NodeTypeID id) {
    if (is_any_of(id, NodeTypeID::Store, NodeTypeID::StoreBang,
                   NodeTypeID::ResizeBang, NodeTypeID::Append, NodeTypeID::AppendBang,
                   NodeTypeID::Erase, NodeTypeID::EraseBang,
                   NodeTypeID::Lock, NodeTypeID::LockBang,
                   NodeTypeID::Iterate, NodeTypeID::IterateBang)) {
        return 1; // first arg is lvalue/reference target
    }
    return 0;
}

void rebuild_all_inline_display(FlowGraph& graph) {
    // Build a map: parent guid → ordered shadow args by descriptor pin index
    // Shadow nodes connect via shadow.out0 → parent.{descriptor_pin_name}
    struct ShadowInfo { int arg_index; std::string expr; };
    std::map<std::string, std::vector<ShadowInfo>> shadow_map; // parent_guid → shadow infos

    for (auto& node : graph.nodes) {
        if (!node.shadow) continue;
        // Find the link from this shadow's out0 to a parent pin
        std::string out0_id = node.guid + ".out0";
        for (auto& link : graph.links) {
            if (link.from_pin != out0_id) continue;
            // link.to_pin is "parent_guid.pin_name"
            auto dot = link.to_pin.find('.');
            if (dot == std::string::npos) continue;
            std::string parent_guid = link.to_pin.substr(0, dot);
            std::string pin_name = link.to_pin.substr(dot + 1);

            // Find parent node to determine arg index from descriptor pin name
            for (auto& parent : graph.nodes) {
                if (parent.guid != parent_guid) continue;
                auto* nt = find_node_type(parent.type_id);
                if (!nt) break;
                int arg_index = -1;
                if (nt->input_ports) {
                    for (int i = 0; i < nt->inputs; i++) {
                        if (nt->input_ports[i].name == pin_name) { arg_index = i; break; }
                    }
                }
                if (arg_index < 0) {
                    // Try parsing "argN" pattern
                    if (pin_name.substr(0, 3) == "arg") {
                        arg_index = std::stoi(pin_name.substr(3));
                    }
                }
                if (arg_index >= 0) {
                    shadow_map[parent_guid].push_back({arg_index, node.args});
                }
                break;
            }
            break; // shadow has exactly one output link
        }
    }

    // Sort each parent's shadows by arg index
    for (auto& [guid, infos] : shadow_map) {
        std::sort(infos.begin(), infos.end(), [](auto& a, auto& b) { return a.arg_index < b.arg_index; });
    }

    // Rebuild inline_display for all non-shadow nodes
    for (auto& node : graph.nodes) {
        if (node.shadow) continue;
        std::string s = node_type_str(node.type_id);

        auto it = shadow_map.find(node.guid);
        if (it != shadow_map.end()) {
            // Has shadow children — reconstruct inline args
            int fsa = first_shadow_arg_for(node.type_id);
            // Lvalue tokens from node.args (positions 0..fsa-1)
            std::vector<std::string> lvalue_tokens;
            if (fsa > 0 && !node.args.empty()) {
                lvalue_tokens = tokenize_args(node.args, false);
            }
            // Merge lvalue + shadow tokens in order
            int max_idx = 0;
            for (auto& si : it->second) max_idx = std::max(max_idx, si.arg_index);
            for (auto& t : lvalue_tokens) max_idx = std::max(max_idx, fsa - 1);

            std::vector<std::string> tokens(max_idx + 1);
            for (int i = 0; i < fsa && i < (int)lvalue_tokens.size(); i++)
                tokens[i] = lvalue_tokens[i];
            for (auto& si : it->second)
                tokens[si.arg_index] = si.expr;

            for (auto& t : tokens) {
                s += " " + t;
            }
        } else if (!node.args.empty()) {
            s += " " + node.args;
        }
        node.inline_display = s;
    }
}

void update_shadows_for_node(FlowGraph& graph, FlowNode& node, const std::string& new_args) {
    // 1. Remove existing shadow nodes for this parent
    std::string parent_guid = node.guid;
    std::set<std::string> shadow_guids;
    for (auto& n : graph.nodes) {
        if (!n.shadow) continue;
        // Check if this shadow connects to our parent
        std::string out0 = n.guid + ".out0";
        for (auto& link : graph.links) {
            if (link.from_pin != out0) continue;
            auto dot = link.to_pin.find('.');
            if (dot != std::string::npos && link.to_pin.substr(0, dot) == parent_guid) {
                shadow_guids.insert(n.guid);
            }
        }
    }

    // Remove shadow links and nodes
    std::erase_if(graph.links, [&](auto& l) {
        auto d1 = l.from_pin.find('.');
        auto d2 = l.to_pin.find('.');
        if (d1 != std::string::npos && shadow_guids.count(l.from_pin.substr(0, d1))) return true;
        if (d2 != std::string::npos && shadow_guids.count(l.to_pin.substr(0, d2))) return true;
        return false;
    });
    std::erase_if(graph.nodes, [&](auto& n) { return shadow_guids.count(n.guid) > 0; });

    // 2. Remove descriptor pins that were added by previous shadow generation
    if (!shadow_guids.empty()) {
        auto* nt = find_node_type(node.type_id);
        if (nt) {
            int fsa = first_shadow_arg_for(node.type_id);
            std::set<std::string> shadow_pin_names;
            for (int i = fsa; i < nt->inputs; i++) {
                if (nt->input_ports) shadow_pin_names.insert(nt->input_ports[i].name);
            }
            std::erase_if(node.inputs, [&](auto& p) {
                return shadow_pin_names.count(p->name) > 0;
            });
        }
    }

    // 3. Set new args and regenerate
    node.args = new_args;
    node.parse_args();

    // 4. Run shadow generation (will only process this node since others have empty args)
    generate_shadow_nodes(graph);

    // 5. Rebuild inline_display for all nodes
    rebuild_all_inline_display(graph);
}
