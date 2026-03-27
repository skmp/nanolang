#include "nets_editor.h"
#include "tooltip_renderer.h"
#include "atto/graph_builder.h"
#include "atto/node_types2.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>

// ─── Factory ───

NetsEditor::NetsEditor(const std::shared_ptr<GraphBuilder>& gb,
                       const std::shared_ptr<AttoEditorSharedState>& shared)
    : VisualEditor(shared), gb_(gb) {
}

std::shared_ptr<IEditorPane> make_nets_editor(
    const std::shared_ptr<GraphBuilder>& gb,
    const std::shared_ptr<AttoEditorSharedState>& shared) {
    return std::make_shared<NetsEditor>(gb, shared);
}

// ─── Layout constants ───

static constexpr float ROW_HEIGHT_MULT = 6.0f;  // rows are 6x node height apart
static constexpr float LEFT_MARGIN = 20.0f;
static constexpr float NODE_GAP = 30.0f;
static constexpr float LABEL_GAP = 60.0f;       // gap between src node and label area
static constexpr float FADE_STUB_LENGTH_MULT = 2.0f; // 2x node height

// ─── Helpers ───

// Find which visual output pin of src_node produces this net entry
static int find_source_output_pin(const FlowNodeBuilderPtr& src_node, const BuilderEntryPtr& net_entry) {
    for (int k = 0; k < (int)src_node->outputs.size(); k++) {
        auto out_net = src_node->outputs[k]->as_net();
        if (out_net && out_net->second() == net_entry) return k;
    }
    int base = (int)src_node->outputs.size();
    for (int k = 0; k < (int)src_node->outputs_va_args.size(); k++) {
        auto out_net = src_node->outputs_va_args[k]->as_net();
        if (out_net && out_net->second() == net_entry) return base + k;
    }
    return 0;
}

// Find which visual input pin of dst_node receives this net entry, using VisualPinMap
static int find_dest_input_pin(const VisualPinMap& vpm, const BuilderEntryPtr& net_entry) {
    for (int i = 0; i < (int)vpm.inputs.size(); i++) {
        auto& pin = vpm.inputs[i];
        if (!pin.arg) continue;
        auto an = pin.arg->as_net();
        if (an && an->second() == net_entry) return i;
    }
    return 0;
}

// ─── rebuild_layout ───

