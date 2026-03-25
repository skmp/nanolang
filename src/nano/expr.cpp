#include "expr.h"
#include "symbol_table.h"

// --- ExprTokenizer::next() ---

ExprToken ExprTokenizer::next() {
    skip_ws();
    if (eof()) return {ExprTokenKind::Eof, "", 0, 0.0};

    char c = peek();

    // String literal
    if (c == '"') {
        advance();
        std::string val;
        while (!eof() && peek() != '"') {
            if (peek() == '\\') { advance(); if (!eof()) val += advance(); }
            else val += advance();
        }
        if (!eof()) advance(); // closing "
        ExprToken t; t.kind = ExprTokenKind::String; t.text = val; return t;
    }

    // & is always the reference operator now (no longer a pin sigil)
    if (c == '&') {
        advance();
        return {ExprTokenKind::Ampersand, "&"};
    }

    // Pin reference: $N (digits only), $name now errors
    if (is_sigil(c)) {
        char sigil = advance();
        if (!eof() && std::isdigit(peek())) {
            // Numeric pin ref
            int n = 0;
            size_t start = pos;
            while (!eof() && std::isdigit(peek())) n = n * 10 + (advance() - '0');
            std::string name;
            if (!eof() && peek() == ':') {
                advance(); // skip :
                while (!eof() && (std::isalnum(peek()) || peek() == '_')) name += advance();
            }
            ExprToken t;
            t.kind = ExprTokenKind::PinRef;
            t.pin_ref = {sigil, n, name};
            t.text = std::string(1, sigil) + std::to_string(n);
            if (!name.empty()) t.text += ":" + name;
            return t;
        }
        if (sigil == '$' && !eof() && (std::isalpha(peek()) || peek() == '_')) {
            // $name is no longer valid — only $N (numeric pin refs) are allowed.
            // Variables are accessed as bare symbols.
            std::string name;
            while (!eof() && (std::isalnum(peek()) || peek() == '_')) name += advance();
            error = "$" + name + " is invalid — use bare name '" + name + "' instead of '$" + name + "'";
            return {ExprTokenKind::Error, "$" + name};
        }
        // Standalone sigil — error
        error = std::string("Unexpected sigil '") + sigil + "' at position " + std::to_string(pos - 1);
        return {ExprTokenKind::Error, std::string(1, sigil)};
    }

    // Number
    if (std::isdigit(c)) {
        size_t start = pos;
        bool has_dot = false;
        while (!eof() && (std::isdigit(peek()) || peek() == '.')) {
            if (peek() == '.') {
                if (has_dot) break;
                has_dot = true;
            }
            advance();
        }
        bool is_f32 = false;
        if (!eof() && (peek() == 'f' || peek() == 'F')) {
            is_f32 = true;
            advance();
        }
        std::string num_str = src.substr(start, pos - start);
        if (is_f32 && num_str.back() == 'f') num_str.pop_back();
        ExprToken t;
        if (has_dot || is_f32) {
            t.kind = ExprTokenKind::Float;
            t.float_val = std::stod(num_str);
            t.is_f32 = is_f32;
        } else {
            t.kind = ExprTokenKind::Int;
            t.int_val = std::stoll(num_str);
        }
        t.text = src.substr(start, pos - start);
        return t;
    }

    // Identifier or keyword
    if (std::isalpha(c) || c == '_') {
        size_t start = pos;
        while (!eof() && (std::isalnum(peek()) || peek() == '_')) advance();
        ExprToken t;
        t.kind = ExprTokenKind::Ident;
        t.text = src.substr(start, pos - start);
        return t;
    }

    // Braces
    if (c == '{') { advance(); return {ExprTokenKind::LBrace, "{"}; }
    if (c == '}') { advance(); return {ExprTokenKind::RBrace, "}"}; }

    // Two-character operators
    if (c == '=' && peek2() == '=') { advance(); advance(); return {ExprTokenKind::Eq, "=="}; }
    if (c == '!' && peek2() == '=') { advance(); advance(); return {ExprTokenKind::Ne, "!="}; }
    // <=> must be checked before <=
    if (c == '<' && peek2() == '=' && pos + 2 < src.size() && src[pos + 2] == '>') {
        advance(); advance(); advance(); return {ExprTokenKind::Spaceship, "<=>"};
    }
    if (c == '<' && peek2() == '=') { advance(); advance(); return {ExprTokenKind::Le, "<="}; }
    if (c == '>' && peek2() == '=') { advance(); advance(); return {ExprTokenKind::Ge, ">="}; }

    // Single-character operators/punctuation
    advance();
    switch (c) {
    case '+': return {ExprTokenKind::Plus, "+"};
    case '-': return {ExprTokenKind::Minus, "-"};
    case '*': return {ExprTokenKind::Star, "*"};
    case '/': return {ExprTokenKind::Slash, "/"};
    case '<': return {ExprTokenKind::Lt, "<"};
    case '>': return {ExprTokenKind::Gt, ">"};
    case '.': return {ExprTokenKind::Dot, "."};
    case '[': return {ExprTokenKind::LBrack, "["};
    case ']': return {ExprTokenKind::RBrack, "]"};
    case '(': return {ExprTokenKind::LParen, "("};
    case ')': return {ExprTokenKind::RParen, ")"};
    case ':':
        if (!eof() && peek() == ':') { advance(); return {ExprTokenKind::ColonColon, "::"}; }
        return {ExprTokenKind::Colon, ":"};
    case '?': return {ExprTokenKind::Question, "?"};
    case ',': return {ExprTokenKind::Comma, ","};
    default:
        error = std::string("Unexpected character '") + c + "' at position " + std::to_string(pos - 1);
        return {ExprTokenKind::Error, std::string(1, c)};
    }
}

// --- ExprParser methods ---

ExprParser::ExprParser(const std::string& src) : tokenizer(src) {
    advance(); // prime the lookahead
}

void ExprParser::advance() {
    current = tokenizer.next();
    if (current.kind == ExprTokenKind::Error && error.empty())
        error = tokenizer.error;
}

bool ExprParser::check(ExprTokenKind k) const { return current.kind == k; }

bool ExprParser::expect(ExprTokenKind k) {
    if (current.kind == k) { advance(); return true; }
    if (error.empty())
        error = "Expected " + std::to_string((int)k) + " but got '" + current.text + "'";
    return false;
}

ExprPtr ExprParser::parse() {
    ExprPtr result;
    // Handle top-level & (reference operator)
    if (check(ExprTokenKind::Ampersand)) {
        advance();
        auto inner = parse_expr();
        auto ref = make_expr(ExprKind::Ref);
        ref->children = {inner};
        result = ref;
    } else {
        result = parse_expr();
    }
    if (!error.empty()) return nullptr;
    if (current.kind != ExprTokenKind::Eof && error.empty())
        error = "Unexpected trailing: '" + current.text + "'";
    return result;
}

ExprPtr ExprParser::parse_expr() { return parse_equality(); }

ExprPtr ExprParser::parse_equality() {
    auto left = parse_comparison();
    while (check(ExprTokenKind::Eq) || check(ExprTokenKind::Ne)) {
        BinOp op = check(ExprTokenKind::Eq) ? BinOp::Eq : BinOp::Ne;
        advance();
        auto right = parse_comparison();
        auto node = make_expr(ExprKind::BinaryOp);
        node->bin_op = op;
        node->children = {left, right};
        left = node;
    }
    return left;
}

ExprPtr ExprParser::parse_comparison() {
    auto left = parse_additive();
    while (check(ExprTokenKind::Lt) || check(ExprTokenKind::Gt) ||
           check(ExprTokenKind::Le) || check(ExprTokenKind::Ge) ||
           check(ExprTokenKind::Spaceship)) {
        BinOp op;
        switch (current.kind) {
        case ExprTokenKind::Lt: op = BinOp::Lt; break;
        case ExprTokenKind::Gt: op = BinOp::Gt; break;
        case ExprTokenKind::Le: op = BinOp::Le; break;
        case ExprTokenKind::Spaceship: op = BinOp::Spaceship; break;
        default: op = BinOp::Ge; break;
        }
        advance();
        auto right = parse_additive();
        auto node = make_expr(ExprKind::BinaryOp);
        node->bin_op = op;
        node->children = {left, right};
        left = node;
    }
    return left;
}

ExprPtr ExprParser::parse_additive() {
    auto left = parse_multiplicative();
    while (check(ExprTokenKind::Plus) || check(ExprTokenKind::Minus)) {
        BinOp op = check(ExprTokenKind::Plus) ? BinOp::Add : BinOp::Sub;
        advance();
        auto right = parse_multiplicative();
        auto node = make_expr(ExprKind::BinaryOp);
        node->bin_op = op;
        node->children = {left, right};
        left = node;
    }
    return left;
}

