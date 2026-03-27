#include "visual_editor.h"
#include <algorithm>
#include <cmath>

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

    // Extract hover node
    FlowNodeBuilderPtr hover_node = hover_to_node(hover_item_);

    // ─── Selection + dragging with left mouse ───
    if (canvas_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        bool ctrl = ImGui::GetIO().KeyCtrl;

        if (ctrl && hover_node) {
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

            // Check initial overlap
            drag_was_overlapping_ = false;
            for (auto& sel : shared_->selected_nodes) {
                if (test_drag_overlap(sel, sel->position.x, sel->position.y)) {
                    drag_was_overlapping_ = true;
                    break;
                }
            }
        } else {
            shared_->selected_nodes.clear();
            selection_rect_active_ = true;
            ImVec2 mouse = ImGui::GetIO().MousePos;
            selection_rect_start_ = {(mouse.x - canvas_origin.x) / canvas_zoom_,
                                     (mouse.y - canvas_origin.y) / canvas_zoom_};
        }
    }

    // Drag selected nodes
    if (dragging_started_ && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !shared_->selected_nodes.empty()) {
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
