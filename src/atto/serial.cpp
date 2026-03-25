#include "serial.h"
#include "shadow.h"
#include "args.h"
#include "expr.h"
#include "node_types.h"
#include "type_utils.h"
#include <fstream>
#include <sstream>
#include <map>
#include <set>
#include <cstdio>
#include <filesystem>

// ─── Shared utilities ───

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
            case 'b':  result += '\b'; i++; break;
            case 'f':  result += '\f'; i++; break;
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

static std::string escape_toml(const std::string& s) {
    std::string result;
    for (char c : s) {
        if (c == '"') result += "\\\"";
        else if (c == '\\') result += "\\\\";
        else if (c == '\n') result += "\\n";
        else if (c == '\t') result += "\\t";
        else if (c == '\r') result += "\\r";
        else result += c;
    }
    return result;
}

static std::vector<std::string> parse_array(const std::string& val) {
    std::vector<std::string> result;
    std::string s = trim(val);
    if (s.empty() || s.front() != '[' || s.back() != ']') return result;
    s = s.substr(1, s.size() - 2);
    std::string item;
    bool in_str = false;
    bool escaped = false;
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

// ─── Build a FlowNode from parsed fields (shared between v1 and v2 loaders) ───

static FlowNode build_node_from_type(FlowGraph& graph, const std::string& type_str,
                                      const std::string& args_str, bool is_shadow) {
    auto* nt = find_node_type(type_str.c_str());
    auto type_id = node_type_id_from_string(type_str.c_str());
    bool is_expr = is_any_of(type_id, NodeTypeID::Expr, NodeTypeID::ExprBang);
    bool args_are_type = is_any_of(type_id, NodeTypeID::Cast, NodeTypeID::New);

    int default_triggers = nt ? nt->num_triggers : 0;
    int default_inputs = nt ? nt->inputs : 0;
    int default_nexts = nt ? nt->num_nexts : 0;
    int default_outputs = nt ? nt->outputs : 1;
    int num_outputs = default_outputs;

    FlowNode node;
    node.id = graph.next_node_id();
    node.type_id = type_id;
    node.args = args_str;
    node.shadow = is_shadow;

    for (int i = 0; i < default_triggers; i++)
        node.triggers.push_back(make_pin("", "bang_in" + std::to_string(i), "", nullptr, FlowPin::BangTrigger));

    if (is_expr) {
        auto parsed = scan_slots(args_str);
        int total_top = parsed.total_pin_count(default_inputs);
        if (!args_str.empty()) {
            auto tokens = tokenize_args(args_str, false);
            num_outputs = std::max(1, (int)tokens.size());
        }
        for (int i = 0; i < total_top; i++) {
            bool is_lambda = parsed.is_lambda_slot(i);
            std::string pin_name = is_lambda ? ("@" + std::to_string(i)) : std::to_string(i);
            node.inputs.push_back(make_pin("", pin_name, "", nullptr, is_lambda ? FlowPin::Lambda : FlowPin::Input));
        }
    } else if (args_are_type) {
        for (int i = 0; i < default_inputs; i++) {
            std::string pin_name;
            std::string pin_type;
            bool is_lambda = false;
            if (nt && nt->input_ports && i < nt->inputs) {
                pin_name = nt->input_ports[i].name;
                is_lambda = (nt->input_ports[i].kind == PortKind::Lambda);
                if (nt->input_ports[i].type_name) pin_type = nt->input_ports[i].type_name;
            } else {
                pin_name = std::to_string(i);
            }
            node.inputs.push_back(make_pin("", pin_name, pin_type, nullptr, is_lambda ? FlowPin::Lambda : FlowPin::Input));
        }
    } else {
        auto info = compute_inline_args(args_str, default_inputs);
        if (!info.error.empty()) node.error = info.error;
        int ref_pins = (info.pin_slots.max_slot >= 0) ? (info.pin_slots.max_slot + 1) : 0;
        for (int i = 0; i < ref_pins; i++) {
            bool is_lambda = info.pin_slots.is_lambda_slot(i);
            std::string pin_name = is_lambda ? ("@" + std::to_string(i)) : std::to_string(i);
            node.inputs.push_back(make_pin("", pin_name, "", nullptr, is_lambda ? FlowPin::Lambda : FlowPin::Input));
        }
        for (int i = info.num_inline_args; i < default_inputs; i++) {
            std::string pin_name;
            std::string pin_type;
            bool is_lambda = false;
            if (nt && nt->input_ports && i < nt->inputs) {
                pin_name = nt->input_ports[i].name;
                is_lambda = (nt->input_ports[i].kind == PortKind::Lambda);
                if (nt->input_ports[i].type_name) pin_type = nt->input_ports[i].type_name;
            } else {
                pin_name = std::to_string(i);
            }
            node.inputs.push_back(make_pin("", pin_name, pin_type, nullptr, is_lambda ? FlowPin::Lambda : FlowPin::Input));
        }
    }

    for (int i = 0; i < default_nexts; i++)
        node.nexts.push_back(make_pin("", "bang" + std::to_string(i), "", nullptr, FlowPin::BangNext));
    for (int i = 0; i < num_outputs; i++)
        node.outputs.push_back(make_pin("", "out" + std::to_string(i), "", nullptr, FlowPin::Output));

    node.parse_args();
    return node;
}

// ─── Resolve imports ───

static void resolve_imports(FlowGraph& graph, const std::string& base_path) {
    namespace fs = std::filesystem;
    fs::path file_dir = base_path.empty() ? fs::current_path() : fs::path(base_path).parent_path();

    std::vector<fs::path> search_paths;
    search_paths.push_back(file_dir / ".." / "attostd");
    search_paths.push_back(file_dir / "attostd");
    search_paths.push_back(fs::path(__FILE__).parent_path() / ".." / ".." / "attostd");

    std::vector<std::string> import_paths;
    for (auto& node : graph.nodes) {
        if (node.type_id != NodeTypeID::DeclImport) continue;
        auto tokens = tokenize_args(node.args, false);
        if (tokens.empty()) continue;
        std::string import_path = tokens[0];
        if (import_path.size() >= 2 && import_path.front() == '"' && import_path.back() == '"')
            import_path = import_path.substr(1, import_path.size() - 2);
        if (import_path.substr(0, 4) != "std/") continue;
        import_paths.push_back(import_path);
    }

    std::set<std::string> imported;
    for (auto& import_path : import_paths) {
        if (imported.count(import_path)) continue;
        imported.insert(import_path);

        std::string module_name = import_path.substr(4);
        std::string atto_file = module_name + ".atto";

        bool found = false;
        for (auto& sp : search_paths) {
            fs::path full = sp / atto_file;
            if (fs::exists(full)) {
                FlowGraph temp;
                load_atto(full.string(), temp);
                for (auto& n : temp.nodes) {
                    if (is_any_of(n.type_id, NodeTypeID::Ffi, NodeTypeID::DeclType)) {
                        n.id = graph.next_node_id();
                        n.imported = true;
                        graph.nodes.push_back(std::move(n));
                    }
                }
                found = true;
                break;
            }
        }
        if (!found) {
            for (auto& n : graph.nodes) {
                if (n.type_id == NodeTypeID::DeclImport) {
                    auto t = tokenize_args(n.args, false);
                    if (!t.empty() && t[0] == import_path)
                        n.error = "decl_import: module not found: " + import_path;
                }
            }
        }
    }
}

// ─── Auto-migrate v1 to v2: assign node_ids and net_names ───

static void migrate_v1_to_v2(FlowGraph& graph) {
    // Assign $auto-<guid> node IDs to nodes that don't have one
    for (auto& node : graph.nodes) {
        if (node.node_id.empty()) {
            node.node_id = "$auto-" + node.guid;
        }
    }

    // Assign net names to links that don't have one
    // Build unique net names from from_pin
    int net_counter = 0;
    for (auto& link : graph.links) {
        if (link.net_name.empty()) {
            // Find the source node for this link
            std::string source_node_id;
            for (auto& node : graph.nodes) {
                for (auto& p : node.outputs) if (p->id == link.from_pin) source_node_id = node.node_id;
                for (auto& p : node.nexts) if (p->id == link.from_pin) source_node_id = node.node_id;
                if (node.lambda_grab.id == link.from_pin) source_node_id = node.node_id;
                if (node.bang_pin.id == link.from_pin) source_node_id = node.node_id;
                if (!source_node_id.empty()) break;
            }
            // Extract pin name from from_pin (guid.pin_name -> pin_name)
            auto dot = link.from_pin.rfind('.');
            std::string pin_name = (dot != std::string::npos) ? link.from_pin.substr(dot + 1) : "out";

            // Check if another link already uses a net from this same source pin
            std::string existing_net;
            for (auto& other : graph.links) {
                if (&other == &link) continue;
                if (other.from_pin == link.from_pin && !other.net_name.empty()) {
                    existing_net = other.net_name;
                    break;
                }
            }
            if (!existing_net.empty()) {
                link.net_name = existing_net;
                link.auto_wire = true;
            } else {
                link.net_name = source_node_id + "-" + pin_name;
                link.auto_wire = true;
            }
        }
    }
}

// ─── V1 Loader (legacy: nanoprog@0/1, attoprog@0/1) ───

static bool load_v1_stream(std::istream& f, FlowGraph& graph, const std::string& base_path, int format_version) {
    struct PendingConnection { std::string target; };
    std::vector<PendingConnection> pending;

    bool in_node = false, in_viewport = false;
    std::string cur_guid, cur_type;
    std::vector<std::string> cur_args;
    float cur_x = 0, cur_y = 0;
    bool cur_shadow = false;
    bool load_error = false;
    std::string load_error_msg;

    auto flush_node = [&]() {
        if (cur_type.empty()) { cur_guid.clear(); cur_args.clear(); return; }

        if (cur_guid.empty()) {
            char buf[17];
            snprintf(buf, sizeof(buf), "%08x%08x", (unsigned)rand(), (unsigned)rand());
            cur_guid = buf;
        }

        std::string args_str;
        for (auto& a : cur_args) {
            if (!args_str.empty()) args_str += " ";
            args_str += a;
        }

        auto* nt = find_node_type(cur_type.c_str());
        if (!nt) {
            load_error = true;
            load_error_msg = "Unknown node type \"" + cur_type + "\" (guid: " + cur_guid + ")";
            return;
        }

        FlowNode node = build_node_from_type(graph, cur_type, args_str, cur_shadow);
        node.guid = cur_guid;
        node.position = {cur_x, cur_y};
        node.rebuild_pin_ids();
        graph.nodes.push_back(std::move(node));

        cur_guid.clear(); cur_type.clear(); cur_args.clear();
        cur_x = 0; cur_y = 0; cur_shadow = false;
    };

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        if (line == "[[node]]") {
            flush_node();
            if (load_error) break;
            in_node = true;
            in_viewport = false;
            continue;
        }

        if (line == "[viewport]") {
            flush_node();
            in_node = false;
            in_viewport = true;
            continue;
        }

        // Skip version line (already parsed)
        if (line.find("version") == 0) continue;

        if (in_viewport) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = trim(line.substr(0, eq));
            std::string val = trim(line.substr(eq + 1));
            if (key == "x") graph.viewport_x = std::stof(val);
            else if (key == "y") graph.viewport_y = std::stof(val);
            else if (key == "zoom") graph.viewport_zoom = std::stof(val);
            graph.has_viewport = true;
            continue;
        }

        if (!in_node) continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (key == "guid" || key == "name") { cur_guid = unquote(val); }
        else if (key == "type") { cur_type = unquote(val); }
        else if (key == "args") { cur_args = parse_array(val); }
        else if (key == "shadow") { cur_shadow = (unquote(val) == "true"); }
        else if (key == "connections") {
            auto conns = parse_array(val);
            for (auto& c : conns) pending.push_back({c});
        }
        else if (key == "position") {
            auto coords = parse_array(val);
            if (coords.size() >= 2) {
                cur_x = std::stof(coords[0]);
                cur_y = std::stof(coords[1]);
            }
        }
    }
    flush_node();

    if (load_error) {
        fprintf(stderr, "Error loading: %s\n", load_error_msg.c_str());
        graph.nodes.clear();
        graph.links.clear();
        return false;
    }

    size_t own_node_count = graph.nodes.size();

    resolve_imports(graph, base_path);
    resolve_type_based_pins(graph);

    for (auto& pc : pending) {
        auto arrow = pc.target.find("->");
        if (arrow == std::string::npos) continue;
        std::string from_id = pc.target.substr(0, arrow);
        std::string to_id = pc.target.substr(arrow + 2);
        graph.add_link(from_id, to_id);
    }

    if (format_version == 0)
        generate_shadow_nodes(graph);

    // Auto-migrate to v2 representation
    migrate_v1_to_v2(graph);

    rebuild_all_inline_display(graph);
    printf("Loaded %zu nodes, %zu links (v1 format)\n", own_node_count, graph.links.size());
    return true;
}

