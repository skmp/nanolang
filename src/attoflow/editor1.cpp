#include "editor1.h"
#include "atto/args.h"
#include "atto/expr.h"
#include "atto/inference.h"
#include "atto/serial.h"
#include "atto/shadow.h"
#include "atto/types.h"
#include "atto/node_types.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <functional>
#include <filesystem>
#include <fstream>

// --- Constants ---
static constexpr float NODE_ROUNDING = 4.0f;
static constexpr float PIN_RADIUS = 5.0f;
static constexpr float PIN_SPACING = 20.0f;
static constexpr float NODE_HEIGHT = 31.0f;
static constexpr float NODE_MIN_WIDTH = 80.0f;
static constexpr float GRID_SIZE = 32.0f;

static constexpr ImU32 COL_BG         = IM_COL32(30, 30, 40, 255);
static constexpr ImU32 COL_GRID       = IM_COL32(50, 50, 60, 255);
static constexpr ImU32 COL_NODE_BG    = IM_COL32(60, 60, 90, 230);
static constexpr ImU32 COL_PIN_IN     = IM_COL32(100, 200, 100, 255);
static constexpr ImU32 COL_PIN_OUT    = IM_COL32(200, 100, 100, 255);
static constexpr ImU32 COL_PIN_HOVER  = IM_COL32(255, 255, 255, 255);
static constexpr ImU32 COL_LINK       = IM_COL32(200, 200, 100, 200);
static constexpr ImU32 COL_LINK_DRAG  = IM_COL32(255, 255, 150, 200);

// --- Static helpers ---

#include "atto/type_utils.h"

// Look up port description for a pin on a node.
// Returns {port_name, port_desc} or {"", ""} if not found.
static std::pair<std::string, std::string> get_port_desc(const FlowNode& node, const FlowPin& pin) {
    if (node.lambda_grab.id == pin.id) return {"as_lambda", "pass as lambda"};
    if (node.bang_pin.id == pin.id) return {"bang", "bang connector"};

    auto* nt = find_node_type(node.type_id);

    auto find_bang = [&](const auto& pins, const PortDesc* descs, int count) -> std::pair<std::string, std::string> {
        int idx = 0;
        for (auto& p : pins) {
            if (p->id == pin.id) {
                if (descs && idx < count) return {descs[idx].name, descs[idx].desc};
                return {pin.name, ""};
            }
            idx++;
        }
        return {"", ""};
    };

    if (nt) {
        auto r = find_bang(node.triggers, nt->trigger_ports, nt->num_triggers);
        if (!r.first.empty()) return r;
        r = find_bang(node.nexts, nt->next_ports, nt->num_nexts);
        if (!r.first.empty()) return r;
        r = find_bang(node.outputs, nt->output_ports, nt->outputs);
        if (!r.first.empty()) return r;
    }

    for (int i = 0; i < (int)node.inputs.size(); i++) {
        if (node.inputs[i]->id != pin.id) continue;
        for (auto& expr : node.parsed_exprs) {
            if (!expr) continue;
            struct Finder {
                int target_idx; std::string result;
                void walk(const ExprPtr& e) {
                    if (!e || !result.empty()) return;
                    if (e->kind == ExprKind::PinRef && e->pin_ref.index == target_idx && !e->pin_ref.name.empty())
                        result = e->pin_ref.name;
                    for (auto& c : e->children) walk(c);
                }
            };
            int pin_idx = -1;
            try { pin_idx = std::stoi(pin.name); } catch (...) {}
            if (pin_idx >= 0) {
                Finder f{pin_idx, {}};
                f.walk(expr);
                if (!f.result.empty()) return {f.result, ""};
            }
        }
        return {pin.name, ""};
    }

    return {pin.name, ""};
}

// Get a display-friendly name for a node
static std::string node_display_name(const FlowNode& node) {
    return node.display_text();
}

// Build "display_name.port_name" label for a pin
static std::string pin_label(const FlowNode& node, const FlowPin& pin) {
    auto [port_name, _] = get_port_desc(node, pin);
    return node_display_name(node) + "." + port_name;
}

