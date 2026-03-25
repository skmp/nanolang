#pragma once
#include "model.h"
#include "graph_index.h"
#include "types.h"
#include "expr.h"
#include "inference.h"
#include <string>
#include <sstream>
#include <vector>
#include <set>
#include <map>
#include <stdexcept>
#include <functional>

// Saved codegen state for lambda scope isolation
struct LambdaScope {
    std::map<std::string, std::string> saved_pin_to_value;
    std::set<std::string> saved_materialized;
    std::set<std::string> saved_emitted_bang_nodes;
};

// Grouped lambda parameter info: all pins that reference the same parameter index
struct LambdaParamInfo {
    int index;                      // parameter index (0, 1, ...)
    std::vector<FlowPin*> pins;     // all unconnected input pins with this index
    std::vector<FlowNode*> nodes;   // owning node for each pin
};

struct CodeGenerator {
    FlowGraph& graph;
    TypePool& pool;
    GraphIndex idx;
    std::string source_name; // e.g. "klavier" from "klavier.atto"

    // Per-event-handler context (set during emit_event_handler)
    std::map<std::string, std::string> pin_to_value; // source_pin_id → C++ expression
    std::set<std::string> materialized; // guids of nodes already emitted as local vars
    std::set<std::string> emitted_bang_nodes_; // guids of bang nodes already emitted
    int temp_counter = 0;
    std::ostringstream* current_out_ = nullptr; // current output stream for on-demand materialization
    int current_indent_ = 0;

    CodeGenerator(FlowGraph& g, TypePool& p, const std::string& name)
        : graph(g), pool(p), source_name(name) {}

    // Generate all output files
    std::string generate_types();
    std::string generate_header();
    std::string generate_impl();
    std::string generate_cmake(const std::string& attoruntime_path,
                               const std::string& attoc_path,
                               const std::string& atto_project_path,
                               const std::string& atto_source_path,
                               const std::string& nanodeps_path);
    std::string generate_vcpkg();

    // Type conversion
    std::string type_to_cpp(const TypePtr& t);
    std::string type_to_cpp_str(const std::string& type_str);

    // Expression codegen with node context for pin resolution
    std::string expr_to_cpp(const ExprPtr& e, FlowNode* context_node);

    // Resolve what value a pin reference ($N) maps to, given the current node
    std::string resolve_pin_value(FlowNode& node, int pin_index);

    // Resolve an inline arg expression (parses + resolves pins)
    std::string resolve_inline_arg(FlowNode& node, int arg_index);

    // Materialize a data-producing node as a local variable, return the var name
    std::string materialize_node(FlowNode& node, std::ostringstream& out, int indent);

    // Emit an event handler function
    void emit_event_handler(FlowNode& event_node, const std::string& event_name,
                           const std::vector<std::pair<std::string, std::string>>& params,
                           std::ostringstream& out);

    // Emit a bang-triggered node and its chain
    void emit_node(FlowNode& node, std::ostringstream& out, int indent);

    // Emit bang output: follow chain + call any () -> void values wired to the pin
    void emit_bang_next(FlowPin& bout, std::ostringstream& out, int indent);

    // Node helpers
    std::vector<FlowNode*> find_nodes(NodeTypeID type_id);
    FlowNode* find_node_by_guid(const std::string& guid);
    FlowNode* find_source_node(const std::string& to_pin_id);
    std::string find_source_pin(const std::string& to_pin_id);
    std::vector<FlowNode*> follow_bang_from(const std::string& from_pin_id);

    // Collect lambda parameter pins (unconnected inputs in data subgraph only)
    void collect_lambda_pins(FlowNode& node, std::vector<FlowPin*>& params,
                            std::set<std::string>& visited);

    // Collect stored lambda parameters grouped by index (data + bang chains)
    void collect_stored_lambda_params(FlowNode& root,
                                      std::vector<LambdaParamInfo>& params,
                                      std::set<std::string>& visited);

    // Lambda scope save/restore
    LambdaScope enter_lambda_scope();
    void exit_lambda_scope(LambdaScope& scope);

    // Emit a stored lambda body (store! with as_lambda source)
    void emit_stored_lambda(FlowNode& store_node, FlowNode& lambda_root,
                           const std::string& target, std::ostringstream& out, int indent);

    std::string indent_str(int level);
    std::string fresh_var(const std::string& prefix = "tmp");
};
