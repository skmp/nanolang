#include "args.h"
#include "model.h"
#include "expr.h"
#include "node_types.h"
#include <functional>

std::vector<std::string> tokenize_args(const std::string& input, bool implicit_parens) {
    std::string src = implicit_parens ? ("(" + input + ")") : input;
    std::vector<std::string> tokens;
    std::string current;
    int paren_depth = 0;
    int brace_depth = 0;
    bool in_string = false;
    bool escape = false;

    for (size_t i = 0; i < src.size(); i++) {
        char c = src[i];

        if (escape) {
            current += c;
            escape = false;
            continue;
        }
        if (c == '\\') {
            escape = true;
            current += c;
            continue;
        }
        if (c == '"') {
            in_string = !in_string;
            current += c;
            continue;
        }
        if (in_string) {
            current += c;
            continue;
        }
        if (c == '(') {
            paren_depth++;
            current += c;
            continue;
        }
        if (c == ')') {
            paren_depth--;
            current += c;
            continue;
        }
        if (c == '{') {
            brace_depth++;
            current += c;
            continue;
        }
        if (c == '}') {
            brace_depth--;
            current += c;
            continue;
        }
        if (c == ' ' && paren_depth == 0 && brace_depth == 0) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current += c;
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

bool has_top_level_colon(const std::string& s) {
    int angle_depth = 0;
    int paren_depth = 0;
    for (char c : s) {
        if (c == '<') angle_depth++;
        else if (c == '>') angle_depth--;
        else if (c == '(') paren_depth++;
        else if (c == ')') paren_depth--;
        else if (c == ':' && angle_depth == 0 && paren_depth == 0) return true;
    }
    return false;
}

int classify_decl_type(const std::vector<std::string>& tokens) {
    if (tokens.size() < 2) return 2; // missing definition, will error elsewhere
    // Function type: second token starts with '('
    if (!tokens[1].empty() && tokens[1][0] == '(') return 1;
    // Check if any token after the name has a top-level colon (field definition)
    for (size_t i = 1; i < tokens.size(); i++) {
        if (has_top_level_colon(tokens[i])) return 2; // struct
    }
    // No colons at top level -> alias
    return 0;
}

int find_max_lambda_ref(const std::string& s) {
    int max_lam = -1;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '@' && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9') {
            int n = 0;
            size_t j = i + 1;
            while (j < s.size() && s[j] >= '0' && s[j] <= '9') {
                n = n * 10 + (s[j] - '0');
                j++;
            }
            max_lam = std::max(max_lam, n);
        }
    }
    return max_lam;
}

int find_max_port_ref(const std::string& s) {
    int max_port = -1;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '$' && i + 1 < s.size()) {
            if (s[i + 1] >= '0' && s[i + 1] <= '9') {
                int n = 0;
                size_t j = i + 1;
                while (j < s.size() && s[j] >= '0' && s[j] <= '9') {
                    n = n * 10 + (s[j] - '0');
                    j++;
                }
                max_port = std::max(max_port, n);
            }
        }
    }
    return max_port;
}

FlowArg parse_token(const std::string& tok) {
    if (tok.empty()) return ArgString{""};

    // String literal: "..."
    if (tok.front() == '"' && tok.back() == '"' && tok.size() >= 2) {
        // Unescape
        std::string val;
        for (size_t i = 1; i + 1 < tok.size(); i++) {
            if (tok[i] == '\\' && i + 2 < tok.size()) { val += tok[i + 1]; i++; }
            else val += tok[i];
        }
        return ArgString{val};
    }

    // Port ref: $N
    if (tok.front() == '$' && tok.size() >= 2 && tok[1] >= '0' && tok[1] <= '9') {
        int n = 0;
        for (size_t i = 1; i < tok.size() && tok[i] >= '0' && tok[i] <= '9'; i++)
            n = n * 10 + (tok[i] - '0');
        return ArgPortRef{n};
    }

    // Lambda ref: @N
    if (tok.front() == '@' && tok.size() >= 2 && tok[1] >= '0' && tok[1] <= '9') {
        int n = 0;
        for (size_t i = 1; i < tok.size() && tok[i] >= '0' && tok[i] <= '9'; i++)
            n = n * 10 + (tok[i] - '0');
        return ArgLambdaRef{n};
    }

    // Variable: $name
    if (tok.front() == '$' && tok.size() >= 2) {
        return ArgVariable{tok.substr(1)};
    }

    // Enum: #enum.value
    if (tok.front() == '#') {
        auto dot = tok.find('.');
        if (dot != std::string::npos) {
            return ArgEnum{tok.substr(1, dot - 1), tok.substr(dot + 1)};
        }
        return ArgEnum{tok.substr(1), ""};
    }

    // Expression: contains (), +, -, *, / or $N refs
    bool has_math = false;
    for (char c : tok) {
        if (c == '(' || c == ')' || c == '+' || c == '*' || c == '/') { has_math = true; break; }
        if (c == '-' && &c != &tok[0]) { has_math = true; break; }
        if (c == '$' || c == '@') { has_math = true; break; }
    }
    if (has_math) {
        return ArgExpr{tok, find_max_port_ref(tok), find_max_lambda_ref(tok)};
    }

    // Number
    bool is_float = false;
    bool is_number = true;
    for (size_t i = 0; i < tok.size(); i++) {
        char c = tok[i];
        if (c == '.' && !is_float) { is_float = true; continue; }
        if (c == '-' && i == 0) continue;
        if (c < '0' || c > '9') { is_number = false; break; }
    }
    if (is_number && !tok.empty()) {
        return ArgNumber{std::stod(tok), is_float};
    }

    // Fallback: treat as string
    return ArgString{tok};
}

