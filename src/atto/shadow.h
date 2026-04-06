#pragma once
#include "model.h"

// Generate shadow expr nodes for inline arguments.
// For each non-expr node with inline args, creates internal expr nodes
// (one per inline arg token) and wires their outputs to the parent node's inputs.
// Shadow nodes have FlowNode::shadow = true and are not serialized or rendered.
//
// Skip list: cast, new, event!, label, decl_* — their args are type names, not expressions.
//
// Must be called after loading and before inference.
// After this pass, all non-declaration nodes have their inputs fully connected
// (no inline args remain — codegen can resolve everything via pin connections).
void generate_shadow_nodes(FlowGraph& graph);

// Remove all shadow nodes from a graph (e.g. before saving)
void remove_shadow_nodes(FlowGraph& graph);

// Returns the first shadow arg index for a given node type.
// Lvalue nodes (store, append, iterate, etc.) keep their first arg inline.
int first_shadow_arg_for(NodeTypeID id);

// Rebuild inline_display for all nodes in the graph.
// For nodes with shadow children: reconstructs from shadow args + lvalue args.
// For nodes without shadow children: "type args" or just "type".
void rebuild_all_inline_display(FlowGraph& graph);

// Update shadow nodes for a single parent node after editing.
// Removes old shadows, sets node.args = new_args, runs shadow generation,
// and updates inline_display. Call after the node's type_id and guid are set.
void update_shadows_for_node(FlowGraph& graph, FlowNode& node, const std::string& new_args);
