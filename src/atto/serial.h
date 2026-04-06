#pragma once
#include "model.h"
#include <string>
#include <iostream>

// Load a .atto file into a FlowGraph.
// Supports formats: nanoprog@0, nanoprog@1, attoprog@0, attoprog@1, instrument@atto:0
// Legacy formats are auto-migrated to v2 representation on load.
bool load_atto(const std::string& path, FlowGraph& graph);
bool load_atto_string(const std::string& data, FlowGraph& graph);
bool load_atto_stream(std::istream& f, FlowGraph& graph, const std::string& base_path = "");

// Save always writes instrument@atto:0 format.
void save_atto_stream(std::ostream& f, const FlowGraph& graph);
std::string save_atto_string(const FlowGraph& graph);
bool save_atto(const std::string& path, const FlowGraph& graph);

// Editor metadata (viewport) saved to .atto/<filename>.yaml
bool load_atto_meta(const std::string& atto_path, FlowGraph& graph);
bool save_atto_meta(const std::string& atto_path, const FlowGraph& graph);