ParsedArgs parse_args(const std::string& args_str, bool is_expr) {
    ParsedArgs result;
    auto tokens = tokenize_args(args_str, is_expr);

    // Helper to register a slot
    auto register_slot = [&](int index, bool is_lambda) {
        result.slots[index] = is_lambda;
        result.max_slot = std::max(result.max_slot, index);
    };

    // Scan raw string for all $N and @N refs (catches refs inside expressions too)
    for (size_t i = 0; i < args_str.size(); i++) {
        if ((args_str[i] == '$' || args_str[i] == '@') && i + 1 < args_str.size() &&
            args_str[i + 1] >= '0' && args_str[i + 1] <= '9') {
            bool is_lambda = (args_str[i] == '@');
            int n = 0;
            size_t j = i + 1;
            while (j < args_str.size() && args_str[j] >= '0' && args_str[j] <= '9') {
                n = n * 10 + (args_str[j] - '0');
                j++;
            }
            register_slot(n, is_lambda);
        }
    }

    for (auto& tok : tokens) {
        result.args.push_back(parse_token(tok));
    }
    result.has_any_args = !tokens.empty();
    return result;
}

void FlowNode::parse_args() {
    parsed_exprs.clear();
    inline_meta = {};

    if (args.empty()) return;

    auto* nt = find_node_type(type_id);
    bool is_expr = is_any_of(type_id, NodeTypeID::Expr, NodeTypeID::ExprBang);
    bool args_are_type = is_any_of(type_id, NodeTypeID::Cast, NodeTypeID::New);
    bool skip = is_any_of(type_id,
        NodeTypeID::Void, NodeTypeID::New,
        NodeTypeID::Decl, NodeTypeID::EventBang, NodeTypeID::Label);

    // Parse expression tokens
    if (!skip) {
        auto tokens = tokenize_args(args, false);
        for (auto& tok : tokens) {
            auto result = parse_expression(tok);
            parsed_exprs.push_back(result.root); // may be nullptr
        }
    }

    // Note: $N:name annotations are NOT applied to pin.name (which is used for pin IDs).
    // The display name comes from the parsed expression and is resolved at display time.

    // Compute inline metadata for non-expr, non-type-arg nodes
    if (!is_expr && !args_are_type) {
        int di = nt ? nt->inputs : 0;
        auto info = compute_inline_args(args, di);
        inline_meta.num_inline_args = info.num_inline_args;
        inline_meta.ref_pin_count = (info.pin_slots.max_slot >= 0) ? (info.pin_slots.max_slot + 1) : 0;
    }
}

// ─── split_args: split string into singular expressions ───

SplitResult split_args(const std::string& args_str) {
    std::vector<std::string> result;
    std::string current;
    int paren_depth = 0;
    int brace_depth = 0;
    bool in_string = false;
    bool escape = false;

    for (size_t i = 0; i < args_str.size(); i++) {
        char c = args_str[i];

        if (escape) {
            current += c;
            escape = false;
            continue;
        }
        if (c == '\\' && in_string) {
            escape = true;
            current += c;
            continue;
        }
        if (c == '"') {
            in_string = !in_string;
            current += c;
            continue;
        }
        if (in_string) {
            current += c;
            continue;
        }

        if (c == '(') { paren_depth++; current += c; continue; }
        if (c == ')') {
            paren_depth--;
            if (paren_depth < 0)
                return std::string("Mismatched ')' at position " + std::to_string(i));
            current += c;
            continue;
        }
        if (c == '{') { brace_depth++; current += c; continue; }
        if (c == '}') {
            brace_depth--;
            if (brace_depth < 0)
                return std::string("Mismatched '}' at position " + std::to_string(i));
            current += c;
            continue;
        }

        if ((c == ' ' || c == '\t') && paren_depth == 0 && brace_depth == 0) {
            if (!current.empty()) {
                result.push_back(current);
                current.clear();
            }
            continue;
        }

        current += c;
    }

    if (in_string)
        return std::string("Unterminated string literal");
    if (paren_depth > 0)
        return std::string("Unclosed '(' — " + std::to_string(paren_depth) + " level(s) deep");
    if (brace_depth > 0)
        return std::string("Unclosed '{' — " + std::to_string(brace_depth) + " level(s) deep");

    if (!current.empty())
        result.push_back(current);

    return result;
}

// (v2 types and functions moved to graph_builder.h/cpp)
