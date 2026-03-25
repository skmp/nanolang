#pragma once
#include "model.h"
#include "types.h"
#include "args.h"
#include "node_types.h"
#include <string>
#include <vector>
#include <memory>

using NodeId = std::string;

// A node under construction — holds structured parsed args instead of raw string.
struct FlowNodeBuilder {
    NodeId id;
    NodeTypeID type_id = NodeTypeID::Unknown;
    std::shared_ptr<ParsedArgs> parsed_args;
    Vec2 position = {0, 0};
    bool shadow = false;
    std::string error;

    // Reconstruct args string (for legacy code)
    std::string args_str() const;
};

struct GraphBuilder {
    TypePool pool;
    std::vector<std::shared_ptr<FlowNodeBuilder>> builders;

    // Add a pre-built node
    std::shared_ptr<FlowNodeBuilder> add(NodeId id, NodeTypeID type, std::shared_ptr<ParsedArgs> args);

    void link(const std::string& from, const std::string& to);

    std::shared_ptr<FlowNodeBuilder> find(const NodeId& id);
};

// Deserializer: parses raw strings into FlowNodeBuilder, with error fallback.
struct Deserializer {
    // Parse a node from raw strings. On failure, returns an Error node builder.
    static std::shared_ptr<FlowNodeBuilder> parse_node(
        const std::shared_ptr<GraphBuilder>& gb,
        const NodeId& id, const std::string& type, const std::string& args_str);
};
