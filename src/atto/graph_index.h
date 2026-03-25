#pragma once
#include "model.h"
#include <unordered_map>
#include <vector>
#include <string>

// O(1) graph lookup index — rebuilt after any structural graph modification.
// All pointers are valid only as long as the graph's node/link vectors are not reallocated.
struct GraphIndex {
    // Pin ID string → FlowPin*
    std::unordered_map<std::string, FlowPin*> pin_map;

    // Pin ID string → owning FlowNode*
    std::unordered_map<std::string, FlowNode*> pin_to_node;

    // Node GUID → FlowNode*
    std::unordered_map<std::string, FlowNode*> guid_to_node;

    // Input pin → source output pin (from link)
    std::unordered_map<FlowPin*, FlowPin*> source_of;

    // Input pin → source node
    std::unordered_map<FlowPin*, FlowNode*> source_node_of;

    // Output/bang pin → list of target nodes
    std::unordered_map<FlowPin*, std::vector<FlowNode*>> bang_targets;

    void rebuild(FlowGraph& graph);
    void clear();

    FlowPin* find_pin(const std::string& id) const;
    FlowNode* find_node_by_guid(const std::string& guid) const;
    FlowNode* find_node_by_pin(const std::string& pin_id) const;
    FlowNode* source_node(FlowPin* to_pin) const;
    FlowPin* source_pin(FlowPin* to_pin) const;
    const std::vector<FlowNode*>& follow_bang(FlowPin* from_pin) const;
};
