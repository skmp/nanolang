#pragma once
#include "editor_pane.h"
#include "atto/model.h"
#include "atto/types.h"
#include "imgui.h"
#include <string>
#include <vector>
#include <set>
#include <memory>

// Conversion between Vec2 (model) and ImVec2 (UI)
inline ImVec2 to_imvec(Vec2 v) { return {v.x, v.y}; }
inline Vec2 to_vec2(ImVec2 v) { return {v.x, v.y}; }

class Editor1Pane : public IEditorPane {
public:
    // IEditorPane
    bool load(const std::string& path) override;
    void draw() override;
    bool is_loaded() const override { return !graph_.nodes.empty() || !file_path_.empty(); }
    bool is_dirty() const override { return dirty_; }
    const std::string& file_path() const override { return file_path_; }
    const std::string& tab_name() const override { return tab_name_; }

    // Legacy graph access (for FlowEditorWindow toolbar/build)
    FlowGraph& graph() { return graph_; }

    // Edit operations
    void mark_dirty();
    void push_undo();
    void undo();
    void redo();
    void copy_selection();
    void paste_at(ImVec2 canvas_pos);

    // Debounced save
    void schedule_save();
    void check_debounced_save();
    void auto_save();

    // Validation & type inference
    void validate_nodes();
    void run_type_inference();

    // Navigation
    void center_on_node(const FlowNode& node, ImVec2 canvas_size);

    // Viewport sync (call before save)
    void sync_viewport();

private:
    // Model
    FlowGraph graph_;
    std::string file_path_;
    std::string tab_name_;
    bool dirty_ = false;

    // Canvas
    ImVec2 canvas_offset_ = {0, 0};
    float canvas_zoom_ = 1.0f;

    // Selection
    std::set<int> selected_nodes_;

    // Undo/Redo
    std::vector<std::string> undo_stack_;
    std::vector<std::string> redo_stack_;

    // Type inference
    TypePool type_pool_;
    bool inference_dirty_ = true;

    // Clipboard
    struct ClipboardNode {
        NodeTypeID type_id; std::string args;
        ImVec2 offset;
    };
    struct ClipboardLink {
        int from_idx, to_idx;
        std::string from_pin_name, to_pin_name;
    };
    std::vector<ClipboardNode> clipboard_nodes_;
    std::vector<ClipboardLink> clipboard_links_;

    // Highlight animation
    int highlight_node_id_ = -1;
    float highlight_timer_ = 0.0f;

    // Interaction state
    int dragging_node_ = -1;
    bool dragging_selection_ = false;
    std::string dragging_link_from_pin_;
    bool dragging_link_from_output_ = true;
    ImVec2 dragging_link_start_;
    bool canvas_dragging_ = false;
    ImVec2 canvas_drag_start_;

    struct GrabbedLink { std::string from_pin; std::string to_pin; };
    std::vector<GrabbedLink> grabbed_links_;
    std::string grabbed_pin_;
    bool grab_is_output_ = false;
    bool grab_pending_ = false;
    ImVec2 grab_start_;

    bool box_selecting_ = false;
    ImVec2 box_select_start_;

    int editing_node_ = -1;
    std::string edit_buf_;
    bool edit_just_opened_ = false;
    bool edit_cursor_to_end_ = false;
    bool creating_new_node_ = false;
    ImVec2 new_node_pos_;

    int editing_link_ = -1;
    std::string link_edit_buf_;
    bool link_edit_just_opened_ = false;

    std::set<std::string> shadow_connected_pins_;

    // Debounced save
    double save_deadline_ = 0;

    // Drawing helpers
    ImVec2 canvas_to_screen(ImVec2 p, ImVec2 canvas_origin) const;
    ImVec2 screen_to_canvas(ImVec2 p, ImVec2 canvas_origin) const;
    ImVec2 get_pin_pos(const FlowNode& node, const FlowPin& pin, ImVec2 canvas_origin) const;
    void draw_node(ImDrawList* dl, FlowNode& node, ImVec2 canvas_origin);
    void draw_link(ImDrawList* dl, const FlowLink& link, ImVec2 canvas_origin);

    // Hit testing
    struct PinHit { int node_id; std::string pin_id; FlowPin::Direction dir; };
    PinHit hit_test_pin(ImVec2 screen_pos, ImVec2 canvas_origin, float radius = 8.0f) const;
    int hit_test_link(ImVec2 screen_pos, ImVec2 canvas_origin, float threshold = 6.0f) const;
};
