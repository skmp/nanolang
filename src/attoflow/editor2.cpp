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

// ─── Style (global instance) ───

Editor2Style S;

// ─── Helpers ───

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

static inline ImVec2 v2add(ImVec2 a, ImVec2 b) { return {a.x + b.x, a.y + b.y}; }
static inline ImVec2 v2sub(ImVec2 a, ImVec2 b) { return {a.x - b.x, a.y - b.y}; }
static inline ImVec2 v2mul(ImVec2 a, float s) { return {a.x * s, a.y * s}; }

// ─── PinMapping::build ───

PinMapping PinMapping::build(const FlowNodeBuilderPtr& node, const NodeType2* nt) {
    PinMapping m;
    m.has_input_va = nt && nt->input_ports_va_args != nullptr;
    int parsed_size = node->parsed_args ? (int)node->parsed_args->size() : 0;

    if (node->parsed_args) {
        for (int i = 0; i < parsed_size; i++) {
            if ((*node->parsed_args)[i]->is(ArgKind::Net)) {
                m.pin_to_port.push_back(i);
                m.base_count++;
            }
        }
    }
    if (nt) {
        for (int i = parsed_size; i < nt->total_inputs(); i++) {
            if (i >= nt->num_inputs) {
                m.pin_to_port.push_back(-3000 - i);
                m.base_count++;
            }
        }
    }
    if (node->parsed_va_args) {
        for (int i = 0; i < (int)node->parsed_va_args->size(); i++) {
            if ((*node->parsed_va_args)[i]->is(ArgKind::Net)) {
                m.pin_to_port.push_back(-(i + 1));
                m.va_count++;
            }
        }
    }
    if (m.has_input_va) {
        m.add_pin_pos = (int)m.pin_to_port.size();
        m.pin_to_port.push_back(-1000);
    }
    for (int i = 0; i < (int)node->remaps.size(); i++) {
        m.pin_to_port.push_back(-2000 - i);
    }
    return m;
}

// ─── compute_node_layout ───

