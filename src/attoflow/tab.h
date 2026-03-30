#pragma once
#include "editor_pane.h"
#include "atto_editor_shared_state.h"
#include "atto/graph_builder.h"
#include <memory>
#include <string>

struct TabState {
    std::shared_ptr<GraphBuilder> gb;
    std::shared_ptr<AttoEditorSharedState> shared;
    std::shared_ptr<IEditorPane> pane;
    std::string file_path;
    std::string tab_name;

    std::string label() const {
        std::string l = tab_name;
        if (pane) l += std::string("[") + pane->type_name() + "]";
        if (gb && gb->is_dirty()) l += "*";
        return l;
    }
};
