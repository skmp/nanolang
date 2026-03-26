#include "editor2.h"
#include "node_renderer.h"
#include "tooltip_renderer.h"
#include "atto/graph_builder.h"
#include "atto/node_types2.h"
#include "imgui.h"
#include <cmath>
#include <algorithm>
#include <fstream>
#include <cstdio>
#include <stdexcept>

// ─── Per-item editor implementations ───

void NodeEditorImpl::rebuild(ImVec2 canvas_origin, float zoom) {
    auto* nt = find_node_type2(node->type_id);
    vpm = VisualPinMap::build(node, nt);
    layout = compute_node_layout(node, vpm, canvas_origin, zoom);
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

        ned->rebuild(canvas_origin, canvas_zoom_);
        auto& dst_layout = ned->layout;
        auto& dst_vpm = ned->vpm;

        // For each input pin with a net connection, compute wire geometry
        for (int i = 0; i < (int)dst_vpm.inputs.size(); i++) {
            auto& pin = dst_vpm.inputs[i];
            if (pin.kind == VisualPinKind::AddDiamond || pin.kind == VisualPinKind::AbsentOptional) continue;
            if (!pin.arg) continue;
            auto an = pin.arg->as_net();
            if (!an) continue;

            auto entry = an->second();
            if (!entry) continue;

            FlowNodeBuilderPtr src_node = nullptr;
            bool is_lambda = false;
            int source_pin = 0;

            if (auto net = entry->as_net()) {
                if (net->is_the_unconnected()) continue;
                auto src_ptr = net->source().lock();
                src_node = src_ptr ? src_ptr->as_node() : nullptr;
                if (!src_node) continue;
                // Find which output pin sources this net
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
                continue;
            }

            auto* src_nt = find_node_type2(src_node->type_id);
            auto src_vpm = VisualPinMap::build(src_node, src_nt);
            auto src_layout = compute_node_layout(src_node, src_vpm, canvas_origin, canvas_zoom_);
            ImVec2 to = dst_layout.input_pin_pos(i);

            ImVec2 from;
            bool is_side_bang = false;
            if (is_lambda) {
                from = src_layout.lambda_grab_pos();
            } else {
                is_side_bang = src_nt && src_nt->is_flow() &&
                    source_pin < (src_nt->num_outputs) &&
                    src_nt->output_ports && src_nt->output_ports[source_pin].kind == PortKind2::BangNext;

                if (is_side_bang) {
                    from = src_layout.side_bang_pos();
                } else {
                    // Map source_pin to visual output index (skip side-bang for flow)
                    int visual_pin = source_pin;
                    if (src_nt && src_nt->is_flow()) visual_pin = std::max(0, visual_pin - 1);
                    from = src_layout.output_pin_pos(visual_pin);
                }
            }

            cached_wires_.push_back(compute_wire_geometry(
                from, to, is_lambda, is_side_bang, canvas_zoom_,
                entry, src_node->id(), dst_id, an->first()));
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
    ImVec2 canvas_origin = v2add(canvas_p0, canvas_offset_);

    render_background(dl, canvas_p0, canvas_sz, canvas_offset_, canvas_zoom_);

    dl->PushClipRect(canvas_p0, v2add(canvas_p0, canvas_sz), true);

    // Rebuild node layouts and draw nodes
    for (auto& [id, ned] : node_editors_) {
        if (ned->node->shadow)
            throw std::logic_error("Editor2Pane: shadow nodes must be folded before rendering (id: " + id + ")");
        ned->rebuild(canvas_origin, canvas_zoom_);

        // Build render state from editor interaction state
        auto& node = ned->node;
        NodeRenderState state;
        state.selected = selected_nodes_.count(node) > 0;
        state.node_hovered = false;
        if (auto* ep = std::get_if<BuilderEntryPtr>(&hover_item_))
            state.node_hovered = (*ep == node);
        state.pin_hovered_on_this = false;
        if (auto* pin = std::get_if<FlowArg2Ptr>(&hover_item_))
            state.pin_hovered_on_this = ((*pin)->node() == node);
        else if (auto* add = std::get_if<AddPinHover>(&hover_item_))
            state.pin_hovered_on_this = (add->node == node);
        state.hovered_pin = nullptr;
        if (auto* pp = std::get_if<FlowArg2Ptr>(&hover_item_))
            state.hovered_pin = *pp;
        state.add_pin_hover = std::get_if<AddPinHover>(&hover_item_);

        auto* nt = find_node_type2(node->type_id);
        render_node(dl, node, nt, ned->layout, ned->vpm, ned->display_text,
                    state, canvas_zoom_, draw_tooltips_);
    }

    // Rebuild wires
    rebuild_wires(canvas_origin);

    // Draw wires
    for (auto& w : cached_wires_) {
        render_wire(dl, w, canvas_zoom_);
        render_wire_label(dl, w, canvas_zoom_);
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
                auto sel_vpm = VisualPinMap::build(sel, find_node_type2(sel->type_id));
                auto sel_layout = compute_node_layout(sel, sel_vpm, {0,0}, 1.0f);
                for (auto& [oid, oned] : node_editors_) {
                    auto on = oned->node;
                    if (selected_nodes_.count(on)) continue;
                    auto on_vpm = VisualPinMap::build(on, find_node_type2(on->type_id));
                    auto ol = compute_node_layout(on, on_vpm, {0,0}, 1.0f);
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
                auto sel_vpm = VisualPinMap::build(sel, find_node_type2(sel->type_id));
                auto sel_layout = compute_node_layout(sel, sel_vpm, {0,0}, 1.0f);
                float nx = sel->position.x + dx, ny = sel->position.y + dy;
                for (auto& [oid, oned] : node_editors_) {
                    auto on = oned->node;
                    if (selected_nodes_.count(on)) continue;
                    auto on_vpm = VisualPinMap::build(on, find_node_type2(on->type_id));
                    auto ol = compute_node_layout(on, on_vpm, {0,0}, 1.0f);
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
        render_selection_rect(dl, sp0, sp1);

        selected_nodes_.clear();
        for (auto& [id, ned] : node_editors_) {
            auto node = ned->node;
            auto node_vpm = VisualPinMap::build(node, find_node_type2(node->type_id));
            auto layout = compute_node_layout(node, node_vpm, {0,0}, 1.0f);
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

    // Pan
    if (canvas_hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        canvas_offset_.x += ImGui::GetIO().MouseDelta.x;
        canvas_offset_.y += ImGui::GetIO().MouseDelta.y;
    }
    if (canvas_hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
        canvas_offset_.x += ImGui::GetIO().MouseDelta.x;
        canvas_offset_.y += ImGui::GetIO().MouseDelta.y;
    }

    // Zoom
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

// ─── Hover detection ───

HoverItem Editor2Pane::detect_hover(ImVec2 mouse, ImVec2 canvas_origin) {
    // Build hit targets from node editors
    std::vector<NodeHitTarget> targets;
    targets.reserve(node_editors_.size());
    for (auto& [id, ned] : node_editors_) {
        auto* nt = find_node_type2(ned->node->type_id);
        if (!nt) continue;
        targets.push_back({ned->node, nt, &ned->layout, &ned->vpm});
    }

    // Test all 3 categories
    auto wire_hit = hit_test_wires(mouse, cached_wires_, canvas_zoom_);
    auto node_hit = hit_test_node_bodies(mouse, targets, canvas_zoom_);
    auto pin_hit = hit_test_pins(mouse, targets, canvas_zoom_);

    // Pick winner (pins have bias built into their distance)
    HitResult best = wire_hit;
    if (node_hit.distance < best.distance) best = node_hit;
    if (pin_hit.distance < best.distance) best = pin_hit;

    return best.item;
}

// ─── Hover effects ───

void Editor2Pane::draw_hover_effects(ImDrawList* dl, ImVec2 canvas_origin, const HoverItem& hover) {
    if (std::holds_alternative<std::monostate>(hover)) return;

    FlowNodeBuilderPtr hover_node = nullptr;
    BuilderEntryPtr hover_entry = nullptr;

    if (auto* ep = std::get_if<BuilderEntryPtr>(&hover)) {
        hover_entry = *ep;
        hover_node = hover_entry ? hover_entry->as_node() : nullptr;
    }

    // Node hovered: highlight lambda wires capturing it
    if (hover_node) {
        for (auto& w : cached_wires_) {
            if (w.is_lambda() && w.entry() == hover_node)
                render_wire_highlight(dl, w, canvas_zoom_);
        }
    }

    // Wire/net hovered: highlight all wires in the same net
    if (hover_entry && hover_entry->as_net()) {
        for (auto& w : cached_wires_) {
            if (w.entry() == hover_entry)
                render_wire_highlight(dl, w, canvas_zoom_);
        }
        if (draw_tooltips_) {
            for (auto& w : cached_wires_) {
                if (w.entry() == hover_entry) {
                    tooltip_wire(w);
                    break;
                }
            }
        }
    }
}