// ─── V2 Loader (instrument@atto:0) ───

static bool load_v2_stream(std::istream& f, FlowGraph& graph, const std::string& base_path) {
    bool in_node = false;
    std::string cur_id, cur_type;
    std::vector<std::string> cur_args;
    std::vector<std::string> cur_inputs, cur_outputs;
    float cur_x = 0, cur_y = 0;
    bool cur_shadow = false;
    bool load_error = false;
    std::string load_error_msg;

    // Store node IDs to index mapping for net resolution
    struct NodeEntry {
        size_t graph_index;
        std::string node_id;
    };
    std::vector<NodeEntry> node_entries;

    // Store input/output net references per node for post-load resolution
    struct NetRef {
        size_t node_index;
        std::vector<std::string> inputs;
        std::vector<std::string> outputs;
    };
    std::vector<NetRef> net_refs;

    auto flush_node = [&]() {
        if (cur_type.empty()) { cur_id.clear(); cur_args.clear(); cur_inputs.clear(); cur_outputs.clear(); return; }

        if (cur_id.empty()) {
            cur_id = "$auto-" + generate_guid();
        }

        auto* nt = find_node_type(cur_type.c_str());
        if (!nt) {
            load_error = true;
            load_error_msg = "Unknown node type \"" + cur_type + "\" (id: " + cur_id + ")";
            return;
        }

        std::string args_str;
        for (auto& a : cur_args) {
            if (!args_str.empty()) args_str += " ";
            args_str += a;
        }

        FlowNode node = build_node_from_type(graph, cur_type, args_str, cur_shadow);
        node.node_id = cur_id;
        // Generate a guid from the node_id for internal use
        // Strip $ prefix and hash, or just use the id directly
        node.guid = cur_id.substr(0, 1) == "$" ? cur_id.substr(1) : cur_id;
        node.position = {cur_x, cur_y};
        node.rebuild_pin_ids();

        size_t idx = graph.nodes.size();
        node_entries.push_back({idx, cur_id});
        net_refs.push_back({idx, cur_inputs, cur_outputs});
        graph.nodes.push_back(std::move(node));

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
            if (load_error) break;
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
        else if (key == "args") { cur_args = parse_array(val); }
        else if (key == "shadow") { cur_shadow = (unquote(val) == "true"); }
        else if (key == "inputs") { cur_inputs = parse_array(val); }
        else if (key == "outputs") { cur_outputs = parse_array(val); }
        else if (key == "position") {
            auto coords = parse_array(val);
            if (coords.size() >= 2) {
                cur_x = std::stof(coords[0]);
                cur_y = std::stof(coords[1]);
            }
        }
    }
    flush_node();

    if (load_error) {
        fprintf(stderr, "Error loading: %s\n", load_error_msg.c_str());
        graph.nodes.clear();
        graph.links.clear();
        return false;
    }

    size_t own_node_count = graph.nodes.size();

    resolve_imports(graph, base_path);
    resolve_type_based_pins(graph);

    // Build node_id → graph index map
    std::map<std::string, size_t> id_to_index;
    for (auto& ne : node_entries) {
        id_to_index[ne.node_id] = ne.graph_index;
    }

    // Build net → source pin map from outputs arrays
    struct NetSource {
        size_t node_index;
        int pin_index; // index in the ordered outputs array
        enum PinKind { Next, DataOut, PostBang, LambdaGrab } kind;
    };
    std::map<std::string, NetSource> net_sources;

    for (auto& nr : net_refs) {
        auto& node = graph.nodes[nr.node_index];
        int nexts_count = (int)node.nexts.size();
        int outputs_count = (int)node.outputs.size();

        for (int i = 0; i < (int)nr.outputs.size(); i++) {
            auto& net_name = nr.outputs[i];
            if (net_name.empty()) continue;

            NetSource src;
            src.node_index = nr.node_index;
            if (i < nexts_count) {
                src.pin_index = i;
                src.kind = NetSource::Next;
            } else if (i < nexts_count + outputs_count) {
                src.pin_index = i - nexts_count;
                src.kind = NetSource::DataOut;
            } else {
                // post_bang
                src.pin_index = 0;
                src.kind = NetSource::PostBang;
            }
            net_sources[net_name] = src;
        }

        // Also register lambda_grab as a net source if the node_id is used as a net
        // (for lambda captures, the node itself is the net source)
    }

    // Resolve nets: for each node's inputs, find the source and create links
    for (auto& nr : net_refs) {
        auto& node = graph.nodes[nr.node_index];
        int triggers_count = (int)node.triggers.size();
        int inputs_count = (int)node.inputs.size();

        for (int i = 0; i < (int)nr.inputs.size(); i++) {
            auto& net_name = nr.inputs[i];
            if (net_name.empty()) continue;

            // Check if this is a lambda capture (referencing a node_id directly)
            bool is_lambda_ref = false;
            if (id_to_index.count(net_name) && !net_sources.count(net_name)) {
                is_lambda_ref = true;
            }

            std::string from_pin_id;
            if (is_lambda_ref) {
                // Lambda capture: wire from source node's as_lambda pin
                auto& src_node = graph.nodes[id_to_index[net_name]];
                from_pin_id = src_node.lambda_grab.id;
            } else if (net_sources.count(net_name)) {
                auto& src = net_sources[net_name];
                auto& src_node = graph.nodes[src.node_index];
                switch (src.kind) {
                case NetSource::Next:     from_pin_id = src_node.nexts[src.pin_index]->id; break;
                case NetSource::DataOut:  from_pin_id = src_node.outputs[src.pin_index]->id; break;
                case NetSource::PostBang: from_pin_id = src_node.bang_pin.id; break;
                case NetSource::LambdaGrab: from_pin_id = src_node.lambda_grab.id; break;
                }
            } else {
                fprintf(stderr, "Warning: net '%s' has no source\n", net_name.c_str());
                continue;
            }

            // Determine target pin
            std::string to_pin_id;
            if (i < triggers_count) {
                to_pin_id = node.triggers[i]->id;
            } else if (i < triggers_count + inputs_count) {
                int input_idx = i - triggers_count;
                if (input_idx < (int)node.inputs.size())
                    to_pin_id = node.inputs[input_idx]->id;
            }

            if (!to_pin_id.empty() && !from_pin_id.empty()) {
                int link_id = graph.add_link(from_pin_id, to_pin_id);
                // Set net name on the link
                for (auto& link : graph.links) {
                    if (link.id == link_id) {
                        link.net_name = net_name;
                        link.auto_wire = (net_name.substr(0, 6) == "$auto-");
                        break;
                    }
                }
            }
        }
    }

    rebuild_all_inline_display(graph);
    printf("Loaded %zu nodes, %zu links (v2 format)\n", own_node_count, graph.links.size());
    return true;
}

