#include "editor2.h"
#include "atto/graphbuilder.h"
#include "atto/node_types2.h"
#include "imgui.h"
#include <cmath>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <stdexcept>

// ─── Constants ───

static constexpr float NODE_MIN_WIDTH = 80.0f;
static constexpr float NODE_HEIGHT = 40.0f;
static constexpr float PIN_RADIUS = 5.0f;
static constexpr float PIN_SPACING = 16.0f;

static constexpr ImU32 COL_BG        = IM_COL32(30, 30, 40, 255);
static constexpr ImU32 COL_GRID      = IM_COL32(50, 50, 60, 255);
static constexpr ImU32 COL_NODE      = IM_COL32(50, 55, 75, 220);
static constexpr ImU32 COL_NODE_SEL  = IM_COL32(80, 90, 130, 255);
static constexpr ImU32 COL_NODE_ERR  = IM_COL32(130, 40, 40, 220);
static constexpr ImU32 COL_TEXT      = IM_COL32(220, 220, 220, 255);
static constexpr ImU32 COL_PIN_DATA  = IM_COL32(100, 200, 100, 255);
static constexpr ImU32 COL_PIN_BANG  = IM_COL32(255, 200, 80, 255);
static constexpr ImU32 COL_PIN_LAMBDA= IM_COL32(180, 130, 255, 255);
static constexpr ImU32 COL_LINK      = IM_COL32(200, 200, 100, 200);

// ─── Helpers ───

static inline ImVec2 v2add(ImVec2 a, ImVec2 b) { return {a.x + b.x, a.y + b.y}; }
static inline ImVec2 v2sub(ImVec2 a, ImVec2 b) { return {a.x - b.x, a.y - b.y}; }
static inline ImVec2 v2mul(ImVec2 a, float s) { return {a.x * s, a.y * s}; }

