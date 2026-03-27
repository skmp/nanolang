#include "graph_builder.h"
#include "node_types2.h"
#include "expr.h"
#include <sstream>
#include <cctype>
#include <set>

// ─── TOML helpers ───

static std::string trim(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

static std::string unescape_toml(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
            case '"':  result += '"';  i++; break;
            case '\\': result += '\\'; i++; break;
            case 'n':  result += '\n'; i++; break;
            case 't':  result += '\t'; i++; break;
            case 'r':  result += '\r'; i++; break;
            default:   result += s[i]; break;
            }
        } else {
            result += s[i];
        }
    }
    return result;
}

static std::string unquote(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return unescape_toml(s.substr(1, s.size() - 2));
    return s;
}

static std::vector<std::string> parse_toml_array(const std::string& val) {
    std::vector<std::string> result;
    std::string s = trim(val);
    if (s.empty() || s.front() != '[' || s.back() != ']') return result;
    s = s.substr(1, s.size() - 2);
    std::string item;
    bool in_str = false, escaped = false;
    for (char c : s) {
        if (escaped) { item += c; escaped = false; continue; }
        if (c == '\\' && in_str) { item += c; escaped = true; continue; }
        if (c == '"') { in_str = !in_str; item += c; continue; }
        if (c == ',' && !in_str) { result.push_back(unquote(trim(item))); item.clear(); continue; }
        item += c;
    }
    if (!trim(item).empty()) result.push_back(unquote(trim(item)));
    return result;
}

// ─── NetBuilder ───

void NetBuilder::compact() {
    destinations.erase(
        std::remove_if(destinations.begin(), destinations.end(), [](auto& w) { return w.expired(); }),
        destinations.end());
}

bool NetBuilder::unused() {
    compact();
    return source.expired() && destinations.empty();
}

void NetBuilder::validate() const {
}

// ─── v2 parse/reconstruct ───

static FlowArg2 parse_token_v2(GraphBuilder& gb, const std::string& tok) {
    if (tok.empty()) return ArgString2{""};

    // Net reference: $name (non-numeric)
    if (tok[0] == '$' && tok.size() >= 2 && !std::isdigit(tok[1])) {
        auto [id, entry] = gb.find_or_create_net(tok, false);
        return ArgNet2{NodeId(id), entry};
    }

    // String literal
    if (tok.front() == '"' && tok.back() == '"' && tok.size() >= 2) {
        return ArgString2{tok.substr(1, tok.size() - 2)};
    }

    // Number
    bool is_float = false;
    bool is_number = true;
    for (size_t i = 0; i < tok.size(); i++) {
        char c = tok[i];
        if (c == '.' && !is_float) { is_float = true; continue; }
        if (c == 'f' && i == tok.size() - 1) { is_float = true; continue; }
        if (c == '-' && i == 0) continue;
        if (c < '0' || c > '9') { is_number = false; break; }
    }
    if (is_number && !tok.empty()) {
        return ArgNumber2{std::stod(tok), is_float};
    }

    // Expression (anything else)
    return ArgExpr2{tok};
}

ParseResult parse_args_v2(const std::shared_ptr<GraphBuilder>& gb,
                          const std::vector<std::string>& exprs, bool is_expr) {
    auto result = std::make_shared<ParsedArgs2>();

    // Scan all expressions for $N refs to compute rewrite_input_count
    std::set<int> slot_indices;
    for (auto& expr : exprs) {
        for (size_t i = 0; i < expr.size(); i++) {
            if (expr[i] == '$' && i + 1 < expr.size() && std::isdigit(expr[i + 1])) {
                int n = 0;
                size_t j = i + 1;
                while (j < expr.size() && std::isdigit(expr[j])) {
                    n = n * 10 + (expr[j] - '0');
                    j++;
                }
                slot_indices.insert(n);
            }
        }
    }

    // Validate contiguous from 0
    if (!slot_indices.empty()) {
        int max_slot = *slot_indices.rbegin();
        if ((int)slot_indices.size() != max_slot + 1) {
            std::string missing;
            for (int i = 0; i <= max_slot; i++) {
                if (!slot_indices.count(i)) {
                    if (!missing.empty()) missing += ", ";
                    missing += "$" + std::to_string(i);
                }
            }
            return std::string("Missing pin reference(s): " + missing);
        }
        result->rewrite_input_count = max_slot + 1;
    }

    for (auto& expr : exprs) {
        result->push_back(parse_token_v2(*gb, expr));
    }
    return result;
}