ExprPtr ExprParser::parse_multiplicative() {
    auto left = parse_unary();
    while (check(ExprTokenKind::Star) || check(ExprTokenKind::Slash)) {
        BinOp op = check(ExprTokenKind::Star) ? BinOp::Mul : BinOp::Div;
        advance();
        auto right = parse_unary();
        auto node = make_expr(ExprKind::BinaryOp);
        node->bin_op = op;
        node->children = {left, right};
        left = node;
    }
    return left;
}

ExprPtr ExprParser::parse_unary() {
    if (check(ExprTokenKind::Minus)) {
        advance();
        auto operand = parse_unary();
        // Fold -literal into a signed literal at parse time
        if (operand && operand->kind == ExprKind::Literal) {
            if (operand->literal_kind == LiteralKind::Unsigned || operand->literal_kind == LiteralKind::Signed) {
                operand->int_value = -operand->int_value;
                operand->literal_kind = LiteralKind::Signed;
                return operand;
            }
            if (operand->literal_kind == LiteralKind::F32) {
                operand->float_value = -operand->float_value;
                return operand;
            }
            if (operand->literal_kind == LiteralKind::F64) {
                operand->float_value = -operand->float_value;
                return operand;
            }
        }
        auto node = make_expr(ExprKind::UnaryMinus);
        node->children = {operand};
        return node;
    }
    return parse_postfix();
}

ExprPtr ExprParser::parse_postfix() {
    auto left = parse_primary();

    while (true) {
        if (check(ExprTokenKind::Dot)) {
            advance();
            if (!check(ExprTokenKind::Ident)) {
                if (error.empty()) error = "Expected field name after '.'";
                return left;
            }
            auto node = make_expr(ExprKind::FieldAccess);
            node->field_name = current.text;
            node->children = {left};
            advance();
            left = node;
        }
        else if (check(ExprTokenKind::Question)) {
            // ?[ query index
            advance(); // consume '?', current is now the next token
            if (!check(ExprTokenKind::LBrack)) break; // not ?[, stop postfix
            advance(); // consume '['
            auto idx = parse_expr();
            if (!expect(ExprTokenKind::RBrack)) return left;
            auto node = make_expr(ExprKind::QueryIndex);
            node->children = {left, idx};
            left = node;
        }
        else if (check(ExprTokenKind::LBrack)) {
            advance();
            auto first = parse_expr();
            if (check(ExprTokenKind::Colon)) {
                // Slice
                advance();
                auto second = parse_expr();
                if (!expect(ExprTokenKind::RBrack)) return left;
                auto node = make_expr(ExprKind::Slice);
                node->children = {left, first, second};
                left = node;
            } else {
                // Index
                if (!expect(ExprTokenKind::RBrack)) return left;
                auto node = make_expr(ExprKind::Index);
                node->children = {left, first};
                left = node;
            }
        }
        else if (check(ExprTokenKind::ColonColon)) {
            // Namespace access: a::b
            advance();
            if (!check(ExprTokenKind::Ident)) {
                if (error.empty()) error = "Expected identifier after '::'";
                return left;
            }
            auto node = make_expr(ExprKind::NamespaceAccess);
            node->field_name = current.text; // reuse field_name for the right-hand name
            node->children = {left};
            advance();
            left = node;
        }
        else if (check(ExprTokenKind::Lt) && left &&
                 (left->kind == ExprKind::PinRef || left->kind == ExprKind::SymbolRef)) {
            // Speculatively try type parameterization: expr<params>
            // Save state for fallback to comparison
            size_t saved_pos = tokenizer.pos;
            auto saved_tok = current;
            std::string saved_error = error;

            advance(); // consume '<'
            std::vector<ExprPtr> params;
            bool ok = true;

            // Parse comma-separated expressions inside <>
            // Each param is parsed via parse_expr() but we stop at ',' or '>'
            while (!check(ExprTokenKind::Gt) && !check(ExprTokenKind::Eof)) {
                auto param = parse_additive(); // use additive to avoid consuming > as comparison
                if (!param || !error.empty()) { ok = false; break; }
                params.push_back(param);
                if (check(ExprTokenKind::Comma)) { advance(); continue; }
                if (!check(ExprTokenKind::Gt)) { ok = false; break; }
            }

            if (ok && check(ExprTokenKind::Gt)) {
                advance(); // consume '>'
                auto node = make_expr(ExprKind::TypeApply);
                node->children.push_back(left);
                for (auto& p : params) node->children.push_back(p);
                left = node;
            } else {
                // Restore — not a type application, fall back to comparison
                tokenizer.pos = saved_pos;
                current = saved_tok;
                error = saved_error;
                break; // exit postfix loop, let comparison handle <
            }
        }
        else if (check(ExprTokenKind::LParen)) {
            // Function/lambda call
            advance();
            auto node = make_expr(ExprKind::FuncCall);
            node->children.push_back(left); // children[0] = callee
            // If callee is a symbol ref, record the name and check for builtin
            if (left->kind == ExprKind::SymbolRef) {
                node->func_name = left->symbol_name;
                node->builtin = lookup_builtin(left->symbol_name);
            }
            if (!check(ExprTokenKind::RParen)) {
                node->children.push_back(parse_expr());
                while (check(ExprTokenKind::Comma)) {
                    advance();
                    node->children.push_back(parse_expr());
                }
            }
            if (!expect(ExprTokenKind::RParen)) return left;
            left = node;
        }
        else {
            break;
        }
    }
    return left;
}

