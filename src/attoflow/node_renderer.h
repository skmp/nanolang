#pragma once
#include "atto/graph_builder.h"
#include "atto/node_types2.h"
#include "imgui.h"
#include <string>
#include <memory>
#include <vector>
#include <variant>

// ─── Style ───

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

// ─── Vector helpers ───

inline ImVec2 v2add(ImVec2 a, ImVec2 b) { return {a.x + b.x, a.y + b.y}; }
inline ImVec2 v2sub(ImVec2 a, ImVec2 b) { return {a.x - b.x, a.y - b.y}; }
inline ImVec2 v2mul(ImVec2 a, float s) { return {a.x * s, a.y * s}; }

// ─── VisualPin: typed pin entry replacing magic sentinel indices ───

enum class VisualPinKind { Base, VaArg, AbsentOptional, AddDiamond, Remap };

struct VisualPin {
    VisualPinKind kind;
    FlowArg2Ptr arg;            // the actual arg (null for AbsentOptional, AddDiamond)
    const PortDesc2* port_desc; // port descriptor (null for remaps)
    PortKind2 port_kind;        // resolved shape (Data, BangTrigger, Lambda, BangNext)
    bool is_optional;           // for visual rendering of optional markers
};

struct VisualPinMap {
    std::vector<VisualPin> inputs;   // input pins in visual order
    std::vector<VisualPin> outputs;  // output pins in visual order (side-bang excluded for flow)
    bool has_side_bang = false;
    FlowArg2Ptr side_bang_arg;
    const PortDesc2* add_diamond_va_port = nullptr; // non-null if +diamond exists
    bool is_flow = false;

    static VisualPinMap build(const FlowNodeBuilderPtr& node, const NodeType2* nt);
};

// ─── NodeLayout: computed screen-space layout ───

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
    ImVec2 side_bang_pos() const {
        return {pos.x + width, pos.y + height * 0.5f};
    }
};

NodeLayout compute_node_layout(const FlowNodeBuilderPtr& node, const VisualPinMap& vpm,
                                ImVec2 canvas_origin, float zoom);

// ─── WireInfo ───

struct WireInfo {
    BuilderEntryPtr entry_;
    ImVec2 p0, p1, p2, p3;
    NodeId src_id, dst_id, net_id;
    BuilderEntryPtr entry() const { return entry_; }
    bool is_lambda() const { return entry_ && entry_->is(IdCategory::Node); }
};

// ─── AddPinHover ───

struct AddPinHover {
    FlowNodeBuilderPtr node;
    const PortDesc2* va_port;
    bool is_input;
};

// ─── HoverItem ───

using HoverItem = std::variant<std::monostate, BuilderEntryPtr, FlowArg2Ptr, AddPinHover>;

// ─── NodeRenderState: editor-derived state passed to renderer ───

struct NodeRenderState {
    bool selected;
    bool node_hovered;
    bool pin_hovered_on_this;
    FlowArg2Ptr hovered_pin;          // null if no pin hovered
    const AddPinHover* add_pin_hover; // null if no +diamond hovered
};

// ─── Hit-testing ───

struct HitResult {
    HoverItem item;
    float distance = 1e18f;
};

struct NodeHitTarget {
    FlowNodeBuilderPtr node;
    const NodeType2* nt;
    const NodeLayout* layout;
    const VisualPinMap* vpm;
};

float point_to_bezier_dist(ImVec2 p, ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3);
ImU32 pin_color(PortKind2 kind);

HitResult hit_test_wires(ImVec2 mouse, const std::vector<WireInfo>& wires, float zoom);
HitResult hit_test_node_bodies(ImVec2 mouse, const std::vector<NodeHitTarget>& nodes, float zoom);
HitResult hit_test_pins(ImVec2 mouse, const std::vector<NodeHitTarget>& nodes, float zoom);

// ─── Rendering functions ───

void render_background(ImDrawList* dl, ImVec2 canvas_p0, ImVec2 canvas_sz,
                        ImVec2 canvas_offset, float zoom);

void render_node(ImDrawList* dl, const FlowNodeBuilderPtr& node, const NodeType2* nt,
                 const NodeLayout& layout, const VisualPinMap& vpm,
                 const std::string& display_text, const NodeRenderState& state,
                 float zoom, bool draw_tooltips);

void render_wire(ImDrawList* dl, const WireInfo& w, float zoom);
void render_wire_label(ImDrawList* dl, const WireInfo& w, float zoom);
void render_wire_highlight(ImDrawList* dl, const WireInfo& w, float zoom);
void render_selection_rect(ImDrawList* dl, ImVec2 p0, ImVec2 p1);

WireInfo compute_wire_geometry(ImVec2 from, ImVec2 to, bool is_lambda, bool is_side_bang,
                                float zoom, const BuilderEntryPtr& entry,
                                const NodeId& src_id, const NodeId& dst_id, const NodeId& net_id);
