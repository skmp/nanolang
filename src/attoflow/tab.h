#pragma once
#include "editor_pane.h"
#include <memory>
#include <string>

struct TabState {
    std::shared_ptr<IEditorPane> pane;

    // Convenience accessors delegating to pane
    bool is_loaded() const { return pane && pane->is_loaded(); }
    bool is_dirty() const { return pane && pane->is_dirty(); }
    const std::string& file_path() const { return pane->file_path(); }
    const std::string& tab_name() const { return pane->tab_name(); }
};