ExprPtr ExprParser::parse_primary() {
    // Number literals → unified Literal kind
    if (check(ExprTokenKind::Int)) {
        auto node = make_expr(ExprKind::Literal);
        node->int_value = current.int_val;
        node->literal_kind = LiteralKind::Unsigned; // all parsed int literals are non-negative; negation comes from UnaryMinus
        advance();
        return node;
    }
    if (check(ExprTokenKind::Float)) {
        auto node = make_expr(ExprKind::Literal);
        node->float_value = current.float_val;
        node->literal_kind = current.is_f32 ? LiteralKind::F32 : LiteralKind::F64;
        advance();
        return node;
    }

    // String literal → unified Literal kind
    if (check(ExprTokenKind::String)) {
        auto node = make_expr(ExprKind::Literal);
        node->string_value = current.text;
        node->literal_kind = LiteralKind::String;
        advance();
        return node;
    }

    // Pin ref
    if (check(ExprTokenKind::PinRef)) {
        auto& pr = current.pin_ref;
        if (pr.index < 0) {
            // $name is no longer valid — use bare names instead
            if (error.empty())
                error = "$" + pr.name + " is not valid; use bare name '" + pr.name + "' instead";
            advance();
            return make_expr(ExprKind::Literal); // dummy
        }
        // Numeric pin ref
        auto node = make_expr(ExprKind::PinRef);
        node->pin_ref = pr;
        slot_info.add(pr.index, pr.sigil, pr.name);
        advance();
        return node;
    }

    // Identifier: check for type expression (name<...>) or symbol reference
    if (check(ExprTokenKind::Ident)) {
        std::string name = current.text;

        // 'literal' keyword must be followed by <T,V>
        if (name == "literal") {
            size_t saved_pos = tokenizer.pos;
            advance(); // consume 'literal'
            if (!check(ExprTokenKind::Lt)) {
                if (error.empty()) error = "'literal' must be followed by '<type,value>'";
                return make_expr(ExprKind::Literal);
            }
            // Reconstruct and parse as type
            std::string type_str = "literal";
            int depth = 0;
            while (!check(ExprTokenKind::Eof)) {
                if (check(ExprTokenKind::Lt)) { depth++; type_str += "<"; advance(); }
                else if (check(ExprTokenKind::Gt)) {
                    type_str += ">";
                    advance();
                    if (--depth <= 0) break;
                }
                else if (check(ExprTokenKind::Comma)) { type_str += ","; advance(); }
                else if (check(ExprTokenKind::String)) { type_str += "\"" + current.text + "\""; advance(); }
                else if (check(ExprTokenKind::Question)) { type_str += "?"; advance(); }
                else if (check(ExprTokenKind::Minus)) { type_str += "-"; advance(); }
                else { type_str += current.text; advance(); }
            }
            std::string parse_err;
            auto parsed_type = parse_type(type_str, parse_err);
            if (!parsed_type || !parse_err.empty()) {
                if (error.empty()) error = "Invalid type expression: " + type_str + (parse_err.empty() ? "" : " (" + parse_err + ")");
                return make_expr(ExprKind::Literal);
            }
            auto node = make_expr(ExprKind::Literal);
            if (parsed_type->kind == TypeKind::String) {
                node->literal_kind = LiteralKind::String;
                if (parsed_type->literal_value.size() >= 2 && parsed_type->literal_value.front() == '"')
                    node->string_value = parsed_type->literal_value.substr(1, parsed_type->literal_value.size() - 2);
            } else if (parsed_type->kind == TypeKind::Bool) {
                node->literal_kind = LiteralKind::Bool;
                node->bool_value = (parsed_type->literal_value == "true");
            } else if (parsed_type->kind == TypeKind::Scalar) {
                if (parsed_type->scalar == ScalarType::F32) {
                    node->literal_kind = LiteralKind::F32;
                    node->float_value = parsed_type->literal_value.empty() ? 0.0 : std::stod(parsed_type->literal_value);
                } else if (parsed_type->scalar == ScalarType::F64) {
                    node->literal_kind = LiteralKind::F64;
                    node->float_value = parsed_type->literal_value.empty() ? 0.0 : std::stod(parsed_type->literal_value);
                } else if (parsed_type->literal_signed) {
                    node->literal_kind = LiteralKind::Signed;
                    node->int_value = parsed_type->literal_value.empty() ? 0 : std::stoll(parsed_type->literal_value);
                } else {
                    node->literal_kind = LiteralKind::Unsigned;
                    node->int_value = parsed_type->literal_value.empty() ? 0 : std::stoll(parsed_type->literal_value);
                }
            }
            return node;
        }
        // true/false are literal booleans, not symbols
        if (name == "true" || name == "false") {
            auto node = make_expr(ExprKind::Literal);
            node->literal_kind = LiteralKind::Bool;
            node->bool_value = (name == "true");
            advance();
            return node;
        }
        // 'symbol' and 'undefined_symbol' are reserved type-system keywords
        if (name == "symbol" || name == "undefined_symbol") {
            if (error.empty()) error = "'" + name + "' is a reserved type keyword and cannot be used as an identifier";
            advance();
            return make_expr(ExprKind::Literal); // dummy
        }
        // Check for identifier<...> — could be a parameterized type expression
        // Save state so we can fall back if type parse fails
        {
            size_t saved_pos = tokenizer.pos;
            auto saved_tok = current;
            std::string saved_error = error;
            advance(); // consume identifier

            bool is_type_keyword = (name == "type");

            if (check(ExprTokenKind::Lt)) {
                // Speculatively parse as type: name<params>
                std::string type_str = name;
                int depth = 0;
                while (!check(ExprTokenKind::Eof)) {
                    if (check(ExprTokenKind::Lt)) { depth++; type_str += "<"; advance(); }
                    else if (check(ExprTokenKind::Gt)) {
                        type_str += ">";
                        advance();
                        if (--depth <= 0) break;
                    }
                    else if (check(ExprTokenKind::Comma)) { type_str += ","; advance(); }
                    else if (check(ExprTokenKind::String)) { type_str += "\"" + current.text + "\""; advance(); }
                    else if (check(ExprTokenKind::Question)) { type_str += "?"; advance(); }
                    else if (check(ExprTokenKind::Minus)) { type_str += "-"; advance(); }
                    else { type_str += current.text; advance(); }
                }

                std::string parse_err;
                auto parsed_type = parse_type(type_str, parse_err);

                if (parsed_type && parse_err.empty()) {
                    // Successfully parsed as parameterized type
                    auto node = make_expr(ExprKind::SymbolRef);
                    node->symbol_name = type_str;
                    node->var_name = type_str;
                    return node;
                } else if (is_type_keyword) {
                    // 'type<' must parse successfully
                    if (error.empty()) error = "Invalid type expression: " + type_str;
                    return make_expr(ExprKind::Literal); // dummy
                } else {
                    // Type parse failed — restore state, treat as plain identifier + comparison
                    tokenizer.pos = saved_pos;
                    current = saved_tok;
                    error = saved_error;
                    advance(); // re-consume identifier
                }
            }

            // Plain SymbolRef (no <, or type parse failed)
            auto node = make_expr(ExprKind::SymbolRef);
            node->symbol_name = name;
            node->var_name = name;
            return node;
        }
    }

    // Struct literal or struct type: { ... }
    if (check(ExprTokenKind::LBrace)) {
        return parse_struct_expr();
    }

    // Parenthesized expression or function type: (...)->T
    if (check(ExprTokenKind::LParen)) {
        // Check if this is a function type by scanning the raw source for )->
        // saved_pos points right after the '(' in the raw source
        size_t saved_pos = tokenizer.pos;
        auto saved_tok = current;
        std::string saved_error = error;

        // Scan raw source for matching ')' then '->'
        size_t paren_open = saved_pos - 1; // the '(' character position
        int depth = 1;
        size_t scan = saved_pos;
        while (scan < tokenizer.src.size() && depth > 0) {
            if (tokenizer.src[scan] == '(') depth++;
            else if (tokenizer.src[scan] == ')') depth--;
            scan++;
        }
        // scan is now past the matching ')'
        // Check for '->' after optional whitespace
        size_t arrow = scan;
        while (arrow < tokenizer.src.size() && tokenizer.src[arrow] == ' ') arrow++;
        if (arrow + 1 < tokenizer.src.size() &&
            tokenizer.src[arrow] == '-' && tokenizer.src[arrow + 1] == '>') {
            // It's a function type — extract the full type string from raw source
            // Find the end: everything up to end of input or next delimiter
            size_t end = arrow + 2;
            // Skip whitespace after ->
            while (end < tokenizer.src.size() && tokenizer.src[end] == ' ') end++;
            // Collect return type (may include <> for parameterized types)
            int angle_depth = 0;
            while (end < tokenizer.src.size()) {
                char c = tokenizer.src[end];
                if (c == '<') angle_depth++;
                else if (c == '>') { if (angle_depth > 0) angle_depth--; else break; }
                else if (angle_depth == 0 && (c == ' ' || c == ')' || c == ',' || c == '}' || c == ']')) break;
                end++;
            }
            std::string type_str = tokenizer.src.substr(paren_open, end - paren_open);
            std::string parse_err;
            auto parsed_type = parse_type(type_str, parse_err);
            if (parsed_type && parse_err.empty()) {
                // Advance tokenizer past the entire type string
                tokenizer.pos = end;
                advance(); // load next token
                auto node = make_expr(ExprKind::SymbolRef);
                node->symbol_name = type_str;
                node->var_name = type_str;
                return node;
            }
            // Failed to parse as type — fall through to restore
        }

        // Not a function type — parse as grouped expression
        // (tokenizer state is still at saved_pos, current is still saved_tok)

        advance(); // consume '('
        auto inner = parse_expr();
        if (!expect(ExprTokenKind::RParen)) return inner;
        return inner;
    }

    if (error.empty())
        error = "Unexpected token: '" + current.text + "'";
    return make_expr(ExprKind::Literal); // dummy
}

ExprPtr ExprParser::parse_struct_expr() {
    // Called when current token is LBrace
    advance(); // consume '{'

    if (check(ExprTokenKind::RBrace)) {
        if (error.empty()) error = "Empty struct literal/type";
        advance();
        return make_expr(ExprKind::StructLiteral); // dummy
    }

    // Parse first field to determine if this is a literal (commas) or type (spaces)
    // Both start with: ident ':' ...
    // We'll parse all fields, then decide based on whether we see commas
    struct PendingField {
        std::string name;
        ExprPtr value;
    };
    std::vector<PendingField> fields;
    bool has_comma = false;

    while (!check(ExprTokenKind::RBrace) && !check(ExprTokenKind::Eof)) {
        if (!check(ExprTokenKind::Ident)) {
            if (error.empty()) error = "Expected field name in struct";
            return make_expr(ExprKind::StructLiteral);
        }
        std::string field_name = current.text;
        advance();
        if (!expect(ExprTokenKind::Colon)) return make_expr(ExprKind::StructLiteral);

        auto value = parse_expr();
        fields.push_back({field_name, value});

        if (check(ExprTokenKind::Comma)) {
            has_comma = true;
            advance();
        }
    }
    expect(ExprTokenKind::RBrace);

    if (has_comma) {
        // Struct literal: {name:value, name:value, ...}
        auto node = make_expr(ExprKind::StructLiteral);
        for (auto& f : fields) {
            node->struct_field_names.push_back(f.name);
            node->children.push_back(f.value);
        }
        return node;
    } else {
        // Struct type: {name:type name:type ...}
        // The "values" are actually type expressions parsed as expressions
        auto node = make_expr(ExprKind::StructType);
        for (auto& f : fields) {
            node->struct_field_names.push_back(f.name);
            node->children.push_back(f.value);
        }
        return node;
    }
}