static ImU32 pin_color(PortKind2 kind) {
    switch (kind) {
    case PortKind2::BangTrigger:
    case PortKind2::BangNext: return COL_PIN_BANG;
    case PortKind2::Lambda:   return COL_PIN_LAMBDA;
    default:                  return COL_PIN_DATA;
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
    dl->AddRectFilled(canvas_p0, v2add(canvas_p0, canvas_sz), COL_BG);

    ImVec2 canvas_origin = v2add(canvas_p0, canvas_offset_);

    // Grid
    float grid_step = 20.0f * canvas_zoom_;
    if (grid_step > 5.0f) {
        for (float x = fmodf(canvas_offset_.x, grid_step); x < canvas_sz.x; x += grid_step)
            dl->AddLine({canvas_p0.x + x, canvas_p0.y}, {canvas_p0.x + x, canvas_p0.y + canvas_sz.y}, COL_GRID);
        for (float y = fmodf(canvas_offset_.y, grid_step); y < canvas_sz.y; y += grid_step)
            dl->AddLine({canvas_p0.x, canvas_p0.y + y}, {canvas_p0.x + canvas_sz.x, canvas_p0.y + y}, COL_GRID);
    }

    // Clip
    dl->PushClipRect(canvas_p0, v2add(canvas_p0, canvas_sz), true);

    // Draw nodes (skip shadows)
    for (auto& [id, entry] : gb_->entries) {
        if (std::holds_alternative<FlowNodeBuilder>(*entry)) {
            auto& node = std::get<FlowNodeBuilder>(*entry);
            if (node.shadow) throw std::logic_error("Editor2Pane: shadow nodes must be folded before rendering (id: " + id + ")");
            draw_node(dl, id, node, canvas_origin);
        }
    }

    // Draw nets (wires)
    // TODO: draw connections between nodes via nets

    dl->PopClipRect();

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

void Editor2Pane::draw_node(ImDrawList* dl, const NodeId& id, const FlowNodeBuilder& node,
                             ImVec2 canvas_origin) {
    auto* nt = find_node_type2(node.type_id);
    if (!nt) return;

    // Compute display text
    std::string display = nt->name;
    std::string args = node.args_str();
    if (!args.empty()) display += " " + args;

    // Node rect
    float font_size = ImGui::GetFontSize() * canvas_zoom_;
    ImVec2 text_sz = ImGui::CalcTextSize(display.c_str());
    float text_w = text_sz.x * canvas_zoom_ + 16.0f * canvas_zoom_;

    // Pin counts
    int num_in = nt->num_inputs + (node.parsed_args ? node.parsed_args->rewrite_input_count : 0);
    int num_out = nt->num_outputs;
    if (node.type_id == NodeTypeID::Expr || node.type_id == NodeTypeID::ExprBang) {
        int args_count = node.parsed_args ? (int)node.parsed_args->size() : 0;
        if (node.type_id == NodeTypeID::ExprBang) num_out = 1 + std::max(1, args_count); // next + outputs
        else num_out = std::max(1, args_count);
    }

    float pin_w_top = std::max(0, num_in) * PIN_SPACING * canvas_zoom_;
    float pin_w_bot = std::max(0, num_out) * PIN_SPACING * canvas_zoom_;
    float node_w = std::max({NODE_MIN_WIDTH * canvas_zoom_, text_w, pin_w_top, pin_w_bot});
    float node_h = NODE_HEIGHT * canvas_zoom_;

    ImVec2 pos = {canvas_origin.x + node.position.x * canvas_zoom_,
                  canvas_origin.y + node.position.y * canvas_zoom_};

    bool selected = selected_nodes_.count(id);
    bool has_error = !node.error.empty();

    ImU32 col = has_error ? COL_NODE_ERR : (selected ? COL_NODE_SEL : COL_NODE);
    dl->AddRectFilled(pos, {pos.x + node_w, pos.y + node_h}, col, 4.0f * canvas_zoom_);
    dl->AddRect(pos, {pos.x + node_w, pos.y + node_h}, IM_COL32(80, 80, 100, 255), 4.0f * canvas_zoom_);

    // Text
    if (font_size > 5.0f) {
        float tw = text_sz.x * canvas_zoom_;
        float cx = pos.x + (node_w - tw) * 0.5f;
        float cy = pos.y + (node_h - font_size) * 0.5f;
        dl->AddText(nullptr, font_size, {cx, cy}, COL_TEXT, display.c_str());
    }

    // Draw input pins (top)
    float pr = PIN_RADIUS * canvas_zoom_;
    for (int i = 0; i < num_in; i++) {
        float px = pos.x + (i + 0.5f) * PIN_SPACING * canvas_zoom_;
        float py = pos.y;

        PortKind2 kind = PortKind2::Data;
        if (nt->input_ports && i < nt->num_inputs) kind = nt->input_ports[i].kind;

        ImU32 pc = pin_color(kind);
        if (kind == PortKind2::BangTrigger) {
            dl->AddRectFilled({px - pr, py - pr}, {px + pr, py + pr}, pc);
        } else if (kind == PortKind2::Lambda) {
            // Triangle for lambda
            dl->AddTriangleFilled({px - pr, py - pr}, {px + pr, py}, {px - pr, py + pr}, pc);
        } else {
            dl->AddCircleFilled({px, py}, pr, pc);
        }
    }

    // Draw output pins (bottom)
    for (int i = 0; i < num_out; i++) {
        float px = pos.x + (i + 0.5f) * PIN_SPACING * canvas_zoom_;
        float py = pos.y + node_h;

        PortKind2 kind = PortKind2::Data;
        if (nt->output_ports && i < nt->num_outputs) kind = nt->output_ports[i].kind;

        ImU32 pc = pin_color(kind);
        if (kind == PortKind2::BangNext) {
            dl->AddRectFilled({px - pr, py - pr}, {px + pr, py + pr}, pc);
        } else {
            dl->AddCircleFilled({px, py}, pr, pc);
        }
    }
}

void Editor2Pane::draw_net(ImDrawList* dl, const NodeId& id, const NetBuilder& net,
                            ImVec2 canvas_origin) {
    // TODO: draw wires between connected nodes
}
