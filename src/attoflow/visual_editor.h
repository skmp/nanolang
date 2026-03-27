#pragma once
#include "atto_editor_shared_state.h"
#include "node_renderer.h"
#include "imgui.h"
#include <memory>
#include <vector>

// Reusable 2D canvas interaction layer.
// Provides pan/zoom/select/drag; subclass provides content drawing and hit-testing.
class VisualEditor {
public:
    VisualEditor(const std::shared_ptr<AttoEditorSharedState>& shared) : shared_(shared) {}
    virtual ~VisualEditor() = default;

    struct CanvasFrame {
        ImDrawList* dl;
        ImVec2 canvas_p0;
        ImVec2 canvas_sz;
        ImVec2 canvas_origin;
        bool hovered;
    };

    // Call from draw(). Sets up canvas, calls draw_content, handles all interaction.
    void draw_canvas(const char* id);

    // Canvas state
    ImVec2 canvas_offset() const { return canvas_offset_; }
    float canvas_zoom() const { return canvas_zoom_; }

    const HoverItem& hover_item() const { return hover_item_; }

protected:
    ImVec2 canvas_offset_ = {0, 0};
    float canvas_zoom_ = 1.0f;
    bool draw_tooltips_ = true;
    HoverItem hover_item_;
    std::shared_ptr<AttoEditorSharedState> shared_;

    // Interaction state
    bool dragging_started_ = false;
    bool drag_was_overlapping_ = false;
    bool selection_rect_active_ = false;
    ImVec2 selection_rect_start_ = {0, 0};

    // ─── Subclass hooks ───

    // Draw all content (nodes, wires) inside the clipped canvas
    virtual void draw_content(const CanvasFrame& frame) = 0;

    // Find what's under the mouse
    virtual HoverItem do_detect_hover(ImVec2 mouse, ImVec2 canvas_origin) = 0;

    // Draw hover highlights and tooltips
    virtual void do_draw_hover_effects(ImDrawList* dl, ImVec2 canvas_origin, const HoverItem& hover) = 0;

    // Extract draggable node from hover item (nullptr if not a node)
    virtual FlowNodeBuilderPtr hover_to_node(const HoverItem& item) = 0;

    // Test if moving sel to (nx, ny) would overlap non-selected nodes
    virtual bool test_drag_overlap(const FlowNodeBuilderPtr& sel, float nx, float ny) = 0;

    // Get all nodes for box-select testing (canvas-space, unzoomed)
    struct BoxTestNode {
        FlowNodeBuilderPtr node;
        float x, y, w, h;
    };
    virtual std::vector<BoxTestNode> get_box_test_nodes() = 0;

    // Called when dragging moves selected nodes (subclass can mark wires dirty etc.)
    virtual void on_nodes_moved() {}
};
