#include "graphbuilder.h"
#include "args.h"
#include "expr.h"
#include "node_types.h"
#include <sstream>

// ─── TOML helpers (shared with serial.cpp) ───

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

// ─── FlowNodeBuilder ───

std::string FlowNodeBuilder::args_str() const {
    if (!parsed_args) return "";
    return reconstruct_args_str(*parsed_args);
}

// ─── GraphBuilder ───

std::shared_ptr<FlowNodeBuilder> GraphBuilder::add(NodeId id, NodeTypeID type, std::shared_ptr<ParsedArgs2> args) {
    auto nb = std::make_shared<FlowNodeBuilder>();
    nb->id = std::move(id);
    nb->type_id = type;
    nb->parsed_args = std::move(args);
    builders.push_back(nb);
    return nb;
}

void GraphBuilder::link(const std::string& from, const std::string& to) {
    // TODO: links between FlowNodeBuilders
}

std::shared_ptr<FlowNodeBuilder> GraphBuilder::find(const NodeId& id) {
    for (auto& nb : builders) if (nb->id == id) return nb;
    return nullptr;
}

// ─── Deserializer ───

BuilderResult Deserializer::parse_node(
    const NodeId& id, const std::string& type, const std::vector<std::string>& args) {

    NodeTypeID type_id = node_type_id_from_string(type.c_str());

    if (type_id == NodeTypeID::Unknown) {
        return BuilderError("Unknown node type: " + type);
    }

    // Labels and errors: exactly 1 ArgString
    if (is_any_of(type_id, NodeTypeID::Label, NodeTypeID::Error)) {
        if (args.size() != 1)
            throw std::invalid_argument("Label/Error node requires exactly 1 argument, got " + std::to_string(args.size()));
        auto nb = std::make_shared<FlowNodeBuilder>();
        nb->id = id;
        nb->type_id = type_id;
        nb->parsed_args = std::make_shared<ParsedArgs2>();
        nb->parsed_args->args.push_back(ArgString{args[0]});
        nb->parsed_args->has_any_args = true;
        return nb;
    }

    bool is_expr = is_any_of(type_id, NodeTypeID::Expr, NodeTypeID::ExprBang);

    // Parse expressions (args are already split)
    auto parse_result = parse_args_v2(args, is_expr);
    if (auto* err = std::get_if<std::string>(&parse_result)) {
        return BuilderError(*err);
    }

    auto parsed = std::get<std::shared_ptr<ParsedArgs2>>(std::move(parse_result));

    auto nb = std::make_shared<FlowNodeBuilder>();
    nb->id = id;
    nb->type_id = type_id;
    nb->parsed_args = std::move(parsed);
    return nb;
}

std::shared_ptr<FlowNodeBuilder> Deserializer::parse_or_error(
    const std::shared_ptr<GraphBuilder>& gb,
    const NodeId& id, const std::string& type, const std::vector<std::string>& args) {

    auto result = parse_node(id, type, args);

    if (auto* nb = std::get_if<std::shared_ptr<FlowNodeBuilder>>(&result)) {
        gb->builders.push_back(*nb);
        return *nb;
    }

    auto& error_msg = std::get<BuilderError>(result);
    // Reconstruct original args for error display
    std::string args_joined;
    for (auto& a : args) {
        if (!args_joined.empty()) args_joined += " ";
        args_joined += a;
    }
    auto nb = std::make_shared<FlowNodeBuilder>();
    nb->id = id;
    nb->type_id = NodeTypeID::Error;
    nb->parsed_args = std::make_shared<ParsedArgs2>();
    nb->parsed_args->args.push_back(ArgString{type + " " + args_joined});
    nb->parsed_args->has_any_args = true;
    nb->error = error_msg;
    gb->builders.push_back(nb);
    return nb;
}

// ─── parse_atto: instrument@atto:0 stream parser ───

Deserializer::ParseAttoResult Deserializer::parse_atto(std::istream& f) {
    // Check version header
    std::string first_line;
    while (std::getline(f, first_line)) {
        first_line = trim(first_line);
        if (!first_line.empty()) break;
    }
    if (first_line != "# version instrument@atto:0") {
        return BuilderError("Expected '# version instrument@atto:0', got: " + first_line);
    }

    auto gb = std::make_shared<GraphBuilder>();

    // Parse state
    bool in_node = false;
    std::string cur_id, cur_type;
    std::vector<std::string> cur_args;
    std::vector<std::string> cur_inputs, cur_outputs;
    float cur_x = 0, cur_y = 0;
    bool cur_shadow = false;

    auto flush_node = [&]() {
        if (cur_type.empty()) {
            cur_id.clear(); cur_args.clear(); cur_inputs.clear(); cur_outputs.clear();
            return;
        }

        if (cur_id.empty()) {
            cur_id = "$auto-" + generate_guid();
        }

        auto nb = parse_or_error(gb, cur_id, cur_type, cur_args);
        nb->position = {cur_x, cur_y};
        nb->shadow = cur_shadow;

        // Store inputs/outputs net refs on the builder for later resolution
        // TODO: resolve net connections after all nodes are parsed

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

    return gb;
}
