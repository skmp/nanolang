#include "graphbuilder.h"
#include "node_types2.h"
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

// ─── Validation helper ───

static void validate_weak_is_node(const BuilderEntryWeak& w) {
    auto p = w.lock();
    if (!p) return; // expired is ok
    if (!std::holds_alternative<FlowNodeBuilder>(*p))
        throw std::logic_error("NetBuilder: weak ref points to a NetBuilder, not a FlowNodeBuilder");
}

// ─── NetBuilder ───

void NetBuilder::compact() {
    validate();
    destinations.erase(
        std::remove_if(destinations.begin(), destinations.end(), [](auto& w) { return w.expired(); }),
        destinations.end());
}

bool NetBuilder::unused() {
    compact();
    return source.expired() && destinations.empty();
}

void NetBuilder::validate() const {
    validate_weak_is_node(source);
    for (auto& d : destinations)
        validate_weak_is_node(d);
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

FlowNodeBuilder& GraphBuilder::add_node(NodeId id, NodeTypeID type, std::shared_ptr<ParsedArgs2> args) {
    auto entry = std::make_shared<BuilderEntry>(FlowNodeBuilder{});
    auto& nb = std::get<FlowNodeBuilder>(*entry);
    nb.type_id = type;
    nb.parsed_args = std::move(args);
    entries[std::move(id)] = entry;
    return nb;
}

void GraphBuilder::ensure_unconnected() {
    if (entries.count("$unconnected")) return;
    auto entry = std::make_shared<BuilderEntry>(NetBuilder{});
    auto& net = std::get<NetBuilder>(*entry);
    net.is_the_unconnected = true;
    net.auto_wire = true;
    entries["$unconnected"] = entry;
}

std::pair<NodeId, BuilderEntryPtr> GraphBuilder::find_or_create_net(const NodeId& name, bool for_source) {
    auto [id, ptr] = find_net(name); // throws if name exists as a node
    if (ptr) {
        if (for_source && !std::get<NetBuilder>(*ptr).source.expired())
            throw std::logic_error("find_or_create_net(\"" + name + "\"): net already has a source");
        return {id, ptr};
    }
    auto entry = std::make_shared<BuilderEntry>(NetBuilder{});
    auto& net = std::get<NetBuilder>(*entry);
    net.auto_wire = (name.size() >= 6 && name.substr(0, 6) == "$auto-");
    entries[name] = entry;
    return {entries.find(name)->first, entry};
}

BuilderEntryPtr GraphBuilder::find(const NodeId& id) {
    auto it = entries.find(id);
    return (it != entries.end()) ? it->second : nullptr;
}

std::pair<NodeId, BuilderEntryPtr> GraphBuilder::find_node(const NodeId& id) {
    auto it = entries.find(id);
    if (it == entries.end()) return {id, nullptr};
    if (!std::holds_alternative<FlowNodeBuilder>(*it->second))
        throw std::logic_error("find_node(\"" + id + "\"): entry exists but is a NetBuilder, not a FlowNodeBuilder");
    return {it->first, it->second};
}

std::pair<NodeId, BuilderEntryPtr> GraphBuilder::find_net(const NodeId& name) {
    auto it = entries.find(name);
    if (it == entries.end()) return {name, nullptr};
    if (!std::holds_alternative<NetBuilder>(*it->second))
        throw std::logic_error("find_net(\"" + name + "\"): entry exists but is a FlowNodeBuilder, not a NetBuilder");
    return {it->first, it->second};
}

void GraphBuilder::compact() {
    for (auto it = entries.begin(); it != entries.end(); ) {
        if (std::holds_alternative<NetBuilder>(*it->second)) {
            auto& net = std::get<NetBuilder>(*it->second);
            if (!net.is_the_unconnected && net.unused()) {
                it = entries.erase(it);
                continue;
            }
        }
        ++it;
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
        auto entry = std::make_shared<BuilderEntry>(std::move(p->second));
        gb->entries[p->first] = entry;
        return std::get<FlowNodeBuilder>(*entry);
    }

    auto& error_msg = std::get<BuilderError>(result);
    std::string args_joined;
    for (auto& a : args) {
        if (!args_joined.empty()) args_joined += " ";
        args_joined += a;
    }
    FlowNodeBuilder nb;
    nb.type_id = NodeTypeID::Error;
    nb.parsed_args = std::make_shared<ParsedArgs2>();
    nb.parsed_args->push_back(ArgString2{type + " " + args_joined});
    nb.error = error_msg;
    auto entry = std::make_shared<BuilderEntry>(std::move(nb));
    gb->entries[id] = entry;
    return std::get<FlowNodeBuilder>(*entry);
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

        // Wire nets from outputs (this node is source)
        for (auto& net_name : cur_outputs) {
            if (net_name.empty()) continue;
            auto [_, net_ptr] = gb->find_or_create_net(net_name, true);
            std::get<NetBuilder>(*net_ptr).source = node_entry;
        }
        // Wire nets from inputs (this node is destination)
        for (auto& net_name : cur_inputs) {
            if (net_name.empty()) continue;
            auto [_, net_ptr] = gb->find_or_create_net(net_name);
            std::get<NetBuilder>(*net_ptr).destinations.push_back(node_entry);
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

    // ─── Fold shadow nodes into parents ───
    auto unconnected_entry = gb->find("$unconnected");

    // Collect shadow ids
    std::vector<NodeId> shadow_ids;
    for (auto& [id, entry] : gb->entries) {
        if (!std::holds_alternative<FlowNodeBuilder>(*entry)) continue;
        if (std::get<FlowNodeBuilder>(*entry).shadow)
            shadow_ids.push_back(id);
    }

    for (auto& shadow_id : shadow_ids) {
        // Extract parent id and arg index: "$auto-xyz_s0" → "$auto-xyz", 0
        auto underscore_s = shadow_id.rfind("_s");
        if (underscore_s == std::string::npos) continue;
        std::string parent_id = shadow_id.substr(0, underscore_s);
        int arg_index = std::stoi(shadow_id.substr(underscore_s + 2));

        auto [_, parent_ptr] = gb->find_node(parent_id);
        if (!parent_ptr) continue;
        auto& parent = std::get<FlowNodeBuilder>(*parent_ptr);

        auto shadow_ptr = gb->find(shadow_id);
        if (!shadow_ptr) continue;
        auto& shadow = std::get<FlowNodeBuilder>(*shadow_ptr);

        // Insert shadow expression into parent's parsed_args at arg_index
        if (parent.parsed_args) {
            while ((int)parent.parsed_args->size() <= arg_index)
                parent.parsed_args->push_back(ArgString2{""});
            if (shadow.parsed_args && !shadow.parsed_args->empty())
                (*parent.parsed_args)[arg_index] = (*shadow.parsed_args)[0];
        }

        // Build remaps from saved shadow input nets
        auto sin_it = shadow_input_nets.find(shadow_id);
        if (sin_it != shadow_input_nets.end()) {
            auto& sin = sin_it->second;
            // Shadow is an expr — no triggers, so inputs[i] maps directly to $i
            for (int i = 0; i < (int)sin.size(); i++) {
                // Ensure parent remaps is large enough
                while ((int)parent.remaps.size() <= i)
                    parent.remaps.push_back(ArgNet2{"$unconnected", unconnected_entry});

                if (!sin[i].empty()) {
                    auto net_ptr = gb->find(sin[i]);
                    if (net_ptr) {
                        parent.remaps[i] = ArgNet2{sin[i], net_ptr};

                        // Remove shadow from net's destinations, add parent instead
                        if (std::holds_alternative<NetBuilder>(*net_ptr)) {
                            auto& net = std::get<NetBuilder>(*net_ptr);
                            auto& dests = net.destinations;
                            dests.erase(
                                std::remove_if(dests.begin(), dests.end(),
                                    [&](auto& w) { return w.lock() == shadow_ptr; }),
                                dests.end());
                            net.destinations.push_back(parent_ptr);
                        }
                    }
                }
            }
            // Update parent's rewrite_input_count to be the max across all shadows
            if (parent.parsed_args) {
                parent.parsed_args->rewrite_input_count = std::max(
                    parent.parsed_args->rewrite_input_count, (int)parent.remaps.size());
            }
        }

        // Remove nets where shadow is source (internal shadow→parent plumbing)
        std::vector<NodeId> nets_to_remove;
        for (auto& [net_id, net_entry] : gb->entries) {
            if (!std::holds_alternative<NetBuilder>(*net_entry)) continue;
            auto src = std::get<NetBuilder>(*net_entry).source.lock();
            if (src == shadow_ptr)
                nets_to_remove.push_back(net_id);
        }
        for (auto& nid : nets_to_remove)
            gb->entries.erase(nid);

        // Remove shadow from graph
        gb->entries.erase(shadow_id);
    }

    // ─── Split parsed_args into base + va_args for nodes with va_args ───
    for (auto& [id, entry] : gb->entries) {
        if (!std::holds_alternative<FlowNodeBuilder>(*entry)) continue;
        auto& node = std::get<FlowNodeBuilder>(*entry);
        auto* nt = find_node_type2(node.type_id);
        if (!nt || !nt->va_args || !node.parsed_args) continue;

        // Count non-bang fixed inputs (args don't include bang triggers)
        int fixed_args = 0;
        for (int i = 0; i < nt->num_inputs; i++) {
            if (nt->input_ports && nt->input_ports[i].kind != PortKind2::BangTrigger)
                fixed_args++;
        }

        if ((int)node.parsed_args->size() > fixed_args) {
            node.parsed_va_args = std::make_shared<ParsedArgs2>();
            for (int i = fixed_args; i < (int)node.parsed_args->size(); i++)
                node.parsed_va_args->push_back(std::move((*node.parsed_args)[i]));
            node.parsed_args->resize(fixed_args);
        }
    }

    gb->compact();

    return gb;
}
