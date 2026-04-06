#include "window.h"
#include "atto/graph_builder.h"
#include "atto/args.h"
#include "atto/serial.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#ifndef _WIN32
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

bool FlowEditorWindow::init(const std::string& project_dir) {
    if (!win_.init("Flow Editor", 900, 600)) return false;
    project_dir_ = project_dir;

    if (!project_dir_.empty()) {
        scan_project_files();
        namespace fs = std::filesystem;
        std::string main_path = (fs::path(project_dir_) / "main.atto").string();
        if (fs::exists(main_path)) {
            open_tab(main_path);
        } else if (!project_files_.empty()) {
            open_tab((fs::path(project_dir_) / project_files_[0]).string());
        }
    }

    return true;
}

void FlowEditorWindow::scan_project_files() {
    namespace fs = std::filesystem;
    project_files_.clear();
    if (project_dir_.empty()) return;
    for (auto& entry : fs::directory_iterator(project_dir_)) {
        if (entry.path().extension() == ".atto") {
            project_files_.push_back(entry.path().filename().string());
        }
    }
    std::sort(project_files_.begin(), project_files_.end());
}

void FlowEditorWindow::open_tab(const std::string& file_path) {
    namespace fs = std::filesystem;
    std::string abs_path = fs::absolute(file_path).string();

    // Check if already open (match on file_path + graph editor type)
    for (int i = 0; i < (int)tabs_.size(); i++) {
        if (tabs_[i].file_path == abs_path && tabs_[i].pane &&
            std::string(tabs_[i].pane->type_name()) == "graph") {
            pending_tab_select_ = i;
            return;
        }
    }

    TabState tab;
    tab.file_path = abs_path;
    tab.tab_name = fs::path(file_path).stem().string();

    // Parse file into GraphBuilder
    if (fs::exists(abs_path)) {
        std::ifstream f(abs_path);
        if (f.is_open()) {
            auto result = Deserializer::parse_atto(f);
            if (auto* gb = std::get_if<std::shared_ptr<GraphBuilder>>(&result)) {
                tab.gb = *gb;
            } else {
                auto* err = std::get_if<BuilderError>(&result);
                fprintf(stderr, "Window: %s\n", err ? err->c_str() : "unknown error");
            }
        }
    }
    if (!tab.gb) tab.gb = std::make_shared<GraphBuilder>();
    tab.shared = std::make_shared<AttoEditorSharedState>();
    tab.pane = make_editor2(tab.gb, tab.shared);

    // Create nets editor tab sharing the same graph + state
    TabState nets_tab;
    nets_tab.file_path = abs_path;
    nets_tab.tab_name = tab.tab_name;
    nets_tab.gb = tab.gb;
    nets_tab.shared = tab.shared;
    nets_tab.pane = make_nets_editor(nets_tab.gb, nets_tab.shared);

    tabs_.push_back(std::move(tab));
    tabs_.push_back(std::move(nets_tab));
    pending_tab_select_ = (int)tabs_.size() - 2; // focus on the graph editor tab
}

void FlowEditorWindow::close_tab(int idx) {
    if (idx < 0 || idx >= (int)tabs_.size()) return;
    #if LEGACY_EDITOR
    // Auto-save before closing (Editor1Pane handles its own save)
    if (auto e1 = std::dynamic_pointer_cast<Editor1Pane>(tabs_[idx].pane)) {
        if (e1->is_dirty() && !e1->file_path().empty()) {
            e1->sync_viewport();
            e1->auto_save();
        }
    }
    #endif
    tabs_.erase(tabs_.begin() + idx);
    if (active_tab_ >= (int)tabs_.size())
        active_tab_ = std::max(0, (int)tabs_.size() - 1);
}

void FlowEditorWindow::shutdown() {
    stop_program();
    if (build_thread_.joinable())
        build_thread_.join();
    win_.shutdown();
}

void FlowEditorWindow::process_event(SDL_Event& e) { win_.process_event(e); }

