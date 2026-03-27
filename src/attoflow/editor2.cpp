#include "editor2.h"
#include "atto/graph_builder.h"
#include "atto/node_types2.h"
#include "imgui.h"
#include <cmath>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <stdexcept>

// ─── Style ───

static struct {
    // Layout
    float node_min_width  = 80.0f;
    float node_height     = 40.0f;
    float pin_radius      = 5.0f;
    float pin_spacing     = 16.0f;
    float node_rounding   = 4.0f;
    float grid_step       = 20.0f;

    // Thickness
    float wire_thickness      = 2.5f;
    float node_border         = 1.0f;   // implicit from AddRect
    float highlight_offset    = 2.0f;
    float highlight_thickness = 2.0f;
    float add_pin_line        = 1.5f;

    // Hit testing
    float pin_hit_radius_mul = 2.5f;

    // Canvas colors
    ImU32 col_bg          = IM_COL32(30, 30, 40, 255);
    ImU32 col_grid        = IM_COL32(50, 50, 60, 255);

    // Node colors
    ImU32 col_node        = IM_COL32(50, 55, 75, 220);
    ImU32 col_node_sel    = IM_COL32(80, 90, 130, 255);
    ImU32 col_node_err    = IM_COL32(130, 40, 40, 220);
    ImU32 col_node_border = IM_COL32(80, 80, 100, 255);
    ImU32 col_err_border  = IM_COL32(255, 80, 80, 255);
    ImU32 col_text        = IM_COL32(220, 220, 220, 255);

    // Pin colors
    ImU32 col_pin_data    = IM_COL32(100, 200, 100, 255);
    ImU32 col_pin_bang    = IM_COL32(255, 200, 80, 255);
    ImU32 col_pin_lambda  = IM_COL32(180, 130, 255, 255);
    ImU32 col_pin_hover   = IM_COL32(255, 255, 255, 255);
    ImU32 col_add_pin     = IM_COL32(120, 120, 140, 180);
    ImU32 col_add_pin_fg  = IM_COL32(200, 200, 220, 220);
    ImU32 col_opt_pin_fg  = IM_COL32(30, 30, 40, 255);

    // Wire colors
    ImU32 col_wire        = IM_COL32(200, 200, 100, 200);
    ImU32 col_wire_named  = IM_COL32(200, 200, 100, 120);
    ImU32 col_wire_lambda = IM_COL32(180, 130, 255, 200);

    // Net label colors
    ImU32 col_label_bg    = IM_COL32(30, 30, 40, 200);
    ImU32 col_label_text  = IM_COL32(180, 220, 255, 255);

    // Tooltip
    float tooltip_scale   = 1.0f;
} S;

// ─── Helpers ───

static inline ImVec2 v2add(ImVec2 a, ImVec2 b) { return {a.x + b.x, a.y + b.y}; }
static inline ImVec2 v2sub(ImVec2 a, ImVec2 b) { return {a.x - b.x, a.y - b.y}; }
static inline ImVec2 v2mul(ImVec2 a, float s) { return {a.x * s, a.y * s}; }

// Maps visible pin index → descriptor port index for input pins
// Sections: base_args ArgNet2, va_args ArgNet2, [+diamond], remaps
struct PinMapping {
    std::vector<int> pin_to_port;   // visible pin idx → port index in parsed_args/descriptor
    int base_count = 0;             // visible base pins (ArgNet2 in parsed_args + absent optionals)
    int va_count = 0;               // visible va_args pins (ArgNet2 in parsed_va_args)
    int add_pin_pos = -1;           // position of +diamond (-1 if none)
    bool has_va = false;

    static PinMapping build(const FlowNodeBuilderPtr& node, const NodeType2* nt) {
        PinMapping m;
        m.has_va = nt && nt->va_args != nullptr;
        int parsed_size = node->parsed_args ? (int)node->parsed_args->size() : 0;

        // Base args: track which parsed_args indices are ArgNet2
        if (node->parsed_args) {
            for (int i = 0; i < parsed_size; i++) {
                if (std::holds_alternative<ArgNet2>((*node->parsed_args)[i])) {
                    m.pin_to_port.push_back(i);
                    m.base_count++;
                }
            }
        }
        // Absent trailing optional ports: show as pins beyond parsed_args
        if (nt) {
            for (int i = parsed_size; i < nt->total_inputs(); i++) {
                if (i >= nt->num_inputs) { // port is in the optional range
                    m.pin_to_port.push_back(-3000 - i);
                    m.base_count++;
                }
            }
        }
        // Va_args
        if (node->parsed_va_args) {
            for (int i = 0; i < (int)node->parsed_va_args->size(); i++) {
                if (std::holds_alternative<ArgNet2>((*node->parsed_va_args)[i])) {
                    m.pin_to_port.push_back(-(i + 1)); // negative = va_args index (1-based)
                    m.va_count++;
                }
            }
        }
        // +diamond slot
        if (m.has_va) {
            m.add_pin_pos = (int)m.pin_to_port.size();
            m.pin_to_port.push_back(-1000); // sentinel for +diamond
        }
        // Remaps
        for (int i = 0; i < (int)node->remaps.size(); i++) {
            m.pin_to_port.push_back(-2000 - i); // sentinel for remap i
        }
        return m;
    }

