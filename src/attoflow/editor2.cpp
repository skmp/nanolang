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

// Computed node layout for drawing
struct NodeLayout {
    ImVec2 pos;      // top-left screen position
    float width;
    float height;
    int num_in;
    int num_out;
    float zoom;

    ImVec2 input_pin_pos(int i) const {
        return {pos.x + (i + 0.5f) * PIN_SPACING * zoom, pos.y};
    }
    ImVec2 output_pin_pos(int i) const {
        return {pos.x + (i + 0.5f) * PIN_SPACING * zoom, pos.y + height};
    }
};

static NodeLayout compute_node_layout(const FlowNodeBuilder& node, ImVec2 canvas_origin, float zoom) {
    auto* nt = find_node_type2(node.type_id);
    std::string display = nt ? nt->name : "?";
    std::string args = node.args_str();
    if (!args.empty()) display += " " + args;

    float font_size = ImGui::GetFontSize() * zoom;
    ImVec2 text_sz = ImGui::CalcTextSize(display.c_str());
    float text_w = text_sz.x * zoom + 16.0f * zoom;

    auto count_net_args = [](const ParsedArgs2* pa) -> int {
        if (!pa) return 0;
        int n = 0;
        for (auto& a : *pa) if (std::holds_alternative<ArgNet2>(a)) n++;
        return n;
    };
    int num_in = count_net_args(node.parsed_args.get())
               + count_net_args(node.parsed_va_args.get())
               + (int)node.remaps.size();
    int num_out = nt ? nt->num_outputs : 1;
    if (node.type_id == NodeTypeID::Expr || node.type_id == NodeTypeID::ExprBang) {
        int args_count = node.parsed_args ? (int)node.parsed_args->size() : 0;
        if (node.type_id == NodeTypeID::ExprBang) num_out = 1 + std::max(1, args_count);
        else num_out = std::max(1, args_count);
    }

    float pin_w_top = std::max(0, num_in) * PIN_SPACING * zoom;
    float pin_w_bot = std::max(0, num_out) * PIN_SPACING * zoom;
    float node_w = std::max({NODE_MIN_WIDTH * zoom, text_w, pin_w_top, pin_w_bot});
    float node_h = NODE_HEIGHT * zoom;

    ImVec2 pos = {canvas_origin.x + node.position.x * zoom,
                  canvas_origin.y + node.position.y * zoom};

    return {pos, node_w, node_h, num_in, num_out, zoom};
}

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

    // Draw wires by iterating each node's inputs/outputs/remaps
    for (auto& [dst_id, dst_entry] : gb_->entries) {
        if (!std::holds_alternative<FlowNodeBuilder>(*dst_entry)) continue;
        auto& dst_node = std::get<FlowNodeBuilder>(*dst_entry);
        auto* dst_nt = find_node_type2(dst_node.type_id);
        if (!dst_nt) continue;
        auto dst_layout = compute_node_layout(dst_node, canvas_origin, canvas_zoom_);

        // Helper: draw wire from a source net to destination pin at index dst_pin
        auto draw_wire_to_pin = [&](int dst_pin, const BuilderEntryPtr& net_entry, const NodeId& net_id) {
            if (!net_entry || !std::holds_alternative<NetBuilder>(*net_entry)) return;
            auto& net = std::get<NetBuilder>(*net_entry);
            if (net.is_the_unconnected) return;

            auto src_ptr = net.source.lock();
            if (!src_ptr || !std::holds_alternative<FlowNodeBuilder>(*src_ptr)) return;
            auto& src_node = std::get<FlowNodeBuilder>(*src_ptr);
            auto src_layout = compute_node_layout(src_node, canvas_origin, canvas_zoom_);

            // Find which output pin index on source this net comes from
            // For now, use pin 0 as default — TODO: track output pin index per net
            ImVec2 from = src_layout.output_pin_pos(0);
            ImVec2 to = dst_layout.input_pin_pos(dst_pin);

            bool named = !net.auto_wire;
            ImU32 col = named ? IM_COL32(200, 200, 100, 120) : COL_LINK;
            float dy = std::max(std::abs(to.y - from.y) * 0.5f, 30.0f * canvas_zoom_);
            float th = 2.5f * canvas_zoom_;
            dl->AddBezierCubic(from, {from.x, from.y + dy}, {to.x, to.y - dy}, to, col, th);

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
                                      IM_COL32(30, 30, 40, 200), 3.0f);
                    dl->AddText(nullptr, font_size, {cx, cy}, IM_COL32(180, 220, 255, 255), net_id.c_str());
                }
            }
        };

        // Helper: iterate ArgNet2 entries and draw wires, returns pin count used
        auto draw_wires_from_args = [&](const ParsedArgs2* pa, int pin_start) -> int {
            if (!pa) return 0;
            int pin = pin_start;
            for (auto& a : *pa) {
                if (auto* an = std::get_if<ArgNet2>(&a)) {
                    draw_wire_to_pin(pin++, an->second, an->first);
                }
            }
            return pin - pin_start;
        };

        // Base args → va_args → remaps
        int pin = 0;
        pin += draw_wires_from_args(dst_node.parsed_args.get(), pin);
        pin += draw_wires_from_args(dst_node.parsed_va_args.get(), pin);

        // Remaps: $N pins (appended after base + va_args)
        for (int i = 0; i < (int)dst_node.remaps.size(); i++) {
            auto& remap = dst_node.remaps[i];
            draw_wire_to_pin(pin + i, remap.second, remap.first);
        }
    }

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

    auto layout = compute_node_layout(node, canvas_origin, canvas_zoom_);

    // Display text
    std::string display = nt->name;
    std::string args = node.args_str();
    if (!args.empty()) display += " " + args;

    bool selected = selected_nodes_.count(id);
    bool has_error = !node.error.empty();

    ImU32 col = has_error ? COL_NODE_ERR : (selected ? COL_NODE_SEL : COL_NODE);
    dl->AddRectFilled(layout.pos, {layout.pos.x + layout.width, layout.pos.y + layout.height},
                      col, 4.0f * canvas_zoom_);
    dl->AddRect(layout.pos, {layout.pos.x + layout.width, layout.pos.y + layout.height},
                IM_COL32(80, 80, 100, 255), 4.0f * canvas_zoom_);

    // Text
    float font_size = ImGui::GetFontSize() * canvas_zoom_;
    if (font_size > 5.0f) {
        ImVec2 text_sz = ImGui::CalcTextSize(display.c_str());
        float tw = text_sz.x * canvas_zoom_;
        float cx = layout.pos.x + (layout.width - tw) * 0.5f;
        float cy = layout.pos.y + (layout.height - font_size) * 0.5f;
        dl->AddText(nullptr, font_size, {cx, cy}, COL_TEXT, display.c_str());
    }

    // Draw input pins (top)
    // Pin order: parsed_args ArgNet2, then va_args ArgNet2, then remaps
    float pr = PIN_RADIUS * canvas_zoom_;
    auto count_net_args_in = [](const ParsedArgs2* pa) -> int {
        if (!pa) return 0;
        int n = 0;
        for (auto& a : *pa) if (std::holds_alternative<ArgNet2>(a)) n++;
        return n;
    };
    int base_pin_count = count_net_args_in(node.parsed_args.get());
    int va_pin_count = count_net_args_in(node.parsed_va_args.get());
    for (int i = 0; i < layout.num_in; i++) {
        ImVec2 pp = layout.input_pin_pos(i);
        PortKind2 kind = PortKind2::Data;
        if (i < base_pin_count) {
            // Map visible pin index to descriptor port (skip bang triggers since they're not in parsed_args)
            if (nt->input_ports) {
                int vis = 0;
                for (int p = 0; p < nt->num_inputs; p++) {
                    if (nt->input_ports[p].kind != PortKind2::BangTrigger) {
                        if (vis == i) { kind = nt->input_ports[p].kind; break; }
                        vis++;
                    }
                }
            }
        } else if (i < base_pin_count + va_pin_count) {
            kind = nt->va_args ? nt->va_args->kind : PortKind2::Data;
        }

        ImU32 pc = pin_color(kind);
        if (kind == PortKind2::BangTrigger) {
            dl->AddRectFilled({pp.x - pr, pp.y - pr}, {pp.x + pr, pp.y + pr}, pc);
        } else if (kind == PortKind2::Lambda) {
            dl->AddTriangleFilled({pp.x - pr, pp.y - pr}, {pp.x + pr, pp.y}, {pp.x - pr, pp.y + pr}, pc);
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
}

void Editor2Pane::draw_net(ImDrawList*, const NodeId&, const NetBuilder&, ImVec2) {
    // Unused — wires drawn per-node in draw()
}