void FlowEditorWindow::draw() {
    if (!win_.open) return;

    win_.begin_frame();
    ImGui::SetCurrentContext(win_.imgui_ctx);

    ImGui::SetNextWindowPos({0, 0});
    int w, h;
    SDL_GetWindowSize(win_.window, &w, &h);
    ImGui::SetNextWindowSize({(float)w, (float)h});
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    draw_toolbar();
    ImGui::Separator();

    poll_child_process();

    float total_w = (float)w;
    float total_h = ImGui::GetContentRegionAvail().y;

    file_panel_width_ = std::clamp(file_panel_width_, 80.0f, total_w * 0.3f);
    side_panel_width_ = std::clamp(side_panel_width_, 100.0f, total_w * 0.5f);
    bottom_panel_height_ = std::clamp(bottom_panel_height_, 40.0f, total_h * 0.5f);

    // --- File browser panel (left) ---
    ImGui::BeginChild("##file_browser", {file_panel_width_, total_h}, false,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::TextUnformatted("Files");
    ImGui::Separator();
    ImGui::BeginChild("##file_list", {0, 0}, false);
    for (auto& fname : project_files_) {
        namespace fs = std::filesystem;
        std::string stem = fs::path(fname).stem().string();
        bool is_active = (active_tab_ < (int)tabs_.size() &&
                          tabs_[active_tab_].tab_name == stem);
        if (is_active) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(100, 200, 255, 255));
        if (ImGui::Selectable(stem.c_str(), is_active)) {
            std::string full_path = (fs::path(project_dir_) / fname).string();
            open_tab(full_path);
        }
        if (is_active) ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::Button("##file_vsplitter", {4.0f, total_h});
    if (ImGui::IsItemActive())
        file_panel_width_ += ImGui::GetIO().MouseDelta.x;
    ImGui::SameLine();

    float center_w = total_w - file_panel_width_ - side_panel_width_ - 12.0f;
    float canvas_h = total_h - bottom_panel_height_ - 4.0f;

    // --- Center column: tabs + canvas + bottom panel ---
    ImGui::BeginGroup();

    // --- Tab bar ---
    if (ImGui::BeginTabBar("##atto_tabs")) {
        int pending_select = pending_tab_select_;
        pending_tab_select_ = -1;
        for (int i = 0; i < (int)tabs_.size(); i++) {
            std::string label = tabs_[i].label();
            label += "###tab" + std::to_string(i);
            bool open = true;
            ImGuiTabItemFlags flags = (i == pending_select) ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(label.c_str(), &open, flags)) {
                active_tab_ = i;
                ImGui::EndTabItem();
            }
            if (!open) {
                close_tab(i);
                if (i <= active_tab_ && active_tab_ > 0) active_tab_--;
                i--;
            }
        }
        ImGui::EndTabBar();
    }

    float canvas_w = center_w;
    last_canvas_w_ = canvas_w;
    last_canvas_h_ = canvas_h;

    // --- Canvas ---
    ImGui::BeginChild("##flow_canvas", {canvas_w, canvas_h}, false,
                      ImGuiWindowFlags_NoScrollbar);
    if (!tabs_.empty() && active().pane) {
        active().pane->draw();
    } else {
        ImVec2 sz = ImGui::GetContentRegionAvail();
        const char* msg = "Select a file from the file list to open it.";
        ImVec2 text_sz = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos({(sz.x - text_sz.x) * 0.5f, (sz.y - text_sz.y) * 0.5f});
        ImGui::TextDisabled("%s", msg);
    }
    ImGui::EndChild();

    // --- Horizontal splitter ---
    ImGui::InvisibleButton("##hsplitter", {canvas_w, 4.0f});
    if (ImGui::IsItemActive())
        bottom_panel_height_ -= ImGui::GetIO().MouseDelta.y;
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

    // --- Bottom panel ---
    // auto* e1 = dynamic_cast<Editor1Pane*>(active().pane.get());
    ImGui::BeginChild("##bottom_panel", {canvas_w, bottom_panel_height_}, true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (ImGui::BeginTabBar("##bottom_tabs")) {
#if LEGACY_EDITOR
        if (e1) {
            int error_count = 0;
            for (auto& node : e1->graph().nodes) if (!node.error.empty()) error_count++;
            for (auto& link : e1->graph().links) if (!link.error.empty()) error_count++;

            char errors_label[64];
            snprintf(errors_label, sizeof(errors_label), "Errors%s", error_count > 0 ? " (!)" : "");

            if (ImGui::BeginTabItem(errors_label)) {
                ImGui::BeginChild("##errors_scroll", {0, 0}, false);
                for (auto& node : e1->graph().nodes) {
                    if (node.error.empty()) continue;
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 100, 100, 255));
                    std::string label = std::string(node_type_str(node.type_id)) + " [" + node.guid.substr(0, 8) + "]: " + node.error;
                    if (ImGui::Selectable(label.c_str())) {
                        e1->center_on_node(node, {canvas_w, canvas_h});
                    }
                    ImGui::PopStyleColor();
                }
                for (auto& link : e1->graph().links) {
                    if (link.error.empty()) continue;
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 160, 80, 255));
                    std::string label = "link [" + link.from_pin.substr(0, 8) + "->...]: " + link.error;
                    if (ImGui::Selectable(label.c_str())) {
                        auto dot = link.from_pin.find('.');
                        if (dot != std::string::npos) {
                            std::string guid = link.from_pin.substr(0, dot);
                            for (auto& n : e1->graph().nodes) {
                                if (n.guid == guid) { e1->center_on_node(n, {canvas_w, canvas_h}); break; }
                            }
                        }
                    }
                    ImGui::PopStyleColor();
                }
                ImGui::Dummy({0, bottom_panel_height_ * 0.5f});
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
#endif

        if (ImGui::BeginTabItem("Build Log", nullptr, show_build_log_ ? ImGuiTabItemFlags_SetSelected : 0)) {
            show_build_log_ = false;
            ImGui::BeginChild("##buildlog_scroll", {0, 0}, false);
            {
                std::lock_guard<std::mutex> lock(build_log_mutex_);
                ImGui::TextWrapped("%s", build_log_.c_str());
            }
            ImGui::Dummy({0, bottom_panel_height_ * 0.5f});
            if (build_state_ == BuildState::Building) {
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40.0f)
                    ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::EndGroup();

    ImGui::SameLine();

    // --- Vertical splitter ---
    ImGui::InvisibleButton("##vsplitter", {4.0f, total_h});
    if (ImGui::IsItemActive())
        side_panel_width_ -= ImGui::GetIO().MouseDelta.x;
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    ImGui::SameLine();

    // --- Side panel: declarations ---
    ImGui::BeginChild("##side_panel", {side_panel_width_, total_h}, true);
#if LEGACY_EDITOR
    if (e1) {
        ImGui::TextUnformatted("Declarations");
        ImGui::Separator();
        for (auto& node : e1->graph().nodes) {
            auto* nt_decl = find_node_type(node.type_id);
            if (!nt_decl || !nt_decl->is_declaration) continue;
            if (node.imported || node.shadow) continue;
            bool has_err = !node.error.empty();
            if (has_err) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 100, 100, 255));
            if (ImGui::Selectable(node.display_text().c_str())) {
                e1->center_on_node(node, {canvas_w, canvas_h});
            }
            if (has_err) ImGui::PopStyleColor();
            if (has_err && ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(node.error.c_str());
                ImGui::EndTooltip();
            }
        }

        for (auto& imp_node : e1->graph().nodes) {
            if (imp_node.type_id != NodeTypeID::DeclImport) continue;
            auto tokens = tokenize_args(imp_node.args, false);
            if (tokens.empty()) continue;
            std::string label = tokens[0];
            if (label.size() >= 2 && label.front() == '"' && label.back() == '"')
                label = label.substr(1, label.size() - 2);
            if (ImGui::TreeNode(label.c_str())) {
                for (auto& node : e1->graph().nodes) {
                    if (!node.imported) continue;
                    auto* nt_decl = find_node_type(node.type_id);
                    if (!nt_decl || !nt_decl->is_declaration) continue;
                    ImGui::TextDisabled("%s", node.display_text().c_str());
                }
                ImGui::TreePop();
            }
        }
    }