BuiltinFunc ExprParser::lookup_builtin(const std::string& name) {
    if (name == "sin") return BuiltinFunc::Sin;
    if (name == "cos") return BuiltinFunc::Cos;
    if (name == "pow") return BuiltinFunc::Pow;
    if (name == "exp") return BuiltinFunc::Exp;
    if (name == "log") return BuiltinFunc::Log;
    if (name == "or")  return BuiltinFunc::Or;
    if (name == "xor") return BuiltinFunc::Xor;
    if (name == "and") return BuiltinFunc::And;
    if (name == "not") return BuiltinFunc::Not;
    if (name == "mod") return BuiltinFunc::Mod;
    if (name == "rand") return BuiltinFunc::Rand;
    return BuiltinFunc::None;
}

// --- Free functions ---

ParsedExpr parse_expression(const std::string& src) {
    ExprParser parser(src);
    auto root = parser.parse();
    return {root, parser.slot_info, parser.var_refs, parser.error};
}

ExprSlotInfo scan_slots(const std::string& s) {
    ExprSlotInfo info;
    info.has_any_args = !s.empty();
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if ((c == '$' || c == '@' || c == '%' || c == '&' || c == '^' || c == '#' || c == '!' || c == '~')
            && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9') {
            int n = 0;
            size_t j = i + 1;
            while (j < s.size() && s[j] >= '0' && s[j] <= '9') {
                n = n * 10 + (s[j] - '0');
                j++;
            }
            info.add(n, c);
        }
    }
    return info;
}

NodeArgs parse_node_expr(const std::string& args_str) {
    NodeArgs result;
    if (args_str.empty()) return result;
    result.has_any_args = true;

    // Always do fast slot scan for reliable pin detection
    result.slots = scan_slots(args_str);

    // Parse the expression into an AST
    auto parsed = parse_expression(args_str);
    result.error = parsed.error;
    result.var_refs = parsed.var_refs;
    if (parsed.root) {
        result.exprs.push_back(parsed.root);
    }
    return result;
}

InlineArgInfo compute_inline_args(const std::string& args, int descriptor_inputs) {
    InlineArgInfo info;
    if (args.empty()) {
        info.remaining_descriptor_inputs = descriptor_inputs;
        info.total_pins = descriptor_inputs;
        return info;
    }

    info.tokens = tokenize_args(args, false);
    info.num_inline_args = (int)info.tokens.size();

    // Validate: too many inline args
    if (info.num_inline_args > descriptor_inputs && descriptor_inputs > 0) {
        info.error = "Too many inline args: got " + std::to_string(info.num_inline_args) +
            ", node accepts " + std::to_string(descriptor_inputs);
    }

    // Scan all tokens for $N/@N refs
    info.pin_slots = scan_slots(args);

    // Validate: no gaps in pin indices (must be contiguous from 0)
    if (info.pin_slots.max_slot >= 0) {
        for (int i = 0; i <= info.pin_slots.max_slot; i++) {
            if (info.pin_slots.slots.find(i) == info.pin_slots.slots.end()) {
                if (info.error.empty())
                    info.error = "Missing pin $" + std::to_string(i);
            }
        }
    }

    // Remaining descriptor inputs: those beyond the number of inline args
    info.remaining_descriptor_inputs = std::max(0, descriptor_inputs - info.num_inline_args);

    // Total pins: $N ref pins + remaining descriptor inputs
    int ref_pins = (info.pin_slots.max_slot >= 0) ? (info.pin_slots.max_slot + 1) : 0;
    info.total_pins = ref_pins + info.remaining_descriptor_inputs;

    return info;
}

void clear_expr_types(const ExprPtr& expr) {
    if (!expr) return;
    expr->resolved_type = nullptr;
    expr->access = ValueAccess::Value;
    for (auto& child : expr->children)
        clear_expr_types(child);
}

bool is_lvalue(const ExprPtr& e) {
    if (!e) return false;
    switch (e->kind) {
    case ExprKind::SymbolRef:   return true;  // bare name or $name (resolves to variable)
    case ExprKind::PinRef:      return true;  // $N
    case ExprKind::FieldAccess: return is_lvalue(e->children[0]); // lvalue.field
    case ExprKind::Index:       return is_lvalue(e->children[0]); // lvalue[expr]
    default: return false;
    }
}

void collect_slots(const ExprPtr& expr, ExprSlotInfo& info) {
    if (!expr) return;
    if (expr->kind == ExprKind::PinRef) {
        info.add(expr->pin_ref.index, expr->pin_ref.sigil, expr->pin_ref.name);
    }
    for (auto& child : expr->children)
        collect_slots(child, info);
}

std::string expr_to_string(const ExprPtr& e) {
    if (!e) return "<null>";
    switch (e->kind) {
    case ExprKind::PinRef: {
        std::string s(1, e->pin_ref.sigil);
        s += std::to_string(e->pin_ref.index);
        if (!e->pin_ref.name.empty()) s += ":" + e->pin_ref.name;
        return s;
    }
    case ExprKind::UnaryMinus: return "-" + expr_to_string(e->children[0]);
    case ExprKind::BinaryOp: {
        static const char* ops[] = {"+","-","*","/","==","!=","<",">","<=",">=","<=>"};
        return "(" + expr_to_string(e->children[0]) + " " + ops[(int)e->bin_op] + " " + expr_to_string(e->children[1]) + ")";
    }
    case ExprKind::FieldAccess: return expr_to_string(e->children[0]) + "." + e->field_name;
    case ExprKind::Index: return expr_to_string(e->children[0]) + "[" + expr_to_string(e->children[1]) + "]";
    case ExprKind::QueryIndex: return expr_to_string(e->children[0]) + "?[" + expr_to_string(e->children[1]) + "]";
    case ExprKind::Slice: return expr_to_string(e->children[0]) + "[" + expr_to_string(e->children[1]) + ":" + expr_to_string(e->children[2]) + "]";
    case ExprKind::FuncCall: {
        std::string s;
        if (!e->func_name.empty()) s = e->func_name;
        else s = expr_to_string(e->children[0]);
        s += "(";
        for (size_t i = 1; i < e->children.size(); i++) {
            if (i > 1) s += ", ";
            s += expr_to_string(e->children[i]);
        }
        s += ")";
        return s;
    }
    case ExprKind::Ref:
        return "&" + expr_to_string(e->children[0]);
    case ExprKind::Deref:
        return "*" + expr_to_string(e->children[0]);
    case ExprKind::Literal: {
        switch (e->literal_kind) {
        case LiteralKind::Unsigned:
        case LiteralKind::Signed:
            return std::to_string(e->int_value);
        case LiteralKind::F32: return std::to_string(e->float_value) + "f";
        case LiteralKind::F64: return std::to_string(e->float_value);
        case LiteralKind::String: return "\"" + e->string_value + "\"";
        case LiteralKind::Bool: return e->bool_value ? "true" : "false";
        }
        return "?literal";
    }
    case ExprKind::SymbolRef:
        return e->symbol_name;
    case ExprKind::StructLiteral: {
        std::string s = "{";
        for (size_t i = 0; i < e->struct_field_names.size(); i++) {
            if (i > 0) s += ", ";
            s += e->struct_field_names[i] + ": " + expr_to_string(e->children[i]);
        }
        s += "}";
        return s;
    }
    case ExprKind::StructType: {
        std::string s = "{";
        for (size_t i = 0; i < e->struct_field_names.size(); i++) {
            if (i > 0) s += " ";
            s += e->struct_field_names[i] + ":" + expr_to_string(e->children[i]);
        }
        s += "}";
        return s;
    }
    case ExprKind::NamespaceAccess:
        return expr_to_string(e->children[0]) + "::" + e->field_name;
    case ExprKind::TypeApply: {
        std::string s = expr_to_string(e->children[0]) + "<";
        for (size_t i = 1; i < e->children.size(); i++) {
            if (i > 1) s += ",";
            s += expr_to_string(e->children[i]);
        }
        s += ">";
        return s;
    }
    }
    return "?";
}

// --- TypeInferenceContext methods ---

void TypeInferenceContext::add_error(const std::string& msg) { errors.push_back(msg); }

