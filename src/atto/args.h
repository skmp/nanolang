#pragma once
#include <string>
#include <vector>
#include <variant>
#include <map>
#include <cstdint>
#include <algorithm>

// Parsed argument types
struct ArgPortRef { int index; };                    // $0, $1, ...
struct ArgLambdaRef { int index; };                  // @0, @1, ...
struct ArgVariable { std::string name; };            // $name
struct ArgEnum { std::string enum_name; std::string value_name; }; // #enum.value
struct ArgNumber { double value; bool is_float; };   // 42, 3.14
struct ArgString { std::string value; };             // "hello\"world"
struct ArgExpr { std::string expr; int max_port; int max_lambda; }; // expression with $N and @N refs

using FlowArg = std::variant<ArgPortRef, ArgLambdaRef, ArgVariable, ArgEnum, ArgNumber, ArgString, ArgExpr>;

struct ParsedArgs {
    std::vector<FlowArg> args;

    // Unified slot map: index -> is_lambda flag
    // @0 and $0 share the same index space (you can't have both @0 and $0)
    std::map<int, bool> slots; // index -> true if lambda (@), false if value ($)
    int max_slot = -1;
    bool has_any_args = false; // true if any arguments were parsed at all

    // Total pin count: if $N/@N numeric refs exist, use max(index)+1.
    // If there are arguments but no numeric refs, return 0.
    // Otherwise fall back to the type default.
    int total_pin_count(int type_default) const {
        if (max_slot >= 0)
            return max_slot + 1;
        if (has_any_args)
            return 0;
        return type_default;
    }

    // Check if slot N is a lambda
    bool is_lambda_slot(int n) const {
        auto it = slots.find(n);
        return it != slots.end() && it->second;
    }
};

// Tokenize an argument string into space-separated tokens, respecting:
// - "quoted strings" with \" escapes
// - (parenthesized groups) that escape spaces
// - Nested parens
std::vector<std::string> tokenize_args(const std::string& input, bool implicit_parens = false);

// Check if a token has a top-level ':' (i.e. name:type field, not inside <>).
// Returns true for "x:f32", false for "map<u8, u64>" or "(x:f32)".
bool has_top_level_colon(const std::string& s);

// Classify a decl_type node's args (tokens after the type name).
// Returns: 0 = alias (e.g. "flist vector<f32>"), 1 = function type (e.g. "gen_fn (x:f32) -> f32"),
//          2 = struct with fields (e.g. "osc_def gen:gen_fn stop:stop_fn")
int classify_decl_type(const std::vector<std::string>& tokens);

// Find the highest @N lambda reference in a string
int find_max_lambda_ref(const std::string& s);

// Find the highest $N port reference in a string
int find_max_port_ref(const std::string& s);

// Parse a single token into a FlowArg
FlowArg parse_token(const std::string& tok);

// Parse a full argument string
ParsedArgs parse_args(const std::string& args_str, bool is_expr = false);
