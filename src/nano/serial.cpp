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

bool load_nano(const std::string& path, FlowGraph& graph) {
    std::ifstream f(path);
    if (!f.is_open()) {
        fprintf(stderr, "Cannot open %s\n", path.c_str());
        return false;
    }

    graph.nodes.clear();
    graph.links.clear();

    std::string line;
    bool in_node = false;
    std::string cur_guid, cur_type;
    std::vector<std::string> cur_args;
    float cur_x = 0, cur_y = 0;
    bool cur_shadow = false;
    int format_version = 0;

    auto trim = [](std::string s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
        return s;
    };

    // TOML escape sequence processing
    auto unescape_toml = [](const std::string& s) -> std::string {
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
                default:   result += s[i]; break; // unknown escape, keep backslash
                }
            } else {
                result += s[i];
            }
        }
        return result;
    };

    auto unquote = [&](std::string s) {
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
            return unescape_toml(s.substr(1, s.size() - 2));
        return s;
    };

    auto parse_array = [&](const std::string& val) -> std::vector<std::string> {
        std::vector<std::string> result;
        std::string s = trim(val);
        if (s.front() != '[' || s.back() != ']') return result;
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
    };

    struct PendingConnection { std::string target; }; // "from_pin_id->to_pin_id"
    std::vector<PendingConnection> pending;

    bool load_error = false;
    std::string load_error_msg;

    auto flush_node = [&]() {
        if (cur_type.empty()) { cur_guid.clear(); cur_args.clear(); return; }

        if (cur_guid.empty()) {
            // Auto-generate guid for nodes without one (e.g. imported nanostd)
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
        int default_triggers = nt ? nt->num_triggers : 0;
        int default_inputs = nt ? nt->inputs : 0;
        int default_nexts = nt ? nt->num_nexts : 0;
        int default_outputs = nt ? nt->outputs : 1;

        auto cur_type_id = node_type_id_from_string(cur_type.c_str());
        bool is_expr = is_any_of(cur_type_id, NodeTypeID::Expr, NodeTypeID::ExprBang);
        int num_outputs = default_outputs;

        FlowNode node;
        node.id = graph.next_node_id();
        node.guid = cur_guid;
        node.type_id = cur_type_id;
        node.args = args_str;
        node.position = {cur_x, cur_y};

        for (int i = 0; i < default_triggers; i++)
            node.triggers.push_back(make_pin("", "bang_in" + std::to_string(i), "", nullptr, FlowPin::BangTrigger));

        // Nodes whose args are type names, not expressions
        bool args_are_type = is_any_of(cur_type_id, NodeTypeID::Cast, NodeTypeID::New);

        if (is_expr) {
            // Expr nodes: pin count from $N refs, output count from tokens
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
            // Args are type names — use descriptor defaults directly
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
            // Non-expr nodes: use inline arg computation
            auto info = compute_inline_args(args_str, default_inputs);
            if (!info.error.empty()) node.error = info.error;

            // First: $N/@N ref pins from inline expressions
            int ref_pins = (info.pin_slots.max_slot >= 0) ? (info.pin_slots.max_slot + 1) : 0;
            for (int i = 0; i < ref_pins; i++) {
                bool is_lambda = info.pin_slots.is_lambda_slot(i);
                std::string pin_name = is_lambda ? ("@" + std::to_string(i)) : std::to_string(i);
                node.inputs.push_back(make_pin("", pin_name, "", nullptr, is_lambda ? FlowPin::Lambda : FlowPin::Input));
            }
            // Then: remaining descriptor inputs not covered by inline args
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

        node.rebuild_pin_ids();

        node.shadow = cur_shadow;
        node.parse_args();
        graph.nodes.push_back(std::move(node));

        cur_guid.clear(); cur_type.clear(); cur_args.clear();
        cur_x = 0; cur_y = 0; cur_shadow = false;
    };

    bool in_viewport = false;

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

        if (line.find("version:") == 0 || line.find("version =") == 0) {
            auto eq = line.find('=');
            if (eq != std::string::npos) {
                std::string val = trim(line.substr(eq + 1));
                val = unquote(val);
                if (val == "nanoprog@1") format_version = 1;
            }
            continue;
        }

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
        fprintf(stderr, "Error loading %s: %s\n", path.c_str(), load_error_msg.c_str());
        graph.nodes.clear();
        graph.links.clear();
        return false;
    }

    size_t own_node_count = graph.nodes.size();

    // Resolve imports: load nanostd modules referenced by decl_import nodes
    {
        namespace fs = std::filesystem;
        // Find nanostd path relative to the loaded file
        fs::path file_dir = fs::path(path).parent_path();
        // Look for nanostd in known locations
        std::vector<fs::path> search_paths;
        // 1. Relative to the source file's project root
        search_paths.push_back(file_dir / ".." / "nanostd");
        search_paths.push_back(file_dir / "nanostd");
        // 2. Relative to the executable (for installed setups)
        search_paths.push_back(fs::path(__FILE__).parent_path() / ".." / ".." / "nanostd");

        // Collect all import paths first (avoid modifying graph while iterating)
        std::vector<std::string> import_paths;
        for (auto& node : graph.nodes) {
            if (node.type_id != NodeTypeID::DeclImport) continue;
            auto tokens = tokenize_args(node.args, false);
            if (tokens.empty()) continue;
            std::string import_path = tokens[0];
            // Strip quotes from string literal
            if (import_path.size() >= 2 && import_path.front() == '"' && import_path.back() == '"')
                import_path = import_path.substr(1, import_path.size() - 2);
            if (import_path.substr(0, 4) != "std/") continue;
            import_paths.push_back(import_path);
        }

        std::set<std::string> imported;
        for (auto& import_path : import_paths) {
            if (imported.count(import_path)) continue;
            imported.insert(import_path);

            std::string module_name = import_path.substr(4); // strip "std/"
            std::string nano_file = module_name + ".nano";

            // Search for the nanostd file
            bool found = false;
            for (auto& sp : search_paths) {
                fs::path full = sp / nano_file;
                if (fs::exists(full)) {
                    // Load the module's nodes into a temp graph
                    FlowGraph temp;
                    load_nano(full.string(), temp);
                    // Merge only ffi and decl_type nodes (declarations) into main graph
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
                // Find the decl_import node to set the error on
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

    // Resolve type-based pins (e.g. "new" nodes derive inputs from type definitions)
    resolve_type_based_pins(graph);

    // Resolve connections — pin IDs are already the serialized form
    for (auto& pc : pending) {
        auto arrow = pc.target.find("->");
        if (arrow == std::string::npos) continue;
        std::string from_id = pc.target.substr(0, arrow);
        std::string to_id = pc.target.substr(arrow + 2);
        graph.add_link(from_id, to_id);
    }

    if (format_version == 0)
        generate_shadow_nodes(graph);

    rebuild_all_inline_display(graph);

    printf("Loaded %zu nodes, %zu links from %s\n", own_node_count, graph.links.size(), path.c_str());
    return true;
}

void save_nano_stream(std::ostream& f, const FlowGraph& graph) {
    f << "version = \"nanoprog@1\"\n\n";

    f << "[viewport]\n";
    f << "x = " << graph.viewport_x << "\n";
    f << "y = " << graph.viewport_y << "\n";
    f << "zoom = " << graph.viewport_zoom << "\n\n";

    for (auto& node : graph.nodes) {
        if (node.imported) continue; // Don't save imported nodes — they're loaded from nanostd
        f << "[[node]]\n";
        f << "guid = \"" << node.guid << "\"\n";
        f << "type = \"" << node_type_str(node.type_id) << "\"\n";
        if (node.shadow) f << "shadow = true\n";

        auto tokens = tokenize_args(node.args, false);
        if (!tokens.empty()) {
            f << "args = [";
            for (size_t i = 0; i < tokens.size(); i++) {
                if (i > 0) f << ", ";
                f << "\"";
                for (char c : tokens[i]) {
                    if (c == '"') f << "\\\"";
                    else if (c == '\\') f << "\\\\";
                    else if (c == '\n') f << "\\n";
                    else if (c == '\t') f << "\\t";
                    else if (c == '\r') f << "\\r";
                    else f << c;
                }
                f << "\"";
            }
            f << "]\n";
        }

        f << "position = [" << node.position.x << ", " << node.position.y << "]\n";

        std::vector<std::string> conns;
        for (auto& link : graph.links) {
            bool from_this = false;
            for (auto& p : node.outputs) if (p->id == link.from_pin) from_this = true;
            for (auto& p : node.nexts) if (p->id == link.from_pin) from_this = true;
            for (auto& p : node.triggers) if (p->id == link.from_pin) from_this = true;
            if (node.lambda_grab.id == link.from_pin) from_this = true;
            if (node.bang_pin.id == link.from_pin) from_this = true;
            if (!from_this) continue;
            conns.push_back(link.from_pin + "->" + link.to_pin);
        }
        if (!conns.empty()) {
            f << "connections = [";
            for (size_t i = 0; i < conns.size(); i++) {
                if (i > 0) f << ", ";
                f << "\"" << conns[i] << "\"";
            }
            f << "]\n";
        }

        f << "\n";
    }

}

std::string save_nano_string(const FlowGraph& graph) {
    std::ostringstream ss;
    save_nano_stream(ss, graph);
    return ss.str();
}

bool save_nano(const std::string& path, const FlowGraph& graph) {
    std::ofstream f(path);
    if (!f.is_open()) {
        fprintf(stderr, "Cannot write %s\n", path.c_str());
        return false;
    }
    save_nano_stream(f, graph);
    printf("Saved %zu nodes, %zu links to %s\n", graph.nodes.size(), graph.links.size(), path.c_str());
    return true;
}

bool load_nano_string(const std::string& data, FlowGraph& graph) {
    std::istringstream f(data);
    // Reuse the load logic but from a string stream
    // For simplicity, write to a temp and load — or inline the parser.
    // Actually, load_nano reads from ifstream. Let's make a stream-based loader too.
    // For now, save to temp file and reload. TODO: refactor load_nano to use istream.
    // Quick approach: write data to a temporary string, use load_nano with a temp path.
    // Better: just duplicate the essential parsing inline.

    // Actually let's just clear and re-parse from the string data directly.
    graph.nodes.clear();
    graph.links.clear();

    std::string line;
    bool in_node = false;
    std::string cur_guid, cur_type;
    std::vector<std::string> cur_args;
    float cur_x = 0, cur_y = 0;
    bool cur_shadow = false;
    int format_version = 0;

    auto trim = [](std::string s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
        return s;
    };
    auto unescape_toml = [](const std::string& s) -> std::string {
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
    };
    auto unquote = [&](std::string s) {
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
            return unescape_toml(s.substr(1, s.size() - 2));
        return s;
    };
    auto parse_array = [&](const std::string& val) -> std::vector<std::string> {
        std::vector<std::string> result;
        std::string s = trim(val);
        if (s.front() != '[' || s.back() != ']') return result;
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
    };

    struct PC { std::string target; };
    std::vector<PC> pending;

    auto flush_node = [&]() {
        if (cur_guid.empty()) return;
        std::string args_str;
        for (auto& a : cur_args) { if (!args_str.empty()) args_str += " "; args_str += a; }

        auto* nt = find_node_type(cur_type.c_str());
        int dbi = nt ? nt->num_triggers : 0, di = nt ? nt->inputs : 0;
        int dbo = nt ? nt->num_nexts : 0, do_ = nt ? nt->outputs : 1;
        auto cur_type_id = node_type_id_from_string(cur_type.c_str());
        bool is_expr = is_any_of(cur_type_id, NodeTypeID::Expr, NodeTypeID::ExprBang);
        int no = do_;

        FlowNode node;
        node.id = graph.next_node_id();
        node.guid = cur_guid; node.type_id = cur_type_id; node.args = args_str;
        node.position = {cur_x, cur_y};
        for (int i = 0; i < dbi; i++) node.triggers.push_back(make_pin("","bang_in"+std::to_string(i), "", nullptr, FlowPin::BangTrigger));

        bool args_are_type = is_any_of(cur_type_id, NodeTypeID::Cast, NodeTypeID::New);

        if (is_expr) {
            auto parsed = scan_slots(args_str);
            int tt = parsed.total_pin_count(di);
            if (!args_str.empty()) { auto t = tokenize_args(args_str, false); no = std::max(1,(int)t.size()); }
            for (int i = 0; i < tt; i++) {
                bool il = parsed.is_lambda_slot(i);
                std::string pn = il ? ("@"+std::to_string(i)) : std::to_string(i);
                node.inputs.push_back(make_pin("", pn, "", nullptr, il ? FlowPin::Lambda : FlowPin::Input));
            }
        } else if (args_are_type) {
            for (int i = 0; i < di; i++) {
                std::string pn; std::string pt; bool il = false;
                if (nt && nt->input_ports && i < nt->inputs) {
                    pn = nt->input_ports[i].name;
                    il = (nt->input_ports[i].kind == PortKind::Lambda);
                    if (nt->input_ports[i].type_name) pt = nt->input_ports[i].type_name;
                } else pn = std::to_string(i);
                node.inputs.push_back(make_pin("", pn, pt, nullptr, il ? FlowPin::Lambda : FlowPin::Input));
            }
        } else {
            auto info = compute_inline_args(args_str, di);
            if (!info.error.empty()) node.error = info.error;
            int ref_pins = (info.pin_slots.max_slot >= 0) ? (info.pin_slots.max_slot + 1) : 0;
            for (int i = 0; i < ref_pins; i++) {
                bool il = info.pin_slots.is_lambda_slot(i);
                std::string pn = il ? ("@"+std::to_string(i)) : std::to_string(i);
                node.inputs.push_back(make_pin("", pn, "", nullptr, il ? FlowPin::Lambda : FlowPin::Input));
            }
            for (int i = info.num_inline_args; i < di; i++) {
                std::string pn; std::string pt; bool il = false;
                if (nt && nt->input_ports && i < nt->inputs) {
                    pn = nt->input_ports[i].name;
                    il = (nt->input_ports[i].kind == PortKind::Lambda);
                    if (nt->input_ports[i].type_name) pt = nt->input_ports[i].type_name;
                } else pn = std::to_string(i);
                node.inputs.push_back(make_pin("", pn, pt, nullptr, il ? FlowPin::Lambda : FlowPin::Input));
            }
        }

        for (int i = 0; i < dbo; i++) node.nexts.push_back(make_pin("","bang"+std::to_string(i), "", nullptr, FlowPin::BangNext));
        for (int i = 0; i < no; i++) node.outputs.push_back(make_pin("","out"+std::to_string(i), "", nullptr, FlowPin::Output));
        node.rebuild_pin_ids();

        node.shadow = cur_shadow;
        node.parse_args();
        graph.nodes.push_back(std::move(node));
        cur_guid.clear(); cur_type.clear(); cur_args.clear(); cur_x=0; cur_y=0; cur_shadow=false;
    };

    bool in_viewport = false;

    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line == "[[node]]") { flush_node(); in_node = true; in_viewport = false; continue; }
        if (line == "[viewport]") { flush_node(); in_node = false; in_viewport = true; continue; }
        if (line.find("version:") == 0 || line.find("version =") == 0) {
            auto eq = line.find('=');
            if (eq != std::string::npos) {
                std::string val = trim(line.substr(eq + 1));
                val = unquote(val);
                if (val == "nanoprog@1") format_version = 1;
            }
            continue;
        }
        if (in_viewport) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = trim(line.substr(0,eq)), val = trim(line.substr(eq+1));
            if (key == "x") graph.viewport_x = std::stof(val);
            else if (key == "y") graph.viewport_y = std::stof(val);
            else if (key == "zoom") graph.viewport_zoom = std::stof(val);
            graph.has_viewport = true;
            continue;
        }
        if (!in_node) continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0,eq)), val = trim(line.substr(eq+1));
        if (key == "guid" || key == "name") cur_guid = unquote(val);
        else if (key == "type") cur_type = unquote(val);
        else if (key == "args") cur_args = parse_array(val);
        else if (key == "shadow") cur_shadow = (unquote(val) == "true");
        else if (key == "connections") { for (auto& c : parse_array(val)) pending.push_back({c}); }
        else if (key == "position") { auto co = parse_array(val); if (co.size()>=2) { cur_x=std::stof(co[0]); cur_y=std::stof(co[1]); } }
    }
    flush_node();
    resolve_type_based_pins(graph);
    for (auto& pc : pending) {
        auto arrow = pc.target.find("->");
        if (arrow == std::string::npos) continue;
        graph.add_link(pc.target.substr(0,arrow), pc.target.substr(arrow+2));
    }
    if (format_version == 0)
        generate_shadow_nodes(graph);
    rebuild_all_inline_display(graph);
    return true;
}
