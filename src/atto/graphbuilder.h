#pragma once
#include "model.h"
#include "types.h"
#include "node_types.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <variant>
#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <cstdio>

using NodeId = std::string;
using BuilderError = std::string;

// ─── v2 argument types (independent of legacy args.h) ───

struct ArgNet2 { std::string id; };                     // "$id" or "$unconnected"
struct ArgNumber2 { double value; bool is_float; };     // 42, 3.14
struct ArgString2 { std::string value; };               // "hello\"world"
struct ArgExpr2 { std::string expr; };                  // expression (contains $N, @N, operators, etc.)

using FlowArg2 = std::variant<ArgNet2, ArgNumber2, ArgString2, ArgExpr2>;
using ParsedArgs2 = std::vector<FlowArg2>;

// Parse pre-split expressions into ParsedArgs2.
using ParseResult = std::variant<std::shared_ptr<ParsedArgs2>, std::string>;
ParseResult parse_args_v2(const std::vector<std::string>& exprs, bool is_expr = false);

// Reconstruct a space-separated args string from ParsedArgs2
std::string reconstruct_args_str(const ParsedArgs2& args);

// ─── Builder types ───

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

    void compact();
    bool unused();
    void validate() const;
};

// A node under construction — holds structured parsed args instead of raw string.
struct FlowNodeBuilder {
    NodeTypeID type_id = NodeTypeID::Unknown;
    std::shared_ptr<ParsedArgs2> parsed_args;
    Vec2 position = {0, 0};
    bool shadow = false;
    std::string error;

    std::string args_str() const;
};

using BuilderResult = std::variant<std::pair<NodeId, FlowNodeBuilder>, BuilderError>;

struct GraphBuilder {
    TypePool pool;
    std::map<NodeId, BuilderEntryPtr> entries;

    FlowNodeBuilder& add_node(NodeId id, NodeTypeID type, std::shared_ptr<ParsedArgs2> args);

    std::pair<const NodeId&, BuilderEntryPtr> find_or_create_net(const NodeId& name, bool for_source = false);

    BuilderEntryPtr find(const NodeId& id);

    std::pair<const NodeId&, BuilderEntryPtr> find_node(const NodeId& id);
    std::pair<const NodeId&, BuilderEntryPtr> find_net(const NodeId& name);

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