std::string reconstruct_args_str(const ParsedArgs2& args) {
    std::string result;
    for (auto& a : args) {
        if (!result.empty()) result += " ";
        std::visit([&](auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, ArgNet2>) result += v.first;
            else if constexpr (std::is_same_v<T, ArgNumber2>) {
                if (v.is_float) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%g", v.value);
                    result += buf;
                } else {
                    result += std::to_string((long long)v.value);
                }
            }
            else if constexpr (std::is_same_v<T, ArgString2>) result += "\"" + v.value + "\"";
            else if constexpr (std::is_same_v<T, ArgExpr2>) result += v.expr;
        }, a);
    }
    return result;
}

// ─── FlowNodeBuilder ───

std::string FlowNodeBuilder::args_str() const {
    std::string result;
    if (parsed_args) result = reconstruct_args_str(*parsed_args);
    if (parsed_va_args && !parsed_va_args->empty()) {
        std::string va = reconstruct_args_str(*parsed_va_args);
        if (!va.empty()) {
            if (!result.empty()) result += " ";
            result += va;
        }
    }
    return result;
}

// ─── GraphBuilder ───

std::shared_ptr<FlowNodeBuilder> GraphBuilder::add_node(NodeId id, NodeTypeID type, std::shared_ptr<ParsedArgs2> args) {
    auto nb = std::make_shared<FlowNodeBuilder>();
    nb->type_id = type;
    nb->parsed_args = std::move(args);
    nb->id = id;
    entries[std::move(id)] = nb;
    return nb;
}

void GraphBuilder::ensure_unconnected() {
    if (entries.count("$unconnected")) return;
    auto net = std::make_shared<NetBuilder>();
    net->is_the_unconnected = true;
    net->auto_wire = true;
    net->id = "$unconnected";
    entries["$unconnected"] = net;
}

std::pair<NodeId, BuilderEntryPtr> GraphBuilder::find_or_create_net(const NodeId& name, bool for_source) {
    auto it = entries.find(name);
    if (it != entries.end()) {
        if (auto net = it->second->as_Net()) {
            if (for_source && !net->source.expired())
                throw std::logic_error("find_or_create_net(\"" + name + "\"): net already has a source");
            return {it->first, it->second};
        }
        // Exists as a node — don't overwrite
        return {it->first, nullptr};
    }
    auto net = std::make_shared<NetBuilder>();
    net->auto_wire = (name.size() >= 6 && name.substr(0, 6) == "$auto-");
    net->id = name;
    entries[name] = net;
    return {entries.find(name)->first, net};
}

BuilderEntryPtr GraphBuilder::find(const NodeId& id) {
    auto it = entries.find(id);
    return (it != entries.end()) ? it->second : nullptr;
}

FlowNodeBuilderPtr GraphBuilder::find_node(const NodeId& id) {
    auto it = entries.find(id);
    if (it == entries.end()) return nullptr;
    return it->second->as_Node();
}

NetBuilderPtr GraphBuilder::find_net(const NodeId& name) {
    auto it = entries.find(name);
    if (it == entries.end()) return nullptr;
    return it->second->as_Net();
}

void GraphBuilder::compact() {
    for (auto it = entries.begin(); it != entries.end(); ) {
        if (auto net = it->second->as_Net()) {
            if (!net->is_the_unconnected && net->unused()) {
                it = entries.erase(it);
                continue;
            }
        }
        ++it;
    }
}

NodeId GraphBuilder::next_id() {
    for (int n = 0; ; n++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "$a-%x", n);
        if (!entries.count(buf)) return buf;
    }
}

// ─── Deserializer ───

BuilderResult Deserializer::parse_node(
    const std::shared_ptr<GraphBuilder>& gb,
    const NodeId& id, const std::string& type, const std::vector<std::string>& args) {

    NodeTypeID type_id = node_type_id_from_string(type.c_str());

    if (type_id == NodeTypeID::Unknown) {
        return BuilderError("Unknown node type: " + type);
    }

    FlowNodeBuilder nb;
    nb.type_id = type_id;

    if (is_any_of(type_id, NodeTypeID::Label, NodeTypeID::Error)) {
        if (args.size() != 1)
            throw std::invalid_argument("Label/Error node requires exactly 1 argument, got " + std::to_string(args.size()));
        nb.parsed_args = std::make_shared<ParsedArgs2>();
        nb.parsed_args->push_back(ArgString2{args[0]});
        return std::pair{id, std::move(nb)};
    }

    bool is_expr = is_any_of(type_id, NodeTypeID::Expr, NodeTypeID::ExprBang);

    auto parse_result = parse_args_v2(gb, args, is_expr);
    if (auto* err = std::get_if<std::string>(&parse_result)) {
        return BuilderError(*err);
    }

    nb.parsed_args = std::get<std::shared_ptr<ParsedArgs2>>(std::move(parse_result));
    return std::pair{id, std::move(nb)};
}

