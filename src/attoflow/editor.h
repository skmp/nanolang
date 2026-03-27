#pragma once
#include "sdl_imgui_window.h"
#include "atto/model.h"
#include "atto/types.h"
#include "editor2.h"
#include <string>
#include <vector>
#include <set>
#include <thread>
#include <atomic>
#include <mutex>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

// Conversion between Vec2 (model) and ImVec2 (UI)
inline ImVec2 to_imvec(Vec2 v) { return {v.x, v.y}; }
inline Vec2 to_vec2(ImVec2 v) { return {v.x, v.y}; }

// Per-tab state: each open .atto file gets its own TabState
struct TabState {
    FlowGraph graph;  // legacy (Editor1)
    Editor2Pane editor2; // new editor pane
    bool use_editor2 = true; // true = use Editor2Pane, false = legacy
    std::string file_path;       // absolute path to this .atto file
    std::string tab_name;        // display name (filename without extension)
    bool dirty = false;

    // Canvas
    ImVec2 canvas_offset = {0, 0};
    float canvas_zoom = 1.0f;

    // Selection
    std::set<int> selected_nodes;

    // Undo/Redo
    std::vector<std::string> undo_stack;
    std::vector<std::string> redo_stack;

    // Type inference
    TypePool type_pool;
    bool inference_dirty = true;

    // Clipboard
    struct ClipboardNode {
        NodeTypeID type_id; std::string args;
        ImVec2 offset; // relative to centroid
    };
    struct ClipboardLink {
        int from_idx, to_idx; // indices into clipboard_nodes
        std::string from_pin_name, to_pin_name;
    };
    std::vector<ClipboardNode> clipboard_nodes;
    std::vector<ClipboardLink> clipboard_links;

    // Highlight animation
    int highlight_node_id = -1;
    float highlight_timer = 0.0f;
};

class FlowEditorWindow {
public:
    bool init(const std::string& project_dir = "");
    void shutdown();
    bool is_open() const { return win_.open; }

    void process_event(SDL_Event& e);
    void draw();

    SdlImGuiWindow& sdl_window() { return win_; }
    FlowGraph& graph() { return active().graph; }

    // Tab management
    TabState& active() { return tabs_[active_tab_]; }
    const TabState& active() const { return tabs_[active_tab_]; }
    void open_tab(const std::string& file_path);
    void close_tab(int idx);
    void scan_project_files();

private:
    SdlImGuiWindow win_;

    // Project
    std::string project_dir_;
    std::vector<std::string> project_files_; // cached .atto filenames
    float file_panel_width_ = 200.0f;

    // Tabs
    std::vector<TabState> tabs_;
    int active_tab_ = 0;

    // Per-tab helpers (operate on active tab)
    void mark_dirty();
    void auto_save();
    void push_undo();
    void undo();
    void redo();
    void copy_selection();
    void paste_at(ImVec2 canvas_pos);

    // Debounced save
    void schedule_save();
    double save_deadline_ = 0; // 0 = no pending save
    void check_debounced_save();

    // Interaction state (global — always applies to active tab)
    int dragging_node_ = -1;
    bool dragging_selection_ = false;
    std::string dragging_link_from_pin_;
    bool dragging_link_from_output_ = true; // true if drag started from output-like pin
    ImVec2 dragging_link_start_;
    bool canvas_dragging_ = false;
    ImVec2 canvas_drag_start_;

    // Grabbed links
    struct GrabbedLink { std::string from_pin; std::string to_pin; };
    std::vector<GrabbedLink> grabbed_links_;
    std::string grabbed_pin_;
    bool grab_is_output_ = false;
    bool grab_pending_ = false;
    ImVec2 grab_start_;

    // Box selection
    bool box_selecting_ = false;
    ImVec2 box_select_start_;

    // Node name editing
    int editing_node_ = -1;
    std::string edit_buf_;
    bool edit_just_opened_ = false;
    bool edit_cursor_to_end_ = false;
    bool creating_new_node_ = false;
    ImVec2 new_node_pos_;

    // Link/wire name editing
    int editing_link_ = -1;
    std::string link_edit_buf_;
    bool link_edit_just_opened_ = false;

    // Shadow pin filtering (rebuilt each frame before drawing)
    std::set<std::string> shadow_connected_pins_; // pin IDs connected from shadow nodes

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

    // Validation & type inference
    void validate_nodes();
    void run_type_inference();

    // Navigation
    void center_on_node(const FlowNode& node, ImVec2 canvas_size);

    // Viewport sync
    void sync_viewport(TabState& tab);

    // Panel sizes
    float side_panel_width_ = 200.0f;
    float bottom_panel_height_ = 250.0f;

    // Run/Stop
    enum class BuildState { Idle, Building, Running, BuildFailed };
    std::atomic<BuildState> build_state_{BuildState::Idle};
    std::string build_log_;
    std::mutex build_log_mutex_;
    bool show_build_log_ = false;
    char search_buf_[128] = {};
    float last_canvas_w_ = 800, last_canvas_h_ = 600;
    std::thread build_thread_;
#ifdef _WIN32
    HANDLE child_process_ = nullptr;
#else
    pid_t child_pid_ = 0;
#endif
    void run_program(bool release = false);
    void stop_program();
    void poll_child_process();
    void draw_toolbar();
};
