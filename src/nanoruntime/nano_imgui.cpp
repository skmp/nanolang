#include "nanoruntime.h"
#include "imgui.h"
#include <string>
#include <vector>

// ImGui FFI wrappers for nanolang
// These match the signatures declared in nanostd/imgui.nano

// Window management
bool imgui_begin(std::string title) {
    return ImGui::Begin(title.c_str());
}

bool imgui_begin_fullscreen() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    return ImGui::Begin("##fullscreen", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
}

void imgui_end() {
    ImGui::End();
}

bool imgui_begin_child(std::string id, f32 width, f32 height) {
    return ImGui::BeginChild(id.c_str(), ImVec2(width, height));
}

void imgui_end_child() {
    ImGui::EndChild();
}

// Text
void imgui_text(std::string text) {
    ImGui::TextUnformatted(text.c_str());
}

void imgui_text_colored(f32 r, f32 g, f32 b, f32 a, std::string text) {
    ImGui::TextColored(ImVec4(r, g, b, a), "%s", text.c_str());
}

// Buttons & inputs
bool imgui_button(std::string label) {
    return ImGui::Button(label.c_str());
}

bool imgui_small_button(std::string label) {
    return ImGui::SmallButton(label.c_str());
}

bool imgui_checkbox(std::string label, bool& value) {
    return ImGui::Checkbox(label.c_str(), &value);
}

bool imgui_slider_float(std::string label, f32& value, f32 min, f32 max) {
    return ImGui::SliderFloat(label.c_str(), &value, min, max);
}

bool imgui_slider_int(std::string label, s32& value, s32 min, s32 max) {
    return ImGui::SliderInt(label.c_str(), &value, min, max);
}

bool imgui_input_float(std::string label, f32& value) {
    return ImGui::InputFloat(label.c_str(), &value);
}

bool imgui_input_int(std::string label, s32& value) {
    return ImGui::InputInt(label.c_str(), &value);
}

// Layout
void imgui_same_line() {
    ImGui::SameLine();
}

void imgui_separator() {
    ImGui::Separator();
}

void imgui_spacing() {
    ImGui::Spacing();
}

void imgui_indent() {
    ImGui::Indent();
}

void imgui_unindent() {
    ImGui::Unindent();
}

// Tree nodes
bool imgui_tree_node(std::string label) {
    return ImGui::TreeNode(label.c_str());
}

void imgui_tree_pop() {
    ImGui::TreePop();
}

// Combo/List
bool imgui_begin_combo(std::string label, std::string preview) {
    return ImGui::BeginCombo(label.c_str(), preview.c_str());
}

void imgui_end_combo() {
    ImGui::EndCombo();
}

bool imgui_selectable(std::string label, bool selected) {
    return ImGui::Selectable(label.c_str(), selected);
}

// Colors
bool imgui_color_edit3(std::string label, f32& r, f32& g, f32& b) {
    f32 col[3] = {r, g, b};
    bool changed = ImGui::ColorEdit3(label.c_str(), col);
    if (changed) { r = col[0]; g = col[1]; b = col[2]; }
    return changed;
}

bool imgui_color_edit4(std::string label, f32& r, f32& g, f32& b, f32& a) {
    f32 col[4] = {r, g, b, a};
    bool changed = ImGui::ColorEdit4(label.c_str(), col);
    if (changed) { r = col[0]; g = col[1]; b = col[2]; a = col[3]; }
    return changed;
}

// Plotting
void imgui_plot_lines(std::string label, std::vector<f32>& values, s32 offset, s32 count, f32 scale_min, f32 scale_max, std::string overlay) {
    int safe_offset = std::clamp(offset, 0, (int)values.size());
    int safe_count = std::clamp(count, 0, (int)values.size() - safe_offset);
    ImGui::PlotLines(label.c_str(), values.data() + safe_offset, safe_count, 0,
                     overlay.empty() ? nullptr : overlay.c_str(),
                     scale_min, scale_max);
}

void imgui_plot_lines_fill(std::string label, std::vector<f32>& values, s32 offset, s32 count, f32 scale_min, f32 scale_max, std::string overlay) {
    auto avail = ImGui::GetContentRegionAvail();
    int safe_offset = std::clamp(offset, 0, (int)values.size());
    int safe_count = std::clamp(count, 0, (int)values.size() - safe_offset);
    ImGui::PlotLines(label.c_str(), values.data() + safe_offset, safe_count, 0,
                     overlay.empty() ? nullptr : overlay.c_str(),
                     scale_min, scale_max, avail);
}


