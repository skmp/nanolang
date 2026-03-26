#pragma once
#include "editor_style.h"
#include "atto/graph_builder.h"
#include "atto/node_types2.h"
#include "imgui.h"
#include <string>
#include <memory>

struct VisualPin;
enum class VisualPinKind;
struct AddPinHover;
struct WireInfo;

// Tooltip for a hovered +diamond (add va_arg) pin
void tooltip_add_diamond(const AddPinHover& hover);

// Tooltip for a hovered input pin
void tooltip_input_pin(const VisualPin& pin);

// Tooltip for a hovered output pin (visual_index for fallback name)
void tooltip_output_pin(const VisualPin& pin, int visual_index);

// Tooltip for the side-bang output
void tooltip_side_bang();

// Tooltip for the node body (detailed debug info)
void tooltip_node_body(const FlowNodeBuilderPtr& node);

// Tooltip for a hovered wire/net
void tooltip_wire(const WireInfo& w);