// ─── Unified stream loader ───

bool load_atto_stream(std::istream& f, FlowGraph& graph, const std::string& base_path) {
    graph.nodes.clear();
    graph.links.clear();

    // Peek at the first non-empty line to determine format version
    std::streampos start = f.tellg();
    std::string first_line;
    while (std::getline(f, first_line)) {
        first_line = trim(first_line);
        if (!first_line.empty()) break;
    }

    // Seek back to start
    f.clear();
    f.seekg(start);

    if (first_line.find("# version instrument@atto:0") == 0) {
        return load_v2_stream(f, graph, base_path);
    }

    // Legacy format — determine version
    int format_version = 0;
    if (first_line.find("attoprog@1") != std::string::npos ||
        first_line.find("nanoprog@1") != std::string::npos) {
        format_version = 1;
    }

    return load_v1_stream(f, graph, base_path, format_version);
}

// ─── Public load functions ───

bool load_atto(const std::string& path, FlowGraph& graph) {
    std::ifstream f(path);
    if (!f.is_open()) {
        fprintf(stderr, "Cannot open %s\n", path.c_str());
        return false;
    }
    bool ok = load_atto_stream(f, graph, path);
    if (ok) load_atto_meta(path, graph);
    return ok;
}

bool load_atto_string(const std::string& data, FlowGraph& graph) {
    std::istringstream f(data);
    return load_atto_stream(f, graph, "");
}