    int total() const { return (int)pin_to_port.size(); }
    bool is_base(int pin) const { return pin < base_count; }
    bool is_absent_optional(int pin) const { return pin < base_count && pin_to_port[pin] <= -3000; }
    int absent_port_index(int pin) const { return -(pin_to_port[pin] + 3000); }
    bool is_va(int pin) const { return pin >= base_count && pin < base_count + va_count; }
    bool is_add_diamond(int pin) const { return pin == add_pin_pos; }
    bool is_remap(int pin) const { return pin >= base_count + va_count + (has_va ? 1 : 0); }
    int port_index(int pin) const { return pin_to_port[pin]; }
    int remap_index(int pin) const { return -(pin_to_port[pin] + 2000); }
};

// Computed node layout for drawing
struct NodeLayout {
    ImVec2 pos;      // top-left screen position
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

static NodeLayout compute_node_layout(const FlowNodeBuilderPtr& node, ImVec2 canvas_origin, float zoom) {
    auto* nt = find_node_type2(node->type_id);
    std::string display = nt ? nt->name : "?";
    std::string args = node->args_str();
    if (!args.empty()) display += " " + args;

    float font_size = ImGui::GetFontSize() * zoom;
    ImVec2 text_sz = ImGui::CalcTextSize(display.c_str());
    float text_w = text_sz.x * zoom + 16.0f * zoom;

    auto pm = PinMapping::build(node, nt);
    int num_in = pm.total();
    int num_out = nt ? nt->num_outputs : 1;
    if (node->type_id == NodeTypeID::Expr || node->type_id == NodeTypeID::ExprBang) {
        int args_count = node->parsed_args ? (int)node->parsed_args->size() : 0;
        if (node->type_id == NodeTypeID::ExprBang) num_out = 1 + std::max(1, args_count);
        else num_out = std::max(1, args_count);
    }

    float pin_w_top = std::max(0, num_in) * S.pin_spacing * zoom;
    float pin_w_bot = std::max(0, num_out) * S.pin_spacing * zoom;
    float node_w = std::max({S.node_min_width * zoom, text_w, pin_w_top, pin_w_bot});
    float node_h = S.node_height * zoom;

    ImVec2 pos = {canvas_origin.x + node->position.x * zoom,
                  canvas_origin.y + node->position.y * zoom};

    return {pos, node_w, node_h, num_in, num_out, zoom};
}

static ImU32 pin_color(PortKind2 kind) {
    switch (kind) {
    case PortKind2::BangTrigger:
    case PortKind2::BangNext: return S.col_pin_bang;
    case PortKind2::Lambda:   return S.col_pin_lambda;
    default:                  return S.col_pin_data;
    }
}

// ─── Load ───

bool Editor2Pane::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        fprintf(stderr, "Editor2: cannot open %s\n", path.c_str());
        return false;
    }

    auto result = Deserializer::parse_atto(f);
    if (auto* err = std::get_if<BuilderError>(&result)) {
        fprintf(stderr, "Editor2: %s\n", err->c_str());
        return false;
    }

    gb_ = std::get<std::shared_ptr<GraphBuilder>>(result);
    file_path_ = path;

    // Extract tab name from path
    auto slash = path.find_last_of("/\\");
    tab_name_ = (slash != std::string::npos) ? path.substr(slash + 1) : path;

    printf("Editor2: loaded %zu entries from %s\n", gb_->entries.size(), path.c_str());
    return true;
}

// ─── Draw ───

