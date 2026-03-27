#pragma once
#include <memory>
#include <string>

// Forward declarations — no graph_builder.h dependency
struct FlowNodeBuilder;
struct NetBuilder;
struct ArgNet2;
struct ArgNumber2;
struct ArgString2;
struct ArgExpr2;
using NodeId = std::string;

// ─── Arg editor interfaces (per-type) ───

struct IArgNetEditor {
    virtual ~IArgNetEditor() = default;
    virtual void arg_net_mutated(const std::shared_ptr<ArgNet2>& arg) = 0;
};

struct IArgNumberEditor {
    virtual ~IArgNumberEditor() = default;
    virtual void arg_number_mutated(const std::shared_ptr<ArgNumber2>& arg) = 0;
};

struct IArgStringEditor {
    virtual ~IArgStringEditor() = default;
    virtual void arg_string_mutated(const std::shared_ptr<ArgString2>& arg) = 0;
};

struct IArgExprEditor {
    virtual ~IArgExprEditor() = default;
    virtual void arg_expr_mutated(const std::shared_ptr<ArgExpr2>& arg) = 0;
};

// ─── Node editor ───

struct INodeEditor {
    virtual ~INodeEditor() = default;

    // Structural change: args, ports, connections changed
    virtual void node_mutated(const std::shared_ptr<FlowNodeBuilder>& node) = 0;

    // Visual-only change: position moved (does NOT bubble up)
    virtual void node_layout_changed(const std::shared_ptr<FlowNodeBuilder>& node) = 0;

    // Arg editor factories — called per-arg when node is registered
    virtual std::shared_ptr<IArgNetEditor> create_arg_net_editor(const std::shared_ptr<ArgNet2>& arg) = 0;
    virtual std::shared_ptr<IArgNumberEditor> create_arg_number_editor(const std::shared_ptr<ArgNumber2>& arg) = 0;
    virtual std::shared_ptr<IArgStringEditor> create_arg_string_editor(const std::shared_ptr<ArgString2>& arg) = 0;
    virtual std::shared_ptr<IArgExprEditor> create_arg_expr_editor(const std::shared_ptr<ArgExpr2>& arg) = 0;
};

// ─── Net editor ───

struct INetEditor {
    virtual ~INetEditor() = default;
    virtual void net_mutated(const std::shared_ptr<NetBuilder>& net) = 0;
};

// ─── Graph editor (top-level observer) ───

struct IGraphEditor {
    virtual ~IGraphEditor() = default;

    // Node lifecycle — returns per-node editor to attach
    virtual std::shared_ptr<INodeEditor> node_added(const NodeId& id, const std::shared_ptr<FlowNodeBuilder>& node) = 0;
    virtual void node_removed(const NodeId& id) = 0;

    // Net lifecycle — returns per-net editor to attach
    virtual std::shared_ptr<INetEditor> net_added(const NodeId& id, const std::shared_ptr<NetBuilder>& net) = 0;
    virtual void net_removed(const NodeId& id) = 0;
};