void NetsEditor::rebuild_layout(ImVec2 canvas_origin) {
    rows_.clear();
    all_wires_.clear();
    rendered_nodes_.clear();

    float zoom = canvas_zoom_;
    float row_h = S.node_height * ROW_HEIGHT_MULT * zoom;
    float node_h = S.node_height * zoom;

    // Collect all non-sentinel nets, sorted alphabetically
    std::vector<std::pair<NodeId, NetBuilderPtr>> nets;
    for (auto& [id, entry] : gb_->entries) {
        auto net = entry->as_net();
        if (!net || net->is_the_unconnected()) continue;
        nets.push_back({id, net});
    }
    std::sort(nets.begin(), nets.end(), [](auto& a, auto& b) { return a.first < b.first; });

    // Layout: net label on baseline, nodes above/below with wires curving to baseline
    //
    //   ┌───────────┐                     ┌──────────┐   ┌──────────┐
    //   │ src_node  │                     │ dst_a    │   │ dst_b    │
    //   └─────┬─────┘                     └─────┬────┘   └────┬─────┘
    //         │ (curve down)                    │             │
    //   ──────┴─────── net_label ───────────────┴─────────────┴──────
    //                  (baseline)

    // Start with enough offset so the first row's source node (above baseline) is visible
    float first_row_offset = node_h * ROW_HEIGHT_MULT;

    int row_idx = 0;
    for (auto& [net_id, net] : nets) {
        auto src_ptr = net->source().lock();
        auto src_node = src_ptr ? src_ptr->as_node() : nullptr;
        if (!src_node) { row_idx++; continue; }

        NetRow row;
        row.net = net;
        row.net_id = net_id;
        row.src_node = src_node;
        row.row_y = first_row_offset + row_idx * row_h;

        float baseline_y = canvas_origin.y + row.row_y;

        // Source node
        auto* src_nt = find_node_type2(src_node->type_id);
        row.src_vpm = VisualPinMap::build(src_node, src_nt);
        row.src_display = src_nt ? src_nt->name : "?";
        std::string args = src_node->args_str();
        if (!args.empty()) row.src_display += " " + args;

        row.src_output_pin = find_source_output_pin(src_node, net);
        row.src_is_bang = src_nt && row.src_output_pin < src_nt->num_outputs &&
                          src_nt->output_ports &&
                          src_nt->output_ports[row.src_output_pin].kind == PortKind2::BangNext;

        // Source node placement: put node on OPPOSITE side of baseline from its pin.
        // Output pins (bottom of node) → node ABOVE baseline, pin near baseline, wire curves down.
        // Side-bang (right of node) → node above baseline (side-bang is mid-height).
        row.src_layout = compute_node_layout(src_node, row.src_vpm, {0,0}, zoom);
        float src_x = canvas_origin.x + LEFT_MARGIN * zoom;
        float gap = node_h * 0.3f;

        bool is_side_bang = row.src_is_bang && row.src_vpm.has_side_bang;
        // Output pins are at the bottom → node above baseline
        row.src_layout.pos = {src_x, baseline_y - row.src_layout.height - gap * 3};

        rendered_nodes_.push_back({src_node, row.src_layout, row.src_vpm, row.src_display});

        // Get source pin position
        ImVec2 src_pin_pos;
        if (is_side_bang) {
            src_pin_pos = row.src_layout.side_bang_pos();
        } else {
            int visual_pin = row.src_output_pin;
            if (row.src_vpm.is_flow) visual_pin = std::max(0, visual_pin - 1);
            src_pin_pos = row.src_layout.output_pin_pos(visual_pin);
        }
        ImVec2 src_baseline = {src_pin_pos.x, baseline_y};

        // Collect destinations sorted alphabetically
        net->compact();
        std::vector<std::pair<NodeId, FlowNodeBuilderPtr>> dest_list;
        for (auto& dw : net->destinations()) {
            auto dp = dw.lock();
            auto dn = dp ? dp->as_node() : nullptr;
            if (dn) dest_list.push_back({dn->id(), dn});
        }
        std::sort(dest_list.begin(), dest_list.end(), [](auto& a, auto& b) { return a.first < b.first; });

        // Net label position: between source and first destination
        float stub_len = node_h * FADE_STUB_LENGTH_MULT;
        float src_extra = row.src_vpm.has_side_bang ? stub_len + S.pin_radius * zoom : 0.0f;
        float label_x = src_x + row.src_layout.width + src_extra + LABEL_GAP * 0.5f * zoom;
        float dest_start_x = src_x + row.src_layout.width + src_extra + LABEL_GAP * zoom;

        // Wire: source pin → baseline (pin is above baseline, curves down)
        {
            float curve_dy = std::abs(src_pin_pos.y - baseline_y) * 0.5f;
            ImVec2 cp1 = {src_pin_pos.x, src_pin_pos.y + curve_dy};
            ImVec2 cp2 = {src_baseline.x, src_baseline.y - curve_dy};
            all_wires_.push_back({net, src_pin_pos, cp1, cp2, src_baseline,
                                  src_node->id(), "", net_id});
        }

        // Deduplicate destinations: group by node ID, collect all input pins
        // A node may appear multiple times if it has multiple pins connected to this net
        struct DestGroup {
            FlowNodeBuilderPtr node;
            NodeId id;
            std::vector<int> input_pins; // all visual input pin indices connected to this net
        };
        std::map<NodeId, DestGroup> dest_groups;
        for (auto& [did, dnode] : dest_list) {
            auto& grp = dest_groups[did];
            if (!grp.node) {
                grp.node = dnode;
                grp.id = did;
            }
            auto dvpm = VisualPinMap::build(dnode, find_node_type2(dnode->type_id));
            // Find ALL input pins connected to this net (not just the first)
            for (int i = 0; i < (int)dvpm.inputs.size(); i++) {
                auto& pin = dvpm.inputs[i];
                if (!pin.arg) continue;
                auto an = pin.arg->as_net();
                if (an && an->second() == net) grp.input_pins.push_back(i);
            }
        }

        // Sort groups alphabetically and lay out
        std::vector<DestGroup*> sorted_groups;
        for (auto& [id, grp] : dest_groups) sorted_groups.push_back(&grp);
        std::sort(sorted_groups.begin(), sorted_groups.end(),
                  [](auto* a, auto* b) { return a->id < b->id; });

        float dest_x = dest_start_x;
        for (auto* grp : sorted_groups) {
            auto* dst_nt = find_node_type2(grp->node->type_id);
            NetRow::Dest dest;
            dest.node = grp->node;
            dest.vpm = VisualPinMap::build(grp->node, dst_nt);
            dest.display = dst_nt ? dst_nt->name : "?";
            std::string dargs = grp->node->args_str();
            if (!dargs.empty()) dest.display += " " + dargs;
            dest.input_pin = grp->input_pins.empty() ? 0 : grp->input_pins[0];
            dest.is_bang = dest.input_pin < (int)dest.vpm.inputs.size() &&
                           dest.vpm.inputs[dest.input_pin].port_kind == PortKind2::BangTrigger;

            // Node below baseline, more gap
            dest.layout = compute_node_layout(grp->node, dest.vpm, {0,0}, zoom);
            dest.layout.pos = {dest_x, baseline_y + gap * 3};

            rendered_nodes_.push_back({grp->node, dest.layout, dest.vpm, dest.display});

            // Wire for EACH connected pin on this node
            for (int pin_idx : grp->input_pins) {
                ImVec2 dst_pin_pos = dest.layout.input_pin_pos(pin_idx);
                ImVec2 dst_baseline = {dst_pin_pos.x, baseline_y};
                float curve_dy = std::abs(dst_pin_pos.y - baseline_y) * 0.5f;
                ImVec2 cp1 = {dst_baseline.x, dst_baseline.y + curve_dy};
                ImVec2 cp2 = {dst_pin_pos.x, dst_pin_pos.y - curve_dy};
                all_wires_.push_back({net, dst_baseline, cp1, cp2, dst_pin_pos,
                                      "", grp->id, net_id});
            }

            // Extra gap if node has side-bang stub (extends past right edge)
            float extra = dest.vpm.has_side_bang ? stub_len + S.pin_radius * zoom : 0.0f;
            dest_x += dest.layout.width + extra + NODE_GAP * zoom;
            row.dests.push_back(std::move(dest));
        }

        row.label_x = label_x;

        // Add short stub wires for non-primary connections on all rendered nodes
        auto is_unconnected = [](const BuilderEntryPtr& e) {
            if (!e) return true;
            auto n = e->as_net();
            return n && n->is_the_unconnected();
        };

        auto add_stubs = [&](const NodeLayout& layout, const VisualPinMap& vpm,
                             const BuilderEntryPtr& primary_net) {
            float stub_len = node_h * FADE_STUB_LENGTH_MULT;
            // Output pin stubs (curve downward from pin)
            for (int i = 0; i < (int)vpm.outputs.size(); i++) {
                auto& pin = vpm.outputs[i];
                if (!pin.arg) continue;
                auto an = pin.arg->as_net();
                if (!an) continue;
                auto entry = an->second();
                if (!entry || entry == primary_net || is_unconnected(entry)) continue;
                ImVec2 pp = layout.output_pin_pos(i);
                ImVec2 end = {pp.x, pp.y + stub_len};
                all_wires_.push_back({entry, pp, {pp.x, pp.y + stub_len * 0.3f},
                                      {pp.x, pp.y + stub_len * 0.7f}, end,
                                      "", "", an->first()});
            }
            // Input pin stubs (curve upward from pin)
            for (int i = 0; i < (int)vpm.inputs.size(); i++) {
                auto& pin = vpm.inputs[i];
                if (!pin.arg || pin.kind == VisualPinKind::AddDiamond || pin.kind == VisualPinKind::AbsentOptional) continue;
                auto an = pin.arg->as_net();
                if (!an) continue;
                auto entry = an->second();
                if (!entry || entry == primary_net || is_unconnected(entry)) continue;
                ImVec2 pp = layout.input_pin_pos(i);
                ImVec2 end = {pp.x, pp.y - stub_len};
                all_wires_.push_back({entry, pp, {pp.x, pp.y - stub_len * 0.3f},
                                      {pp.x, pp.y - stub_len * 0.7f}, end,
                                      "", "", an->first()});
            }
            // Side-bang stub
            if (vpm.has_side_bang && vpm.side_bang_arg) {
                auto an = vpm.side_bang_arg->as_net();
                if (an) {
                    auto entry = an->second();
                    if (entry && entry != primary_net && !is_unconnected(entry)) {
                        ImVec2 pp = layout.side_bang_pos();
                        ImVec2 end = {pp.x + stub_len, pp.y};
                        all_wires_.push_back({entry, pp, {pp.x + stub_len * 0.3f, pp.y},
                                              {pp.x + stub_len * 0.7f, pp.y}, end,
                                              "", "", an->first()});
                    }
                }
            }
        };

        add_stubs(row.src_layout, row.src_vpm, net);
        for (auto& dest : row.dests)
            add_stubs(dest.layout, dest.vpm, net);

        // Baseline as two flat wire segments (left of label, right of label)
        // so they get automatic hit-testing and highlighting
        float bl_left = src_baseline.x;
        float bl_right = dest_x > dest_start_x ? dest_x : dest_start_x;
        // Estimate label width for the gap
        ImVec2 label_sz = ImGui::CalcTextSize(net_id.c_str());
        float label_w = label_sz.x * canvas_zoom_ * 0.8f / ImGui::GetFontSize() * ImGui::GetFontSize();
        // Simpler: just use a fixed estimate
        float label_half = (label_sz.x * zoom * 0.8f + 6.0f) * 0.5f;
        float label_cx = label_x + label_half;

        // Left segment: source baseline → just before label
        if (label_x - 3.0f * zoom > bl_left) {
            ImVec2 l0 = {bl_left, baseline_y};
            ImVec2 l1 = {label_x - 3.0f * zoom, baseline_y};
            all_wires_.push_back({net, l0, l0, l1, l1,
                                  src_node->id(), "", net_id});
        }
        // Right segment: just after label → rightmost dest
        if (bl_right > label_x + label_half * 2 + 3.0f * zoom) {
            ImVec2 r0 = {label_x + label_half * 2 + 3.0f * zoom, baseline_y};
            ImVec2 r1 = {bl_right, baseline_y};
            all_wires_.push_back({net, r0, r0, r1, r1,
                                  "", "", net_id});
        }

        rows_.push_back(std::move(row));
        row_idx++;
    }
}

