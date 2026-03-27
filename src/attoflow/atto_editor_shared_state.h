#pragma once
#include "atto/graph_builder.h"
#include <set>

struct AttoEditorSharedState {
    std::set<FlowNodeBuilderPtr> selected_nodes;
};
