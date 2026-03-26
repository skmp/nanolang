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

// ─── FlowArg2: base class for all pin/arg types ───

struct PortDesc2; // forward

enum class ArgKind : uint8_t { Net, Number, String, Expr };

struct ArgNet2;
struct ArgNumber2;
struct ArgString2;
struct ArgExpr2;

struct FlowArg2 : std::enable_shared_from_this<FlowArg2> {
    friend struct GraphBuilder;
    virtual ~FlowArg2() = default;

    ArgKind kind() const { return kind_; }
    bool is(ArgKind k) const { return kind_ == k; }

    std::shared_ptr<ArgNet2> as_net();
    std::shared_ptr<ArgNumber2> as_number();
    std::shared_ptr<ArgString2> as_string();
    std::shared_ptr<ArgExpr2> as_expr();

    // Context: which node/wire/port this arg belongs to
    std::shared_ptr<FlowNodeBuilder> node() const { return node_.lock(); }
    void node(const std::shared_ptr<FlowNodeBuilder>& n) { node_ = n; }

    std::shared_ptr<NetBuilder> wire() const { return wire_.lock(); }
    void wire(const std::shared_ptr<NetBuilder>& w) { wire_ = w; }

    const PortDesc2* port() const { return port_; }
    void port(const PortDesc2* p) { port_ = p; }

    std::shared_ptr<GraphBuilder> owner() const { return owner_; }

    // Computed name: "node.port_name" or "node.va_name[idx]" etc.
    std::string name() const;

protected:
    FlowArg2(ArgKind kind, const std::shared_ptr<GraphBuilder>& owner)
        : kind_(kind), owner_(owner) {}

    void mark_dirty();

private:
    ArgKind kind_;
    std::shared_ptr<GraphBuilder> owner_;
    std::weak_ptr<FlowNodeBuilder> node_;
    std::weak_ptr<NetBuilder> wire_;
    const PortDesc2* port_ = nullptr;
};

using FlowArg2Ptr = std::shared_ptr<FlowArg2>;

// ─── Concrete arg types ───

struct ArgNet2 : FlowArg2 {
    friend struct GraphBuilder;

    const NodeId& net_id() const { return net_id_; }
    void net_id(const NodeId& v);

    const std::shared_ptr<struct BuilderEntry>& entry() const { return entry_; }
    void entry(std::shared_ptr<struct BuilderEntry> v);

    // Convenience aliases
    const NodeId& first() const { return net_id_; }
    const std::shared_ptr<struct BuilderEntry>& second() const { return entry_; }

private:
    ArgNet2(NodeId id, std::shared_ptr<struct BuilderEntry> entry,
            const std::shared_ptr<GraphBuilder>& owner)
        : FlowArg2(ArgKind::Net, owner), net_id_(std::move(id)), entry_(std::move(entry)) {}

    NodeId net_id_;
    std::shared_ptr<struct BuilderEntry> entry_;
};

struct ArgNumber2 : FlowArg2 {
    friend struct GraphBuilder;

    double value() const { return value_; }
    void value(double v);

    bool is_float() const { return is_float_; }
    void is_float(bool v);

private:
    ArgNumber2(double v, bool f, const std::shared_ptr<GraphBuilder>& owner)
        : FlowArg2(ArgKind::Number, owner), value_(v), is_float_(f) {}

    double value_ = 0;
    bool is_float_ = false;
};

struct ArgString2 : FlowArg2 {
    friend struct GraphBuilder;

    const std::string& value() const { return value_; }
    void value(const std::string& v);

private:
    ArgString2(std::string v, const std::shared_ptr<GraphBuilder>& owner)
        : FlowArg2(ArgKind::String, owner), value_(std::move(v)) {}

    std::string value_;
};

struct ArgExpr2 : FlowArg2 {
    friend struct GraphBuilder;

    const std::string& expr() const { return expr_; }
    void expr(const std::string& v);

private:
    ArgExpr2(std::string e, const std::shared_ptr<GraphBuilder>& owner)
        : FlowArg2(ArgKind::Expr, owner), expr_(std::move(e)) {}

    std::string expr_;
};

// ─── ParsedArgs2: vector of FlowArg2Ptr with dirty tracking ───

struct ParsedArgs2 {
    int rewrite_input_count = 0;

    // Read access
    bool empty() const { return items_.empty(); }
    int size() const { return (int)items_.size(); }
    FlowArg2Ptr operator[](int i) { return items_[i]; }
    FlowArg2Ptr operator[](int i) const { return items_[i]; }

    using iterator = std::vector<FlowArg2Ptr>::iterator;
    using const_iterator = std::vector<FlowArg2Ptr>::const_iterator;
    iterator begin() { return items_.begin(); }
    iterator end() { return items_.end(); }
    const_iterator begin() const { return items_.begin(); }
    const_iterator end() const { return items_.end(); }
    FlowArg2Ptr back() { return items_.back(); }
    FlowArg2Ptr back() const { return items_.back(); }

    // Write access (marks dirty)
    void push_back(FlowArg2Ptr arg);
    void pop_back();
    void resize(int n);
    void insert(iterator pos, FlowArg2Ptr arg);
    void clear();

    // Set item at index (marks dirty)
    void set(int i, FlowArg2Ptr arg);

    // Owner
    std::shared_ptr<GraphBuilder> owner;

private:
    std::vector<FlowArg2Ptr> items_;
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

using Remaps = std::vector<FlowArg2Ptr>;
using Outputs = std::vector<FlowArg2Ptr>;

struct FlowNodeBuilder: BuilderEntry {
    FlowNodeBuilder(const std::shared_ptr<GraphBuilder>& owner = nullptr): BuilderEntry(IdCategory::Node, owner) { }

    NodeTypeID type_id = NodeTypeID::Unknown;
    std::shared_ptr<ParsedArgs2> parsed_args;
    std::shared_ptr<ParsedArgs2> parsed_va_args;
    Remaps remaps;
    Outputs outputs;
    Vec2 position = {0, 0};
    bool shadow = false;
    bool is_the_empty = false;          // true for the special $empty sentinel node
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

    // Sentinel accessors (created once, cached)
    FlowNodeBuilderPtr empty_node();    // the $empty sentinel node
    NetBuilderPtr unconnected_net();    // the $unconnected sentinel net
    void ensure_sentinels();            // create both if not yet created

    std::pair<NodeId, BuilderEntryPtr> find_or_create_net(const NodeId& name, bool for_source = false);

    BuilderEntryPtr find(const NodeId& id);

    FlowNodeBuilderPtr find_node(const NodeId& id);
    NetBuilderPtr find_net(const NodeId& name);

    void compact();

    NodeId next_id();

    // Rename an entry (node or net). Returns false if new_id already exists.
    bool rename(const BuilderEntryPtr& entry, const NodeId& new_id);

    // Arg factories — all pins are tracked in pins_
    FlowArg2Ptr build_arg_net(NodeId id, BuilderEntryPtr entry);
    FlowArg2Ptr build_arg_number(double value, bool is_float);
    FlowArg2Ptr build_arg_string(std::string value);
    FlowArg2Ptr build_arg_expr(std::string expr);

    const std::vector<FlowArg2Ptr>& pins() const { return pins_; }

    // Dirty tracking
    void mark_dirty() { dirty_ = true; }
    bool is_dirty() { return dirty_; }

private:
    bool dirty_ = false;
    std::vector<FlowArg2Ptr> pins_;
    FlowNodeBuilderPtr empty_;
    NetBuilderPtr unconnected_;
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
