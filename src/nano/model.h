#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <random>
#include <memory>

// Simple 2D vector (replaces Vec2 dependency)
struct Vec2 { float x = 0, y = 0; };

// Forward declarations from flow_types.h and flow_expr.h
struct TypeExpr;
using TypePtr = std::shared_ptr<TypeExpr>;
struct ExprNode;
using ExprPtr = std::shared_ptr<ExprNode>;

// Generate a random 16-character hex guid
inline std::string generate_guid() {
    static std::mt19937_64 rng(std::random_device{}());
    static const char hex[] = "0123456789abcdef";
    std::string s(16, '0');
    auto val = rng();
    for (int i = 0; i < 16; i++) {
        s[i] = hex[(val >> (i * 4)) & 0xf];
    }
    return s;
}

struct FlowPin {
    std::string id;        // "guid.pin_name" e.g. "42.out0", "44.gen"
    std::string name;      // short name e.g. "out0", "gen"
    std::string type_name; // type string for serialization e.g. "f32", "osc_def", or "value"
    TypePtr resolved_type; // runtime resolved type pointer (filled during inference)
    enum Direction { Input, BangInput, Output, BangOutput, Lambda, LambdaGrab } direction = Input;
};

struct FlowNode {
    int id = 0;                   // internal numeric id (for UI operations)
    std::string guid;             // unique identifier for serialization/connections
    std::string type;             // node type (e.g. "expr", "decl_type", "decl_var")
    std::string args;             // arguments string
    Vec2 position = {0, 0};     // canvas coordinates
    Vec2 size = {120, 60};      // computed during draw
    std::vector<FlowPin> bang_inputs;   // bang inputs (top, squares, before data)
    std::vector<FlowPin> inputs;        // data inputs AND lambdas (top, in slot order)
    std::vector<FlowPin> outputs;       // data outputs (bottom, circles)
    std::vector<FlowPin> bang_outputs;  // bang outputs (bottom, squares, before data)
    FlowPin lambda_grab = {"", "as_lambda", "lambda", nullptr, FlowPin::LambdaGrab};
    FlowPin bang_pin = {"", "bang", "bang", nullptr, FlowPin::BangOutput};
    std::string error;            // non-empty if node has a validation error
    bool imported = false;        // true if this node was loaded from a nanostd import

    // Expression parsing cache
    std::vector<ExprPtr> parsed_exprs;   // cached AST(s) for expr nodes
    std::string last_parsed_args;        // for cache invalidation
    bool type_dirty = true;              // set true when args/connections change

    // Build a pin id from this node's guid and a pin name
    std::string pin_id(const std::string& pin_name) const { return guid + "." + pin_name; }

    // Rebuild all pin IDs from guid (call after guid is set or changed)
    void rebuild_pin_ids() {
        lambda_grab.id = pin_id("as_lambda");
        lambda_grab.type_name = "lambda";
        lambda_grab.resolved_type = nullptr;
        bang_pin.id = pin_id("post_bang");
        bang_pin.type_name = "bang";
        bang_pin.resolved_type = nullptr;
        for (auto& p : bang_inputs)  { p.id = pin_id(p.name); p.type_name = "bang"; p.resolved_type = nullptr; }
        for (auto& p : inputs)      { p.id = pin_id(p.name); if (p.type_name.empty()) p.type_name = "value"; p.resolved_type = nullptr; }
        for (auto& p : outputs)     { p.id = pin_id(p.name); if (p.type_name.empty()) p.type_name = "value"; p.resolved_type = nullptr; }
        for (auto& p : bang_outputs) { p.id = pin_id(p.name); p.type_name = "bang"; p.resolved_type = nullptr; }
        type_dirty = true;
    }

    // Display text for rendering inside the node: "type args"
    std::string display_text() const {
        std::string s = type;
        if (!args.empty()) s += " " + args;
        return s;
    }

    // Edit text for the inline editor (same as display)
    std::string edit_text() const {
        return display_text();
    }
};

struct FlowLink {
    int id = 0;
    std::string from_pin; // output pin id e.g. "42.out0"
    std::string to_pin;   // input pin id e.g. "7.0"
    std::string error;    // non-empty if this link has a type error (set during inference)
};

class FlowGraph {
public:
    std::vector<FlowNode> nodes;
    std::vector<FlowLink> links;

    // Viewport state (saved/loaded from [viewport] section)
    float viewport_x = 0, viewport_y = 0, viewport_zoom = 1.0f;
    bool has_viewport = false; // true if loaded from file

    int add_node(const std::string& guid, Vec2 pos, int num_inputs = 1, int num_outputs = 1) {
        FlowNode node;
        node.id = next_id_++;
        node.guid = guid;
        node.position = pos;
        node.lambda_grab = {"", "as_lambda", "lambda", nullptr, FlowPin::LambdaGrab};
        node.bang_pin = {"", "bang", "bang", nullptr, FlowPin::BangOutput};
        for (int i = 0; i < num_inputs; i++) {
            std::string name = std::to_string(i);
            node.inputs.push_back({"", name, "", nullptr, FlowPin::Input});
        }
        for (int i = 0; i < num_outputs; i++) {
            std::string name = "out" + std::to_string(i);
            node.outputs.push_back({"", name, "", nullptr, FlowPin::Output});
        }
        if (!guid.empty()) node.rebuild_pin_ids();
        nodes.push_back(std::move(node));
        return nodes.back().id;
    }

    // Find a pin by its ID across all nodes
    FlowPin* find_pin(const std::string& pin_id) {
        for (auto& node : nodes) {
            if (node.lambda_grab.id == pin_id) return &node.lambda_grab;
            if (node.bang_pin.id == pin_id) return &node.bang_pin;
            for (auto& p : node.bang_inputs) if (p.id == pin_id) return &p;
            for (auto& p : node.inputs) if (p.id == pin_id) return &p;
            for (auto& p : node.outputs) if (p.id == pin_id) return &p;
            for (auto& p : node.bang_outputs) if (p.id == pin_id) return &p;
        }
        return nullptr;
    }

    int add_link(const std::string& from_pin, const std::string& to_pin) {
        FlowLink link;
        link.id = next_id_++;
        link.from_pin = from_pin;
        link.to_pin = to_pin;
        links.push_back(link);
        return link.id;
    }

    void remove_node(int node_id) {
        for (auto& node : nodes) {
            if (node.id != node_id) continue;
            auto erase_pin = [&](const std::string& pid, bool is_from) {
                if (is_from)
                    std::erase_if(links, [&](auto& l) { return l.from_pin == pid; });
                else
                    std::erase_if(links, [&](auto& l) { return l.to_pin == pid; });
            };
            for (auto& pin : node.bang_inputs)  erase_pin(pin.id, false);
            for (auto& pin : node.inputs)       erase_pin(pin.id, false);
            for (auto& pin : node.outputs)      erase_pin(pin.id, true);
            for (auto& pin : node.bang_outputs)  erase_pin(pin.id, true);
            erase_pin(node.lambda_grab.id, true);
            erase_pin(node.bang_pin.id, true);
        }
        std::erase_if(nodes, [&](auto& n) { return n.id == node_id; });
    }

    void remove_link(int link_id) {
        std::erase_if(links, [&](auto& l) { return l.id == link_id; });
    }

    int next_node_id() { return next_id_++; }

private:
    int next_id_ = 1;
};
