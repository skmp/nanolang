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

        for (auto& p : node.triggers) index_pin(*p);
        for (auto& p : node.inputs) index_pin(*p);
        for (auto& p : node.outputs) index_pin(*p);
        for (auto& p : node.nexts) index_pin(*p);
        index_pin(node.lambda_grab);
        index_pin(node.bang_pin);
    }

    // Index all links and resolve their endpoint pointers
    for (auto& link : graph.links) {
        auto from_it = pin_map.find(link.from_pin);
        auto to_it = pin_map.find(link.to_pin);

        // Resolve pointers directly on the link
        link.from = (from_it != pin_map.end()) ? from_it->second : nullptr;
        link.to = (to_it != pin_map.end()) ? to_it->second : nullptr;

        auto fn_it = pin_to_node.find(link.from_pin);
        auto tn_it = pin_to_node.find(link.to_pin);
        link.from_node = (fn_it != pin_to_node.end()) ? fn_it->second : nullptr;
        link.to_node = (tn_it != pin_to_node.end()) ? tn_it->second : nullptr;

        if (link.from && link.to) {
            source_of[link.to] = link.from;
            if (link.from_node) source_node_of[link.to] = link.from_node;
            if (link.to_node) bang_targets[link.from].push_back(link.to_node);
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