NodeLayout compute_node_layout(const FlowNodeBuilderPtr& node, ImVec2 canvas_origin, float zoom) {
    auto* nt = find_node_type2(node->type_id);
    std::string display = nt ? nt->name : "?";
    std::string args = node->args_str();
    if (!args.empty()) display += " " + args;

    float font_size = ImGui::GetFontSize() * zoom;
    ImVec2 text_sz = ImGui::CalcTextSize(display.c_str());
    float text_w = text_sz.x * zoom + 16.0f * zoom;

    auto pm = PinMapping::build(node, nt);
    int num_in = pm.total();

    int fixed_out = nt ? nt->num_outputs : (int)node->outputs.size();
    int skip_side_bang = 0;
    if (nt && nt->is_flow()) {
        if (node->outputs.empty())
            throw std::logic_error("Flow node '" + node->id() + "' must have at least one output (side-bang)");
        skip_side_bang = 1;
    }
    int va_out = (int)node->outputs_va_args.size();
    int num_out = std::max(0, fixed_out - skip_side_bang) + va_out;

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

// ─── Per-item editor implementations ───

void NodeEditorImpl::rebuild(ImVec2 canvas_origin, float zoom) {
    auto* nt = find_node_type2(node->type_id);
    layout = compute_node_layout(node, canvas_origin, zoom);
    pin_mapping = PinMapping::build(node, nt);
    display_text = nt ? nt->name : "?";
    std::string args = node->args_str();
    if (!args.empty()) display_text += " " + args;
    has_error = !node->error.empty();
}

void NodeEditorImpl::node_mutated(const std::shared_ptr<FlowNodeBuilder>&) {
    pane->invalidate_wires();
}

void NodeEditorImpl::node_layout_changed(const std::shared_ptr<FlowNodeBuilder>&) {
    pane->invalidate_wires();
}

std::shared_ptr<IArgNetEditor> NodeEditorImpl::create_arg_net_editor(const std::shared_ptr<ArgNet2>& arg) {
    return std::make_shared<ArgNetEditorImpl>(pane, arg);
}
std::shared_ptr<IArgNumberEditor> NodeEditorImpl::create_arg_number_editor(const std::shared_ptr<ArgNumber2>& arg) {
    return std::make_shared<ArgNumberEditorImpl>(pane, arg);
}
std::shared_ptr<IArgStringEditor> NodeEditorImpl::create_arg_string_editor(const std::shared_ptr<ArgString2>& arg) {
    return std::make_shared<ArgStringEditorImpl>(pane, arg);
}
std::shared_ptr<IArgExprEditor> NodeEditorImpl::create_arg_expr_editor(const std::shared_ptr<ArgExpr2>& arg) {
    return std::make_shared<ArgExprEditorImpl>(pane, arg);
}

void NetEditorImpl::net_mutated(const std::shared_ptr<NetBuilder>&) {
    pane->invalidate_wires();
}

// Arg editor callbacks — structural changes bubble up via node_mutated, so these are no-ops for now
void ArgNetEditorImpl::arg_net_mutated(const std::shared_ptr<ArgNet2>&) {}
void ArgNumberEditorImpl::arg_number_mutated(const std::shared_ptr<ArgNumber2>&) {}
void ArgStringEditorImpl::arg_string_mutated(const std::shared_ptr<ArgString2>&) {}
void ArgExprEditorImpl::arg_expr_mutated(const std::shared_ptr<ArgExpr2>&) {}

// ─── IGraphEditor implementation ───

std::shared_ptr<INodeEditor> Editor2Pane::node_added(const NodeId& id, const std::shared_ptr<FlowNodeBuilder>& node) {
    auto ned = std::make_shared<NodeEditorImpl>(this, node);
    node_editors_[id] = ned;
    wires_dirty_ = true;
    return ned;
}

void Editor2Pane::node_removed(const NodeId& id) {
    node_editors_.erase(id);
    wires_dirty_ = true;
}

std::shared_ptr<INetEditor> Editor2Pane::net_added(const NodeId& id, const std::shared_ptr<NetBuilder>& net) {
    auto ned = std::make_shared<NetEditorImpl>(this, net);
    net_editors_[id] = ned;
    wires_dirty_ = true;
    return ned;
}

void Editor2Pane::net_removed(const NodeId& id) {
    net_editors_.erase(id);
    wires_dirty_ = true;
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

    auto slash = path.find_last_of("/\\");
    tab_name_ = (slash != std::string::npos) ? path.substr(slash + 1) : path;

    // Register as editor — triggers node_added/net_added for all existing entries
    gb_->add_editor(shared_from_this());

    printf("Editor2: loaded %zu entries from %s\n", gb_->entries.size(), path.c_str());
    return true;
}

// ─── Wire rebuilding ───

void Editor2Pane::rebuild_wires(ImVec2 canvas_origin) {
    cached_wires_.clear();

    for (auto& [dst_id, ned] : node_editors_) {
        auto dst_node = ned->node;
        auto* dst_nt = find_node_type2(dst_node->type_id);
        if (!dst_nt) continue;

        // Rebuild layout for this node at current zoom
        ned->rebuild(canvas_origin, canvas_zoom_);
        auto& dst_layout = ned->layout;
        auto& dst_pm = ned->pin_mapping;

        auto draw_wire_to_pin = [&](int dst_pin, const BuilderEntryPtr& entry, const NodeId& net_id) {
            if (!entry) return;

            FlowNodeBuilderPtr src_node = nullptr;
            bool named = false;
            bool is_lambda = false;
            int source_pin = 0;

            if (auto net = entry->as_net()) {
                if (net->is_the_unconnected()) return;
                auto src_ptr = net->source().lock();
                src_node = src_ptr ? src_ptr->as_node() : nullptr;
                if (!src_node) return;
                named = !net->auto_wire();
                for (int k = 0; k < (int)src_node->outputs.size(); k++) {
                    auto out_net = src_node->outputs[k]->as_net();
                    if (out_net && out_net->second() == entry) {
                        source_pin = k;
                        break;
                    }
                }
                if (source_pin == 0) {
                    int base = (int)src_node->outputs.size();
                    for (int k = 0; k < (int)src_node->outputs_va_args.size(); k++) {
                        auto out_net = src_node->outputs_va_args[k]->as_net();
                        if (out_net && out_net->second() == entry) {
                            source_pin = base + k;
                            break;
                        }
                    }
                }
            } else if (auto node = entry->as_node()) {
                src_node = node;
                is_lambda = true;
            } else {
                return;
            }

            auto* src_nt = find_node_type2(src_node->type_id);
            auto src_layout = compute_node_layout(src_node, canvas_origin, canvas_zoom_);
            ImVec2 to = dst_layout.input_pin_pos(dst_pin);

            ImVec2 from, cp1, cp2;
            ImU32 wire_col;
            if (is_lambda) {
                from = src_layout.lambda_grab_pos();
                float dx = std::max(std::abs(to.x - from.x) * 0.5f, 30.0f * canvas_zoom_);
                float dy = std::max(std::abs(to.y - from.y) * 0.5f, 30.0f * canvas_zoom_);
                cp1 = {from.x - dx, from.y}; cp2 = {to.x, to.y - dy};
                wire_col = S.col_wire_lambda;
            } else {
                bool is_side_bang = src_nt && src_nt->is_flow() &&
                    source_pin < (src_nt->num_outputs) &&
                    src_nt->output_ports && src_nt->output_ports[source_pin].kind == PortKind2::BangNext;

                if (is_side_bang) {
                    from = {src_layout.pos.x + src_layout.width, src_layout.pos.y + src_layout.height * 0.5f};
                    float dx = std::max(std::abs(to.x - from.x) * 0.5f, 30.0f * canvas_zoom_);
                    float dy = std::max(std::abs(to.y - from.y) * 0.5f, 30.0f * canvas_zoom_);
                    cp1 = {from.x + dx, from.y}; cp2 = {to.x, to.y - dy};
                } else {
                    int visual_pin = source_pin;
                    if (src_nt && src_nt->is_flow()) visual_pin = std::max(0, visual_pin - 1);
                    from = src_layout.output_pin_pos(visual_pin);
                    float dy = std::max(std::abs(to.y - from.y) * 0.5f, 30.0f * canvas_zoom_);
                    cp1 = {from.x, from.y + dy}; cp2 = {to.x, to.y - dy};
                }
                wire_col = named ? S.col_wire_named : S.col_wire;
            }

            cached_wires_.push_back({entry, from, cp1, cp2, to, src_node->id(), dst_id, net_id});
        };

        for (int i = 0; i < dst_pm.total(); i++) {
            if (dst_pm.is_add_diamond(i)) continue;
            if (dst_pm.is_absent_optional(i)) continue;
            if (dst_pm.is_base(i)) {
                int port = dst_pm.port_index(i);
                if (dst_node->parsed_args && port < (int)dst_node->parsed_args->size()) {
                    if (auto an = (*dst_node->parsed_args)[port]->as_net())
                        draw_wire_to_pin(i, an->second(), an->first());
                }
            } else if (dst_pm.is_input_va(i)) {
                int va_idx = -(dst_pm.port_index(i) + 1);
                if (dst_node->parsed_va_args && va_idx < (int)dst_node->parsed_va_args->size()) {
                    if (auto an = (*dst_node->parsed_va_args)[va_idx]->as_net())
                        draw_wire_to_pin(i, an->second(), an->first());
                }
            } else if (dst_pm.is_remap(i)) {
                int ri = dst_pm.remap_index(i);
                if (ri < (int)dst_node->remaps.size()) {
                    if (auto an = dst_node->remaps[ri]->as_net())
                        draw_wire_to_pin(i, an->second(), an->first());
                }
            }
        }
    }

    wires_dirty_ = false;
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

    // Rebuild node layouts and draw nodes
    for (auto& [id, ned] : node_editors_) {
        if (ned->node->shadow)
            throw std::logic_error("Editor2Pane: shadow nodes must be folded before rendering (id: " + id + ")");
        ned->rebuild(canvas_origin, canvas_zoom_);
        draw_node(dl, ned, canvas_origin);
    }

    // Rebuild wires if dirty (always dirty after layout rebuild since positions depend on zoom/pan)
    rebuild_wires(canvas_origin);

    // Draw wires
    for (auto& w : cached_wires_) {
        // Determine wire color
        bool is_lambda = w.is_lambda();
        bool named = false;
        if (!is_lambda) {
            if (auto net = w.entry_->as_net())
                named = !net->auto_wire();
        }
        ImU32 wire_col = is_lambda ? S.col_wire_lambda : (named ? S.col_wire_named : S.col_wire);
        float th = S.wire_thickness * canvas_zoom_;
        dl->AddBezierCubic(w.p0, w.p1, w.p2, w.p3, wire_col, th);

        // Label for named nets
        if (named) {
            float font_size = ImGui::GetFontSize() * canvas_zoom_ * 0.8f;
            if (font_size > 5.0f) {
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
        }
    }

    dl->PopClipRect();

    // ─── Hover detection + effects ───
    if (canvas_hovered) {
        ImVec2 mouse = ImGui::GetIO().MousePos;
        hover_item_ = detect_hover(mouse, canvas_origin);
    } else {
        hover_item_ = std::monostate{};
    }
    draw_hover_effects(dl, canvas_origin, hover_item_);

    // Extract hover node from variant
    FlowNodeBuilderPtr hover_node = nullptr;
    if (auto* ep = std::get_if<BuilderEntryPtr>(&hover_item_)) {
        if (*ep) hover_node = (*ep)->as_node();
    } else if (auto* pin = std::get_if<FlowArg2Ptr>(&hover_item_)) {
        hover_node = (*pin)->node();
    }

    // ─── Selection + dragging with left mouse ───
    if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        bool ctrl = ImGui::GetIO().KeyCtrl;

        if (ctrl && hover_node) {
            if (selected_nodes_.count(hover_node))
                selected_nodes_.erase(hover_node);
            else
                selected_nodes_.insert(hover_node);
        } else if (hover_node) {
            if (!selected_nodes_.count(hover_node)) {
                selected_nodes_.clear();
                selected_nodes_.insert(hover_node);
            }
            dragging_started_ = true;

            drag_was_overlapping_ = false;
            float pad = S.node_height * 0.5f;
            for (auto& sel : selected_nodes_) {
                auto sel_layout = compute_node_layout(sel, {0,0}, 1.0f);
                for (auto& [oid, oned] : node_editors_) {
                    auto on = oned->node;
                    if (selected_nodes_.count(on)) continue;
                    auto ol = compute_node_layout(on, {0,0}, 1.0f);
                    if (sel->position.x < on->position.x - pad + ol.width + pad * 2 &&
                        sel->position.x + sel_layout.width > on->position.x - pad &&
                        sel->position.y < on->position.y - pad + ol.height + pad * 2 &&
                        sel->position.y + sel_layout.height > on->position.y - pad) {
                        drag_was_overlapping_ = true;
                        break;
                    }
                }
                if (drag_was_overlapping_) break;
            }
        } else {
            selected_nodes_.clear();
            selection_rect_active_ = true;
            ImVec2 mouse = ImGui::GetIO().MousePos;
            selection_rect_start_ = {(mouse.x - canvas_origin.x) / canvas_zoom_,
                                     (mouse.y - canvas_origin.y) / canvas_zoom_};
        }
    }

    // Drag all selected nodes
    if (dragging_started_ && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !selected_nodes_.empty()) {
        ImVec2 delta = ImGui::GetIO().MouseDelta;
        float dx = delta.x / canvas_zoom_;
        float dy = delta.y / canvas_zoom_;

        bool blocked = false;
        if (!drag_was_overlapping_) {
            float pad = S.node_height * 0.5f;
            for (auto& sel : selected_nodes_) {
                auto sel_layout = compute_node_layout(sel, {0,0}, 1.0f);
                float nx = sel->position.x + dx, ny = sel->position.y + dy;
                for (auto& [oid, oned] : node_editors_) {
                    auto on = oned->node;
                    if (selected_nodes_.count(on)) continue;
                    auto ol = compute_node_layout(on, {0,0}, 1.0f);
                    float ox = on->position.x - pad, oy = on->position.y - pad;
                    float ow = ol.width + pad * 2, oh = ol.height + pad * 2;
                    if (nx < ox + ow && nx + sel_layout.width > ox &&
                        ny < oy + oh && ny + sel_layout.height > oy) {
                        blocked = true;
                        break;
                    }
                }
                if (blocked) break;
            }
        }
        if (!blocked) {
            for (auto& sel : selected_nodes_) {
                sel->position.x += dx;
                sel->position.y += dy;
            }
            wires_dirty_ = true;
        }
    }

    // Selection rectangle
    if (selection_rect_active_) {
        ImVec2 mouse = ImGui::GetIO().MousePos;
        ImVec2 cur_canvas = {(mouse.x - canvas_origin.x) / canvas_zoom_,
                             (mouse.y - canvas_origin.y) / canvas_zoom_};

        float x0 = std::min(selection_rect_start_.x, cur_canvas.x);
        float y0 = std::min(selection_rect_start_.y, cur_canvas.y);
        float x1 = std::max(selection_rect_start_.x, cur_canvas.x);
        float y1 = std::max(selection_rect_start_.y, cur_canvas.y);

        ImVec2 sp0 = {canvas_origin.x + x0 * canvas_zoom_, canvas_origin.y + y0 * canvas_zoom_};
        ImVec2 sp1 = {canvas_origin.x + x1 * canvas_zoom_, canvas_origin.y + y1 * canvas_zoom_};
        dl->AddRectFilled(sp0, sp1, IM_COL32(100, 130, 200, 40));
        dl->AddRect(sp0, sp1, IM_COL32(100, 130, 200, 180), 0, 0, 1.5f);

        selected_nodes_.clear();
        for (auto& [id, ned] : node_editors_) {
            auto node = ned->node;
            auto layout = compute_node_layout(node, {0,0}, 1.0f);
            float nx0 = node->position.x, ny0 = node->position.y;
            float nx1 = nx0 + layout.width, ny1 = ny0 + layout.height;
            if (nx0 < x1 && nx1 > x0 && ny0 < y1 && ny1 > y0)
                selected_nodes_.insert(node);
        }
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        dragging_started_ = false;
        selection_rect_active_ = false;
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
            ImVec2 mouse = ImGui::GetIO().MousePos;
            ImVec2 mouse_rel = v2sub(v2sub(mouse, canvas_p0), canvas_offset_);
            ImVec2 mouse_canvas = v2mul(mouse_rel, 1.0f / old_zoom);
            canvas_offset_ = v2sub(v2sub(mouse, canvas_p0), v2mul(mouse_canvas, canvas_zoom_));
        }
    }
}

