#include "graph_index.h"

static const std::vector<FlowNode*> empty_vec;

void GraphIndex::clear() {
    pin_map.clear();
    pin_to_node.clear();
    guid_to_node.clear();
    source_of.clear();
    source_node_of.clear();
    bang_targets.clear();
}

void GraphIndex::rebuild(FlowGraph& graph) {
    clear();

    // Index all nodes and their pins
    for (auto& node : graph.nodes) {
        guid_to_node[node.guid] = &node;

        auto index_pin = [&](FlowPin& p) {
            if (!p.id.empty()) {
                pin_map[p.id] = &p;
                pin_to_node[p.id] = &node;
            }
        };

        for (auto& p : node.bang_inputs) index_pin(p);
        for (auto& p : node.inputs) index_pin(p);
        for (auto& p : node.outputs) index_pin(p);
        for (auto& p : node.bang_outputs) index_pin(p);
        index_pin(node.lambda_grab);
        index_pin(node.bang_pin);
    }

    // Index all links
    for (auto& link : graph.links) {
        // Resolve link endpoints to pointers
        auto from_it = pin_map.find(link.from_pin);
        auto to_it = pin_map.find(link.to_pin);
        if (from_it != pin_map.end() && to_it != pin_map.end()) {
            FlowPin* from = from_it->second;
            FlowPin* to = to_it->second;

            source_of[to] = from;

            auto from_node_it = pin_to_node.find(link.from_pin);
            if (from_node_it != pin_to_node.end()) {
                source_node_of[to] = from_node_it->second;
            }

            auto to_node_it = pin_to_node.find(link.to_pin);
            if (to_node_it != pin_to_node.end()) {
                bang_targets[from].push_back(to_node_it->second);
            }
        }
    }
}

FlowPin* GraphIndex::find_pin(const std::string& id) const {
    auto it = pin_map.find(id);
    return it != pin_map.end() ? it->second : nullptr;
}

FlowNode* GraphIndex::find_node_by_guid(const std::string& guid) const {
    auto it = guid_to_node.find(guid);
    return it != guid_to_node.end() ? it->second : nullptr;
}

FlowNode* GraphIndex::find_node_by_pin(const std::string& pin_id) const {
    auto it = pin_to_node.find(pin_id);
    return it != pin_to_node.end() ? it->second : nullptr;
}

FlowNode* GraphIndex::source_node(FlowPin* to_pin) const {
    auto it = source_node_of.find(to_pin);
    return it != source_node_of.end() ? it->second : nullptr;
}

FlowPin* GraphIndex::source_pin(FlowPin* to_pin) const {
    auto it = source_of.find(to_pin);
    return it != source_of.end() ? it->second : nullptr;
}

const std::vector<FlowNode*>& GraphIndex::follow_bang(FlowPin* from_pin) const {
    auto it = bang_targets.find(from_pin);
    return it != bang_targets.end() ? it->second : empty_vec;
}
