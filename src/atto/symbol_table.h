#pragma once
#include "types.h"
#include <string>
#include <map>

// A single entry in the symbol table
struct SymbolEntry {
    std::string name;
    TypePtr decay_type;       // what the symbol resolves to at use site
    enum Source { Builtin, Declaration } source = Builtin;
};

// Symbol table: maps symbol names to their meanings.
// Populated with builtins at construction, extended by declaration nodes.
struct SymbolTable {
    std::map<std::string, SymbolEntry> entries;

    // Construct with builtins populated from a TypePool
    SymbolTable() = default;
    void populate_builtins(TypePool& pool);

    // Lookup a symbol by name. Returns nullptr if not found.
    SymbolEntry* lookup(const std::string& name);
    const SymbolEntry* lookup(const std::string& name) const;

    // Add a symbol. Overwrites if already present.
    void add(const std::string& name, TypePtr decay_type, SymbolEntry::Source src = SymbolEntry::Declaration);

    // Check if a symbol exists
    bool has(const std::string& name) const;

    // Clear all non-builtin entries (for re-running inference)
    void clear_declarations();
};