// ─── V2 Writer (instrument@atto:0) ───

void save_atto_stream(std::ostream& f, const FlowGraph& graph) {
    f << "# version instrument@atto:0\n\n";

    for (auto& node : graph.nodes) {
        if (node.imported) continue;
        f << "[[node]]\n";

        // Use node_id if available, fall back to $auto-<guid>
        std::string id = node.node_id.empty() ? ("$auto-" + node.guid) : node.node_id;
        f << "id = \"" << escape_toml(id) << "\"\n";
        f << "type = \"" << node_type_str(node.type_id) << "\"\n";
        if (node.shadow) f << "shadow = true\n";

        auto tokens = tokenize_args(node.args, false);
        if (!tokens.empty()) {
            f << "args = [";
            for (size_t i = 0; i < tokens.size(); i++) {
                if (i > 0) f << ", ";
                f << "\"" << escape_toml(tokens[i]) << "\"";
            }
            f << "]\n";
        }

        // Build inputs array: [trigger_nets..., data_input_nets...]
        // Order: triggers first, then data inputs
        {
            std::vector<std::string> input_nets;

            // Triggers
            for (auto& p : node.triggers) {
                std::string net;
                for (auto& link : graph.links) {
                    if (link.to_pin == p->id) {
                        net = link.net_name;
                        if (net.empty()) {
                            // Generate from source pin
                            net = "$auto-" + link.from_pin;
                        }
                        break;
                    }
                }
                input_nets.push_back(net);
            }

            // Data inputs (including lambdas)
            for (auto& p : node.inputs) {
                std::string net;
                for (auto& link : graph.links) {
                    if (link.to_pin == p->id) {
                        net = link.net_name;
                        if (net.empty()) {
                            net = "$auto-" + link.from_pin;
                        }
                        break;
                    }
                }
                input_nets.push_back(net);
            }

            // Trim trailing empty entries
            while (!input_nets.empty() && input_nets.back().empty())
                input_nets.pop_back();

            if (!input_nets.empty()) {
                f << "inputs = [";
                for (size_t i = 0; i < input_nets.size(); i++) {
                    if (i > 0) f << ", ";
                    f << "\"" << escape_toml(input_nets[i]) << "\"";
                }
                f << "]\n";
            }
        }

        // Build outputs array: [next_nets..., data_output_nets..., post_bang_net]
        {
            std::vector<std::string> output_nets;

            // Nexts (bang outputs)
            for (auto& p : node.nexts) {
                std::string net;
                for (auto& link : graph.links) {
                    if (link.from_pin == p->id) {
                        net = link.net_name;
                        if (net.empty()) net = "$auto-" + link.from_pin;
                        break; // Take first — nets have one source
                    }
                }
                output_nets.push_back(net);
            }

            // Data outputs
            for (auto& p : node.outputs) {
                std::string net;
                for (auto& link : graph.links) {
                    if (link.from_pin == p->id) {
                        net = link.net_name;
                        if (net.empty()) net = "$auto-" + link.from_pin;
                        break;
                    }
                }
                output_nets.push_back(net);
            }

            // Post-bang output
            {
                std::string net;
                for (auto& link : graph.links) {
                    if (link.from_pin == node.bang_pin.id) {
                        net = link.net_name;
                        if (net.empty()) net = "$auto-" + link.from_pin;
                        break;
                    }
                }
                output_nets.push_back(net);
            }

            // Also check lambda_grab
            {
                std::string net;
                for (auto& link : graph.links) {
                    if (link.from_pin == node.lambda_grab.id) {
                        net = link.net_name;
                        if (net.empty()) net = "$auto-" + link.from_pin;
                        break;
                    }
                }
                if (!net.empty()) output_nets.push_back(net);
            }

            // Trim trailing empty entries
            while (!output_nets.empty() && output_nets.back().empty())
                output_nets.pop_back();

            if (!output_nets.empty()) {
                f << "outputs = [";
                for (size_t i = 0; i < output_nets.size(); i++) {
                    if (i > 0) f << ", ";
                    f << "\"" << escape_toml(output_nets[i]) << "\"";
                }
                f << "]\n";
            }
        }

        f << "position = [" << node.position.x << ", " << node.position.y << "]\n";
        f << "\n";
    }
}

