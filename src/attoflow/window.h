#pragma once
#include "sdl_imgui_window.h"
#include "tab.h"
#include "editor2.h"
#include "nets_editor.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

class FlowEditorWindow {
public:
    bool init(const std::string& project_dir = "");
    void shutdown();
    bool is_open() const { return win_.open; }

    void process_event(SDL_Event& e);
    void draw();

    SdlImGuiWindow& sdl_window() { return win_; }

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
    std::vector<std::string> project_files_;
    float file_panel_width_ = 200.0f;

    // Tabs
    std::vector<TabState> tabs_;
    int active_tab_ = 0;
    int pending_tab_select_ = -1; // one-shot: set to force tab selection next frame

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
