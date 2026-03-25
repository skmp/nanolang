#pragma once
#include "model.h"
#include "types.h"
#include "expr.h"
#include "args.h"
#include "type_utils.h"
#include "node_types.h"
#include "graph_index.h"
#include "symbol_table.h"
#include <set>
#include <functional>

// Standalone type inference for a FlowGraph.
// No editor dependencies — can be unit tested.
struct GraphInference {
    TypePool& pool;
    TypeRegistry registry;
    TypeInferenceContext ctx;
    GraphIndex idx;
    SymbolTable symbol_table;

    GraphInference(TypePool& p) : pool(p), ctx(p, registry) {
        symbol_table.populate_builtins(pool);
    }

    // Run full inference on a graph. Populates resolved_type on all pins.
    // Returns collected errors (also stored on individual nodes).
    std::vector<std::string> run(FlowGraph& graph);

    // --- Individual phases (public for testing) ---

    void clear_all(FlowGraph& graph);
    void resolve_pin_type_names(FlowGraph& graph);
    bool propagate_connections(FlowGraph& graph);
    bool infer_expr_nodes(FlowGraph& graph);
    void propagate_pin_ref_types(FlowNode& node, bool& changed);
    bool resolve_lambdas(FlowGraph& graph);

    // Find the node that owns a given pin ID
    FlowNode* find_node_by_pin(FlowGraph& graph, const std::string& pin_id);

    // Follow bang connections from a pin and collect lambda params from downstream nodes
    void follow_bang_chain(FlowGraph& graph, const std::string& from_pin_id,
                          std::vector<FlowPin*>& params, std::set<std::string>& visited,
                          const std::set<std::string>* caller_scope = nullptr);

    void collect_lambda_params(FlowGraph& graph, FlowNode& node,
                               std::vector<FlowPin*>& params, std::set<std::string>& visited,
                               const std::set<std::string>* caller_scope = nullptr);

    void validate_lambda(FlowNode& node, const std::vector<FlowPin*>& params,
                         const TypePtr& expected, FlowLink& link);

    // Post-inference: insert ExprKind::Deref in expressions where iterator→non-iterator
    void fixup_expr_derefs(FlowGraph& graph);

    // Insert shadow deref nodes where iterators flow into non-iterator pins
    void insert_deref_nodes(FlowGraph& graph);

    // Pre-compute resolved data for codegen (lambda roots, fn types, cast flags)
    void precompute_resolved_data(FlowGraph& graph);
};
