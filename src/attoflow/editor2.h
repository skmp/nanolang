#pragma once
#include "editor_pane.h"
#include "node_renderer.h"
#include "atto/graph_editor_interfaces.h"
#include <string>
#include <memory>
#include <set>
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

    // INodeEditor
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

class Editor2Pane : public IEditorPane, public IGraphEditor, public std::enable_shared_from_this<Editor2Pane> {
public:
    // IEditorPane
    bool load(const std::string& path) override;
    void draw() override;
    bool is_loaded() const override { return gb_ != nullptr; }
    bool is_dirty() const override { return gb_ && gb_->is_dirty(); }
    const std::string& file_path() const override { return file_path_; }
    const std::string& tab_name() const override { return tab_name_; }

    // IGraphEditor
    std::shared_ptr<INodeEditor> node_added(const NodeId& id, const std::shared_ptr<FlowNodeBuilder>& node) override;
    void node_removed(const NodeId& id) override;
    std::shared_ptr<INetEditor> net_added(const NodeId& id, const std::shared_ptr<NetBuilder>& net) override;
    void net_removed(const NodeId& id) override;

    void invalidate_wires() { wires_dirty_ = true; }

private:
    std::shared_ptr<GraphBuilder> gb_;
    std::string file_path_;
    std::string tab_name_;

    // Canvas state
    ImVec2 canvas_offset_ = {0, 0};
    float canvas_zoom_ = 1.0f;

    // Interaction state
    HoverItem hover_item_;
    bool draw_tooltips_ = true;
    std::set<FlowNodeBuilderPtr> selected_nodes_;
    bool dragging_started_ = false;
    bool drag_was_overlapping_ = false;
    bool selection_rect_active_ = false;
    ImVec2 selection_rect_start_ = {0, 0};

    // Per-item editor caches
    std::map<NodeId, std::shared_ptr<NodeEditorImpl>> node_editors_;
    std::map<NodeId, std::shared_ptr<NetEditorImpl>> net_editors_;

    // Wire cache
    std::vector<WireInfo> cached_wires_;
    bool wires_dirty_ = true;

    void rebuild_wires(ImVec2 canvas_origin);
    HoverItem detect_hover(ImVec2 mouse, ImVec2 canvas_origin);
    void draw_hover_effects(ImDrawList* dl, ImVec2 canvas_origin, const HoverItem& hover);
};