// Popups
void imgui_open_popup(std::string id) {
    ImGui::OpenPopup(id.c_str());
}

bool imgui_begin_popup(std::string id) {
    return ImGui::BeginPopup(id.c_str());
}

void imgui_end_popup() {
    ImGui::EndPopup();
}

// Tables
bool imgui_begin_table(std::string id, s32 columns) {
    return ImGui::BeginTable(id.c_str(), columns);
}

void imgui_end_table() {
    ImGui::EndTable();
}

void imgui_table_next_row() {
    ImGui::TableNextRow();
}

bool imgui_table_next_column() {
    return ImGui::TableNextColumn();
}

// Misc
bool imgui_is_item_hovered() {
    return ImGui::IsItemHovered();
}

bool imgui_is_item_clicked() {
    return ImGui::IsItemClicked();
}

void imgui_set_tooltip(std::string text) {
    ImGui::SetTooltip("%s", text.c_str());
}

// Input - Keyboard
bool imgui_is_key_down(s32 key) {
    return ImGui::IsKeyDown(static_cast<ImGuiKey>(key));
}

bool imgui_is_key_pressed(s32 key) {
    return ImGui::IsKeyPressed(static_cast<ImGuiKey>(key));
}

bool imgui_is_key_released(s32 key) {
    return ImGui::IsKeyReleased(static_cast<ImGuiKey>(key));
}

// Input - Mouse
bool imgui_is_mouse_down(s32 button) {
    return ImGui::IsMouseDown(button);
}

bool imgui_is_mouse_clicked(s32 button) {
    return ImGui::IsMouseClicked(button);
}

bool imgui_is_mouse_double_clicked(s32 button) {
    return ImGui::IsMouseDoubleClicked(button);
}

f32 imgui_get_mouse_pos_x() {
    return ImGui::GetMousePos().x;
}

f32 imgui_get_mouse_pos_y() {
    return ImGui::GetMousePos().y;
}

f32 imgui_get_mouse_drag_delta_x(s32 button) {
    return ImGui::GetMouseDragDelta(button).x;
}

f32 imgui_get_mouse_drag_delta_y(s32 button) {
    return ImGui::GetMouseDragDelta(button).y;
}

// Window info
f32 imgui_get_window_width() {
    return ImGui::GetWindowWidth();
}

f32 imgui_get_window_height() {
    return ImGui::GetWindowHeight();
}

// Drawing
void imgui_push_style_color(s32 idx, f32 r, f32 g, f32 b, f32 a) {
    ImGui::PushStyleColor(idx, ImVec4(r, g, b, a));
}

void imgui_pop_style_color(s32 count) {
    ImGui::PopStyleColor(count);
}

// Progress bar
void imgui_progress_bar(f32 fraction, std::string overlay) {
    ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0), overlay.empty() ? nullptr : overlay.c_str());
}

// Drag inputs
bool imgui_drag_float(std::string label, f32& value, f32 speed, f32 min, f32 max) {
    return ImGui::DragFloat(label.c_str(), &value, speed, min, max);
}

bool imgui_drag_int(std::string label, s32& value, f32 speed, s32 min, s32 max) {
    return ImGui::DragInt(label.c_str(), &value, speed, min, max);
}