TypePtr TypeInferenceContext::resolve_named(const std::string& name) {
    auto it = named_types.find(name);
    if (it != named_types.end()) return it->second;
    auto rit = registry.parsed.find(name);
    if (rit != registry.parsed.end()) return rit->second;
    return pool.intern(name);
}

TypePtr TypeInferenceContext::resolve_type(const TypePtr& t) {
    if (!t) return nullptr;
    // Auto-decay symbols
    if (t->kind == TypeKind::Symbol && t->wrapped_type) return resolve_type(t->wrapped_type);
    if (t->kind == TypeKind::Named) {
        auto resolved = resolve_named(t->named_ref);
        if (resolved && resolved.get() != t.get()) return resolve_type(resolved);
    }
    return t;
}

TypePtr TypeInferenceContext::find_field_type(const TypePtr& obj_type, const std::string& field_name) {
    if (!obj_type) return nullptr;
    // Direct struct
    if (obj_type->kind == TypeKind::Struct) {
        for (auto& f : obj_type->fields)
            if (f.name == field_name) return f.type;
        return nullptr;
    }
    // Named type → resolve to struct
    if (obj_type->kind == TypeKind::Named) {
        auto resolved = resolve_named(obj_type->named_ref);
        if (resolved && resolved.get() != obj_type.get() && resolved->kind != TypeKind::Named)
            return find_field_type(resolved, field_name);
    }
    // Container iterator fields
    if (obj_type->kind == TypeKind::ContainerIterator) {
        bool is_map_iter = (obj_type->iterator == IteratorKind::Map ||
                            obj_type->iterator == IteratorKind::OrderedMap);
        if (is_map_iter) {
            // Map iterators: .key and .value only, no auto-deref
            if (field_name == "key") return obj_type->key_type;
            if (field_name == "value") return obj_type->value_type;
            return nullptr;
        }
        // Non-map iterators: auto-dereference to value type fields
        if (obj_type->value_type) {
            auto val_field = find_field_type(obj_type->value_type, field_name);
            if (val_field) return val_field;
        }
        return nullptr;
    }
    return nullptr;
}

// Strip reference category — references decay to values when used in rvalue context
static TypePtr decay_ref(const TypePtr& t) {
    if (!t || t->category != TypeCategory::Reference) return t;
    auto copy = std::make_shared<TypeExpr>(*t);
    copy->category = TypeCategory::Data;
    return copy;
}

