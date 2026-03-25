#pragma once
#include "model.h"
#include "types.h"
#include <string>
#include <vector>

struct GraphBuilder {
    FlowGraph graph;
    TypePool pool;

    // Add a node and return reference
    FlowNode& add(const std::string& guid, const std::string& type, const std::string& args,
                  int num_inputs = -1, int num_outputs = -1);

    void link(const std::string& from, const std::string& to);

    FlowNode* find(const std::string& guid);
    FlowPin* find_pin(const std::string& pin_id);

    std::vector<std::string> run_inference();
    std::vector<std::string> run_full_pipeline();
};