// ─── Draw ───

void NetsEditor::draw() {
    if (!gb_) {
        ImGui::TextDisabled("No graph loaded");
        return;
    }
    draw_canvas("##canvas_nets");
}

// ─── draw_content ───

void NetsEditor::draw_content(const CanvasFrame& frame) {
    rebuild_layout(frame.canvas_origin);

    float zoom = canvas_zoom_;

    float row_h = S.node_height * ROW_HEIGHT_MULT * zoom;

    // Track which nodes already had tooltips to avoid duplicates
    std::set<FlowNodeBuilderPtr> tooltipped_nodes;

    for (int ri = 0; ri < (int)rows_.size(); ri++) {
        auto& row = rows_[ri];
        float baseline_y = frame.canvas_origin.y + row.row_y;

        // Draw separator between rows
        if (ri > 0) {
            float sep_y = baseline_y - row_h * 0.5f;
            float sep_left = frame.canvas_origin.x;
            float sep_right = sep_left + frame.canvas_sz.x / zoom * 2.0f; // wide enough
            frame.dl->AddLine({sep_left, sep_y}, {sep_right, sep_y},
                              IM_COL32(60, 60, 80, 120), 1.0f);
        }

        // Render source node (only show tooltip on first occurrence)
        auto* src_nt = find_node_type2(row.src_node->type_id);
        auto src_state = build_render_state(row.src_node, hover_item_, shared_.get());
        bool src_tt = draw_tooltips_ && tooltipped_nodes.insert(row.src_node).second;
        render_node(frame.dl, row.src_node, src_nt, row.src_layout, row.src_vpm,
                    row.src_display, src_state, zoom, src_tt);

        // Render destination nodes
        for (auto& dest : row.dests) {
            auto* dst_nt = find_node_type2(dest.node->type_id);
            auto dst_state = build_render_state(dest.node, hover_item_, shared_.get());
            bool dst_tt = draw_tooltips_ && tooltipped_nodes.insert(dest.node).second;
            render_node(frame.dl, dest.node, dst_nt, dest.layout, dest.vpm,
                        dest.display, dst_state, zoom, dst_tt);
        }
    }

    // Draw all wires (baseline segments are included as WireInfo)
    for (auto& w : all_wires_) {
        render_wire(frame.dl, w, zoom);
    }

    // Draw net labels ON TOP of wires so highlights don't cover them
    float font_size = ImGui::GetFontSize() * zoom * 0.8f;
    if (font_size > 5.0f) {
        for (auto& row : rows_) {
            float baseline_y = frame.canvas_origin.y + row.row_y;
            ImVec2 text_sz = ImGui::CalcTextSize(row.net_id.c_str());
            float tw = text_sz.x * (font_size / ImGui::GetFontSize());
            float th = text_sz.y * (font_size / ImGui::GetFontSize());
            frame.dl->AddRectFilled({row.label_x - 3, baseline_y - th * 0.5f - 1},
                                    {row.label_x + tw + 3, baseline_y + th * 0.5f + 1},
                                    S.col_label_bg, S.node_rounding);
            frame.dl->AddText(nullptr, font_size, {row.label_x, baseline_y - th * 0.5f},
                              S.col_label_text, row.net_id.c_str());
        }
    }
}