// ─── Draw a node ───

void Editor2Pane::draw_node(ImDrawList* dl, const std::shared_ptr<NodeEditorImpl>& ned,
                             ImVec2 canvas_origin) {
    auto& node = ned->node;
    auto* nt = find_node_type2(node->type_id);
    if (!nt) return;

    auto& layout = ned->layout;
    auto& pm = ned->pin_mapping;

    // Special nodes: label and error
    if (nt->is_special()) {
        std::string display;
        if (node->parsed_args && !node->parsed_args->empty()) {
            auto a = (*node->parsed_args)[0];
            if (auto s = a->as_string()) display = s->value();
            else if (auto e = a->as_expr()) display = e->expr();
            else display = node->args_str();
        }

        float font_size = ImGui::GetFontSize() * canvas_zoom_;
        bool is_error = (node->type_id == NodeTypeID::Error);

        if (is_error) {
            dl->AddRectFilled(layout.pos, {layout.pos.x + layout.width, layout.pos.y + layout.height},
                              S.col_node_err, S.node_rounding * canvas_zoom_);
            dl->AddRect(layout.pos, {layout.pos.x + layout.width, layout.pos.y + layout.height},
                        S.col_err_border, S.node_rounding * canvas_zoom_);
        }

        if (font_size > 5.0f) {
            ImVec2 text_sz = ImGui::CalcTextSize(display.c_str());
            float tw = text_sz.x * canvas_zoom_;
            float cx = layout.pos.x + (layout.width - tw) * 0.5f;
            float cy = layout.pos.y + (layout.height - font_size) * 0.5f;
            dl->AddText(nullptr, font_size, {cx, cy}, S.col_text, display.c_str());
        }
        return;
    }

    bool selected = selected_nodes_.count(node);
    bool node_hovered = false;
    if (auto* ep = std::get_if<BuilderEntryPtr>(&hover_item_))
        node_hovered = (*ep == node);

    bool pin_hovered_on_this = false;
    if (auto* pin = std::get_if<FlowArg2Ptr>(&hover_item_))
        pin_hovered_on_this = ((*pin)->node() == node);
    else if (auto* add = std::get_if<AddPinHover>(&hover_item_))
        pin_hovered_on_this = (add->node == node);
    bool has_error = ned->has_error;

    ImU32 col = has_error ? S.col_node_err : (selected ? S.col_node_sel : S.col_node);
    dl->AddRectFilled(layout.pos, {layout.pos.x + layout.width, layout.pos.y + layout.height},
                      col, S.node_rounding * canvas_zoom_);
    dl->AddRect(layout.pos, {layout.pos.x + layout.width, layout.pos.y + layout.height},
                node_hovered ? S.col_pin_hover : S.col_node_border, S.node_rounding * canvas_zoom_,
                0, node_hovered ? S.highlight_thickness : 1.0f);

    // Text
    float font_size = ImGui::GetFontSize() * canvas_zoom_;
    if (font_size > 5.0f) {
        ImVec2 text_sz = ImGui::CalcTextSize(ned->display_text.c_str());
        float tw = text_sz.x * canvas_zoom_;
        float cx = layout.pos.x + (layout.width - tw) * 0.5f;
        float cy = layout.pos.y + (layout.height - font_size) * 0.5f;
        dl->AddText(nullptr, font_size, {cx, cy}, S.col_text, ned->display_text.c_str());
    }

    // Draw input pins (top) using PinMapping
    float pr = S.pin_radius * canvas_zoom_;
    for (int i = 0; i < layout.num_in; i++) {
        ImVec2 pp = layout.input_pin_pos(i);

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
        bool is_input_va = pm.is_input_va(i);
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
        } else if (is_input_va) {
            kind = nt->input_ports_va_args ? nt->input_ports_va_args->kind : PortKind2::Data;
        }

        ImU32 pc = pin_color(kind);
        if (kind == PortKind2::BangTrigger) {
            dl->AddRectFilled({pp.x - pr, pp.y - pr}, {pp.x + pr, pp.y + pr}, pc);
        } else if (kind == PortKind2::Lambda) {
            dl->AddTriangleFilled({pp.x - pr, pp.y - pr}, {pp.x + pr, pp.y - pr}, {pp.x, pp.y + pr}, pc);
        } else if (is_input_va) {
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
    int skip_sb = (nt->is_flow()) ? 1 : 0;
    int fixed_out = nt->num_outputs;
    int rendered_fixed = fixed_out - skip_sb;
    for (int i = 0; i < layout.num_out; i++) {
        ImVec2 pp = layout.output_pin_pos(i);
        int out_idx = i + skip_sb;
        bool is_output_va = (i >= rendered_fixed);

        PortKind2 kind = PortKind2::Data;
        if (!is_output_va && nt->output_ports && out_idx < nt->num_outputs)
            kind = nt->output_ports[out_idx].kind;
        else if (is_output_va && nt->output_ports_va_args)
            kind = nt->output_ports_va_args->kind;

        ImU32 pc = pin_color(kind);
        if (kind == PortKind2::BangNext) {
            dl->AddRectFilled({pp.x - pr, pp.y - pr}, {pp.x + pr, pp.y + pr}, pc);
        } else if (is_output_va) {
            dl->AddQuadFilled({pp.x, pp.y + pr}, {pp.x + pr, pp.y}, {pp.x, pp.y - pr}, {pp.x - pr, pp.y}, pc);
        } else {
            dl->AddCircleFilled(pp, pr, pc);
        }
    }

    // Flow-only: lambda grab (left) and side-bang (right)
    if (nt->is_flow()) {
        ImVec2 gp = layout.lambda_grab_pos();
        ImU32 lc = S.col_pin_lambda;
        dl->AddTriangleFilled(
            {gp.x + pr, gp.y - pr},
            {gp.x - pr, gp.y},
            {gp.x + pr, gp.y + pr},
            lc);
        if (node_hovered) {
            float ho = S.highlight_offset * canvas_zoom_;
            dl->AddTriangle(
                {gp.x + pr + ho, gp.y - pr - ho},
                {gp.x - pr - ho, gp.y},
                {gp.x + pr + ho, gp.y + pr + ho},
                S.col_pin_hover, S.highlight_thickness);
        }

        ImVec2 bp = {layout.pos.x + layout.width, layout.pos.y + layout.height * 0.5f};
        dl->AddRectFilled({bp.x - pr, bp.y - pr}, {bp.x + pr, bp.y + pr}, S.col_pin_bang);
    }

    // Pin/node hover visuals
    if (!node_hovered && !pin_hovered_on_this) return;

    float ho = S.highlight_offset * canvas_zoom_;
    ImU32 COL_HOVER = S.col_pin_hover;
    float ht = S.highlight_thickness;

    enum class PinShape2 { Circle, Square, Diamond, TriangleDown, TriangleLeft };
    auto draw_highlight = [&](ImVec2 pos, PinShape2 shape) {
        switch (shape) {
        case PinShape2::Circle:    dl->AddCircle(pos, pr + ho, COL_HOVER, 0, ht); break;
        case PinShape2::Square:    dl->AddRect({pos.x-pr-ho,pos.y-pr-ho},{pos.x+pr+ho,pos.y+pr+ho}, COL_HOVER, 0, 0, ht); break;
        case PinShape2::Diamond:   dl->AddQuad({pos.x,pos.y-pr-ho},{pos.x+pr+ho,pos.y},{pos.x,pos.y+pr+ho},{pos.x-pr-ho,pos.y}, COL_HOVER, ht); break;
        case PinShape2::TriangleDown: dl->AddTriangle({pos.x-pr-ho,pos.y-pr-ho},{pos.x+pr+ho,pos.y-pr-ho},{pos.x,pos.y+pr+ho}, COL_HOVER, ht); break;
        case PinShape2::TriangleLeft: dl->AddTriangle({pos.x+pr+ho,pos.y-pr-ho},{pos.x-pr-ho,pos.y},{pos.x+pr+ho,pos.y+pr+ho}, COL_HOVER, ht); break;
        }
    };

    FlowArg2Ptr hovered_pin = nullptr;
    if (auto* pp = std::get_if<FlowArg2Ptr>(&hover_item_))
        hovered_pin = *pp;

    // +diamond hover
    if (auto* add_hover = std::get_if<AddPinHover>(&hover_item_)) {
        if (add_hover->node == node) {
            for (int i = 0; i < pm.total(); i++) {
                if (pm.is_add_diamond(i)) {
                    ImVec2 pp = layout.input_pin_pos(i);
                    draw_highlight(pp, PinShape2::Diamond);
                    if (draw_tooltips_) {
                        ImGui::BeginTooltip();
                        ImGui::SetWindowFontScale(S.tooltip_scale);
                        ImGui::Text("add %s", add_hover->va_port ? add_hover->va_port->name : "arg");
                        ImGui::EndTooltip();
                    }
                    return;
                }
            }
        }
    }

    if (hovered_pin) {
        // Input pins
        for (int i = 0; i < pm.total(); i++) {
            if (pm.is_add_diamond(i) || pm.is_absent_optional(i)) continue;
            FlowArg2Ptr pin_arg = nullptr;
            if (pm.is_base(i)) {
                int port = pm.port_index(i);
                if (node->parsed_args && port < node->parsed_args->size())
                    pin_arg = (*node->parsed_args)[port];
            } else if (pm.is_input_va(i)) {
                int vi = -(pm.port_index(i) + 1);
                if (node->parsed_va_args && vi < node->parsed_va_args->size())
                    pin_arg = (*node->parsed_va_args)[vi];
            } else if (pm.is_remap(i)) {
                int ri = pm.remap_index(i);
                if (ri < (int)node->remaps.size()) pin_arg = node->remaps[ri];
            }
            if (pin_arg == hovered_pin) {
                ImVec2 pp = layout.input_pin_pos(i);
                auto shape = pm.is_input_va(i) ? PinShape2::Diamond : PinShape2::Circle;
                if (pm.is_base(i)) {
                    if (auto* pd = nt->input_port(pm.port_index(i))) {
                        if (pd->kind == PortKind2::BangTrigger) shape = PinShape2::Square;
                        else if (pd->kind == PortKind2::Lambda) shape = PinShape2::TriangleDown;
                        else if (pd->optional) shape = PinShape2::Diamond;
                    }
                }
                draw_highlight(pp, shape);
                if (draw_tooltips_) {
                    ImGui::BeginTooltip();
                    ImGui::SetWindowFontScale(S.tooltip_scale);
                    if (hovered_pin->port())
                        ImGui::Text("%s", hovered_pin->name().c_str());
                    else if (pm.is_remap(i)) {
                        int ri = pm.remap_index(i);
                        ImGui::Text("$%d", ri);
                    }
                    ImGui::EndTooltip();
                }
                return;
            }
        }
        // Output pins
        {
        int hl_skip_sb = nt->is_flow() ? 1 : 0;
        int hl_rendered_fixed = fixed_out - hl_skip_sb;
        for (int i = 0; i < layout.num_out; i++) {
            int out_idx = i + hl_skip_sb;
            bool is_output_va = (i >= hl_rendered_fixed);
            FlowArg2Ptr out_pin = nullptr;
            if (!is_output_va && out_idx < (int)node->outputs.size())
                out_pin = node->outputs[out_idx];
            else if (is_output_va && (i - hl_rendered_fixed) < (int)node->outputs_va_args.size())
                out_pin = node->outputs_va_args[i - hl_rendered_fixed];

            if (out_pin == hovered_pin) {
                ImVec2 pp = layout.output_pin_pos(i);
                PinShape2 shape = PinShape2::Circle;
                if (!is_output_va && nt->output_ports && out_idx < nt->num_outputs &&
                    nt->output_ports[out_idx].kind == PortKind2::BangNext)
                    shape = PinShape2::Square;
                else if (is_output_va)
                    shape = PinShape2::Diamond;
                draw_highlight(pp, shape);
                if (draw_tooltips_) {
                    ImGui::BeginTooltip();
                    ImGui::SetWindowFontScale(S.tooltip_scale);
                    if (hovered_pin->port())
                        ImGui::Text("%s", hovered_pin->name().c_str());
                    else
                        ImGui::Text("out%d", i);
                    ImGui::EndTooltip();
                }
                return;
            }
        }
        }
        // Side-bang
        if (nt->is_flow() && !node->outputs.empty() && node->outputs[0] == hovered_pin) {
            ImVec2 bp = {layout.pos.x + layout.width, layout.pos.y + layout.height * 0.5f};
            draw_highlight(bp, PinShape2::Square);
            if (draw_tooltips_) {
                ImGui::BeginTooltip();
                ImGui::SetWindowFontScale(S.tooltip_scale);
                ImGui::Text("post_bang");
                ImGui::EndTooltip();
            }
            return;
        }
    }

    // Node body tooltip
    if (node_hovered && draw_tooltips_) {
        ImGui::BeginTooltip();
        ImGui::SetWindowFontScale(S.tooltip_scale);
        ImGui::Text("id: %s", node->id().c_str());
        auto show_args = [](const char* label, const ParsedArgs2* pa) {
            if (!pa) return;
            ImGui::Text("%s (%d):", label, pa->size());
            for (int i = 0; i < pa->size(); i++) {
                auto a = (*pa)[i];
                if (auto n = a->as_net())
                    ImGui::Text("  [%d] net: %s", i, n->first().c_str());
                else if (auto e = a->as_expr())
                    ImGui::Text("  [%d] expr: %s", i, e->expr().c_str());
                else if (auto s = a->as_string())
                    ImGui::Text("  [%d] str: %s", i, s->value().c_str());
                else if (auto v = a->as_number())
                    ImGui::Text("  [%d] num: %g", i, v->value());
            }
        };
        show_args("parsed_args", node->parsed_args.get());
        if (node->parsed_va_args && !node->parsed_va_args->empty())
            show_args("parsed_va_args", node->parsed_va_args.get());
        if (!node->remaps.empty()) {
            ImGui::Text("remaps (%d):", (int)node->remaps.size());
            for (int i = 0; i < (int)node->remaps.size(); i++) {
                if (auto n = node->remaps[i]->as_net())
                    ImGui::Text("  $%d -> %s", i, n->first().c_str());
            }
        }
        ImGui::EndTooltip();
    }
}

void Editor2Pane::draw_net(ImDrawList*, const NetBuilderPtr&, ImVec2) {
    // Unused — wires drawn via cached_wires_ in draw()
}

// ─── Hover detection ───

Editor2Pane::HoverItem Editor2Pane::detect_hover(ImVec2 mouse, ImVec2 canvas_origin) {
    auto d2 = [](ImVec2 a, ImVec2 b) { return std::sqrt((a.x-b.x)*(a.x-b.x) + (a.y-b.y)*(a.y-b.y)); };

    float best_dist = 1e18f;
    HoverItem result = std::monostate{};

    float pin_bias = S.pin_priority_bias;

    auto try_candidate = [&](float dist, HoverItem candidate) {
        if (dist < best_dist) {
            best_dist = dist;
            result = std::move(candidate);
        }
    };

    // Wires
    float wire_thresh = S.wire_hit_threshold * canvas_zoom_;
    for (auto& w : cached_wires_) {
        float d = point_to_bezier_dist(mouse, w.p0, w.p1, w.p2, w.p3);
        if (d < wire_thresh)
            try_candidate(d, w.entry());
    }

    // Nodes
    for (auto it = node_editors_.rbegin(); it != node_editors_.rend(); ++it) {
        auto& ned = it->second;
        auto& layout = ned->layout;

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
            nd = d2(mouse, {cx, cy});
        }
        if (nd < S.pin_radius * canvas_zoom_ * S.node_hit_threshold_mul)
            try_candidate(nd, BuilderEntryPtr(ned->node));
    }

    // Pins
    for (auto it = node_editors_.rbegin(); it != node_editors_.rend(); ++it) {
        auto& ned = it->second;
        auto& node = ned->node;
        auto* nt = find_node_type2(node->type_id);
        if (!nt) continue;
        auto& layout = ned->layout;
        auto& pm = ned->pin_mapping;
        float pin_thresh = S.pin_radius * canvas_zoom_ * S.pin_hit_radius_mul;

        // Input pins
        for (int i = 0; i < pm.total(); i++) {
            if (pm.is_absent_optional(i)) continue;
            float pd = d2(mouse, layout.input_pin_pos(i));
            if (pm.is_add_diamond(i)) {
                if (pd < pin_thresh && nt->input_ports_va_args)
                    try_candidate(pd - pin_bias, AddPinHover{node, nt->input_ports_va_args, true});
                continue;
            }
            if (pd < pin_thresh) {
                FlowArg2Ptr pin_arg = nullptr;
                if (pm.is_base(i)) {
                    int port = pm.port_index(i);
                    if (node->parsed_args && port < node->parsed_args->size())
                        pin_arg = (*node->parsed_args)[port];
                } else if (pm.is_input_va(i)) {
                    int vi = -(pm.port_index(i) + 1);
                    if (node->parsed_va_args && vi < node->parsed_va_args->size())
                        pin_arg = (*node->parsed_va_args)[vi];
                } else if (pm.is_remap(i)) {
                    int ri = pm.remap_index(i);
                    if (ri < (int)node->remaps.size())
                        pin_arg = node->remaps[ri];
                }
                if (pin_arg) try_candidate(pd - pin_bias, pin_arg);
            }
        }

        // Output pins
        {
            int skip_sb = nt->is_flow() ? 1 : 0;
            int rendered_fixed = nt->num_outputs - skip_sb;
            for (int i = 0; i < layout.num_out; i++) {
                float pd = d2(mouse, layout.output_pin_pos(i));
                if (pd < pin_thresh) {
                    int out_idx = i + skip_sb;
                    bool is_va = (i >= rendered_fixed);
                    if (!is_va && out_idx < (int)node->outputs.size() && node->outputs[out_idx])
                        try_candidate(pd - pin_bias, node->outputs[out_idx]);
                    else if (is_va) {
                        int vi = i - rendered_fixed;
                        if (vi < (int)node->outputs_va_args.size() && node->outputs_va_args[vi])
                            try_candidate(pd - pin_bias, node->outputs_va_args[vi]);
                    }
                }
            }
        }

        // Lambda grab
        if (nt->is_flow()) {
            float pd = d2(mouse, layout.lambda_grab_pos());
            if (pd < pin_thresh)
                try_candidate(pd - pin_bias, BuilderEntryPtr(node));
        }

        // Side-bang
        if (nt->is_flow()) {
            ImVec2 bp = {layout.pos.x + layout.width, layout.pos.y + layout.height * 0.5f};
            float pd = d2(mouse, bp);
            if (pd < pin_thresh) {
                if (node->outputs.empty() || !node->outputs[0])
                    throw std::logic_error("Flow node '" + node->id() + "' missing side-bang output[0]");
                try_candidate(pd - pin_bias, node->outputs[0]);
            }
        }
    }

    return result;
}