#endif
    ImGui::EndChild();

    ImGui::End(); // main

#if LEGACY_EDITOR
    // Check debounced save for Editor1Pane
    if (e1) e1->check_debounced_save();
#endif

    win_.end_frame(30, 30, 40);
}

// --- Toolbar ---

void FlowEditorWindow::draw_toolbar() {
    auto state = build_state_.load();

    bool can_run = (state == BuildState::Idle || state == BuildState::BuildFailed);
    bool can_stop = (state == BuildState::Running);

    if (!can_run) ImGui::BeginDisabled();
    if (ImGui::Button("Run")) run_program(false);
    ImGui::SameLine();
    if (ImGui::Button("Run Release")) run_program(true);
    if (!can_run) ImGui::EndDisabled();

    ImGui::SameLine();
    if (!can_stop) ImGui::BeginDisabled();
    if (ImGui::Button("Stop")) stop_program();
    if (!can_stop) ImGui::EndDisabled();

    ImGui::SameLine();

    // Search (Editor1 only)
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputTextWithHint("##search", "Find node...", search_buf_, sizeof(search_buf_),
                                  ImGuiInputTextFlags_EnterReturnsTrue)) {
    #if LEGACY_EDITOR
        if (auto* e1 = dynamic_cast<Editor1Pane*>(active().pane.get())) {
            std::string query(search_buf_);
            if (!query.empty()) {
                for (auto& node : e1->graph().nodes) {
                    if (node.imported || node.shadow) continue;
                    if (node.guid.find(query) != std::string::npos ||
                        node.display_text().find(query) != std::string::npos) {
                        e1->center_on_node(node, {last_canvas_w_, last_canvas_h_});
                        break;
                    }
                }
            }
        }
    #endif
    }

    ImGui::SameLine();
    switch (state) {
    case BuildState::Idle:       ImGui::TextDisabled("Idle"); break;
    case BuildState::Building:   ImGui::TextColored({1.0f, 0.8f, 0.0f, 1.0f}, "Building..."); break;
    case BuildState::Running:    ImGui::TextColored({0.0f, 1.0f, 0.0f, 1.0f}, "Running"); break;
    case BuildState::BuildFailed: ImGui::TextColored({1.0f, 0.2f, 0.2f, 1.0f}, "Build Failed"); break;
    }
}