// ─── Hover detection ───

HoverItem NetsEditor::do_detect_hover(ImVec2 mouse, ImVec2 canvas_origin) {
    // Build hit targets from all rendered node instances
    std::vector<NodeHitTarget> targets;
    targets.reserve(rendered_nodes_.size());
    for (auto& rn : rendered_nodes_) {
        auto* nt = find_node_type2(rn.node->type_id);
        if (!nt) continue;
        targets.push_back({rn.node, nt, &rn.layout, &rn.vpm});
    }

    auto wire_hit = hit_test_wires(mouse, all_wires_, canvas_zoom_);
    auto node_hit = hit_test_node_bodies(mouse, targets, canvas_zoom_);
    auto pin_hit = hit_test_pins(mouse, targets, canvas_zoom_);

    HitResult best = wire_hit;
    if (node_hit.distance < best.distance) best = node_hit;
    if (pin_hit.distance < best.distance) best = pin_hit;

    return best.item;
}

void NetsEditor::do_draw_hover_effects(ImDrawList* dl, ImVec2 canvas_origin, const HoverItem& hover) {
    if (std::holds_alternative<std::monostate>(hover)) return;

    BuilderEntryPtr hover_entry = nullptr;
    if (auto* ep = std::get_if<BuilderEntryPtr>(&hover)) {
        hover_entry = *ep;
    }

    // Highlight all wire segments sharing the hovered entry (nets and lambda nodes)
    if (hover_entry) {
        for (auto& w : all_wires_) {
            if (w.entry() == hover_entry)
                render_wire_highlight(dl, w, canvas_zoom_);
        }
        if (draw_tooltips_) {
            for (auto& w : all_wires_) {
                if (w.entry() == hover_entry) {
                    tooltip_wire(w);
                    break;
                }
            }
        }
    }
}

FlowNodeBuilderPtr NetsEditor::hover_to_node(const HoverItem& item) {
    if (auto* ep = std::get_if<BuilderEntryPtr>(&item)) {
        if (*ep) return (*ep)->as_node();
    } else if (auto* pin = std::get_if<FlowArg2Ptr>(&item)) {
        return (*pin)->node();
    }
    return nullptr;
}

bool NetsEditor::test_drag_overlap(const FlowNodeBuilderPtr&, float, float) {
    return true; // Dragging disabled — positions are computed
}

std::vector<VisualEditor::BoxTestNode> NetsEditor::get_box_test_nodes() {
    std::vector<BoxTestNode> result;
    for (auto& rn : rendered_nodes_) {
        result.push_back({rn.node, rn.layout.pos.x, rn.layout.pos.y,
                          rn.layout.width, rn.layout.height});
    }
    return result;
}
