#pragma once
#include "model.h"
#include "types.h"
#include "args.h"
#include "node_types.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <variant>
#include <iostream>
#include <algorithm>
#include <stdexcept>

using NodeId = std::string;
using BuilderError = std::string;

struct FlowNodeBuilder;
struct NetBuilder;

using BuilderEntry = std::variant<FlowNodeBuilder, NetBuilder>;
using BuilderEntryPtr = std::shared_ptr<BuilderEntry>;
using BuilderEntryWeak = std::weak_ptr<BuilderEntry>;

// Named wire — one source, many destinations (weak refs to BuilderEntry, must be FlowNodeBuilder).
struct NetBuilder {
    bool auto_wire = false;

    BuilderEntryWeak source;
    std::vector<BuilderEntryWeak> destinations;

    // Remove expired weak refs, throw if any live ref is not a FlowNodeBuilder
    void compact();

    // compact + check if net has no live source and no live destinations
    bool unused();

    // Throw if any live weak ref is not a FlowNodeBuilder (or null)
    void validate() const;
};

// A node under construction — holds structured parsed args instead of raw string.
struct FlowNodeBuilder {
    NodeTypeID type_id = NodeTypeID::Unknown;
    std::shared_ptr<ParsedArgs2> parsed_args;
    Vec2 position = {0, 0};
    bool shadow = false;
    std::string error;

    // Reconstruct args string (for legacy code)
    std::string args_str() const;
};

using BuilderResult = std::variant<std::pair<NodeId, FlowNodeBuilder>, BuilderError>;

struct GraphBuilder {
    TypePool pool;
    std::map<NodeId, BuilderEntryPtr> entries;

    // Add a node
    FlowNodeBuilder& add_node(NodeId id, NodeTypeID type, std::shared_ptr<ParsedArgs2> args);

    // Get or create a net — throws if name exists as a node, or if for_source and source already set
    std::pair<const NodeId&, BuilderEntryPtr> find_or_create_net(const NodeId& name, bool for_source = false);

    // Find any entry by id
    BuilderEntryPtr find(const NodeId& id);

    // Find typed — throws if id exists but is wrong type
    std::pair<const NodeId&, BuilderEntryPtr> find_node(const NodeId& id);
    std::pair<const NodeId&, BuilderEntryPtr> find_net(const NodeId& name);

    // Remove unused nets
    void compact();
};

// Deserializer: parses raw strings into FlowNodeBuilder, with error fallback.
struct Deserializer {
    static BuilderResult parse_node(
        const NodeId& id, const std::string& type, const std::vector<std::string>& args);

    static FlowNodeBuilder& parse_or_error(
        GraphBuilder& gb,
        const NodeId& id, const std::string& type, const std::vector<std::string>& args);

    using ParseAttoResult = std::variant<std::shared_ptr<GraphBuilder>, BuilderError>;
    static ParseAttoResult parse_atto(std::istream& f);
};
