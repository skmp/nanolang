#include "graphbuilder.h"
#include "args.h"
#include "expr.h"
#include "node_types.h"

// ─── FlowNodeBuilder ───

std::string FlowNodeBuilder::args_str() const {
    if (!parsed_args) return "";
    return reconstruct_args_str(*parsed_args);
}

// ─── GraphBuilder ───

std::shared_ptr<FlowNodeBuilder> GraphBuilder::add(NodeId id, NodeTypeID type, std::shared_ptr<ParsedArgs> args) {
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

static std::shared_ptr<FlowNodeBuilder> make_error(const NodeId& id, const std::string& type,
                                                     const std::string& args_str, const std::string& error_msg) {
    auto nb = std::make_shared<FlowNodeBuilder>();
    nb->id = id;
    nb->type_id = NodeTypeID::Error;
    nb->parsed_args = std::make_shared<ParsedArgs>();
    nb->parsed_args->args.push_back(ArgString{type + " " + args_str});
    nb->parsed_args->has_any_args = true;
    nb->error = error_msg;
    return nb;
}

std::shared_ptr<FlowNodeBuilder> Deserializer::parse_node(
    const std::shared_ptr<GraphBuilder>& gb,
    const NodeId& id, const std::string& type, const std::string& args_str) {

    NodeTypeID type_id = node_type_id_from_string(type.c_str());

    if (type_id == NodeTypeID::Unknown) {
        auto nb = make_error(id, type, args_str, "Unknown node type: " + type);
        gb->builders.push_back(nb);
        return nb;
    }

    // Labels and errors: no expression parsing
    if (is_any_of(type_id, NodeTypeID::Label, NodeTypeID::Error)) {
        auto pa = std::make_shared<ParsedArgs>();
        if (!args_str.empty()) {
            pa->args.push_back(ArgString{args_str});
            pa->has_any_args = true;
        }
        return gb->add(id, type_id, std::move(pa));
    }

    // Split args into expressions
    auto split_result = split_args(args_str);
    if (auto* err = std::get_if<std::string>(&split_result)) {
        auto nb = make_error(id, type, args_str, *err);
        gb->builders.push_back(nb);
        return nb;
    }

    auto& exprs = std::get<std::vector<std::string>>(split_result);
    bool is_expr = is_any_of(type_id, NodeTypeID::Expr, NodeTypeID::ExprBang);

    // Parse expressions
    auto parse_result = parse_args_v2(exprs, is_expr);
    if (auto* err = std::get_if<std::string>(&parse_result)) {
        auto nb = make_error(id, type, args_str, *err);
        gb->builders.push_back(nb);
        return nb;
    }

    auto parsed = std::shared_ptr<ParsedArgs>(
        std::get<std::unique_ptr<ParsedArgs>>(std::move(parse_result)).release());
    return gb->add(id, type_id, std::move(parsed));
}
