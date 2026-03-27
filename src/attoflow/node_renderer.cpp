#include "node_renderer.h"
#include "atto_editor_shared_state.h"
#include "tooltip_renderer.h"
#include <cmath>
#include <algorithm>

// ─── build_render_state ───

NodeRenderState build_render_state(const FlowNodeBuilderPtr& node,
                                    const HoverItem& hover_item,
                                    const AttoEditorSharedState* shared) {
    NodeRenderState state;
    state.selected = shared && shared->selected_nodes.count(node) > 0;
    state.node_hovered = false;
    if (auto* ep = std::get_if<BuilderEntryPtr>(&hover_item))
        state.node_hovered = (*ep == node);
    state.pin_hovered_on_this = false;
    if (auto* pin = std::get_if<FlowArg2Ptr>(&hover_item))
        state.pin_hovered_on_this = ((*pin)->node() == node);
    else if (auto* add = std::get_if<AddPinHover>(&hover_item))
        state.pin_hovered_on_this = (add->node == node);
    state.hovered_pin = nullptr;
    if (auto* pp = std::get_if<FlowArg2Ptr>(&hover_item))
        state.hovered_pin = *pp;
    state.add_pin_hover = std::get_if<AddPinHover>(&hover_item);
    return state;
}

// ─── Geometry helpers ───

