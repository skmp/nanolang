#include "editor2.h"
#include "tooltip_renderer.h"
#include "atto/graph_builder.h"
#include "atto/node_types2.h"
#include "imgui.h"
#include <cmath>
#include <algorithm>
#include <fstream>
#include <cstdio>
#include <stdexcept>

// ─── Factory ───

Editor2Pane::Editor2Pane(const std::shared_ptr<GraphBuilder>& gb,
                         const std::shared_ptr<AttoEditorSharedState>& shared)
    : VisualEditor(shared), gb_(gb) {
}

std::shared_ptr<IEditorPane> make_editor2(
    const std::shared_ptr<GraphBuilder>& gb,
    const std::shared_ptr<AttoEditorSharedState>& shared) {
    auto pane = std::make_shared<Editor2Pane>(gb, shared);
    gb->add_editor(pane);
    return pane;
}

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
                for (int k = 0; k < (int)src_node->outputs.size(); k++) {
                    auto out_net = src_node->outputs[k]->as_net();
                    if (out_net && out_net->second() == entry) { source_pin = k; break; }
                }
                if (source_pin == 0) {
                    int base = (int)src_node->outputs.size();
                    for (int k = 0; k < (int)src_node->outputs_va_args.size(); k++) {
                        auto out_net = src_node->outputs_va_args[k]->as_net();
                        if (out_net && out_net->second() == entry) { source_pin = base + k; break; }
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
        ImGui::TextDisabled("No graph loaded");
        return;
    }
    draw_canvas("##canvas2");
}

// ─── VisualEditor hooks ───

void Editor2Pane::draw_content(const CanvasFrame& frame) {
    // Rebuild node layouts and draw nodes
    for (auto& [id, ned] : node_editors_) {
        if (ned->node->shadow)
            throw std::logic_error("Editor2Pane: shadow nodes must be folded before rendering (id: " + id + ")");
        ned->rebuild(frame.canvas_origin, canvas_zoom_);

        auto& node = ned->node;
        auto state = build_render_state(node, hover_item_, shared_.get());
        auto* nt = find_node_type2(node->type_id);
        render_node(frame.dl, node, nt, ned->layout, ned->vpm, ned->display_text,
                    state, canvas_zoom_, draw_tooltips_);
    }

    // Rebuild wires
    rebuild_wires(frame.canvas_origin);

    // Draw wires
    for (auto& w : cached_wires_) {
        render_wire(frame.dl, w, canvas_zoom_);
        render_wire_label(frame.dl, w, canvas_zoom_);
    }
}

HoverItem Editor2Pane::do_detect_hover(ImVec2 mouse, ImVec2 canvas_origin) {
    std::vector<NodeHitTarget> targets;
    targets.reserve(node_editors_.size());
    for (auto& [id, ned] : node_editors_) {
        auto* nt = find_node_type2(ned->node->type_id);
        if (!nt) continue;
        targets.push_back({ned->node, nt, &ned->layout, &ned->vpm});
    }

    auto wire_hit = hit_test_wires(mouse, cached_wires_, canvas_zoom_);
    auto node_hit = hit_test_node_bodies(mouse, targets, canvas_zoom_);
    auto pin_hit = hit_test_pins(mouse, targets, canvas_zoom_);

    HitResult best = wire_hit;
    if (node_hit.distance < best.distance) best = node_hit;
    if (pin_hit.distance < best.distance) best = pin_hit;

    return best.item;
}

void Editor2Pane::do_draw_hover_effects(ImDrawList* dl, ImVec2 canvas_origin, const HoverItem& hover) {
    if (std::holds_alternative<std::monostate>(hover)) return;

    FlowNodeBuilderPtr hover_node = nullptr;
    BuilderEntryPtr hover_entry = nullptr;

    if (auto* ep = std::get_if<BuilderEntryPtr>(&hover)) {
        hover_entry = *ep;
        hover_node = hover_entry ? hover_entry->as_node() : nullptr;
    }

    if (hover_node) {
        for (auto& w : cached_wires_)
            if (w.is_lambda() && w.entry() == hover_node)
                render_wire_highlight(dl, w, canvas_zoom_);
    }

    if (hover_entry && hover_entry->as_net()) {
        for (auto& w : cached_wires_)
            if (w.entry() == hover_entry)
                render_wire_highlight(dl, w, canvas_zoom_);
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

FlowNodeBuilderPtr Editor2Pane::hover_to_node(const HoverItem& item) {
    if (auto* ep = std::get_if<BuilderEntryPtr>(&item)) {
        if (*ep) return (*ep)->as_node();
    } else if (auto* pin = std::get_if<FlowArg2Ptr>(&item)) {
        return (*pin)->node();
    }
    return nullptr;
}

bool Editor2Pane::test_drag_overlap(const FlowNodeBuilderPtr& sel, float nx, float ny) {
    float pad = S.node_height * 0.5f;
    auto sel_vpm = VisualPinMap::build(sel, find_node_type2(sel->type_id));
    auto sel_layout = compute_node_layout(sel, sel_vpm, {0,0}, 1.0f);
    for (auto& [oid, oned] : node_editors_) {
        auto on = oned->node;
        if (shared_ && shared_->selected_nodes.count(on)) continue;
        auto on_vpm = VisualPinMap::build(on, find_node_type2(on->type_id));
        auto ol = compute_node_layout(on, on_vpm, {0,0}, 1.0f);
        float ox = on->position.x - pad, oy = on->position.y - pad;
        float ow = ol.width + pad * 2, oh = ol.height + pad * 2;
        if (nx < ox + ow && nx + sel_layout.width > ox &&
            ny < oy + oh && ny + sel_layout.height > oy)
            return true;
    }
    return false;
}

std::vector<VisualEditor::BoxTestNode> Editor2Pane::get_box_test_nodes() {
    std::vector<BoxTestNode> result;
    for (auto& [id, ned] : node_editors_) {
        auto node = ned->node;
        auto vpm = VisualPinMap::build(node, find_node_type2(node->type_id));
        auto layout = compute_node_layout(node, vpm, {0,0}, 1.0f);
        result.push_back({node, node->position.x, node->position.y, layout.width, layout.height});
    }
    return result;
}
