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
