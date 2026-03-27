#pragma once
#include "atto/graph_builder.h"
#include "atto/node_types2.h"
#include "imgui.h"
#include <string>
#include <memory>
#include <set>

// Editor2Pane: new editor using GraphBuilder exclusively.
// No FlowGraph, no inference, no codegen.
class Editor2Pane {
public:
    // Load an .atto file (instrument@atto:0 format)
    bool load(const std::string& path);

    // Draw the pane into the current ImGui context
    void draw();

    bool is_loaded() const { return gb_ != nullptr; }
    bool is_dirty() const { return dirty_; }
    const std::string& file_path() const { return file_path_; }
    const std::string& tab_name() const { return tab_name_; }

private:
    std::shared_ptr<GraphBuilder> gb_;
    std::string file_path_;
    std::string tab_name_;
    bool dirty_ = false;

    // Canvas state
    ImVec2 canvas_offset_ = {0, 0};
    float canvas_zoom_ = 1.0f;

    // Interaction state
    std::set<NodeId> selected_nodes_;
    NodeId dragging_node_;         // node being dragged (empty = none)
    bool dragging_started_ = false;
    bool drag_was_overlapping_ = false; // true if node was overlapping when drag began
    int editing_link_id_ = -1; // not used yet, placeholder

    // Drawing helpers
    void draw_node(ImDrawList* dl, const FlowNodeBuilderPtr& node,
                   ImVec2 canvas_origin);
    void draw_net(ImDrawList* dl, const NetBuilderPtr& net,
                  ImVec2 canvas_origin);
};