TypePtr TypeInferenceContext::infer(const ExprPtr& expr) {
    if (!expr) return pool.t_unknown;
    // Return cached result only if it's a concrete (non-generic) type
    if (expr->resolved_type && !expr->resolved_type->is_generic) return expr->resolved_type;

    TypePtr result = nullptr;

    switch (expr->kind) {
    case ExprKind::PinRef: {
        auto it = input_pin_types.find(expr->pin_ref.index);
        result = (it != input_pin_types.end()) ? it->second : pool.t_unknown;
        break;
    }

    case ExprKind::SymbolRef:
        result = [&]() -> TypePtr {
            // 1. Declared variables (var_types from decl_var)
            auto vit = var_types.find(expr->symbol_name);
            if (vit != var_types.end()) {
                auto sym = std::make_shared<TypeExpr>();
                sym->kind = TypeKind::Symbol;
                sym->symbol_name = expr->symbol_name;
                sym->wrapped_type = vit->second;
                return sym;
            }
            // 2. Symbol table (builtins + declarations)
            if (symbol_table) {
                auto* e = symbol_table->lookup(expr->symbol_name);
                if (e) {
                    auto sym = std::make_shared<TypeExpr>();
                    sym->kind = TypeKind::Symbol;
                    sym->symbol_name = expr->symbol_name;
                    sym->wrapped_type = e->decay_type;
                    return sym;
                }
            }
            // 3. Try parsing as a type expression (e.g., "(x:f32)->f32", "vector<f32>")
            {
                std::string parse_err;
                auto parsed = parse_type(expr->symbol_name, parse_err);
                if (parsed && parse_err.empty() && parsed->kind != TypeKind::Named) {
                    // It's a valid type — return type<T> directly (not a symbol)
                    auto meta = std::make_shared<TypeExpr>();
                    meta->kind = TypeKind::MetaType;
                    meta->wrapped_type = parsed;
                    return meta;
                }
            }
            // 4. Unknown — return undefined_symbol<name>
            auto sym = std::make_shared<TypeExpr>();
            sym->kind = TypeKind::UndefinedSymbol;
            sym->symbol_name = expr->symbol_name;
            return sym;
        }();
        break;

    case ExprKind::Literal: {
        switch (expr->literal_kind) {
        case LiteralKind::Unsigned: {
            auto t = std::make_shared<TypeExpr>(*pool.t_int_literal);
            t->literal_value = std::to_string(expr->int_value);
            result = t;
            break;
        }
        case LiteralKind::Signed: {
            auto t = std::make_shared<TypeExpr>(*pool.t_int_literal);
            t->literal_value = std::to_string(expr->int_value);
            t->literal_signed = true;
            result = t;
            break;
        }
        case LiteralKind::F32: {
            auto t = std::make_shared<TypeExpr>(*pool.t_f32);
            t->literal_value = std::to_string(expr->float_value) + "f";
            result = t;
            break;
        }
        case LiteralKind::F64: {
            auto t = std::make_shared<TypeExpr>(*pool.t_f64);
            t->literal_value = std::to_string(expr->float_value);
            result = t;
            break;
        }
        case LiteralKind::String: {
            auto t = std::make_shared<TypeExpr>(*pool.t_string);
            t->literal_value = "\"" + expr->string_value + "\"";
            result = t;
            break;
        }
        case LiteralKind::Bool: {
            auto t = std::make_shared<TypeExpr>(*pool.t_bool);
            t->literal_value = expr->bool_value ? "true" : "false";
            result = t;
            break;
        }
        }
        break;
    }

    case ExprKind::StructType: {
        // {name:type name:type ...} → type<{name:type ...}>
        auto struct_type = std::make_shared<TypeExpr>();
        struct_type->kind = TypeKind::Struct;
        for (size_t i = 0; i < expr->struct_field_names.size(); i++) {
            TypePtr field_type = pool.t_unknown;
            if (i < expr->children.size() && expr->children[i]) {
                auto child_type = decay_symbol(infer(expr->children[i]));
                // If child resolves to type<T>, unwrap to get T
                if (child_type && child_type->kind == TypeKind::MetaType && child_type->wrapped_type)
                    field_type = child_type->wrapped_type;
                else if (child_type)
                    field_type = child_type;
            }
            struct_type->fields.push_back({expr->struct_field_names[i], field_type});
        }
        auto meta = std::make_shared<TypeExpr>();
        meta->kind = TypeKind::MetaType;
        meta->wrapped_type = struct_type;
        result = meta;
        break;
    }

    case ExprKind::TypeApply: {
        // expr<params> — type parameterization
        auto base = decay_symbol(infer(expr->children[0]));
        if (!base || base->kind != TypeKind::MetaType || !base->wrapped_type) {
            // Base not resolved to a type yet — defer (don't error, allow re-inference)
            if (base && !base->is_generic && base->kind != TypeKind::Void)
                add_error("Type parameterization requires a type, got " + type_to_string(base));
            result = pool.t_unknown;
            break;
        }
        // Get the base type name from the wrapped type
        auto& inner = base->wrapped_type;
        std::string base_name;
        if (inner->kind == TypeKind::Container) {
            static const char* cnames[] = {"map","ordered_map","set","ordered_set","list","queue","vector"};
            base_name = cnames[(int)inner->container];
        } else if (inner->kind == TypeKind::Array) {
            base_name = "array";
        } else if (inner->kind == TypeKind::Tensor) {
            base_name = "tensor";
        } else {
            base_name = type_to_string(inner);
            auto angle = base_name.find('<');
            if (angle != std::string::npos) base_name = base_name.substr(0, angle);
        }

        // Build parameterized type string: base_name<param1,param2,...>
        std::string type_str = base_name + "<";
        for (size_t i = 1; i < expr->children.size(); i++) {
            if (i > 1) type_str += ",";
            auto param_type = decay_symbol(infer(expr->children[i]));
            if (param_type && param_type->kind == TypeKind::MetaType && param_type->wrapped_type) {
                // Type parameter — use inner type name
                type_str += type_to_string(param_type->wrapped_type);
            } else if (param_type && !param_type->literal_value.empty()) {
                // Literal value — use the value directly (for array dimensions etc.)
                type_str += param_type->literal_value;
            } else if (param_type) {
                type_str += type_to_string(param_type);
            } else {
                type_str += "?";
            }
        }
        type_str += ">";

        // Parse the constructed type string
        auto parameterized = pool.intern(type_str);
        if (parameterized && parameterized->kind != TypeKind::Named) {
            auto meta = std::make_shared<TypeExpr>();
            meta->kind = TypeKind::MetaType;
            meta->wrapped_type = parameterized;
            result = meta;
        } else {
            add_error("Failed to parameterize type: " + type_str);
            result = pool.t_unknown;
        }
        break;
    }

    case ExprKind::StructLiteral:
    case ExprKind::NamespaceAccess:
        // TODO: full inference for these
        result = pool.t_unknown;
        break;

    case ExprKind::UnaryMinus: {
        auto operand = decay_symbol(resolve_type(infer(expr->children[0])));
        if (operand && is_numeric(operand)) {
            result = operand;
        } else if (operand && operand->is_generic) {
            result = operand; // propagate generic
        } else if (operand && is_collection(operand)) {
            // Broadcasting: -collection → collection with negated elements
            auto elem = element_type(operand);
            if (elem && is_numeric(elem)) result = operand;
            else { add_error("Cannot negate non-numeric collection elements"); result = pool.t_unknown; }
        } else {
            add_error("Cannot negate non-numeric type");
            result = pool.t_unknown;
        }
        break;
    }

    case ExprKind::BinaryOp:
        result = infer_binary_op(expr);
        break;

    case ExprKind::FieldAccess: {
        auto obj = decay_symbol(infer(expr->children[0]));
        auto field_type = find_field_type(obj, expr->field_name);
        if (field_type) {
            result = field_type;
        } else {
            if (obj && !obj->is_generic)
                add_error("Unknown field '" + expr->field_name + "' on type " + type_to_string(obj));
            result = pool.t_unknown;
        }
        break;
    }

    case ExprKind::Index: {
        auto obj = decay_symbol(infer(expr->children[0]));
        auto obj_resolved = resolve_type(obj);
        auto idx = decay_symbol(infer(expr->children[1]));
        auto idx_resolved = resolve_type(idx);

        // Get the effective index type (unwrap collection for array manipulation)
        auto effective_idx = idx_resolved;
        bool idx_is_collection = effective_idx && is_collection(effective_idx);
        if (idx_is_collection) effective_idx = element_type(effective_idx);
        if (effective_idx) effective_idx = resolve_type(effective_idx);

        // Helper: validate index is integer type (or collection of integer for array manip)
        auto check_int_index = [&]() {
            if (effective_idx && !effective_idx->is_generic &&
                !(effective_idx->kind == TypeKind::Scalar && is_integer(effective_idx))) {
                add_error("Index must be integer, got " + type_to_string(idx));
            }
        };

        if (obj_resolved && obj_resolved->kind == TypeKind::Container) {
            switch (obj_resolved->container) {
            case ContainerKind::Vector:
                check_int_index();
                result = obj_resolved->value_type ? obj_resolved->value_type : pool.t_unknown;
                if (idx_is_collection && result) result = make_collection_of(idx_resolved, result);
                break;
            case ContainerKind::Map:
            case ContainerKind::OrderedMap:
                // Index must match key type (use effective_idx for array manip)
                if (effective_idx && obj_resolved->key_type && !effective_idx->is_generic &&
                    !obj_resolved->key_type->is_generic &&
                    !types_compatible(effective_idx, obj_resolved->key_type)) {
                    add_error("Map index type " + type_to_string(idx) +
                        " does not match key type " + type_to_string(obj_resolved->key_type));
                }
                result = obj_resolved->value_type ? obj_resolved->value_type : pool.t_unknown;
                if (idx_is_collection && result) result = make_collection_of(idx_resolved, result);
                break;
            default:
                add_error("Cannot index into " + type_to_string(obj) + " (only map, vector, array, tensor support indexing)");
                result = pool.t_unknown;
                break;
            }
        } else if (obj_resolved && (obj_resolved->kind == TypeKind::Array || obj_resolved->kind == TypeKind::Tensor)) {
            check_int_index();
            result = obj_resolved->value_type ? obj_resolved->value_type : pool.t_unknown;
            if (idx_is_collection && result) result = make_collection_of(idx_resolved, result);
        } else if (obj_resolved && obj_resolved->kind == TypeKind::String) {
            check_int_index();
            result = pool.t_u8;
            if (idx_is_collection) result = make_collection_of(idx_resolved, pool.t_u8);
        } else if (obj_resolved && !obj_resolved->is_generic) {
            add_error("Cannot index into " + type_to_string(obj));
            result = pool.t_unknown;
        } else {
            result = pool.t_unknown;
        }
        break;
    }

    case ExprKind::QueryIndex: {
        auto obj = infer(expr->children[0]);
        auto obj_resolved = resolve_type(obj);
        infer(expr->children[1]);
        // ?[] is valid on containers that support key lookup: map, ordered_map, set, ordered_set
        if (obj_resolved && !obj_resolved->is_generic) {
            if (obj_resolved->kind == TypeKind::Container) {
                switch (obj_resolved->container) {
                case ContainerKind::Map:
                case ContainerKind::OrderedMap:
                case ContainerKind::Set:
                case ContainerKind::OrderedSet:
                    break; // valid
                default:
                    add_error("Query index ?[] not supported on " + type_to_string(obj) + " (use on map, ordered_map, set, ordered_set)");
                    break;
                }
            } else if (obj_resolved->kind != TypeKind::Void && !obj_resolved->is_generic) {
                add_error("Query index ?[] not supported on " + type_to_string(obj));
            }
        }
        result = pool.t_bool;
        break;
    }

    case ExprKind::Slice: {
        auto obj = infer(expr->children[0]);
        auto start_t = infer(expr->children[1]);
        auto end_t = infer(expr->children[2]);
        if (start_t && end_t && !start_t->is_generic && !end_t->is_generic) {
            if (!types_compatible(start_t, end_t))
                add_error("Slice start and end must be same type");
        }
        result = obj; // same collection type
        break;
    }

    case ExprKind::FuncCall:
        result = infer_func_call(expr);
        break;

    case ExprKind::Ref:
        result = infer_ref(expr);
        break;

    case ExprKind::Deref:
        // Deref nodes are inserted by inference — just propagate the child's type
        if (!expr->children.empty()) result = infer(expr->children[0]);
        if (result && result->kind == TypeKind::ContainerIterator)
            result = result->value_type;
        break;
    }

    expr->resolved_type = result ? result : pool.t_unknown;

    // Decay references for rvalue expressions — arithmetic, function calls, etc.
    // produce values, not references. Only lvalue-compatible kinds keep references.
    if (expr->resolved_type && expr->resolved_type->category == TypeCategory::Reference) {
        bool is_lvalue_kind = (expr->kind == ExprKind::PinRef ||
                               expr->kind == ExprKind::SymbolRef ||
                               expr->kind == ExprKind::FieldAccess ||
                               expr->kind == ExprKind::Index ||
                               expr->kind == ExprKind::Ref);
        if (!is_lvalue_kind) {
            expr->resolved_type = decay_ref(expr->resolved_type);
        }
    }

    // Set access kind based on resolved type
    if (expr->resolved_type) {
        switch (expr->resolved_type->kind) {
        case TypeKind::ContainerIterator:
            expr->access = ValueAccess::Iterator;
            break;
        case TypeKind::Struct:
        case TypeKind::Named:
            expr->access = (expr->resolved_type->category == TypeCategory::Reference)
                ? ValueAccess::Reference : ValueAccess::Field;
            break;
        default:
            expr->access = ValueAccess::Value;
            break;
        }
    }

    return expr->resolved_type;
}

TypePtr TypeInferenceContext::infer_binary_op(const ExprPtr& expr) {
    auto left_t = decay_symbol(resolve_type(infer(expr->children[0])));
    auto right_t = decay_symbol(resolve_type(infer(expr->children[1])));

    bool is_comparison = (expr->bin_op == BinOp::Eq || expr->bin_op == BinOp::Ne ||
                          expr->bin_op == BinOp::Lt || expr->bin_op == BinOp::Gt ||
                          expr->bin_op == BinOp::Le || expr->bin_op == BinOp::Ge ||
                          expr->bin_op == BinOp::Spaceship);

    // Handle broadcasting: scalar op collection or collection op scalar
    if (left_t && right_t) {
        bool l_col = is_collection(left_t);
        bool r_col = is_collection(right_t);
        if (l_col && !r_col) {
            auto elem = element_type(left_t);
            auto scalar_result = infer_scalar_binop(elem, right_t, is_comparison, expr->bin_op);
            if (is_comparison) {
                auto cmp_type = (expr->bin_op == BinOp::Spaceship) ? pool.t_s32 : pool.t_bool;
                return make_collection_of(left_t, cmp_type);
            }
            return left_t;
        }
        if (!l_col && r_col) {
            auto elem = element_type(right_t);
            auto scalar_result = infer_scalar_binop(left_t, elem, is_comparison, expr->bin_op);
            if (is_comparison) {
                auto cmp_type = (expr->bin_op == BinOp::Spaceship) ? pool.t_s32 : pool.t_bool;
                return make_collection_of(right_t, cmp_type);
            }
            return right_t;
        }
    }

    // Scalar op
    return infer_scalar_binop(left_t, right_t, is_comparison, expr->bin_op);
}

