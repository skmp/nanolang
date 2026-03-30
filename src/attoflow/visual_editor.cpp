#include "visual_editor.h"
#include <algorithm>
#include <cmath>

static FlowArg2Ptr hover_to_pin(const HoverItem& item) {
    if (auto* pp = std::get_if<FlowArg2Ptr>(&item))
        return *pp;
    return nullptr;
}

void VisualEditor::draw_canvas(const char* id) {
    ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
    ImVec2 canvas_sz = ImGui::GetContentRegionAvail();
    if (canvas_sz.x < 50.0f) canvas_sz.x = 50.0f;
    if (canvas_sz.y < 50.0f) canvas_sz.y = 50.0f;

    ImGui::InvisibleButton(id, canvas_sz,
                            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool canvas_hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 canvas_origin = v2add(canvas_p0, canvas_offset_);

    render_background(dl, canvas_p0, canvas_sz, canvas_offset_, canvas_zoom_);

    dl->PushClipRect(canvas_p0, v2add(canvas_p0, canvas_sz), true);

    CanvasFrame frame{dl, canvas_p0, canvas_sz, canvas_origin, canvas_hovered};
    draw_content(frame);

    dl->PopClipRect();

    // ─── Hover detection + effects ───
    if (canvas_hovered) {
        ImVec2 mouse = ImGui::GetIO().MousePos;
        hover_item_ = do_detect_hover(mouse, canvas_origin);
    } else {
        hover_item_ = std::monostate{};
    }
    do_draw_hover_effects(dl, canvas_origin, hover_item_);

    FlowNodeBuilderPtr hover_node = hover_to_node(hover_item_);
    FlowArg2Ptr hover_pin = hover_to_pin(hover_item_);

    // ─── Wire drag preview ───
    if (wire_drag_active_ || wire_grab_active_) {
        ImVec2 from = wire_drag_active_ ? wire_drag_start_ : wire_grab_anchor_;
        ImVec2 to = ImGui::GetIO().MousePos;
        float dy = std::max(std::abs(to.y - from.y) * 0.5f, 30.0f * canvas_zoom_);
        ImVec2 cp1 = {from.x, from.y + dy};
        ImVec2 cp2 = {to.x, to.y - dy};
        dl->AddBezierCubic(from, cp1, cp2, to, S.col_wire, S.wire_thickness * canvas_zoom_);
    }

    // ─── Left-click: wire creation from pin, OR selection/node drag ───
    if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        bool ctrl = ImGui::GetIO().KeyCtrl;

        if (hover_pin && !ctrl && !wire_drag_active_ && !wire_grab_active_) {
            // Left-click on pin → start new wire creation
            auto pos = get_pin_position(hover_pin);
            auto port = hover_pin->port();
            PortKind2 kind = port ? port->kind : PortKind2::Data;
            if (is_wire_source(kind, pos) || is_wire_dest(kind, pos)) {
                wire_drag_active_ = true;
                wire_drag_pin_ = hover_pin;
                wire_drag_pin_pos_ = pos;
                wire_drag_start_ = get_pin_screen_pos(hover_pin);
                wire_drag_is_source_ = is_wire_source(kind, pos);
            }
        } else if (ctrl && hover_node) {
            if (shared_->selected_nodes.count(hover_node))
                shared_->selected_nodes.erase(hover_node);
            else
                shared_->selected_nodes.insert(hover_node);
        } else if (hover_node) {
            if (!shared_->selected_nodes.count(hover_node)) {
                shared_->selected_nodes.clear();
                shared_->selected_nodes.insert(hover_node);
            }
            dragging_started_ = true;

            drag_was_overlapping_ = false;
            for (auto& sel : shared_->selected_nodes) {
                if (test_drag_overlap(sel, sel->position.x, sel->position.y)) {
                    drag_was_overlapping_ = true;
                    break;
                }
            }
        } else if (!hover_pin) {
            shared_->selected_nodes.clear();
            selection_rect_active_ = true;
            ImVec2 mouse = ImGui::GetIO().MousePos;
            selection_rect_start_ = {(mouse.x - canvas_origin.x) / canvas_zoom_,
                                     (mouse.y - canvas_origin.y) / canvas_zoom_};
        }
    }

    // Left-click release
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (wire_drag_active_) {
            // Complete wire creation
            if (hover_pin && hover_pin != wire_drag_pin_) {
                auto to_pos = get_pin_position(hover_pin);
                if (can_connect_pins(wire_drag_pin_, wire_drag_pin_pos_, hover_pin, to_pos))
                    do_connect_pins(wire_drag_pin_, wire_drag_pin_pos_, hover_pin, to_pos);
            }
            wire_drag_active_ = false;
            wire_drag_pin_ = nullptr;
        }
        dragging_started_ = false;
        selection_rect_active_ = false;
    }

    // ─── Right-click: wire grab/move from connected pin, OR pan ───
    bool wire_grab_just_started = false;
    if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (hover_pin && pin_is_connected(hover_pin) && !wire_grab_active_) {
            // Right-click on connected pin → grab and move the wire
            auto pos = get_pin_position(hover_pin);
            wire_grab_active_ = true;
            wire_grab_just_started = true;
            wire_grab_pin_ = hover_pin;
            wire_grab_pin_pos_ = pos;
            wire_grab_anchor_ = get_pin_screen_pos(hover_pin);
            do_disconnect_pin(hover_pin, pos);
        }
    }

    // Right-click release (skip if grab just started this frame)
    if (!wire_grab_just_started && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        if (wire_grab_active_) {
            bool connected = false;
            if (hover_pin && hover_pin != wire_grab_pin_) {
                auto to_pos = get_pin_position(hover_pin);
                if (can_connect_pins(wire_grab_pin_, wire_grab_pin_pos_, hover_pin, to_pos))
                    connected = do_connect_pins(wire_grab_pin_, wire_grab_pin_pos_, hover_pin, to_pos);
            }
            if (!connected)
                do_reconnect_pin(wire_grab_pin_, wire_grab_pin_pos_);
            wire_grab_active_ = false;
            wire_grab_pin_ = nullptr;
        } else if (canvas_hovered && ImGui::GetIO().KeyCtrl &&
                   !std::holds_alternative<std::monostate>(hover_item_)) {
            // Ctrl+right-click release: delete hovered item
            do_delete_hovered(hover_item_);
        }
    }

    // Right-click drag: pan (only if not wire-grabbing)
    if (!wire_grab_active_ && canvas_hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
        canvas_offset_.x += ImGui::GetIO().MouseDelta.x;
        canvas_offset_.y += ImGui::GetIO().MouseDelta.y;
    }

    // ─── Node dragging (left-click, not wire-dragging) ───
    if (!wire_drag_active_ && !wire_grab_active_ && dragging_started_ &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !shared_->selected_nodes.empty()) {
        ImVec2 delta = ImGui::GetIO().MouseDelta;
        float dx = delta.x / canvas_zoom_;
        float dy = delta.y / canvas_zoom_;

        bool blocked = false;
        if (!drag_was_overlapping_) {
            for (auto& sel : shared_->selected_nodes) {
                if (test_drag_overlap(sel, sel->position.x + dx, sel->position.y + dy)) {
                    blocked = true;
                    break;
                }
            }
        }
        if (!blocked) {
            for (auto& sel : shared_->selected_nodes) {
                sel->position.x += dx;
                sel->position.y += dy;
            }
            on_nodes_moved();
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

        shared_->selected_nodes.clear();
        for (auto& btn : get_box_test_nodes()) {
            if (btn.x < x1 && btn.x + btn.w > x0 && btn.y < y1 && btn.y + btn.h > y0)
                shared_->selected_nodes.insert(btn.node);
        }
    }

    // Pan with middle mouse
    if (canvas_hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        canvas_offset_.x += ImGui::GetIO().MouseDelta.x;
        canvas_offset_.y += ImGui::GetIO().MouseDelta.y;
    }

    // Scroll: zoom, or pan with modifiers
    if (canvas_hovered) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0) {
            bool shift = ImGui::GetIO().KeyShift;
            bool alt = ImGui::GetIO().KeyAlt;
            if (shift || alt) {
                float pan_speed = S.scroll_pan_speed;
                if (shift) canvas_offset_.x += wheel * pan_speed;
                if (alt)   canvas_offset_.y += wheel * pan_speed;
            } else {
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
}
