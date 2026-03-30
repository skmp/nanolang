#pragma once
#include "model.h"
#include "args.h"
#include <string>
#include <vector>

// Parse type fields from a "decl_type" node's args.
// Args format: "type_name field1:type1 field2:type2 ..."
// The first token is the type name (skipped).
// For type-based nodes, we only care about simple "name:type" fields
// (not function signatures or array types).
// Returns list of {field_name, field_type} pairs.
struct TypeField { std::string name; std::string type_name; TypePtr resolved = nullptr; };
std::vector<TypeField> parse_type_fields(const FlowNode& type_node);

// Extract the declaration name from a node — checks parsed_exprs, then shadow
// nodes connected to the "name" pin, then falls back to raw args tokenization.
std::string get_decl_name(const FlowNode& node, const FlowGraph& graph);

// Extract the type string from a declaration node — checks shadow nodes
// connected to the "type" pin, then falls back to raw args tokenization.
std::string get_decl_type_str(const FlowNode& node, const FlowGraph& graph);

// Find a "decl_type" node by its name (first arg token) in the graph
const FlowNode* find_type_node(const FlowGraph& graph, const std::string& type_name);

// Find a "decl_event" node by its name (first arg token) in the graph.
// The event_name may have a ~ prefix (stripped for matching).
const FlowNode* find_event_node(const FlowGraph& graph, const std::string& event_name);

// Parse function arguments from a decl_event's function type.
// decl_event args: "event_name fn_type_name" or "event_name (arg:type ...) -> ret"
// Returns the argument list of the function.
std::vector<TypeField> parse_event_args(const FlowNode& event_decl, const FlowGraph& graph);

// Desired pin description for reconciliation
struct DesiredPinDesc { std::string name; std::string type_name; FlowPin::Direction dir; TypePtr resolved = nullptr; };

// Reconcile a pin vector with a desired pin list, preserving links where pin names match.
void reconcile_pins(PinVec& pins,
                    const std::vector<DesiredPinDesc>& desired,
                    const std::string& node_guid, bool is_output,
                    std::vector<FlowLink>& links);

// Resolve type-based pins for nodes like "new" and "event!" that derive their pins from declarations.
// Preserves existing links where pin names match across updates.
void resolve_type_based_pins(FlowGraph& graph);