TypePtr TypeInferenceContext::infer_scalar_binop(const TypePtr& left_t, const TypePtr& right_t, bool is_comparison, BinOp op) {
    if (!left_t || !right_t) return pool.t_unknown;

    if (is_comparison) {
        // Both must be compatible, result is bool
        if (!left_t->is_generic && !right_t->is_generic && !types_compatible(left_t, right_t))
            add_error("Cannot compare " + type_to_string(left_t) + " with " + type_to_string(right_t));
        return (op == BinOp::Spaceship) ? pool.t_s32 : pool.t_bool;
    }

    // String concatenation: string + string
    if (op == BinOp::Add) {
        bool l_str = (left_t->kind == TypeKind::String);
        bool r_str = (right_t->kind == TypeKind::String);
        if (l_str && r_str) return pool.t_string;
        if (l_str || r_str) {
            // If the other side is still unknown, defer — it may resolve to string later
            if (left_t == pool.t_unknown || right_t == pool.t_unknown) return pool.t_unknown;
            add_error("Cannot add string and " + type_to_string(l_str ? right_t : left_t) +
                       " (convert to string first)");
            return pool.t_unknown;
        }
    }

    // Arithmetic: +, -, *, /
    // Operations on literals produce runtime values (strip literal annotations)
    if (left_t->is_generic && right_t->is_generic) {
        if (is_float(left_t) || is_float(right_t))
            return strip_literal(left_t);
        return strip_literal(left_t);
    }
    if (left_t->is_generic) return strip_literal(right_t);
    if (right_t->is_generic) return strip_literal(left_t);

    if (left_t.get() == right_t.get()) return strip_literal(left_t);
    if (types_compatible(left_t, right_t)) return strip_literal(left_t);

    add_error("Cannot apply arithmetic between " + type_to_string(left_t) + " and " + type_to_string(right_t));
    return pool.t_unknown;
}

TypePtr TypeInferenceContext::make_collection_of(const TypePtr& collection, const TypePtr& elem_type) {
    if (!collection) return pool.t_unknown;
    auto result = std::make_shared<TypeExpr>(*collection);
    result->value_type = elem_type;
    return result;
}

TypePtr TypeInferenceContext::infer_ref(const ExprPtr& expr) {
    auto& inner = expr->children[0];
    if (!inner) { add_error("& requires an expression"); return pool.t_unknown; }

    // Only valid forms: &$name, &$name[expr]
    // Invalid: &$name.field, &$name[expr].field, &literal, &(expr), etc.

    if (inner->kind == ExprKind::SymbolRef) {
        // &name → reference to variable
        auto var_type = decay_symbol(infer(inner));
        if (var_type && !var_type->is_generic) {
            auto ref_type = std::make_shared<TypeExpr>(*var_type);
            ref_type->category = TypeCategory::Reference;
            return ref_type;
        }
        return pool.t_unknown;
    }

    if (inner->kind == ExprKind::Index) {
        // &$name[expr] → iterator for indexable containers, error for array/tensor
        auto obj = infer(inner->children[0]);
        auto obj_resolved = resolve_type(obj);
        infer(inner->children[1]);

        if (obj_resolved && obj_resolved->kind == TypeKind::Container) {
            switch (obj_resolved->container) {
            case ContainerKind::Vector: {
                auto it = std::make_shared<TypeExpr>();
                it->kind = TypeKind::ContainerIterator;
                it->iterator = IteratorKind::Vector;
                it->value_type = obj_resolved->value_type;
                return it;
            }
            case ContainerKind::Map: {
                auto it = std::make_shared<TypeExpr>();
                it->kind = TypeKind::ContainerIterator;
                it->iterator = IteratorKind::Map;
                it->key_type = obj_resolved->key_type;
                it->value_type = obj_resolved->value_type;
                return it;
            }
            case ContainerKind::OrderedMap: {
                auto it = std::make_shared<TypeExpr>();
                it->kind = TypeKind::ContainerIterator;
                it->iterator = IteratorKind::OrderedMap;
                it->key_type = obj_resolved->key_type;
                it->value_type = obj_resolved->value_type;
                return it;
            }
            default:
                add_error("Cannot create reference/iterator for " + type_to_string(obj));
                return pool.t_unknown;
            }
        }
        if (obj_resolved && (obj_resolved->kind == TypeKind::Array || obj_resolved->kind == TypeKind::Tensor)) {
            add_error("Cannot create reference to array/tensor element");
            return pool.t_unknown;
        }
        if (obj_resolved && !obj_resolved->is_generic) {
            add_error("Cannot create reference to " + type_to_string(obj));
            return pool.t_unknown;
        }
        return pool.t_unknown;
    }

    if (inner->kind == ExprKind::PinRef) {
        // &$N → reference to pin value
        auto pin_type = infer(inner);
        if (pin_type && !pin_type->is_generic) {
            auto ref_type = std::make_shared<TypeExpr>(*pin_type);
            ref_type->category = TypeCategory::Reference;
            return ref_type;
        }
        return pool.t_unknown;
    }

    // &$name.field, &(expr), &literal, etc. → error
    if (inner->kind == ExprKind::FieldAccess) {
        add_error("Cannot create reference to field access (& not allowed on .field)");
    } else {
        add_error("& operator requires a variable or indexed variable");
    }
    return pool.t_unknown;
}

TypePtr TypeInferenceContext::infer_func_call(const ExprPtr& expr) {
    if (expr->builtin != BuiltinFunc::None) {
        return infer_builtin_call(expr);
    }

    // Lambda/function call: children[0] = callee, children[1..] = args
    auto callee_type = decay_symbol(infer(expr->children[0]));
    auto callee_resolved = resolve_type(callee_type);
    // Infer all argument types
    for (size_t i = 1; i < expr->children.size(); i++)
        infer(expr->children[i]);

    if (callee_resolved && callee_resolved->kind == TypeKind::Function) {
        // Validate argument count
        size_t expected_args = callee_resolved->func_args.size();
        size_t actual_args = expr->children.size() - 1; // children[0] is callee
        if (actual_args != expected_args) {
            add_error("Function call expects " + std::to_string(expected_args) +
                " argument(s), got " + std::to_string(actual_args));
        }
        // Validate argument types
        for (size_t i = 0; i < std::min(actual_args, expected_args); i++) {
            auto arg_type = expr->children[i + 1]->resolved_type;
            auto& orig_param_type = callee_resolved->func_args[i].type;
            auto param_type = resolve_type(orig_param_type);
            if (!arg_type || !param_type) continue;

            // Generic numeric literals (int?, float?) can only match numeric/bool types
            // They cannot match Named types, structs, containers, etc.
            if (arg_type->is_generic && arg_type->kind == TypeKind::Scalar) {
                if (param_type->kind != TypeKind::Scalar && param_type->kind != TypeKind::Bool) {
                    add_error("Argument " + std::to_string(i) + " type mismatch: " +
                        type_to_string(arg_type) + " cannot be used as " +
                        type_to_string(orig_param_type));
                    continue;
                }
            }

            // Auto-deref: if arg is an iterator but param expects value/ref, insert Deref
            if (arg_type->kind == TypeKind::ContainerIterator &&
                param_type->kind != TypeKind::ContainerIterator) {
                auto deref = std::make_shared<ExprNode>();
                deref->kind = ExprKind::Deref;
                deref->children.push_back(expr->children[i + 1]);
                deref->resolved_type = arg_type->value_type;
                deref->access = ValueAccess::Value;
                expr->children[i + 1] = deref;
                continue; // deref'd type matches param
            }

            if (!arg_type->is_generic && !param_type->is_generic &&
                !types_compatible(arg_type, param_type)) {
                add_error("Argument " + std::to_string(i) + " type mismatch: " +
                    type_to_string(arg_type) + " vs expected " +
                    type_to_string(orig_param_type));
            }
        }
        return callee_resolved->return_type ? callee_resolved->return_type : pool.t_void;
    }
    if (callee_type && !callee_type->is_generic) {
        add_error("Cannot call non-function type " + type_to_string(callee_type));
    }
    return pool.t_unknown;
}