static float dist2(ImVec2 a, ImVec2 b) {
    float dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

static float point_to_bezier_dist(ImVec2 p, ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3) {
    float min_d2 = 1e18f;
    for (int i = 0; i <= 20; i++) {
        float t = i / 20.0f;
        float u = 1.0f - t;
        float x = u*u*u*p0.x + 3*u*u*t*p1.x + 3*u*t*t*p2.x + t*t*t*p3.x;
        float y = u*u*u*p0.y + 3*u*u*t*p1.y + 3*u*t*t*p2.y + t*t*t*p3.y;
        float dx = p.x - x, dy = p.y - y;
        float d2 = dx*dx + dy*dy;
        if (d2 < min_d2) min_d2 = d2;
    }
    return std::sqrt(min_d2);
}

enum class PinShape { Square, Signal, LambdaDown, LambdaLeft };

static void draw_pin(ImDrawList* dl, ImVec2 pos, float r, ImU32 col, PinShape shape, float zoom) {
    switch (shape) {
    case PinShape::Signal:
        dl->AddCircleFilled(pos, r, col);
        {
            float font_sz = r * 1.6f;
            if (font_sz > 3.0f) {
                ImVec2 ts = ImGui::CalcTextSize("~");
                float scale = font_sz / ImGui::GetFontSize();
                dl->AddText(nullptr, font_sz,
                            {pos.x - ts.x * scale * 0.5f, pos.y - ts.y * scale * 0.5f},
                            IM_COL32(30, 30, 40, 255), "~");
            }
        }
        break;
    case PinShape::LambdaDown:
        dl->AddTriangleFilled(
            {pos.x - r, pos.y - r},
            {pos.x + r, pos.y - r},
            {pos.x, pos.y + r},
            col);
        break;
    case PinShape::LambdaLeft:
        dl->AddTriangleFilled(
            {pos.x + r, pos.y - r},
            {pos.x - r, pos.y},
            {pos.x + r, pos.y + r},
            col);
        break;
    case PinShape::Square:
    default:
        dl->AddRectFilled({pos.x - r, pos.y - r}, {pos.x + r, pos.y + r}, col);
        break;
    }
}

static void draw_pin_highlight(ImDrawList* dl, ImVec2 pos, float r, ImU32 col, PinShape shape, float zoom) {
    float o = 2 * zoom;
    switch (shape) {
    case PinShape::Signal:
        dl->AddCircle(pos, r + o, col, 0, 2.0f);
        break;
    case PinShape::LambdaDown:
        dl->AddTriangle(
            {pos.x - r - o, pos.y - r - o},
            {pos.x + r + o, pos.y - r - o},
            {pos.x, pos.y + r + o},
            col, 2.0f);
        break;
    case PinShape::LambdaLeft:
        dl->AddTriangle(
            {pos.x + r + o, pos.y - r - o},
            {pos.x - r - o, pos.y},
            {pos.x + r + o, pos.y + r + o},
            col, 2.0f);
        break;
    case PinShape::Square:
    default:
        dl->AddRect({pos.x - r - o, pos.y - r - o}, {pos.x + r + o, pos.y + r + o}, col, 0, 0, 2.0f);
        break;
    }
}

static void draw_vbezier(ImDrawList* dl, ImVec2 from, ImVec2 to, ImU32 col, float thickness, float zoom) {
    float dy = std::max(std::abs(to.y - from.y) * 0.5f, 30.0f * zoom);
    dl->AddBezierCubic(from, {from.x, from.y + dy}, {to.x, to.y - dy}, to, col, thickness * zoom);
}

static ImVec2 bezier_sample(ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, float t) {
    float u = 1.0f - t;
    float uu = u * u, uuu = uu * u;
    float tt = t * t, ttt = tt * t;
    return {uuu*p0.x + 3*uu*t*p1.x + 3*u*tt*p2.x + ttt*p3.x,
            uuu*p0.y + 3*uu*t*p1.y + 3*u*tt*p2.y + ttt*p3.y};
}

static void draw_dashed_bezier(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3,
                                ImU32 col, float thickness, float dash_len, float gap_len) {
    const int N = 128;
    ImVec2 pts[N + 1];
    float arc[N + 1];
    pts[0] = p0; arc[0] = 0;
    for (int i = 1; i <= N; i++) {
        pts[i] = bezier_sample(p0, p1, p2, p3, (float)i / N);
        float dx = pts[i].x - pts[i-1].x, dy = pts[i].y - pts[i-1].y;
        arc[i] = arc[i-1] + sqrtf(dx*dx + dy*dy);
    }
    float total = arc[N];
    if (total < 1.0f) return;
    float cycle = dash_len + gap_len;

    auto lerp_at = [&](float d) -> ImVec2 {
        if (d <= 0) return pts[0];
        if (d >= total) return pts[N];
        int lo = 0, hi = N;
        while (lo < hi - 1) { int mid = (lo+hi)/2; if (arc[mid] < d) lo = mid; else hi = mid; }
        float seg_len = arc[hi] - arc[lo];
        float t = (seg_len > 0) ? (d - arc[lo]) / seg_len : 0;
        return {pts[lo].x + t * (pts[hi].x - pts[lo].x),
                pts[lo].y + t * (pts[hi].y - pts[lo].y)};
    };

    float d = 0;
    while (d < total) {
        float d_end = std::min(d + dash_len, total);
        ImVec2 prev = lerp_at(d);
        float step = 3.0f;
        for (float dd = d + step; dd <= d_end; dd += step) {
            ImVec2 cur = lerp_at(dd);
            dl->AddLine(prev, cur, col, thickness);
            prev = cur;
        }
        ImVec2 end = lerp_at(d_end);
        dl->AddLine(prev, end, col, thickness);
        d += cycle;
    }
}

static void draw_dashed_vbezier(ImDrawList* dl, ImVec2 from, ImVec2 to, ImU32 col, float thickness, float zoom) {
    float dy = std::max(std::abs(to.y - from.y) * 0.5f, 30.0f * zoom);
    draw_dashed_bezier(dl, from, {from.x, from.y + dy}, {to.x, to.y - dy}, to,
                       col, thickness * zoom, 8.0f * zoom, 4.0f * zoom);
}

// ============================================================
// Editor1Pane methods
// ============================================================

bool Editor1Pane::load(const std::string& path) {
    namespace fs = std::filesystem;
    std::string abs_path = fs::absolute(path).string();
    file_path_ = abs_path;
    tab_name_ = fs::path(path).stem().string();

    if (fs::exists(abs_path)) {
        if (!load_atto(abs_path, graph_))
            return false;
    }

    if (graph_.has_viewport) {
        canvas_offset_ = {graph_.viewport_x, graph_.viewport_y};
        canvas_zoom_ = graph_.viewport_zoom;
    }
    inference_dirty_ = true;
    return true;
}

void Editor1Pane::mark_dirty() {
    push_undo();
    dirty_ = true;
    inference_dirty_ = true;
    schedule_save();
}

void Editor1Pane::push_undo() {
    undo_stack_.push_back(save_atto_string(graph_));
    redo_stack_.clear();
    if (undo_stack_.size() > 200) undo_stack_.erase(undo_stack_.begin());
}

void Editor1Pane::undo() {
    if (undo_stack_.empty()) return;
    redo_stack_.push_back(save_atto_string(graph_));
    load_atto_string(undo_stack_.back(), graph_);
    undo_stack_.pop_back();
    dirty_ = true;
}

void Editor1Pane::redo() {
    if (redo_stack_.empty()) return;
    undo_stack_.push_back(save_atto_string(graph_));
    load_atto_string(redo_stack_.back(), graph_);
    redo_stack_.pop_back();
    dirty_ = true;
}

void Editor1Pane::schedule_save() {
    dirty_ = true;
    save_deadline_ = ImGui::GetTime() + 0.5;
}

void Editor1Pane::check_debounced_save() {
    if (save_deadline_ > 0 && ImGui::GetTime() >= save_deadline_) {
        save_deadline_ = 0;
        auto_save();
    }
}

void Editor1Pane::sync_viewport() {
    graph_.viewport_x = canvas_offset_.x;
    graph_.viewport_y = canvas_offset_.y;
    graph_.viewport_zoom = canvas_zoom_;
}

void Editor1Pane::auto_save() {
    if (dirty_ && !file_path_.empty()) {
        sync_viewport();
        save_atto(file_path_, graph_);
        dirty_ = false;
    }
}

ImVec2 Editor1Pane::canvas_to_screen(ImVec2 p, ImVec2 origin) const {
    return {origin.x + (p.x + canvas_offset_.x) * canvas_zoom_,
            origin.y + (p.y + canvas_offset_.y) * canvas_zoom_};
}

ImVec2 Editor1Pane::screen_to_canvas(ImVec2 p, ImVec2 origin) const {
    return {(p.x - origin.x) / canvas_zoom_ - canvas_offset_.x,
            (p.y - origin.y) / canvas_zoom_ - canvas_offset_.y};
}

ImVec2 Editor1Pane::get_pin_pos(const FlowNode& node, const FlowPin& pin, ImVec2 origin) const {
    if (pin.direction == FlowPin::LambdaGrab) {
        float x = node.position.x;
        float y = node.position.y + node.size.y * 0.5f;
        return canvas_to_screen({x, y}, origin);
    }

    if (pin.id == node.bang_pin.id && pin.name == "bang") {
        float x = node.position.x + node.size.x;
        float y = node.position.y + node.size.y * 0.5f;
        return canvas_to_screen({x, y}, origin);
    }

    if (pin.direction == FlowPin::BangTrigger) {
        int idx = 0;
        for (auto& p : node.triggers) { if (p->id == pin.id) break; idx++; }
        float x = node.position.x + PIN_SPACING * (idx + 0.5f);
        float y = node.position.y;
        return canvas_to_screen({x, y}, origin);
    }

    if (pin.direction == FlowPin::Input || pin.direction == FlowPin::Lambda) {
        int bang_offset = (int)node.triggers.size();
        int slot = 0;
        for (auto& p : node.inputs) {
            if (p->id == pin.id) break;
            if (!shadow_connected_pins_.count(p->id)) slot++;
        }
        float x = node.position.x + PIN_SPACING * (bang_offset + slot + 0.5f);
        float y = node.position.y;
        return canvas_to_screen({x, y}, origin);
    }

    if (pin.direction == FlowPin::BangNext) {
        int idx = 0;
        for (auto& p : node.nexts) { if (p->id == pin.id) break; idx++; }
        float x = node.position.x + PIN_SPACING * (idx + 0.5f);
        float y = node.position.y + node.size.y;
        return canvas_to_screen({x, y}, origin);
    }

    int offset = (int)node.nexts.size();
    int idx = 0;
    for (auto& p : node.outputs) { if (p->id == pin.id) break; idx++; }
    float x = node.position.x + PIN_SPACING * (offset + idx + 0.5f);
    float y = node.position.y + node.size.y;
    return canvas_to_screen({x, y}, origin);
}

Editor1Pane::PinHit Editor1Pane::hit_test_pin(ImVec2 sp, ImVec2 co, float radius) const {
    float r2 = radius * radius * canvas_zoom_ * canvas_zoom_;
    for (auto& node : graph_.nodes) {
        if (node.imported || node.shadow) continue;
        for (auto& pin : node.triggers)
            if (dist2(sp, get_pin_pos(node, *pin, co)) < r2)
                return {node.id, pin->id, FlowPin::BangTrigger};
        for (auto& pin : node.inputs)
            if (dist2(sp, get_pin_pos(node, *pin, co)) < r2)
                return {node.id, pin->id, pin->direction};
        for (auto& pin : node.outputs)
            if (dist2(sp, get_pin_pos(node, *pin, co)) < r2)
                return {node.id, pin->id, FlowPin::Output};
        for (auto& pin : node.nexts)
            if (dist2(sp, get_pin_pos(node, *pin, co)) < r2)
                return {node.id, pin->id, FlowPin::BangNext};
        if (!node.lambda_grab.id.empty() && dist2(sp, get_pin_pos(node, node.lambda_grab, co)) < r2) {
            auto* nt_hit = find_node_type(node.type_id);
            if (nt_hit && nt_hit->has_lambda)
                return {node.id, node.lambda_grab.id, FlowPin::LambdaGrab};
        }
        if (!node.bang_pin.id.empty() && dist2(sp, get_pin_pos(node, node.bang_pin, co)) < r2) {
            auto* nt_hit = find_node_type(node.type_id);
            bool hidden = (nt_hit && (nt_hit->is_event || nt_hit->no_post_bang));
            if (!hidden) return {node.id, node.bang_pin.id, FlowPin::BangNext};
        }
    }
    return {-1, "", FlowPin::Input};
}

int Editor1Pane::hit_test_link(ImVec2 sp, ImVec2 co, float threshold) const {
    for (auto& link : graph_.links) {
        ImVec2 fp = {}, tp = {};
        bool ff = false, ft = false;
        bool from_grab = false, from_bang_pin = false, to_lambda = false;
        for (auto& n : graph_.nodes) {
            for (auto& p : n.outputs) if (p->id == link.from_pin) { fp = get_pin_pos(n, *p, co); ff = true; }
            for (auto& p : n.nexts) if (p->id == link.from_pin) { fp = get_pin_pos(n, *p, co); ff = true; }
            for (auto& p : n.triggers) if (p->id == link.from_pin) { fp = get_pin_pos(n, *p, co); ff = true; }
            if (n.lambda_grab.id == link.from_pin) { fp = get_pin_pos(n, n.lambda_grab, co); ff = true; from_grab = true; }
            for (auto& p : n.triggers) if (p->id == link.to_pin) { tp = get_pin_pos(n, *p, co); ft = true; }
            for (auto& p : n.inputs) if (p->id == link.to_pin) { tp = get_pin_pos(n, *p, co); ft = true; if (p->direction == FlowPin::Lambda) to_lambda = true; }
            if (n.bang_pin.id == link.from_pin) { fp = get_pin_pos(n, n.bang_pin, co); ff = true; from_bang_pin = true; }
        }
        if (!ff || !ft) continue;
        float d;
        if (from_grab) {
            float dx = std::max(std::abs(tp.x - fp.x) * 0.5f, 30.0f * canvas_zoom_);
            float dy = std::max(std::abs(tp.y - fp.y) * 0.5f, 30.0f * canvas_zoom_);
            d = point_to_bezier_dist(sp, fp, {fp.x - dx, fp.y}, {tp.x, tp.y - dy}, tp);
        } else if (from_bang_pin) {
            float dx = std::max(std::abs(tp.x - fp.x) * 0.5f, 30.0f * canvas_zoom_);
            float dy_hit = std::max(std::abs(tp.y - fp.y) * 0.5f, 30.0f * canvas_zoom_);
            d = point_to_bezier_dist(sp, fp, {fp.x + dx, fp.y}, {tp.x, tp.y - dy_hit}, tp);
        } else {
            float dy = std::max(std::abs(tp.y - fp.y) * 0.5f, 30.0f * canvas_zoom_);
            d = point_to_bezier_dist(sp, fp, {fp.x, fp.y + dy}, {tp.x, tp.y - dy}, tp);
        }
        if (d < threshold * canvas_zoom_) return link.id;
    }
    return -1;
}

void Editor1Pane::draw_node(ImDrawList* dl, FlowNode& node, ImVec2 origin) {
    bool is_label = (node.type_id == NodeTypeID::Label);

    int visible_inputs = 0;
    for (auto& pin : node.inputs)
        if (!shadow_connected_pins_.count(pin->id)) visible_inputs++;
    int top_pins = (int)node.triggers.size() + visible_inputs;
    int bottom_pins = (int)(node.nexts.size() + node.outputs.size());
    int max_pins = std::max(top_pins, bottom_pins);
    float pin_w = (float)(max_pins + 1) * PIN_SPACING;

    std::string display_text;
    if (is_label) {
        display_text = node.args.empty() ? "(label)" : node.args;
    } else {
        display_text = node.display_text();
    }
    float font_scale = 17.0f / ImGui::GetFontSize();
    ImVec2 ts = ImGui::CalcTextSize(display_text.c_str());
    float text_w = ts.x * font_scale + 16.0f;

    float needed_w = std::max({pin_w, text_w, NODE_MIN_WIDTH});
    node.size = {needed_w, NODE_HEIGHT};

    ImVec2 tl = canvas_to_screen(to_imvec(node.position), origin);
    ImVec2 br = canvas_to_screen({node.position.x + node.size.x,
                                   node.position.y + node.size.y}, origin);

    if (is_label) {
        float font_size = 17.0f * canvas_zoom_;
        if (font_size > 6.0f && editing_node_ != node.id) {
            const char* display = node.args.empty() ? "(label)" : node.args.c_str();
            ImU32 col = node.args.empty() ? IM_COL32(100, 100, 100, 180) : IM_COL32(255, 255, 255, 255);
            dl->AddText(nullptr, font_size,
                        {tl.x + 2 * canvas_zoom_, tl.y + (br.y - tl.y - font_size) * 0.5f},
                        col, display);
        }
    } else {
        ImU32 bg = node.error.empty() ? COL_NODE_BG : IM_COL32(120, 30, 30, 230);
        ImU32 border = node.error.empty() ? IM_COL32(100, 100, 150, 255) : IM_COL32(200, 60, 60, 255);
        dl->AddRectFilled(tl, br, bg, NODE_ROUNDING * canvas_zoom_);

        if (highlight_node_id_ == node.id && highlight_timer_ > 0.0f) {
            float blink = std::sin(highlight_timer_ * 6.0f) * 0.5f + 0.5f;
            int a = (int)(blink * 140.0f);
            dl->AddRectFilled(tl, br, IM_COL32(180, 160, 40, a), NODE_ROUNDING * canvas_zoom_);
        }

        dl->AddRect(tl, br, border, NODE_ROUNDING * canvas_zoom_);

        if (selected_nodes_.count(node.id)) {
            dl->AddRect({tl.x - 2*canvas_zoom_, tl.y - 2*canvas_zoom_},
                        {br.x + 2*canvas_zoom_, br.y + 2*canvas_zoom_},
                        IM_COL32(100, 180, 255, 200), NODE_ROUNDING * canvas_zoom_, 0, 2.0f * canvas_zoom_);
        }

        float font_size = 17.0f * canvas_zoom_;
        if (font_size > 6.0f && editing_node_ != node.id) {
            std::string text = node.display_text();
            float scale = font_size / ImGui::GetFontSize();
            ImVec2 text_sz = ImGui::CalcTextSize(text.c_str());
            float tw = text_sz.x * scale;
            float cx = (tl.x + br.x) * 0.5f - tw * 0.5f;
            float cy = tl.y + (br.y - tl.y - font_size) * 0.5f;
            dl->AddText(nullptr, font_size, {cx, cy}, IM_COL32(220, 220, 220, 255), text.c_str());
        }
    }

    auto* nt = find_node_type(node.type_id);
    bool is_event = nt && nt->is_event;

    PinShape io_shape = PinShape::Signal;
    float pr = PIN_RADIUS * canvas_zoom_;
    {
        for (auto& pin : node.triggers) {
            ImVec2 pp = get_pin_pos(node, *pin, origin);
            draw_pin(dl, pp, pr, IM_COL32(255, 200, 80, 255), PinShape::Square, canvas_zoom_);
        }
        for (auto& pin : node.inputs) {
            if (shadow_connected_pins_.count(pin->id)) continue;
            ImVec2 pp = get_pin_pos(node, *pin, origin);
            if (pin->direction == FlowPin::Lambda)
                draw_pin(dl, pp, pr, IM_COL32(180, 130, 255, 255), PinShape::LambdaDown, canvas_zoom_);
            else
                draw_pin(dl, pp, pr, COL_PIN_IN, io_shape, canvas_zoom_);
        }
        for (auto& pin : node.nexts) {
            ImVec2 pp = get_pin_pos(node, *pin, origin);
            draw_pin(dl, pp, pr, IM_COL32(255, 200, 80, 255), PinShape::Square, canvas_zoom_);
        }
        for (auto& pin : node.outputs) {
            ImVec2 pp = get_pin_pos(node, *pin, origin);
            draw_pin(dl, pp, pr, COL_PIN_OUT, io_shape, canvas_zoom_);
        }
        bool show_lambda = nt && nt->has_lambda;
        if (!node.lambda_grab.id.empty() && show_lambda) {
            ImVec2 pp = get_pin_pos(node, node.lambda_grab, origin);
            draw_pin(dl, pp, pr, IM_COL32(180, 130, 255, 150), PinShape::LambdaLeft, canvas_zoom_);
        }
        bool no_post_bang = nt && nt->no_post_bang;
        if (!node.bang_pin.id.empty() && !is_event && !no_post_bang) {
            ImVec2 pp = get_pin_pos(node, node.bang_pin, origin);
            draw_pin(dl, pp, pr * 0.7f, IM_COL32(255, 200, 80, 255), PinShape::Square, canvas_zoom_);
        }
    }
}

void Editor1Pane::draw_link(ImDrawList* dl, const FlowLink& link, ImVec2 origin) {
    ImVec2 fp = {}, tp = {};
    bool ff = false, ft = false;
    bool to_lambda = false;
    bool from_grab = false;
    bool from_bang_pin = false;
    FlowPin* from_pin_ptr = nullptr;
    FlowPin* to_pin_ptr = nullptr;
    for (auto& n : graph_.nodes) {
        for (auto& p : n.outputs) if (p->id == link.from_pin) { fp = get_pin_pos(n, *p, origin); ff = true; from_pin_ptr = p.get(); }
        for (auto& p : n.nexts) if (p->id == link.from_pin) { fp = get_pin_pos(n, *p, origin); ff = true; from_pin_ptr = p.get(); }
        for (auto& p : n.triggers) if (p->id == link.from_pin) { fp = get_pin_pos(n, *p, origin); ff = true; from_pin_ptr = p.get(); }
        if (n.lambda_grab.id == link.from_pin) { fp = get_pin_pos(n, n.lambda_grab, origin); ff = true; from_grab = true; from_pin_ptr = &n.lambda_grab; }
        if (n.bang_pin.id == link.from_pin) { fp = get_pin_pos(n, n.bang_pin, origin); ff = true; from_bang_pin = true; from_pin_ptr = &n.bang_pin; }
        for (auto& p : n.triggers) if (p->id == link.to_pin) { tp = get_pin_pos(n, *p, origin); ft = true; to_pin_ptr = p.get(); }
        for (auto& p : n.inputs) if (p->id == link.to_pin) { tp = get_pin_pos(n, *p, origin); ft = true; to_pin_ptr = p.get(); if (p->direction == FlowPin::Lambda) to_lambda = true; }
    }
    if (!ff || !ft) return;

    bool type_error = !link.error.empty();
    if (!type_error && from_pin_ptr && to_pin_ptr &&
        from_pin_ptr->resolved_type && to_pin_ptr->resolved_type &&
        !from_pin_ptr->resolved_type->is_generic && !to_pin_ptr->resolved_type->is_generic) {
        type_error = !types_compatible(from_pin_ptr->resolved_type, to_pin_ptr->resolved_type);
    }

    bool from_trigger = from_pin_ptr && from_pin_ptr->direction == FlowPin::BangTrigger;

    ImU32 col_error = IM_COL32(255, 60, 60, 220);
    bool named = !link.net_name.empty() && !link.auto_wire;

    auto dim = [](ImU32 c) -> ImU32 {
        return (c & 0x00FFFFFF) | (((c >> 24) * 100 / 255) << 24);
    };
    if (named) col_error = dim(col_error);

    auto wire_col = [&](ImU32 c) { return named ? dim(c) : c; };

    if (from_trigger) {
        float dy = std::max(std::abs(tp.y - fp.y) * 0.5f, 40.0f * canvas_zoom_);
        ImU32 col = type_error ? col_error : wire_col(IM_COL32(255, 200, 80, 200));
        float th = 2.5f * canvas_zoom_;
        if (named)
            draw_dashed_bezier(dl, fp, {fp.x, fp.y - dy}, {tp.x, tp.y - dy}, tp, col, th, 8.0f * canvas_zoom_, 4.0f * canvas_zoom_);
        else
            dl->AddBezierCubic(fp, {fp.x, fp.y - dy}, {tp.x, tp.y - dy}, tp, col, th);
    } else if (from_grab) {
        float dx = std::max(std::abs(tp.x - fp.x) * 0.5f, 30.0f * canvas_zoom_);
        float dy = std::max(std::abs(tp.y - fp.y) * 0.5f, 30.0f * canvas_zoom_);
        ImU32 col = type_error ? col_error : wire_col(IM_COL32(180, 130, 255, 200));
        float th = 2.5f * canvas_zoom_;
        if (named)
            draw_dashed_bezier(dl, fp, {fp.x - dx, fp.y}, {tp.x, tp.y - dy}, tp, col, th, 8.0f * canvas_zoom_, 4.0f * canvas_zoom_);
        else
            dl->AddBezierCubic(fp, {fp.x - dx, fp.y}, {tp.x, tp.y - dy}, tp, col, th);
    } else if (from_bang_pin) {
        float dx = std::max(std::abs(tp.x - fp.x) * 0.5f, 30.0f * canvas_zoom_);
        float dy = std::max(std::abs(tp.y - fp.y) * 0.5f, 30.0f * canvas_zoom_);
        ImU32 col = type_error ? col_error : wire_col(IM_COL32(255, 200, 80, 200));
        float th = 2.5f * canvas_zoom_;
        if (named)
            draw_dashed_bezier(dl, fp, {fp.x + dx, fp.y}, {tp.x, tp.y - dy}, tp, col, th, 8.0f * canvas_zoom_, 4.0f * canvas_zoom_);
        else
            dl->AddBezierCubic(fp, {fp.x + dx, fp.y}, {tp.x, tp.y - dy}, tp, col, th);
    } else if (to_lambda) {
        ImU32 col = type_error ? col_error : wire_col(IM_COL32(180, 130, 255, 200));
        if (named)
            draw_dashed_vbezier(dl, fp, tp, col, 2.5f, canvas_zoom_);
        else
            draw_vbezier(dl, fp, tp, col, 2.5f, canvas_zoom_);
    } else {
        ImU32 col = type_error ? col_error : wire_col(COL_LINK);
        if (named)
            draw_dashed_vbezier(dl, fp, tp, col, 2.5f, canvas_zoom_);
        else
            draw_vbezier(dl, fp, tp, col, 2.5f, canvas_zoom_);
    }

    if (!link.net_name.empty() && !link.auto_wire) {
        float font_size = ImGui::GetFontSize() * canvas_zoom_ * 0.8f;
        if (font_size > 5.0f) {
            ImVec2 mid = {(fp.x + tp.x) * 0.5f, (fp.y + tp.y) * 0.5f};
            ImVec2 text_sz = ImGui::CalcTextSize(link.net_name.c_str());
            float tw = text_sz.x * (font_size / ImGui::GetFontSize());
            float th = text_sz.y * (font_size / ImGui::GetFontSize());
            float cx = mid.x - tw * 0.5f;
            float cy = mid.y - th * 0.5f;
            dl->AddRectFilled({cx - 3, cy - 1}, {cx + tw + 3, cy + th + 1},
                              IM_COL32(30, 30, 40, 200), 3.0f);
            dl->AddText(nullptr, font_size, {cx, cy}, IM_COL32(180, 220, 255, 255), link.net_name.c_str());
        }
    }
}

void Editor1Pane::validate_nodes() {
    resolve_type_based_pins(graph_);

    TypeRegistry registry;
    for (auto& node : graph_.nodes) {
        if (node.type_id == NodeTypeID::DeclType) {
            auto tokens = tokenize_args(node.args, false);
            if (tokens.size() >= 2) {
                std::string type_name = tokens[0];
                std::string def;
                for (size_t i = 1; i < tokens.size(); i++) {
                    if (!def.empty()) def += " ";
                    def += tokens[i];
                }
                int decl_class = classify_decl_type(tokens);
                if (decl_class == 0 || decl_class == 1) {
                    registry.register_type(type_name, def);
                } else {
                    registry.register_type(type_name, "void");
                }
            }
        }
    }

    registry.resolve_all();

    for (auto& node : graph_.nodes) {
        node.error.clear();

        auto* nt = find_node_type(node.type_id);
        if (!nt) {
            node.error = "Unknown node type: " + std::string(node_type_str(node.type_id));
            continue;
        }

        for (auto& other : graph_.nodes) {
            if (&other != &node && other.guid == node.guid) {
                node.error = "Duplicate guid: " + node.guid;
                break;
            }
        }
        if (!node.error.empty()) continue;

        if (node.type_id == NodeTypeID::DeclType) {
            auto tokens = tokenize_args(node.args, false);
            if (tokens.empty()) {
                node.error = "decl_type requires a type name";
                continue;
            }
            std::string type_name = tokens[0];
            if (!type_name.empty() && type_name[0] == '$') {
                node.error = "Type name should not start with $";
                continue;
            }

            auto err_it = registry.errors.find(type_name);
            if (err_it != registry.errors.end()) {
                node.error = err_it->second;
                continue;
            }

            int decl_class_v = classify_decl_type(tokens);
            if (decl_class_v == 2) {
                bool has_any_field = false;
                for (size_t i = 1; i < tokens.size(); i++) {
                    if (tokens[i].find(':') != std::string::npos) { has_any_field = true; break; }
                }
                if (!has_any_field) {
                    node.error = "Struct type '" + type_name + "' must have at least one field (name:type)";
                    continue;
                }
            }

            for (size_t i = 1; i < tokens.size(); i++) {
                auto& tok = tokens[i];
                if (tok == "->" || tok[0] == '(') continue;
                auto colon = tok.find(':');
                if (colon != std::string::npos) {
                    std::string field_type = tok.substr(colon + 1);
                    std::string err;
                    if (!registry.validate_type(field_type, err)) {
                        node.error = "Field '" + tok.substr(0, colon) + "': " + err;
                        break;
                    }
                }
            }
        }

        if (node.type_id == NodeTypeID::DeclVar) {
            auto tokens = tokenize_args(node.args, false);
            if (tokens.size() < 2) {
                node.error = "decl_var requires: name type";
                continue;
            }
            if (!tokens[0].empty() && tokens[0][0] == '$') {
                node.error = "Variable name should not start with $ in declarations";
                continue;
            }
            std::string err;
            if (!registry.validate_type(tokens[1], err)) {
                node.error = "Invalid type: " + err;
            }
        }

        if (node.type_id == NodeTypeID::New) {
            auto tokens = tokenize_args(node.args, false);
            if (tokens.empty()) {
                node.error = "new requires a type name";
                continue;
            }
            if (registry.type_defs.count(tokens[0]) == 0) {
                node.error = "Unknown type: " + tokens[0];
            }
        }

        if (node.type_id == NodeTypeID::EventBang) {
            auto tokens = tokenize_args(node.args, false);
            if (tokens.empty()) {
                node.error = "event! requires an event name (e.g. ~my_event)";
                continue;
            }
            if (tokens[0].empty() || tokens[0][0] != '~') {
                node.error = "Event name must start with ~ (e.g. ~" + tokens[0] + ")";
                continue;
            }
            auto* event_decl = find_event_node(graph_, tokens[0]);
            if (!event_decl) {
                node.error = "Unknown event: " + tokens[0];
                continue;
            }
            auto ev_tokens = tokenize_args(event_decl->args, false);
            bool found_arrow = false;
            std::string ret_type;
            for (size_t i = 1; i < ev_tokens.size(); i++) {
                if (ev_tokens[i] == "->") {
                    found_arrow = true;
                    if (i + 1 < ev_tokens.size()) ret_type = ev_tokens[i + 1];
                    break;
                }
            }
            if (found_arrow && ret_type != "void") {
                node.error = "Event return type must be void (got: " + ret_type + ")";
            }
        }
    }

    run_type_inference();
}

void Editor1Pane::run_type_inference() {
    GraphInference inference(type_pool_);
    inference.run(graph_);
}

void Editor1Pane::center_on_node(const FlowNode& node, ImVec2 canvas_size) {
    canvas_offset_.x = -node.position.x - node.size.x * 0.5f + canvas_size.x * 0.5f / canvas_zoom_;
    canvas_offset_.y = -node.position.y - node.size.y * 0.5f + canvas_size.y * 0.5f / canvas_zoom_;
    highlight_node_id_ = node.id;
    highlight_timer_ = 3.0f;
}

void Editor1Pane::copy_selection() {
    clipboard_nodes_.clear();
    clipboard_links_.clear();
    if (selected_nodes_.empty()) return;

    ImVec2 centroid = {0, 0};
    int count = 0;
    for (auto& node : graph_.nodes) {
        if (!selected_nodes_.count(node.id)) continue;
        centroid.x += node.position.x;
        centroid.y += node.position.y;
        count++;
    }
    if (count > 0) { centroid.x /= count; centroid.y /= count; }

    std::map<int, int> id_to_idx;
    for (auto& node : graph_.nodes) {
        if (!selected_nodes_.count(node.id)) continue;
        int idx = (int)clipboard_nodes_.size();
        id_to_idx[node.id] = idx;
        clipboard_nodes_.push_back({node.type_id, node.args,
            {node.position.x - centroid.x, node.position.y - centroid.y}});
    }

    std::map<std::string, std::pair<int, std::string>> pin_owner;
    for (auto& node : graph_.nodes) {
        if (!selected_nodes_.count(node.id)) continue;
        auto register_pin = [&](const FlowPin& p) { pin_owner[p.id] = {node.id, p.name}; };
        for (auto& p : node.triggers) register_pin(*p);
        for (auto& p : node.inputs) register_pin(*p);
        for (auto& p : node.outputs) register_pin(*p);
        for (auto& p : node.nexts) register_pin(*p);
        register_pin(node.lambda_grab);
        register_pin(node.bang_pin);
    }
    for (auto& link : graph_.links) {
        auto fi = pin_owner.find(link.from_pin);
        auto ti = pin_owner.find(link.to_pin);
        if (fi != pin_owner.end() && ti != pin_owner.end()) {
            auto from_idx = id_to_idx[fi->second.first];
            auto to_idx = id_to_idx[ti->second.first];
            clipboard_links_.push_back({from_idx, to_idx, fi->second.second, ti->second.second});
        }
    }
}

void Editor1Pane::paste_at(ImVec2 canvas_pos) {
    if (clipboard_nodes_.empty()) return;

    selected_nodes_.clear();
    std::vector<std::string> new_guids;

    for (auto& cn : clipboard_nodes_) {
        std::string guid = generate_guid();
        new_guids.push_back(guid);
        ImVec2 pos = {canvas_pos.x + cn.offset.x, canvas_pos.y + cn.offset.y};
        int id = graph_.add_node(guid, to_vec2(pos), 0, 0);

        for (auto& node : graph_.nodes) {
            if (node.id != id) continue;
            node.type_id = cn.type_id;
            node.args = cn.args;
            node.parse_args();

            auto* nt = find_node_type(cn.type_id);
            if (nt) {
                node.triggers.clear();
                node.inputs.clear();
                node.outputs.clear();
                node.nexts.clear();

                for (int i = 0; i < nt->num_triggers; i++) {
                    std::string biname = (nt->trigger_ports && i < nt->num_triggers) ? nt->trigger_ports[i].name : ("bang_in" + std::to_string(i));
                    node.triggers.push_back(make_pin("", biname, "", nullptr, FlowPin::BangTrigger));
                }

                bool is_expr_paste = is_any_of(cn.type_id, NodeTypeID::Expr, NodeTypeID::ExprBang);
                int num_outputs = nt->outputs;
                if (is_expr_paste) {
                    auto parsed = scan_slots(cn.args);
                    int total_top = parsed.total_pin_count(nt->inputs);
                    for (int i = 0; i < total_top; i++) {
                        bool il = parsed.is_lambda_slot(i);
                        std::string pn = il ? ("@"+std::to_string(i)) : std::to_string(i);
                        node.inputs.push_back(make_pin("", pn, "", nullptr, il ? FlowPin::Lambda : FlowPin::Input));
                    }
                    if (!cn.args.empty()) {
                        auto tokens = tokenize_args(cn.args, false);
                        num_outputs = std::max(1, (int)tokens.size());
                    }
                } else {
                    auto info = compute_inline_args(cn.args, nt->inputs);
                    if (!info.error.empty()) node.error = info.error;
                    int ref_pins = (info.pin_slots.max_slot >= 0) ? (info.pin_slots.max_slot + 1) : 0;
                    for (int i = 0; i < ref_pins; i++) {
                        bool il = info.pin_slots.is_lambda_slot(i);
                        std::string pn = il ? ("@"+std::to_string(i)) : std::to_string(i);
                        node.inputs.push_back(make_pin("", pn, "", nullptr, il ? FlowPin::Lambda : FlowPin::Input));
                    }
                    for (int i = info.num_inline_args; i < nt->inputs; i++) {
                        std::string pn; bool il = false;
                        if (nt->input_ports && i < nt->inputs) {
                            pn = nt->input_ports[i].name;
                            il = (nt->input_ports[i].kind == PortKind::Lambda);
                        } else pn = std::to_string(i);
                        node.inputs.push_back(make_pin("", pn, "", nullptr, il ? FlowPin::Lambda : FlowPin::Input));
                    }
                }
                for (int i = 0; i < num_outputs; i++) {
                    std::string oname = (nt->output_ports && i < nt->outputs) ? nt->output_ports[i].name : ("out" + std::to_string(i));
                    node.outputs.push_back(make_pin("", oname, "", nullptr, FlowPin::Output));
                }
                for (int i = 0; i < nt->num_nexts; i++) {
                    std::string bname = (nt->next_ports && i < nt->num_nexts) ? nt->next_ports[i].name : ("bang" + std::to_string(i));
                    node.nexts.push_back(make_pin("", bname, "", nullptr, FlowPin::BangNext));
                }
            }
            node.rebuild_pin_ids();
            selected_nodes_.insert(id);
            break;
        }
    }

    for (auto& cl : clipboard_links_) {
        if (cl.from_idx < 0 || cl.from_idx >= (int)new_guids.size()) continue;
        if (cl.to_idx < 0 || cl.to_idx >= (int)new_guids.size()) continue;
        std::string from_id = new_guids[cl.from_idx] + "." + cl.from_pin_name;
        std::string to_id = new_guids[cl.to_idx] + "." + cl.to_pin_name;
        graph_.add_link(from_id, to_id);
    }

    resolve_type_based_pins(graph_);
    mark_dirty();
}

// ============================================================
// Editor1Pane::draw() — the big legacy canvas drawing function
// ============================================================

void Editor1Pane::draw() {
    // Tick highlight timer
    if (highlight_timer_ > 0.0f) {
        highlight_timer_ -= ImGui::GetIO().DeltaTime;
        if (highlight_timer_ <= 0.0f) {
            highlight_timer_ = 0.0f;
            highlight_node_id_ = -1;
        }
    }

    // Validate only when graph structure changes
    if (graph_.dirty) {
        validate_nodes();
        graph_.dirty = false;
    }

    // Check debounced save
    check_debounced_save();

    ImVec2 canvas_origin = ImGui::GetCursorScreenPos();
    ImVec2 canvas_size = ImGui::GetContentRegionAvail();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background
    dl->AddRectFilled(canvas_origin,
                      {canvas_origin.x + canvas_size.x, canvas_origin.y + canvas_size.y}, COL_BG);

    // Safety: remove any empty-named nodes that aren't currently being edited
    std::erase_if(graph_.nodes, [&](auto& n) {
        if (n.id == editing_node_) return false;
        if (n.guid.empty()) return true;
        return false;
    });

    // Grid
    float grid = GRID_SIZE * canvas_zoom_;
    if (grid > 4.0f) {
        float ox = std::fmod(canvas_offset_.x * canvas_zoom_, grid);
        float oy = std::fmod(canvas_offset_.y * canvas_zoom_, grid);
        for (float x = ox; x < canvas_size.x; x += grid)
            dl->AddLine({canvas_origin.x + x, canvas_origin.y},
                        {canvas_origin.x + x, canvas_origin.y + canvas_size.y}, COL_GRID);
        for (float y = oy; y < canvas_size.y; y += grid)
            dl->AddLine({canvas_origin.x, canvas_origin.y + y},
                        {canvas_origin.x + canvas_size.x, canvas_origin.y + y}, COL_GRID);
    }

    ImGui::InvisibleButton("##canvas", canvas_size,
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonMiddle |
                           ImGuiButtonFlags_MouseButtonRight);
    bool canvas_hovered = ImGui::IsItemHovered();
    ImVec2 mouse_pos = ImGui::GetMousePos();

    // --- Canvas pan ---
    if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        canvas_dragging_ = true;
        canvas_drag_start_ = mouse_pos;
    }
    if (canvas_dragging_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
            ImVec2 delta = {mouse_pos.x - canvas_drag_start_.x, mouse_pos.y - canvas_drag_start_.y};
            canvas_offset_.x += delta.x / canvas_zoom_;
            canvas_offset_.y += delta.y / canvas_zoom_;
            canvas_drag_start_ = mouse_pos;
            schedule_save();
        } else { canvas_dragging_ = false; }
    }

    // --- Canvas zoom ---
    if (canvas_hovered) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (std::abs(wheel) > 0.01f) {
            float zf = std::pow(1.1f, wheel);
            ImVec2 mc = screen_to_canvas(mouse_pos, canvas_origin);
            canvas_zoom_ *= zf;
            canvas_zoom_ = std::clamp(canvas_zoom_, 0.2f, 5.0f);
            ImVec2 mc2 = screen_to_canvas(mouse_pos, canvas_origin);
            canvas_offset_.x += mc2.x - mc.x;
            canvas_offset_.y += mc2.y - mc.y;
            schedule_save();
        }
    }

    // Helper: hit test node at canvas pos
    auto hit_test_node = [&](ImVec2 mc) -> int {
        for (int i = (int)graph_.nodes.size() - 1; i >= 0; i--) {
            auto& node = graph_.nodes[i];
            if (node.imported || node.shadow) continue;
            if (mc.x >= node.position.x && mc.x <= node.position.x + node.size.x &&
                mc.y >= node.position.y && mc.y <= node.position.y + node.size.y)
                return node.id;
        }
        return -1;
    };

    // --- Double-click on node: edit ---
    if (canvas_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        ImVec2 mc = screen_to_canvas(mouse_pos, canvas_origin);
        int hit_id = hit_test_node(mc);
        if (hit_id >= 0) {
            for (auto& node : graph_.nodes) {
                if (node.id == hit_id) {
                    editing_node_ = node.id;
                    creating_new_node_ = false;
                    dragging_node_ = -1;
                    edit_buf_ = node.edit_text();
                    edit_just_opened_ = true;
                    break;
                }
            }
        }
    }
    // --- Single click ---
    else if (canvas_hovered && editing_link_ < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (editing_node_ >= 0) {
            if (creating_new_node_ && editing_node_ > 0) graph_.remove_node(editing_node_);
            editing_node_ = -1;
            creating_new_node_ = false;
            selected_nodes_.clear();
        } else {
            auto pin_hit = hit_test_pin(mouse_pos, canvas_origin);
            if (!pin_hit.pin_id.empty()) {
                dragging_link_from_pin_ = pin_hit.pin_id;
                dragging_link_from_output_ = true;
                dragging_node_ = -1;
                dragging_selection_ = false;
            } else {
                dragging_link_from_pin_.clear();
                ImVec2 mc = screen_to_canvas(mouse_pos, canvas_origin);
                int hit_id = hit_test_node(mc);

                if (hit_id >= 0) {
                    if (selected_nodes_.count(hit_id)) {
                        dragging_selection_ = true;
                        dragging_node_ = -1;
                    } else {
                        selected_nodes_.clear();
                        selected_nodes_.insert(hit_id);
                        dragging_selection_ = true;
                        dragging_node_ = -1;
                    }
                } else {
                    int wire_hit = hit_test_link(mouse_pos, canvas_origin);
                    if (wire_hit >= 0) {
                        dragging_node_ = -1;
                        dragging_selection_ = false;
                    } else {
                        box_selecting_ = true;
                        box_select_start_ = mouse_pos;
                        dragging_node_ = -1;
                        dragging_selection_ = false;
                    }
                }
            }
        }
    }

    // --- Box selection ---
    if (box_selecting_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float dx = mouse_pos.x - box_select_start_.x;
            float dy = mouse_pos.y - box_select_start_.y;
            float dist = dx*dx + dy*dy;
            if (dist > 25.0f) {
                ImVec2 a = box_select_start_;
                ImVec2 b = mouse_pos;
                ImVec2 tl_box = {std::min(a.x, b.x), std::min(a.y, b.y)};
                ImVec2 br_box = {std::max(a.x, b.x), std::max(a.y, b.y)};
                dl->AddRectFilled(tl_box, br_box, IM_COL32(100, 150, 255, 40));
                dl->AddRect(tl_box, br_box, IM_COL32(100, 150, 255, 180));

                ImVec2 ca = screen_to_canvas(tl_box, canvas_origin);
                ImVec2 cb = screen_to_canvas(br_box, canvas_origin);
                selected_nodes_.clear();
                for (auto& node : graph_.nodes) {
                    if (node.imported || node.shadow) continue;
                    if (node.position.x + node.size.x >= ca.x && node.position.x <= cb.x &&
                        node.position.y + node.size.y >= ca.y && node.position.y <= cb.y)
                        selected_nodes_.insert(node.id);
                }
            }
        } else {
            float dx = mouse_pos.x - box_select_start_.x;
            float dy = mouse_pos.y - box_select_start_.y;
            if (dx*dx + dy*dy <= 25.0f) {
                if (!selected_nodes_.empty()) {
                    selected_nodes_.clear();
                } else {
                    creating_new_node_ = true;
                    editing_node_ = 0;
                    new_node_pos_ = screen_to_canvas(mouse_pos, canvas_origin);
                    edit_buf_.clear();
                    edit_just_opened_ = true;
                }
            }
            box_selecting_ = false;
        }
    }

    // Link dragging
    if (!dragging_link_from_pin_.empty()) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            auto pin_hit = hit_test_pin(mouse_pos, canvas_origin);
            if (!pin_hit.pin_id.empty() && pin_hit.pin_id != dragging_link_from_pin_) {
                auto from_dir = FlowPin::Input;
                for (auto& node : graph_.nodes) {
                    for (auto& p : node.triggers) if (p->id == dragging_link_from_pin_) from_dir = p->direction;
                    for (auto& p : node.inputs) if (p->id == dragging_link_from_pin_) from_dir = p->direction;
                    for (auto& p : node.outputs) if (p->id == dragging_link_from_pin_) from_dir = p->direction;
                    for (auto& p : node.nexts) if (p->id == dragging_link_from_pin_) from_dir = p->direction;
                    if (node.lambda_grab.id == dragging_link_from_pin_) from_dir = node.lambda_grab.direction;
                    if (node.bang_pin.id == dragging_link_from_pin_) from_dir = node.bang_pin.direction;
                }

                auto is_source = [](FlowPin::Direction d) {
                    return d == FlowPin::Output || d == FlowPin::BangNext ||
                           d == FlowPin::LambdaGrab || d == FlowPin::BangTrigger;
                };
                auto is_dest = [](FlowPin::Direction d) {
                    return d == FlowPin::Input || d == FlowPin::BangTrigger ||
                           d == FlowPin::Lambda;
                };

                std::string from_pin, to_pin;
                bool valid = false;
                if (is_source(from_dir) && is_dest(pin_hit.dir)) {
                    from_pin = dragging_link_from_pin_;
                    to_pin = pin_hit.pin_id;
                    valid = true;
                } else if (is_source(pin_hit.dir) && is_dest(from_dir)) {
                    from_pin = pin_hit.pin_id;
                    to_pin = dragging_link_from_pin_;
                    valid = true;
                }

                if (valid) {
                    FlowPin::Direction to_dir = FlowPin::Input;
                    for (auto& node : graph_.nodes) {
                        for (auto& p : node.triggers) if (p->id == to_pin) to_dir = FlowPin::BangTrigger;
                        for (auto& p : node.inputs) if (p->id == to_pin) to_dir = p->direction;
                    }
                    bool allow_multi = (to_dir == FlowPin::BangTrigger || to_dir == FlowPin::Lambda);
                    if (!allow_multi)
                        std::erase_if(graph_.links, [&](auto& l) { return l.to_pin == to_pin; });
                    graph_.add_link(from_pin, to_pin);
                    mark_dirty();
                }
            }
            dragging_link_from_pin_.clear();
        }
    }

    // Grab pending: waiting for drag threshold before detaching links (right mouse)
    if (grab_pending_ && !grabbed_pin_.empty()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            float dx = mouse_pos.x - grab_start_.x;
            float dy = mouse_pos.y - grab_start_.y;
            if (dx*dx + dy*dy > 25.0f) {
                grab_pending_ = false;
                for (auto& l : graph_.links) {
                    if (grab_is_output_) {
                        if (l.from_pin == grabbed_pin_)
                            grabbed_links_.push_back({l.from_pin, l.to_pin});
                    } else {
                        if (l.to_pin == grabbed_pin_)
                            grabbed_links_.push_back({l.from_pin, l.to_pin});
                    }
                }
                if (!grabbed_links_.empty()) {
                    if (grab_is_output_)
                        std::erase_if(graph_.links, [&](auto& l) { return l.from_pin == grabbed_pin_; });
                    else
                        std::erase_if(graph_.links, [&](auto& l) { return l.to_pin == grabbed_pin_; });
                    graph_.dirty = true;
                } else {
                    grabbed_pin_.clear();
                }
            }
        } else {
            grab_pending_ = false;
            grabbed_pin_.clear();
        }
    }

    // Grabbed links: actively dragging detached connections (right mouse)
    if (!grabbed_links_.empty() && !grab_pending_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            for (auto& gl : grabbed_links_) {
                ImVec2 anchor = {};
                bool found = false;
                std::string anchor_id = grab_is_output_ ? gl.to_pin : gl.from_pin;
                for (auto& n : graph_.nodes) {
                    for (auto& p : n.outputs) if (p->id == anchor_id) { anchor = get_pin_pos(n, *p, canvas_origin); found = true; }
                    for (auto& p : n.nexts) if (p->id == anchor_id) { anchor = get_pin_pos(n, *p, canvas_origin); found = true; }
                    if (n.lambda_grab.id == anchor_id) { anchor = get_pin_pos(n, n.lambda_grab, canvas_origin); found = true; }
                    if (n.bang_pin.id == anchor_id) { anchor = get_pin_pos(n, n.bang_pin, canvas_origin); found = true; }
                    for (auto& p : n.triggers) if (p->id == anchor_id) { anchor = get_pin_pos(n, *p, canvas_origin); found = true; }
                    for (auto& p : n.inputs) if (p->id == anchor_id) { anchor = get_pin_pos(n, *p, canvas_origin); found = true; }
                }
                if (found) {
                    ImU32 col = COL_LINK_DRAG;
                    if (grab_is_output_)
                        draw_vbezier(dl, mouse_pos, anchor, col, 2.5f, canvas_zoom_);
                    else
                        draw_vbezier(dl, anchor, mouse_pos, col, 2.5f, canvas_zoom_);
                }
            }
        } else {
            auto pin_hit = hit_test_pin(mouse_pos, canvas_origin);
            bool reconnected = false;
            if (!pin_hit.pin_id.empty()) {
                if (grab_is_output_) {
                    if (pin_hit.dir == FlowPin::Output || pin_hit.dir == FlowPin::BangNext || pin_hit.dir == FlowPin::LambdaGrab) {
                        for (auto& gl : grabbed_links_)
                            graph_.add_link(pin_hit.pin_id, gl.to_pin);
                        reconnected = true;
                        mark_dirty();
                    }
                } else {
                    if (pin_hit.dir == FlowPin::Input || pin_hit.dir == FlowPin::BangTrigger || pin_hit.dir == FlowPin::Lambda) {
                        if (pin_hit.dir != FlowPin::BangTrigger && pin_hit.dir != FlowPin::Lambda)
                            std::erase_if(graph_.links, [&](auto& l) { return l.to_pin == pin_hit.pin_id; });
                        for (auto& gl : grabbed_links_)
                            graph_.add_link(gl.from_pin, pin_hit.pin_id);
                        reconnected = true;
                        mark_dirty();
                    }
                }
            }
            if (!reconnected) {
                for (auto& gl : grabbed_links_)
                    graph_.add_link(gl.from_pin, gl.to_pin);
            }
            grabbed_links_.clear();
            grabbed_pin_.clear();
        }
    }

    // Selection dragging (move all selected nodes)
    if (dragging_selection_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        ImVec2 delta = ImGui::GetIO().MouseDelta;
        for (auto& node : graph_.nodes) {
            if (selected_nodes_.count(node.id)) {
                node.position.x += delta.x / canvas_zoom_;
                node.position.y += delta.y / canvas_zoom_;
            }
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (dragging_selection_) mark_dirty();
        dragging_selection_ = false;
        dragging_node_ = -1;
    }

    // --- Keyboard shortcuts ---
    if (canvas_hovered && editing_node_ < 0) {
        bool ctrl = ImGui::GetIO().KeyCtrl;
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C)) {
            copy_selection();
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V)) {
            ImVec2 mc = screen_to_canvas(mouse_pos, canvas_origin);
            paste_at(mc);
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D)) {
            auto saved_nodes = clipboard_nodes_;
            auto saved_links = clipboard_links_;
            copy_selection();
            ImVec2 mc = screen_to_canvas(mouse_pos, canvas_origin);
            paste_at(mc);
            clipboard_nodes_ = saved_nodes;
            clipboard_links_ = saved_links;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !selected_nodes_.empty()) {
            for (int id : selected_nodes_)
                graph_.remove_node(id);
            selected_nodes_.clear();
            mark_dirty();
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
            if (ImGui::GetIO().KeyShift)
                redo();
            else
                undo();
            selected_nodes_.clear();
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) {
            redo();
            selected_nodes_.clear();
        }
    }

    // --- Right click: track start position and check for pin grab ---
    static ImVec2 right_click_start = {};
    if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        right_click_start = mouse_pos;
        auto pin_hit = hit_test_pin(mouse_pos, canvas_origin);
        if (!pin_hit.pin_id.empty()) {
            grabbed_links_.clear();
            grabbed_pin_ = pin_hit.pin_id;
            grab_is_output_ = (pin_hit.dir == FlowPin::Output || pin_hit.dir == FlowPin::BangNext ||
                               pin_hit.dir == FlowPin::LambdaGrab);
            grab_pending_ = true;
            grab_start_ = mouse_pos;
        }
    }

    // --- Right click release: disconnect pin, delete link, or delete node ---
    if (canvas_hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        float rdx = mouse_pos.x - right_click_start.x;
        float rdy = mouse_pos.y - right_click_start.y;
        bool was_drag = (rdx*rdx + rdy*rdy > 25.0f);

        if (!was_drag) {
        auto pin_hit = hit_test_pin(mouse_pos, canvas_origin);
        if (!pin_hit.pin_id.empty()) {
            std::erase_if(graph_.links, [&](auto& l) {
                return l.from_pin == pin_hit.pin_id || l.to_pin == pin_hit.pin_id;
            });
            graph_.dirty = true;
        }
        else {
            int lid = hit_test_link(mouse_pos, canvas_origin);
            if (lid >= 0) {
                graph_.remove_link(lid);
            } else {
                ImVec2 mc = screen_to_canvas(mouse_pos, canvas_origin);
                for (int i = (int)graph_.nodes.size() - 1; i >= 0; i--) {
                    auto& node = graph_.nodes[i];
                    if (mc.x >= node.position.x && mc.x <= node.position.x + node.size.x &&
                        mc.y >= node.position.y && mc.y <= node.position.y + node.size.y) {
                        graph_.remove_node(node.id);
                        if (editing_node_ == node.id) {
                            editing_node_ = -1;
                            creating_new_node_ = false;
                        }
                        break;
                    }
                }
            }
        }
        mark_dirty();
        } // !was_drag
    }

    // --- Build shadow filter sets for drawing ---
    std::set<std::string> shadow_guids;
    shadow_connected_pins_.clear();
    for (auto& node : graph_.nodes)
        if (node.shadow) shadow_guids.insert(node.guid);
    for (auto& link : graph_.links) {
        auto d1 = link.from_pin.find('.');
        if (d1 != std::string::npos && shadow_guids.count(link.from_pin.substr(0, d1)))
            shadow_connected_pins_.insert(link.to_pin);
        auto d2 = link.to_pin.find('.');
        if (d2 != std::string::npos && shadow_guids.count(link.to_pin.substr(0, d2)))
            shadow_connected_pins_.insert(link.from_pin);
    }

    // --- Draw links (skip links involving shadow nodes) ---
    for (auto& link : graph_.links) {
        auto d1 = link.from_pin.find('.');
        auto d2 = link.to_pin.find('.');
        if (d1 != std::string::npos && shadow_guids.count(link.from_pin.substr(0, d1))) continue;
        if (d2 != std::string::npos && shadow_guids.count(link.to_pin.substr(0, d2))) continue;
        draw_link(dl, link, canvas_origin);
    }

    // --- Draw link being dragged ---
    if (!dragging_link_from_pin_.empty() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        for (auto& node : graph_.nodes) {
            ImVec2 from = {};
            bool from_grab = false;
            bool from_bang_pin = false;
            bool found = false;
            for (auto& pin : node.outputs) {
                if (pin->id == dragging_link_from_pin_) { from = get_pin_pos(node, *pin, canvas_origin); found = true; break; }
            }
            if (!found) for (auto& pin : node.nexts) {
                if (pin->id == dragging_link_from_pin_) { from = get_pin_pos(node, *pin, canvas_origin); found = true; break; }
            }
            if (!found) for (auto& pin : node.inputs) {
                if (pin->id == dragging_link_from_pin_) { from = get_pin_pos(node, *pin, canvas_origin); found = true; break; }
            }
            if (!found) for (auto& pin : node.triggers) {
                if (pin->id == dragging_link_from_pin_) { from = get_pin_pos(node, *pin, canvas_origin); found = true; break; }
            }
            if (!found && node.lambda_grab.id == dragging_link_from_pin_) {
                from = get_pin_pos(node, node.lambda_grab, canvas_origin);
                found = true;
                from_grab = true;
            }
            if (!found && node.bang_pin.id == dragging_link_from_pin_) {
                from = get_pin_pos(node, node.bang_pin, canvas_origin);
                found = true;
                from_bang_pin = true;
            }
            if (found) {
                auto target = hit_test_pin(mouse_pos, canvas_origin);
                bool valid_target = !target.pin_id.empty() && target.pin_id != dragging_link_from_pin_;
                ImU32 col = valid_target ? COL_PIN_HOVER : COL_LINK_DRAG;
                if (from_grab) {
                    float dx = std::max(std::abs(mouse_pos.x - from.x) * 0.5f, 30.0f * canvas_zoom_);
                    float dy = std::max(std::abs(mouse_pos.y - from.y) * 0.5f, 30.0f * canvas_zoom_);
                    dl->AddBezierCubic(from, {from.x - dx, from.y}, {mouse_pos.x, mouse_pos.y - dy},
                                       mouse_pos, col, 2.5f * canvas_zoom_);
                } else if (from_bang_pin) {
                    float dx = std::max(std::abs(mouse_pos.x - from.x) * 0.5f, 30.0f * canvas_zoom_);
                    dl->AddBezierCubic(from, {from.x + dx, from.y}, {mouse_pos.x - dx, mouse_pos.y},
                                       mouse_pos, col, 2.5f * canvas_zoom_);
                } else {
                    draw_vbezier(dl, from, mouse_pos, col, 2.5f, canvas_zoom_);
                }
                goto done_drag;
            }
        }
        done_drag:;
    }

    // --- Draw nodes ---
    auto hovered_pin = hit_test_pin(mouse_pos, canvas_origin);
    for (auto& node : graph_.nodes) {
        if (node.imported || node.shadow) continue;
        draw_node(dl, node, canvas_origin);
    }

    // Pin hover highlight
    if (!hovered_pin.pin_id.empty()) {
        for (auto& node : graph_.nodes) {
            PinShape io_shape = PinShape::Signal;
            float pr = PIN_RADIUS * canvas_zoom_;
            auto check = [&](auto& pins, PinShape shape) {
                for (auto& pin : pins)
                    if (pin->id == hovered_pin.pin_id) {
                        ImVec2 pp = get_pin_pos(node, *pin, canvas_origin);
                        draw_pin_highlight(dl, pp, pr, COL_PIN_HOVER, shape, canvas_zoom_);
                    }
            };
            check(node.triggers, PinShape::Square);
            for (auto& pin : node.inputs)
                if (pin->id == hovered_pin.pin_id) {
                    ImVec2 pp = get_pin_pos(node, *pin, canvas_origin);
                    PinShape shape = (pin->direction == FlowPin::Lambda) ? PinShape::LambdaDown : io_shape;
                    draw_pin_highlight(dl, pp, pr, COL_PIN_HOVER, shape, canvas_zoom_);
                }
            check(node.nexts, PinShape::Square);
            check(node.outputs, io_shape);
            if (node.lambda_grab.id == hovered_pin.pin_id) {
                ImVec2 pp = get_pin_pos(node, node.lambda_grab, canvas_origin);
                draw_pin_highlight(dl, pp, pr, COL_PIN_HOVER, PinShape::LambdaLeft, canvas_zoom_);
            }
        }
    }

    // --- Tooltips ---
    if (canvas_hovered && editing_node_ < 0 && editing_link_ < 0) {
        if (!hovered_pin.pin_id.empty()) {
            for (auto& node : graph_.nodes) {
                if (node.id != hovered_pin.node_id) continue;
                auto find_pin = [&](auto& pins) -> const FlowPin* {
                    for (auto& p : pins) if (p->id == hovered_pin.pin_id) return p.get();
                    return nullptr;
                };
                const FlowPin* pin = find_pin(node.triggers);
                if (!pin) pin = find_pin(node.inputs);
                if (!pin) pin = find_pin(node.outputs);
                if (!pin) pin = find_pin(node.nexts);
                if (!pin && node.lambda_grab.id == hovered_pin.pin_id) pin = &node.lambda_grab;
                if (!pin && node.bang_pin.id == hovered_pin.pin_id) pin = &node.bang_pin;
                if (pin) {
                    auto [port_name, port_desc] = get_port_desc(node, *pin);
                    std::string type_str;
                    if (pin->resolved_type)
                        type_str = type_to_string(pin->resolved_type);
                    else if (pin->direction == FlowPin::BangTrigger || pin->direction == FlowPin::BangNext)
                        type_str = "bang";
                    else
                        type_str = "?";
                    ImGui::BeginTooltip();
                    ImGui::SetWindowFontScale(canvas_zoom_);
                    ImGui::TextUnformatted((port_name + " : " + type_str).c_str());
                    if (!port_desc.empty())
                        ImGui::TextDisabled("%s", port_desc.c_str());
                    ImGui::EndTooltip();
                }
                break;
            }
        } else {
            int lid = hit_test_link(mouse_pos, canvas_origin);
            if (lid >= 0) {
                for (auto& link : graph_.links) {
                    if (link.id != lid) continue;
                    std::string from_label, to_label;
                    for (auto& n : graph_.nodes) {
                        for (auto& p : n.outputs) if (p->id == link.from_pin) from_label = pin_label(n, *p);
                        for (auto& p : n.nexts) if (p->id == link.from_pin) from_label = pin_label(n, *p);
                        for (auto& p : n.triggers) if (p->id == link.from_pin) from_label = pin_label(n, *p);
                        if (n.lambda_grab.id == link.from_pin) from_label = pin_label(n, n.lambda_grab);
                        if (n.bang_pin.id == link.from_pin) from_label = pin_label(n, n.bang_pin);
                        for (auto& p : n.inputs) if (p->id == link.to_pin) to_label = pin_label(n, *p);
                        for (auto& p : n.triggers) if (p->id == link.to_pin) to_label = pin_label(n, *p);
                    }
                    if (!from_label.empty() && !to_label.empty()) {
                        auto* fp = graph_.find_pin(link.from_pin);
                        auto* tp = graph_.find_pin(link.to_pin);
                        std::string from_type_str = (fp && fp->resolved_type) ? type_to_string(fp->resolved_type) : "?";
                        std::string to_type_str = (tp && tp->resolved_type) ? type_to_string(tp->resolved_type) : "?";
                        bool type_err = !link.error.empty();
                        if (!type_err && fp && tp && fp->resolved_type && tp->resolved_type &&
                            !fp->resolved_type->is_generic && !tp->resolved_type->is_generic)
                            type_err = !types_compatible(fp->resolved_type, tp->resolved_type);

                        ImGui::BeginTooltip();
                        ImGui::SetWindowFontScale(canvas_zoom_);
                        if (!link.net_name.empty()) {
                            ImGui::TextColored({0.7f, 0.9f, 1.0f, 1.0f}, "%s", link.net_name.c_str());
                        }
                        ImGui::TextUnformatted((from_label + " -> " + to_label).c_str());
                        ImGui::TextDisabled("%s -> %s", from_type_str.c_str(), to_type_str.c_str());
                        if (!link.error.empty())
                            ImGui::TextColored({1.0f, 0.2f, 0.2f, 1.0f}, "%s", link.error.c_str());
                        else if (type_err)
                            ImGui::TextColored({1.0f, 0.2f, 0.2f, 1.0f}, "Type mismatch!");
                        ImGui::TextDisabled("Click to rename wire");
                        ImGui::EndTooltip();

                        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                            editing_link_ = link.id;
                            link_edit_buf_ = link.net_name.empty() ? "$" : link.net_name;
                            link_edit_just_opened_ = true;
                        }
                    }
                    break;
                }
            } else {
                ImVec2 mc = screen_to_canvas(mouse_pos, canvas_origin);
                for (int i = (int)graph_.nodes.size() - 1; i >= 0; i--) {
                    auto& node = graph_.nodes[i];
                    if (mc.x >= node.position.x && mc.x <= node.position.x + node.size.x &&
                        mc.y >= node.position.y && mc.y <= node.position.y + node.size.y) {
                        auto* nt = find_node_type(node.type_id);
                        ImGui::BeginTooltip();
                        ImGui::SetWindowFontScale(canvas_zoom_);
                        ImGui::TextUnformatted(node_display_name(node).c_str());
                        if (nt && nt->desc)
                            ImGui::TextDisabled("%s", nt->desc);
                        if (!node.error.empty()) {
                            ImGui::Separator();
                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 100, 100, 255));
                            ImGui::TextUnformatted("Errors:");
                            ImGui::TextUnformatted(node.error.c_str());
                            ImGui::PopStyleColor();
                        }
                        ImGui::TextDisabled("(%s)", node.guid.c_str());
                        ImGui::EndTooltip();
                        break;
                    }
                }
            }
        }
    }

    // --- Name editing: inline inside the node ---
    if (editing_node_ >= 0) {
        FlowNode* edit_node = nullptr;
        for (auto& node : graph_.nodes) {
            if (node.id == editing_node_) { edit_node = &node; break; }
        }
        ImVec2 edit_pos = edit_node ? to_imvec(edit_node->position) : new_node_pos_;
        ImVec2 edit_size = edit_node ? to_imvec(edit_node->size) : ImVec2{NODE_MIN_WIDTH, NODE_HEIGHT};

        {
            ImVec2 tl = canvas_to_screen(edit_pos, canvas_origin);
            ImVec2 br = canvas_to_screen({edit_pos.x + edit_size.x,
                                           edit_pos.y + edit_size.y}, canvas_origin);
            float nw = br.x - tl.x;

            float text_w = ImGui::CalcTextSize(edit_buf_.c_str()).x * canvas_zoom_ + 40.0f * canvas_zoom_;
            float scaled_min_w = std::max({nw, 160.0f * canvas_zoom_, text_w});
            ImGui::SetNextWindowPos(tl);
            ImGui::SetNextWindowSize({scaled_min_w, 0});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {2 * canvas_zoom_, 2 * canvas_zoom_});
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4 * canvas_zoom_, 2 * canvas_zoom_});
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4 * canvas_zoom_, 2 * canvas_zoom_});
            ImGui::Begin("##name_edit", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
            ImGui::SetWindowFontScale(canvas_zoom_);

            if (edit_just_opened_) {
                ImGui::SetKeyboardFocusHere();
                edit_just_opened_ = false;
            }

            char buf[128];
            strncpy(buf, edit_buf_.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';

            bool* cursor_to_end_ptr = &edit_cursor_to_end_;
            auto edit_callback = [](ImGuiInputTextCallbackData* data) -> int {
                bool* flag = (bool*)data->UserData;
                if (*flag) {
                    data->CursorPos = data->BufTextLen;
                    data->SelectionStart = data->SelectionEnd = data->CursorPos;
                    *flag = false;
                }
                return 0;
            };

            ImGui::SetNextItemWidth(-1);
            bool committed = ImGui::InputText("##edit", buf, sizeof(buf),
                                               ImGuiInputTextFlags_EnterReturnsTrue |
                                               ImGuiInputTextFlags_CallbackAlways,
                                               edit_callback, cursor_to_end_ptr);
            edit_buf_ = buf;

            std::string first_word = edit_buf_;
            std::string rest_args;
            auto space_pos = edit_buf_.find(' ');
            if (space_pos != std::string::npos) {
                first_word = edit_buf_.substr(0, space_pos);
                rest_args = edit_buf_.substr(space_pos + 1);
            }

            if (space_pos == std::string::npos) {
                for (int i = 0; i < NUM_NODE_TYPES; i++) {
                    std::string nt_name(NODE_TYPES[i].name);
                    if (first_word.empty() || (nt_name.find(first_word) != std::string::npos && nt_name != first_word)) {
                        if (ImGui::Selectable(NODE_TYPES[i].name)) {
                            edit_buf_ = nt_name + " ";
                            edit_just_opened_ = true;
                            edit_cursor_to_end_ = true;
                        }
                    }
                }
            }

            if (committed) do {
                std::string first_word, rest_args;
                auto sp = edit_buf_.find(' ');
                if (sp != std::string::npos) {
                    first_word = edit_buf_.substr(0, sp);
                    rest_args = edit_buf_.substr(sp + 1);
                } else {
                    first_word = edit_buf_;
                }

                std::string node_type = first_word;
                if (node_type.empty()) break;

                if (creating_new_node_ && !edit_node) {
                    int id = graph_.add_node("", to_vec2(new_node_pos_), 0, 0);
                    for (auto& n : graph_.nodes) {
                        if (n.id == id) { edit_node = &n; break; }
                    }
                    editing_node_ = id;
                }
                if (!edit_node) break;

                auto* nt = find_node_type(node_type.c_str());
                if (!nt) {
                    nt = find_node_type("expr");
                    node_type = "expr";
                    rest_args = edit_buf_;
                }
                int default_triggers = nt ? nt->num_triggers : 0;
                int default_inputs = nt ? nt->inputs : 0;
                int default_outputs = nt ? nt->outputs : 0;
                int default_nexts = nt ? nt->num_nexts : 0;

                auto& node = *edit_node;
                if (node.guid.empty())
                    node.guid = generate_guid();
                node.type_id = node_type_id_from_string(node_type.c_str());
                node.args = rest_args;
                node.parse_args();
                graph_.dirty = true;
                creating_new_node_ = false;

                auto resize_pins = [&](PinVec& pins, int needed,
                                       const std::vector<std::string>& names,
                                       FlowPin::Direction dir, bool is_output) {
                    for (int i = 0; i < std::min((int)pins.size(), needed); i++)
                        pins[i]->name = names[i];
                    for (int i = (int)pins.size(); i < needed; i++)
                        pins.push_back(make_pin("", names[i], "", nullptr, dir));
                    while ((int)pins.size() > needed) {
                        auto pid = pins.back()->id;
                        if (is_output)
                            std::erase_if(graph_.links, [&pid](auto& l) { return l.from_pin == pid; });
                        else
                            std::erase_if(graph_.links, [&pid](auto& l) { return l.to_pin == pid; });
                        pins.pop_back();
                    }
                };

                auto make_names = [](const std::string& prefix, int count) {
                    std::vector<std::string> names;
                    for (int i = 0; i < count; i++) names.push_back(prefix + std::to_string(i));
                    return names;
                };

                int needed_outputs = default_outputs;
                bool is_expr_type = is_any_of(node.type_id, NodeTypeID::Expr, NodeTypeID::ExprBang);

                struct DesiredPin { std::string name; FlowPin::Direction dir; };
                std::vector<DesiredPin> desired_inputs;

                if (node.type_id == NodeTypeID::New) {
                    auto tokens = tokenize_args(rest_args, false);
                    std::string inst_type_name = tokens.empty() ? "" : tokens[0];
                    auto* type_node = find_type_node(graph_, inst_type_name);
                    if (type_node) {
                        auto fields = parse_type_fields(*type_node);
                        for (auto& field : fields)
                            desired_inputs.push_back({field.name, FlowPin::Input});
                    }
                    needed_outputs = 1;
                } else if (node.type_id == NodeTypeID::EventBang) {
                    auto tokens = tokenize_args(rest_args, false);
                    std::string event_name = tokens.empty() ? "" : tokens[0];
                    auto* event_decl = find_event_node(graph_, event_name);
                    if (event_decl) {
                        auto args = parse_event_args(*event_decl, graph_);
                        std::vector<std::string> out_names;
                        for (auto& a : args) out_names.push_back(a.name);
                        needed_outputs = (int)out_names.size();
                        for (int i = 0; i < std::min((int)node.outputs.size(), needed_outputs); i++)
                            node.outputs[i]->name = out_names[i];
                        for (int i = (int)node.outputs.size(); i < needed_outputs; i++)
                            node.outputs.push_back(make_pin("", out_names[i], "", nullptr, FlowPin::Output));
                        while ((int)node.outputs.size() > needed_outputs) {
                            auto pid = node.outputs.back()->id;
                            std::erase_if(graph_.links, [&pid](auto& l) { return l.from_pin == pid; });
                            node.outputs.pop_back();
                        }
                        needed_outputs = -1;
                    }
                } else {
                    if (is_expr_type) {
                        auto parsed = scan_slots(rest_args);
                        int total_top = parsed.total_pin_count(default_inputs);
                        for (int i = 0; i < total_top; i++) {
                            bool is_lambda = parsed.is_lambda_slot(i);
                            std::string pin_name = is_lambda ? ("@" + std::to_string(i)) : std::to_string(i);
                            desired_inputs.push_back({pin_name, is_lambda ? FlowPin::Lambda : FlowPin::Input});
                        }
                        if (!node.args.empty()) {
                            auto tokens = tokenize_args(rest_args, false);
                            needed_outputs = std::max(1, (int)tokens.size());
                        }
                    } else if (node_type == "cast" || node_type == "new") {
                        for (int i = 0; i < default_inputs; i++) {
                            std::string pin_name;
                            bool is_lambda = false;
                            if (nt && nt->input_ports && i < nt->inputs) {
                                pin_name = nt->input_ports[i].name;
                                is_lambda = (nt->input_ports[i].kind == PortKind::Lambda);
                            } else {
                                pin_name = std::to_string(i);
                            }
                            desired_inputs.push_back({pin_name, is_lambda ? FlowPin::Lambda : FlowPin::Input});
                        }
                    } else {
                        auto info = compute_inline_args(rest_args, default_inputs);
                        if (!info.error.empty()) node.error = info.error;
                        int ref_pins = (info.pin_slots.max_slot >= 0) ? (info.pin_slots.max_slot + 1) : 0;
                        for (int i = 0; i < ref_pins; i++) {
                            bool is_lambda = info.pin_slots.is_lambda_slot(i);
                            std::string pin_name = is_lambda ? ("@" + std::to_string(i)) : std::to_string(i);
                            desired_inputs.push_back({pin_name, is_lambda ? FlowPin::Lambda : FlowPin::Input});
                        }
                        for (int i = info.num_inline_args; i < default_inputs; i++) {
                            std::string pin_name;
                            bool is_lambda = false;
                            if (nt && nt->input_ports && i < nt->inputs) {
                                pin_name = nt->input_ports[i].name;
                                is_lambda = (nt->input_ports[i].kind == PortKind::Lambda);
                            } else {
                                pin_name = std::to_string(i);
                            }
                            desired_inputs.push_back({pin_name, is_lambda ? FlowPin::Lambda : FlowPin::Input});
                        }
                    }
                }

                // Resize inputs (unified data + lambda), preserving connections
                {
                    int needed = (int)desired_inputs.size();
                    for (int i = 0; i < std::min((int)node.inputs.size(), needed); i++) {
                        node.inputs[i]->name = desired_inputs[i].name;
                        node.inputs[i]->direction = desired_inputs[i].dir;
                    }
                    for (int i = (int)node.inputs.size(); i < needed; i++)
                        node.inputs.push_back(make_pin("", desired_inputs[i].name, "", nullptr, desired_inputs[i].dir));
                    while ((int)node.inputs.size() > needed) {
                        auto pid = node.inputs.back()->id;
                        std::erase_if(graph_.links, [&pid](auto& l) { return l.to_pin == pid; });
                        node.inputs.pop_back();
                    }
                }

                resize_pins(node.triggers, default_triggers,
                            make_names("bang_in", default_triggers), FlowPin::BangTrigger, false);
                if (needed_outputs >= 0)
                    resize_pins(node.outputs, needed_outputs,
                                make_names("out", needed_outputs), FlowPin::Output, true);
                resize_pins(node.nexts, default_nexts,
                            make_names("bang", default_nexts), FlowPin::BangNext, true);

                auto update_pin_ids = [&](PinVec& pins) {
                    for (auto& p : pins) {
                        std::string new_id = node.pin_id(p->name);
                        if (p->id != new_id) {
                            for (auto& l : graph_.links) {
                                if (l.from_pin == p->id) l.from_pin = new_id;
                                if (l.to_pin == p->id) l.to_pin = new_id;
                            }
                            p->id = new_id;
                        }
                    }
                };
                update_pin_ids(node.triggers);
                update_pin_ids(node.inputs);
                update_pin_ids(node.outputs);
                update_pin_ids(node.nexts);
                {
                    std::string new_id = node.pin_id("as_lambda");
                    for (auto& l : graph_.links) {
                        if (l.from_pin == node.lambda_grab.id) l.from_pin = new_id;
                        if (l.to_pin == node.lambda_grab.id) l.to_pin = new_id;
                    }
                    node.lambda_grab.id = new_id;
                }
                {
                    std::string new_id = node.pin_id("post_bang");
                    for (auto& l : graph_.links) {
                        if (l.from_pin == node.bang_pin.id) l.from_pin = new_id;
                        if (l.to_pin == node.bang_pin.id) l.to_pin = new_id;
                    }
                    node.bang_pin.id = new_id;
                }

                update_shadows_for_node(graph_, node, rest_args);

                editing_node_ = -1;
                mark_dirty();
            } while (false);

            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                if (creating_new_node_ && edit_node) {
                    graph_.remove_node(editing_node_);
                }
                creating_new_node_ = false;
                editing_node_ = -1;
            }

            ImGui::End();
            ImGui::PopStyleVar(3);
        } // end of edit window block
    }

    // --- Wire name editing popup ---
    if (editing_link_ >= 0) {
        FlowLink* edit_link = nullptr;
        for (auto& link : graph_.links) {
            if (link.id == editing_link_) { edit_link = &link; break; }
        }
        if (!edit_link) {
            editing_link_ = -1;
        } else {
            ImVec2 fp = {}, tp = {};
            for (auto& n : graph_.nodes) {
                for (auto& p : n.outputs) if (p->id == edit_link->from_pin) fp = get_pin_pos(n, *p, canvas_origin);
                for (auto& p : n.nexts) if (p->id == edit_link->from_pin) fp = get_pin_pos(n, *p, canvas_origin);
                if (n.lambda_grab.id == edit_link->from_pin) fp = get_pin_pos(n, n.lambda_grab, canvas_origin);
                if (n.bang_pin.id == edit_link->from_pin) fp = get_pin_pos(n, n.bang_pin, canvas_origin);
                for (auto& p : n.inputs) if (p->id == edit_link->to_pin) tp = get_pin_pos(n, *p, canvas_origin);
                for (auto& p : n.triggers) if (p->id == edit_link->to_pin) tp = get_pin_pos(n, *p, canvas_origin);
            }
            ImVec2 mid = {(fp.x + tp.x) * 0.5f, (fp.y + tp.y) * 0.5f};

            float text_w = ImGui::CalcTextSize(link_edit_buf_.c_str()).x * canvas_zoom_ + 40.0f * canvas_zoom_;
            float popup_w = std::max(200.0f * canvas_zoom_, text_w);
            ImGui::SetNextWindowPos({mid.x - popup_w * 0.5f, mid.y - 15.0f * canvas_zoom_});
            ImGui::SetNextWindowSize({popup_w, 0});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {4 * canvas_zoom_, 4 * canvas_zoom_});
            ImGui::Begin("##wire_rename", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
            ImGui::SetWindowFontScale(canvas_zoom_);

            bool was_just_opened = link_edit_just_opened_;
            if (link_edit_just_opened_) {
                ImGui::SetKeyboardFocusHere();
                link_edit_just_opened_ = false;
            }

            char buf[128];
            strncpy(buf, link_edit_buf_.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';

            bool committed = ImGui::InputText("##wire_name", buf, sizeof(buf),
                                               ImGuiInputTextFlags_EnterReturnsTrue);
            link_edit_buf_ = buf;

            bool valid = true;
            std::string error_msg;
            std::string new_name = link_edit_buf_;
            if (new_name.empty() || new_name[0] != '$') {
                valid = false;
                error_msg = "Must start with $";
            } else if (new_name.size() < 2) {
                valid = false;
                error_msg = "Name too short";
            } else {
                for (auto& other : graph_.links) {
                    if (other.id == edit_link->id) continue;
                    if (other.net_name == new_name && other.from_pin != edit_link->from_pin) {
                        valid = false;
                        error_msg = "Name already in use";
                        break;
                    }
                }
            }

            if (!valid && !error_msg.empty()) {
                ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "%s", error_msg.c_str());
            }

            if (committed && valid) {
                std::string old_from = edit_link->from_pin;
                for (auto& link : graph_.links) {
                    if (link.from_pin == old_from) {
                        link.net_name = new_name;
                        link.auto_wire = false;
                    }
                }
                editing_link_ = -1;
                rebuild_all_inline_display(graph_);
                mark_dirty();
            }

            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                editing_link_ = -1;
            }

            if (!was_just_opened &&
                !ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                editing_link_ = -1;
            }

            ImGui::End();
            ImGui::PopStyleVar(1);
        }
    }
}