// ─── Hover effects + tooltips ───

void Editor2Pane::draw_hover_effects(
    ImDrawList* dl, ImVec2 canvas_origin, const HoverItem& hover)
{
    if (std::holds_alternative<std::monostate>(hover)) return;

    float th_wire = (S.wire_thickness + 2.0f) * canvas_zoom_;

    FlowNodeBuilderPtr hover_node = nullptr;
    BuilderEntryPtr hover_entry = nullptr;
    FlowArg2Ptr hover_pin = nullptr;

    if (auto* ep = std::get_if<BuilderEntryPtr>(&hover)) {
        hover_entry = *ep;
        hover_node = hover_entry ? hover_entry->as_node() : nullptr;
    } else if (auto* pp = std::get_if<FlowArg2Ptr>(&hover)) {
        hover_pin = *pp;
    }

    // Node hovered: highlight lambda wires capturing it
    if (hover_node) {
        for (auto& w : cached_wires_) {
            if (w.is_lambda() && w.entry() == hover_node)
                dl->AddBezierCubic(w.p0, w.p1, w.p2, w.p3, S.col_pin_hover, th_wire);
        }
    }

    // Wire/net hovered: highlight all wires in the same net
    if (hover_entry && hover_entry->as_net()) {
        for (auto& w : cached_wires_) {
            if (w.entry() == hover_entry)
                dl->AddBezierCubic(w.p0, w.p1, w.p2, w.p3, S.col_pin_hover, th_wire);
        }
        if (draw_tooltips_) {
            for (auto& w : cached_wires_) {
                if (w.entry() == hover_entry) {
                    ImGui::BeginTooltip();
                    ImGui::SetWindowFontScale(S.tooltip_scale);
                    if (w.is_lambda())
                        ImGui::Text("lambda: %s", w.src_id.c_str());
                    else
                        ImGui::Text("net: %s", w.net_id.c_str());
                    ImGui::Text("src: %s", w.src_id.c_str());
                    ImGui::Text("dst: %s", w.dst_id.c_str());
                    ImGui::EndTooltip();
                    break;
                }
            }
        }
    }
}
