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
            if (pin.kind == VisualPinKind::AddDiamond) continue;
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

// ─── Wire connection hooks ───

ImVec2 Editor2Pane::get_pin_screen_pos(const FlowArg2Ptr& pin) {
    if (!pin) return {0, 0};
    auto node = pin->node();
    auto it = node_editors_.find(node->id());
    if (it == node_editors_.end()) return {0, 0};
    auto& ned = it->second;

    // Check if it's the side-bang
    if (ned->vpm.has_side_bang && ned->vpm.side_bang_arg == pin)
        return ned->layout.side_bang_pos();

    // Search inputs
    for (int i = 0; i < (int)ned->vpm.inputs.size(); i++)
        if (ned->vpm.inputs[i].arg == pin)
            return ned->layout.input_pin_pos(i);

    // Search outputs
    for (int i = 0; i < (int)ned->vpm.outputs.size(); i++)
        if (ned->vpm.outputs[i].arg == pin)
            return ned->layout.output_pin_pos(i);

    return {0, 0};
}

PortPosition2 Editor2Pane::get_pin_position(const FlowArg2Ptr& pin) {
    if (!pin) return PortPosition2::Input;
    auto node = pin->node();
    auto it = node_editors_.find(node->id());
    if (it == node_editors_.end()) return PortPosition2::Input;
    auto& ned = it->second;

    // Side-bang is an output
    if (ned->vpm.has_side_bang && ned->vpm.side_bang_arg == pin)
        return PortPosition2::Output;

    // Check outputs
    for (auto& p : ned->vpm.outputs)
        if (p.arg == pin) return PortPosition2::Output;

    return PortPosition2::Input;
}

bool Editor2Pane::pin_is_connected(const FlowArg2Ptr& pin) {
    if (!pin) return false;
    auto an = pin->as_net();
    if (!an) return false;
    auto entry = an->second();
    if (!entry) return false;
    // Connected if entry is a node (lambda) or a non-unconnected net
    if (entry->as_node()) return true;
    auto net = entry->as_net();
    return net && !net->is_the_unconnected();
}

bool Editor2Pane::do_connect_pins(const FlowArg2Ptr& from_pin, PortPosition2 from_pos,
                                   const FlowArg2Ptr& to_pin, PortPosition2 to_pos) {
    if (!from_pin || !to_pin || !gb_) return false;

    auto from_port = from_pin->port();
    auto to_port = to_pin->port();
    PortKind2 from_kind = from_port ? from_port->kind : PortKind2::Data;
    PortKind2 to_kind = to_port ? to_port->kind : PortKind2::Data;

    // Determine which is source and which is dest
    FlowArg2Ptr src_pin, dst_pin;
    if (is_wire_source(from_kind, from_pos) && is_wire_dest(to_kind, to_pos)) {
        src_pin = from_pin;
        dst_pin = to_pin;
    } else if (is_wire_source(to_kind, to_pos) && is_wire_dest(from_kind, from_pos)) {
        src_pin = to_pin;
        dst_pin = from_pin;
    } else {
        return false;
    }

    auto src_node = src_pin->node();
    auto dst_node = dst_pin->node();

    gb_->edit_start();

    // Get or create net for source
    auto src_an = src_pin->as_net();
    BuilderEntryPtr net_entry;
    NodeId net_id;

    if (src_an) {
        auto existing = src_an->second();
        auto existing_net = existing ? existing->as_net() : nullptr;
        if (existing_net && !existing_net->is_the_unconnected()) {
            // Fan-out: reuse existing net
            net_entry = existing;
            net_id = src_an->first();
        }
    }

    if (!net_entry) {
        // Create new auto-wire net
        auto [new_id, entry] = gb_->find_or_create_net(gb_->next_id(), true);
        net_id = new_id;
        net_entry = entry;
        auto net = entry->as_net();
        net->auto_wire(true);
        net->source(src_node);

        // Update source pin to point to this net
        if (src_an) {
            src_an->net_id(net_id);
            src_an->entry(net_entry);
        }
    }

    // Handle fan-in: if dst already connected and doesn't allow multi, disconnect old
    auto dst_an = dst_pin->as_net();
    auto dst_port = dst_pin->port();
    PortKind2 dst_kind = dst_port ? dst_port->kind : PortKind2::Data;
    if (dst_an && !allows_multi_input(dst_kind)) {
        auto old_entry = dst_an->second();
        auto old_net = old_entry ? old_entry->as_net() : nullptr;
        if (old_net && !old_net->is_the_unconnected()) {
            // Remove dst_node from old net's destinations
            auto& dests = old_net->destinations();
            dests.erase(std::remove_if(dests.begin(), dests.end(),
                [&](auto& w) { return w.lock() == dst_node; }), dests.end());
        }
    }

    // Connect destination pin to net
    if (dst_an) {
        dst_an->net_id(net_id);
        dst_an->entry(net_entry);
    }

    // Add dst_node to net's destinations
    auto net = net_entry->as_net();
    if (net) {
        net->destinations().push_back(dst_node);
    }

    gb_->edit_commit();
    wires_dirty_ = true;
    return true;
}

