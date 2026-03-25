#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <random>
#include <memory>
#include "node_types.h"

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
    enum Direction { Input, BangTrigger, Output, BangNext, Lambda, LambdaGrab } direction = Input;
};

using PinPtr = std::unique_ptr<FlowPin>;
using PinVec = std::vector<PinPtr>;

inline PinPtr make_pin(std::string id, std::string name, std::string type_name,
                       TypePtr resolved, FlowPin::Direction dir) {
    return std::make_unique<FlowPin>(FlowPin{std::move(id), std::move(name), std::move(type_name), std::move(resolved), dir});
}

struct FlowNode {
    FlowNode() = default;
    FlowNode(FlowNode&&) = default;
    FlowNode& operator=(FlowNode&&) = default;
    FlowNode(const FlowNode&) = delete;
    FlowNode& operator=(const FlowNode&) = delete;

    int id = 0;                   // internal numeric id (for UI operations)
    std::string guid;             // unique identifier for serialization/connections
    NodeTypeID type_id = NodeTypeID::Unknown;  // node type enum
    std::string args;             // arguments string
    Vec2 position = {0, 0};     // canvas coordinates
    Vec2 size = {120, 60};      // computed during draw
    PinVec triggers;      // bang inputs (top, squares, before data)
    PinVec inputs;        // data inputs AND lambdas (top, in slot order)
    PinVec outputs;       // data outputs (bottom, circles)
    PinVec nexts;         // bang outputs (bottom, squares, before data)
    FlowPin lambda_grab = {"", "as_lambda", "lambda", nullptr, FlowPin::LambdaGrab};
    FlowPin bang_pin = {"", "bang", "bang", nullptr, FlowPin::BangNext};
    std::string error;            // non-empty if node has a validation error
    bool imported = false;        // true if this node was loaded from a attostd import
    bool shadow = false;          // true if this is an internal shadow expr node
    std::string inline_display;   // cached display text (always populated by rebuild_all_inline_display)

    // Parsed expressions — populated at load time, never re-parsed from strings
    std::vector<ExprPtr> parsed_exprs;
    bool type_dirty = true;

    // Pre-computed inline arg metadata — populated at load time
    struct InlineArgMeta {
        int num_inline_args = 0;     // how many tokens fill descriptor inputs
        int ref_pin_count = 0;       // number of $N/@N ref pins
    };
    InlineArgMeta inline_meta;

    // Parse expressions from args string and populate parsed_exprs + inline_meta.
    // Call after type_id and args are set.
    void parse_args();

    // Pre-computed by inference — consumed by codegen
    struct ResolvedLambda {
        FlowNode* root = nullptr;              // lambda root node (connected via Lambda pin)
        std::vector<FlowPin*> params;          // unconnected input pins = lambda parameters
    };
    std::vector<ResolvedLambda> resolved_lambdas;  // one per Lambda-direction input pin
    TypePtr resolved_fn_type;                      // for store!/call: fully resolved function type
    bool needs_narrowing_cast = false;             // for new: fields need static_cast

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
        for (auto& p : triggers)    { p->id = pin_id(p->name); p->type_name = "bang"; p->resolved_type = nullptr; }
        for (auto& p : inputs)      { p->id = pin_id(p->name); if (p->type_name.empty()) p->type_name = "value"; p->resolved_type = nullptr; }
        for (auto& p : outputs)     { p->id = pin_id(p->name); if (p->type_name.empty()) p->type_name = "value"; p->resolved_type = nullptr; }
        for (auto& p : nexts)       { p->id = pin_id(p->name); p->type_name = "bang"; p->resolved_type = nullptr; }
        type_dirty = true;
    }

    // Display text for rendering inside the node
    std::string display_text() const {
        return inline_display;
    }

    // Edit text for the inline editor (same as display)
    std::string edit_text() const {
        return inline_display;
    }
};

struct FlowLink {
    int id = 0;
    std::string from_pin; // output pin id string (for serialization)
    std::string to_pin;   // input pin id string (for serialization)
    std::string error;    // non-empty if this link has a type error (set during inference)
    // Resolved pointers — populated by GraphIndex::rebuild(), not serialized
    FlowPin* from = nullptr;
    FlowPin* to = nullptr;
    FlowNode* from_node = nullptr;
    FlowNode* to_node = nullptr;
};

class FlowGraph {
    FlowGraph(const FlowGraph&) = delete;
    FlowGraph& operator=(const FlowGraph&) = delete;
public:
    FlowGraph() = default;
    FlowGraph(FlowGraph&&) = default;
    FlowGraph& operator=(FlowGraph&&) = default;

public:
    std::vector<FlowNode> nodes;
    std::vector<FlowLink> links;

    // Viewport state (saved/loaded from [viewport] section)
    float viewport_x = 0, viewport_y = 0, viewport_zoom = 1.0f;
    bool has_viewport = false; // true if loaded from file

    // Dirty flag — set when graph structure changes, cleared after validation
    bool dirty = true;

    int add_node(const std::string& guid, Vec2 pos, int num_inputs = 1, int num_outputs = 1) {
        FlowNode node;
        node.id = next_id_++;
        node.guid = guid;
        node.position = pos;
        node.lambda_grab = {"", "as_lambda", "lambda", nullptr, FlowPin::LambdaGrab};
        node.bang_pin = {"", "bang", "bang", nullptr, FlowPin::BangNext};
        for (int i = 0; i < num_inputs; i++) {
            node.inputs.push_back(make_pin("", std::to_string(i), "", nullptr, FlowPin::Input));
        }
        for (int i = 0; i < num_outputs; i++) {
            node.outputs.push_back(make_pin("", "out" + std::to_string(i), "", nullptr, FlowPin::Output));
        }
        if (!guid.empty()) node.rebuild_pin_ids();
        nodes.push_back(std::move(node));
        dirty = true;
        return nodes.back().id;
    }

    // Find a pin by its ID across all nodes
    FlowPin* find_pin(const std::string& pin_id) {
        for (auto& node : nodes) {
            if (node.lambda_grab.id == pin_id) return &node.lambda_grab;
            if (node.bang_pin.id == pin_id) return &node.bang_pin;
            for (auto& p : node.triggers) if (p->id == pin_id) return p.get();
            for (auto& p : node.inputs) if (p->id == pin_id) return p.get();
            for (auto& p : node.outputs) if (p->id == pin_id) return p.get();
            for (auto& p : node.nexts) if (p->id == pin_id) return p.get();
        }
        return nullptr;
    }

    int add_link(const std::string& from_pin, const std::string& to_pin) {
        FlowLink link;
        link.id = next_id_++;
        link.from_pin = from_pin;
        link.to_pin = to_pin;
        links.push_back(link);
        dirty = true;
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
            for (auto& pin : node.triggers)     erase_pin(pin->id, false);
            for (auto& pin : node.inputs)       erase_pin(pin->id, false);
            for (auto& pin : node.outputs)      erase_pin(pin->id, true);
            for (auto& pin : node.nexts)        erase_pin(pin->id, true);
            erase_pin(node.lambda_grab.id, true);
            erase_pin(node.bang_pin.id, true);
        }
        std::erase_if(nodes, [&](auto& n) { return n.id == node_id; });
        dirty = true;
    }

    void remove_link(int link_id) {
        std::erase_if(links, [&](auto& l) { return l.id == link_id; });
        dirty = true;
    }

    int next_node_id() { return next_id_++; }

private:
    int next_id_ = 1;
};