// Key code lookup by name
s32 imgui_key_code(std::string name) {
    // Common key names to ImGuiKey mapping
    if (name == "a" || name == "A") return ImGuiKey_A;
    if (name == "b" || name == "B") return ImGuiKey_B;
    if (name == "c" || name == "C") return ImGuiKey_C;
    if (name == "d" || name == "D") return ImGuiKey_D;
    if (name == "e" || name == "E") return ImGuiKey_E;
    if (name == "f" || name == "F") return ImGuiKey_F;
    if (name == "g" || name == "G") return ImGuiKey_G;
    if (name == "h" || name == "H") return ImGuiKey_H;
    if (name == "i" || name == "I") return ImGuiKey_I;
    if (name == "j" || name == "J") return ImGuiKey_J;
    if (name == "k" || name == "K") return ImGuiKey_K;
    if (name == "l" || name == "L") return ImGuiKey_L;
    if (name == "m" || name == "M") return ImGuiKey_M;
    if (name == "n" || name == "N") return ImGuiKey_N;
    if (name == "o" || name == "O") return ImGuiKey_O;
    if (name == "p" || name == "P") return ImGuiKey_P;
    if (name == "q" || name == "Q") return ImGuiKey_Q;
    if (name == "r" || name == "R") return ImGuiKey_R;
    if (name == "s" || name == "S") return ImGuiKey_S;
    if (name == "t" || name == "T") return ImGuiKey_T;
    if (name == "u" || name == "U") return ImGuiKey_U;
    if (name == "v" || name == "V") return ImGuiKey_V;
    if (name == "w" || name == "W") return ImGuiKey_W;
    if (name == "x" || name == "X") return ImGuiKey_X;
    if (name == "y" || name == "Y") return ImGuiKey_Y;
    if (name == "z" || name == "Z") return ImGuiKey_Z;
    if (name == "0") return ImGuiKey_0;
    if (name == "1") return ImGuiKey_1;
    if (name == "2") return ImGuiKey_2;
    if (name == "3") return ImGuiKey_3;
    if (name == "4") return ImGuiKey_4;
    if (name == "5") return ImGuiKey_5;
    if (name == "6") return ImGuiKey_6;
    if (name == "7") return ImGuiKey_7;
    if (name == "8") return ImGuiKey_8;
    if (name == "9") return ImGuiKey_9;
    if (name == "space") return ImGuiKey_Space;
    if (name == "enter") return ImGuiKey_Enter;
    if (name == "escape") return ImGuiKey_Escape;
    if (name == "tab") return ImGuiKey_Tab;
    if (name == "semicolon" || name == ";") return ImGuiKey_Semicolon;
    if (name == "apostrophe" || name == "'") return ImGuiKey_Apostrophe;
    if (name == "comma" || name == ",") return ImGuiKey_Comma;
    if (name == "period" || name == ".") return ImGuiKey_Period;
    if (name == "slash" || name == "/") return ImGuiKey_Slash;
    return -1;
}

// Scan piano-mapped keys and fire callbacks for pressed/released this frame
static f32 midi_to_freq(u8 note) {
    return 440.0f * std::pow(2.0f, (note - 69) / 12.0f);
}

struct PianoKeyMapping { ImGuiKey key; u8 midi; };
static const PianoKeyMapping piano_keys[] = {
    // Top row: white keys A-K + sharps on W,E,T,Y,U,O,P
    {ImGuiKey_A, 60}, {ImGuiKey_W, 61}, {ImGuiKey_S, 62}, {ImGuiKey_E, 63},
    {ImGuiKey_D, 64}, {ImGuiKey_F, 65}, {ImGuiKey_T, 66}, {ImGuiKey_G, 67},
    {ImGuiKey_Y, 68}, {ImGuiKey_H, 69}, {ImGuiKey_U, 70}, {ImGuiKey_J, 71},
    {ImGuiKey_K, 72}, {ImGuiKey_O, 73}, {ImGuiKey_L, 74}, {ImGuiKey_P, 75},
    {ImGuiKey_Semicolon, 76}, {ImGuiKey_Apostrophe, 77},
    // Bottom row: octave below
    {ImGuiKey_Z, 48}, {ImGuiKey_X, 50}, {ImGuiKey_C, 52}, {ImGuiKey_V, 53},
    {ImGuiKey_B, 55}, {ImGuiKey_N, 57}, {ImGuiKey_M, 59},
    {ImGuiKey_Comma, 60}, {ImGuiKey_Period, 62}, {ImGuiKey_Slash, 64},
};

s32 imgui_scan_piano_keys(
    std::function<void(u8, f32)>& on_down,
    std::function<void(u8)>& on_up
) {
    s32 count = 0;
    for (auto& km : piano_keys) {
        if (ImGui::IsKeyPressed(km.key, false)) {
            on_down(km.midi, midi_to_freq(km.midi));
            count++;
        }
        if (ImGui::IsKeyReleased(km.key)) {
            on_up(km.midi);
            count++;
        }
    }
    return count;
}
