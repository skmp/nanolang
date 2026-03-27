#pragma once
#include "editor_pane.h"
#include "visual_editor.h"
#include "atto/graph_editor_interfaces.h"
#include <string>
#include <memory>
#include <map>

// ─── Forward declaration ───

class Editor2Pane;

// ─── Per-item editor implementations ───

struct NodeEditorImpl : INodeEditor, std::enable_shared_from_this<NodeEditorImpl> {
    Editor2Pane* pane;
    FlowNodeBuilderPtr node;
    NodeLayout layout;
    VisualPinMap vpm;
    std::string display_text;
    bool has_error = false;

    NodeEditorImpl(Editor2Pane* p, const FlowNodeBuilderPtr& n) : pane(p), node(n) {}

    void rebuild(ImVec2 canvas_origin, float zoom);

    void node_mutated(const std::shared_ptr<FlowNodeBuilder>& node) override;
    void node_layout_changed(const std::shared_ptr<FlowNodeBuilder>& node) override;
    std::shared_ptr<IArgNetEditor> create_arg_net_editor(const std::shared_ptr<ArgNet2>& arg) override;
    std::shared_ptr<IArgNumberEditor> create_arg_number_editor(const std::shared_ptr<ArgNumber2>& arg) override;
    std::shared_ptr<IArgStringEditor> create_arg_string_editor(const std::shared_ptr<ArgString2>& arg) override;
    std::shared_ptr<IArgExprEditor> create_arg_expr_editor(const std::shared_ptr<ArgExpr2>& arg) override;
};

struct NetEditorImpl : INetEditor {
    Editor2Pane* pane;
    NetBuilderPtr net;
    NetEditorImpl(Editor2Pane* p, const NetBuilderPtr& n) : pane(p), net(n) {}
    void net_mutated(const std::shared_ptr<NetBuilder>& net) override;
};

struct ArgNetEditorImpl : IArgNetEditor {
    Editor2Pane* pane;
    std::shared_ptr<ArgNet2> arg;
    ArgNetEditorImpl(Editor2Pane* p, const std::shared_ptr<ArgNet2>& a) : pane(p), arg(a) {}
    void arg_net_mutated(const std::shared_ptr<ArgNet2>& arg) override;
};

struct ArgNumberEditorImpl : IArgNumberEditor {
    Editor2Pane* pane;
    std::shared_ptr<ArgNumber2> arg;
    ArgNumberEditorImpl(Editor2Pane* p, const std::shared_ptr<ArgNumber2>& a) : pane(p), arg(a) {}
    void arg_number_mutated(const std::shared_ptr<ArgNumber2>& arg) override;
};

struct ArgStringEditorImpl : IArgStringEditor {
    Editor2Pane* pane;
    std::shared_ptr<ArgString2> arg;
    ArgStringEditorImpl(Editor2Pane* p, const std::shared_ptr<ArgString2>& a) : pane(p), arg(a) {}
    void arg_string_mutated(const std::shared_ptr<ArgString2>& arg) override;
};

struct ArgExprEditorImpl : IArgExprEditor {
    Editor2Pane* pane;
    std::shared_ptr<ArgExpr2> arg;
    ArgExprEditorImpl(Editor2Pane* p, const std::shared_ptr<ArgExpr2>& a) : pane(p), arg(a) {}
    void arg_expr_mutated(const std::shared_ptr<ArgExpr2>& arg) override;
};

// ─── Editor2Pane ───

class Editor2Pane : public IEditorPane, public VisualEditor,
                    public IGraphEditor, public std::enable_shared_from_this<Editor2Pane> {
public:
    Editor2Pane(const std::shared_ptr<GraphBuilder>& gb,
                const std::shared_ptr<AttoEditorSharedState>& shared);

    // IEditorPane
    void draw() override;
    const char* type_name() const override { return "graph"; }
    std::shared_ptr<GraphBuilder> get_graph_builder() const override { return gb_; }

    // IGraphEditor (observer)
    std::shared_ptr<INodeEditor> node_added(const NodeId& id, const std::shared_ptr<FlowNodeBuilder>& node) override;
    void node_removed(const NodeId& id) override;
    std::shared_ptr<INetEditor> net_added(const NodeId& id, const std::shared_ptr<NetBuilder>& net) override;
    void net_removed(const NodeId& id) override;

    void invalidate_wires() { wires_dirty_ = true; }

protected:
    // VisualEditor hooks
    void draw_content(const CanvasFrame& frame) override;
    HoverItem do_detect_hover(ImVec2 mouse, ImVec2 canvas_origin) override;
    void do_draw_hover_effects(ImDrawList* dl, ImVec2 canvas_origin, const HoverItem& hover) override;
    FlowNodeBuilderPtr hover_to_node(const HoverItem& item) override;
    bool test_drag_overlap(const FlowNodeBuilderPtr& sel, float nx, float ny) override;
    std::vector<BoxTestNode> get_box_test_nodes() override;
    void on_nodes_moved() override { wires_dirty_ = true; }

    // Wire connection hooks
    ImVec2 get_pin_screen_pos(const FlowArg2Ptr& pin) override;
    PortPosition2 get_pin_position(const FlowArg2Ptr& pin) override;
    bool pin_is_connected(const FlowArg2Ptr& pin) override;
    bool do_connect_pins(const FlowArg2Ptr& from_pin, PortPosition2 from_pos,
                          const FlowArg2Ptr& to_pin, PortPosition2 to_pos) override;
    bool do_disconnect_pin(const FlowArg2Ptr& pin, PortPosition2 pos) override;
    void do_reconnect_pin(const FlowArg2Ptr& pin, PortPosition2 pos) override;
    void do_delete_hovered(const HoverItem& item) override;

private:
    friend struct NodeEditorImpl;
    friend struct NetEditorImpl;

    std::shared_ptr<GraphBuilder> gb_;

    // Per-item editor caches
    std::map<NodeId, std::shared_ptr<NodeEditorImpl>> node_editors_;
    std::map<NodeId, std::shared_ptr<NetEditorImpl>> net_editors_;

    // Wire cache
    std::vector<WireInfo> cached_wires_;
    bool wires_dirty_ = true;

    // Wire grab undo state (for restoring on cancel)
    struct GrabUndoState {
        FlowArg2Ptr pin;
        NodeId old_net_id;
        BuilderEntryPtr old_entry;
    };
    GrabUndoState grab_undo_;

    void rebuild_wires(ImVec2 canvas_origin);
};

// Factory
std::shared_ptr<IEditorPane> make_editor2(
    const std::shared_ptr<GraphBuilder>& gb,
    const std::shared_ptr<AttoEditorSharedState>& shared);
