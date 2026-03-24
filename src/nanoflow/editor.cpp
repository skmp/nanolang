#include "editor.h"
#include "nano/args.h"
#include "nano/expr.h"
#include "nano/inference.h"
#include "nano/serial.h"
#include "nano/types.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <functional>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#ifndef _WIN32
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

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

#include "nano/node_types.h"

// Look up port description for a pin on a node.
// Returns {port_name, port_desc} or {"", ""} if not found.
static std::pair<std::string, std::string> get_port_desc(const FlowNode& node, const FlowPin& pin) {
    auto* nt = find_node_type(node.type_id);
    if (!nt) return {pin.name, ""};

    auto find_in = [&](const auto& pins, const PortDesc* descs, int count) -> std::pair<std::string, std::string> {
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

    auto r = find_in(node.bang_inputs, nt->bang_input_ports, nt->bang_inputs);
    if (!r.first.empty()) return r;
    r = find_in(node.inputs, nt->input_ports, nt->inputs);
    if (!r.first.empty()) return r;
    r = find_in(node.bang_outputs, nt->bang_output_ports, nt->bang_outputs);
    if (!r.first.empty()) return r;
    r = find_in(node.outputs, nt->output_ports, nt->outputs);
    if (!r.first.empty()) return r;
    if (node.lambda_grab.id == pin.id) return {"as_lambda", "pass as lambda"};
    if (node.bang_pin.id == pin.id) return {"bang", "bang connector"};
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

#include "nano/type_utils.h"

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
        // Down-pointing triangle (for lambda inputs on top)
        dl->AddTriangleFilled(
            {pos.x - r, pos.y - r},
            {pos.x + r, pos.y - r},
            {pos.x, pos.y + r},
            col);
        break;
    case PinShape::LambdaLeft:
        // Left-pointing triangle (for lambda grab on left)
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

bool FlowEditorWindow::init(const std::string& project_dir) {
    if (!win_.init("Flow Editor", 900, 600)) return false;
    project_dir_ = project_dir;

    if (!project_dir_.empty()) {
        scan_project_files();
        // Open main.nano as the first tab
        namespace fs = std::filesystem;
        std::string main_path = (fs::path(project_dir_) / "main.nano").string();
        if (fs::exists(main_path)) {
            open_tab(main_path);
        } else if (!project_files_.empty()) {
            open_tab((fs::path(project_dir_) / project_files_[0]).string());
        }
    }

    // Ensure at least one tab exists
    if (tabs_.empty()) {
        tabs_.push_back({});
        tabs_.back().tab_name = "untitled";
    }

    return true;
}

void FlowEditorWindow::scan_project_files() {
    namespace fs = std::filesystem;
    project_files_.clear();
    if (project_dir_.empty()) return;
    for (auto& entry : fs::directory_iterator(project_dir_)) {
        if (entry.path().extension() == ".nano") {
            project_files_.push_back(entry.path().filename().string());
        }
    }
    std::sort(project_files_.begin(), project_files_.end());
}

void FlowEditorWindow::open_tab(const std::string& file_path) {
    namespace fs = std::filesystem;
    std::string abs_path = fs::absolute(file_path).string();

    // Check if already open
    for (int i = 0; i < (int)tabs_.size(); i++) {
        if (tabs_[i].file_path == abs_path) {
            active_tab_ = i;
            return;
        }
    }

    // Create new tab
    TabState tab;
    tab.file_path = abs_path;
    tab.tab_name = fs::path(file_path).stem().string();
    if (fs::exists(abs_path)) {
        load_nano(abs_path, tab.graph);
    }
    if (tab.graph.has_viewport) {
        tab.canvas_offset = {tab.graph.viewport_x, tab.graph.viewport_y};
        tab.canvas_zoom = tab.graph.viewport_zoom;
    }
    tab.inference_dirty = true;
    tabs_.push_back(std::move(tab));
    active_tab_ = (int)tabs_.size() - 1;
}

void FlowEditorWindow::close_tab(int idx) {
    if (idx < 0 || idx >= (int)tabs_.size()) return;
    // Auto-save before closing
    if (tabs_[idx].dirty && !tabs_[idx].file_path.empty()) {
        sync_viewport(tabs_[idx]);
        save_nano(tabs_[idx].file_path, tabs_[idx].graph);
    }
    tabs_.erase(tabs_.begin() + idx);
    if (active_tab_ >= (int)tabs_.size())
        active_tab_ = std::max(0, (int)tabs_.size() - 1);
    // Ensure at least one tab
    if (tabs_.empty()) {
        tabs_.push_back({});
        tabs_.back().tab_name = "untitled";
    }
}

void FlowEditorWindow::mark_dirty() {
    push_undo();
    active().dirty = true;
    active().inference_dirty = true;
    schedule_save();
}

void FlowEditorWindow::push_undo() {
    active().undo_stack.push_back(save_nano_string(active().graph));
    active().redo_stack.clear();
    // Limit undo history
    if (active().undo_stack.size() > 200) active().undo_stack.erase(active().undo_stack.begin());
}

void FlowEditorWindow::undo() {
    if (active().undo_stack.empty()) return;
    // Save current state to redo
    active().redo_stack.push_back(save_nano_string(active().graph));
    // Restore from undo
    load_nano_string(active().undo_stack.back(), active().graph);
    active().undo_stack.pop_back();
    active().dirty = true;
}

void FlowEditorWindow::redo() {
    if (active().redo_stack.empty()) return;
    // Save current state to undo (without clearing redo)
    active().undo_stack.push_back(save_nano_string(active().graph));
    // Restore from redo
    load_nano_string(active().redo_stack.back(), active().graph);
    active().redo_stack.pop_back();
    active().dirty = true;
}

void FlowEditorWindow::schedule_save() {
    active().dirty = true;
    save_deadline_ = ImGui::GetTime() + 0.5; // 500ms debounce
}

void FlowEditorWindow::check_debounced_save() {
    if (save_deadline_ > 0 && ImGui::GetTime() >= save_deadline_) {
        save_deadline_ = 0;
        auto_save();
    }
}

void FlowEditorWindow::sync_viewport(TabState& tab) {
    tab.graph.viewport_x = tab.canvas_offset.x;
    tab.graph.viewport_y = tab.canvas_offset.y;
    tab.graph.viewport_zoom = tab.canvas_zoom;
}

void FlowEditorWindow::auto_save() {
    if (active().dirty && !active().file_path.empty()) {
        sync_viewport(active());
        save_nano(active().file_path, active().graph);
        active().dirty = false;
    }
}

void FlowEditorWindow::shutdown() {
    stop_program();
    if (build_thread_.joinable())
        build_thread_.join();
    win_.shutdown();
}
void FlowEditorWindow::process_event(SDL_Event& e) { win_.process_event(e); }

ImVec2 FlowEditorWindow::canvas_to_screen(ImVec2 p, ImVec2 origin) const {
    return {origin.x + (p.x + active().canvas_offset.x) * active().canvas_zoom,
            origin.y + (p.y + active().canvas_offset.y) * active().canvas_zoom};
}

ImVec2 FlowEditorWindow::screen_to_canvas(ImVec2 p, ImVec2 origin) const {
    return {(p.x - origin.x) / active().canvas_zoom - active().canvas_offset.x,
            (p.y - origin.y) / active().canvas_zoom - active().canvas_offset.y};
}

ImVec2 FlowEditorWindow::get_pin_pos(const FlowNode& node, const FlowPin& pin, ImVec2 origin) const {
    if (pin.direction == FlowPin::LambdaGrab) {
        // Grab handle: middle-left
        float x = node.position.x;
        float y = node.position.y + node.size.y * 0.5f;
        return canvas_to_screen({x, y}, origin);
    }

    // Bang pin: middle-right
    if (pin.id == node.bang_pin.id && pin.name == "bang") {
        float x = node.position.x + node.size.x;
        float y = node.position.y + node.size.y * 0.5f;
        return canvas_to_screen({x, y}, origin);
    }

    // Bang inputs first on top
    if (pin.direction == FlowPin::BangInput) {
        int idx = 0;
        for (auto& p : node.bang_inputs) { if (p->id == pin.id) break; idx++; }
        float x = node.position.x + PIN_SPACING * (idx + 0.5f);
        float y = node.position.y;
        return canvas_to_screen({x, y}, origin);
    }

    if (pin.direction == FlowPin::Input || pin.direction == FlowPin::Lambda) {
        // Data inputs and lambdas after bang inputs on the top row.
        int bang_offset = (int)node.bang_inputs.size();
        int slot = 0;
        for (auto& p : node.inputs) { if (p->id == pin.id) break; slot++; }
        float x = node.position.x + PIN_SPACING * (bang_offset + slot + 0.5f);
        float y = node.position.y;
        return canvas_to_screen({x, y}, origin);
    }

    // Bang outputs first on bottom, then data outputs
    if (pin.direction == FlowPin::BangOutput) {
        int idx = 0;
        for (auto& p : node.bang_outputs) { if (p->id == pin.id) break; idx++; }
        float x = node.position.x + PIN_SPACING * (idx + 0.5f);
        float y = node.position.y + node.size.y;
        return canvas_to_screen({x, y}, origin);
    }

    // Data outputs after bang outputs
    int offset = (int)node.bang_outputs.size();
    int idx = 0;
    for (auto& p : node.outputs) { if (p->id == pin.id) break; idx++; }
    float x = node.position.x + PIN_SPACING * (offset + idx + 0.5f);
    float y = node.position.y + node.size.y;
    return canvas_to_screen({x, y}, origin);
}

FlowEditorWindow::PinHit FlowEditorWindow::hit_test_pin(ImVec2 sp, ImVec2 co, float radius) const {
    float r2 = radius * radius * active().canvas_zoom * active().canvas_zoom;
    for (auto& node : active().graph.nodes) {
        for (auto& pin : node.bang_inputs)
            if (dist2(sp, get_pin_pos(node, *pin, co)) < r2)
                return {node.id, pin->id, FlowPin::BangInput};
        for (auto& pin : node.inputs)
            if (dist2(sp, get_pin_pos(node, *pin, co)) < r2)
                return {node.id, pin->id, pin->direction};
        for (auto& pin : node.outputs)
            if (dist2(sp, get_pin_pos(node, *pin, co)) < r2)
                return {node.id, pin->id, FlowPin::Output};
        for (auto& pin : node.bang_outputs)
            if (dist2(sp, get_pin_pos(node, *pin, co)) < r2)
                return {node.id, pin->id, FlowPin::BangOutput};
        if (!node.lambda_grab.id.empty() && dist2(sp, get_pin_pos(node, node.lambda_grab, co)) < r2) {
            auto* nt_hit = find_node_type(node.type_id);
            if (nt_hit && nt_hit->has_lambda)
                return {node.id, node.lambda_grab.id, FlowPin::LambdaGrab};
        }
        if (!node.bang_pin.id.empty() && dist2(sp, get_pin_pos(node, node.bang_pin, co)) < r2) {
            auto* nt_hit = find_node_type(node.type_id);
            bool hidden = (nt_hit && (nt_hit->is_event || nt_hit->no_post_bang));
            if (!hidden) return {node.id, node.bang_pin.id, FlowPin::BangOutput};
        }
    }
    return {-1, "", FlowPin::Input};
}

int FlowEditorWindow::hit_test_link(ImVec2 sp, ImVec2 co, float threshold) const {
    for (auto& link : active().graph.links) {
        ImVec2 fp = {}, tp = {};
        bool ff = false, ft = false;
        bool from_grab = false, from_bang_pin = false, to_lambda = false;
        for (auto& n : active().graph.nodes) {
            for (auto& p : n.outputs) if (p->id == link.from_pin) { fp = get_pin_pos(n, *p, co); ff = true; }
            for (auto& p : n.bang_outputs) if (p->id == link.from_pin) { fp = get_pin_pos(n, *p, co); ff = true; }
            if (n.lambda_grab.id == link.from_pin) { fp = get_pin_pos(n, n.lambda_grab, co); ff = true; from_grab = true; }
            for (auto& p : n.bang_inputs) if (p->id == link.to_pin) { tp = get_pin_pos(n, *p, co); ft = true; }
            for (auto& p : n.inputs) if (p->id == link.to_pin) { tp = get_pin_pos(n, *p, co); ft = true; if (p->direction == FlowPin::Lambda) to_lambda = true; }
            if (n.bang_pin.id == link.from_pin) { fp = get_pin_pos(n, n.bang_pin, co); ff = true; from_bang_pin = true; }
        }
        if (!ff || !ft) continue;
        // Use the same curve shape as draw_link for accurate hit testing
        float d;
        if (from_grab) {
            float dx = std::max(std::abs(tp.x - fp.x) * 0.5f, 30.0f * active().canvas_zoom);
            float dy = std::max(std::abs(tp.y - fp.y) * 0.5f, 30.0f * active().canvas_zoom);
            d = point_to_bezier_dist(sp, fp, {fp.x - dx, fp.y}, {tp.x, tp.y - dy}, tp);
        } else if (from_bang_pin) {
            float dx = std::max(std::abs(tp.x - fp.x) * 0.5f, 30.0f * active().canvas_zoom);
            float dy_hit = std::max(std::abs(tp.y - fp.y) * 0.5f, 30.0f * active().canvas_zoom);
            d = point_to_bezier_dist(sp, fp, {fp.x + dx, fp.y}, {tp.x, tp.y - dy_hit}, tp);
        } else {
            float dy = std::max(std::abs(tp.y - fp.y) * 0.5f, 30.0f * active().canvas_zoom);
            d = point_to_bezier_dist(sp, fp, {fp.x, fp.y + dy}, {tp.x, tp.y - dy}, tp);
        }
        if (d < threshold * active().canvas_zoom) return link.id;
    }
    return -1;
}

void FlowEditorWindow::draw_node(ImDrawList* dl, FlowNode& node, ImVec2 origin) {
    bool is_label = (node.type_id == NodeTypeID::Label);

    // Width from pins (top row = inputs + lambdas, bottom row = outputs)
    int top_pins = (int)(node.bang_inputs.size() + node.inputs.size());
    int bottom_pins = (int)(node.bang_outputs.size() + node.outputs.size());
    int max_pins = std::max(top_pins, bottom_pins);
    float pin_w = (float)(max_pins + 1) * PIN_SPACING;

    // Width from display text
    std::string display_text;
    if (is_label) {
        display_text = node.args.empty() ? "(label)" : node.args;
    } else {
        display_text = node.display_text();
    }
    float font_scale = 17.0f / ImGui::GetFontSize();
    ImVec2 ts = ImGui::CalcTextSize(display_text.c_str());
    float text_w = ts.x * font_scale + 16.0f; // padding

    float needed_w = std::max({pin_w, text_w, NODE_MIN_WIDTH});
    node.size = {needed_w, NODE_HEIGHT};

    ImVec2 tl = canvas_to_screen(to_imvec(node.position), origin);
    ImVec2 br = canvas_to_screen({node.position.x + node.size.x,
                                   node.position.y + node.size.y}, origin);

    if (is_label) {
        // Labels: no background box, just text
        float font_size = 17.0f * active().canvas_zoom;
        if (font_size > 6.0f && editing_node_ != node.id) {
            const char* display = node.args.empty() ? "(label)" : node.args.c_str();
            ImU32 col = node.args.empty() ? IM_COL32(100, 100, 100, 180) : IM_COL32(255, 255, 255, 255);
            dl->AddText(nullptr, font_size,
                        {tl.x + 2 * active().canvas_zoom, tl.y + (br.y - tl.y - font_size) * 0.5f},
                        col, display);
        }
    } else {
        // Normal node: filled bar (red if error)
        ImU32 bg = node.error.empty() ? COL_NODE_BG : IM_COL32(120, 30, 30, 230);
        ImU32 border = node.error.empty() ? IM_COL32(100, 100, 150, 255) : IM_COL32(200, 60, 60, 255);
        dl->AddRectFilled(tl, br, bg, NODE_ROUNDING * active().canvas_zoom);

        // Highlight animation: blink dark yellow overlay
        if (active().highlight_node_id == node.id && active().highlight_timer > 0.0f) {
            float blink = std::sin(active().highlight_timer * 6.0f) * 0.5f + 0.5f;
            int a = (int)(blink * 140.0f);
            dl->AddRectFilled(tl, br, IM_COL32(180, 160, 40, a), NODE_ROUNDING * active().canvas_zoom);
        }

        dl->AddRect(tl, br, border, NODE_ROUNDING * active().canvas_zoom);

        // Selection highlight
        if (active().selected_nodes.count(node.id)) {
            dl->AddRect({tl.x - 2*active().canvas_zoom, tl.y - 2*active().canvas_zoom},
                        {br.x + 2*active().canvas_zoom, br.y + 2*active().canvas_zoom},
                        IM_COL32(100, 180, 255, 200), NODE_ROUNDING * active().canvas_zoom, 0, 2.0f * active().canvas_zoom);
        }

        // Display text
        float font_size = 17.0f * active().canvas_zoom;
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
    bool is_structural = nt && nt->is_declaration;
    bool is_event = nt && nt->is_event;

    // Pins
    PinShape io_shape = PinShape::Signal;
    float pr = PIN_RADIUS * active().canvas_zoom;
    if (!is_structural) {
        // Bang inputs (top, before data inputs)
        for (auto& pin : node.bang_inputs) {
            ImVec2 pp = get_pin_pos(node, *pin, origin);
            draw_pin(dl, pp, pr, IM_COL32(255, 200, 80, 255), PinShape::Square, active().canvas_zoom);
        }
        for (auto& pin : node.inputs) {
            ImVec2 pp = get_pin_pos(node, *pin, origin);
            if (pin->direction == FlowPin::Lambda)
                draw_pin(dl, pp, pr, IM_COL32(180, 130, 255, 255), PinShape::LambdaDown, active().canvas_zoom);
            else
                draw_pin(dl, pp, pr, COL_PIN_IN, io_shape, active().canvas_zoom);
        }
        // Bang outputs (bottom, before data outputs)
        for (auto& pin : node.bang_outputs) {
            ImVec2 pp = get_pin_pos(node, *pin, origin);
            draw_pin(dl, pp, pr, IM_COL32(255, 200, 80, 255), PinShape::Square, active().canvas_zoom);
        }
        for (auto& pin : node.outputs) {
            ImVec2 pp = get_pin_pos(node, *pin, origin);
            draw_pin(dl, pp, pr, COL_PIN_OUT, io_shape, active().canvas_zoom);
        }
        // Lambda grab handle (left) — not on event nodes
        bool show_lambda = nt && nt->has_lambda;
        if (!node.lambda_grab.id.empty() && show_lambda) {
            ImVec2 pp = get_pin_pos(node, node.lambda_grab, origin);
            draw_pin(dl, pp, pr, IM_COL32(180, 130, 255, 150), PinShape::LambdaLeft, active().canvas_zoom);
        }
        // Bang pin (right) — not on event nodes or no_post_bang nodes
        bool no_post_bang = nt && nt->no_post_bang;
        if (!node.bang_pin.id.empty() && !is_event && !no_post_bang) {
            ImVec2 pp = get_pin_pos(node, node.bang_pin, origin);
            draw_pin(dl, pp, pr * 0.7f, IM_COL32(255, 200, 80, 255), PinShape::Square, active().canvas_zoom);
        }
    }
}

void FlowEditorWindow::draw_link(ImDrawList* dl, const FlowLink& link, ImVec2 origin) {
    ImVec2 fp = {}, tp = {};
    bool ff = false, ft = false;
    bool to_lambda = false;
    bool from_grab = false;
    bool from_bang_pin = false;
    FlowPin* from_pin_ptr = nullptr;
    FlowPin* to_pin_ptr = nullptr;
    for (auto& n : active().graph.nodes) {
        for (auto& p : n.outputs) if (p->id == link.from_pin) { fp = get_pin_pos(n, *p, origin); ff = true; from_pin_ptr = p.get(); }
        for (auto& p : n.bang_outputs) if (p->id == link.from_pin) { fp = get_pin_pos(n, *p, origin); ff = true; from_pin_ptr = p.get(); }
        if (n.lambda_grab.id == link.from_pin) { fp = get_pin_pos(n, n.lambda_grab, origin); ff = true; from_grab = true; from_pin_ptr = &n.lambda_grab; }
        if (n.bang_pin.id == link.from_pin) { fp = get_pin_pos(n, n.bang_pin, origin); ff = true; from_bang_pin = true; from_pin_ptr = &n.bang_pin; }
        for (auto& p : n.bang_inputs) if (p->id == link.to_pin) { tp = get_pin_pos(n, *p, origin); ft = true; to_pin_ptr = p.get(); }
        for (auto& p : n.inputs) if (p->id == link.to_pin) { tp = get_pin_pos(n, *p, origin); ft = true; to_pin_ptr = p.get(); if (p->direction == FlowPin::Lambda) to_lambda = true; }
    }
    if (!ff || !ft) return;

    // Check type compatibility for link coloring
    bool type_error = !link.error.empty(); // lambda/inference errors
    if (!type_error && from_pin_ptr && to_pin_ptr &&
        from_pin_ptr->resolved_type && to_pin_ptr->resolved_type &&
        !from_pin_ptr->resolved_type->is_generic && !to_pin_ptr->resolved_type->is_generic) {
        type_error = !types_compatible(from_pin_ptr->resolved_type, to_pin_ptr->resolved_type);
    }

    ImU32 col_error = IM_COL32(255, 60, 60, 220);

    if (from_grab) {
        float dx = std::max(std::abs(tp.x - fp.x) * 0.5f, 30.0f * active().canvas_zoom);
        float dy = std::max(std::abs(tp.y - fp.y) * 0.5f, 30.0f * active().canvas_zoom);
        dl->AddBezierCubic(fp, {fp.x - dx, fp.y}, {tp.x, tp.y - dy}, tp,
                           type_error ? col_error : IM_COL32(180, 130, 255, 200), 2.5f * active().canvas_zoom);
    } else if (from_bang_pin) {
        float dx = std::max(std::abs(tp.x - fp.x) * 0.5f, 30.0f * active().canvas_zoom);
        float dy = std::max(std::abs(tp.y - fp.y) * 0.5f, 30.0f * active().canvas_zoom);
        // Side bang exits horizontally right, enters bang input vertically from above
        dl->AddBezierCubic(fp, {fp.x + dx, fp.y}, {tp.x, tp.y - dy}, tp,
                           type_error ? col_error : IM_COL32(255, 200, 80, 200), 2.5f * active().canvas_zoom);
    } else if (to_lambda) {
        draw_vbezier(dl, fp, tp, type_error ? col_error : IM_COL32(180, 130, 255, 200), 2.5f, active().canvas_zoom);
    } else {
        draw_vbezier(dl, fp, tp, type_error ? col_error : COL_LINK, 2.5f, active().canvas_zoom);
    }
}

void FlowEditorWindow::draw() {
    if (!win_.open) return;

    win_.begin_frame();
    ImGui::SetCurrentContext(win_.imgui_ctx);

    // Tick highlight timer
    if (active().highlight_timer > 0.0f) {
        active().highlight_timer -= ImGui::GetIO().DeltaTime;
        if (active().highlight_timer <= 0.0f) {
            active().highlight_timer = 0.0f;
            active().highlight_node_id = -1;
        }
    }

    // Validate only when graph structure changes
    if (active().graph.dirty) {
        validate_nodes();
        active().graph.dirty = false;
    }

    ImGui::SetNextWindowPos({0, 0});
    int w, h;
    SDL_GetWindowSize(win_.window, &w, &h);
    ImGui::SetNextWindowSize({(float)w, (float)h});
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Toolbar
    draw_toolbar();
    ImGui::Separator();

    // Poll child process
    poll_child_process();

    float total_w = (float)w;
    float total_h = ImGui::GetContentRegionAvail().y;

    // Clamp panel sizes
    file_panel_width_ = std::clamp(file_panel_width_, 80.0f, total_w * 0.3f);
    side_panel_width_ = std::clamp(side_panel_width_, 100.0f, total_w * 0.5f);
    bottom_panel_height_ = std::clamp(bottom_panel_height_, 40.0f, total_h * 0.5f);

    // --- File browser panel (left) ---
    ImGui::BeginChild("##file_browser", {file_panel_width_, total_h}, false,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::TextUnformatted("Files");
    ImGui::Separator();
    ImGui::BeginChild("##file_list", {0, 0}, false);
    for (auto& fname : project_files_) {
        namespace fs = std::filesystem;
        std::string stem = fs::path(fname).stem().string();
        bool is_active = (active_tab_ < (int)tabs_.size() &&
                          tabs_[active_tab_].tab_name == stem);
        if (is_active) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(100, 200, 255, 255));
        if (ImGui::Selectable(stem.c_str(), is_active)) {
            std::string full_path = (fs::path(project_dir_) / fname).string();
            open_tab(full_path);
        }
        if (is_active) ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::EndChild();

    // File browser splitter
    ImGui::SameLine();
    ImGui::Button("##file_vsplitter", {4.0f, total_h});
    if (ImGui::IsItemActive())
        file_panel_width_ += ImGui::GetIO().MouseDelta.x;
    ImGui::SameLine();

    float center_w = total_w - file_panel_width_ - side_panel_width_ - 12.0f;
    float canvas_h = total_h - bottom_panel_height_ - 4.0f;

    // --- Center column: tabs + canvas + bottom panel ---
    ImGui::BeginGroup();

    // --- Tab bar ---
    if (ImGui::BeginTabBar("##nano_tabs")) {
        for (int i = 0; i < (int)tabs_.size(); i++) {
            std::string label = tabs_[i].tab_name;
            if (tabs_[i].dirty) label += "*";
            label += "###tab" + std::to_string(i);
            bool open = true;
            ImGuiTabItemFlags flags = (i == active_tab_) ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(label.c_str(), &open, flags)) {
                if (active_tab_ != i) {
                    active_tab_ = i;
                    // Reset interaction state when switching tabs
                    editing_node_ = -1;
                    dragging_node_ = -1;
                    dragging_link_from_pin_.clear();
                    grabbed_links_.clear();
                }
                ImGui::EndTabItem();
            }
            if (!open) {
                close_tab(i);
                if (i <= active_tab_ && active_tab_ > 0) active_tab_--;
                i--; // re-check this index
            }
        }
        ImGui::EndTabBar();
    }

    float canvas_w = center_w;

    // --- Canvas ---
    ImGui::BeginChild("##flow_canvas", {canvas_w, canvas_h}, false,
                      ImGuiWindowFlags_NoScrollbar);

    ImVec2 canvas_origin = ImGui::GetCursorScreenPos();
    ImVec2 canvas_size = ImGui::GetContentRegionAvail();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background
    dl->AddRectFilled(canvas_origin,
                      {canvas_origin.x + canvas_size.x, canvas_origin.y + canvas_size.y}, COL_BG);

    // Safety: remove any empty-named nodes that aren't currently being edited
    // (type validation relaxed — any name is allowed)
    std::erase_if(active().graph.nodes, [&](auto& n) {
        if (n.id == editing_node_) return false;
        if (n.guid.empty()) return true;
        return false;
    });

    // Grid
    float grid = GRID_SIZE * active().canvas_zoom;
    if (grid > 4.0f) {
        float ox = std::fmod(active().canvas_offset.x * active().canvas_zoom, grid);
        float oy = std::fmod(active().canvas_offset.y * active().canvas_zoom, grid);
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
            active().canvas_offset.x += delta.x / active().canvas_zoom;
            active().canvas_offset.y += delta.y / active().canvas_zoom;
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
            active().canvas_zoom *= zf;
            active().canvas_zoom = std::clamp(active().canvas_zoom, 0.2f, 5.0f);
            ImVec2 mc2 = screen_to_canvas(mouse_pos, canvas_origin);
            active().canvas_offset.x += mc2.x - mc.x;
            active().canvas_offset.y += mc2.y - mc.y;
            schedule_save();
        }
    }

    // Helper: hit test node at canvas pos
    auto hit_test_node = [&](ImVec2 mc) -> int {
        for (int i = (int)active().graph.nodes.size() - 1; i >= 0; i--) {
            auto& node = active().graph.nodes[i];
            if (node.imported) continue;
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
            for (auto& node : active().graph.nodes) {
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
    else if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (editing_node_ >= 0) {
            if (creating_new_node_ && editing_node_ > 0) active().graph.remove_node(editing_node_);
            editing_node_ = -1;
            creating_new_node_ = false;
            active().selected_nodes.clear();
        } else {
            auto pin_hit = hit_test_pin(mouse_pos, canvas_origin);
            if (!pin_hit.pin_id.empty() && (pin_hit.dir == FlowPin::Output || pin_hit.dir == FlowPin::BangOutput || pin_hit.dir == FlowPin::LambdaGrab)) {
                // Start new link from output pin
                dragging_link_from_pin_ = pin_hit.pin_id;
                dragging_node_ = -1;
                dragging_selection_ = false;
            } else {
                dragging_link_from_pin_.clear();
                ImVec2 mc = screen_to_canvas(mouse_pos, canvas_origin);
                int hit_id = hit_test_node(mc);

                if (hit_id >= 0) {
                    if (active().selected_nodes.count(hit_id)) {
                        // Clicking an already-selected node: start dragging selection
                        dragging_selection_ = true;
                        dragging_node_ = -1;
                    } else {
                        // Click unselected node: select only this one
                        active().selected_nodes.clear();
                        active().selected_nodes.insert(hit_id);
                        dragging_selection_ = true;
                        dragging_node_ = -1;
                    }
                } else {
                    // Click empty space: start potential box select
                    // If released without dragging: deselect (if selected) or create node
                    box_selecting_ = true;
                    box_select_start_ = mouse_pos;
                    dragging_node_ = -1;
                    dragging_selection_ = false;
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
            // Only draw box if dragged more than a few pixels
            if (dist > 25.0f) {
                ImVec2 a = box_select_start_;
                ImVec2 b = mouse_pos;
                ImVec2 tl_box = {std::min(a.x, b.x), std::min(a.y, b.y)};
                ImVec2 br_box = {std::max(a.x, b.x), std::max(a.y, b.y)};
                dl->AddRectFilled(tl_box, br_box, IM_COL32(100, 150, 255, 40));
                dl->AddRect(tl_box, br_box, IM_COL32(100, 150, 255, 180));

                ImVec2 ca = screen_to_canvas(tl_box, canvas_origin);
                ImVec2 cb = screen_to_canvas(br_box, canvas_origin);
                active().selected_nodes.clear();
                for (auto& node : active().graph.nodes) {
                    if (node.position.x + node.size.x >= ca.x && node.position.x <= cb.x &&
                        node.position.y + node.size.y >= ca.y && node.position.y <= cb.y)
                        active().selected_nodes.insert(node.id);
                }
            }
        } else {
            // Released: if didn't drag much, deselect or create node
            float dx = mouse_pos.x - box_select_start_.x;
            float dy = mouse_pos.y - box_select_start_.y;
            if (dx*dx + dy*dy <= 25.0f) {
                if (!active().selected_nodes.empty()) {
                    // Had selection: just deselect
                    active().selected_nodes.clear();
                } else {
                    // No selection: open editor for a new node (node created on commit)
                    creating_new_node_ = true;
                    editing_node_ = 0; // sentinel: no real node yet
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
            if (!pin_hit.pin_id.empty() && (pin_hit.dir == FlowPin::Input || pin_hit.dir == FlowPin::BangInput || pin_hit.dir == FlowPin::Lambda)) {
                if (pin_hit.dir != FlowPin::BangInput)
                    std::erase_if(active().graph.links, [&](auto& l) { return l.to_pin == pin_hit.pin_id; });
                active().graph.add_link(dragging_link_from_pin_, pin_hit.pin_id);
                mark_dirty();
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
                for (auto& l : active().graph.links) {
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
                        std::erase_if(active().graph.links, [&](auto& l) { return l.from_pin == grabbed_pin_; });
                    else
                        std::erase_if(active().graph.links, [&](auto& l) { return l.to_pin == grabbed_pin_; });
                    active().graph.dirty = true;
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
                // Find the anchored end position (the end NOT being dragged)
                ImVec2 anchor = {};
                bool found = false;
                std::string anchor_id = grab_is_output_ ? gl.to_pin : gl.from_pin;
                for (auto& n : active().graph.nodes) {
                    for (auto& p : n.outputs) if (p->id == anchor_id) { anchor = get_pin_pos(n, *p, canvas_origin); found = true; }
                    for (auto& p : n.bang_outputs) if (p->id == anchor_id) { anchor = get_pin_pos(n, *p, canvas_origin); found = true; }
                    if (n.lambda_grab.id == anchor_id) { anchor = get_pin_pos(n, n.lambda_grab, canvas_origin); found = true; }
                    if (n.bang_pin.id == anchor_id) { anchor = get_pin_pos(n, n.bang_pin, canvas_origin); found = true; }
                    for (auto& p : n.bang_inputs) if (p->id == anchor_id) { anchor = get_pin_pos(n, *p, canvas_origin); found = true; }
                    for (auto& p : n.inputs) if (p->id == anchor_id) { anchor = get_pin_pos(n, *p, canvas_origin); found = true; }
                }
                if (found) {
                    ImU32 col = COL_LINK_DRAG;
                    if (grab_is_output_)
                        draw_vbezier(dl, mouse_pos, anchor, col, 2.5f, active().canvas_zoom);
                    else
                        draw_vbezier(dl, anchor, mouse_pos, col, 2.5f, active().canvas_zoom);
                }
            }
        } else {
            // Released: try to reconnect
            auto pin_hit = hit_test_pin(mouse_pos, canvas_origin);
            bool reconnected = false;
            if (!pin_hit.pin_id.empty()) {
                if (grab_is_output_) {
                    // Was dragging output side: drop on another output pin
                    if (pin_hit.dir == FlowPin::Output || pin_hit.dir == FlowPin::BangOutput || pin_hit.dir == FlowPin::LambdaGrab) {
                        for (auto& gl : grabbed_links_)
                            active().graph.add_link(pin_hit.pin_id, gl.to_pin);
                        reconnected = true;
                        mark_dirty();
                    }
                } else {
                    // Was dragging input side: drop on another input pin
                    if (pin_hit.dir == FlowPin::Input || pin_hit.dir == FlowPin::BangInput || pin_hit.dir == FlowPin::Lambda) {
                        if (pin_hit.dir != FlowPin::BangInput)
                            std::erase_if(active().graph.links, [&](auto& l) { return l.to_pin == pin_hit.pin_id; });
                        for (auto& gl : grabbed_links_)
                            active().graph.add_link(gl.from_pin, pin_hit.pin_id);
                        reconnected = true;
                        mark_dirty();
                    }
                }
            }
            if (!reconnected) {
                // Put links back where they were
                for (auto& gl : grabbed_links_)
                    active().graph.add_link(gl.from_pin, gl.to_pin);
            }
            grabbed_links_.clear();
            grabbed_pin_.clear();
        }
    }

    // Selection dragging (move all selected nodes)
    if (dragging_selection_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        ImVec2 delta = ImGui::GetIO().MouseDelta;
        for (auto& node : active().graph.nodes) {
            if (active().selected_nodes.count(node.id)) {
                node.position.x += delta.x / active().canvas_zoom;
                node.position.y += delta.y / active().canvas_zoom;
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
            // Duplicate: copy + paste at mouse, without affecting clipboard
            auto saved_nodes = active().clipboard_nodes;
            auto saved_links = active().clipboard_links;
            copy_selection();
            ImVec2 mc = screen_to_canvas(mouse_pos, canvas_origin);
            paste_at(mc);
            active().clipboard_nodes = saved_nodes;
            active().clipboard_links = saved_links;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !active().selected_nodes.empty()) {
            for (int id : active().selected_nodes)
                active().graph.remove_node(id);
            active().selected_nodes.clear();
            mark_dirty();
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
            if (ImGui::GetIO().KeyShift)
                redo();
            else
                undo();
            active().selected_nodes.clear();
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) {
            redo();
            active().selected_nodes.clear();
        }
    }

    // --- Right click: track start position and check for pin grab ---
    static ImVec2 right_click_start = {};
    if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        right_click_start = mouse_pos;
        // Check if right-clicking a pin with connections -> potential grab
        auto pin_hit = hit_test_pin(mouse_pos, canvas_origin);
        if (!pin_hit.pin_id.empty()) {
            grabbed_links_.clear();
            grabbed_pin_ = pin_hit.pin_id;
            grab_is_output_ = (pin_hit.dir == FlowPin::Output || pin_hit.dir == FlowPin::BangOutput ||
                               pin_hit.dir == FlowPin::LambdaGrab);
            grab_pending_ = true;
            grab_start_ = mouse_pos;
        }
    }

    // --- Right click release: disconnect pin, delete link, or delete node (only if not dragged) ---
    if (canvas_hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        float rdx = mouse_pos.x - right_click_start.x;
        float rdy = mouse_pos.y - right_click_start.y;
        bool was_drag = (rdx*rdx + rdy*rdy > 25.0f);

        if (!was_drag) {
        // First check if right-clicking a connected pin to disconnect
        auto pin_hit = hit_test_pin(mouse_pos, canvas_origin);
        if (!pin_hit.pin_id.empty()) {
            // Remove all links to/from this pin
            std::erase_if(active().graph.links, [&](auto& l) {
                return l.from_pin == pin_hit.pin_id || l.to_pin == pin_hit.pin_id;
            });
            active().graph.dirty = true;
        }
        // Then check links
        else {
            int lid = hit_test_link(mouse_pos, canvas_origin);
            if (lid >= 0) {
                active().graph.remove_link(lid);
            } else {
                // Check if right-clicking a node to delete it
                ImVec2 mc = screen_to_canvas(mouse_pos, canvas_origin);
                for (int i = (int)active().graph.nodes.size() - 1; i >= 0; i--) {
                    auto& node = active().graph.nodes[i];
                    if (mc.x >= node.position.x && mc.x <= node.position.x + node.size.x &&
                        mc.y >= node.position.y && mc.y <= node.position.y + node.size.y) {
                        active().graph.remove_node(node.id);
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

    // --- Draw links ---
    for (auto& link : active().graph.links) draw_link(dl, link, canvas_origin);

    // --- Draw link being dragged ---
    if (!dragging_link_from_pin_.empty() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        for (auto& node : active().graph.nodes) {
            // Find the source pin (output or lambda grab)
            ImVec2 from = {};
            bool from_grab = false;
            bool from_bang_pin = false;
            bool found = false;
            for (auto& pin : node.outputs) {
                if (pin->id == dragging_link_from_pin_) { from = get_pin_pos(node, *pin, canvas_origin); found = true; break; }
            }
            if (!found) for (auto& pin : node.bang_outputs) {
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
                bool valid_target = !target.pin_id.empty() &&
                    (target.dir == FlowPin::Input || target.dir == FlowPin::BangInput || target.dir == FlowPin::Lambda);
                ImU32 col = valid_target ? COL_PIN_HOVER : COL_LINK_DRAG;
                if (from_grab) {
                    float dx = std::max(std::abs(mouse_pos.x - from.x) * 0.5f, 30.0f * active().canvas_zoom);
                    float dy = std::max(std::abs(mouse_pos.y - from.y) * 0.5f, 30.0f * active().canvas_zoom);
                    dl->AddBezierCubic(from, {from.x - dx, from.y}, {mouse_pos.x, mouse_pos.y - dy},
                                       mouse_pos, col, 2.5f * active().canvas_zoom);
                } else if (from_bang_pin) {
                    float dx = std::max(std::abs(mouse_pos.x - from.x) * 0.5f, 30.0f * active().canvas_zoom);
                    dl->AddBezierCubic(from, {from.x + dx, from.y}, {mouse_pos.x - dx, mouse_pos.y},
                                       mouse_pos, col, 2.5f * active().canvas_zoom);
                } else {
                    draw_vbezier(dl, from, mouse_pos, col, 2.5f, active().canvas_zoom);
                }
                goto done_drag;
            }
        }
        done_drag:;
    }

    // --- Draw nodes ---
    auto hovered_pin = hit_test_pin(mouse_pos, canvas_origin);
    for (auto& node : active().graph.nodes) {
        if (node.imported) continue;
        draw_node(dl, node, canvas_origin);
    }

    // Pin hover highlight
    if (!hovered_pin.pin_id.empty()) {
        for (auto& node : active().graph.nodes) {
            PinShape io_shape = PinShape::Signal;
            float pr = PIN_RADIUS * active().canvas_zoom;
            auto check = [&](auto& pins, PinShape shape) {
                for (auto& pin : pins)
                    if (pin->id == hovered_pin.pin_id) {
                        ImVec2 pp = get_pin_pos(node, *pin, canvas_origin);
                        draw_pin_highlight(dl, pp, pr, COL_PIN_HOVER, shape, active().canvas_zoom);
                    }
            };
            check(node.bang_inputs, PinShape::Square);
            // Inputs: check each pin's direction for shape
            for (auto& pin : node.inputs)
                if (pin->id == hovered_pin.pin_id) {
                    ImVec2 pp = get_pin_pos(node, *pin, canvas_origin);
                    PinShape shape = (pin->direction == FlowPin::Lambda) ? PinShape::LambdaDown : io_shape;
                    draw_pin_highlight(dl, pp, pr, COL_PIN_HOVER, shape, active().canvas_zoom);
                }
            check(node.bang_outputs, PinShape::Square);
            check(node.outputs, io_shape);
            if (node.lambda_grab.id == hovered_pin.pin_id) {
                ImVec2 pp = get_pin_pos(node, node.lambda_grab, canvas_origin);
                draw_pin_highlight(dl, pp, pr, COL_PIN_HOVER, PinShape::LambdaLeft, active().canvas_zoom);
            }
        }
    }

    // --- Tooltips ---
    if (canvas_hovered && editing_node_ < 0) {
        if (!hovered_pin.pin_id.empty()) {
            // Pin tooltip
            for (auto& node : active().graph.nodes) {
                if (node.id != hovered_pin.node_id) continue;
                // Find the pin object
                auto find_pin = [&](auto& pins) -> const FlowPin* {
                    for (auto& p : pins) if (p->id == hovered_pin.pin_id) return p.get();
                    return nullptr;
                };
                const FlowPin* pin = find_pin(node.bang_inputs);
                if (!pin) pin = find_pin(node.inputs);
                if (!pin) pin = find_pin(node.outputs);
                if (!pin) pin = find_pin(node.bang_outputs);
                if (!pin && node.lambda_grab.id == hovered_pin.pin_id) pin = &node.lambda_grab;
                if (!pin && node.bang_pin.id == hovered_pin.pin_id) pin = &node.bang_pin;
                if (pin) {
                    auto [port_name, port_desc] = get_port_desc(node, *pin);
                    std::string type_str;
                    if (pin->resolved_type)
                        type_str = type_to_string(pin->resolved_type);
                    else if (pin->direction == FlowPin::BangInput || pin->direction == FlowPin::BangOutput)
                        type_str = "bang";
                    else
                        type_str = "?";
                    ImGui::BeginTooltip();
                    ImGui::SetWindowFontScale(active().canvas_zoom);
                    ImGui::TextUnformatted((port_name + " : " + type_str).c_str());
                    if (!port_desc.empty())
                        ImGui::TextDisabled("%s", port_desc.c_str());
                    ImGui::EndTooltip();
                }
                break;
            }
        } else {
            // Check link hover
            int lid = hit_test_link(mouse_pos, canvas_origin);
            if (lid >= 0) {
                // Find the link
                for (auto& link : active().graph.links) {
                    if (link.id != lid) continue;
                    std::string from_label, to_label;
                    for (auto& n : active().graph.nodes) {
                        for (auto& p : n.outputs) if (p->id == link.from_pin) from_label = pin_label(n, *p);
                        for (auto& p : n.bang_outputs) if (p->id == link.from_pin) from_label = pin_label(n, *p);
                        if (n.lambda_grab.id == link.from_pin) from_label = pin_label(n, n.lambda_grab);
                        if (n.bang_pin.id == link.from_pin) from_label = pin_label(n, n.bang_pin);
                        for (auto& p : n.inputs) if (p->id == link.to_pin) to_label = pin_label(n, *p);
                        for (auto& p : n.bang_inputs) if (p->id == link.to_pin) to_label = pin_label(n, *p);
                    }
                    if (!from_label.empty() && !to_label.empty()) {
                        // Get types for the link endpoints
                        auto* fp = active().graph.find_pin(link.from_pin);
                        auto* tp = active().graph.find_pin(link.to_pin);
                        std::string from_type_str = (fp && fp->resolved_type) ? type_to_string(fp->resolved_type) : "?";
                        std::string to_type_str = (tp && tp->resolved_type) ? type_to_string(tp->resolved_type) : "?";
                        bool type_err = !link.error.empty();
                        if (!type_err && fp && tp && fp->resolved_type && tp->resolved_type &&
                            !fp->resolved_type->is_generic && !tp->resolved_type->is_generic)
                            type_err = !types_compatible(fp->resolved_type, tp->resolved_type);

                        ImGui::BeginTooltip();
                        ImGui::SetWindowFontScale(active().canvas_zoom);
                        ImGui::TextUnformatted((from_label + " -> " + to_label).c_str());
                        ImGui::TextDisabled("%s -> %s", from_type_str.c_str(), to_type_str.c_str());
                        if (!link.error.empty())
                            ImGui::TextColored({1.0f, 0.2f, 0.2f, 1.0f}, "%s", link.error.c_str());
                        else if (type_err)
                            ImGui::TextColored({1.0f, 0.2f, 0.2f, 1.0f}, "Type mismatch!");
                        ImGui::EndTooltip();
                    }
                    break;
                }
            } else {
                // Check node hover
                ImVec2 mc = screen_to_canvas(mouse_pos, canvas_origin);
                for (int i = (int)active().graph.nodes.size() - 1; i >= 0; i--) {
                    auto& node = active().graph.nodes[i];
                    if (mc.x >= node.position.x && mc.x <= node.position.x + node.size.x &&
                        mc.y >= node.position.y && mc.y <= node.position.y + node.size.y) {
                        auto* nt = find_node_type(node.type_id);
                        ImGui::BeginTooltip();
                        ImGui::SetWindowFontScale(active().canvas_zoom);
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
        // Find the node, or use new_node_pos_ for pending new nodes
        FlowNode* edit_node = nullptr;
        for (auto& node : active().graph.nodes) {
            if (node.id == editing_node_) { edit_node = &node; break; }
        }
        ImVec2 edit_pos = edit_node ? to_imvec(edit_node->position) : new_node_pos_;
        ImVec2 edit_size = edit_node ? to_imvec(edit_node->size) : ImVec2{NODE_MIN_WIDTH, NODE_HEIGHT};

        {
            ImVec2 tl = canvas_to_screen(edit_pos, canvas_origin);
            ImVec2 br = canvas_to_screen({edit_pos.x + edit_size.x,
                                           edit_pos.y + edit_size.y}, canvas_origin);
            float nw = br.x - tl.x;

            float text_w = ImGui::CalcTextSize(edit_buf_.c_str()).x * active().canvas_zoom + 40.0f * active().canvas_zoom;
            float scaled_min_w = std::max({nw, 160.0f * active().canvas_zoom, text_w});
            ImGui::SetNextWindowPos(tl);
            ImGui::SetNextWindowSize({scaled_min_w, 0});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {2 * active().canvas_zoom, 2 * active().canvas_zoom});
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4 * active().canvas_zoom, 2 * active().canvas_zoom});
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {4 * active().canvas_zoom, 2 * active().canvas_zoom});
            ImGui::Begin("##name_edit", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
            ImGui::SetWindowFontScale(active().canvas_zoom);

            if (edit_just_opened_) {
                ImGui::SetKeyboardFocusHere();
                edit_just_opened_ = false;
            }

            char buf[128];
            strncpy(buf, edit_buf_.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';

            // Callback to move cursor to end after autocomplete
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

            // Split into first word (type name) and rest (args) for matching
            std::string first_word = edit_buf_;
            std::string rest_args;
            auto space_pos = edit_buf_.find(' ');
            if (space_pos != std::string::npos) {
                first_word = edit_buf_.substr(0, space_pos);
                rest_args = edit_buf_.substr(space_pos + 1);
            }

            // Autocompletion: match against first word, show all when empty
            // Only show when no space yet (still typing the type name)
            if (space_pos == std::string::npos) {
                for (int i = 0; i < NUM_NODE_TYPES; i++) {
                    std::string nt_name(NODE_TYPES[i].name);
                    if (first_word.empty() || (nt_name.find(first_word) != std::string::npos && nt_name != first_word)) {
                        if (ImGui::Selectable(NODE_TYPES[i].name)) {
                            // Insert type + space, keep editor open
                            edit_buf_ = nt_name + " ";
                            edit_just_opened_ = true; // re-focus the text input next frame
                            edit_cursor_to_end_ = true; // place cursor at end, not select-all
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

                // If this is a pending new node (no backing node yet), create it now
                if (creating_new_node_ && !edit_node) {
                    int id = active().graph.add_node("", to_vec2(new_node_pos_), 0, 0);
                    for (auto& n : active().graph.nodes) {
                        if (n.id == id) { edit_node = &n; break; }
                    }
                    editing_node_ = id;
                }
                if (!edit_node) break;

                auto* nt = find_node_type(node_type.c_str());
                if (!nt) {
                    // Unknown type: treat entire input as an expr node
                    nt = find_node_type("expr");
                    node_type = "expr";
                    rest_args = edit_buf_;
                }
                int default_bang_inputs = nt ? nt->bang_inputs : 0;
                int default_inputs = nt ? nt->inputs : 0;
                int default_outputs = nt ? nt->outputs : 0;
                int default_bang_outputs = nt ? nt->bang_outputs : 0;

                // Auto-assign guid if not set
                auto& node = *edit_node;
                if (node.guid.empty())
                    node.guid = generate_guid();
                node.type_id = node_type_id_from_string(node_type.c_str());
                node.args = rest_args;
                node.parse_args();
                active().graph.dirty = true;
                creating_new_node_ = false;

                // Resize a pin vector: reuse existing pins (preserving IDs/links),
                // add new ones at end, remove excess from end (clearing their links).
                auto resize_pins = [&](PinVec& pins, int needed,
                                       const std::vector<std::string>& names,
                                       FlowPin::Direction dir, bool is_output) {
                    // Reuse existing: just rename
                    for (int i = 0; i < std::min((int)pins.size(), needed); i++)
                        pins[i]->name = names[i];
                    // Add new
                    for (int i = (int)pins.size(); i < needed; i++)
                        pins.push_back(make_pin("", names[i], "", nullptr, dir));
                    // Remove excess (from back)
                    while ((int)pins.size() > needed) {
                        auto pid = pins.back()->id;
                        if (is_output)
                            std::erase_if(active().graph.links, [&pid](auto& l) { return l.from_pin == pid; });
                        else
                            std::erase_if(active().graph.links, [&pid](auto& l) { return l.to_pin == pid; });
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

                // Build desired input pin list (data + lambda unified, in slot order)
                struct DesiredPin { std::string name; FlowPin::Direction dir; };
                std::vector<DesiredPin> desired_inputs;

                if (node.type_id == NodeTypeID::New) {
                    auto tokens = tokenize_args(rest_args, false);
                    std::string inst_type_name = tokens.empty() ? "" : tokens[0];
                    auto* type_node = find_type_node(active().graph, inst_type_name);
                    if (type_node) {
                        auto fields = parse_type_fields(*type_node);
                        for (auto& field : fields)
                            desired_inputs.push_back({field.name, FlowPin::Input});
                    }
                    needed_outputs = 1;
                } else if (node.type_id == NodeTypeID::EventBang) {
                    // Outputs come from event declaration args
                    auto tokens = tokenize_args(rest_args, false);
                    std::string event_name = tokens.empty() ? "" : tokens[0];
                    auto* event_decl = find_event_node(active().graph, event_name);
                    if (event_decl) {
                        auto args = parse_event_args(*event_decl, active().graph);
                        // Override outputs
                        std::vector<std::string> out_names;
                        for (auto& a : args) out_names.push_back(a.name);
                        needed_outputs = (int)out_names.size();
                        // Resize outputs directly here
                        for (int i = 0; i < std::min((int)node.outputs.size(), needed_outputs); i++)
                            node.outputs[i]->name = out_names[i];
                        for (int i = (int)node.outputs.size(); i < needed_outputs; i++)
                            node.outputs.push_back(make_pin("", out_names[i], "", nullptr, FlowPin::Output));
                        while ((int)node.outputs.size() > needed_outputs) {
                            auto pid = node.outputs.back()->id;
                            std::erase_if(active().graph.links, [&pid](auto& l) { return l.from_pin == pid; });
                            node.outputs.pop_back();
                        }
                        needed_outputs = -1; // skip generic output resize below
                    }
                } else {
                    if (is_expr_type) {
                        // Expr nodes: pin count from $N refs, output count from tokens
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
                        // Args are type names — use descriptor defaults directly
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
                        // Non-expr nodes: use inline arg computation
                        auto info = compute_inline_args(rest_args, default_inputs);
                        if (!info.error.empty()) node.error = info.error;
                        // First: $N/@N ref pins
                        int ref_pins = (info.pin_slots.max_slot >= 0) ? (info.pin_slots.max_slot + 1) : 0;
                        for (int i = 0; i < ref_pins; i++) {
                            bool is_lambda = info.pin_slots.is_lambda_slot(i);
                            std::string pin_name = is_lambda ? ("@" + std::to_string(i)) : std::to_string(i);
                            desired_inputs.push_back({pin_name, is_lambda ? FlowPin::Lambda : FlowPin::Input});
                        }
                        // Then: remaining descriptor inputs
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
                    // Reuse existing: update name and direction
                    for (int i = 0; i < std::min((int)node.inputs.size(), needed); i++) {
                        node.inputs[i]->name = desired_inputs[i].name;
                        node.inputs[i]->direction = desired_inputs[i].dir;
                    }
                    // Add new
                    for (int i = (int)node.inputs.size(); i < needed; i++)
                        node.inputs.push_back(make_pin("", desired_inputs[i].name, "", nullptr, desired_inputs[i].dir));
                    // Remove excess
                    while ((int)node.inputs.size() > needed) {
                        auto pid = node.inputs.back()->id;
                        std::erase_if(active().graph.links, [&pid](auto& l) { return l.to_pin == pid; });
                        node.inputs.pop_back();
                    }
                }

                // Resize bang inputs
                resize_pins(node.bang_inputs, default_bang_inputs,
                            make_names("bang_in", default_bang_inputs), FlowPin::BangInput, false);
                if (needed_outputs >= 0)
                    resize_pins(node.outputs, needed_outputs,
                                make_names("out", needed_outputs), FlowPin::Output, true);
                resize_pins(node.bang_outputs, default_bang_outputs,
                            make_names("bang", default_bang_outputs), FlowPin::BangOutput, true);

                // Rebuild pin IDs from guid and update links
                // Collect old->new ID mapping for pins whose name changed
                auto update_pin_ids = [&](PinVec& pins) {
                    for (auto& p : pins) {
                        std::string new_id = node.pin_id(p->name);
                        if (p->id != new_id) {
                            // Update any links referencing old ID
                            for (auto& l : active().graph.links) {
                                if (l.from_pin == p->id) l.from_pin = new_id;
                                if (l.to_pin == p->id) l.to_pin = new_id;
                            }
                            p->id = new_id;
                        }
                    }
                };
                update_pin_ids(node.bang_inputs);
                update_pin_ids(node.inputs);
                update_pin_ids(node.outputs);
                update_pin_ids(node.bang_outputs);
                {
                    std::string new_id = node.pin_id("as_lambda");
                    for (auto& l : active().graph.links) {
                        if (l.from_pin == node.lambda_grab.id) l.from_pin = new_id;
                        if (l.to_pin == node.lambda_grab.id) l.to_pin = new_id;
                    }
                    node.lambda_grab.id = new_id;
                }
                {
                    std::string new_id = node.pin_id("post_bang");
                    for (auto& l : active().graph.links) {
                        if (l.from_pin == node.bang_pin.id) l.from_pin = new_id;
                        if (l.to_pin == node.bang_pin.id) l.to_pin = new_id;
                    }
                    node.bang_pin.id = new_id;
                }

                editing_node_ = -1;
                mark_dirty();
            } while (false);

            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                if (creating_new_node_ && edit_node) {
                    active().graph.remove_node(editing_node_);
                }
                creating_new_node_ = false;
                editing_node_ = -1;
            }

            ImGui::End();
            ImGui::PopStyleVar(3);
        } // end of edit window block
    }

    ImGui::EndChild(); // flow_canvas

    // --- Horizontal splitter (between canvas and bottom panel) ---
    ImGui::InvisibleButton("##hsplitter", {canvas_w, 4.0f});
    if (ImGui::IsItemActive()) {
        bottom_panel_height_ -= ImGui::GetIO().MouseDelta.y;
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

    // --- Bottom panel: tabbed (Errors / Build Log) ---
    ImGui::BeginChild("##bottom_panel", {canvas_w, bottom_panel_height_}, true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (ImGui::BeginTabBar("##bottom_tabs")) {
        // Count errors for tab label
        int error_count = 0;
        for (auto& node : active().graph.nodes) if (!node.error.empty()) error_count++;
        for (auto& link : active().graph.links) if (!link.error.empty()) error_count++;

        char errors_label[64];
        snprintf(errors_label, sizeof(errors_label), "Errors%s", error_count > 0 ? " (!)" : "");

        if (ImGui::BeginTabItem(errors_label)) {
            ImGui::BeginChild("##errors_scroll", {0, 0}, false);
            for (auto& node : active().graph.nodes) {
                if (node.error.empty()) continue;
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 100, 100, 255));
                std::string label = std::string(node_type_str(node.type_id)) + " [" + node.guid.substr(0, 8) + "]: " + node.error;
                if (ImGui::Selectable(label.c_str())) {
                    center_on_node(node, {canvas_w, canvas_h});
                }
                ImGui::PopStyleColor();
            }
            for (auto& link : active().graph.links) {
                if (link.error.empty()) continue;
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 160, 80, 255));
                std::string label = "link [" + link.from_pin.substr(0, 8) + "->...]: " + link.error;
                if (ImGui::Selectable(label.c_str())) {
                    auto dot = link.from_pin.find('.');
                    if (dot != std::string::npos) {
                        std::string guid = link.from_pin.substr(0, dot);
                        for (auto& n : active().graph.nodes) {
                            if (n.guid == guid) { center_on_node(n, {canvas_w, canvas_h}); break; }
                        }
                    }
                }
                ImGui::PopStyleColor();
            }
            ImGui::Dummy({0, bottom_panel_height_ * 0.5f});
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Build Log", nullptr, show_build_log_ ? ImGuiTabItemFlags_SetSelected : 0)) {
            show_build_log_ = false;
            ImGui::BeginChild("##buildlog_scroll", {0, 0}, false);
            {
                std::lock_guard<std::mutex> lock(build_log_mutex_);
                ImGui::TextWrapped("%s", build_log_.c_str());
            }
            // Bottom padding so the last line isn't stuck at the edge
            ImGui::Dummy({0, bottom_panel_height_ * 0.5f});
            if (build_state_ == BuildState::Building) {
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40.0f)
                    ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::EndGroup();

    ImGui::SameLine();

    // --- Vertical splitter (between canvas column and side panel) ---
    ImGui::InvisibleButton("##vsplitter", {4.0f, total_h});
    if (ImGui::IsItemActive()) {
        side_panel_width_ -= ImGui::GetIO().MouseDelta.x;
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    ImGui::SameLine();

    // --- Side panel: declarations (right) ---
    ImGui::BeginChild("##side_panel", {side_panel_width_, total_h}, true);
    ImGui::TextUnformatted("Declarations");
    ImGui::Separator();

    // Local declarations (non-imported)
    for (auto& node : active().graph.nodes) {
        auto* nt_decl = find_node_type(node.type_id);
        if (!nt_decl || !nt_decl->is_declaration) continue;
        if (node.imported) continue;
        bool has_err = !node.error.empty();
        if (has_err) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 100, 100, 255));
        if (ImGui::Selectable(node.display_text().c_str())) {
            center_on_node(node, {canvas_w, canvas_h});
        }
        if (has_err) ImGui::PopStyleColor();
        if (has_err && ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(node.error.c_str());
            ImGui::EndTooltip();
        }
    }

    // Imported declarations grouped by import source
    // Collect unique import paths
    for (auto& imp_node : active().graph.nodes) {
        if (imp_node.type_id != NodeTypeID::DeclImport) continue;
        auto tokens = tokenize_args(imp_node.args, false);
        if (tokens.empty()) continue;
        std::string label = tokens[0]; // e.g. "std/imgui"
        if (ImGui::TreeNode(label.c_str())) {
            for (auto& node : active().graph.nodes) {
                if (!node.imported) continue;
                auto* nt_decl = find_node_type(node.type_id);
                if (!nt_decl || !nt_decl->is_declaration) continue;
                ImGui::TextDisabled("%s", node.display_text().c_str());
            }
            ImGui::TreePop();
        }
    }
    ImGui::EndChild();

    ImGui::End(); // main
    check_debounced_save();
    win_.end_frame(30, 30, 40);
}

void FlowEditorWindow::validate_nodes() {
    // Resolve type-based pins (new, event!) from current declarations
    resolve_type_based_pins(active().graph);

    // Build type registry from decl_type nodes
    TypeRegistry registry;
    for (auto& node : active().graph.nodes) {
        if (node.type_id == NodeTypeID::DeclType) {
            auto tokens = tokenize_args(node.args, false);
            if (tokens.size() >= 2) {
                // First token is the type name, rest is the definition
                std::string type_name = tokens[0];
                // Reconstruct the definition: for struct types, build field list
                // For now, register the raw args minus the name
                std::string def;
                for (size_t i = 1; i < tokens.size(); i++) {
                    if (!def.empty()) def += " ";
                    def += tokens[i];
                }
                int decl_class = classify_decl_type(tokens);
                if (decl_class == 0 || decl_class == 1) { // alias or function type
                    registry.register_type(type_name, def);
                } else {
                    registry.register_type(type_name, "void"); // placeholder, fields validated below
                }
            }
        }
    }

    // Resolve all types and check for cycles
    registry.resolve_all();

    for (auto& node : active().graph.nodes) {
        node.error.clear();

        auto* nt = find_node_type(node.type_id);
        if (!nt) {
            node.error = "Unknown node type: " + std::string(node_type_str(node.type_id));
            continue;
        }

        // Check for duplicate guids
        for (auto& other : active().graph.nodes) {
            if (&other != &node && other.guid == node.guid) {
                node.error = "Duplicate guid: " + node.guid;
                break;
            }
        }
        if (!node.error.empty()) continue;

        // Validate decl_type nodes
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

            // Check registry errors for this type
            auto err_it = registry.errors.find(type_name);
            if (err_it != registry.errors.end()) {
                node.error = err_it->second;
                continue;
            }

            // Check struct types have fields
            int decl_class_v = classify_decl_type(tokens);
            if (decl_class_v == 2) { // struct
                // Must be a struct — check it has at least one field
                bool has_any_field = false;
                for (size_t i = 1; i < tokens.size(); i++) {
                    if (tokens[i].find(':') != std::string::npos) { has_any_field = true; break; }
                }
                if (!has_any_field) {
                    node.error = "Struct type '" + type_name + "' must have at least one field (name:type)";
                    continue;
                }
            }

            // Validate each field type
            for (size_t i = 1; i < tokens.size(); i++) {
                auto& tok = tokens[i];
                // Skip function syntax tokens
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

        // Validate decl_var nodes: decl_var <name> <type>
        if (node.type_id == NodeTypeID::DeclVar) {
            auto tokens = tokenize_args(node.args, false);
            if (tokens.size() < 2) {
                node.error = "decl_var requires: name type";
                continue;
            }
            // Check name doesn't start with $
            if (!tokens[0].empty() && tokens[0][0] == '$') {
                node.error = "Variable name should not start with $ in declarations";
                continue;
            }
            // Validate type (second arg)
            std::string err;
            if (!registry.validate_type(tokens[1], err)) {
                node.error = "Invalid type: " + err;
            }
        }


        // Validate 'new' nodes — type must exist
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

        // Validate event! nodes — must reference a valid decl_event with ~ prefix, return must be void
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
            auto* event_decl = find_event_node(active().graph, tokens[0]);
            if (!event_decl) {
                node.error = "Unknown event: " + tokens[0];
                continue;
            }
            // Check return type is void
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

    // Run type inference (always, since validate_nodes clears errors each frame)
    run_type_inference();
}

void FlowEditorWindow::run_type_inference() {
    GraphInference inference(active().type_pool);
    inference.run(active().graph);
}

void FlowEditorWindow::center_on_node(const FlowNode& node, ImVec2 canvas_size) {
    active().canvas_offset.x = -node.position.x - node.size.x * 0.5f + canvas_size.x * 0.5f / active().canvas_zoom;
    active().canvas_offset.y = -node.position.y - node.size.y * 0.5f + canvas_size.y * 0.5f / active().canvas_zoom;
    active().highlight_node_id = node.id;
    active().highlight_timer = 3.0f;
}

void FlowEditorWindow::copy_selection() {
    active().clipboard_nodes.clear();
    active().clipboard_links.clear();
    if (active().selected_nodes.empty()) return;

    // Compute centroid
    ImVec2 centroid = {0, 0};
    int count = 0;
    for (auto& node : active().graph.nodes) {
        if (!active().selected_nodes.count(node.id)) continue;
        centroid.x += node.position.x;
        centroid.y += node.position.y;
        count++;
    }
    if (count > 0) { centroid.x /= count; centroid.y /= count; }

    // Build index map: node id -> clipboard index
    std::map<int, int> id_to_idx;
    for (auto& node : active().graph.nodes) {
        if (!active().selected_nodes.count(node.id)) continue;
        int idx = (int)active().clipboard_nodes.size();
        id_to_idx[node.id] = idx;
        active().clipboard_nodes.push_back({node.type_id, node.args,
            {node.position.x - centroid.x, node.position.y - centroid.y}});
    }

    // Copy internal links (both endpoints in selection)
    // Build pin_id -> (node_id, pin_name) map
    std::map<std::string, std::pair<int, std::string>> pin_owner;
    for (auto& node : active().graph.nodes) {
        if (!active().selected_nodes.count(node.id)) continue;
        auto register_pin = [&](const FlowPin& p) { pin_owner[p.id] = {node.id, p.name}; };
        for (auto& p : node.bang_inputs) register_pin(*p);
        for (auto& p : node.inputs) register_pin(*p);
        for (auto& p : node.outputs) register_pin(*p);
        for (auto& p : node.bang_outputs) register_pin(*p);
        register_pin(node.lambda_grab);
        register_pin(node.bang_pin);
    }
    for (auto& link : active().graph.links) {
        auto fi = pin_owner.find(link.from_pin);
        auto ti = pin_owner.find(link.to_pin);
        if (fi != pin_owner.end() && ti != pin_owner.end()) {
            auto from_idx = id_to_idx[fi->second.first];
            auto to_idx = id_to_idx[ti->second.first];
            active().clipboard_links.push_back({from_idx, to_idx, fi->second.second, ti->second.second});
        }
    }
}

void FlowEditorWindow::paste_at(ImVec2 canvas_pos) {
    if (active().clipboard_nodes.empty()) return;

    active().selected_nodes.clear();
    std::vector<std::string> new_guids;

    // Create nodes
    for (auto& cn : active().clipboard_nodes) {
        std::string guid = generate_guid();
        new_guids.push_back(guid);
        ImVec2 pos = {canvas_pos.x + cn.offset.x, canvas_pos.y + cn.offset.y};
        int id = active().graph.add_node(guid, to_vec2(pos), 0, 0);

        // Set type and args, rebuild pins
        for (auto& node : active().graph.nodes) {
            if (node.id != id) continue;
            node.type_id = cn.type_id;
            node.args = cn.args;
            node.parse_args();

            // Rebuild pins from type descriptor
            auto* nt = find_node_type(cn.type_id);
            if (nt) {
                node.bang_inputs.clear();
                node.inputs.clear();
                node.outputs.clear();
                node.bang_outputs.clear();

                for (int i = 0; i < nt->bang_inputs; i++)
                    node.bang_inputs.push_back(make_pin("", "bang_in" + std::to_string(i), "", nullptr, FlowPin::BangInput));

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
                for (int i = 0; i < num_outputs; i++)
                    node.outputs.push_back(make_pin("", "out" + std::to_string(i), "", nullptr, FlowPin::Output));
                for (int i = 0; i < nt->bang_outputs; i++)
                    node.bang_outputs.push_back(make_pin("", "bang" + std::to_string(i), "", nullptr, FlowPin::BangOutput));
            }
            node.rebuild_pin_ids();
            active().selected_nodes.insert(id);
            break;
        }
    }

    // Recreate internal links
    for (auto& cl : active().clipboard_links) {
        if (cl.from_idx < 0 || cl.from_idx >= (int)new_guids.size()) continue;
        if (cl.to_idx < 0 || cl.to_idx >= (int)new_guids.size()) continue;
        std::string from_id = new_guids[cl.from_idx] + "." + cl.from_pin_name;
        std::string to_id = new_guids[cl.to_idx] + "." + cl.to_pin_name;
        active().graph.add_link(from_id, to_id);
    }

    // Resolve type-based pins for pasted nodes
    resolve_type_based_pins(active().graph);
    mark_dirty();
}

// --- Run/Stop ---

void FlowEditorWindow::draw_toolbar() {
    auto state = build_state_.load();

    bool can_run = (state == BuildState::Idle || state == BuildState::BuildFailed);
    bool can_stop = (state == BuildState::Running);

    if (!can_run) ImGui::BeginDisabled();
    if (ImGui::Button("Run")) {
        run_program(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Run Release")) {
        run_program(true);
    }
    if (!can_run) ImGui::EndDisabled();

    ImGui::SameLine();

    if (!can_stop) ImGui::BeginDisabled();
    if (ImGui::Button("Stop")) {
        stop_program();
    }
    if (!can_stop) ImGui::EndDisabled();

    ImGui::SameLine();

    // Status indicator
    switch (state) {
    case BuildState::Idle:
        ImGui::TextDisabled("Idle");
        break;
    case BuildState::Building:
        ImGui::TextColored({1.0f, 0.8f, 0.0f, 1.0f}, "Building...");
        break;
    case BuildState::Running:
        ImGui::TextColored({0.0f, 1.0f, 0.0f, 1.0f}, "Running");
        break;
    case BuildState::BuildFailed:
        ImGui::TextColored({1.0f, 0.2f, 0.2f, 1.0f}, "Build Failed");
        break;
    }
}

void FlowEditorWindow::run_program(bool release) {
    // Stop existing
    stop_program();

    // Wait for any previous build thread
    if (build_thread_.joinable())
        build_thread_.join();

    // Auto-open build log and clear it
    show_build_log_ = true;
    {
        std::lock_guard<std::mutex> lock(build_log_mutex_);
        build_log_.clear();
    }

    // Auto-save
    auto_save();

    if (active().file_path.empty()) return;

    namespace fs = std::filesystem;

    // Determine paths — nanoc expects a project folder containing main.nano
    fs::path nano_path = fs::absolute(active().file_path);
    fs::path project_dir = nano_path.parent_path();
    std::string source_name = project_dir.filename().string();
    fs::path output_dir = project_dir / ".generated" / source_name;

    // Find nanoc relative to this exe
    fs::path exe_path;
#ifdef _WIN32
    char exe_buf[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_buf, MAX_PATH);
    exe_path = fs::path(exe_buf).parent_path();
#elif defined(__APPLE__)
    {
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::string buf(size, '\0');
        _NSGetExecutablePath(buf.data(), &size);
        exe_path = fs::canonical(buf).parent_path();
    }
#else
    exe_path = fs::canonical("/proc/self/exe").parent_path();
#endif
    fs::path nanoc_path = exe_path / "nanoc.exe";
    if (!fs::exists(nanoc_path))
        nanoc_path = exe_path / "nanoc";

    // vcpkg toolchain (Windows only — Linux/macOS use FetchContent via NanoDeps.cmake)
    std::string tc_str;
#ifdef _WIN32
    {
        const char* vr = std::getenv("VCPKG_ROOT");
        if (!vr) {
            std::lock_guard<std::mutex> lock(build_log_mutex_);
            build_log_ += "Error: VCPKG_ROOT environment variable is not set\n";
            build_state_ = BuildState::BuildFailed;
            return;
        }
        tc_str = (fs::path(vr) / "scripts" / "buildsystems" / "vcpkg.cmake").string();
    }
#endif

    // Capture paths as strings for the thread
    std::string nanoc_str = nanoc_path.string();
    std::string nano_str = project_dir.string();
    std::string out_str = output_dir.string();
    std::string sn = source_name;

    build_state_ = BuildState::Building;
    {
        std::lock_guard<std::mutex> lock(build_log_mutex_);
        build_log_.clear();
    }

    build_thread_ = std::thread([this, nanoc_str, nano_str, out_str, tc_str, sn, release]() {
        namespace fs = std::filesystem;
        fs::create_directories(out_str);

        auto run_cmd = [this](const std::string& cmd) -> int {
#ifdef _WIN32
            // cmd.exe needs the entire command wrapped in quotes when args contain quotes
            std::string full_cmd = "\"" + cmd + " 2>&1\"";
            FILE* pipe = _popen(full_cmd.c_str(), "r");
#else
            std::string full_cmd = cmd + " 2>&1";
            FILE* pipe = popen(full_cmd.c_str(), "r");
#endif
            if (!pipe) return -1;
            char buf[256];
            while (fgets(buf, sizeof(buf), pipe)) {
                std::lock_guard<std::mutex> lock(build_log_mutex_);
                build_log_ += buf;
            }
#ifdef _WIN32
            return _pclose(pipe);
#else
            return pclose(pipe);
#endif
        };

        // Step 1: nanoc
        {
            std::lock_guard<std::mutex> lock(build_log_mutex_);
            build_log_ += "=== Running nanoc ===\n";
        }
        std::string cmd1 = "\"" + nanoc_str + "\" \"" + nano_str + "\" -o \"" + out_str + "\"";
        if (run_cmd(cmd1) != 0) {
            build_state_ = BuildState::BuildFailed;
            return;
        }

        // Step 2: cmake configure (skip if already configured)
        std::string build_dir = out_str + "/build";
        std::string cache_file = build_dir + "/CMakeCache.txt";
        {
            std::ifstream cache_check(cache_file);
            if (!cache_check.good()) {
                {
                    std::lock_guard<std::mutex> lock(build_log_mutex_);
                    build_log_ += "\n=== CMake Configure ===\n";
                }
                std::string cmd2 = "cmake -B \"" + build_dir + "\" -S \"" + out_str + "\"";
                if (!tc_str.empty())
                    cmd2 += " \"-DCMAKE_TOOLCHAIN_FILE=" + tc_str + "\"";
                if (run_cmd(cmd2) != 0) {
                    build_state_ = BuildState::BuildFailed;
                    return;
                }
            } else {
                std::lock_guard<std::mutex> lock(build_log_mutex_);
                build_log_ += "\n=== CMake Configure (cached) ===\n";
            }
        }

        // Step 3: cmake build
        {
            std::lock_guard<std::mutex> lock(build_log_mutex_);
            build_log_ += "\n=== CMake Build ===\n";
        }
        std::string config = release ? "Release" : "Debug";
        std::string cmd3 = "cmake --build \"" + build_dir + "\" --config " + config + " --parallel";
        if (run_cmd(cmd3) != 0) {
            build_state_ = BuildState::BuildFailed;
            return;
        }

        // Step 4: launch exe
#ifdef _WIN32
        fs::path exe_path = fs::path(build_dir) / config / (sn + ".exe");
        if (!fs::exists(exe_path))
            exe_path = fs::path(build_dir) / (sn + ".exe");
#else
        fs::path exe_path = fs::path(build_dir) / sn;
#endif
        if (!fs::exists(exe_path)) {
            std::lock_guard<std::mutex> lock(build_log_mutex_);
            build_log_ += "\nError: executable not found at " + exe_path.string() + "\n";
            build_state_ = BuildState::BuildFailed;
            return;
        }

#ifdef _WIN32
        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        std::string exe_str = exe_path.string();
        if (CreateProcessA(exe_str.c_str(), nullptr, nullptr, nullptr, FALSE,
                          0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            child_process_ = pi.hProcess;
            build_state_ = BuildState::Running;
        } else {
            std::lock_guard<std::mutex> lock(build_log_mutex_);
            build_log_ += "\nError: failed to launch " + exe_str + "\n";
            build_state_ = BuildState::BuildFailed;
        }
#else
        pid_t pid = fork();
        if (pid == 0) {
            execl(exe_path.c_str(), exe_path.c_str(), nullptr);
            _exit(1);
        } else if (pid > 0) {
            child_pid_ = pid;
            build_state_ = BuildState::Running;
        } else {
            std::lock_guard<std::mutex> lock(build_log_mutex_);
            build_log_ += "\nError: fork failed\n";
            build_state_ = BuildState::BuildFailed;
        }
#endif
    });
}

void FlowEditorWindow::stop_program() {
#ifdef _WIN32
    if (child_process_) {
        TerminateProcess(child_process_, 0);
        WaitForSingleObject(child_process_, 1000);
        CloseHandle(child_process_);
        child_process_ = nullptr;
    }
#else
    if (child_pid_ > 0) {
        kill(child_pid_, SIGTERM);
        waitpid(child_pid_, nullptr, 0);
        child_pid_ = 0;
    }
#endif
    build_state_ = BuildState::Idle;
}

void FlowEditorWindow::poll_child_process() {
    if (build_state_.load() != BuildState::Running) return;

#ifdef _WIN32
    if (child_process_) {
        DWORD exit_code;
        if (GetExitCodeProcess(child_process_, &exit_code) && exit_code != STILL_ACTIVE) {
            CloseHandle(child_process_);
            child_process_ = nullptr;
            build_state_ = BuildState::Idle;
        }
    }
#else
    if (child_pid_ > 0) {
        int status;
        pid_t result = waitpid(child_pid_, &status, WNOHANG);
        if (result == child_pid_) {
            child_pid_ = 0;
            build_state_ = BuildState::Idle;
        }
    }
#endif
}
