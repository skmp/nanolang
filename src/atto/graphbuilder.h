#pragma once
#include "model.h"
#include "types.h"
#include "args.h"
#include "node_types.h"
#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <iostream>

using NodeId = std::string;
using BuilderError = std::string;
using BuilderResult = std::variant<std::shared_ptr<struct FlowNodeBuilder>, BuilderError>;

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
    // Parse a node from pre-split args. Returns builder on success, error string on failure.
    static BuilderResult parse_node(
        const NodeId& id, const std::string& type, const std::vector<std::string>& args);

    // Parse a node and add to graph. On failure, creates an Error node instead.
    // Always returns a valid FlowNodeBuilder (added to gb).
    static std::shared_ptr<FlowNodeBuilder> parse_or_error(
        const std::shared_ptr<GraphBuilder>& gb,
        const NodeId& id, const std::string& type, const std::vector<std::string>& args);

    // Parse an instrument@atto:0 stream into a GraphBuilder.
    using ParseAttoResult = std::variant<std::shared_ptr<GraphBuilder>, BuilderError>;
    static ParseAttoResult parse_atto(std::istream& f);
};
