#pragma once
#include "atto_editor_shared_state.h"
#include "node_renderer.h"
#include "imgui.h"
#include <memory>
#include <vector>

// Reusable 2D canvas interaction layer.
// Provides pan/zoom/select/drag/wire-connect; subclass provides content drawing and hit-testing.
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

    // Node drag state
    bool dragging_started_ = false;
    bool drag_was_overlapping_ = false;
    bool selection_rect_active_ = false;
    ImVec2 selection_rect_start_ = {0, 0};

    // Wire drag state (right-click: create new wire)
    bool wire_drag_active_ = false;
    FlowArg2Ptr wire_drag_pin_;        // pin being dragged from
    PortPosition2 wire_drag_pin_pos_;  // Input or Output
    ImVec2 wire_drag_start_;           // screen position of the pin
    bool wire_drag_is_source_ = false; // whether this pin is a wire source

    // Wire grab state (left-click: move existing wire)
    bool wire_grab_active_ = false;
    FlowArg2Ptr wire_grab_pin_;        // pin whose wire was grabbed
    PortPosition2 wire_grab_pin_pos_;
    ImVec2 wire_grab_anchor_;          // screen pos of the anchored end

    // ─── Subclass hooks ───

    virtual void draw_content(const CanvasFrame& frame) = 0;
    virtual HoverItem do_detect_hover(ImVec2 mouse, ImVec2 canvas_origin) = 0;
    virtual void do_draw_hover_effects(ImDrawList* dl, ImVec2 canvas_origin, const HoverItem& hover) = 0;
    virtual FlowNodeBuilderPtr hover_to_node(const HoverItem& item) = 0;
    virtual bool test_drag_overlap(const FlowNodeBuilderPtr& sel, float nx, float ny) = 0;

    struct BoxTestNode {
        FlowNodeBuilderPtr node;
        float x, y, w, h;
    };
    virtual std::vector<BoxTestNode> get_box_test_nodes() = 0;
    virtual void on_nodes_moved() {}

    // Wire connection hooks (subclass implements actual graph mutations)

    // Get the screen position of a pin (for wire preview drawing)
    virtual ImVec2 get_pin_screen_pos(const FlowArg2Ptr& pin) { return {0,0}; }

    // Determine if a pin is in the inputs or outputs of its node
    virtual PortPosition2 get_pin_position(const FlowArg2Ptr& pin) { return PortPosition2::Input; }

    // Check if a pin has an existing connection (not $unconnected)
    virtual bool pin_is_connected(const FlowArg2Ptr& pin) { return false; }

    // Execute a wire connection between two pins. Return true if successful.
    virtual bool do_connect_pins(const FlowArg2Ptr& from_pin, PortPosition2 from_pos,
                                  const FlowArg2Ptr& to_pin, PortPosition2 to_pos) { return false; }

    // Disconnect a pin from its current net. Return true if was connected.
    virtual bool do_disconnect_pin(const FlowArg2Ptr& pin, PortPosition2 pos) { return false; }

    // Reconnect a previously disconnected pin (undo grab). Called on cancel.
    virtual void do_reconnect_pin(const FlowArg2Ptr& pin, PortPosition2 pos) {}

    // Delete a node, net, or pin's connection. Called on ctrl+right-click release.
    virtual void do_delete_hovered(const HoverItem& item) {}
};