bool Editor2Pane::do_disconnect_pin(const FlowArg2Ptr& pin, PortPosition2 pos) {
    if (!pin || !gb_) return false;
    auto an = pin->as_net();
    if (!an) return false;
    auto entry = an->second();
    auto net = entry ? entry->as_net() : nullptr;
    if (!net || net->is_the_unconnected()) return false;

    // Save undo state
    grab_undo_.pin = pin;
    grab_undo_.old_net_id = an->first();
    grab_undo_.old_entry = entry;

    gb_->edit_start();

    // Remove from net's destinations if this is a dest pin
    auto node = pin->node();
    auto& dests = net->destinations();
    dests.erase(std::remove_if(dests.begin(), dests.end(),
        [&](auto& w) { return w.lock() == node; }), dests.end());

    // Point pin to $unconnected
    auto unconnected = gb_->unconnected_net();
    an->net_id(unconnected->id());
    an->entry(std::static_pointer_cast<BuilderEntry>(unconnected));

    gb_->edit_commit();
    wires_dirty_ = true;
    return true;
}

void Editor2Pane::do_reconnect_pin(const FlowArg2Ptr& pin, PortPosition2 pos) {
    if (!pin || !gb_ || !grab_undo_.pin) return;
    auto an = pin->as_net();
    if (!an) return;

    gb_->edit_start();

    // Restore old connection
    an->net_id(grab_undo_.old_net_id);
    an->entry(grab_undo_.old_entry);

    // Re-add to net's destinations
    auto net = grab_undo_.old_entry ? grab_undo_.old_entry->as_net() : nullptr;
    if (net) {
        net->destinations().push_back(pin->node());
    }

    gb_->edit_commit();
    grab_undo_ = {};
    wires_dirty_ = true;
}

void Editor2Pane::do_delete_hovered(const HoverItem& item) {
    if (!gb_) return;

    gb_->edit_start();

    if (auto* pin_ptr = std::get_if<FlowArg2Ptr>(&item)) {
        // Pin hovered: disconnect it from its net
        auto& pin = *pin_ptr;
        if (pin) {
            auto an = pin->as_net();
            if (an) {
                auto entry = an->second();
                auto net = entry ? entry->as_net() : nullptr;
                if (net && !net->is_the_unconnected()) {
                    // Remove this node from the net's destinations
                    auto node = pin->node();
                    auto& dests = net->destinations();
                    dests.erase(std::remove_if(dests.begin(), dests.end(),
                        [&](auto& w) { return w.lock() == node; }), dests.end());

                    // Point pin to $unconnected
                    auto unconnected = gb_->unconnected_net();
                    an->net_id(unconnected->id());
                    an->entry(std::static_pointer_cast<BuilderEntry>(unconnected));
                }
            }
        }
    } else if (auto* entry_ptr = std::get_if<BuilderEntryPtr>(&item)) {
        auto& entry = *entry_ptr;
        if (!entry) { gb_->edit_commit(); return; }

        if (auto net = entry->as_net()) {
            // Net hovered: disconnect all pins from this net
            auto unconnected = gb_->unconnected_net();
            auto uncon_entry = std::static_pointer_cast<BuilderEntry>(unconnected);
            // Disconnect all args referencing this net
            for (auto& p : gb_->pins()) {
                auto an = p->as_net();
                if (an && an->second() == entry) {
                    an->net_id(unconnected->id());
                    an->entry(uncon_entry);
                }
            }
            net->destinations().clear();
            net->source(BuilderEntryWeak{});
            gb_->entries.erase(net->id());
        } else if (auto node = entry->as_node()) {
            // Node hovered: disconnect all its pins, then remove the node
            auto unconnected = gb_->unconnected_net();
            auto uncon_entry = std::static_pointer_cast<BuilderEntry>(unconnected);

            // Disconnect all input args
            auto disconnect_args = [&](ParsedArgs2* pa) {
                if (!pa) return;
                for (int i = 0; i < pa->size(); i++) {
                    auto an = (*pa)[i]->as_net();
                    if (!an) continue;
                    auto net_e = an->second() ? an->second()->as_net() : nullptr;
                    if (net_e && !net_e->is_the_unconnected()) {
                        auto& dests = net_e->destinations();
                        dests.erase(std::remove_if(dests.begin(), dests.end(),
                            [&](auto& w) { return w.lock() == node; }), dests.end());
                    }
                    an->net_id(unconnected->id());
                    an->entry(uncon_entry);
                }
            };
            disconnect_args(node->parsed_args.get());
            disconnect_args(node->parsed_va_args.get());
            for (auto& r : node->remaps) {
                auto an = r->as_net();
                if (an) { an->net_id(unconnected->id()); an->entry(uncon_entry); }
            }
            // Disconnect output args and clear net sources
            for (auto& o : node->outputs) {
                auto an = o->as_net();
                if (!an) continue;
                auto net_e = an->second() ? an->second()->as_net() : nullptr;
                if (net_e && !net_e->is_the_unconnected())
                    net_e->source(BuilderEntryWeak{});
                an->net_id(unconnected->id());
                an->entry(uncon_entry);
            }
            for (auto& o : node->outputs_va_args) {
                auto an = o->as_net();
                if (!an) continue;
                auto net_e = an->second() ? an->second()->as_net() : nullptr;
                if (net_e && !net_e->is_the_unconnected())
                    net_e->source(BuilderEntryWeak{});
                an->net_id(unconnected->id());
                an->entry(uncon_entry);
            }

            // Remove from selection
            if (shared_) shared_->selected_nodes.erase(node);

            // Remove node from entries
            gb_->entries.erase(node->id());
        }
    }

    gb_->edit_commit();
    wires_dirty_ = true;
}