FlowNodeBuilder& Deserializer::parse_or_error(
    const std::shared_ptr<GraphBuilder>& gb,
    const NodeId& id, const std::string& type, const std::vector<std::string>& args) {

    auto result = parse_node(gb, id, type, args);

    if (auto* p = std::get_if<std::pair<NodeId, FlowNodeBuilder>>(&result)) {
        auto entry = std::make_shared<FlowNodeBuilder>(std::move(p->second));
        entry->id = p->first;
        gb->entries[p->first] = entry;
        return *entry;
    }

    auto& error_msg = std::get<BuilderError>(result);
    std::string args_joined;
    for (auto& a : args) {
        if (!args_joined.empty()) args_joined += " ";
        args_joined += a;
    }
    auto entry = std::make_shared<FlowNodeBuilder>();
    entry->type_id = NodeTypeID::Error;
    entry->parsed_args = std::make_shared<ParsedArgs2>();
    entry->parsed_args->push_back(ArgString2{type + " " + args_joined});
    entry->error = error_msg;
    entry->id = id;
    gb->entries[id] = entry;
    return *entry;
}

// ─── parse_atto ───

Deserializer::ParseAttoResult Deserializer::parse_atto(std::istream& f) {
    std::string first_line;
    while (std::getline(f, first_line)) {
        first_line = trim(first_line);
        if (!first_line.empty()) break;
    }
    if (first_line != "# version instrument@atto:0") {
        return BuilderError("Expected '# version instrument@atto:0', got: " + first_line);
    }

    auto gb = std::make_shared<GraphBuilder>();
    gb->ensure_unconnected();

    bool in_node = false;
    std::string cur_id, cur_type;
    std::vector<std::string> cur_args;
    std::vector<std::string> cur_inputs, cur_outputs;
    float cur_x = 0, cur_y = 0;
    bool cur_shadow = false;

    // Track shadow input nets for remap construction during folding
    std::map<NodeId, std::vector<std::string>> shadow_input_nets; // shadow_id → input net names

    auto flush_node = [&]() {
        if (cur_type.empty()) {
            cur_id.clear(); cur_args.clear(); cur_inputs.clear(); cur_outputs.clear();
            return;
        }

        if (cur_id.empty()) {
            cur_id = "$auto-" + generate_guid();
        }

        auto& nb = parse_or_error(gb, cur_id, cur_type, cur_args);
        nb.position = {cur_x, cur_y};
        nb.shadow = cur_shadow;

        // Save shadow input nets for later folding
        if (cur_shadow) {
            shadow_input_nets[cur_id] = cur_inputs;
        }

        auto node_entry = gb->find(cur_id);

        // Wire nets from outputs — smart map old positions to new descriptor order
        // Old outputs array: [nexts..., data_outs..., post_bang, lambda_grab]
        // New: nb.outputs[i] = ArgNet2 for new output_ports[i]
        {
            auto* old_nt = find_node_type(cur_type.c_str());
            auto* new_nt = find_node_type2(nb.type_id);
            bool is_expr = is_any_of(nb.type_id, NodeTypeID::Expr, NodeTypeID::ExprBang);
            int old_num_nexts = old_nt ? old_nt->num_nexts : 0;

            // Helper: wire a net and return ArgNet2
            auto wire_output = [&](const std::string& net_name) -> ArgNet2 {
                auto [resolved, net_ptr] = gb->find_or_create_net(net_name, true);
                if (auto net = net_ptr ? net_ptr->as_Net() : nullptr)
                    net->source = node_entry;
                return {resolved, net_ptr};
            };

            // Filter out empty and -as_lambda entries, wire all nets
            // For expr: outputs are all data (no nexts), dynamic count
            // For others: [nexts..., data_outs..., post_bang]
            if (is_expr) {
                // Expr: positional mapping — all outputs are data, last may be post_bang
                for (int i = 0; i < (int)cur_outputs.size(); i++) {
                    auto& net_name = cur_outputs[i];
                    if (net_name.empty()) continue;
                    // Check for post_bang suffix
                    bool is_post_bang = (net_name.size() > 10 &&
                        net_name.compare(net_name.size() - 10, 10, "-post_bang") == 0);
                    if (net_name.size() > 10 && net_name.compare(net_name.size() - 10, 10, "-as_lambda") == 0)
                        continue;
                    auto arg = wire_output(net_name);
                    // post_bang is side-bang for flow nodes — don't add to outputs array
                    // (it's implicit from NodeKind2::Flow)
                    if (!is_post_bang) {
                        while ((int)nb.outputs.size() <= i)
                            nb.outputs.push_back({"$unconnected", gb->find("$unconnected")});
                        nb.outputs[i] = std::move(arg);
                    }
                }
            } else {
                // Name-based mapping for non-expr nodes
                int old_num_outs = old_nt ? old_nt->outputs : 0;
                std::map<std::string, ArgNet2> out_net_map;

                for (int i = 0; i < (int)cur_outputs.size(); i++) {
                    auto& net_name = cur_outputs[i];
                    if (net_name.empty()) continue;
                    if (net_name.size() > 10 && net_name.compare(net_name.size() - 10, 10, "-as_lambda") == 0)
                        continue;

                    auto arg = wire_output(net_name);

                    // Determine old pin name from position
                    std::string old_pin_name;
                    if (i < old_num_nexts) {
                        old_pin_name = (old_nt && old_nt->next_ports) ? old_nt->next_ports[i].name : "bang";
                    } else if (i < old_num_nexts + old_num_outs) {
                        int out_idx = i - old_num_nexts;
                        old_pin_name = (old_nt && old_nt->output_ports) ? old_nt->output_ports[out_idx].name : "result";
                    } else {
                        old_pin_name = "post_bang";
                    }

                    out_net_map[old_pin_name] = std::move(arg);
                }

                // Map to new descriptor order
                if (new_nt) {
                    nb.outputs.resize(new_nt->num_outputs);
                    auto unconnected = gb->find("$unconnected");
                    for (int i = 0; i < new_nt->num_outputs; i++) {
                        const char* name = new_nt->output_ports[i].name;
                        auto it = out_net_map.find(name);
                        if (it != out_net_map.end()) {
                            nb.outputs[i] = std::move(it->second);
                        } else if (strcmp(name, "next") == 0) {
                            auto it2 = out_net_map.find("bang");
                            if (it2 != out_net_map.end())
                                nb.outputs[i] = std::move(it2->second);
                            else
                                nb.outputs[i] = {"$unconnected", unconnected};
                        } else {
                            nb.outputs[i] = {"$unconnected", unconnected};
                        }
                    }
                }
            }
        }

        // ─── v0 → v1 port mapping: merge inputs + args by port name ───
        if (!cur_inputs.empty() && !cur_shadow) {
            auto* old_nt = find_node_type(cur_type.c_str());
            auto* new_nt = find_node_type2(nb.type_id);
            bool is_expr = is_any_of(nb.type_id, NodeTypeID::Expr, NodeTypeID::ExprBang);
            bool args_are_type = is_any_of(nb.type_id, NodeTypeID::Cast, NodeTypeID::New);

            // Helper: resolve net/node name to ArgNet2 and register destination
            auto resolve_net = [&](const std::string& net_name) -> ArgNet2 {
                if (net_name.empty()) {
                    auto [resolved, ptr] = gb->find_or_create_net("$unconnected");
                    return {resolved, ptr};
                }
                // Strip -as_lambda suffix → resolve to node entry directly
                std::string resolved_name = net_name;
                if (resolved_name.size() > 10 &&
                    resolved_name.compare(resolved_name.size() - 10, 10, "-as_lambda") == 0) {
                    resolved_name.resize(resolved_name.size() - 10);
                }
                // Try finding as any entry (node or net)
                auto ptr = gb->find(resolved_name);
                if (ptr) {
                    // If it's a net, register as destination
                    if (auto net = ptr->as_Net())
                        net->destinations.push_back(node_entry);
                    return {resolved_name, ptr};
                }
                // Not found yet — create as net
                auto [id, net_ptr] = gb->find_or_create_net(resolved_name);
                net_ptr->as_Net()->destinations.push_back(node_entry);
                return {id, net_ptr};
            };

            if (is_expr) {
                // Expr nodes: inputs map to $N remaps, not descriptor ports
                // For expr!, inputs[0] is the bang trigger, rest are $N
                int bang_offset = is_any_of(nb.type_id, NodeTypeID::ExprBang) ? 1 : 0;
                for (int i = 0; i < (int)cur_inputs.size(); i++) {
                    auto arg = resolve_net(cur_inputs[i]);
                    if (i < bang_offset) {
                        // Bang trigger → prepend to parsed_args
                        if (!nb.parsed_args) nb.parsed_args = std::make_shared<ParsedArgs2>();
                        nb.parsed_args->insert(nb.parsed_args->begin(), std::move(arg));
                    } else {
                        // $N remap
                        int remap_idx = i - bang_offset;
                        while ((int)nb.remaps.size() <= remap_idx)
                            nb.remaps.push_back({"$unconnected", gb->find("$unconnected")});
                        nb.remaps[remap_idx] = std::move(arg);
                    }
                }
            } else if (!old_nt || !new_nt) {
                // Unknown types: simple positional prepend
                auto merged = std::make_shared<ParsedArgs2>();
                for (auto& net_name : cur_inputs)
                    merged->push_back(resolve_net(net_name));
                if (nb.parsed_args) {
                    for (auto& a : *nb.parsed_args)
                        merged->push_back(std::move(a));
                    merged->rewrite_input_count = nb.parsed_args->rewrite_input_count;
                }
                nb.parsed_args = std::move(merged);
            } else {
                // Name-based mapping using old and new descriptors

                // Step 1: Build old pin name list (matching inputs array order)
                std::vector<std::string> old_pin_names;
                // Triggers first
                for (int i = 0; i < old_nt->num_triggers; i++) {
                    if (old_nt->trigger_ports)
                        old_pin_names.push_back(old_nt->trigger_ports[i].name);
                    else
                        old_pin_names.push_back("bang_in");
                }
                // Data pins depend on args
                std::string args_joined;
                for (auto& a : cur_args) {
                    if (!args_joined.empty()) args_joined += " ";
                    args_joined += a;
                }

                if (args_are_type) {
                    // Type nodes: all descriptor inputs become pins
                    for (int i = 0; i < old_nt->inputs; i++) {
                        if (old_nt->input_ports && i < old_nt->inputs)
                            old_pin_names.push_back(old_nt->input_ports[i].name);
                        else
                            old_pin_names.push_back(std::to_string(i));
                    }
                } else {
                    auto info = compute_inline_args(args_joined, old_nt->inputs);
                    // $N ref pins first
                    int ref_pins = (info.pin_slots.max_slot >= 0) ? (info.pin_slots.max_slot + 1) : 0;
                    for (int i = 0; i < ref_pins; i++) {
                        bool is_lambda = info.pin_slots.is_lambda_slot(i);
                        old_pin_names.push_back(is_lambda ? ("@" + std::to_string(i)) : std::to_string(i));
                    }
                    // Remaining descriptor pins
                    for (int i = info.num_inline_args; i < old_nt->inputs; i++) {
                        if (old_nt->input_ports && i < old_nt->inputs)
                            old_pin_names.push_back(old_nt->input_ports[i].name);
                        else
                            old_pin_names.push_back(std::to_string(i));
                    }
                }

                // Step 2: Build port_name → ArgNet2 map from inputs array
                std::map<std::string, ArgNet2> net_map;
                for (int i = 0; i < (int)cur_inputs.size() && i < (int)old_pin_names.size(); i++) {
                    net_map[old_pin_names[i]] = resolve_net(cur_inputs[i]);
                }

                // Step 3: Build port_name → parsed_value map from inlined args
                // Inlined args cover input_ports[0..num_inline_args-1]
                std::map<std::string, FlowArg2> inline_map;
                if (!args_are_type && nb.parsed_args) {
                    auto info = compute_inline_args(args_joined, old_nt->inputs);
                    int num_inline = std::min(info.num_inline_args, old_nt->inputs);
                    for (int i = 0; i < num_inline && i < (int)nb.parsed_args->size(); i++) {
                        if (old_nt->input_ports && i < old_nt->inputs)
                            inline_map[old_nt->input_ports[i].name] = std::move((*nb.parsed_args)[i]);
                    }
                }

                // Step 4: Build unified parsed_args in new descriptor order
                auto merged = std::make_shared<ParsedArgs2>();
                if (nb.parsed_args)
                    merged->rewrite_input_count = nb.parsed_args->rewrite_input_count;

                // Helper: find value by port name with fallback for bang→bang_in rename
                auto find_by_name = [&](const char* name) -> std::pair<bool, FlowArg2> {
                    auto net_it = net_map.find(name);
                    if (net_it != net_map.end())
                        return {true, std::move(net_it->second)};
                    auto inline_it = inline_map.find(name);
                    if (inline_it != inline_map.end())
                        return {true, std::move(inline_it->second)};
                    // Fallback: old "bang" → new "bang_in"
                    if (strcmp(name, "bang_in") == 0) {
                        auto it2 = net_map.find("bang");
                        if (it2 != net_map.end())
                            return {true, std::move(it2->second)};
                    }
                    return {false, {}};
                };

                // Pass 1: fill by name matching
                std::vector<bool> filled(new_nt->total_inputs(), false);
                for (int i = 0; i < new_nt->total_inputs(); i++) {
                    auto [found, value] = find_by_name(new_nt->input_port(i)->name);
                    if (found) {
                        merged->push_back(std::move(value));
                        filled[i] = true;
                    } else {
                        merged->push_back(resolve_net("")); // placeholder
                    }
                }

                // Pass 2: fill unfilled non-bang slots from unconsumed parsed_args
                // inline_map consumed parsed_args[0..num_inline-1]; rest are available
                if (nb.parsed_args) {
                    int consumed = 0;
                    if (!args_are_type) {
                        auto info2 = compute_inline_args(args_joined, old_nt->inputs);
                        consumed = std::min(info2.num_inline_args, (int)nb.parsed_args->size());
                        consumed = std::min(consumed, old_nt->inputs);
                    }
                    int arg_cursor = consumed;
                    for (int i = 0; i < new_nt->total_inputs(); i++) {
                        if (!filled[i] && new_nt->input_port(i)->kind != PortKind2::BangTrigger) {
                            if (arg_cursor < (int)nb.parsed_args->size()) {
                                (*merged)[i] = std::move((*nb.parsed_args)[arg_cursor++]);
                                filled[i] = true;
                            }
                        }
                    }
                    // Remaining args beyond descriptor slots → appended (for va_args split later)
                    for (; arg_cursor < (int)nb.parsed_args->size(); arg_cursor++)
                        merged->push_back(std::move((*nb.parsed_args)[arg_cursor]));
                }

                nb.parsed_args = std::move(merged);
            }
        } else if (!cur_inputs.empty() && cur_shadow) {
            // Shadows: inputs wired as net destinations (handled during folding)
            for (auto& net_name : cur_inputs) {
                if (net_name.empty()) continue;
                auto [_, net_ptr] = gb->find_or_create_net(net_name);
                if (auto net = net_ptr ? net_ptr->as_Net() : nullptr)
                    net->destinations.push_back(node_entry);
            }
        }

        // Ensure remaps are sized to rewrite_input_count (from $N refs in expressions)
        if (nb.parsed_args && nb.parsed_args->rewrite_input_count > (int)nb.remaps.size()) {
            auto unconnected = gb->find("$unconnected");
            while ((int)nb.remaps.size() < nb.parsed_args->rewrite_input_count)
                nb.remaps.push_back({"$unconnected", unconnected});
        }

        // Trim trailing $unconnected optional ports from parsed_args
        // Optional ports are always trailing: anything beyond num_inputs is optional
        {
            auto* trim_nt = find_node_type2(nb.type_id);
            if (trim_nt && nb.parsed_args) {
                while ((int)nb.parsed_args->size() > trim_nt->num_inputs) {
                    auto* an = std::get_if<ArgNet2>(&nb.parsed_args->back());
                    if (!an || an->first != "$unconnected") break;
                    nb.parsed_args->pop_back();
                }
            }
        }

        cur_id.clear(); cur_type.clear(); cur_args.clear();
        cur_inputs.clear(); cur_outputs.clear();
        cur_x = 0; cur_y = 0; cur_shadow = false;
    };

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || (line[0] == '#' && line.find("# version") != 0)) continue;

        if (line == "[[node]]") {
            flush_node();
            in_node = true;
            continue;
        }

        if (line.find("# version") == 0) continue;
        if (!in_node) continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (key == "id") { cur_id = unquote(val); }
        else if (key == "type") { cur_type = unquote(val); }
        else if (key == "args") { cur_args = parse_toml_array(val); }
        else if (key == "shadow") { cur_shadow = (unquote(val) == "true"); }
        else if (key == "inputs") { cur_inputs = parse_toml_array(val); }
        else if (key == "outputs") { cur_outputs = parse_toml_array(val); }
        else if (key == "position") {
            auto coords = parse_toml_array(val);
            if (coords.size() >= 2) {
                cur_x = std::stof(coords[0]);
                cur_y = std::stof(coords[1]);
            }
        }
    }
    flush_node();

    // ─── Re-resolve ArgNet2 entries pointing to stale placeholders ───
    // When a lambda capture references a node parsed later in the file,
    // resolve_net creates a NetBuilder placeholder. Now re-resolve to the actual node.
    {
        auto fixup_args = [&](ParsedArgs2* pa) {
            if (!pa) return;
            for (auto& a : *pa) {
                if (auto* an = std::get_if<ArgNet2>(&a)) {
                    if (!an->second || !an->second->as_Net()) continue;
                    auto actual = gb->find(an->first);
                    if (actual && actual->as_Node())
                        an->second = actual;
                }
            }
        };
        for (auto& [id, entry] : gb->entries) {
            auto node_p = entry->as_Node();
            if (!node_p) continue;
            fixup_args(node_p->parsed_args.get());
            fixup_args(node_p->parsed_va_args.get());
        }
    }

    // ─── Fold shadow nodes into parents ───
    auto unconnected_entry = gb->find("$unconnected");

    // Collect shadow ids
    std::vector<NodeId> shadow_ids;
    for (auto& [id, entry] : gb->entries) {
        auto node_p = entry->as_Node();
        if (!node_p) continue;
        if (node_p->shadow)
            shadow_ids.push_back(id);
    }

    for (auto& shadow_id : shadow_ids) {
        // Extract parent id and arg index: "$auto-xyz_s0" → "$auto-xyz", 0
        auto underscore_s = shadow_id.rfind("_s");
        if (underscore_s == std::string::npos) continue;
        std::string parent_id = shadow_id.substr(0, underscore_s);
        int arg_index = std::stoi(shadow_id.substr(underscore_s + 2));

        auto parent_ptr = gb->find_node(parent_id);
        if (!parent_ptr) continue;

        auto shadow_entry = gb->find(shadow_id);
        auto shadow_ptr = shadow_entry ? shadow_entry->as_Node() : nullptr;
        if (!shadow_ptr) continue;

        // Insert shadow expression into parent's parsed_args
        // Find the shadow's output net (e.g. "$auto-xxx_s0-out0") in parent's parsed_args and replace
        if (parent_ptr->parsed_args && shadow_ptr->parsed_args && !shadow_ptr->parsed_args->empty()) {
            std::string shadow_out_prefix = shadow_id + "-out";
            bool replaced = false;
            for (auto& a : *parent_ptr->parsed_args) {
                if (auto* an = std::get_if<ArgNet2>(&a)) {
                    if (an->first.compare(0, shadow_out_prefix.size(), shadow_out_prefix) == 0) {
                        a = (*shadow_ptr->parsed_args)[0];
                        replaced = true;
                        break;
                    }
                }
            }
            // Fallback: try positional insertion (for nodes without merged inputs)
            if (!replaced) {
                while ((int)parent_ptr->parsed_args->size() <= arg_index)
                    parent_ptr->parsed_args->push_back(ArgString2{""});
                (*parent_ptr->parsed_args)[arg_index] = (*shadow_ptr->parsed_args)[0];
            }
        }

        // Build remaps from saved shadow input nets
        auto sin_it = shadow_input_nets.find(shadow_id);
        if (sin_it != shadow_input_nets.end()) {
            auto& sin = sin_it->second;
            for (int i = 0; i < (int)sin.size(); i++) {
                while ((int)parent_ptr->remaps.size() <= i)
                    parent_ptr->remaps.push_back(ArgNet2{"$unconnected", unconnected_entry});

                if (!sin[i].empty()) {
                    auto net_ptr = gb->find(sin[i]);
                    if (net_ptr) {
                        parent_ptr->remaps[i] = ArgNet2{sin[i], net_ptr};

                        if (auto net = net_ptr->as_Net()) {
                            auto& dests = net->destinations;
                            dests.erase(
                                std::remove_if(dests.begin(), dests.end(),
                                    [&](auto& w) { return w.lock() == shadow_entry; }),
                                dests.end());
                            net->destinations.push_back(parent_ptr);
                        }
                    }
                }
            }
            if (parent_ptr->parsed_args) {
                parent_ptr->parsed_args->rewrite_input_count = std::max(
                    parent_ptr->parsed_args->rewrite_input_count, (int)parent_ptr->remaps.size());
            }
        }

        // Remove nets where shadow is source (internal shadow→parent plumbing)
        std::vector<NodeId> nets_to_remove;
        for (auto& [net_id, net_entry] : gb->entries) {
            auto net_as = net_entry->as_Net();
            if (!net_as) continue;
            auto src = net_as->source.lock();
            if (src == shadow_entry)
                nets_to_remove.push_back(net_id);
        }
        for (auto& nid : nets_to_remove)
            gb->entries.erase(nid);

        // Remove shadow from graph
        gb->entries.erase(shadow_id);
    }

    // ─── Split parsed_args into base + va_args for nodes with va_args ───
    for (auto& [id, entry] : gb->entries) {
        if (!entry->is(IdCategory::Node)) continue;
        auto& node = *entry->as_Node();
        auto* nt = find_node_type2(node.type_id);
        if (!nt || !nt->va_args || !node.parsed_args) continue;

        // Split at total descriptor input count (required + optional)
        int fixed_args = nt->total_inputs();

        if ((int)node.parsed_args->size() > fixed_args) {
            node.parsed_va_args = std::make_shared<ParsedArgs2>();
            for (int i = fixed_args; i < (int)node.parsed_args->size(); i++)
                node.parsed_va_args->push_back(std::move((*node.parsed_args)[i]));
            node.parsed_args->resize(fixed_args);
        }
    }

    // ─── Re-ID: $auto-xxx → $a-N (compact hex IDs) ───
    {
        // Build rename map for $auto- entries
        std::map<std::string, std::string> rename;
        int next_id = 0;
        for (auto& [id, _] : gb->entries) {
            if (id.compare(0, 6, "$auto-") == 0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "$a-%x", next_id++);
                rename[id] = buf;
            }
        }

        // Also rename net names that start with $auto- but aren't in entries
        // (they appear as ArgNet2 first-values referencing $auto- prefixed names)

        // Helper: rename an id if it has a mapping
        auto remap_id = [&](const std::string& id) -> std::string {
            // Check exact match
            auto it = rename.find(id);
            if (it != rename.end()) return it->second;
            // Check if it starts with a known $auto- prefix (e.g. "$auto-xxx-out0")
            // Find the longest matching prefix
            for (auto& [old_prefix, new_prefix] : rename) {
                if (id.size() > old_prefix.size() && id.compare(0, old_prefix.size(), old_prefix) == 0) {
                    // Check the char after the prefix is a separator
                    char sep = id[old_prefix.size()];
                    if (sep == '-' || sep == '_') {
                        return new_prefix + id.substr(old_prefix.size());
                    }
                }
            }
            return id;
        };

        // Helper: rename ArgNet2 in-place
        auto remap_arg = [&](FlowArg2& a) {
            if (auto* an = std::get_if<ArgNet2>(&a))
                an->first = remap_id(an->first);
        };
        auto remap_args = [&](ParsedArgs2* pa) {
            if (!pa) return;
            for (auto& a : *pa) remap_arg(a);
        };

        // Rename all references inside nodes
        for (auto& [id, entry] : gb->entries) {
            auto node_p = entry->as_Node();
            if (!node_p) continue;
            remap_args(node_p->parsed_args.get());
            remap_args(node_p->parsed_va_args.get());
            for (auto& r : node_p->remaps) r.first = remap_id(r.first);
            for (auto& o : node_p->outputs) o.first = remap_id(o.first);
        }

        // Rebuild entries map with new keys
        std::map<NodeId, BuilderEntryPtr> new_entries;
        for (auto& [id, entry] : gb->entries) {
            new_entries[remap_id(id)] = std::move(entry);
        }
        gb->entries = std::move(new_entries);
    }

    gb->compact();

    return gb;
}
