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

// ─── Forward declarations ───

enum class IdCategory {
    Node,
    Net
};

struct GraphBuilder;
struct FlowNodeBuilder;
struct NetBuilder;

// ─── Arg types with dirty tracking ───

struct ArgNet2 {
    ArgNet2() = default;
    ArgNet2(NodeId id, std::shared_ptr<struct BuilderEntry> entry) : id_(std::move(id)), entry_(std::move(entry)) {}

    const NodeId& id() const { return id_; }
    void id(const NodeId& v, GraphBuilder* gb = nullptr);

    const std::shared_ptr<struct BuilderEntry>& entry() const { return entry_; }
    void entry(std::shared_ptr<struct BuilderEntry> v, GraphBuilder* gb = nullptr);

    // Convenience: access like old pair
    const NodeId& first() const { return id_; }
    const std::shared_ptr<struct BuilderEntry>& second() const { return entry_; }

private:
    NodeId id_;
    std::shared_ptr<struct BuilderEntry> entry_;
};

struct ArgNumber2 {
    ArgNumber2() = default;
    ArgNumber2(double v, bool f) : value_(v), is_float_(f) {}

    double value() const { return value_; }
    void value(double v, GraphBuilder* gb = nullptr);

    bool is_float() const { return is_float_; }
    void is_float(bool v, GraphBuilder* gb = nullptr);

private:
    double value_ = 0;
    bool is_float_ = false;
};

struct ArgString2 {
    ArgString2() = default;
    ArgString2(std::string v) : value_(std::move(v)) {}

    const std::string& value() const { return value_; }
    void value(const std::string& v, GraphBuilder* gb = nullptr);

private:
    std::string value_;
};

struct ArgExpr2 {
    ArgExpr2() = default;
    ArgExpr2(std::string e) : expr_(std::move(e)) {}

    const std::string& expr() const { return expr_; }
    void expr(const std::string& v, GraphBuilder* gb = nullptr);

private:
    std::string expr_;
};

using FlowArg2 = std::variant<ArgNet2, ArgNumber2, ArgString2, ArgExpr2>;

// ─── ParsedArgs2: vector wrapper with dirty tracking ───

struct ParsedArgs2 {
    int rewrite_input_count = 0;

    // Read access
    bool empty() const { return items_.empty(); }
    int size() const { return (int)items_.size(); }
    FlowArg2& operator[](int i) { return items_[i]; }
    const FlowArg2& operator[](int i) const { return items_[i]; }
    auto begin() { return items_.begin(); }
    auto end() { return items_.end(); }
    auto begin() const { return items_.begin(); }
    auto end() const { return items_.end(); }
    FlowArg2& back() { return items_.back(); }
    const FlowArg2& back() const { return items_.back(); }

    // Write access (marks dirty)
    void push_back(FlowArg2 arg);
    void pop_back();
    void resize(int n);
    void insert(typename std::vector<FlowArg2>::iterator pos, FlowArg2 arg);
    void clear();

    // Owner
    std::shared_ptr<GraphBuilder> owner;

private:
    std::vector<FlowArg2> items_;
};

// ─── BuilderEntry base ───

struct BuilderEntry: std::enable_shared_from_this<BuilderEntry> {
    BuilderEntry(IdCategory category, const std::shared_ptr<GraphBuilder>& owner = nullptr)
        : category_(category), owner_(owner) { }
    virtual ~BuilderEntry() = default;

    const NodeId& id() const { return id_; }
    void id(const NodeId& v);

    bool is(IdCategory cat) const { return category_ == cat; }

    std::shared_ptr<FlowNodeBuilder> as_Node();
    std::shared_ptr<NetBuilder> as_Net();

    std::shared_ptr<GraphBuilder> owner() const { return owner_; }
    void owner(const std::shared_ptr<GraphBuilder>& gb) { owner_ = gb; }

protected:
    void mark_dirty();

private:
    const IdCategory category_;
    NodeId id_;
    std::shared_ptr<GraphBuilder> owner_;
};

using BuilderEntryPtr = std::shared_ptr<BuilderEntry>;
using BuilderEntryWeak = std::weak_ptr<BuilderEntry>;

// ─── NetBuilder ───

struct NetBuilder: BuilderEntry {
    NetBuilder(const std::shared_ptr<GraphBuilder>& owner = nullptr): BuilderEntry(IdCategory::Net, owner) { }

    bool auto_wire() const { return auto_wire_; }
    void auto_wire(bool v) { auto_wire_ = v; }

    bool is_the_unconnected() const { return is_the_unconnected_; }
    void is_the_unconnected(bool v) { is_the_unconnected_ = v; }

    const BuilderEntryWeak& source() const { return source_; }
    void source(BuilderEntryWeak v) { source_ = std::move(v); mark_dirty(); }

    std::vector<BuilderEntryWeak>& destinations() { return destinations_; }
    const std::vector<BuilderEntryWeak>& destinations() const { return destinations_; }

    void compact();
    bool unused();
    void validate() const;

private:
    bool auto_wire_ = false;
    bool is_the_unconnected_ = false;
    BuilderEntryWeak source_;
    std::vector<BuilderEntryWeak> destinations_;
};

using NetBuilderPtr = std::shared_ptr<NetBuilder>;

// ─── FlowNodeBuilder ───

using Remaps = std::vector<ArgNet2>;
using Outputs = std::vector<ArgNet2>;

struct FlowNodeBuilder: BuilderEntry {
    FlowNodeBuilder(const std::shared_ptr<GraphBuilder>& owner = nullptr): BuilderEntry(IdCategory::Node, owner) { }

    NodeTypeID type_id = NodeTypeID::Unknown;
    std::shared_ptr<ParsedArgs2> parsed_args;
    std::shared_ptr<ParsedArgs2> parsed_va_args;
    Remaps remaps;
    Outputs outputs;
    Vec2 position = {0, 0};
    bool shadow = false;
    std::string error;

    std::string args_str() const;
};

using FlowNodeBuilderPtr = std::shared_ptr<FlowNodeBuilder>;

using BuilderResult = std::variant<std::pair<NodeId, FlowNodeBuilder>, BuilderError>;

// ─── GraphBuilder ───

struct GraphBuilder : std::enable_shared_from_this<GraphBuilder> {
    TypePool pool;
    std::map<NodeId, BuilderEntryPtr> entries;

    std::shared_ptr<FlowNodeBuilder> add_node(NodeId id, NodeTypeID type, std::shared_ptr<ParsedArgs2> args);

    void ensure_unconnected();

    std::pair<NodeId, BuilderEntryPtr> find_or_create_net(const NodeId& name, bool for_source = false);

    BuilderEntryPtr find(const NodeId& id);

    FlowNodeBuilderPtr find_node(const NodeId& id);
    NetBuilderPtr find_net(const NodeId& name);

    void compact();

    NodeId next_id();

    // Dirty tracking
    void mark_dirty() { dirty_ = true; }
    bool is_dirty() { return dirty_; }

private:
    bool dirty_ = false;
};

// ─── Parse/reconstruct helpers ───

using ParseResult = std::variant<std::shared_ptr<ParsedArgs2>, std::string>;
ParseResult parse_args_v2(const std::shared_ptr<GraphBuilder>& gb,
                          const std::vector<std::string>& exprs, bool is_expr = false);

std::string reconstruct_args_str(const ParsedArgs2& args);

// ─── Deserializer ───

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
