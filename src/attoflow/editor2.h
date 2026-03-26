#pragma once
#include "atto/graph_builder.h"
#include "atto/graph_editor_interfaces.h"
#include "atto/node_types2.h"
#include "imgui.h"
#include <string>
#include <memory>
#include <set>
#include <map>

// ─── Style (extern for use in layout computation) ───

struct Editor2Style {
    float node_min_width  = 80.0f;
    float node_height     = 40.0f;
    float pin_radius      = 5.0f;
    float pin_spacing     = 16.0f;
    float node_rounding   = 4.0f;
    float grid_step       = 20.0f;

    float wire_thickness      = 2.5f;
    float node_border         = 1.0f;
    float highlight_offset    = 2.0f;
    float highlight_thickness = 2.0f;
    float add_pin_line        = 1.5f;

    float pin_hit_radius_mul = 2.5f;
    float wire_hit_threshold = 30.0f;
    float node_hit_threshold_mul = 6.f;
    float dismiss_radius = 20.0f;
    float pin_priority_bias = 1e6f;

    ImU32 col_bg          = IM_COL32(30, 30, 40, 255);
    ImU32 col_grid        = IM_COL32(50, 50, 60, 255);

    ImU32 col_node        = IM_COL32(50, 55, 75, 220);
    ImU32 col_node_sel    = IM_COL32(80, 90, 130, 255);
    ImU32 col_node_err    = IM_COL32(130, 40, 40, 220);
    ImU32 col_node_border = IM_COL32(80, 80, 100, 255);
    ImU32 col_err_border  = IM_COL32(255, 80, 80, 255);
    ImU32 col_text        = IM_COL32(220, 220, 220, 255);

    ImU32 col_pin_data    = IM_COL32(100, 200, 100, 255);
    ImU32 col_pin_bang    = IM_COL32(255, 200, 80, 255);
    ImU32 col_pin_lambda  = IM_COL32(180, 130, 255, 255);
    ImU32 col_pin_hover   = IM_COL32(255, 255, 255, 255);
    ImU32 col_add_pin     = IM_COL32(120, 120, 140, 180);
    ImU32 col_add_pin_fg  = IM_COL32(200, 200, 220, 220);
    ImU32 col_opt_pin_fg  = IM_COL32(30, 30, 40, 255);

    ImU32 col_wire        = IM_COL32(200, 200, 100, 200);
    ImU32 col_wire_named  = IM_COL32(200, 200, 100, 120);
    ImU32 col_wire_lambda = IM_COL32(180, 130, 255, 200);

    ImU32 col_label_bg    = IM_COL32(30, 30, 40, 200);
    ImU32 col_label_text  = IM_COL32(180, 220, 255, 255);

    float tooltip_scale   = 1.0f;
};

extern Editor2Style S;

// ─── PinMapping: maps visible pin index → port descriptor ───

struct PinMapping {
    std::vector<int> pin_to_port;
    int base_count = 0;
    int va_count = 0;
    int add_pin_pos = -1;
    bool has_input_va = false;

    static PinMapping build(const FlowNodeBuilderPtr& node, const NodeType2* nt);

    int total() const { return (int)pin_to_port.size(); }
    bool is_base(int pin) const { return pin < base_count; }
    bool is_absent_optional(int pin) const { return pin < base_count && pin_to_port[pin] <= -3000; }
    int absent_port_index(int pin) const { return -(pin_to_port[pin] + 3000); }
    bool is_input_va(int pin) const { return pin >= base_count && pin < base_count + va_count; }
    bool is_add_diamond(int pin) const { return pin == add_pin_pos; }
    bool is_remap(int pin) const { return pin >= base_count + va_count + (has_input_va ? 1 : 0); }
    int port_index(int pin) const { return pin_to_port[pin]; }
    int remap_index(int pin) const { return -(pin_to_port[pin] + 2000); }
};

// ─── NodeLayout: computed screen-space layout for a node ───

struct NodeLayout {
    ImVec2 pos;
    float width;
    float height;
    int num_in;
    int num_out;
    float zoom;