// --- Run/Stop ---

void FlowEditorWindow::run_program(bool release) {
    stop_program();
    if (build_thread_.joinable())
        build_thread_.join();

    show_build_log_ = true;
    {
        std::lock_guard<std::mutex> lock(build_log_mutex_);
        build_log_.clear();
    }

#if LEGACY_EDITOR
    // Auto-save via Editor1Pane if applicable
    if (auto* e1 = dynamic_cast<Editor1Pane*>(active().pane.get())) {
        e1->auto_save();
    }
#endif

    if (active().file_path.empty()) return;
    std::string active_path = active().file_path;

    namespace fs = std::filesystem;

    fs::path atto_path = fs::absolute(active_path);
    fs::path project_dir = atto_path.parent_path();
    std::string source_name = project_dir.filename().string();
    fs::path output_dir = project_dir / ".generated" / source_name;

    fs::path exe_dir;
#ifdef _WIN32
    char exe_buf[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_buf, MAX_PATH);
    exe_dir = fs::path(exe_buf).parent_path();
#elif defined(__APPLE__)
    {
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::string buf(size, '\0');
        _NSGetExecutablePath(buf.data(), &size);
        exe_dir = fs::canonical(buf).parent_path();
    }
#else
    exe_dir = fs::canonical("/proc/self/exe").parent_path();
#endif
    fs::path attoc_path = exe_dir / "attoc.exe";
    if (!fs::exists(attoc_path))
        attoc_path = exe_dir / "attoc";

    std::string tc_str;
#ifdef _WIN32
    {
        const char* vr = std::getenv("VCPKG_ROOT");
        if (!vr) {
            std::lock_guard<std::mutex> lock(build_log_mutex_);
            build_log_ += "Error: VCPKG_ROOT environment variable is not set\n";
            build_state_ = BuildState::BuildFailed;
            return;
        }
        tc_str = (fs::path(vr) / "scripts" / "buildsystems" / "vcpkg.cmake").string();
    }
#endif

    std::string attoc_str = attoc_path.string();
    std::string atto_str = project_dir.string();
    std::string out_str = output_dir.string();
    std::string sn = source_name;

    build_state_ = BuildState::Building;
    {
        std::lock_guard<std::mutex> lock(build_log_mutex_);
        build_log_.clear();
    }

    build_thread_ = std::thread([this, attoc_str, atto_str, out_str, tc_str, sn, release]() {
        namespace fs = std::filesystem;
        fs::create_directories(out_str);

        auto run_cmd = [this](const std::string& cmd) -> int {
#ifdef _WIN32
            std::string full_cmd = "\"" + cmd + " 2>&1\"";
            FILE* pipe = _popen(full_cmd.c_str(), "r");
#else
            std::string full_cmd = cmd + " 2>&1";
            FILE* pipe = popen(full_cmd.c_str(), "r");
#endif
            if (!pipe) return -1;
            char buf[256];
            while (fgets(buf, sizeof(buf), pipe)) {
                std::lock_guard<std::mutex> lock(build_log_mutex_);
                build_log_ += buf;
            }
#ifdef _WIN32
            return _pclose(pipe);
#else
            return pclose(pipe);
#endif
        };

        {
            std::lock_guard<std::mutex> lock(build_log_mutex_);
            build_log_ += "=== Running attoc ===\n";
        }
        std::string cmd1 = "\"" + attoc_str + "\" \"" + atto_str + "\" -o \"" + out_str + "\"";
        if (run_cmd(cmd1) != 0) { build_state_ = BuildState::BuildFailed; return; }

        std::string build_dir = out_str + "/build";
        std::string cache_file = build_dir + "/CMakeCache.txt";
        {
            std::ifstream cache_check(cache_file);
            if (!cache_check.good()) {
                {
                    std::lock_guard<std::mutex> lock(build_log_mutex_);
                    build_log_ += "\n=== CMake Configure ===\n";
                }
                std::string cmd2 = "cmake -B \"" + build_dir + "\" -S \"" + out_str + "\"";
                if (!tc_str.empty())
                    cmd2 += " \"-DCMAKE_TOOLCHAIN_FILE=" + tc_str + "\"";
                if (run_cmd(cmd2) != 0) { build_state_ = BuildState::BuildFailed; return; }
            } else {
                std::lock_guard<std::mutex> lock(build_log_mutex_);
                build_log_ += "\n=== CMake Configure (cached) ===\n";
            }
        }

        {
            std::lock_guard<std::mutex> lock(build_log_mutex_);
            build_log_ += "\n=== CMake Build ===\n";
        }
        std::string config = release ? "Release" : "Debug";
        std::string cmd3 = "cmake --build \"" + build_dir + "\" --config " + config + " --parallel";
        if (run_cmd(cmd3) != 0) { build_state_ = BuildState::BuildFailed; return; }

#ifdef _WIN32
        fs::path exe_path = fs::path(build_dir) / config / (sn + ".exe");
        if (!fs::exists(exe_path))
            exe_path = fs::path(build_dir) / (sn + ".exe");
#else
        fs::path exe_path = fs::path(build_dir) / sn;
#endif
        if (!fs::exists(exe_path)) {
            std::lock_guard<std::mutex> lock(build_log_mutex_);
            build_log_ += "\nError: executable not found at " + exe_path.string() + "\n";
            build_state_ = BuildState::BuildFailed;
            return;
        }

#ifdef _WIN32
        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        std::string exe_str = exe_path.string();
        if (CreateProcessA(exe_str.c_str(), nullptr, nullptr, nullptr, FALSE,
                          0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            child_process_ = pi.hProcess;
            build_state_ = BuildState::Running;
        } else {
            std::lock_guard<std::mutex> lock(build_log_mutex_);
            build_log_ += "\nError: failed to launch " + exe_str + "\n";
            build_state_ = BuildState::BuildFailed;
        }
#else
        pid_t pid = fork();
        if (pid == 0) {
            execl(exe_path.c_str(), exe_path.c_str(), nullptr);
            _exit(1);
        } else if (pid > 0) {
            child_pid_ = pid;
            build_state_ = BuildState::Running;
        } else {
            std::lock_guard<std::mutex> lock(build_log_mutex_);
            build_log_ += "\nError: fork failed\n";
            build_state_ = BuildState::BuildFailed;
        }
#endif
    });
}

void FlowEditorWindow::stop_program() {
#ifdef _WIN32
    if (child_process_) {
        TerminateProcess(child_process_, 0);
        WaitForSingleObject(child_process_, 1000);
        CloseHandle(child_process_);
        child_process_ = nullptr;
    }
#else
    if (child_pid_ > 0) {
        kill(child_pid_, SIGTERM);
        waitpid(child_pid_, nullptr, 0);
        child_pid_ = 0;
    }
#endif
    build_state_ = BuildState::Idle;
}

void FlowEditorWindow::poll_child_process() {
    if (build_state_.load() != BuildState::Running) return;

#ifdef _WIN32
    if (child_process_) {
        DWORD exit_code;
        if (GetExitCodeProcess(child_process_, &exit_code) && exit_code != STILL_ACTIVE) {
            CloseHandle(child_process_);
            child_process_ = nullptr;
            build_state_ = BuildState::Idle;
        }
    }
#else
    if (child_pid_ > 0) {
        int status;
        pid_t result = waitpid(child_pid_, &status, WNOHANG);
        if (result == child_pid_) {
            child_pid_ = 0;
            build_state_ = BuildState::Idle;
        }
    }
#endif
}