std::string save_atto_string(const FlowGraph& graph) {
    std::ostringstream ss;
    save_atto_stream(ss, graph);
    return ss.str();
}

bool save_atto(const std::string& path, const FlowGraph& graph) {
    std::ofstream f(path);
    if (!f.is_open()) {
        fprintf(stderr, "Cannot write %s\n", path.c_str());
        return false;
    }
    save_atto_stream(f, graph);
    save_atto_meta(path, graph);
    printf("Saved %zu nodes, %zu links to %s\n", graph.nodes.size(), graph.links.size(), path.c_str());
    return true;
}

// ─── Meta file (.atto/<filename>.yaml) ───

static std::string meta_path_for(const std::string& atto_path) {
    namespace fs = std::filesystem;
    fs::path p(atto_path);
    fs::path dir = p.parent_path() / ".atto";
    fs::path meta = dir / (p.filename().string() + ".yaml");
    return meta.string();
}

bool load_atto_meta(const std::string& atto_path, FlowGraph& graph) {
    std::string path = meta_path_for(atto_path);
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trim(line.substr(0, colon));
        std::string val = trim(line.substr(colon + 1));
        if (key == "viewport_x") graph.viewport_x = std::stof(val);
        else if (key == "viewport_y") graph.viewport_y = std::stof(val);
        else if (key == "viewport_zoom") graph.viewport_zoom = std::stof(val);
    }
    graph.has_viewport = true;
    return true;
}

bool save_atto_meta(const std::string& atto_path, const FlowGraph& graph) {
    namespace fs = std::filesystem;
    std::string path = meta_path_for(atto_path);

    // Create .atto directory if needed
    fs::path dir = fs::path(path).parent_path();
    if (!fs::exists(dir)) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            fprintf(stderr, "Cannot create meta dir %s: %s\n", dir.string().c_str(), ec.message().c_str());
            return false;
        }
    }

    std::ofstream f(path);
    if (!f.is_open()) {
        fprintf(stderr, "Cannot write meta %s\n", path.c_str());
        return false;
    }

    f << "# Editor metadata for " << fs::path(atto_path).filename().string() << "\n";
    f << "viewport_x: " << graph.viewport_x << "\n";
    f << "viewport_y: " << graph.viewport_y << "\n";
    f << "viewport_zoom: " << graph.viewport_zoom << "\n";
    return true;
}