    ImVec2 input_pin_pos(int i) const {
        return {pos.x + (i + 0.5f) * S.pin_spacing * zoom, pos.y};
    }
    ImVec2 output_pin_pos(int i) const {
        return {pos.x + (i + 0.5f) * S.pin_spacing * zoom, pos.y + height};
    }
    ImVec2 lambda_grab_pos() const {
        return {pos.x, pos.y + height * 0.5f};
    }
};

NodeLayout compute_node_layout(const FlowNodeBuilderPtr& node, ImVec2 canvas_origin, float zoom);

// ─── Forward declaration ───

class Editor2Pane;

// ─── Per-item editor implementations ───

struct NodeEditorImpl : INodeEditor, std::enable_shared_from_this<NodeEditorImpl> {
    Editor2Pane* pane;
    FlowNodeBuilderPtr node;
    NodeLayout layout;
    PinMapping pin_mapping;
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

class Editor2Pane : public IGraphEditor, public std::enable_shared_from_this<Editor2Pane> {
public:
    // Load an .atto file (instrument@atto:0 format)
    bool load(const std::string& path);

    // Draw the pane into the current ImGui context
    void draw();

    bool is_loaded() const { return gb_ != nullptr; }
    bool is_dirty() const { return gb_ && gb_->is_dirty(); }
    const std::string& file_path() const { return file_path_; }
    const std::string& tab_name() const { return tab_name_; }

    // IGraphEditor
    std::shared_ptr<INodeEditor> node_added(const NodeId& id, const std::shared_ptr<FlowNodeBuilder>& node) override;
    void node_removed(const NodeId& id) override;
    std::shared_ptr<INetEditor> net_added(const NodeId& id, const std::shared_ptr<NetBuilder>& net) override;
    void net_removed(const NodeId& id) override;

    // Mark wires for rebuild (called by editor impls)
    void invalidate_wires() { wires_dirty_ = true; }

private:
    std::shared_ptr<GraphBuilder> gb_;
    std::string file_path_;
    std::string tab_name_;

    // Canvas state
    ImVec2 canvas_offset_ = {0, 0};
    float canvas_zoom_ = 1.0f;

    // Interaction state
    struct AddPinHover {
        FlowNodeBuilderPtr node;
        const PortDesc2* va_port;
        bool is_input;
    };
    using HoverItem = std::variant<std::monostate, BuilderEntryPtr, FlowArg2Ptr, AddPinHover>;
    HoverItem hover_item_;
    bool draw_tooltips_ = true;
    std::set<FlowNodeBuilderPtr> selected_nodes_;
    bool dragging_started_ = false;
    bool drag_was_overlapping_ = false;
    bool selection_rect_active_ = false;
    ImVec2 selection_rect_start_ = {0, 0};
    int editing_link_id_ = -1;

    // Per-item editor caches
    std::map<NodeId, std::shared_ptr<NodeEditorImpl>> node_editors_;
    std::map<NodeId, std::shared_ptr<NetEditorImpl>> net_editors_;

    // Wire info for hover hit-testing
    struct WireInfo {
        BuilderEntryPtr entry_;
        ImVec2 p0, p1, p2, p3;
        NodeId src_id, dst_id, net_id;
        BuilderEntryPtr entry() const { return entry_; }
        bool is_lambda() const { return entry_ && entry_->is(IdCategory::Node); }
    };

    std::vector<WireInfo> cached_wires_;
    bool wires_dirty_ = true;

    void rebuild_wires(ImVec2 canvas_origin);

    // Hover detection — returns best hover match
    HoverItem detect_hover(ImVec2 mouse, ImVec2 canvas_origin);

    // Tooltip + highlight drawing (driven by hover_item parameter)
    void draw_hover_effects(ImDrawList* dl, ImVec2 canvas_origin,
                            const HoverItem& hover);

    // Drawing helpers
    void draw_node(ImDrawList* dl, const std::shared_ptr<NodeEditorImpl>& ned,
                   ImVec2 canvas_origin);
    void draw_net(ImDrawList* dl, const NetBuilderPtr& net,
                  ImVec2 canvas_origin);
};
