#pragma once
#include "model.h"
#include <string>

// Load a .atto file into a FlowGraph.
// Format:
//   version = "attoprog@0"
//   [[node]]
//   guid = "42"
//   type = "osc~"
//   args = ["440"]
//   connections = ["42.out0->7.0"]
//   position = [100, 200]

bool load_atto(const std::string& path, FlowGraph& graph);
void save_atto_stream(std::ostream& f, const FlowGraph& graph);
std::string save_atto_string(const FlowGraph& graph);
bool save_atto(const std::string& path, const FlowGraph& graph);
bool load_atto_string(const std::string& data, FlowGraph& graph);