float point_to_bezier_dist(ImVec2 p, ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3) {
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

static float dist2d(ImVec2 a, ImVec2 b) {
    return std::sqrt((a.x-b.x)*(a.x-b.x) + (a.y-b.y)*(a.y-b.y));
}

ImU32 pin_color(PortKind2 kind) {
    switch (kind) {
    case PortKind2::BangTrigger:
    case PortKind2::BangNext: return S.col_pin_bang;
    case PortKind2::Lambda:   return S.col_pin_lambda;
    default:                  return S.col_pin_data;
    }
}

// ─── VisualPinMap::build ───

VisualPinMap VisualPinMap::build(const FlowNodeBuilderPtr& node, const NodeType2* nt) {
    VisualPinMap vpm;
    if (!nt) return vpm;

    vpm.is_flow = nt->is_flow();
    bool has_input_va = nt->input_ports_va_args != nullptr;

    // Input pins: base args (only Net kind get pins)
    int parsed_size = node->parsed_args ? (int)node->parsed_args->size() : 0;
    if (node->parsed_args) {
        for (int i = 0; i < parsed_size; i++) {
            auto arg = (*node->parsed_args)[i];
            if (arg->is(ArgKind::Net)) {
                PortKind2 pk = PortKind2::Data;
                bool opt = false;
                const PortDesc2* pd = nt->input_port(i);
                if (pd) { pk = pd->kind; opt = pd->optional; }
                vpm.inputs.push_back({VisualPinKind::Base, arg, pd, pk, opt});
            }
        }
    }
    // Absent trailing optional ports
    for (int i = parsed_size; i < nt->total_inputs(); i++) {
        if (i >= nt->num_inputs) {
            const PortDesc2* pd = nt->input_port(i);
            PortKind2 pk = pd ? pd->kind : PortKind2::Data;
            vpm.inputs.push_back({VisualPinKind::AbsentOptional, nullptr, pd, pk, true});
        }
    }
    // Va_args
    if (node->parsed_va_args) {
        PortKind2 va_kind = nt->input_ports_va_args ? nt->input_ports_va_args->kind : PortKind2::Data;
        for (int i = 0; i < (int)node->parsed_va_args->size(); i++) {
            auto arg = (*node->parsed_va_args)[i];
            if (arg->is(ArgKind::Net)) {
                vpm.inputs.push_back({VisualPinKind::VaArg, arg, nt->input_ports_va_args, va_kind, false});
            }
        }
    }
    // +diamond
    if (has_input_va) {
        vpm.add_diamond_va_port = nt->input_ports_va_args;
        vpm.inputs.push_back({VisualPinKind::AddDiamond, nullptr, nt->input_ports_va_args, PortKind2::Data, false});
    }
    // Remaps
    for (int i = 0; i < (int)node->remaps.size(); i++) {
        vpm.inputs.push_back({VisualPinKind::Remap, node->remaps[i], nullptr, PortKind2::Data, false});
    }

    // Output pins
    int skip_sb = nt->is_flow() ? 1 : 0;
    if (skip_sb && !node->outputs.empty()) {
        vpm.has_side_bang = true;
        vpm.side_bang_arg = node->outputs[0];
    }
    // Fixed outputs (skipping side-bang)
    for (int i = skip_sb; i < (int)node->outputs.size(); i++) {
        PortKind2 pk = PortKind2::Data;
        const PortDesc2* pd = (nt->output_ports && i < nt->num_outputs) ? &nt->output_ports[i] : nullptr;
        if (pd) pk = pd->kind;
        vpm.outputs.push_back({VisualPinKind::Base, node->outputs[i], pd, pk, false});
    }
    // Va_args outputs
    PortKind2 out_va_kind = nt->output_ports_va_args ? nt->output_ports_va_args->kind : PortKind2::Data;
    for (int i = 0; i < (int)node->outputs_va_args.size(); i++) {
        vpm.outputs.push_back({VisualPinKind::VaArg, node->outputs_va_args[i], nt->output_ports_va_args, out_va_kind, false});
    }

    return vpm;
}

// ─── compute_node_layout ───

NodeLayout compute_node_layout(const FlowNodeBuilderPtr& node, const VisualPinMap& vpm,
                                ImVec2 canvas_origin, float zoom) {
    auto* nt = find_node_type2(node->type_id);
    std::string display = nt ? nt->name : "?";
    std::string args = node->args_str();
    if (!args.empty()) display += " " + args;

    ImVec2 text_sz = ImGui::CalcTextSize(display.c_str());
    float text_w = text_sz.x * zoom + 16.0f * zoom;

    int num_in = (int)vpm.inputs.size();
    int num_out = (int)vpm.outputs.size();

    float pin_w_top = std::max(0, num_in) * S.pin_spacing * zoom;
    float pin_w_bot = std::max(0, num_out) * S.pin_spacing * zoom;
    float node_w = std::max({S.node_min_width * zoom, text_w, pin_w_top, pin_w_bot});
    float node_h = S.node_height * zoom;

    ImVec2 pos = {canvas_origin.x + node->position.x * zoom,
                  canvas_origin.y + node->position.y * zoom};

    return {pos, node_w, node_h, num_in, num_out, zoom};
}

// ─── render_background ───

void render_background(ImDrawList* dl, ImVec2 canvas_p0, ImVec2 canvas_sz,
                        ImVec2 canvas_offset, float zoom) {
    dl->AddRectFilled(canvas_p0, v2add(canvas_p0, canvas_sz), S.col_bg);

    float grid_step = S.grid_step * zoom;
    if (grid_step > 5.0f) {
        for (float x = fmodf(canvas_offset.x, grid_step); x < canvas_sz.x; x += grid_step)
            dl->AddLine({canvas_p0.x + x, canvas_p0.y}, {canvas_p0.x + x, canvas_p0.y + canvas_sz.y}, S.col_grid);
        for (float y = fmodf(canvas_offset.y, grid_step); y < canvas_sz.y; y += grid_step)
            dl->AddLine({canvas_p0.x, canvas_p0.y + y}, {canvas_p0.x + canvas_sz.x, canvas_p0.y + y}, S.col_grid);
    }
}

// ─── render_node ───

void render_node(ImDrawList* dl, const FlowNodeBuilderPtr& node, const NodeType2* nt,
                 const NodeLayout& layout, const VisualPinMap& vpm,
                 const std::string& display_text, const NodeRenderState& state,
                 float zoom, bool draw_tooltips) {
    if (!nt) return;

    float pr = S.pin_radius * zoom;

    // Special nodes: label and error
    if (nt->is_special()) {
        std::string display;
        if (node->parsed_args && !node->parsed_args->empty()) {
            auto a = (*node->parsed_args)[0];
            if (auto s = a->as_string()) display = s->value();
            else if (auto e = a->as_expr()) display = e->expr();
            else display = node->args_str();
        }

        float font_size = ImGui::GetFontSize() * zoom;
        bool is_error = (node->type_id == NodeTypeID::Error);

        if (is_error) {
            dl->AddRectFilled(layout.pos, {layout.pos.x + layout.width, layout.pos.y + layout.height},
                              S.col_node_err, S.node_rounding * zoom);
            dl->AddRect(layout.pos, {layout.pos.x + layout.width, layout.pos.y + layout.height},
                        S.col_err_border, S.node_rounding * zoom);
        }

        if (font_size > 5.0f) {
            ImVec2 text_sz = ImGui::CalcTextSize(display.c_str());
            float tw = text_sz.x * zoom;
            float cx = layout.pos.x + (layout.width - tw) * 0.5f;
            float cy = layout.pos.y + (layout.height - font_size) * 0.5f;
            dl->AddText(nullptr, font_size, {cx, cy}, S.col_text, display.c_str());
        }
        return;
    }

    // Node body
    ImU32 col = state.selected ? S.col_node_sel : S.col_node;
    if (!node->error.empty()) col = S.col_node_err;
    dl->AddRectFilled(layout.pos, {layout.pos.x + layout.width, layout.pos.y + layout.height},
                      col, S.node_rounding * zoom);
    dl->AddRect(layout.pos, {layout.pos.x + layout.width, layout.pos.y + layout.height},
                state.node_hovered ? S.col_pin_hover : S.col_node_border, S.node_rounding * zoom,
                0, state.node_hovered ? S.highlight_thickness : 1.0f);

    // Text
    float font_size = ImGui::GetFontSize() * zoom;
    if (font_size > 5.0f) {
        ImVec2 text_sz = ImGui::CalcTextSize(display_text.c_str());
        float tw = text_sz.x * zoom;
        float cx = layout.pos.x + (layout.width - tw) * 0.5f;
        float cy = layout.pos.y + (layout.height - font_size) * 0.5f;
        dl->AddText(nullptr, font_size, {cx, cy}, S.col_text, display_text.c_str());
    }

    // ─── Input pins ───
    for (int i = 0; i < (int)vpm.inputs.size(); i++) {
        auto& pin = vpm.inputs[i];
        ImVec2 pp = layout.input_pin_pos(i);

        if (pin.kind == VisualPinKind::AddDiamond) {
            ImU32 pc = S.col_add_pin;
            dl->AddQuadFilled({pp.x, pp.y - pr}, {pp.x + pr, pp.y}, {pp.x, pp.y + pr}, {pp.x - pr, pp.y}, pc);
            float cr = pr * 0.5f;
            float lth = S.add_pin_line * zoom;
            dl->AddLine({pp.x - cr, pp.y}, {pp.x + cr, pp.y}, S.col_add_pin_fg, lth);
            dl->AddLine({pp.x, pp.y - cr}, {pp.x, pp.y + cr}, S.col_add_pin_fg, lth);
            continue;
        }

        ImU32 pc = pin_color(pin.port_kind);
        if (pin.kind == VisualPinKind::AbsentOptional || (pin.is_optional && pin.kind == VisualPinKind::Base)) {
            dl->AddQuadFilled({pp.x, pp.y - pr}, {pp.x + pr, pp.y}, {pp.x, pp.y + pr}, {pp.x - pr, pp.y}, pc);
            if (pin.kind == VisualPinKind::AbsentOptional) {
                float font_sz = pr * 1.6f;
                if (font_sz > 3.0f) {
                    ImVec2 ts = ImGui::CalcTextSize("?");
                    float scale = font_sz / ImGui::GetFontSize();
                    dl->AddText(nullptr, font_sz,
                        {pp.x - ts.x * scale * 0.5f, pp.y - ts.y * scale * 0.5f},
                        S.col_opt_pin_fg, "?");
                }
            }
        } else if (pin.port_kind == PortKind2::BangTrigger) {
            dl->AddRectFilled({pp.x - pr, pp.y - pr}, {pp.x + pr, pp.y + pr}, pc);
        } else if (pin.port_kind == PortKind2::Lambda) {
            dl->AddTriangleFilled({pp.x - pr, pp.y - pr}, {pp.x + pr, pp.y - pr}, {pp.x, pp.y + pr}, pc);
        } else if (pin.kind == VisualPinKind::VaArg) {
            dl->AddQuadFilled({pp.x, pp.y - pr}, {pp.x + pr, pp.y}, {pp.x, pp.y + pr}, {pp.x - pr, pp.y}, pc);
        } else {
            dl->AddCircleFilled(pp, pr, pc);
        }
    }

    // ─── Output pins ───
    for (int i = 0; i < (int)vpm.outputs.size(); i++) {
        auto& pin = vpm.outputs[i];
        ImVec2 pp = layout.output_pin_pos(i);

        ImU32 pc = pin_color(pin.port_kind);
        if (pin.port_kind == PortKind2::BangNext) {
            dl->AddRectFilled({pp.x - pr, pp.y - pr}, {pp.x + pr, pp.y + pr}, pc);
        } else if (pin.kind == VisualPinKind::VaArg) {
            dl->AddQuadFilled({pp.x, pp.y + pr}, {pp.x + pr, pp.y}, {pp.x, pp.y - pr}, {pp.x - pr, pp.y}, pc);
        } else {
            dl->AddCircleFilled(pp, pr, pc);
        }
    }

    // ─── Flow-only: lambda grab (left) and side-bang (right) ───
    if (vpm.is_flow) {
        ImVec2 gp = layout.lambda_grab_pos();
        dl->AddTriangleFilled(
            {gp.x + pr, gp.y - pr}, {gp.x - pr, gp.y}, {gp.x + pr, gp.y + pr},
            S.col_pin_lambda);
        if (state.node_hovered) {
            float ho = S.highlight_offset * zoom;
            dl->AddTriangle(
                {gp.x + pr + ho, gp.y - pr - ho}, {gp.x - pr - ho, gp.y}, {gp.x + pr + ho, gp.y + pr + ho},
                S.col_pin_hover, S.highlight_thickness);
        }

        ImVec2 bp = layout.side_bang_pos();
        dl->AddRectFilled({bp.x - pr, bp.y - pr}, {bp.x + pr, bp.y + pr}, S.col_pin_bang);
    }

    // ─── Hover highlights ───
    if (!state.node_hovered && !state.pin_hovered_on_this) return;

    float ho = S.highlight_offset * zoom;
    ImU32 COL_HOVER = S.col_pin_hover;
    float ht = S.highlight_thickness;

    enum class PinShape { Circle, Square, Diamond, TriangleDown, TriangleLeft };
    auto draw_highlight = [&](ImVec2 pos, PinShape shape) {
        switch (shape) {
        case PinShape::Circle:       dl->AddCircle(pos, pr + ho, COL_HOVER, 0, ht); break;
        case PinShape::Square:       dl->AddRect({pos.x-pr-ho,pos.y-pr-ho},{pos.x+pr+ho,pos.y+pr+ho}, COL_HOVER, 0, 0, ht); break;
        case PinShape::Diamond:      dl->AddQuad({pos.x,pos.y-pr-ho},{pos.x+pr+ho,pos.y},{pos.x,pos.y+pr+ho},{pos.x-pr-ho,pos.y}, COL_HOVER, ht); break;
        case PinShape::TriangleDown: dl->AddTriangle({pos.x-pr-ho,pos.y-pr-ho},{pos.x+pr+ho,pos.y-pr-ho},{pos.x,pos.y+pr+ho}, COL_HOVER, ht); break;
        case PinShape::TriangleLeft: dl->AddTriangle({pos.x+pr+ho,pos.y-pr-ho},{pos.x-pr-ho,pos.y},{pos.x+pr+ho,pos.y+pr+ho}, COL_HOVER, ht); break;
        }
    };

    auto pin_shape_for = [](const VisualPin& pin) -> PinShape {
        if (pin.kind == VisualPinKind::VaArg || pin.kind == VisualPinKind::AddDiamond) return PinShape::Diamond;
        if (pin.is_optional || pin.kind == VisualPinKind::AbsentOptional) return PinShape::Diamond;
        if (pin.port_kind == PortKind2::BangTrigger || pin.port_kind == PortKind2::BangNext) return PinShape::Square;
        if (pin.port_kind == PortKind2::Lambda) return PinShape::TriangleDown;
        return PinShape::Circle;
    };

    // +diamond hover
    if (state.add_pin_hover && state.add_pin_hover->node == node) {
        for (int i = 0; i < (int)vpm.inputs.size(); i++) {
            if (vpm.inputs[i].kind == VisualPinKind::AddDiamond) {
                draw_highlight(layout.input_pin_pos(i), PinShape::Diamond);
                if (draw_tooltips)
                    tooltip_add_diamond(*state.add_pin_hover);
                return;
            }
        }
    }

    if (state.hovered_pin) {
        // Input pins
        for (int i = 0; i < (int)vpm.inputs.size(); i++) {
            auto& pin = vpm.inputs[i];
            if (pin.kind == VisualPinKind::AddDiamond || pin.kind == VisualPinKind::AbsentOptional) continue;
            if (pin.arg == state.hovered_pin) {
                draw_highlight(layout.input_pin_pos(i), pin_shape_for(pin));
                if (draw_tooltips)
                    tooltip_input_pin(pin);
                return;
            }
        }
        // Output pins
        for (int i = 0; i < (int)vpm.outputs.size(); i++) {
            auto& pin = vpm.outputs[i];
            if (pin.arg == state.hovered_pin) {
                draw_highlight(layout.output_pin_pos(i), pin_shape_for(pin));
                if (draw_tooltips)
                    tooltip_output_pin(pin, i);
                return;
            }
        }
        // Side-bang
        if (vpm.has_side_bang && vpm.side_bang_arg == state.hovered_pin) {
            draw_highlight(layout.side_bang_pos(), PinShape::Square);
            if (draw_tooltips)
                tooltip_side_bang();
            return;
        }
    }

    // Node body tooltip
    if (state.node_hovered && draw_tooltips)
        tooltip_node_body(node);
}

// ─── Wire rendering ───

void render_wire(ImDrawList* dl, const WireInfo& w, float zoom) {
    bool is_lambda = w.is_lambda();
    bool named = false;
    if (!is_lambda) {
        if (auto net = w.entry_->as_net())
            named = !net->auto_wire();
    }
    ImU32 wire_col = is_lambda ? S.col_wire_lambda : (named ? S.col_wire_named : S.col_wire);
    float th = S.wire_thickness * zoom;
    dl->AddBezierCubic(w.p0, w.p1, w.p2, w.p3, wire_col, th);
}

void render_wire_label(ImDrawList* dl, const WireInfo& w, float zoom) {
    if (w.is_lambda()) return;
    auto net = w.entry_ ? w.entry_->as_net() : nullptr;
    if (!net || net->auto_wire()) return;

    float font_size = ImGui::GetFontSize() * zoom * 0.8f;
    if (font_size <= 5.0f) return;

    ImVec2 mid = {(w.p0.x + w.p3.x) * 0.5f, (w.p0.y + w.p3.y) * 0.5f};
    ImVec2 text_sz = ImGui::CalcTextSize(w.net_id.c_str());
    float tw = text_sz.x * (font_size / ImGui::GetFontSize());
    float tth = text_sz.y * (font_size / ImGui::GetFontSize());
    float cx = mid.x - tw * 0.5f;
    float cy = mid.y - tth * 0.5f;
    dl->AddRectFilled({cx - 3, cy - 1}, {cx + tw + 3, cy + tth + 1},
                      S.col_label_bg, S.node_rounding);
    dl->AddText(nullptr, font_size, {cx, cy}, S.col_label_text, w.net_id.c_str());
}

void render_wire_highlight(ImDrawList* dl, const WireInfo& w, float zoom) {
    float th = (S.wire_thickness + 2.0f) * zoom;
    dl->AddBezierCubic(w.p0, w.p1, w.p2, w.p3, S.col_pin_hover, th);
}

void render_selection_rect(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
    dl->AddRectFilled(p0, p1, IM_COL32(100, 130, 200, 40));
    dl->AddRect(p0, p1, IM_COL32(100, 130, 200, 180), 0, 0, 1.5f);
}

// ─── compute_wire_geometry ───

WireInfo compute_wire_geometry(ImVec2 from, ImVec2 to, bool is_lambda, bool is_side_bang,
                                float zoom, const BuilderEntryPtr& entry,
                                const NodeId& src_id, const NodeId& dst_id, const NodeId& net_id) {
    ImVec2 cp1, cp2;
    if (is_lambda) {
        float dx = std::max(std::abs(to.x - from.x) * 0.5f, 30.0f * zoom);
        float dy = std::max(std::abs(to.y - from.y) * 0.5f, 30.0f * zoom);
        cp1 = {from.x - dx, from.y};
        cp2 = {to.x, to.y - dy};
    } else if (is_side_bang) {
        float dx = std::max(std::abs(to.x - from.x) * 0.5f, 30.0f * zoom);
        float dy = std::max(std::abs(to.y - from.y) * 0.5f, 30.0f * zoom);
        cp1 = {from.x + dx, from.y};
        cp2 = {to.x, to.y - dy};
    } else {
        float dy = std::max(std::abs(to.y - from.y) * 0.5f, 30.0f * zoom);
        cp1 = {from.x, from.y + dy};
        cp2 = {to.x, to.y - dy};
    }
    return {entry, from, cp1, cp2, to, src_id, dst_id, net_id};
}

// ─── Hit-testing ───

HitResult hit_test_wires(ImVec2 mouse, const std::vector<WireInfo>& wires, float zoom) {
    HitResult best;
    float wire_thresh = S.wire_hit_threshold * zoom;
    for (auto& w : wires) {
        float d = point_to_bezier_dist(mouse, w.p0, w.p1, w.p2, w.p3);
        if (d < wire_thresh && d < best.distance) {
            best.distance = d;
            best.item = w.entry();
        }
    }
    return best;
}

HitResult hit_test_node_bodies(ImVec2 mouse, const std::vector<NodeHitTarget>& nodes, float zoom) {
    HitResult best;
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
        auto& layout = *it->layout;
        float nd;
        bool inside = mouse.x >= layout.pos.x && mouse.x <= layout.pos.x + layout.width &&
                      mouse.y >= layout.pos.y && mouse.y <= layout.pos.y + layout.height;
        if (inside) {
            float dl_ = mouse.x - layout.pos.x;
            float dr = layout.pos.x + layout.width - mouse.x;
            float dt = mouse.y - layout.pos.y;
            float db = layout.pos.y + layout.height - mouse.y;
            nd = std::min({dl_, dr, dt, db});
        } else {
            float cx = std::clamp(mouse.x, layout.pos.x, layout.pos.x + layout.width);
            float cy = std::clamp(mouse.y, layout.pos.y, layout.pos.y + layout.height);
            nd = dist2d(mouse, {cx, cy});
        }
        if (nd < S.pin_radius * zoom * S.node_hit_threshold_mul && nd < best.distance) {
            best.distance = nd;
            best.item = BuilderEntryPtr(it->node);
        }
    }
    return best;
}