TypePtr TypeInferenceContext::infer_builtin_call(const ExprPtr& expr) {
    // Infer arg types (decay symbols — builtins operate on values)
    std::vector<TypePtr> arg_types;
    for (size_t i = 1; i < expr->children.size(); i++)
        arg_types.push_back(decay_symbol(infer(expr->children[i])));

    // All builtin results strip literal annotations — operations produce runtime values
    auto result = [&]() -> TypePtr {
    switch (expr->builtin) {
    case BuiltinFunc::Sin: case BuiltinFunc::Cos:
    case BuiltinFunc::Exp: case BuiltinFunc::Log: {
        // 1 arg, must be f32 or f64 (or collection of)
        if (arg_types.empty()) { add_error("Expected 1 argument"); return pool.t_unknown; }
        auto t = arg_types[0];
        if (t && is_collection(t)) {
            auto elem = element_type(t);
            if (elem && is_float(elem)) return t; // collection of same type
            if (elem && elem->is_generic) return t;
            add_error(std::string(expr->func_name) + " requires f32 or f64 elements");
            return pool.t_unknown;
        }
        if (t && is_float(t)) return t;
        if (t && t->is_generic) return t;
        add_error(std::string(expr->func_name) + " requires f32 or f64");
        return pool.t_unknown;
    }

    case BuiltinFunc::Pow: {
        if (arg_types.size() < 2) { add_error("pow requires 2 arguments"); return pool.t_unknown; }
        auto a = arg_types[0], b = arg_types[1];
        if (a && b && is_float(a) && is_float(b)) {
            if (types_compatible(a, b)) return a;
            add_error("pow arguments must be same float type");
        }
        if ((a && a->is_generic) || (b && b->is_generic)) return a ? a : b;
        return pool.t_unknown;
    }

    case BuiltinFunc::Or: case BuiltinFunc::And: case BuiltinFunc::Xor: {
        if (arg_types.size() < 2) { add_error("Expected 2 arguments"); return pool.t_unknown; }
        auto a = arg_types[0], b = arg_types[1];
        // bool, bool -> bool
        if (a && b && a->kind == TypeKind::Bool && b->kind == TypeKind::Bool) return pool.t_bool;
        // integer, integer -> integer
        if (a && b && is_integer(a) && is_integer(b)) {
            if (types_compatible(a, b)) return a;
            add_error("Bitwise args must be same integer type");
        }
        if ((a && a->is_generic) || (b && b->is_generic)) return pool.t_unknown;
        return pool.t_unknown;
    }

    case BuiltinFunc::Not: {
        if (arg_types.empty()) { add_error("Expected 1 argument"); return pool.t_unknown; }
        auto t = arg_types[0];
        if (t && t->kind == TypeKind::Bool) return pool.t_bool;
        if (t && is_integer(t)) return t;
        if (t && t->is_generic) return t;
        return pool.t_unknown;
    }

    case BuiltinFunc::Mod: {
        if (arg_types.size() < 2) { add_error("mod requires 2 arguments"); return pool.t_unknown; }
        auto a = arg_types[0], b = arg_types[1];
        if (a && b && is_numeric(a) && is_numeric(b)) {
            if (types_compatible(a, b)) return a;
            add_error("mod arguments must be same numeric type");
        }
        if ((a && a->is_generic) || (b && b->is_generic)) return pool.t_unknown;
        return pool.t_unknown;
    }

    case BuiltinFunc::Rand: {
        if (arg_types.size() < 2) { add_error("rand requires 2 arguments (min, max)"); return pool.t_unknown; }
        auto a = arg_types[0], b = arg_types[1];
        if (a && b && is_numeric(a) && is_numeric(b)) {
            if (types_compatible(a, b)) return a;
            add_error("rand arguments must be same numeric type");
        }
        if ((a && a->is_generic) || (b && b->is_generic)) return a ? a : b;
        return pool.t_unknown;
    }

    default: return pool.t_unknown;
    }
    }();
    return strip_literal(result);
}

void TypeInferenceContext::resolve_int_literals(const ExprPtr& expr, const TypePtr& expected) {
    if (!expr) return;
    auto is_generic_int = [&](const TypePtr& t) {
        return t && t->is_generic && t->kind == TypeKind::Scalar &&
               t->scalar != ScalarType::F32 && t->scalar != ScalarType::F64;
    };
    bool is_int_lit = (expr->kind == ExprKind::Literal &&
                       (expr->literal_kind == LiteralKind::Unsigned ||
                        expr->literal_kind == LiteralKind::Signed) &&
                       is_generic_int(expr->resolved_type));
    if (is_int_lit) {
        if (expected && !expected->is_generic && is_numeric(expected)) {
            // If target is float, check exact representability
            if (is_float(expected)) {
                int64_t v = expr->int_value;
                if (v < 0) v = -v;
                if (expected->scalar == ScalarType::F32) {
                    if (v <= (1LL << 24)) {
                        expr->resolved_type = expected;
                    } else {
                        add_error("Integer " + std::to_string(expr->int_value) +
                            " cannot be represented exactly in f32");
                    }
                } else {
                    if (v <= (1LL << 53)) {
                        expr->resolved_type = expected;
                    } else {
                        add_error("Integer " + std::to_string(expr->int_value) +
                            " cannot be represented exactly in f64");
                    }
                }
            } else {
                expr->resolved_type = expected;
            }
        }
        // Don't default to s32 here — keep as int? so connections can still resolve it
    }
    // Resolve unresolved float literals (constants like pi, e, tau)
    auto is_generic_float = [&](const TypePtr& t) {
        return t && t->is_generic && t->kind == TypeKind::Scalar &&
               (t->scalar == ScalarType::F32 || t->scalar == ScalarType::F64);
    };
    if (is_generic_float(expr->resolved_type)) {
        if (expected && !expected->is_generic && is_float(expected)) {
            expr->resolved_type = expected;
        }
        // Don't default to f64 here — keep as float? so connections can still resolve it
    }
    // Propagate to children based on context
    switch (expr->kind) {
    case ExprKind::BinaryOp: {
        // For arithmetic, propagate expected to both sides
        bool is_cmp = (expr->bin_op == BinOp::Eq || expr->bin_op == BinOp::Ne ||
                       expr->bin_op == BinOp::Lt || expr->bin_op == BinOp::Gt ||
                       expr->bin_op == BinOp::Le || expr->bin_op == BinOp::Ge);
        if (!is_cmp && expected) {
            resolve_int_literals(expr->children[0], expected);
            resolve_int_literals(expr->children[1], expected);
        } else {
            // For comparisons, use resolved types of each other
            auto lt = expr->children[0]->resolved_type;
            auto rt = expr->children[1]->resolved_type;
            if (lt && !lt->is_generic) resolve_int_literals(expr->children[1], lt);
            else if (rt && !rt->is_generic) resolve_int_literals(expr->children[0], rt);
            else {
                resolve_int_literals(expr->children[0], pool.t_s32);
                resolve_int_literals(expr->children[1], pool.t_s32);
            }
        }
        break;
    }
    case ExprKind::UnaryMinus:
        resolve_int_literals(expr->children[0], expected);
        break;
    case ExprKind::FuncCall:
        // TODO: Generalize this — any builtin whose return type is generic (determined
        // solely by its args) should backpropagate `expected` to its children, so that
        // e.g. pow(2, 3) assigned to f32 resolves both literals as f32. Currently only
        // rand does this. Extending requires checking whether the builtin's inferred
        // return type is generic before propagating, and re-inferring after resolution.
        if (expr->builtin == BuiltinFunc::Rand) {
            for (size_t i = 1; i < expr->children.size(); i++)
                resolve_int_literals(expr->children[i], expected);
            if (expected && !expected->is_generic && is_numeric(expected))
                expr->resolved_type = expected;
        } else {
            for (size_t i = 1; i < expr->children.size(); i++)
                resolve_int_literals(expr->children[i], nullptr);
        }
        break;
    case ExprKind::Index:
        resolve_int_literals(expr->children[1], nullptr);
        break;
    case ExprKind::Slice:
        resolve_int_literals(expr->children[1], nullptr);
        resolve_int_literals(expr->children[2], nullptr);
        break;
    default:
        for (auto& child : expr->children)
            resolve_int_literals(child, nullptr);
        break;
    }
}