void Editor2Pane::draw() {
    if (!gb_) {
        ImGui::TextDisabled("No file loaded");
        return;
    }

    ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
    ImVec2 canvas_sz = ImGui::GetContentRegionAvail();
    if (canvas_sz.x < 50.0f) canvas_sz.x = 50.0f;
    if (canvas_sz.y < 50.0f) canvas_sz.y = 50.0f;

    ImGui::InvisibleButton("##canvas2", canvas_sz,
                            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool canvas_hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background
    dl->AddRectFilled(canvas_p0, v2add(canvas_p0, canvas_sz), S.col_bg);

    ImVec2 canvas_origin = v2add(canvas_p0, canvas_offset_);

    // Grid
    float grid_step = S.grid_step * canvas_zoom_;
    if (grid_step > 5.0f) {
        for (float x = fmodf(canvas_offset_.x, grid_step); x < canvas_sz.x; x += grid_step)
            dl->AddLine({canvas_p0.x + x, canvas_p0.y}, {canvas_p0.x + x, canvas_p0.y + canvas_sz.y}, S.col_grid);
        for (float y = fmodf(canvas_offset_.y, grid_step); y < canvas_sz.y; y += grid_step)
            dl->AddLine({canvas_p0.x, canvas_p0.y + y}, {canvas_p0.x + canvas_sz.x, canvas_p0.y + y}, S.col_grid);
    }

    // Clip
    dl->PushClipRect(canvas_p0, v2add(canvas_p0, canvas_sz), true);

    // Draw nodes (skip shadows)
    for (auto& [id, entry] : gb_->entries) {
        if (auto node = entry->as_Node()) {
            if (node->shadow) throw std::logic_error("Editor2Pane: shadow nodes must be folded before rendering (id: " + id + ")");
            draw_node(dl, node, canvas_origin);
        }
    }

    // Draw wires by iterating each node's inputs/outputs/remaps
    for (auto& [dst_id, dst_entry] : gb_->entries) {
        auto dst_node = dst_entry->as_Node();
        if (!dst_node) continue;

        auto* dst_nt = find_node_type2(dst_node->type_id);
        if (!dst_nt) continue;
        auto dst_layout = compute_node_layout(dst_node, canvas_origin, canvas_zoom_);

        // Helper: draw wire from a source to destination pin at index dst_pin
        // Source can be a NetBuilder (regular wire) or FlowNodeBuilder (lambda capture)
        auto draw_wire_to_pin = [&](int dst_pin, const BuilderEntryPtr& entry, const NodeId& net_id) {
            if (!entry) return;

            FlowNodeBuilderPtr src_node = nullptr;
            bool named = false;
            bool is_lambda = false;
            int source_pin = 0;

            if (auto net = entry->as_Net()) {
                if (net->is_the_unconnected) return;
                auto src_ptr = net->source.lock();
                src_node = src_ptr ? src_ptr->as_Node() : nullptr;
                if (!src_node) return;
                named = !net->auto_wire;
                for (int k = 0; k < (int)src_node->outputs.size(); k++) {
                    if (src_node->outputs[k].second == entry) {
                        source_pin = k;
                        break;
                    }
                }
            } else if (auto node = entry->as_Node()) {
                src_node = node;
                is_lambda = true;
            } else {
                return;
            }

            auto* src_nt = find_node_type2(src_node->type_id);
            auto src_layout = compute_node_layout(src_node, canvas_origin, canvas_zoom_);
            ImVec2 to = dst_layout.input_pin_pos(dst_pin);
            float th = S.wire_thickness * canvas_zoom_;

            ImVec2 from;
            if (is_lambda) {
                from = src_layout.lambda_grab_pos();
                float dx = std::max(std::abs(to.x - from.x) * 0.5f, 30.0f * canvas_zoom_);
                float dy = std::max(std::abs(to.y - from.y) * 0.5f, 30.0f * canvas_zoom_);
                ImU32 col = S.col_wire_lambda;
                dl->AddBezierCubic(from, {from.x - dx, from.y}, {to.x, to.y - dy}, to, col, th);
            } else {
                bool is_side_bang = src_nt && src_nt->is_flow() &&
                    source_pin < (src_nt->num_outputs) &&
                    src_nt->output_ports && src_nt->output_ports[source_pin].kind == PortKind2::BangNext;

                if (is_side_bang) {
                    from = {src_layout.pos.x + src_layout.width, src_layout.pos.y + src_layout.height * 0.5f};
                    float dx = std::max(std::abs(to.x - from.x) * 0.5f, 30.0f * canvas_zoom_);
                    float dy = std::max(std::abs(to.y - from.y) * 0.5f, 30.0f * canvas_zoom_);
                    ImU32 col = named ? S.col_wire_named : S.col_wire;
                    dl->AddBezierCubic(from, {from.x + dx, from.y}, {to.x, to.y - dy}, to, col, th);
                } else {
                    from = src_layout.output_pin_pos(source_pin);
                    ImU32 col = named ? S.col_wire_named : S.col_wire;
                    float dy = std::max(std::abs(to.y - from.y) * 0.5f, 30.0f * canvas_zoom_);
                    dl->AddBezierCubic(from, {from.x, from.y + dy}, {to.x, to.y - dy}, to, col, th);
                }
            }

            // Label for named nets
            if (named) {
                float font_size = ImGui::GetFontSize() * canvas_zoom_ * 0.8f;
                if (font_size > 5.0f) {
                    ImVec2 mid = {(from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f};
                    ImVec2 text_sz = ImGui::CalcTextSize(net_id.c_str());
                    float tw = text_sz.x * (font_size / ImGui::GetFontSize());
                    float tth = text_sz.y * (font_size / ImGui::GetFontSize());
                    float cx = mid.x - tw * 0.5f;
                    float cy = mid.y - tth * 0.5f;
                    dl->AddRectFilled({cx - 3, cy - 1}, {cx + tw + 3, cy + tth + 1},
                                      S.col_label_bg, S.node_rounding);
                    dl->AddText(nullptr, font_size, {cx, cy}, S.col_label_text, net_id.c_str());
                }
            }
        };

        // Draw wires using PinMapping for correct pin positions
        auto dst_pm = PinMapping::build(dst_node, dst_nt);
        for (int i = 0; i < dst_pm.total(); i++) {
            if (dst_pm.is_add_diamond(i)) continue;
            if (dst_pm.is_absent_optional(i)) continue;
            if (dst_pm.is_base(i)) {
                int port = dst_pm.port_index(i);
                if (dst_node->parsed_args && port < (int)dst_node->parsed_args->size()) {
                    if (auto* an = std::get_if<ArgNet2>(&(*dst_node->parsed_args)[port]))
                        draw_wire_to_pin(i, an->second, an->first);
                }
            } else if (dst_pm.is_va(i)) {
                int va_idx = -(dst_pm.port_index(i) + 1);
                if (dst_node->parsed_va_args && va_idx < (int)dst_node->parsed_va_args->size()) {
                    if (auto* an = std::get_if<ArgNet2>(&(*dst_node->parsed_va_args)[va_idx]))
                        draw_wire_to_pin(i, an->second, an->first);
                }
            } else if (dst_pm.is_remap(i)) {
                int ri = dst_pm.remap_index(i);
                if (ri < (int)dst_node->remaps.size())
                    draw_wire_to_pin(i, dst_node->remaps[ri].second, dst_node->remaps[ri].first);
            }
        }
    }

    dl->PopClipRect();

    // ─── Node dragging with left mouse ───
    if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        // Hit test: find node under mouse
        ImVec2 mouse = ImGui::GetIO().MousePos;
        dragging_node_.clear();
        // Iterate in reverse so topmost (last drawn) is hit first
        for (auto it = gb_->entries.rbegin(); it != gb_->entries.rend(); ++it) {
            auto node = it->second->as_Node();
            if (!node) continue;
            auto layout = compute_node_layout(node, canvas_origin, canvas_zoom_);
            if (mouse.x >= layout.pos.x && mouse.x <= layout.pos.x + layout.width &&
                mouse.y >= layout.pos.y && mouse.y <= layout.pos.y + layout.height) {
                dragging_node_ = it->first;
                selected_nodes_.clear();
                selected_nodes_.insert(dragging_node_);
                dragging_started_ = true;
                // Check if already overlapping at drag start
                drag_was_overlapping_ = false;
                {
                    auto dl2 = compute_node_layout(node, {0,0}, 1.0f);
                    float pad = S.node_height * 0.5f;
                    for (auto& [oid, oe] : gb_->entries) {
                        if (oid == dragging_node_) continue;
                        auto on = oe->as_Node();
                        if (!on) continue;
                        auto ol = compute_node_layout(on, {0,0}, 1.0f);
                        if (node->position.x < on->position.x - pad + ol.width + pad * 2 &&
                            node->position.x + dl2.width > on->position.x - pad &&
                            node->position.y < on->position.y - pad + ol.height + pad * 2 &&
                            node->position.y + dl2.height > on->position.y - pad) {
                            drag_was_overlapping_ = true;
                            break;
                        }
                    }
                }
                break;
            }
        }
        if (dragging_node_.empty()) {
            selected_nodes_.clear();
        }
    }
    if (dragging_started_ && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !dragging_node_.empty()) {
        auto it = gb_->entries.find(dragging_node_);
        auto drag_node = (it != gb_->entries.end()) ? it->second->as_Node() : nullptr;
        if (drag_node) {
            ImVec2 delta = ImGui::GetIO().MouseDelta;
            float new_x = drag_node->position.x + delta.x / canvas_zoom_;
            float new_y = drag_node->position.y + delta.y / canvas_zoom_;

            // Compute proposed layout
            auto proposed = compute_node_layout(drag_node, {0,0}, 1.0f);
            float pw = proposed.width;
            float ph = proposed.height;

            // Check overlap with all other nodes (0.5em padding)
            // If the node was already overlapping at drag start, allow free movement
            bool blocked = false;
            if (!drag_was_overlapping_) {
                float pad = S.node_height * 0.5f;
                for (auto& [other_id, other_entry] : gb_->entries) {
                    if (other_id == dragging_node_) continue;
                    auto other_node = other_entry->as_Node();
                    if (!other_node) continue;
                    auto other_layout = compute_node_layout(other_node, {0,0}, 1.0f);
                    float ox = other_node->position.x - pad, oy = other_node->position.y - pad;
                    float ow = other_layout.width + pad * 2, oh = other_layout.height + pad * 2;
                    if (new_x < ox + ow && new_x + pw > ox &&
                        new_y < oy + oh && new_y + ph > oy) {
                        blocked = true;
                        break;
                    }
                }
            }
            if (!blocked) {
                drag_node->position.x = new_x;
                drag_node->position.y = new_y;
            }
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        dragging_node_.clear();
        dragging_started_ = false;
    }

    // Pan with middle mouse or right mouse
    if (canvas_hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        canvas_offset_.x += ImGui::GetIO().MouseDelta.x;
        canvas_offset_.y += ImGui::GetIO().MouseDelta.y;
    }
    if (canvas_hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
        canvas_offset_.x += ImGui::GetIO().MouseDelta.x;
        canvas_offset_.y += ImGui::GetIO().MouseDelta.y;
    }

    // Zoom with scroll
    if (canvas_hovered) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0) {
            float old_zoom = canvas_zoom_;
            canvas_zoom_ *= (wheel > 0) ? 1.1f : 0.9f;
            canvas_zoom_ = std::clamp(canvas_zoom_, 0.1f, 10.0f);
            // Zoom toward mouse position
            ImVec2 mouse = ImGui::GetIO().MousePos;
            ImVec2 mouse_rel = v2sub(v2sub(mouse, canvas_p0), canvas_offset_);
            ImVec2 mouse_canvas = v2mul(mouse_rel, 1.0f / old_zoom);
            canvas_offset_ = v2sub(v2sub(mouse, canvas_p0), v2mul(mouse_canvas, canvas_zoom_));
        }
    }
}