HitResult hit_test_pins(ImVec2 mouse, const std::vector<NodeHitTarget>& nodes, float zoom) {
    HitResult best;
    float pin_thresh = S.pin_radius * zoom * S.pin_hit_radius_mul;
    float pin_bias = S.pin_priority_bias;

    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
        auto& node = it->node;
        auto& layout = *it->layout;
        auto& vpm = *it->vpm;

        // Input pins
        for (int i = 0; i < (int)vpm.inputs.size(); i++) {
            auto& pin = vpm.inputs[i];
            if (pin.kind == VisualPinKind::AbsentOptional) continue;
            float pd = dist2d(mouse, layout.input_pin_pos(i));
            if (pin.kind == VisualPinKind::AddDiamond) {
                if (pd < pin_thresh && vpm.add_diamond_va_port) {
                    float biased = pd - pin_bias;
                    if (biased < best.distance) {
                        best.distance = biased;
                        best.item = AddPinHover{node, vpm.add_diamond_va_port, true};
                    }
                }
                continue;
            }
            if (pd < pin_thresh && pin.arg) {
                float biased = pd - pin_bias;
                if (biased < best.distance) {
                    best.distance = biased;
                    best.item = pin.arg;
                }
            }
        }

        // Output pins
        for (int i = 0; i < (int)vpm.outputs.size(); i++) {
            auto& pin = vpm.outputs[i];
            float pd = dist2d(mouse, layout.output_pin_pos(i));
            if (pd < pin_thresh && pin.arg) {
                float biased = pd - pin_bias;
                if (biased < best.distance) {
                    best.distance = biased;
                    best.item = pin.arg;
                }
            }
        }

        // Lambda grab → node itself
        if (vpm.is_flow) {
            float pd = dist2d(mouse, layout.lambda_grab_pos());
            if (pd < pin_thresh) {
                float biased = pd - pin_bias;
                if (biased < best.distance) {
                    best.distance = biased;
                    best.item = BuilderEntryPtr(node);
                }
            }
        }

        // Side-bang
        if (vpm.has_side_bang && vpm.side_bang_arg) {
            float pd = dist2d(mouse, layout.side_bang_pos());
            if (pd < pin_thresh) {
                float biased = pd - pin_bias;
                if (biased < best.distance) {
                    best.distance = biased;
                    best.item = vpm.side_bang_arg;
                }
            }
        }
    }

    return best;
}
