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

// ─── Forward declarations & aliases ───

enum class IdCategory {
    Node,
    Net
};

struct FlowNodeBuilder;
struct NetBuilder;

struct BuilderEntry: std::enable_shared_from_this<BuilderEntry> {
    BuilderEntry(IdCategory category) : category(category) { }
    virtual ~BuilderEntry() = default;

    NodeId id;

    bool is(IdCategory category) { return this->category == category; }
    
    std::shared_ptr<FlowNodeBuilder> as_Node() {
        return std::dynamic_pointer_cast<FlowNodeBuilder>(shared_from_this());
    }

    std::shared_ptr<NetBuilder> as_Net() {
        return std::dynamic_pointer_cast<NetBuilder>(shared_from_this());
    }

    private:
        const IdCategory category;
};

using BuilderEntryPtr = std::shared_ptr<BuilderEntry>;
using BuilderEntryWeak = std::weak_ptr<BuilderEntry>;

// ─── v2 argument types ───

using ArgNet2 = std::pair<NodeId, BuilderEntryPtr>;  // resolved net ref from find_or_create_net
struct ArgNumber2 { double value; bool is_float; };     // 42, 3.14
struct ArgString2 { std::string value; };               // "hello\"world"
struct ArgExpr2 { std::string expr; };                  // expression (contains $N, @N, operators, etc.)

using FlowArg2 = std::variant<ArgNet2, ArgNumber2, ArgString2, ArgExpr2>;

struct ParsedArgs2 : std::vector<FlowArg2> {
    using vector::vector;
    int rewrite_input_count = 0;  // count of unique $N refs across all expressions (contiguous from $0)
};

struct GraphBuilder; // forward for parse_args_v2

// Parse pre-split expressions into ParsedArgs2. Resolves $name tokens via gb.
using ParseResult = std::variant<std::shared_ptr<ParsedArgs2>, std::string>;
ParseResult parse_args_v2(const std::shared_ptr<GraphBuilder>& gb,
                          const std::vector<std::string>& exprs, bool is_expr = false);

// Reconstruct a space-separated args string from ParsedArgs2
std::string reconstruct_args_str(const ParsedArgs2& args);

// ─── Builder types ───

// Named wire — one source, many destinations (weak refs to BuilderEntry, must be FlowNodeBuilder).
struct NetBuilder: BuilderEntry {
    NetBuilder(): BuilderEntry(IdCategory::Net) { }

    bool auto_wire = false;
    bool is_the_unconnected = false;  // true for the special $unconnected sentinel

    BuilderEntryWeak source;
    std::vector<BuilderEntryWeak> destinations;

    void compact();
    bool unused();
    void validate() const;
};

using NetBuilderPtr = std::shared_ptr<NetBuilder>;

// Remap: $N → net mapping (from folded shadow inputs)
using Remaps = std::vector<ArgNet2>;
using Outputs = std::vector<ArgNet2>;

// A node under construction — holds structured parsed args instead of raw string.
struct FlowNodeBuilder: BuilderEntry {
    FlowNodeBuilder(): BuilderEntry(IdCategory::Node) { }

    NodeTypeID type_id = NodeTypeID::Unknown;
    std::shared_ptr<ParsedArgs2> parsed_args;      // base pins (1:1 with descriptor)
    std::shared_ptr<ParsedArgs2> parsed_va_args;   // va_args pins
    Remaps remaps;                // $N → net mapping (remaps[0] = net for $0, etc.)
    Outputs outputs;              // output pin → net mapping (1:1 with descriptor output_ports)
    Vec2 position = {0, 0};
    bool shadow = false;          // only used during migration, must be false after folding
    std::string error;

    std::string args_str() const;
};

using FlowNodeBuilderPtr = std::shared_ptr<FlowNodeBuilder>;

using BuilderResult = std::variant<std::pair<NodeId, FlowNodeBuilder>, BuilderError>;

struct GraphBuilder {
    TypePool pool;
    std::map<NodeId, BuilderEntryPtr> entries;

    std::shared_ptr<FlowNodeBuilder> add_node(NodeId id, NodeTypeID type, std::shared_ptr<ParsedArgs2> args);

    // Ensure the $unconnected sentinel net exists
    void ensure_unconnected();

    std::pair<NodeId, BuilderEntryPtr> find_or_create_net(const NodeId& name, bool for_source = false);

    BuilderEntryPtr find(const NodeId& id);

    FlowNodeBuilderPtr find_node(const NodeId& id);
    NetBuilderPtr find_net(const NodeId& name);

    void compact();

    // Returns the next unused $a-N id
    NodeId next_id();
};

// Deserializer: parses raw strings into FlowNodeBuilder, with error fallback.
struct Deserializer {
    static BuilderResult parse_node(
        const std::shared_ptr<GraphBuilder>& gb,
        const NodeId& id, const std::string& type, const std::vector<std::string>& args);

    static FlowNodeBuilder& parse_or_error(
        const std::shared_ptr<GraphBuilder>& gb,
        const NodeId& id, const std::string& type, const std::vector<std::string>& args);

    using ParseAttoResult = std::variant<std::shared_ptr<GraphBuilder>, BuilderError>;
    static ParseAttoResult parse_atto(std::istream& f);
};