// ─── Draw a node ───

void Editor2Pane::draw_node(ImDrawList* dl, const FlowNodeBuilderPtr& node,
                             ImVec2 canvas_origin) {
    auto* nt = find_node_type2(node->type_id);
    if (!nt) return;

    auto layout = compute_node_layout(node, canvas_origin, canvas_zoom_);

    // Special nodes: label and error
    if (nt->is_special()) {
        // Display first arg without quotes
        std::string display;
        if (node->parsed_args && !node->parsed_args->empty()) {
            auto& a = (*node->parsed_args)[0];
            if (auto* s = std::get_if<ArgString2>(&a)) display = s->value;
            else if (auto* e = std::get_if<ArgExpr2>(&a)) display = e->expr;
            else display = node->args_str();
        }

        float font_size = ImGui::GetFontSize() * canvas_zoom_;
        bool is_error = (node->type_id == NodeTypeID::Error);

        if (is_error) {
            // Error: red box
            dl->AddRectFilled(layout.pos, {layout.pos.x + layout.width, layout.pos.y + layout.height},
                              S.col_node_err, S.node_rounding * canvas_zoom_);
            dl->AddRect(layout.pos, {layout.pos.x + layout.width, layout.pos.y + layout.height},
                        S.col_err_border, S.node_rounding * canvas_zoom_);
        }
        // Label: no box at all

        if (font_size > 5.0f) {
            ImVec2 text_sz = ImGui::CalcTextSize(display.c_str());
            float tw = text_sz.x * canvas_zoom_;
            float cx = layout.pos.x + (layout.width - tw) * 0.5f;
            float cy = layout.pos.y + (layout.height - font_size) * 0.5f;
            dl->AddText(nullptr, font_size, {cx, cy}, S.col_text, display.c_str());
        }
        return; // no pins for special nodes
    }

    // Display text
    std::string display = nt->name;
    std::string args = node->args_str();
    if (!args.empty()) display += " " + args;

    bool selected = selected_nodes_.count(node->id);
    bool has_error = !node->error.empty();

    ImU32 col = has_error ? S.col_node_err : (selected ? S.col_node_sel : S.col_node);
    dl->AddRectFilled(layout.pos, {layout.pos.x + layout.width, layout.pos.y + layout.height},
                      col, S.node_rounding * canvas_zoom_);
    dl->AddRect(layout.pos, {layout.pos.x + layout.width, layout.pos.y + layout.height},
                S.col_node_border, S.node_rounding * canvas_zoom_);

    // Text
    float font_size = ImGui::GetFontSize() * canvas_zoom_;
    if (font_size > 5.0f) {
        ImVec2 text_sz = ImGui::CalcTextSize(display.c_str());
        float tw = text_sz.x * canvas_zoom_;
        float cx = layout.pos.x + (layout.width - tw) * 0.5f;
        float cy = layout.pos.y + (layout.height - font_size) * 0.5f;
        dl->AddText(nullptr, font_size, {cx, cy}, S.col_text, display.c_str());
    }

    // Draw input pins (top) using PinMapping for correct port-to-pin association
    float pr = S.pin_radius * canvas_zoom_;
    auto pm = PinMapping::build(node, nt);
    for (int i = 0; i < layout.num_in; i++) {
        ImVec2 pp = layout.input_pin_pos(i);

        // +diamond slot
        if (pm.is_add_diamond(i)) {
            ImU32 pc = S.col_add_pin;
            dl->AddQuadFilled({pp.x, pp.y - pr}, {pp.x + pr, pp.y}, {pp.x, pp.y + pr}, {pp.x - pr, pp.y}, pc);
            float cr = pr * 0.5f;
            float lth = S.add_pin_line * canvas_zoom_;
            dl->AddLine({pp.x - cr, pp.y}, {pp.x + cr, pp.y}, S.col_add_pin_fg, lth);
            dl->AddLine({pp.x, pp.y - cr}, {pp.x, pp.y + cr}, S.col_add_pin_fg, lth);
            continue;
        }

        PortKind2 kind = PortKind2::Data;
        bool is_va = pm.is_va(i);
        bool is_optional = false;

        if (pm.is_absent_optional(i)) {
            int port = pm.absent_port_index(i);
            if (auto* pd = nt->input_port(port)) kind = pd->kind;
            is_optional = true;
        } else if (pm.is_base(i)) {
            int port = pm.port_index(i);
            if (auto* pd = nt->input_port(port)) {
                kind = pd->kind;
                is_optional = pd->optional;
            }
        } else if (is_va) {
            kind = nt->va_args ? nt->va_args->kind : PortKind2::Data;
        }
        // remaps are always Data

        ImU32 pc = pin_color(kind);
        if (kind == PortKind2::BangTrigger) {
            dl->AddRectFilled({pp.x - pr, pp.y - pr}, {pp.x + pr, pp.y + pr}, pc);
        } else if (kind == PortKind2::Lambda) {
            dl->AddTriangleFilled({pp.x - pr, pp.y - pr}, {pp.x + pr, pp.y - pr}, {pp.x, pp.y + pr}, pc);
        } else if (is_va) {
            dl->AddQuadFilled({pp.x, pp.y - pr}, {pp.x + pr, pp.y}, {pp.x, pp.y + pr}, {pp.x - pr, pp.y}, pc);
        } else if (is_optional) {
            dl->AddQuadFilled({pp.x, pp.y - pr}, {pp.x + pr, pp.y}, {pp.x, pp.y + pr}, {pp.x - pr, pp.y}, pc);
            float font_sz = pr * 1.6f;
            if (font_sz > 3.0f) {
                ImVec2 ts = ImGui::CalcTextSize("?");
                float scale = font_sz / ImGui::GetFontSize();
                dl->AddText(nullptr, font_sz,
                    {pp.x - ts.x * scale * 0.5f, pp.y - ts.y * scale * 0.5f},
                    S.col_opt_pin_fg, "?");
            }
        } else {
            dl->AddCircleFilled(pp, pr, pc);
        }
    }

    // Draw output pins (bottom)
    for (int i = 0; i < layout.num_out; i++) {
        ImVec2 pp = layout.output_pin_pos(i);
        PortKind2 kind = PortKind2::Data;
        if (nt->output_ports && i < nt->num_outputs) kind = nt->output_ports[i].kind;

        ImU32 pc = pin_color(kind);
        if (kind == PortKind2::BangNext) {
            dl->AddRectFilled({pp.x - pr, pp.y - pr}, {pp.x + pr, pp.y + pr}, pc);
        } else {
            dl->AddCircleFilled(pp, pr, pc);
        }
    }

    // Flow-only: lambda grab (left) and side-bang (right)
    if (nt->is_flow()) {
        // Lambda grab handle (left-pointing triangle, middle-left)
        ImVec2 gp = layout.lambda_grab_pos();
        ImU32 lc = S.col_pin_lambda;
        dl->AddTriangleFilled(
            {gp.x + pr, gp.y - pr},
            {gp.x - pr, gp.y},
            {gp.x + pr, gp.y + pr},
            lc);

        // Side-bang (square, middle-right)
        ImVec2 bp = {layout.pos.x + layout.width, layout.pos.y + layout.height * 0.5f};
        dl->AddRectFilled({bp.x - pr, bp.y - pr}, {bp.x + pr, bp.y + pr}, S.col_pin_bang);
    }

    // Pin hover: highlight + tooltip
    ImVec2 mouse = ImGui::GetMousePos();
    float hit_r = pr * S.pin_hit_radius_mul;
    auto dist2 = [](ImVec2 a, ImVec2 b) { return (a.x-b.x)*(a.x-b.x) + (a.y-b.y)*(a.y-b.y); };
    float hit_r2 = hit_r * hit_r;
    float ho = S.highlight_offset * canvas_zoom_;
    ImU32 COL_HOVER = S.col_pin_hover;

    // Helper to get input pin name using PinMapping
    auto get_input_pin_name = [&](int i) -> const char* {
        if (pm.is_absent_optional(i)) {
            int port = pm.absent_port_index(i);
            if (auto* pd = nt->input_port(port)) return pd->name;
        } else if (pm.is_base(i)) {
            int port = pm.port_index(i);
            if (auto* pd = nt->input_port(port)) return pd->name;
        } else if (pm.is_va(i)) {
            return nt->va_args ? nt->va_args->name : "va";
        } else if (pm.is_remap(i)) {
            static char remap_buf[16];
            snprintf(remap_buf, sizeof(remap_buf), "$%d", pm.remap_index(i));
            return remap_buf;
        }
        return "?";
    };

    // Helper to get input pin shape for highlight
    enum class PinShape2 { Circle, Square, Diamond, TriangleDown, TriangleLeft };
    auto get_input_pin_shape = [&](int i) -> PinShape2 {
        if (pm.is_add_diamond(i)) return PinShape2::Diamond;
        if (pm.is_absent_optional(i)) return PinShape2::Diamond;
        if (pm.is_base(i)) {
            int port = pm.port_index(i);
            if (auto* pd = nt->input_port(port)) {
                if (pd->kind == PortKind2::BangTrigger) return PinShape2::Square;
                if (pd->kind == PortKind2::Lambda) return PinShape2::TriangleDown;
                if (pd->optional) return PinShape2::Diamond;
            }
        } else if (pm.is_va(i)) {
            return PinShape2::Diamond;
        }
        return PinShape2::Circle;
    };

    float ht = S.highlight_thickness;
    auto draw_highlight = [&](ImVec2 pos, PinShape2 shape) {
        switch (shape) {
        case PinShape2::Circle:
            dl->AddCircle(pos, pr + ho, COL_HOVER, 0, ht);
            break;
        case PinShape2::Square:
            dl->AddRect({pos.x - pr - ho, pos.y - pr - ho}, {pos.x + pr + ho, pos.y + pr + ho}, COL_HOVER, 0, 0, ht);
            break;
        case PinShape2::Diamond:
            dl->AddQuad({pos.x, pos.y - pr - ho}, {pos.x + pr + ho, pos.y}, {pos.x, pos.y + pr + ho}, {pos.x - pr - ho, pos.y}, COL_HOVER, ht);
            break;
        case PinShape2::TriangleDown:
            dl->AddTriangle({pos.x - pr - ho, pos.y - pr - ho}, {pos.x + pr + ho, pos.y - pr - ho}, {pos.x, pos.y + pr + ho}, COL_HOVER, ht);
            break;
        case PinShape2::TriangleLeft:
            dl->AddTriangle({pos.x + pr + ho, pos.y - pr - ho}, {pos.x - pr - ho, pos.y}, {pos.x + pr + ho, pos.y + pr + ho}, COL_HOVER, ht);
            break;
        }
    };

    bool pin_hovered = false;
    // Check input pins
    for (int i = 0; i < layout.num_in; i++) {
        ImVec2 pp = layout.input_pin_pos(i);
        if (dist2(mouse, pp) < hit_r2) {
            draw_highlight(pp, pm.is_add_diamond(i) ? PinShape2::Diamond : get_input_pin_shape(i));
            ImGui::BeginTooltip();
            ImGui::SetWindowFontScale(S.tooltip_scale);
            if (pm.is_add_diamond(i)) {
                const char* va_name = nt->va_args ? nt->va_args->name : "arg";
                ImGui::Text("add %s", va_name);
            } else {
                ImGui::Text("%s", get_input_pin_name(i));
            }
            ImGui::EndTooltip();
            pin_hovered = true;
            break;
        }
    }
    // Check output pins
    if (!pin_hovered) {
        for (int i = 0; i < layout.num_out; i++) {
            ImVec2 pp = layout.output_pin_pos(i);
            if (dist2(mouse, pp) < hit_r2) {
                PortKind2 kind = (nt->output_ports && i < nt->num_outputs) ? nt->output_ports[i].kind : PortKind2::Data;
                draw_highlight(pp, kind == PortKind2::BangNext ? PinShape2::Square : PinShape2::Circle);
                const char* name = (nt->output_ports && i < nt->num_outputs) ? nt->output_ports[i].name : "out";
                ImGui::BeginTooltip();
                ImGui::SetWindowFontScale(S.tooltip_scale);
                ImGui::Text("%s", name);
                ImGui::EndTooltip();
                pin_hovered = true;
                break;
            }
        }
    }
    // Check lambda grab
    if (!pin_hovered && nt->is_flow()) {
        ImVec2 gp = layout.lambda_grab_pos();
        if (dist2(mouse, gp) < hit_r2) {
            draw_highlight(gp, PinShape2::TriangleLeft);
            ImGui::BeginTooltip();
            ImGui::SetWindowFontScale(S.tooltip_scale);
            ImGui::Text("as_lambda");
            ImGui::EndTooltip();
            pin_hovered = true;
        }
    }
    // Check side-bang
    if (!pin_hovered && nt->is_flow()) {
        ImVec2 bp = {layout.pos.x + layout.width, layout.pos.y + layout.height * 0.5f};
        if (dist2(mouse, bp) < hit_r2) {
            draw_highlight(bp, PinShape2::Square);
            ImGui::BeginTooltip();
            ImGui::SetWindowFontScale(S.tooltip_scale);
            ImGui::Text("post_bang");
            ImGui::EndTooltip();
            pin_hovered = true;
        }
    }

    // Node tooltip (when hovering body, not a pin)
    if (!pin_hovered &&
        mouse.x >= layout.pos.x && mouse.x <= layout.pos.x + layout.width &&
        mouse.y >= layout.pos.y && mouse.y <= layout.pos.y + layout.height) {
        ImGui::BeginTooltip();
        ImGui::SetWindowFontScale(S.tooltip_scale);
        ImGui::Text("id: %s", node->id.c_str());
        if (node->parsed_args) {
            ImGui::Text("parsed_args (%d):", (int)node->parsed_args->size());
            for (int i = 0; i < (int)node->parsed_args->size(); i++) {
                auto& a = (*node->parsed_args)[i];
                if (auto* n = std::get_if<ArgNet2>(&a))
                    ImGui::Text("  [%d] net: %s", i, n->first.c_str());
                else if (auto* e = std::get_if<ArgExpr2>(&a))
                    ImGui::Text("  [%d] expr: %s", i, e->expr.c_str());
                else if (auto* s = std::get_if<ArgString2>(&a))
                    ImGui::Text("  [%d] str: %s", i, s->value.c_str());
                else if (auto* v = std::get_if<ArgNumber2>(&a))
                    ImGui::Text("  [%d] num: %g", i, v->value);
            }
        }
        if (node->parsed_va_args && !node->parsed_va_args->empty()) {
            ImGui::Text("parsed_va_args (%d):", (int)node->parsed_va_args->size());
            for (int i = 0; i < (int)node->parsed_va_args->size(); i++) {
                auto& a = (*node->parsed_va_args)[i];
                if (auto* n = std::get_if<ArgNet2>(&a))
                    ImGui::Text("  [%d] net: %s", i, n->first.c_str());
                else if (auto* e = std::get_if<ArgExpr2>(&a))
                    ImGui::Text("  [%d] expr: %s", i, e->expr.c_str());
                else if (auto* s = std::get_if<ArgString2>(&a))
                    ImGui::Text("  [%d] str: %s", i, s->value.c_str());
                else if (auto* v = std::get_if<ArgNumber2>(&a))
                    ImGui::Text("  [%d] num: %g", i, v->value);
            }
        }
        if (!node->remaps.empty()) {
            ImGui::Text("remaps (%d):", (int)node->remaps.size());
            for (int i = 0; i < (int)node->remaps.size(); i++)
                ImGui::Text("  $%d -> %s", i, node->remaps[i].first.c_str());
        }
        ImGui::EndTooltip();
    }
}

void Editor2Pane::draw_net(ImDrawList*, const NetBuilderPtr&, ImVec2) {
    // Unused — wires drawn per-node in draw()
}
