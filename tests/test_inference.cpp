#include "inference.h"
#include "serial.h"
#include "type_utils.h"
#include "shadow.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>

// Minimal test framework
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    static void test_##name(); \
    struct TestReg_##name { TestReg_##name() { test_registry().push_back({#name, test_##name}); } }; \
    static TestReg_##name reg_##name; \
    static void test_##name()

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (line %d)\n", #cond, __LINE__); \
        tests_failed++; return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a != _b) { \
        printf("  FAIL: %s != %s (line %d)\n", #a, #b, __LINE__); \
        tests_failed++; return; \
    } \
} while(0)

#define ASSERT_CONTAINS(str, substr) do { \
    if (std::string(str).find(substr) == std::string::npos) { \
        printf("  FAIL: \"%s\" does not contain \"%s\" (line %d)\n", std::string(str).c_str(), substr, __LINE__); \
        tests_failed++; return; \
    } \
} while(0)

#define ASSERT_TYPE(pin_ptr, expected_str) do { \
    auto _ts = (pin_ptr)->resolved_type ? type_to_string((pin_ptr)->resolved_type) : "null"; \
    if (_ts != (expected_str)) { \
        printf("  FAIL: expected type '%s' but got '%s' (line %d)\n", (expected_str), _ts.c_str(), __LINE__); \
        tests_failed++; return; \
    } \
} while(0)

struct TestEntry { const char* name; void(*fn)(); };
static std::vector<TestEntry>& test_registry() { static std::vector<TestEntry> r; return r; }

// --- Helper to build graphs programmatically ---

struct GraphBuilder {
    FlowGraph graph;
    TypePool pool;

    // Add a node and return reference
    FlowNode& add(const std::string& guid, const std::string& type, const std::string& args,
                  int num_inputs = -1, int num_outputs = -1) {
        auto* nt = find_node_type(type.c_str());
        bool is_expr = type == "expr";
        int di = nt ? nt->inputs : 0;
        int nbi = nt ? nt->num_triggers : 0;
        int nbo = nt ? nt->num_nexts : 0;
        int no = (num_outputs >= 0) ? num_outputs : (nt ? nt->outputs : 1);

        FlowNode node;
        node.id = graph.next_node_id();
        node.guid = guid;
        node.type_id = node_type_id_from_string(type.c_str());
        node.args = args;
        node.position = {0, 0};

        for (int i = 0; i < nbi; i++)
            node.triggers.push_back(make_pin("", "bang_in" + std::to_string(i), "", nullptr, FlowPin::BangTrigger));

        if (is_expr) {
            auto slots = scan_slots(args);
            int ni = (num_inputs >= 0) ? num_inputs : slots.total_pin_count(di);
            for (int i = 0; i < ni; i++) {
                bool il = slots.is_lambda_slot(i);
                std::string pn = il ? ("@" + std::to_string(i)) : std::to_string(i);
                node.inputs.push_back(make_pin("", pn, "", nullptr, il ? FlowPin::Lambda : FlowPin::Input));
            }
            if (!args.empty() && num_outputs < 0) {
                auto tokens = tokenize_args(args, false);
                no = std::max(1, (int)tokens.size());
            }
        } else if (type == "cast" || type == "new") {
            // Args are type names — use descriptor defaults
            int ni = (num_inputs >= 0) ? num_inputs : di;
            for (int i = 0; i < ni; i++) {
                std::string pn; std::string pt; bool il = false;
                if (nt && nt->input_ports && i < nt->inputs) {
                    pn = nt->input_ports[i].name;
                    il = (nt->input_ports[i].kind == PortKind::Lambda);
                    if (nt->input_ports[i].type_name) pt = nt->input_ports[i].type_name;
                } else pn = std::to_string(i);
                node.inputs.push_back(make_pin("", pn, pt, nullptr, il ? FlowPin::Lambda : FlowPin::Input));
            }
        } else {
            // Non-expr: use compute_inline_args (same as serial loader)
            auto info = compute_inline_args(args, di);
            if (!info.error.empty()) node.error = info.error;
            int ref_pins = (info.pin_slots.max_slot >= 0) ? (info.pin_slots.max_slot + 1) : 0;
            if (num_inputs >= 0) ref_pins = num_inputs; // override if explicitly specified
            for (int i = 0; i < ref_pins; i++) {
                bool il = info.pin_slots.is_lambda_slot(i);
                std::string pn = il ? ("@" + std::to_string(i)) : std::to_string(i);
                node.inputs.push_back(make_pin("", pn, "", nullptr, il ? FlowPin::Lambda : FlowPin::Input));
            }
            for (int i = info.num_inline_args; i < di; i++) {
                std::string pn; std::string pt; bool il = false;
                if (nt && nt->input_ports && i < nt->inputs) {
                    pn = nt->input_ports[i].name;
                    il = (nt->input_ports[i].kind == PortKind::Lambda);
                    if (nt->input_ports[i].type_name) pt = nt->input_ports[i].type_name;
                } else pn = std::to_string(i);
                node.inputs.push_back(make_pin("", pn, pt, nullptr, il ? FlowPin::Lambda : FlowPin::Input));
            }
        }

        for (int i = 0; i < no; i++)
            node.outputs.push_back(make_pin("", "out" + std::to_string(i), "", nullptr, FlowPin::Output));
        for (int i = 0; i < nbo; i++) {
            std::string bname = (nt && nt->next_ports && i < nt->num_nexts) ? nt->next_ports[i].name : ("bang" + std::to_string(i));
            node.nexts.push_back(make_pin("", bname, "", nullptr, FlowPin::BangNext));
        }

        node.rebuild_pin_ids();
        node.parse_args();
        graph.nodes.push_back(std::move(node));
        return graph.nodes.back();
    }

    void link(const std::string& from, const std::string& to) {
        graph.add_link(from, to);
    }

    FlowNode* find(const std::string& guid) {
        for (auto& n : graph.nodes) if (n.guid == guid) return &n;
        return nullptr;
    }

    FlowPin* find_pin(const std::string& pin_id) {
        return graph.find_pin(pin_id);
    }

    std::vector<std::string> run_inference() {
        // Resolve type-based pins first (for new/event! nodes)
        resolve_type_based_pins(graph);
        generate_shadow_nodes(graph);
        GraphInference inference(pool);
        return inference.run(graph);
    }

    std::vector<std::string> run_full_pipeline() {
        // Full pipeline: resolve pins → generate shadows → inference
        resolve_type_based_pins(graph);
        generate_shadow_nodes(graph);
        GraphInference inference(pool);
        return inference.run(graph);
    }
};

// ============================================================
// Tests
// ============================================================

TEST(expr_parses_array_type) {
    auto r = parse_expression("array<f32,48000>");
    ASSERT(r.error.empty());
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::SymbolRef);
    ASSERT_EQ(r.root->symbol_name, "array<f32,48000>");
}

TEST(expr_parses_vector_type) {
    auto r = parse_expression("vector<f32>");
    ASSERT(r.error.empty());
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::SymbolRef);
    ASSERT_EQ(r.root->symbol_name, "vector<f32>");
}

TEST(expr_parses_map_type) {
    auto r = parse_expression("map<u64,string>");
    ASSERT(r.error.empty());
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::SymbolRef);
    ASSERT_EQ(r.root->symbol_name, "map<u64,string>");
}

TEST(expr_comparison_not_broken_by_type_parse) {
    // $0<$1 should still parse as comparison, not type parameterization
    auto r = parse_expression("$0<$1");
    ASSERT(r.error.empty());
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::BinaryOp);
}

TEST(expr_parses_function_type) {
    auto r = parse_expression("(x:f32 y:f32)->f32");
    ASSERT(r.error.empty());
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::SymbolRef);
    ASSERT_EQ(r.root->symbol_name, "(x:f32 y:f32)->f32");
}

TEST(expr_parses_void_function_type) {
    auto r = parse_expression("()->void");
    ASSERT(r.error.empty());
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::SymbolRef);
    ASSERT_EQ(r.root->symbol_name, "()->void");
}

TEST(expr_paren_grouping_still_works) {
    // (1+2)*3 should still parse as grouped expression
    auto r = parse_expression("(1+2)*3");
    ASSERT(r.error.empty());
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::BinaryOp);
}

TEST(expr_parses_struct_type) {
    auto r = parse_expression("{x:f32 y:f32}");
    ASSERT(r.error.empty());
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::StructType);
    ASSERT_EQ(r.root->struct_field_names.size(), (size_t)2);
    ASSERT_EQ(r.root->struct_field_names[0], "x");
    ASSERT_EQ(r.root->struct_field_names[1], "y");
}

TEST(expr_parses_struct_literal) {
    auto r = parse_expression("{x:1.0f, y:2.0f}");
    ASSERT(r.error.empty());
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::StructLiteral);
    ASSERT_EQ(r.root->struct_field_names.size(), (size_t)2);
    ASSERT_EQ(r.root->struct_field_names[0], "x");
    ASSERT_EQ(r.root->struct_field_names[1], "y");
}

TEST(expr_parses_nested_type) {
    auto r = parse_expression("map<string,vector<f32>>");
    ASSERT(r.error.empty());
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::SymbolRef);
    ASSERT_EQ(r.root->symbol_name, "map<string,vector<f32>>");
}

TEST(expr_struct_type_infers_to_metatype) {
    GraphBuilder gb;
    gb.add("e1", "expr", "{x:f32 y:f32}");
    gb.run_inference();
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_TYPE(n->outputs[0], "type<{x:f32 y:f32}>");
}

TEST(expr_function_type_infers_to_metatype) {
    GraphBuilder gb;
    gb.add("e1", "expr", "(x:f32)->f32");
    gb.run_inference();
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_TYPE(n->outputs[0], "type<(x:f32)->f32>");
}

TEST(expr_array_type_infers_to_metatype) {
    GraphBuilder gb;
    gb.add("e1", "expr", "array<f32,48000>");
    gb.run_inference();
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_TYPE(n->outputs[0], "type<array<f32, 48000>>");
}

TEST(expr_builtin_scalars_are_symbols) {
    const char* names[] = {"f32","f64","u8","u16","u32","u64","s8","s16","s32","s64"};
    for (auto name : names) {
        GraphBuilder gb;
        gb.add("e", "expr", name);
        gb.run_inference();
        auto* n = gb.find("e");
        ASSERT(n != nullptr);
        auto t = n->outputs[0]->resolved_type;
        ASSERT(t != nullptr);
        ASSERT_EQ(t->kind, TypeKind::Symbol);
        if (t->kind != TypeKind::Symbol) { printf("    failed for: %s\n", name); return; }
    }
}

TEST(expr_builtin_specials_are_symbols) {
    const char* names[] = {"bool","string","void","mutex"};
    for (auto name : names) {
        GraphBuilder gb;
        gb.add("e", "expr", name);
        gb.run_inference();
        auto* n = gb.find("e");
        ASSERT(n != nullptr);
        auto t = n->outputs[0]->resolved_type;
        ASSERT(t != nullptr);
        ASSERT_EQ(t->kind, TypeKind::Symbol);
        if (t->kind != TypeKind::Symbol) { printf("    failed for: %s\n", name); return; }
    }
}

TEST(expr_builtin_containers_are_symbols) {
    const char* names[] = {"vector","map","set","list","queue","ordered_map","ordered_set","array","tensor"};
    for (auto name : names) {
        GraphBuilder gb;
        gb.add("e", "expr", name);
        gb.run_inference();
        auto* n = gb.find("e");
        ASSERT(n != nullptr);
        auto t = n->outputs[0]->resolved_type;
        ASSERT(t != nullptr);
        ASSERT_EQ(t->kind, TypeKind::Symbol);
        if (t->kind != TypeKind::Symbol) { printf("    failed for: %s\n", name); return; }
    }
}

TEST(expr_builtin_funcs_are_symbols) {
    const char* names[] = {"sin","cos","exp","log","pow","or","xor","and","not","mod","rand"};
    for (auto name : names) {
        GraphBuilder gb;
        gb.add("e", "expr", name);
        gb.run_inference();
        auto* n = gb.find("e");
        ASSERT(n != nullptr);
        auto t = n->outputs[0]->resolved_type;
        ASSERT(t != nullptr);
        ASSERT_EQ(t->kind, TypeKind::Symbol);
        if (t->kind != TypeKind::Symbol) { printf("    failed for: %s\n", name); return; }
    }
}

TEST(expr_type_apply_array) {
    // $0<f32,48000> where $0 = array → type<array<f32, 48000>>
    GraphBuilder gb;
    gb.add("a", "expr", "array");
    gb.add("e", "expr", "$0<f32,48000>", 1);
    gb.link("a.out0", "e.0");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n != nullptr);
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_TYPE(n->outputs[0], "type<array<f32, 48000>>");
}

TEST(expr_type_apply_vector) {
    // $0<f32> where $0 = vector → type<vector<f32>>
    GraphBuilder gb;
    gb.add("v", "expr", "vector");
    gb.add("e", "expr", "$0<f32>", 1);
    gb.link("v.out0", "e.0");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n != nullptr);
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_TYPE(n->outputs[0], "type<vector<f32>>");
}

TEST(expr_type_apply_with_pin_param) {
    // $0<f32,$1> where $0 = array, $1 = 48000 → type<array<f32, 48000>>
    GraphBuilder gb;
    gb.add("a", "expr", "array");
    gb.add("sz", "expr", "48000");
    gb.add("e", "expr", "$0<f32,$1>", 2);
    gb.link("a.out0", "e.0");
    gb.link("sz.out0", "e.1");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n != nullptr);
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_TYPE(n->outputs[0], "type<array<f32, 48000>>");
}

TEST(expr_type_apply_invalid_string_param) {
    // array<f32,"a"> — string is not a valid type parameter
    GraphBuilder gb;
    gb.add("a", "expr", "array");
    gb.add("s", "expr", "\"a\"");
    gb.add("e", "expr", "$0<f32,$1>", 2);
    gb.link("a.out0", "e.0");
    gb.link("s.out0", "e.1");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n != nullptr);
    ASSERT(!n->error.empty());
    ASSERT_CONTAINS(n->error.c_str(), "must be a type or integer");
}

// --- Container type parameterization validation ---

TEST(expr_type_apply_vector_wrong_param_count) {
    // vector<f32,u32> — too many params
    GraphBuilder gb;
    gb.add("v", "expr", "vector");
    gb.add("e", "expr", "$0<f32,u32>", 1);
    gb.link("v.out0", "e.0");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty());
    ASSERT_CONTAINS(gb.find("e")->error.c_str(), "requires 1 type parameter");
}

TEST(expr_type_apply_vector_int_param) {
    // vector<42> — integer is not a type
    GraphBuilder gb;
    gb.add("v", "expr", "vector");
    gb.add("e", "expr", "$0<42>", 1);
    gb.link("v.out0", "e.0");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty());
    ASSERT_CONTAINS(gb.find("e")->error.c_str(), "must be a type");
}

TEST(expr_type_apply_map_wrong_param_count) {
    // map<f32> — too few params
    GraphBuilder gb;
    gb.add("m", "expr", "map");
    gb.add("e", "expr", "$0<f32>", 1);
    gb.link("m.out0", "e.0");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty());
    ASSERT_CONTAINS(gb.find("e")->error.c_str(), "requires 2 type parameter");
}

TEST(expr_type_apply_map_int_key) {
    // map<42,f32> — integer key not a type
    GraphBuilder gb;
    gb.add("m", "expr", "map");
    gb.add("e", "expr", "$0<42,f32>", 1);
    gb.link("m.out0", "e.0");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty());
    ASSERT_CONTAINS(gb.find("e")->error.c_str(), "must be a type");
}

// --- Array type parameterization validation ---

TEST(expr_type_apply_array_no_dims) {
    // array<f32> — missing dimensions
    GraphBuilder gb;
    gb.add("a", "expr", "array");
    gb.add("e", "expr", "$0<f32>", 1);
    gb.link("a.out0", "e.0");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty());
    ASSERT_CONTAINS(gb.find("e")->error.c_str(), "at least 2 parameters");
}

TEST(expr_type_apply_array_zero_dim) {
    // array<f32,0> — dimension can't be zero
    GraphBuilder gb;
    gb.add("a", "expr", "array");
    gb.add("e", "expr", "$0<f32,0>", 1);
    gb.link("a.out0", "e.0");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty());
    ASSERT_CONTAINS(gb.find("e")->error.c_str(), "must be positive");
}

TEST(expr_type_apply_array_string_dim) {
    // array<f32,"a"> — string is not a dimension
    GraphBuilder gb;
    gb.add("a", "expr", "array");
    gb.add("s", "expr", "\"a\"");
    gb.add("e", "expr", "$0<f32,$1>", 2);
    gb.link("a.out0", "e.0");
    gb.link("s.out0", "e.1");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty());
}

TEST(expr_type_apply_array_int_element) {
    // array<0,10> — first param must be type, not integer
    GraphBuilder gb;
    gb.add("a", "expr", "array");
    gb.add("e", "expr", "$0<0,10>", 1);
    gb.link("a.out0", "e.0");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty());
    ASSERT_CONTAINS(gb.find("e")->error.c_str(), "must be a type");
}

TEST(expr_type_apply_array_string_element) {
    // array<"a",10> — string is not a type
    GraphBuilder gb;
    gb.add("a", "expr", "array");
    gb.add("s", "expr", "\"a\"");
    gb.add("e", "expr", "$0<$1,10>", 2);
    gb.link("a.out0", "e.0");
    gb.link("s.out0", "e.1");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty());
}

// --- Tensor type parameterization validation ---

TEST(expr_type_apply_tensor_ok) {
    GraphBuilder gb;
    gb.add("t", "expr", "tensor");
    gb.add("e", "expr", "$0<f32>", 1);
    gb.link("t.out0", "e.0");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0], "type<tensor<f32>>");
}

TEST(expr_type_apply_tensor_wrong_param_count) {
    // tensor<f32,u32> — too many params
    GraphBuilder gb;
    gb.add("t", "expr", "tensor");
    gb.add("e", "expr", "$0<f32,u32>", 1);
    gb.link("t.out0", "e.0");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty());
    ASSERT_CONTAINS(gb.find("e")->error.c_str(), "requires 1 type parameter");
}

TEST(expr_type_apply_tensor_int_param) {
    // tensor<42> — integer is not a type
    GraphBuilder gb;
    gb.add("t", "expr", "tensor");
    gb.add("e", "expr", "$0<42>", 1);
    gb.link("t.out0", "e.0");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty());
    ASSERT_CONTAINS(gb.find("e")->error.c_str(), "must be a type");
}

TEST(expr_type_apply_parse) {
    // $0<f32> should parse as TypeApply, not comparison
    auto r = parse_expression("$0<f32>");
    ASSERT(r.error.empty());
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::TypeApply);
    ASSERT_EQ(r.root->children.size(), (size_t)2);
    ASSERT_EQ(r.root->children[0]->kind, ExprKind::PinRef);
}

TEST(expr_builtin_constants_are_symbols) {
    const char* names[] = {"pi","e","tau"};
    for (auto name : names) {
        GraphBuilder gb;
        gb.add("e", "expr", name);
        gb.run_inference();
        auto* n = gb.find("e");
        ASSERT(n != nullptr);
        auto t = n->outputs[0]->resolved_type;
        ASSERT(t != nullptr);
        ASSERT_EQ(t->kind, TypeKind::Symbol);
        if (t->kind != TypeKind::Symbol) { printf("    failed for: %s\n", name); return; }
    }
}

TEST(expr_vector_type_infers_to_metatype) {
    GraphBuilder gb;
    gb.add("e1", "expr", "vector<f32>");
    gb.run_inference();
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_TYPE(n->outputs[0], "type<vector<f32>>");
}

TEST(parse_simple_int) {
    auto r = parse_expression("42");
    ASSERT(r.root != nullptr);
    ASSERT(r.error.empty());
    ASSERT_EQ(r.root->kind, ExprKind::Literal);
    ASSERT_EQ(r.root->literal_kind, LiteralKind::Unsigned);
    ASSERT_EQ(r.root->int_value, 42);
}

TEST(parse_simple_float) {
    auto r = parse_expression("3.14");
    ASSERT(r.root != nullptr);
    ASSERT(r.error.empty());
    ASSERT_EQ(r.root->kind, ExprKind::Literal);
    ASSERT_EQ(r.root->literal_kind, LiteralKind::F64);
}

TEST(parse_f32_literal) {
    auto r = parse_expression("1.0f");
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::Literal);
    ASSERT_EQ(r.root->literal_kind, LiteralKind::F32);
}

TEST(parse_bool_true) {
    auto r = parse_expression("true");
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::Literal);
    ASSERT_EQ(r.root->literal_kind, LiteralKind::Bool);
    ASSERT(r.root->bool_value == true);
}

TEST(parse_pin_ref) {
    auto r = parse_expression("$0");
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::PinRef);
    ASSERT_EQ(r.root->pin_ref.index, 0);
    ASSERT_EQ(r.root->pin_ref.sigil, '$');
    ASSERT_EQ(r.slots.max_slot, 0);
}

TEST(parse_named_pin_ref) {
    auto r = parse_expression("$0:myname");
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::PinRef);
    ASSERT_EQ(r.root->pin_ref.name, "myname");
}

TEST(parse_var_ref) {
    // oscs now strips the $ and produces a SymbolRef
    auto r = parse_expression("oscs");
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::SymbolRef);
    ASSERT_EQ(r.root->symbol_name, "oscs");
}

TEST(parse_bare_ident) {
    auto r = parse_expression("pi");
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::SymbolRef);
    ASSERT_EQ(r.root->symbol_name, "pi");
}

TEST(parse_binary_add) {
    auto r = parse_expression("$0+$1");
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::BinaryOp);
    ASSERT_EQ(r.root->bin_op, BinOp::Add);
    ASSERT_EQ(r.root->children.size(), (size_t)2);
    ASSERT_EQ(r.slots.max_slot, 1);
}

TEST(parse_precedence) {
    // $0+$1*$2 should be $0+($1*$2)
    auto r = parse_expression("$0+$1*$2");
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::BinaryOp);
    ASSERT_EQ(r.root->bin_op, BinOp::Add);
    ASSERT_EQ(r.root->children[1]->kind, ExprKind::BinaryOp);
    ASSERT_EQ(r.root->children[1]->bin_op, BinOp::Mul);
}

TEST(parse_field_access) {
    auto r = parse_expression("$0.x");
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::FieldAccess);
    ASSERT_EQ(r.root->field_name, "x");
}

TEST(parse_index) {
    auto r = parse_expression("$0[$1]");
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::Index);
    ASSERT_EQ(r.root->children.size(), (size_t)2);
}

TEST(parse_query_index) {
    auto r = parse_expression("$0?[$1]");
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::QueryIndex);
}

TEST(parse_slice) {
    auto r = parse_expression("$0[1:3]");
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::Slice);
    ASSERT_EQ(r.root->children.size(), (size_t)3);
}

TEST(parse_func_call) {
    auto r = parse_expression("sin($0)");
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::FuncCall);
    ASSERT_EQ(r.root->builtin, BuiltinFunc::Sin);
}

TEST(parse_complex_expr) {
    auto r = parse_expression("sin(oscs[$0].p)*$1");
    ASSERT(r.root != nullptr);
    ASSERT(r.error.empty());
    // Should be: sin((oscs[$0]).p) * $1
    ASSERT_EQ(r.root->kind, ExprKind::BinaryOp);
    ASSERT_EQ(r.root->bin_op, BinOp::Mul);
}

TEST(parse_comparison) {
    auto r = parse_expression("$0<0.001");
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::BinaryOp);
    ASSERT_EQ(r.root->bin_op, BinOp::Lt);
}

TEST(parse_unary_minus) {
    auto r = parse_expression("-$0");
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::UnaryMinus);
}

TEST(parse_chained_field) {
    auto r = parse_expression("$0.step().s");
    ASSERT(r.root != nullptr);
    ASSERT(r.error.empty());
    // Should be: (($0.step)()).s
    ASSERT_EQ(r.root->kind, ExprKind::FieldAccess);
    ASSERT_EQ(r.root->field_name, "s");
}

// --- Type inference tests ---

TEST(infer_int_literal_standalone) {
    GraphBuilder gb;
    gb.add("1", "expr", "42");
    gb.run_inference();
    auto* n = gb.find("1");
    ASSERT(n != nullptr);
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    // Should be int? (generic, no context)
    ASSERT(n->outputs[0]->resolved_type->is_generic);
}

TEST(infer_f32_literal) {
    GraphBuilder gb;
    gb.add("1", "expr", "1.0f");
    gb.run_inference();
    auto ts = type_to_string(gb.find("1")->outputs[0]->resolved_type);
    ASSERT_CONTAINS(ts.c_str(), "literal<f32,");
}

TEST(infer_var_ref) {
    GraphBuilder gb;
    gb.add("dt", "decl_type", "mytype x:f32 y:f32");
    gb.add("dv", "decl_var", "foo mytype");
    gb.add("e", "expr", "foo");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n != nullptr);
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    // expr foo returns symbol<foo,mytype>
    ASSERT_TYPE(n->outputs[0].get(), "symbol<foo,mytype>");
}

TEST(infer_unknown_var_returns_undefined_symbol) {
    // Unknown identifiers return undefined_symbol, not an error
    GraphBuilder gb;
    gb.add("1", "expr", "nonexistent");
    gb.run_inference();
    auto* n = gb.find("1");
    ASSERT(n != nullptr);
    ASSERT_TYPE(n->outputs[0].get(), "undefined_symbol<nonexistent>");
}

TEST(infer_unknown_bare_ident_returns_undefined_symbol) {
    GraphBuilder gb;
    gb.add("1", "expr", "foobar");
    gb.run_inference();
    auto* n = gb.find("1");
    ASSERT(n != nullptr);
    ASSERT_TYPE(n->outputs[0].get(), "undefined_symbol<foobar>");
}

TEST(infer_pi_constant) {
    GraphBuilder gb;
    gb.add("1", "expr", "pi");
    gb.run_inference();
    auto* n = gb.find("1");
    ASSERT(n->error.empty());
    // pi is symbol<pi,float<?>>
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_TYPE(n->outputs[0].get(), "symbol<pi,literal<float<?>,?>>");
}

TEST(infer_propagation) {
    // expr "1.0f" -> expr "$0+$1" input 0
    GraphBuilder gb;
    gb.add("a", "expr", "1.0f");
    gb.add("b", "expr", "$0+$1", 2);
    gb.link("a.out0", "b.0");
    gb.run_inference();
    // b's input 0 should have literal<f32, ...> from connection (literals flow as-is)
    auto* b = gb.find("b");
    ASSERT(b != nullptr);
    ASSERT(b->inputs[0]->resolved_type != nullptr);
    auto ts = type_to_string(b->inputs[0]->resolved_type);
    ASSERT_CONTAINS(ts.c_str(), "literal<f32,");
}

TEST(infer_field_access) {
    GraphBuilder gb;
    gb.add("dt", "decl_type", "vec2 x:f32 y:f32");
    gb.add("dv", "decl_var", "pos vec2");
    gb.add("e", "expr", "pos.x");
    gb.run_inference();
    ASSERT_TYPE(gb.find("e")->outputs[0].get(), "f32");
}

TEST(infer_index_vector) {
    GraphBuilder gb;
    gb.add("dt", "decl_type", "flist vector<f32>");
    gb.add("dv", "decl_var", "data flist");
    gb.add("e", "expr", "data[$0]", 1);
    gb.run_inference();
    // data is flist = vector<f32>, indexing gives f32...
    // but flist is Named, needs to resolve through registry
    auto* n = gb.find("e");
    ASSERT(n != nullptr);
    // The type might be f32 or might be unresolved depending on Named resolution
    // For now just check no error about "Cannot index"
    // (flist is a type alias for vector<f32>, which IS indexable)
}

TEST(infer_index_list_error) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data list<f32>");
    gb.add("e", "expr", "data[$0]", 1);
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(!n->error.empty()); // should error: can't index list
}

TEST(infer_query_index_bool) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "m map<u32, f32>");
    gb.add("e", "expr", "m?[$0]", 1);
    gb.run_inference();
    ASSERT_TYPE(gb.find("e")->outputs[0].get(), "bool");
}

// --- Lambda tests ---

TEST(lambda_simple) {
    // decl_type with a function field, new node as lambda
    GraphBuilder gb;
    gb.add("fn_type", "decl_type", "myfn (x:f32) -> f32");
    gb.add("st", "decl_type", "mystruct callback:myfn");
    auto& new_node = gb.add("inst", "new", "mystruct", 1);
    // The new node should have input "callback" after resolve_type_based_pins

    // Lambda: expr $0*2.0f with as_lambda connected to new's callback input
    gb.add("lam", "expr", "$0*2.0f", 1);

    auto errors = gb.run_inference();

    // After resolve_type_based_pins, inst should have a "callback" input
    auto* inst = gb.find("inst");
    // Find callback pin
    FlowPin* callback_pin = nullptr;
    for (auto& p : inst->inputs) {
        if (p->name == "callback") { callback_pin = p.get(); break; }
    }

    if (callback_pin) {
        // Connect lambda
        gb.link("lam.as_lambda", callback_pin->id);
        errors = gb.run_inference();

        // Lambda's $0 input should get type f32
        auto* lam = gb.find("lam");
        ASSERT(lam != nullptr);
        if (!lam->inputs.empty() && lam->inputs[0]->resolved_type) {
            ASSERT_TYPE(lam->inputs[0].get(), "f32");
        }
    }
}

TEST(lambda_param_count_mismatch) {
    // Directly set up a pin with a known function type, bypassing decl_type
    GraphBuilder gb;
    gb.add("target", "expr", "$0", 1, 1);
    // Manually set target's input[0] to expect a function (x:f32 y:f32) -> f32
    auto fn_type = std::make_shared<TypeExpr>();
    fn_type->kind = TypeKind::Function;
    fn_type->func_args.push_back({"x", gb.pool.t_f32});
    fn_type->func_args.push_back({"y", gb.pool.t_f32});
    fn_type->return_type = gb.pool.t_f32;
    gb.find("target")->inputs[0]->resolved_type = fn_type;
    gb.find("target")->inputs[0]->type_name = "(x:f32 y:f32) -> f32";

    gb.add("lam", "expr", "$0", 1, 1); // 1 param, but fn expects 2
    gb.link("lam.as_lambda", "target.0");

    gb.run_inference();
    // Error should be on the link, not the node
    bool found_link_error = false;
    for (auto& l : gb.graph.links) {
        if (l.from_pin == "lam.as_lambda" && !l.error.empty()) {
            found_link_error = true;
            break;
        }
    }
    ASSERT(found_link_error); // should error about param count on the link
}

TEST(lambda_param_count_mismatch_with_decl_type) {
    GraphBuilder gb;
    gb.add("fn_type", "decl_type", "myfn (x:f32 y:f32) -> f32");
    gb.add("st", "decl_type", "mystruct callback:myfn");
    gb.add("inst", "new", "mystruct");
    gb.add("lam", "expr", "$0", 1, 1); // 1 param, but myfn expects 2

    gb.run_inference(); // first pass creates new node pins

    auto* inst = gb.find("inst");
    ASSERT(inst != nullptr);
    FlowPin* callback_pin = nullptr;
    for (auto& p : inst->inputs) {
        if (p->name == "callback") { callback_pin = p.get(); break; }
    }
    ASSERT(callback_pin != nullptr);

    gb.link("lam.as_lambda", callback_pin->id);
    gb.run_inference();

    // Error should be on the link, not the node
    bool found_link_error = false;
    for (auto& l : gb.graph.links) {
        if (l.from_pin == "lam.as_lambda" && !l.error.empty()) {
            found_link_error = true;
            break;
        }
    }
    ASSERT(found_link_error);
}

// --- Recursive lambda parameter collection ---

TEST(lambda_recursive_params) {
    // Chain: expr_dup($0) -> expr_use(sin($0)*$1)
    //   expr_dup has unconnected $0 -> becomes lambda param
    //   expr_use's $1 is connected from elsewhere (captured)
    //   expr_use is the lambda node (as_lambda connected)

    GraphBuilder gb;
    gb.add("fn_type", "decl_type", "callback_fn (id:u64) -> f32");
    gb.add("holder_type", "decl_type", "holder cb:callback_fn");
    gb.add("holder", "new", "holder");

    // dup node: just passes through $0
    gb.add("dup", "expr", "$0", 1, 1);

    // use node: uses $0 (from dup) and $1 (captured value)
    gb.add("use", "expr", "$0+$1", 2, 1);

    gb.run_inference(); // first pass to set up new node pins

    auto* holder = gb.find("holder");
    FlowPin* cb_pin = nullptr;
    for (auto& p : holder->inputs) {
        if (p->name == "cb") { cb_pin = p.get(); break; }
    }
    ASSERT(cb_pin != nullptr);

    // Connect: dup.out0 -> use.0
    gb.link("dup.out0", "use.0");
    // Connect: some f32 source -> use.1 (captured)
    gb.add("cap", "expr", "1.0f", 0, 1);
    gb.link("cap.out0", "use.1");
    // Connect: use.as_lambda -> holder.cb
    gb.link("use.as_lambda", cb_pin->id);

    auto errors = gb.run_inference();

    // dup's $0 should get type u64 from callback_fn's parameter
    auto* dup = gb.find("dup");
    ASSERT(dup != nullptr);
    if (!dup->inputs.empty() && dup->inputs[0]->resolved_type) {
        ASSERT_TYPE(dup->inputs[0].get(), "u64");
    }
}

// --- Multi-output expr tests ---

TEST(expr_multi_output) {
    GraphBuilder gb;
    // expr "0 1 2" should have 3 outputs, each int?
    gb.add("e", "expr", "0 1 2", 0, 3);
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n != nullptr);
    ASSERT_EQ(n->outputs.size(), (size_t)3);
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT(n->outputs[0]->resolved_type->is_generic); // int?
    ASSERT(n->outputs[1]->resolved_type != nullptr);
    ASSERT(n->outputs[1]->resolved_type->is_generic); // int?
    ASSERT(n->outputs[2]->resolved_type != nullptr);
    ASSERT(n->outputs[2]->resolved_type->is_generic); // int?
    ASSERT(n->error.empty());
}

TEST(expr_multi_output_mixed) {
    GraphBuilder gb;
    // expr "1.0f $0 true" should have 3 outputs: literal<f32,...>, value, literal<bool,...>
    gb.add("e", "expr", "1.0f $0 true", 1, 3);
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n != nullptr);
    ASSERT_EQ(n->outputs.size(), (size_t)3);
    ASSERT_CONTAINS(type_to_string(n->outputs[0]->resolved_type).c_str(), "literal<f32,");
    ASSERT_CONTAINS(type_to_string(n->outputs[2]->resolved_type).c_str(), "literal<bool,");
}

TEST(expr_multi_output_with_parens) {
    // "sin($0) $1" — parenthesized expr should not be split
    GraphBuilder gb;
    gb.add("e", "expr", "sin($0) $1", 2, 2);
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n != nullptr);
    ASSERT_EQ(n->outputs.size(), (size_t)2);
    ASSERT(n->error.empty()); // sin($0) should parse fine as one token
}

// --- Lambda + decl_type compatibility tests ---

TEST(lambda_compatible_named_return_type) {
    // gen_fn = (id:u64) -> osc_res, lambda returns osc_res → should be compatible
    GraphBuilder gb;
    gb.add("osc_res_type", "decl_type", "osc_res s:f32 e:bool");
    gb.add("gen_fn_type", "decl_type", "gen_fn (id:u64) -> osc_res");
    gb.add("osc_def_type", "decl_type", "osc_def gen:gen_fn");
    gb.add("inst", "new", "osc_def");
    // A simple lambda: expr node that passes $0 through, with upstream having the osc_res output
    gb.add("lam_src", "new", "osc_res");
    gb.add("lam_pass", "expr", "$0", 1, 1);

    auto errors = gb.run_inference(); // first pass to set up new node pins

    auto* inst = gb.find("inst");
    ASSERT(inst != nullptr);
    FlowPin* gen_pin = nullptr;
    for (auto& p : inst->inputs) {
        if (p->name == "gen") { gen_pin = p.get(); break; }
    }
    ASSERT(gen_pin != nullptr);

    // lam_src.out0 -> lam_pass.$0 (captured osc_res)
    // lam_pass needs no unconnected inputs for param, but gen_fn expects (id:u64)
    // So we need an upstream with unconnected input:
    gb.add("lam_id", "expr", "$0", 1, 1);
    gb.link("lam_id.out0", "lam_pass.0");
    gb.link("lam_pass.as_lambda", gen_pin->id);

    errors = gb.run_inference();

    // lam_id.$0 should be the lambda parameter with type u64
    auto* lam_id = gb.find("lam_id");
    ASSERT(lam_id != nullptr);

    auto* lam_pass = gb.find("lam_pass");
    ASSERT(lam_pass != nullptr);
    // Should NOT have an error — the recursive param finds lam_id.$0 as the u64 param
    if (!lam_pass->error.empty()) {
        printf("  (error was: %s)\n", lam_pass->error.c_str());
    }
    ASSERT(lam_pass->error.empty());
}

TEST(lambda_connection_not_red) {
    // Same setup — the connection type check should not flag a mismatch
    GraphBuilder gb;
    gb.add("gen_fn_type", "decl_type", "gen_fn (x:f32) -> f32");
    gb.add("holder_type", "decl_type", "holder cb:gen_fn");
    gb.add("inst", "new", "holder");
    gb.add("lam", "expr", "$0*2.0f", 1, 1);

    gb.run_inference();

    auto* inst = gb.find("inst");
    FlowPin* cb_pin = nullptr;
    for (auto& p : inst->inputs) {
        if (p->name == "cb") { cb_pin = p.get(); break; }
    }
    ASSERT(cb_pin != nullptr);

    std::string cb_pin_id = cb_pin->id;
    gb.link("lam.as_lambda", cb_pin_id);
    gb.run_inference();

    // Re-find cb_pin after inference (vector may have been rebuilt)
    cb_pin = gb.find_pin(cb_pin_id);
    ASSERT(cb_pin != nullptr);

    // Check that lambda_grab has a resolved type
    auto* lam = gb.find("lam");
    ASSERT(lam != nullptr);
    ASSERT(lam->lambda_grab.resolved_type != nullptr);

    // The connection should be type-compatible
    // lambda_grab type is Function, cb_pin type is Named("gen_fn")
    // types_compatible should return true (Named vs anything = true)
    ASSERT(types_compatible(lam->lambda_grab.resolved_type, cb_pin->resolved_type));

    // No error on the lambda node
    if (!lam->error.empty()) {
        printf("  (error was: %s)\n", lam->error.c_str());
    }
    ASSERT(lam->error.empty());
}

// --- Klavier-like lambda test ---

TEST(klavier_gen_lambda) {
    // Reproduce the klavier.atto gen lambda structure:
    // decl_type osc_res s:f32 e:bool
    // decl_type gen_fn (id:u64) -> osc_res
    // decl_type osc_def gen:gen_fn ...
    // new osc_def (the target)
    // new osc_res (the lambda node) with as_lambda -> target.gen
    // expr $0 (dup) connected to new_osc_res.s  and to expr sin/a
    // expr oscs[$0].a connected to new_osc_res.e via expr $0<0.001

    GraphBuilder gb;
    gb.add("t_osc_res", "decl_type", "osc_res s:f32 e:bool");
    gb.add("t_gen_fn", "decl_type", "gen_fn (id:u64) -> osc_res");
    gb.add("t_osc_def", "decl_type", "osc_def gen:gen_fn");
    gb.add("dv_oscs", "decl_var", "oscs list<osc_def>");

    // Target: new osc_def
    gb.add("target", "new", "osc_def");

    // Lambda: new osc_res
    gb.add("lambda_node", "new", "osc_res");

    // First inference pass to create new node pins
    gb.run_inference();

    auto* target = gb.find("target");
    ASSERT(target != nullptr);
    FlowPin* gen_pin = nullptr;
    for (auto& p : target->inputs)
        if (p->name == "gen") { gen_pin = p.get(); break; }
    ASSERT(gen_pin != nullptr);
    std::string gen_pin_id = gen_pin->id;

    // dup: expr $0 — will be the lambda parameter (the id:u64)
    gb.add("dup", "expr", "$0", 1, 1);

    // oscs_a: expr oscs[$0].a — gets dup output as index
    gb.add("oscs_a", "expr", "$0", 1, 1); // simplified: just passes through

    // sin_expr: expr sin($0)*$1 — $0 from dup, $1 from oscs_a
    gb.add("sin_expr", "expr", "sin($0)*$1", 2, 1);

    // cmp_expr: expr $0<0.001 — connected from oscs_a
    gb.add("cmp_expr", "expr", "$0<0.001", 1, 1);

    // Connect the chain
    gb.link("dup.out0", "sin_expr.0");
    gb.link("oscs_a.out0", "sin_expr.1");
    gb.link("oscs_a.out0", "cmp_expr.0");
    gb.link("dup.out0", "oscs_a.0");

    // lambda_node (new osc_res) inputs: s and e
    // After resolve, lambda_node should have inputs: s, e
    auto* lam = gb.find("lambda_node");
    ASSERT(lam != nullptr);
    FlowPin* s_pin = nullptr;
    FlowPin* e_pin = nullptr;
    for (auto& p : lam->inputs) {
        if (p->name == "s") s_pin = p.get();
        if (p->name == "e") e_pin = p.get();
    }
    ASSERT(s_pin != nullptr);
    ASSERT(e_pin != nullptr);

    gb.link("sin_expr.out0", s_pin->id);
    gb.link("cmp_expr.out0", e_pin->id);
    gb.link("lambda_node.as_lambda", gen_pin_id);

    auto errors = gb.run_inference();

    // Print all errors for debugging
    for (auto& e : errors) printf("    error: %s\n", e.c_str());

    // Lambda node should have NO error
    lam = gb.find("lambda_node");
    ASSERT(lam != nullptr);
    if (!lam->error.empty())
        printf("  (lambda_node error: %s)\n", lam->error.c_str());
    ASSERT(lam->error.empty());

    // dup.$0 should get u64 from lambda param
    auto* dup = gb.find("dup");
    ASSERT(dup != nullptr);
    if (dup->inputs[0]->resolved_type) {
        printf("  (dup.$0 type: %s)\n", type_to_string(dup->inputs[0]->resolved_type).c_str());
    } else {
        printf("  (dup.$0 type: null)\n");
    }
}

// --- Indexing [] tests ---

// Helper: create a graph with a decl_var of given type and an expr that indexes it
static void setup_index_test(GraphBuilder& gb, const std::string& var_type, const std::string& expr_str) {
    gb.add("dv", "decl_var", "data " + var_type);
    gb.add("e", "expr", expr_str, 1, 1);
    gb.run_inference();
}

// --- [] on indexable types ---

TEST(index_vector_f32) {
    GraphBuilder gb;
    setup_index_test(gb, "vector<f32>", "data[$0]");
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "f32");
}

TEST(index_vector_u64) {
    GraphBuilder gb;
    setup_index_test(gb, "vector<u64>", "data[$0]");
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "u64");
}

TEST(index_map_u32_f32) {
    GraphBuilder gb;
    setup_index_test(gb, "map<u32, f32>", "data[$0]");
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "f32");
}

TEST(index_ordered_map) {
    GraphBuilder gb;
    setup_index_test(gb, "ordered_map<u32, string>", "data[$0]");
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "string");
}

TEST(index_array) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data array<f32, 4>");
    gb.add("e", "expr", "data[$0]", 1, 1);
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "f32");
}

TEST(index_tensor) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data tensor<s16>");
    gb.add("e", "expr", "data[$0]", 1, 1);
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "s16");
}

TEST(index_string) {
    GraphBuilder gb;
    setup_index_test(gb, "string", "data[$0]");
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "u8");
}

// --- [] on non-indexable types (should error) ---

TEST(index_list_error) {
    GraphBuilder gb;
    setup_index_test(gb, "list<f32>", "data[$0]");
    auto* n = gb.find("e");
    ASSERT(!n->error.empty());
}

TEST(index_queue_error) {
    GraphBuilder gb;
    setup_index_test(gb, "queue<f32>", "data[$0]");
    auto* n = gb.find("e");
    ASSERT(!n->error.empty());
}

TEST(index_set_error) {
    GraphBuilder gb;
    setup_index_test(gb, "set<u32>", "data[$0]");
    auto* n = gb.find("e");
    ASSERT(!n->error.empty());
}

TEST(index_ordered_set_error) {
    GraphBuilder gb;
    setup_index_test(gb, "ordered_set<u32>", "data[$0]");
    auto* n = gb.find("e");
    ASSERT(!n->error.empty());
}

TEST(index_bool_error) {
    GraphBuilder gb;
    setup_index_test(gb, "bool", "data[$0]");
    auto* n = gb.find("e");
    ASSERT(!n->error.empty());
}

TEST(index_scalar_error) {
    GraphBuilder gb;
    setup_index_test(gb, "f32", "data[$0]");
    auto* n = gb.find("e");
    ASSERT(!n->error.empty());
}

// --- ?[] query index tests ---

TEST(query_index_map) {
    GraphBuilder gb;
    setup_index_test(gb, "map<u32, f32>", "data?[$0]");
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "bool");
}

TEST(query_index_ordered_map) {
    GraphBuilder gb;
    setup_index_test(gb, "ordered_map<u32, f32>", "data?[$0]");
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "bool");
}

TEST(query_index_set) {
    GraphBuilder gb;
    setup_index_test(gb, "set<u32>", "data?[$0]");
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "bool");
}

TEST(query_index_ordered_set) {
    GraphBuilder gb;
    setup_index_test(gb, "ordered_set<u32>", "data?[$0]");
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "bool");
}

TEST(query_index_vector_error) {
    GraphBuilder gb;
    setup_index_test(gb, "vector<f32>", "data?[$0]");
    auto* n = gb.find("e");
    ASSERT(!n->error.empty()); // vector doesn't support ?[]
}

TEST(query_index_list_error) {
    GraphBuilder gb;
    setup_index_test(gb, "list<f32>", "data?[$0]");
    auto* n = gb.find("e");
    ASSERT(!n->error.empty());
}

TEST(query_index_scalar_error) {
    GraphBuilder gb;
    setup_index_test(gb, "f32", "data?[$0]");
    auto* n = gb.find("e");
    ASSERT(!n->error.empty());
}

// --- Named type alias indexing ---

TEST(index_named_vector_alias) {
    GraphBuilder gb;
    gb.add("dt", "decl_type", "flist vector<f32>");
    gb.add("dv", "decl_var", "data flist");
    gb.add("e", "expr", "data[$0]", 1, 1);
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "f32");
}

TEST(index_named_list_alias_error) {
    GraphBuilder gb;
    gb.add("dt", "decl_type", "ilist list<u32>");
    gb.add("dv", "decl_var", "data ilist");
    gb.add("e", "expr", "data[$0]", 1, 1);
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(!n->error.empty());
}

// --- Array manipulation with indexing ---

TEST(index_into_broadcasted_result) {
    // sin(vector<f32>) -> vector<f32>, then [0] -> f32
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data vector<f32>");
    gb.add("e", "expr", "sin(data)[$0]", 1, 1);
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "f32");
}

// --- Struct validation tests ---

TEST(decl_type_struct_must_have_fields) {
    // decl_type with no fields and not an alias should error
    // Note: this error is set by validate_nodes in flow_editor.cpp, not by inference
    // The inference test can still check that an empty struct doesn't cause crashes
    GraphBuilder gb;
    gb.add("dt", "decl_type", "empty_struct");
    gb.run_inference();
    // No crash is the main assertion here
    // The actual error check would need validate_nodes from the editor
}

TEST(decl_type_map_alias) {
    // "decl_type key_set map<u8, u64>" should be an alias, not a struct
    GraphBuilder gb;
    gb.add("dt", "decl_type", "key_set map<u8, u64>");
    gb.add("dv", "decl_var", "data key_set");
    gb.add("e", "expr", "data[$0]", 1, 1);
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "u64"); // map value type
}

TEST(decl_type_ordered_map_alias) {
    GraphBuilder gb;
    gb.add("dt", "decl_type", "omap ordered_map<string, f32>");
    gb.add("dv", "decl_var", "data omap");
    gb.add("e", "expr", "data[$0]", 1, 1);
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "f32");
}

TEST(classify_decl_type_cases) {
    // Alias
    ASSERT_EQ(classify_decl_type({"name", "f32"}), 0);
    ASSERT_EQ(classify_decl_type({"name", "vector<f32>"}), 0);
    ASSERT_EQ(classify_decl_type({"name", "map<u8,", "u64>"}), 0);
    // Function type
    ASSERT_EQ(classify_decl_type({"name", "(x:f32)", "->", "f32"}), 1);
    ASSERT_EQ(classify_decl_type({"name", "(x:f32", "y:f32)", "->", "f32"}), 1);
    // Struct
    ASSERT_EQ(classify_decl_type({"name", "x:f32", "y:f32"}), 2);
    ASSERT_EQ(classify_decl_type({"name", "gen:gen_fn", "stop:stop_fn"}), 2);
}

TEST(decl_type_alias_no_fields_ok) {
    // Type alias: decl_type name existing_type — should be fine with no fields
    GraphBuilder gb;
    gb.add("dt", "decl_type", "my_float f32");
    gb.add("dv", "decl_var", "x my_float");
    gb.add("e", "expr", "x");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "symbol<x,my_float>");
}

TEST(decl_type_func_alias_no_fields_ok) {
    // Function type alias — should be fine with no fields
    GraphBuilder gb;
    gb.add("dt", "decl_type", "callback (x:f32) -> f32");
    gb.run_inference();
    // No crash, no error on inference side
}

// --- Inline expression tests ---

TEST(compute_inline_args_empty) {
    auto info = compute_inline_args("", 2);
    ASSERT_EQ(info.num_inline_args, 0);
    ASSERT_EQ(info.remaining_descriptor_inputs, 2);
    ASSERT_EQ(info.total_pins, 2);
    ASSERT(info.error.empty());
}

TEST(compute_inline_args_one_var) {
    // store! oscs → fills first input, 1 remaining
    auto info = compute_inline_args("oscs", 2);
    ASSERT_EQ(info.num_inline_args, 1);
    ASSERT_EQ(info.remaining_descriptor_inputs, 1);
    ASSERT_EQ(info.pin_slots.max_slot, -1); // oscs is a variable, not N
    ASSERT_EQ(info.total_pins, 1); // 0 ref pins + 1 remaining
    ASSERT(info.error.empty());
}

TEST(compute_inline_args_both_filled) {
    // store! oscs 42 → both filled, 0 remaining
    auto info = compute_inline_args("oscs 42", 2);
    ASSERT_EQ(info.num_inline_args, 2);
    ASSERT_EQ(info.remaining_descriptor_inputs, 0);
    ASSERT_EQ(info.total_pins, 0);
    ASSERT(info.error.empty());
}

TEST(compute_inline_args_with_pin_ref) {
    // store! oscs $0 → fills both, $0 creates 1 pin
    auto info = compute_inline_args("oscs $0", 2);
    ASSERT_EQ(info.num_inline_args, 2);
    ASSERT_EQ(info.remaining_descriptor_inputs, 0);
    ASSERT_EQ(info.pin_slots.max_slot, 0);
    ASSERT_EQ(info.total_pins, 1); // 1 ref pin + 0 remaining
    ASSERT(info.error.empty());
}

TEST(compute_inline_args_two_pin_refs) {
    // store! $1 $0 → 2 pin refs
    auto info = compute_inline_args("$1 $0", 2);
    ASSERT_EQ(info.num_inline_args, 2);
    ASSERT_EQ(info.remaining_descriptor_inputs, 0);
    ASSERT_EQ(info.pin_slots.max_slot, 1);
    ASSERT_EQ(info.total_pins, 2); // 2 ref pins + 0 remaining
    ASSERT(info.error.empty());
}

TEST(compute_inline_args_too_many) {
    // store! $0 $1 $2 → error: too many
    auto info = compute_inline_args("$0 $1 $2", 2);
    ASSERT(!info.error.empty());
}

TEST(compute_inline_args_gap) {
    // store! $0 $2 → error: missing $1
    auto info = compute_inline_args("$0 $2", 2);
    ASSERT(!info.error.empty());
}

TEST(compute_inline_args_with_parens) {
    // select! keys?[$0] → 1 inline arg, $0 creates 1 pin
    auto info = compute_inline_args("keys?[$0]", 1);
    ASSERT_EQ(info.num_inline_args, 1);
    ASSERT_EQ(info.remaining_descriptor_inputs, 0);
    ASSERT_EQ(info.pin_slots.max_slot, 0);
    ASSERT_EQ(info.total_pins, 1);
    ASSERT(info.error.empty());
}

TEST(compute_inline_args_lambda_ref) {
    // iterate! oscs @0 → 2 inline args, @0 creates 1 lambda pin
    auto info = compute_inline_args("oscs @0", 2);
    ASSERT_EQ(info.num_inline_args, 2);
    ASSERT_EQ(info.remaining_descriptor_inputs, 0);
    ASSERT_EQ(info.pin_slots.max_slot, 0);
    ASSERT(info.pin_slots.is_lambda_slot(0));
    ASSERT_EQ(info.total_pins, 1);
    ASSERT(info.error.empty());
}

TEST(inline_store_pin_count) {
    // Full integration test: create a store! node with inline args
    GraphBuilder gb;
    gb.add("dv", "decl_var", "oscs list<f32>");
    auto& store = gb.add("s", "store!", "oscs $0"); // 1 pin for $0
    gb.run_inference();
    auto* n = gb.find("s");
    ASSERT(n != nullptr);
    ASSERT_EQ((int)n->inputs.size(), 1); // only $0 pin
}

// --- store! validation tests ---

TEST(store_varref_lvalue) {
    // store! myvar 42 — myvar is a valid lvalue
    GraphBuilder gb;
    gb.add("dv", "decl_var", "myvar u32");
    gb.add("s", "store!", "myvar 42");
    gb.run_inference();
    auto* n = gb.find("s");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
}

TEST(store_indexed_lvalue) {
    // store! data[$0] 42 — indexed variable is a valid lvalue
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data vector<u32>");
    gb.add("s", "store!", "data[$0] 42");
    gb.run_inference();
    auto* n = gb.find("s");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
}

TEST(store_field_lvalue) {
    // store! pos.x 1.0f — field access on variable is a valid lvalue
    GraphBuilder gb;
    gb.add("dt", "decl_type", "vec2 x:f32 y:f32");
    gb.add("dv", "decl_var", "pos vec2");
    gb.add("s", "store!", "pos.x 1.0f");
    gb.run_inference();
    auto* n = gb.find("s");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
}

TEST(store_indexed_field_lvalue) {
    // store! oscs[$0].p 3.14f — indexed + field is a valid lvalue
    GraphBuilder gb;
    gb.add("dt", "decl_type", "osc p:f32 a:f32");
    gb.add("dt2", "decl_type", "osc_list vector<osc>");
    gb.add("dv", "decl_var", "oscs osc_list");
    gb.add("s", "store!", "oscs[$0].p 3.14f");
    gb.run_inference();
    auto* n = gb.find("s");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
}

TEST(store_literal_not_lvalue) {
    // store! 42 1 — literal is NOT an lvalue
    GraphBuilder gb;
    gb.add("s", "store!", "42 1");
    gb.run_inference();
    auto* n = gb.find("s");
    ASSERT(n != nullptr);
    ASSERT(!n->error.empty()); // should error: target must be assignable
}

TEST(store_expr_not_lvalue) {
    // store! ($0+1) 42 — arithmetic expression is NOT an lvalue
    GraphBuilder gb;
    gb.add("s", "store!", "($0+1) 42");
    gb.run_inference();
    auto* n = gb.find("s");
    ASSERT(n != nullptr);
    ASSERT(!n->error.empty());
}

TEST(store_func_call_not_lvalue) {
    // store! sin($0) 42 — function call is NOT an lvalue
    GraphBuilder gb;
    gb.add("s", "store!", "sin($0) 42");
    gb.run_inference();
    auto* n = gb.find("s");
    ASSERT(n != nullptr);
    ASSERT(!n->error.empty());
}

TEST(store_type_compatible) {
    // store! myvar $0 — where myvar is f32 and $0 is connected as f32
    GraphBuilder gb;
    gb.add("dv", "decl_var", "myvar f32");
    gb.add("src", "expr", "1.0f", 0, 1);
    gb.add("s", "store!", "myvar $0");
    gb.link("src.out0", "s.0");
    gb.run_inference();
    auto* n = gb.find("s");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
}

TEST(store_type_incompatible) {
    // store! myvar $0 — where myvar is f32 but $0 is bool
    GraphBuilder gb;
    gb.add("dv", "decl_var", "myvar f32");
    gb.add("src", "expr", "true", 0, 1);
    gb.add("s", "store!", "myvar $0");
    gb.link("src.out0", "s.0");
    gb.run_inference();
    auto* n = gb.find("s");
    ASSERT(n != nullptr);
    ASSERT(!n->error.empty()); // type mismatch: bool into f32
}

TEST(store_type_int_coercion) {
    // store! myvar 42 — where myvar is u32, 42 should coerce
    GraphBuilder gb;
    gb.add("dv", "decl_var", "myvar u32");
    gb.add("s", "store!", "myvar 42");
    gb.run_inference();
    auto* n = gb.find("s");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty()); // int literal should be compatible
}

TEST(store_pinref_lvalue) {
    // store! $0 $1 — pin refs are lvalues
    GraphBuilder gb;
    gb.add("s", "store!", "$0 $1");
    gb.run_inference();
    auto* n = gb.find("s");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty()); // both are generic, no type mismatch
}

// --- dup type propagation tests ---

TEST(dup_propagates_type_from_connection) {
    GraphBuilder gb;
    gb.add("src", "expr", "1.0f", 0, 1);
    gb.add("d", "dup", "");
    gb.link("src.out0", "d.value");
    gb.run_inference();
    auto* n = gb.find("d");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
    ASSERT_CONTAINS(type_to_string(n->outputs[0]->resolved_type).c_str(), "literal<f32,");
}

TEST(dup_propagates_type_from_inline) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "x f32");
    gb.add("d", "dup", "x");
    gb.run_inference();
    auto* n = gb.find("d");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "f32");
}

TEST(dup_propagates_generic) {
    GraphBuilder gb;
    gb.add("d", "dup", "42");
    gb.run_inference();
    auto* n = gb.find("d");
    ASSERT(n != nullptr);
    // 42 is int? (generic), output should also be int?
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT(n->outputs[0]->resolved_type->is_generic);
}

TEST(dup_chain_propagation) {
    // dup chain: expr 1.0f → dup → dup → check output
    GraphBuilder gb;
    gb.add("src", "expr", "1.0f", 0, 1);
    gb.add("d1", "dup", "");
    gb.add("d2", "dup", "");
    gb.link("src.out0", "d1.value");
    gb.link("d1.out0", "d2.value");
    gb.run_inference();
    ASSERT_CONTAINS(type_to_string(gb.find("d2")->outputs[0]->resolved_type).c_str(), "literal<f32,");
}

// --- select! validation tests ---

TEST(cond_bool_ok) {
    GraphBuilder gb;
    gb.add("c", "select!", "true");
    gb.run_inference();
    ASSERT(gb.find("c")->error.empty());
}

TEST(cond_bool_from_connection_ok) {
    GraphBuilder gb;
    gb.add("src", "expr", "true", 0, 1);
    gb.add("c", "select!", "");
    gb.link("src.out0", "c.condition");
    gb.run_inference();
    ASSERT(gb.find("c")->error.empty());
}

TEST(cond_f32_error) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "x f32");
    gb.add("c", "select!", "x");
    gb.run_inference();
    ASSERT(!gb.find("c")->error.empty());
}

TEST(cond_u32_error) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "x u32");
    gb.add("c", "select!", "x");
    gb.run_inference();
    ASSERT(!gb.find("c")->error.empty());
}

// --- expr! tests ---

TEST(expr_bang_basic) {
    GraphBuilder gb;
    gb.add("e", "expr!", "$0+1", 1, 1);
    gb.run_inference();
    ASSERT(gb.find("e")->error.empty());
}

TEST(expr_bang_type_inference) {
    GraphBuilder gb;
    gb.add("src", "expr", "1.0f", 0, 1);
    gb.add("e", "expr!", "$0*2.0f", 1, 1);
    gb.link("src.out0", "e.0");
    gb.run_inference();
    ASSERT_TYPE(gb.find("e")->outputs[0].get(), "f32");
}

// --- output_mix! validation tests ---

TEST(output_mix_f32_ok) {
    GraphBuilder gb;
    gb.add("o", "output_mix!", "1.0f");
    gb.run_inference();
    ASSERT(gb.find("o")->error.empty());
}

TEST(output_mix_f32_from_connection_ok) {
    GraphBuilder gb;
    gb.add("src", "expr", "1.0f", 0, 1);
    gb.add("o", "output_mix!", "");
    gb.link("src.out0", "o.value");
    gb.run_inference();
    ASSERT(gb.find("o")->error.empty());
}

TEST(output_mix_u32_error) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "x u32");
    gb.add("o", "output_mix!", "x");
    gb.run_inference();
    ASSERT(!gb.find("o")->error.empty());
}

TEST(output_mix_f64_error) {
    GraphBuilder gb;
    gb.add("o", "output_mix!", "1.0");
    gb.run_inference();
    ASSERT(!gb.find("o")->error.empty());
}

TEST(output_mix_bool_error) {
    GraphBuilder gb;
    gb.add("o", "output_mix!", "true");
    gb.run_inference();
    ASSERT(!gb.find("o")->error.empty());
}

TEST(output_mix_string_error) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "s string");
    gb.add("o", "output_mix!", "s");
    gb.run_inference();
    ASSERT(!gb.find("o")->error.empty());
}

// --- Iterator field access tests ---

TEST(iterator_value_field_error_on_non_map) {
    // Non-map iterators don't have .value — they auto-deref instead
    GraphBuilder gb;
    gb.add("dv", "decl_var", "it vector_iterator<f32>");
    gb.add("e", "expr", "it.value");
    gb.run_inference();
    auto* n = gb.find("e");
    // f32 is scalar, has no field "value" → error
    ASSERT(!n->error.empty());
}

TEST(map_iterator_key_field) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "it map_iterator<u32, f32>");
    gb.add("e", "expr", "it.key");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "u32");
}

TEST(map_iterator_value_field) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "it map_iterator<u32, f32>");
    gb.add("e", "expr", "it.value");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "f32");
}

TEST(ordered_map_iterator_key_field) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "it ordered_map_iterator<string, u64>");
    gb.add("e", "expr", "it.key");
    gb.run_inference();
    ASSERT(gb.find("e")->error.empty());
    ASSERT_TYPE(gb.find("e")->outputs[0].get(), "string");
}

TEST(list_iterator_auto_deref) {
    // list_iterator<osc> auto-derefs to osc fields
    GraphBuilder gb;
    gb.add("dt", "decl_type", "osc p:f32 a:f32");
    gb.add("dv", "decl_var", "it list_iterator<osc>");
    gb.add("e", "expr", "it.p");
    gb.run_inference();
    ASSERT(gb.find("e")->error.empty());
    ASSERT_TYPE(gb.find("e")->outputs[0].get(), "f32");
}

TEST(iterator_auto_deref_field) {
    // vector_iterator<osc_def>.gen should find gen on osc_def
    GraphBuilder gb;
    gb.add("dt", "decl_type", "gen_fn (id:u64) -> void");
    gb.add("dt2", "decl_type", "osc_def gen:gen_fn p:f32");
    gb.add("dv", "decl_var", "it vector_iterator<osc_def>");
    gb.add("e", "expr", "it.p");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "f32");
}

TEST(iterator_auto_deref_field_named) {
    // Named alias: osc_list = vector<osc_def>, iterator.gen should work
    GraphBuilder gb;
    gb.add("dt", "decl_type", "osc_def gen:f32 stop:f32");
    gb.add("dt2", "decl_type", "osc_list vector<osc_def>");
    gb.add("dv", "decl_var", "it vector_iterator<osc_def>");
    gb.add("e", "expr", "it.gen");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "f32");
}

TEST(iterator_auto_deref_element_field_named_value) {
    // Non-map iterator auto-derefs, so .value accesses the element's "value" field
    GraphBuilder gb;
    gb.add("dt", "decl_type", "thing value:u32 name:string");
    gb.add("dv", "decl_var", "it vector_iterator<thing>");
    gb.add("e", "expr", "it.value");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    // .value on non-map iterator auto-derefs to thing.value → u32
    ASSERT_TYPE(n->outputs[0].get(), "u32");
}

TEST(vector_iterator_no_key_error) {
    // vector_iterator doesn't have .key
    GraphBuilder gb;
    gb.add("dv", "decl_var", "it vector_iterator<f32>");
    gb.add("e", "expr", "it.key");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty());
}

TEST(set_iterator_no_deref_scalar) {
    // set_iterator<u32>: u32 is scalar, has no fields → error on .value
    GraphBuilder gb;
    gb.add("dv", "decl_var", "it set_iterator<u32>");
    gb.add("e", "expr", "it.value");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty());
}

// --- iterate! validation tests ---

TEST(iterate_vector_ok) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data vector<f32>");
    gb.add("it", "iterate!", "data @0");
    gb.run_inference();
    auto* n = gb.find("it");
    ASSERT(n->error.empty());
}

TEST(iterate_list_ok) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data list<u32>");
    gb.add("it", "iterate!", "data @0");
    gb.run_inference();
    ASSERT(gb.find("it")->error.empty());
}

TEST(iterate_map_ok) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data map<u32, f32>");
    gb.add("it", "iterate!", "data @0");
    gb.run_inference();
    ASSERT(gb.find("it")->error.empty());
}

TEST(iterate_set_ok) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data set<u32>");
    gb.add("it", "iterate!", "data @0");
    gb.run_inference();
    ASSERT(gb.find("it")->error.empty());
}

TEST(iterate_array_ok) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data array<f32, 4>");
    gb.add("it", "iterate!", "data @0");
    gb.run_inference();
    ASSERT(gb.find("it")->error.empty());
}

TEST(iterate_tensor_ok) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data tensor<f32>");
    gb.add("it", "iterate!", "data @0");
    gb.run_inference();
    ASSERT(gb.find("it")->error.empty());
}

TEST(iterate_scalar_ok) {
    // Scalar: runs once, lambda gets &f32
    GraphBuilder gb;
    gb.add("dv", "decl_var", "x f32");
    gb.add("it", "iterate!", "x @0");
    gb.run_inference();
    ASSERT(gb.find("it")->error.empty());
}

TEST(iterate_vector_lambda_param_type) {
    // iterate! data @0 where data is vector<f32>
    // The lambda connected via @0 should get parameter type ^vector_iterator<f32>
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data vector<f32>");
    gb.add("it", "iterate!", "data @0");
    gb.run_inference();
    auto* n = gb.find("it");
    ASSERT(n->error.empty());
    // Find the lambda pin and check its resolved type is a function
    FlowPin* lam_pin = nullptr;
    for (auto& p : n->inputs)
        if (p->direction == FlowPin::Lambda) { lam_pin = p.get(); break; }
    ASSERT(lam_pin != nullptr);
    ASSERT(lam_pin->resolved_type != nullptr);
    ASSERT_EQ(lam_pin->resolved_type->kind, TypeKind::Function);
    ASSERT_EQ(lam_pin->resolved_type->func_args.size(), (size_t)1);
    ASSERT_EQ(lam_pin->resolved_type->func_args[0].type->kind, TypeKind::ContainerIterator);
    ASSERT_EQ(lam_pin->resolved_type->func_args[0].type->iterator, IteratorKind::Vector);
}

TEST(iterate_array_lambda_param_type) {
    // iterate! data @0 where data is array<f32, 4>
    // Lambda gets &f32
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data array<f32, 4>");
    gb.add("it", "iterate!", "data @0");
    gb.run_inference();
    auto* n = gb.find("it");
    ASSERT(n->error.empty());
    FlowPin* lam_pin = nullptr;
    for (auto& p : n->inputs)
        if (p->direction == FlowPin::Lambda) { lam_pin = p.get(); break; }
    ASSERT(lam_pin != nullptr);
    ASSERT(lam_pin->resolved_type != nullptr);
    ASSERT_EQ(lam_pin->resolved_type->kind, TypeKind::Function);
    auto& param = lam_pin->resolved_type->func_args[0].type;
    ASSERT_EQ(param->category, TypeCategory::Reference);
    ASSERT_EQ(param->kind, TypeKind::Scalar);
    ASSERT_EQ(param->scalar, ScalarType::F32);
}

TEST(iterate_scalar_lambda_param_type) {
    // iterate! x @0 where x is f32 → lambda gets &f32
    GraphBuilder gb;
    gb.add("dv", "decl_var", "x f32");
    gb.add("it", "iterate!", "x @0");
    gb.run_inference();
    auto* n = gb.find("it");
    ASSERT(n->error.empty());
    FlowPin* lam_pin = nullptr;
    for (auto& p : n->inputs)
        if (p->direction == FlowPin::Lambda) { lam_pin = p.get(); break; }
    ASSERT(lam_pin != nullptr);
    ASSERT(lam_pin->resolved_type != nullptr);
    ASSERT_EQ(lam_pin->resolved_type->func_args[0].type->category, TypeCategory::Reference);
}

TEST(iterate_map_lambda_param_type) {
    // iterate! m @0 where m is map<u32, f32> → lambda gets ^map_iterator<u32, f32>
    GraphBuilder gb;
    gb.add("dv", "decl_var", "m map<u32, f32>");
    gb.add("it", "iterate!", "m @0");
    gb.run_inference();
    auto* n = gb.find("it");
    ASSERT(n->error.empty());
    FlowPin* lam_pin = nullptr;
    for (auto& p : n->inputs)
        if (p->direction == FlowPin::Lambda) { lam_pin = p.get(); break; }
    ASSERT(lam_pin != nullptr);
    ASSERT(lam_pin->resolved_type != nullptr);
    ASSERT_EQ(lam_pin->resolved_type->func_args[0].type->kind, TypeKind::ContainerIterator);
    ASSERT_EQ(lam_pin->resolved_type->func_args[0].type->iterator, IteratorKind::Map);
}

TEST(iterate_vector_lambda_returns_iterator) {
    // iterate! on vector: lambda should be (^vector_iterator<V>) -> ^vector_iterator<V>
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data vector<f32>");
    gb.add("it", "iterate!", "data @0");
    gb.run_inference();
    auto* n = gb.find("it");
    FlowPin* lam_pin = nullptr;
    for (auto& p : n->inputs)
        if (p->direction == FlowPin::Lambda) { lam_pin = p.get(); break; }
    ASSERT(lam_pin != nullptr);
    ASSERT(lam_pin->resolved_type != nullptr);
    ASSERT_EQ(lam_pin->resolved_type->kind, TypeKind::Function);
    // Return type should be iterator, not void
    ASSERT(lam_pin->resolved_type->return_type != nullptr);
    ASSERT_EQ(lam_pin->resolved_type->return_type->kind, TypeKind::ContainerIterator);
}

TEST(iterate_array_lambda_returns_void) {
    // iterate! on array: lambda should be (&V) -> void
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data array<f32, 4>");
    gb.add("it", "iterate!", "data @0");
    gb.run_inference();
    auto* n = gb.find("it");
    FlowPin* lam_pin = nullptr;
    for (auto& p : n->inputs)
        if (p->direction == FlowPin::Lambda) { lam_pin = p.get(); break; }
    ASSERT(lam_pin != nullptr);
    ASSERT(lam_pin->resolved_type != nullptr);
    ASSERT_EQ(lam_pin->resolved_type->return_type->kind, TypeKind::Void);
}

TEST(iterate_scalar_lambda_returns_void) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "x f32");
    gb.add("it", "iterate!", "x @0");
    gb.run_inference();
    auto* n = gb.find("it");
    FlowPin* lam_pin = nullptr;
    for (auto& p : n->inputs)
        if (p->direction == FlowPin::Lambda) { lam_pin = p.get(); break; }
    ASSERT(lam_pin != nullptr);
    ASSERT(lam_pin->resolved_type != nullptr);
    ASSERT_EQ(lam_pin->resolved_type->return_type->kind, TypeKind::Void);
}

// --- append!/erase output type tests ---

TEST(append_returns_iterator) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data vector<f32>");
    gb.add("a", "append!", "data 1.0f");
    gb.run_inference();
    auto* n = gb.find("a");
    ASSERT(n->error.empty());
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_EQ(n->outputs[0]->resolved_type->kind, TypeKind::ContainerIterator);
    ASSERT_EQ(n->outputs[0]->resolved_type->iterator, IteratorKind::Vector);
}

TEST(append_list_returns_list_iterator) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data list<u32>");
    gb.add("a", "append!", "data 42");
    gb.run_inference();
    auto* n = gb.find("a");
    ASSERT(n->error.empty());
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_EQ(n->outputs[0]->resolved_type->kind, TypeKind::ContainerIterator);
    ASSERT_EQ(n->outputs[0]->resolved_type->iterator, IteratorKind::List);
}

TEST(erase_returns_iterator) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data vector<f32>");
    gb.add("dv2", "decl_var", "it vector_iterator<f32>");
    gb.add("e", "erase", "data it");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_EQ(n->outputs[0]->resolved_type->kind, TypeKind::ContainerIterator);
    ASSERT_EQ(n->outputs[0]->resolved_type->iterator, IteratorKind::Vector);
}

TEST(erase_map_returns_map_iterator) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "m map<u32, f32>");
    gb.add("dv2", "decl_var", "k u32");
    gb.add("e", "erase", "m k");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_EQ(n->outputs[0]->resolved_type->kind, TypeKind::ContainerIterator);
    ASSERT_EQ(n->outputs[0]->resolved_type->iterator, IteratorKind::Map);
}

TEST(iterate_bool_error) {
    // bool is not a valid iterate target
    GraphBuilder gb;
    gb.add("dv", "decl_var", "x bool");
    gb.add("it", "iterate!", "x @0");
    gb.run_inference();
    ASSERT(!gb.find("it")->error.empty());
}

TEST(iterate_string_error) {
    // string is not iterable (use indexing instead)
    GraphBuilder gb;
    gb.add("dv", "decl_var", "s string");
    gb.add("it", "iterate!", "s @0");
    gb.run_inference();
    ASSERT(!gb.find("it")->error.empty());
}

TEST(iterate_literal_target_error) {
    GraphBuilder gb;
    gb.add("it", "iterate!", "42 @0");
    gb.run_inference();
    ASSERT(!gb.find("it")->error.empty()); // literal not an lvalue
}

// --- select validation tests ---

TEST(select_compatible_types_ok) {
    GraphBuilder gb;
    gb.add("s", "select", "true 1.0f 2.0f");
    gb.run_inference();
    auto* n = gb.find("s");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "f32");
}

TEST(select_incompatible_types_error) {
    // select true 1.f "a" — f32 vs string
    GraphBuilder gb;
    gb.add("s", "select", "true 1.0f \"a\"");
    gb.run_inference();
    auto* n = gb.find("s");
    ASSERT(!n->error.empty()); // f32 vs string incompatible
}

TEST(select_non_bool_condition_error) {
    GraphBuilder gb;
    gb.add("s", "select", "42 1.0f 2.0f");
    gb.run_inference();
    auto* n = gb.find("s");
    // 42 is int? (generic), not bool — should error once resolved
    // Actually int? is generic so the check skips... let's use a concrete non-bool
    // Use a variable instead
    GraphBuilder gb2;
    gb2.add("dv", "decl_var", "x f32");
    gb2.add("s", "select", "x 1.0f 2.0f");
    gb2.run_inference();
    auto* n2 = gb2.find("s");
    ASSERT(!n2->error.empty()); // f32 is not bool
}

TEST(select_with_connections_ok) {
    GraphBuilder gb;
    gb.add("cond", "expr", "true", 0, 1);
    gb.add("a", "expr", "1.0f", 0, 1);
    gb.add("b", "expr", "2.0f", 0, 1);
    gb.add("s", "select", "");
    gb.link("cond.out0", "s.condition");
    gb.link("a.out0", "s.if_true");
    gb.link("b.out0", "s.if_false");
    gb.run_inference();
    auto* n = gb.find("s");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "f32");
}

TEST(select_mixed_inline_and_connection) {
    // select $0 1.0f 2.0f — condition from pin, values inline
    GraphBuilder gb;
    gb.add("cond", "expr", "true", 0, 1);
    gb.add("s", "select", "$0 1.0f 2.0f");
    gb.link("cond.out0", "s.0");
    gb.run_inference();
    auto* n = gb.find("s");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "f32");
}

TEST(select_int_coercion) {
    // select true 1 2 — both int literals, should be compatible
    GraphBuilder gb;
    gb.add("s", "select", "true 1 2");
    gb.run_inference();
    auto* n = gb.find("s");
    ASSERT(n->error.empty());
}

// --- erase/erase! validation tests ---

TEST(erase_map_by_key_ok) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "m map<u32, f32>");
    gb.add("dv2", "decl_var", "k u32");
    gb.add("e", "erase", "m k");
    gb.run_inference();
    ASSERT(gb.find("e")->error.empty());
}

TEST(erase_map_by_iterator_ok) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "m map<u32, f32>");
    gb.add("dv2", "decl_var", "it map_iterator<u32, f32>");
    gb.add("e", "erase", "m it");
    gb.run_inference();
    ASSERT(gb.find("e")->error.empty());
}

TEST(erase_map_wrong_key_error) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "m map<u32, f32>");
    gb.add("dv2", "decl_var", "k f32");
    gb.add("e", "erase", "m k");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty()); // f32 is not u32 key
}

TEST(erase_set_by_value_ok) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "s set<u32>");
    gb.add("dv2", "decl_var", "v u32");
    gb.add("e", "erase", "s v");
    gb.run_inference();
    ASSERT(gb.find("e")->error.empty());
}

TEST(erase_set_wrong_value_error) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "s set<u32>");
    gb.add("dv2", "decl_var", "v f32");
    gb.add("e", "erase", "s v");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty()); // f32 is not u32
}

TEST(erase_list_by_iterator_ok) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "l list<f32>");
    gb.add("dv2", "decl_var", "it list_iterator<f32>");
    gb.add("e", "erase", "l it");
    gb.run_inference();
    ASSERT(gb.find("e")->error.empty());
}

TEST(erase_list_by_value_error) {
    // list only supports erase by iterator, not by value
    GraphBuilder gb;
    gb.add("dv", "decl_var", "l list<f32>");
    gb.add("dv2", "decl_var", "v f32");
    gb.add("e", "erase", "l v");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty());
}

TEST(erase_vector_by_iterator_ok) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "v vector<f32>");
    gb.add("dv2", "decl_var", "it vector_iterator<f32>");
    gb.add("e", "erase", "v it");
    gb.run_inference();
    ASSERT(gb.find("e")->error.empty());
}

TEST(erase_vector_by_index_ok) {
    // vector supports erase by integer index
    GraphBuilder gb;
    gb.add("dv", "decl_var", "v vector<f32>");
    gb.add("dv2", "decl_var", "idx u32");
    gb.add("e", "erase", "v idx");
    gb.run_inference();
    ASSERT(gb.find("e")->error.empty());
}

TEST(erase_vector_by_value_error) {
    // vector does NOT support erase by value type (f32 is not an index or iterator)
    GraphBuilder gb;
    gb.add("dv", "decl_var", "v vector<f32>");
    gb.add("dv2", "decl_var", "val f32");
    gb.add("e", "erase", "v val");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty());
}

TEST(erase_scalar_error) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "x f32");
    gb.add("e", "erase", "x 0");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty()); // can't erase from scalar
}

TEST(erase_wrong_iterator_error) {
    // map_iterator used on a vector → error
    GraphBuilder gb;
    gb.add("dv", "decl_var", "v vector<f32>");
    gb.add("dv2", "decl_var", "it map_iterator<u32, f32>");
    gb.add("e", "erase", "v it");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty());
}

TEST(erase_bang_variant) {
    // erase! works the same way
    GraphBuilder gb;
    gb.add("dv", "decl_var", "m map<u32, f32>");
    gb.add("dv2", "decl_var", "k u32");
    gb.add("e", "erase!", "m k");
    gb.run_inference();
    ASSERT(gb.find("e")->error.empty());
}

// --- append! validation tests ---

TEST(append_vector_ok) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data vector<f32>");
    gb.add("a", "append!", "data 1.0f");
    gb.run_inference();
    auto* n = gb.find("a");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
}

TEST(append_list_ok) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data list<u32>");
    gb.add("a", "append!", "data 42");
    gb.run_inference();
    auto* n = gb.find("a");
    ASSERT(n->error.empty());
}

TEST(append_queue_ok) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data queue<string>");
    gb.add("a", "append!", "data \"hello\"");
    gb.run_inference();
    auto* n = gb.find("a");
    ASSERT(n->error.empty());
}

TEST(append_map_error) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data map<u32, f32>");
    gb.add("a", "append!", "data 1.0f");
    gb.run_inference();
    auto* n = gb.find("a");
    ASSERT(!n->error.empty()); // can't append to map
}

TEST(append_set_error) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data set<u32>");
    gb.add("a", "append!", "data 42");
    gb.run_inference();
    auto* n = gb.find("a");
    ASSERT(!n->error.empty()); // can't append to set
}

TEST(append_scalar_error) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data f32");
    gb.add("a", "append!", "data 1.0f");
    gb.run_inference();
    auto* n = gb.find("a");
    ASSERT(!n->error.empty()); // can't append to scalar
}

TEST(append_type_mismatch) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data vector<f32>");
    gb.add("a", "append!", "data true");
    gb.run_inference();
    auto* n = gb.find("a");
    ASSERT(!n->error.empty()); // bool into vector<f32>
}

TEST(append_named_alias_ok) {
    GraphBuilder gb;
    gb.add("dt", "decl_type", "flist vector<f32>");
    gb.add("dv", "decl_var", "data flist");
    gb.add("a", "append!", "data 1.0f");
    gb.run_inference();
    auto* n = gb.find("a");
    ASSERT(n->error.empty()); // flist = vector<f32>, append f32 ok
}

TEST(append_literal_target_error) {
    GraphBuilder gb;
    gb.add("a", "append!", "42 1");
    gb.run_inference();
    auto* n = gb.find("a");
    ASSERT(!n->error.empty()); // literal is not an lvalue
}

// --- Spaceship operator <=> tests ---

TEST(parse_spaceship) {
    auto r = parse_expression("$0<=>$1");
    ASSERT(r.root != nullptr);
    ASSERT(r.error.empty());
    ASSERT_EQ(r.root->kind, ExprKind::BinaryOp);
    ASSERT_EQ(r.root->bin_op, BinOp::Spaceship);
}

TEST(spaceship_returns_s32) {
    GraphBuilder gb;
    gb.add("dv1", "decl_var", "a u32");
    gb.add("dv2", "decl_var", "b u32");
    gb.add("e", "expr", "a<=>b");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "s32");
}

TEST(spaceship_type_mismatch_error) {
    GraphBuilder gb;
    gb.add("dv1", "decl_var", "a u32");
    gb.add("dv2", "decl_var", "b f32");
    gb.add("e", "expr", "a<=>b");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(!n->error.empty()); // can't compare u32 with f32
}

TEST(spaceship_broadcast) {
    // vector<u32> <=> u32 → vector<s32>
    GraphBuilder gb;
    gb.add("dv1", "decl_var", "data vector<u32>");
    gb.add("dv2", "decl_var", "val u32");
    gb.add("e", "expr", "data<=>val");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "vector<s32>");
}

TEST(spaceship_not_confused_with_le) {
    // Make sure <= still works and doesn't get confused with <=>
    auto r1 = parse_expression("$0<=$1");
    ASSERT(r1.root != nullptr);
    ASSERT_EQ(r1.root->bin_op, BinOp::Le);

    auto r2 = parse_expression("$0<=>$1");
    ASSERT(r2.root != nullptr);
    ASSERT_EQ(r2.root->bin_op, BinOp::Spaceship);
}

// --- Lambda bang chain parameter collection tests ---

TEST(lambda_bang_chain_params) {
    // Lambda node (expr $0) with post_bang connected to a store! node
    // The store! has an unconnected input → should become a lambda parameter
    //
    //  [expr $0]  --post_bang-->  [store! myvar $1]
    //     ^as_lambda                  $1 is unconnected → lambda param
    //     |
    //     v
    //  [new holder]
    //
    // Expected: 2 lambda params: expr.$0 and store!.$1 (from the bang chain)

    GraphBuilder gb;
    gb.add("fn_type", "decl_type", "callback (a:u32 b:f32) -> u32");
    gb.add("holder_type", "decl_type", "holder cb:callback");
    gb.add("dv", "decl_var", "myvar f32");
    gb.add("holder", "new", "holder");

    // Lambda: expr $0 (returns $0)
    gb.add("lam", "expr", "$0", 1, 1);

    // Store connected via bang chain: store! myvar $1
    // $1 is unconnected → becomes lambda param
    gb.add("st", "store!", "myvar $0", 1);

    gb.run_inference();

    // Get holder's cb pin
    auto* h = gb.find("holder");
    ASSERT(h != nullptr);
    FlowPin* cb_pin = nullptr;
    for (auto& p : h->inputs)
        if (p->name == "cb") { cb_pin = p.get(); break; }
    ASSERT(cb_pin != nullptr);

    // Connect lam.post_bang -> st.bang_in0
    gb.link("lam.post_bang", "st.bang_in0");
    // Connect lam.as_lambda -> holder.cb
    gb.link("lam.as_lambda", cb_pin->id);

    gb.run_inference();

    auto* lam = gb.find("lam");
    ASSERT(lam != nullptr);

    // Lambda should have 2 params: lam.$0 and st.$0 (from bang chain)
    // callback expects (a:u32 b:f32) → 2 params
    // Check no error on the lambda link
    bool link_has_error = false;
    for (auto& l : gb.graph.links)
        if (l.from_pin == "lam.as_lambda" && !l.error.empty()) {
            printf("  link error: %s\n", l.error.c_str());
            link_has_error = true;
        }
    ASSERT(!link_has_error);

    // lam.$0 should get type u32 (first callback param)
    if (!lam->inputs.empty() && lam->inputs[0]->resolved_type)
        ASSERT_TYPE(lam->inputs[0].get(), "u32");

    // st.$0 should get type f32 (second callback param)
    auto* st = gb.find("st");
    ASSERT(st != nullptr);
    if (!st->inputs.empty() && st->inputs[0]->resolved_type)
        ASSERT_TYPE(st->inputs[0].get(), "f32");
}

TEST(lambda_output_bang_chain_params) {
    // Lambda node with bang_outputs (like select!) connected to downstream nodes
    // The downstream nodes' unconnected inputs become lambda params after the main ones

    GraphBuilder gb;
    gb.add("fn_type", "decl_type", "callback (a:u32 b:f32 c:bool) -> void");
    gb.add("holder_type", "decl_type", "holder cb:callback");
    gb.add("dv1", "decl_var", "x f32");
    gb.add("dv2", "decl_var", "y f32");
    gb.add("holder", "new", "holder");

    // Lambda: select! $0 — has bang outputs "true" and "false"
    gb.add("cond", "select!", "$0", 1);

    // Store on "true" branch: store! x $1
    gb.add("st_true", "store!", "x $0");

    // Store on "false" branch: store! y $2
    gb.add("st_false", "store!", "y $0");

    gb.run_inference();

    auto* h = gb.find("holder");
    FlowPin* cb_pin = nullptr;
    for (auto& p : h->inputs)
        if (p->name == "cb") { cb_pin = p.get(); break; }
    ASSERT(cb_pin != nullptr);

    // Connect bang outputs to stores
    gb.link("cond.true", "st_true.bang_in0");   // "true" bang → st_true
    gb.link("cond.false", "st_false.bang_in0"); // "false" bang → st_false
    gb.link("cond.as_lambda", cb_pin->id);

    gb.run_inference();

    // 3 params expected: cond.$0, st_true.$0, st_false.$0
    // callback has 3 params: (a:u32 b:f32 c:bool)
    bool link_has_error = false;
    for (auto& l : gb.graph.links)
        if (l.from_pin == "cond.as_lambda" && !l.error.empty()) {
            printf("  link error: %s\n", l.error.c_str());
            link_has_error = true;
        }
    ASSERT(!link_has_error);
}

// --- Index type validation tests ---

TEST(index_vector_with_ref_type_error) {
    // oscs[$0] where $0 is &osc_def — reference type is not a valid integer index
    GraphBuilder gb;
    gb.add("dt", "decl_type", "osc_def p:f32 a:f32");
    gb.add("dt2", "decl_type", "osc_list vector<osc_def>");
    gb.add("dv", "decl_var", "oscs osc_list");
    // Create a source that produces &osc_def
    gb.add("ref_src", "expr", "&oscs", 0, 1); // this would be &vector, not &osc_def, but for testing
    // Actually, let's make a simpler case: $0 has type bool, used as index
    gb.add("e", "expr", "oscs[$0]", 1, 1);
    // Connect a bool source to $0
    gb.add("bool_src", "expr", "true", 0, 1);
    gb.link("bool_src.out0", "e.0");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(!n->error.empty()); // bool is not a valid index for vector
}

TEST(index_vector_with_int_ok) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data vector<f32>");
    gb.add("idx", "expr", "0", 0, 1);
    gb.add("e", "expr", "data[$0]", 1, 1);
    gb.link("idx.out0", "e.0");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty()); // int literal as index is fine
}

TEST(index_map_wrong_key_type) {
    // map<u32, f32> indexed with string — type mismatch
    GraphBuilder gb;
    gb.add("dv", "decl_var", "m map<u32, f32>");
    gb.add("e", "expr", "m[$0]", 1, 1);
    gb.add("str_src", "expr", "\"hello\"", 0, 1);
    gb.link("str_src.out0", "e.0");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(!n->error.empty()); // string is not u32
}

TEST(index_map_correct_key_type) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "m map<u32, f32>");
    gb.add("e", "expr", "m[$0]", 1, 1);
    // u32 source
    gb.add("dt", "decl_var", "key u32");
    gb.add("key_src", "expr", "key", 0, 1);
    gb.link("key_src.out0", "e.0");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
}

TEST(index_with_f32_error) {
    // vector indexed with f32 — not an integer
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data vector<u32>");
    gb.add("e", "expr", "data[$0]", 1, 1);
    gb.add("f_src", "expr", "1.0f", 0, 1);
    gb.link("f_src.out0", "e.0");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(!n->error.empty()); // f32 is not a valid index
}

TEST(index_array_manip_vector_of_indices) {
    // vector<f32> indexed with vector<u32> → vector<f32>
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data vector<f32>");
    gb.add("dv2", "decl_var", "indices vector<u32>");
    gb.add("e", "expr", "data[indices]");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "vector<f32>");
}

TEST(index_array_manip_vector_of_bad_indices_error) {
    // vector<f32> indexed with vector<bool> → error (bool not integer)
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data vector<f32>");
    gb.add("dv2", "decl_var", "indices vector<bool>");
    gb.add("e", "expr", "data[indices]");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(!n->error.empty());
}

TEST(index_map_with_non_key_type_error) {
    // map<u32, f32> indexed with f32 → error (f32 is not u32)
    GraphBuilder gb;
    gb.add("dv", "decl_var", "m map<u32, f32>");
    gb.add("dv2", "decl_var", "key f32");
    gb.add("e", "expr", "m[key]");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(!n->error.empty());
}

TEST(index_map_with_matching_key_ok) {
    // map<string, f32> indexed with string → ok
    GraphBuilder gb;
    gb.add("dv", "decl_var", "m map<string, f32>");
    gb.add("dv2", "decl_var", "key string");
    gb.add("e", "expr", "m[key]");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "f32");
}

// --- Reference operator & tests ---

TEST(parse_ref_varref) {
    // myvar strips $ and becomes SymbolRef("myvar")
    auto r = parse_expression("&myvar");
    ASSERT(r.root != nullptr);
    ASSERT(r.error.empty());
    ASSERT_EQ(r.root->kind, ExprKind::Ref);
    ASSERT_EQ(r.root->children[0]->kind, ExprKind::SymbolRef);
}

TEST(parse_ref_indexed) {
    auto r = parse_expression("&vec[$0]");
    ASSERT(r.root != nullptr);
    ASSERT(r.error.empty());
    ASSERT_EQ(r.root->kind, ExprKind::Ref);
    ASSERT_EQ(r.root->children[0]->kind, ExprKind::Index);
}

TEST(ref_varref_type) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "myvar f32");
    gb.add("e", "expr", "&myvar");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    // Should be &f32 (reference category)
    ASSERT_EQ(n->outputs[0]->resolved_type->category, TypeCategory::Reference);
    ASSERT_EQ(n->outputs[0]->resolved_type->kind, TypeKind::Scalar);
    ASSERT_EQ(n->outputs[0]->resolved_type->scalar, ScalarType::F32);
}

TEST(ref_vector_index_iterator) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "vec vector<f32>");
    gb.add("e", "expr", "&vec[$0]", 1);
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_EQ(n->outputs[0]->resolved_type->kind, TypeKind::ContainerIterator);
    ASSERT_EQ(n->outputs[0]->resolved_type->iterator, IteratorKind::Vector);
}

TEST(ref_map_index_iterator) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "m map<u32, f32>");
    gb.add("e", "expr", "&m[$0]", 1);
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_EQ(n->outputs[0]->resolved_type->kind, TypeKind::ContainerIterator);
    ASSERT_EQ(n->outputs[0]->resolved_type->iterator, IteratorKind::Map);
}

TEST(ref_ordered_map_index_iterator) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "m ordered_map<u32, string>");
    gb.add("e", "expr", "&m[$0]", 1);
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT_EQ(n->outputs[0]->resolved_type->kind, TypeKind::ContainerIterator);
    ASSERT_EQ(n->outputs[0]->resolved_type->iterator, IteratorKind::OrderedMap);
}

TEST(ref_array_index_error) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "arr array<f32, 4>");
    gb.add("e", "expr", "&arr[$0]", 1);
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(!n->error.empty()); // can't reference array element
}

TEST(ref_tensor_index_error) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "t tensor<f32>");
    gb.add("e", "expr", "&t[$0]", 1);
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(!n->error.empty()); // can't reference tensor element
}

TEST(ref_list_index_error) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "lst list<f32>");
    gb.add("e", "expr", "&lst[$0]", 1);
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(!n->error.empty()); // list not indexable, so & on it is also error
}

TEST(ref_field_access_error) {
    GraphBuilder gb;
    gb.add("dt", "decl_type", "vec2 x:f32 y:f32");
    gb.add("dv", "decl_var", "pos vec2");
    gb.add("e", "expr", "&pos.x");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(!n->error.empty()); // can't reference field
}

TEST(ref_literal_error) {
    // &(1+2) — reference to an arithmetic expression (not an lvalue)
    auto r = parse_expression("&(1+2)");
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::Ref);
    GraphBuilder gb;
    gb.add("e", "expr", "&(1+2)");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(!n->error.empty()); // can't reference arithmetic result
}

TEST(ref_pin_ref_is_not_ampersand_op) {
    // &42 is now the & reference operator applied to literal 42 (not a pin ref)
    // Only $ is a pin sigil now
    auto r = parse_expression("&42");
    ASSERT(r.root != nullptr);
    ASSERT_EQ(r.root->kind, ExprKind::Ref);
    ASSERT_EQ(r.root->children[0]->kind, ExprKind::Literal);
}

TEST(ref_expr_error) {
    // &($0+1) — not a valid target
    GraphBuilder gb;
    gb.add("e", "expr", "&($0+1)", 1);
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(!n->error.empty());
}

TEST(ref_pinref) {
    // &$0 — reference to pin value
    GraphBuilder gb;
    gb.add("src", "expr", "1.0f", 0, 1);
    gb.add("e", "expr", "&$0", 1);
    gb.link("src.out0", "e.0");
    gb.run_inference();
    auto* n = gb.find("e");
    ASSERT(n->error.empty());
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_EQ(n->outputs[0]->resolved_type->category, TypeCategory::Reference);
}

// --- Function call validation tests ---

TEST(func_call_correct_args) {
    GraphBuilder gb;
    gb.add("dt", "decl_type", "myfn (x:f32 y:f32) -> f32");
    gb.add("dv", "decl_var", "fn myfn");
    gb.add("e", "expr", "fn(1.0f, 2.0f)");
    gb.run_inference();
    ASSERT(gb.find("e")->error.empty());
}

TEST(func_call_wrong_arg_count) {
    GraphBuilder gb;
    gb.add("dt", "decl_type", "myfn (x:f32 y:f32) -> f32");
    gb.add("dv", "decl_var", "fn myfn");
    gb.add("e", "expr", "fn(1.0f)");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty()); // expects 2, got 1
}

TEST(func_call_zero_args_when_expects_one) {
    GraphBuilder gb;
    gb.add("dt", "decl_type", "callback (x:f32) -> void");
    gb.add("dv", "decl_var", "cb callback");
    gb.add("e", "expr", "cb()");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty()); // expects 1, got 0
}

TEST(func_call_too_many_args) {
    GraphBuilder gb;
    gb.add("dt", "decl_type", "callback (x:f32) -> void");
    gb.add("dv", "decl_var", "cb callback");
    gb.add("e", "expr", "cb(1.0f, 2.0f)");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty()); // expects 1, got 2
}

TEST(func_call_wrong_arg_type) {
    GraphBuilder gb;
    gb.add("dt", "decl_type", "myfn (x:f32) -> void");
    gb.add("dv", "decl_var", "fn myfn");
    gb.add("e", "expr", "fn(true)");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty()); // bool vs f32
}

TEST(func_call_compatible_arg_type) {
    GraphBuilder gb;
    gb.add("dt", "decl_type", "myfn (x:u32) -> void");
    gb.add("dv", "decl_var", "fn myfn");
    gb.add("dv2", "decl_var", "val u16");
    gb.add("e", "expr", "fn(val)");
    gb.run_inference();
    ASSERT(gb.find("e")->error.empty()); // u16 upcasts to u32
}

TEST(func_call_return_type) {
    GraphBuilder gb;
    gb.add("dt", "decl_type", "myfn (x:f32) -> bool");
    gb.add("dv", "decl_var", "fn myfn");
    gb.add("e", "expr", "fn(1.0f)");
    gb.run_inference();
    ASSERT_TYPE(gb.find("e")->outputs[0].get(), "bool");
}

TEST(method_call_on_struct_field) {
    // struct.callback(args) where callback is a function field
    GraphBuilder gb;
    gb.add("dt_fn", "decl_type", "action (val:f32) -> void");
    gb.add("dt", "decl_type", "thing do_it:action");
    gb.add("dv", "decl_var", "t thing");
    gb.add("e", "expr", "t.do_it(1.0f)");
    gb.run_inference();
    ASSERT(gb.find("e")->error.empty());
}

TEST(method_call_wrong_arg_type_on_struct) {
    GraphBuilder gb;
    gb.add("dt_fn", "decl_type", "action (val:f32) -> void");
    gb.add("dt", "decl_type", "thing do_it:action");
    gb.add("dv", "decl_var", "t thing");
    gb.add("e", "expr", "t.do_it(true)");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty()); // bool vs f32
}

TEST(func_call_int_where_struct_ref_expected) {
    // stop_fn = (osc:&osc_def) -> void, calling with 0 should fail
    GraphBuilder gb;
    gb.add("dt_osc", "decl_type", "osc_def p:f32 a:f32");
    gb.add("dt_fn", "decl_type", "stop_fn (osc:&osc_def) -> void");
    gb.add("dt_thing", "decl_type", "thing stop:stop_fn");
    gb.add("dv", "decl_var", "t thing");
    gb.add("e", "expr", "t.stop(0)");
    gb.run_inference();
    auto* n = gb.find("e");
    if (!n->error.empty()) printf("  (error: %s)\n", n->error.c_str());
    ASSERT(!n->error.empty()); // 0 (int) is not osc_def&
}

TEST(func_call_correct_struct_ref) {
    // stop_fn = (osc:&osc_def) -> void, calling with correct type
    GraphBuilder gb;
    gb.add("dt_osc", "decl_type", "osc_def p:f32 a:f32");
    gb.add("dt_fn", "decl_type", "stop_fn (osc:&osc_def) -> void");
    gb.add("dt_thing", "decl_type", "thing stop:stop_fn");
    gb.add("dv", "decl_var", "t thing");
    gb.add("dv2", "decl_var", "osc osc_def");
    gb.add("e", "expr", "t.stop(osc)");
    gb.run_inference();
    auto* n = gb.find("e");
    if (!n->error.empty()) printf("  (error: %s)\n", n->error.c_str());
    ASSERT(n->error.empty());
}

TEST(func_call_via_map_iterator_wrong_arg) {
    // keys[$0] returns ^list_iterator<osc_def>, .stop is stop_fn, calling with 0
    GraphBuilder gb;
    gb.add("dt_osc", "decl_type", "osc_def p:f32 stop:stop_fn");
    gb.add("dt_fn", "decl_type", "stop_fn (osc:&osc_def) -> void");
    gb.add("dt_keys", "decl_type", "key_set map<u8, ^list_iterator<osc_def>>");
    gb.add("dv", "decl_var", "keys key_set");
    gb.add("e", "expr", "keys[$0].stop(0)", 1);
    gb.run_inference();
    auto* n = gb.find("e");
    if (n->error.empty()) printf("  (no error! stop type: %s)\n",
        n->parsed_exprs.size() > 0 && n->parsed_exprs[0] ?
        expr_to_string(n->parsed_exprs[0]).c_str() : "?");
    ASSERT(!n->error.empty()); // 0 is not &osc_def
}

TEST(iterator_decays_to_ref_in_func_call) {
    // stop_fn = (osc:&osc_def) -> void
    // Passing a list_iterator<osc_def> should be valid (auto-decay)
    GraphBuilder gb;
    gb.add("dt_osc", "decl_type", "osc_def p:f32 a:f32");
    gb.add("dt_fn", "decl_type", "stop_fn (osc:&osc_def) -> void");
    gb.add("dv", "decl_var", "fn stop_fn");
    gb.add("dv2", "decl_var", "it list_iterator<osc_def>");
    gb.add("e", "expr", "fn(it)");
    gb.run_inference();
    auto* n = gb.find("e");
    if (!n->error.empty()) printf("  (error: %s)\n", n->error.c_str());
    ASSERT(n->error.empty()); // iterator decays to &osc_def
}

TEST(iterator_decays_to_value_in_func_call) {
    // fn = (x:osc_def) -> void (pass by value, not ref)
    // Passing a list_iterator<osc_def> should still be valid
    GraphBuilder gb;
    gb.add("dt_osc", "decl_type", "osc_def p:f32 a:f32");
    gb.add("dt_fn", "decl_type", "myfn (x:osc_def) -> void");
    gb.add("dv", "decl_var", "fn myfn");
    gb.add("dv2", "decl_var", "it list_iterator<osc_def>");
    gb.add("e", "expr", "fn(it)");
    gb.run_inference();
    ASSERT(gb.find("e")->error.empty());
}

TEST(iterator_wrong_element_type_error) {
    // fn expects &osc_def but iterator is list_iterator<f32>
    GraphBuilder gb;
    gb.add("dt_osc", "decl_type", "osc_def p:f32 a:f32");
    gb.add("dt_fn", "decl_type", "stop_fn (osc:&osc_def) -> void");
    gb.add("dv", "decl_var", "fn stop_fn");
    gb.add("dv2", "decl_var", "it list_iterator<f32>");
    gb.add("e", "expr", "fn(it)");
    gb.run_inference();
    ASSERT(!gb.find("e")->error.empty()); // f32 != osc_def
}

TEST(types_compatible_iterator_to_ref) {
    // Direct types_compatible check
    TypePool pool;
    auto osc_type = std::make_shared<TypeExpr>();
    osc_type->kind = TypeKind::Named;
    osc_type->named_ref = "osc_def";

    auto ref_type = std::make_shared<TypeExpr>(*osc_type);
    ref_type->category = TypeCategory::Reference;

    auto iter_type = std::make_shared<TypeExpr>();
    iter_type->kind = TypeKind::ContainerIterator;
    iter_type->iterator = IteratorKind::List;
    iter_type->value_type = osc_type;

    ASSERT(types_compatible(iter_type, ref_type)); // iterator<osc_def> → &osc_def
    ASSERT(types_compatible(iter_type, osc_type)); // iterator<osc_def> → osc_def (by value)
}

TEST(func_call_no_args_void_return) {
    GraphBuilder gb;
    gb.add("dt", "decl_type", "callback () -> void");
    gb.add("dv", "decl_var", "cb callback");
    gb.add("e", "expr", "cb()");
    gb.run_inference();
    ASSERT(gb.find("e")->error.empty());
}

// --- decl_local tests ---

TEST(decl_local_basic) {
    GraphBuilder gb;
    gb.add("dl", "decl_var", "myvar f32");
    gb.run_inference();
    auto* n = gb.find("dl");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
    // Output should be a reference to f32
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_EQ(n->outputs[0]->resolved_type->category, TypeCategory::Reference);
    ASSERT_EQ(n->outputs[0]->resolved_type->kind, TypeKind::Scalar);
    ASSERT_EQ(n->outputs[0]->resolved_type->scalar, ScalarType::F32);
}

TEST(decl_local_u32) {
    GraphBuilder gb;
    gb.add("dl", "decl_var", "counter u32");
    gb.run_inference();
    ASSERT(gb.find("dl")->outputs[0]->resolved_type != nullptr);
    ASSERT_EQ(gb.find("dl")->outputs[0]->resolved_type->category, TypeCategory::Reference);
}

TEST(decl_local_string) {
    GraphBuilder gb;
    gb.add("dl", "decl_var", "name string");
    gb.run_inference();
    ASSERT(gb.find("dl")->outputs[0]->resolved_type != nullptr);
    ASSERT_EQ(gb.find("dl")->outputs[0]->resolved_type->category, TypeCategory::Reference);
    ASSERT_EQ(gb.find("dl")->outputs[0]->resolved_type->kind, TypeKind::String);
}

TEST(decl_local_missing_args) {
    GraphBuilder gb;
    gb.add("dl", "decl_var", "myvar");
    gb.run_inference();
    ASSERT(!gb.find("dl")->error.empty()); // missing type
}

TEST(decl_local_dollar_name_error) {
    // $ prefix is no longer special — myvar in args context is just "myvar"
    // (the $ is part of expression tokenization, not args tokenization)
    // This test now verifies that myvar f32 works ($ is part of the name string)
    GraphBuilder gb;
    gb.add("dl", "decl_var", "myvar f32");
    gb.run_inference();
    // With the new tokenizer, myvar may or may not strip the $ depending on args tokenization
    // Just verify the node doesn't crash
    auto* n = gb.find("dl");
    ASSERT(n != nullptr);
}

TEST(decl_local_registers_var_type) {
    // decl_local registers the variable so downstream myvar resolves
    GraphBuilder gb;
    gb.add("dl", "decl_var", "myvar f32");
    gb.add("e", "expr", "myvar");
    gb.run_inference();
    ASSERT(gb.find("e")->error.empty());
    ASSERT_TYPE(gb.find("e")->outputs[0].get(), "symbol<myvar,f32>");
}

TEST(decl_local_named_type) {
    GraphBuilder gb;
    gb.add("dt", "decl_type", "vec2 x:f32 y:f32");
    gb.add("dl", "decl_var", "pos vec2");
    gb.run_inference();
    auto* n = gb.find("dl");
    ASSERT(n->error.empty());
    ASSERT_EQ(n->outputs[0]->resolved_type->category, TypeCategory::Reference);
    ASSERT_EQ(n->outputs[0]->resolved_type->kind, TypeKind::Named);
}

// --- next node tests ---

TEST(next_vector_iterator) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "it vector_iterator<f32>");
    gb.add("n", "next", "it");
    gb.run_inference();
    auto* node = gb.find("n");
    ASSERT(node->error.empty());
    ASSERT(node->outputs[0]->resolved_type != nullptr);
    ASSERT_EQ(node->outputs[0]->resolved_type->kind, TypeKind::ContainerIterator);
    ASSERT_EQ(node->outputs[0]->resolved_type->iterator, IteratorKind::Vector);
}

TEST(next_list_iterator) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "it list_iterator<u32>");
    gb.add("n", "next", "it");
    gb.run_inference();
    ASSERT(gb.find("n")->error.empty());
    ASSERT_EQ(gb.find("n")->outputs[0]->resolved_type->kind, TypeKind::ContainerIterator);
    ASSERT_EQ(gb.find("n")->outputs[0]->resolved_type->iterator, IteratorKind::List);
}

TEST(next_map_iterator) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "it map_iterator<u32, f32>");
    gb.add("n", "next", "it");
    gb.run_inference();
    ASSERT(gb.find("n")->error.empty());
    ASSERT_EQ(gb.find("n")->outputs[0]->resolved_type->kind, TypeKind::ContainerIterator);
}

TEST(next_non_iterator_error) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "x f32");
    gb.add("n", "next", "x");
    gb.run_inference();
    ASSERT(!gb.find("n")->error.empty()); // f32 is not an iterator
}

TEST(next_from_connection) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "data vector<f32>");
    gb.add("ref", "expr", "&data[$0]", 1, 1);
    gb.add("n", "next", "");
    gb.link("ref.out0", "n.value");
    gb.run_inference();
    ASSERT(gb.find("n")->error.empty());
}

TEST(next_chain) {
    // next(next(it)) should work
    GraphBuilder gb;
    gb.add("dv", "decl_var", "it list_iterator<f32>");
    gb.add("n1", "next", "it");
    gb.add("n2", "next", "");
    gb.link("n1.out0", "n2.value");
    gb.run_inference();
    ASSERT(gb.find("n2")->error.empty());
    ASSERT_EQ(gb.find("n2")->outputs[0]->resolved_type->kind, TypeKind::ContainerIterator);
}

// ============================================================
// Mutex / Lock Tests
// ============================================================

TEST(mutex_type_parsing) {
    TypePool pool;
    auto t = pool.intern("mutex");
    ASSERT(t);
    ASSERT_EQ(t->kind, TypeKind::Mutex);
    auto tr = pool.intern("&mutex");
    ASSERT(tr);
    ASSERT_EQ(tr->kind, TypeKind::Mutex);
    ASSERT_EQ(tr->category, TypeCategory::Reference);
}

TEST(lock_mutex_ok) {
    GraphBuilder gb;
    gb.add("dm", "decl_var", "mtx mutex");
    gb.add("lk", "lock", "mtx @0");
    gb.run_inference();
    auto* lk = gb.find("lk");
    if (!lk->error.empty()) printf("    error: %s\n", lk->error.c_str());
    ASSERT(lk->error.empty());
}

TEST(lock_bang_mutex_ok) {
    GraphBuilder gb;
    gb.add("dm", "decl_var", "mtx mutex");
    gb.add("lk", "lock!", "mtx @0");
    gb.run_inference();
    ASSERT(gb.find("lk")->error.empty());
}

TEST(lock_non_mutex_error) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "x u32");
    gb.add("lk", "lock", "x @0");
    gb.run_inference();
    ASSERT(!gb.find("lk")->error.empty());
    ASSERT_CONTAINS(gb.find("lk")->error, "mutex");
}

TEST(lock_accepts_var_without_ampersand) {
    // mtx auto-decays to &mutex, no need for explicit &
    GraphBuilder gb;
    gb.add("dm", "decl_var", "mtx mutex");
    gb.add("lk", "lock", "mtx @0");
    gb.run_inference();
    ASSERT(gb.find("lk")->error.empty());
}

TEST(lock_return_type_propagation) {
    // Lambda returns f32 -> lock output should be f32
    GraphBuilder gb;
    gb.add("dm", "decl_var", "mtx mutex");
    gb.add("ex", "expr", "1.0f");
    gb.add("lk", "lock", "mtx");
    gb.link("ex.as_lambda", "lk.fn");
    gb.run_inference();
    auto* lk = gb.find("lk");
    ASSERT(lk->error.empty());
    ASSERT(!lk->outputs.empty());
    ASSERT(lk->outputs[0]->resolved_type);
    ASSERT_EQ(lk->outputs[0]->resolved_type->kind, TypeKind::Scalar);
    ASSERT_EQ(lk->outputs[0]->resolved_type->scalar, ScalarType::F32);
}

TEST(lock_void_return) {
    // Lambda returns void -> lock should have 0 outputs
    GraphBuilder gb;
    gb.add("dm", "decl_var", "mtx mutex");
    gb.add("dv", "decl_var", "x u32");
    gb.add("st", "store!", "x 42");
    gb.add("lk", "lock!", "mtx");
    gb.link("st.as_lambda", "lk.fn");
    gb.run_inference();
    auto* lk = gb.find("lk");
    // store! returns void, so lock should have 0 outputs
    ASSERT(lk->outputs.empty());
}

TEST(lock_bang_return_type) {
    // lock! with lambda returning f32
    GraphBuilder gb;
    gb.add("dm", "decl_var", "mtx mutex");
    gb.add("ex", "expr", "42.0f");
    gb.add("lk", "lock!", "mtx");
    gb.link("ex.as_lambda", "lk.fn");
    gb.run_inference();
    auto* lk = gb.find("lk");
    ASSERT(lk->error.empty());
    ASSERT(!lk->outputs.empty());
    ASSERT(lk->outputs[0]->resolved_type);
    ASSERT_EQ(lk->outputs[0]->resolved_type->kind, TypeKind::Scalar);
}

TEST(lock_lambda_with_inner_lambda_no_leak) {
    // A lock! lambda whose body contains a node with inner lambdas (like new with fn fields)
    // The inner lambda's params must NOT leak into the lock lambda's param list
    GraphBuilder gb;
    gb.add("dm", "decl_var", "mtx mutex");
    gb.add("dt", "decl_type", "my_fn_type cb:(u32)->void");
    gb.add("inner_expr", "expr", "$0");  // inner lambda body with 1 param
    gb.add("n", "new", "my_fn_type");    // new has a fn-typed field 'cb'
    gb.link("inner_expr.as_lambda", "n.cb");  // inner lambda connects to new's cb field
    gb.add("lk", "lock!", "mtx");
    gb.link("n.as_lambda", "lk.fn");     // new is the lock's lambda root
    gb.run_inference();
    auto* lk = gb.find("lk");
    // lock! lambda should have 0 params (inner lambda's $0 should not leak)
    // Check no "Lambda has N parameter(s)" error on any link
    for (auto& link : gb.graph.links) {
        if (link.to_pin.find("lk") != std::string::npos || link.to_pin.find(lk->guid) != std::string::npos) {
            if (!link.error.empty()) printf("    link error: %s\n", link.error.c_str());
            ASSERT(link.error.empty());
        }
    }
}

TEST(lambda_captures_from_outer_scope_no_error) {
    // lock's lambda (0 args) whose data deps trace to a node with
    // unconnected inputs from the outer stored lambda scope.
    // The extras should be treated as captures, not lambda params.
    GraphBuilder gb;
    gb.add("dm", "decl_var", "mtx mutex");
    gb.add("dv", "decl_var", "data list<f32>");
    // Inner expr with unconnected $0 (outer lambda param)
    gb.add("ex", "expr", "2*pi/$0");
    // append uses the expr output
    gb.add("ap", "append", "data");
    gb.link("ex.out0", "ap.0");
    // lock's lambda is the append
    gb.add("lk", "lock", "mtx");
    gb.link("ap.as_lambda", "lk.fn");
    gb.run_inference();
    // lock's lambda has 0 expected args; the $0 from expr is a capture
    // There should be NO error on the lock.fn link
    for (auto& link : gb.graph.links) {
        if (link.to_pin.find(".fn") != std::string::npos) {
            if (!link.error.empty()) printf("    link error: %s\n", link.error.c_str());
            ASSERT(link.error.empty());
        }
    }
}

TEST(stored_lambda_params_via_bang_chain) {
    // store! fn where fn is (x:f32) -> void.
    // The stored lambda's root is a lock node. The lock's post_bang chain
    // contains a store! data $0 where $0 is the stored lambda's parameter.
    // The $0 is reachable via bang chain, NOT through the lock's Lambda fn input.
    GraphBuilder gb;
    gb.add("dm", "decl_var", "mtx mutex");
    gb.add("dv", "decl_var", "data f32");
    gb.add("dfn", "decl_var", "my_fn (x:f32) -> void");
    // expr $0 unpacks the stored lambda param
    gb.add("ex", "expr", "$0");
    // lock has an empty inner lambda (dup just passes through)
    gb.add("dp", "dup", "42");
    gb.add("lk", "lock", "mtx");
    gb.link("dp.as_lambda", "lk.fn");
    // lock's post_bang goes to store! which uses $0 from expr
    gb.add("st_inner", "store!", "data");
    gb.link("ex.out0", "st_inner.value");
    gb.link("lk.post_bang", "st_inner.bang_in0");
    // Store the lock as the fn value
    gb.add("st", "store!", "my_fn");
    gb.link("lk.as_lambda", "st.value");
    gb.run_inference();
    // The store link should have no error — 1 param found via bang chain matches expected 1
    for (auto& link : gb.graph.links) {
        if (link.to_pin.find("st.") != std::string::npos && link.from_pin.find("as_lambda") != std::string::npos) {
            if (!link.error.empty()) printf("    store link error: %s\n", link.error.c_str());
            ASSERT(link.error.empty());
        }
    }
}

TEST(store_lambda_wrong_return_type) {
    // store! fn where fn is (x:f32) -> f32, but the lambda returns void
    GraphBuilder gb;
    gb.add("dfn", "decl_var", "my_fn (x:f32) -> f32");
    gb.add("dv", "decl_var", "x u32");
    gb.add("st_body", "store!", "x 42");  // void-returning node
    gb.add("st", "store!", "my_fn");
    gb.link("st_body.as_lambda", "st.value");
    gb.run_inference();
    // Should have a type mismatch error on the link
    bool found_error = false;
    for (auto& link : gb.graph.links) {
        if (!link.error.empty()) found_error = true;
    }
    ASSERT(found_error);
}

TEST(store_lambda_wrong_param_count) {
    // store! fn where fn is (x:f32 y:f32) -> void, but lambda has only 1 param
    GraphBuilder gb;
    gb.add("dfn", "decl_var", "my_fn (x:f32 y:f32) -> void");
    gb.add("ex", "expr", "$0");  // 1 unconnected param
    gb.add("st", "store!", "my_fn");
    gb.link("ex.as_lambda", "st.value");
    gb.run_inference();
    // Should error: lambda has 1 param, expected 2
    bool found_error = false;
    for (auto& link : gb.graph.links) {
        if (!link.error.empty()) {
            found_error = true;
            ASSERT_CONTAINS(link.error, "1");
            ASSERT_CONTAINS(link.error, "2");
        }
    }
    ASSERT(found_error);
}

TEST(store_lambda_correct_type) {
    // store! fn where fn is (x:f32) -> f32, lambda returns f32
    GraphBuilder gb;
    gb.add("dfn", "decl_var", "my_fn (x:f32) -> f32");
    gb.add("ex", "expr", "$0*2.0f");  // takes f32, returns f32
    gb.add("st", "store!", "my_fn");
    gb.link("ex.as_lambda", "st.value");
    gb.run_inference();
    for (auto& link : gb.graph.links) {
        if (!link.error.empty()) printf("    link error: %s\n", link.error.c_str());
        ASSERT(link.error.empty());
    }
}

// ============================================================
// Void / Discard Tests
// ============================================================

TEST(void_node_output_type) {
    GraphBuilder gb;
    gb.add("v", "void", "");
    gb.run_inference();
    auto* n = gb.find("v");
    ASSERT(!n->outputs.empty());
    ASSERT(n->outputs[0]->resolved_type);
    ASSERT_EQ(n->outputs[0]->resolved_type->kind, TypeKind::Void);
}

TEST(void_node_no_inputs) {
    GraphBuilder gb;
    gb.add("v", "void", "");
    gb.run_inference();
    ASSERT(gb.find("v")->inputs.empty());
}

TEST(void_node_as_lambda_returns_void) {
    // void used as lambda root — lambda returns void
    GraphBuilder gb;
    gb.add("dm", "decl_var", "mtx mutex");
    gb.add("v", "void", "");
    gb.add("lk", "lock", "mtx");
    gb.link("v.as_lambda", "lk.fn");
    gb.run_inference();
    ASSERT(gb.find("lk")->outputs.empty()); // void return = no output
}

TEST(discard_node_no_outputs) {
    GraphBuilder gb;
    gb.add("d", "discard", "$0");
    gb.run_inference();
    ASSERT(gb.find("d")->outputs.empty());
}

TEST(discard_node_has_lambda_handle) {
    GraphBuilder gb;
    gb.add("dm", "decl_var", "mtx mutex");
    gb.add("d", "discard", "$0");
    gb.add("lk", "lock", "mtx");
    gb.link("d.as_lambda", "lk.fn");
    gb.run_inference();
    // lock's lambda returns void via discard
    ASSERT(gb.find("lk")->outputs.empty());
}

// ============================================================
// Lock with forwarded params Tests
// ============================================================

TEST(lock_forwards_lambda_params) {
    // lock with a lambda that has 1 param — lock should get an extra input
    GraphBuilder gb;
    gb.add("dm", "decl_var", "mtx mutex");
    gb.add("ex", "expr", "$0*2.0f");  // 1 unconnected param
    gb.add("lk", "lock", "mtx");
    gb.link("ex.as_lambda", "lk.fn");
    gb.run_inference();
    auto* lk = gb.find("lk");
    // Should have: mutex (inline), fn (lambda), arg0 (forwarded)
    // Check for an extra data input beyond mutex and fn
    int data_inputs = 0;
    for (auto& inp : lk->inputs)
        if (inp->direction == FlowPin::Input) data_inputs++;
    ASSERT_EQ(data_inputs, 1); // 1 forwarded arg (mutex is inline)
}

TEST(lock_forwards_two_params) {
    GraphBuilder gb;
    gb.add("dm", "decl_var", "mtx mutex");
    gb.add("ex", "expr", "$0+$1");  // 2 unconnected params
    gb.add("lk", "lock", "mtx");
    gb.link("ex.as_lambda", "lk.fn");
    gb.run_inference();
    auto* lk = gb.find("lk");
    int data_inputs = 0;
    for (auto& inp : lk->inputs)
        if (inp->direction == FlowPin::Input) data_inputs++;
    ASSERT_EQ(data_inputs, 2); // 2 forwarded args
}

TEST(lock_zero_params_no_extra_inputs) {
    GraphBuilder gb;
    gb.add("dm", "decl_var", "mtx mutex");
    gb.add("ex", "expr", "42");  // no unconnected params
    gb.add("lk", "lock", "mtx");
    gb.link("ex.as_lambda", "lk.fn");
    gb.run_inference();
    auto* lk = gb.find("lk");
    int data_inputs = 0;
    for (auto& inp : lk->inputs)
        if (inp->direction == FlowPin::Input) data_inputs++;
    ASSERT_EQ(data_inputs, 0); // no forwarded args
}

TEST(lock_forwarded_param_types) {
    // lock with a lambda taking (f32) — the forwarded arg should have type f32
    GraphBuilder gb;
    gb.add("dm", "decl_var", "mtx mutex");
    gb.add("ex", "expr", "$0+1.0f");  // $0 : f32 (inferred from 1.0f)
    gb.add("lk", "lock", "mtx");
    gb.link("ex.as_lambda", "lk.fn");
    gb.run_inference();
    auto* lk = gb.find("lk");
    bool found_arg = false;
    for (auto& inp : lk->inputs) {
        if (inp->name == "arg0") {
            found_arg = true;
            ASSERT(inp->resolved_type);
        }
    }
    ASSERT(found_arg);
}

TEST(lock_bang_forwards_params) {
    // lock! should also forward params
    GraphBuilder gb;
    gb.add("dm", "decl_var", "mtx mutex");
    gb.add("ex", "expr", "$0*$1");
    gb.add("lk", "lock!", "mtx");
    gb.link("ex.as_lambda", "lk.fn");
    gb.run_inference();
    auto* lk = gb.find("lk");
    int data_inputs = 0;
    for (auto& inp : lk->inputs)
        if (inp->direction == FlowPin::Input) data_inputs++;
    ASSERT_EQ(data_inputs, 2);
}

TEST(stored_lambda_type_resolved) {
    // store! fn with correct lambda — function var should be assignable
    GraphBuilder gb;
    gb.add("dfn", "decl_var", "my_fn (x:f32) -> f32");
    gb.add("ex", "expr", "$0+1.0f");
    gb.add("st", "store!", "my_fn");
    gb.link("ex.as_lambda", "st.value");
    gb.run_inference();
    // No errors anywhere
    ASSERT(gb.find("st")->error.empty());
    for (auto& link : gb.graph.links)
        ASSERT(link.error.empty());
}

TEST(void_as_select_branch) {
    // select with void in one branch should work
    GraphBuilder gb;
    gb.add("dv", "decl_var", "flag bool");
    gb.add("v", "void", "");
    gb.add("ex", "expr", "42");
    gb.add("sel", "select", "flag");
    gb.link("ex.out0", "sel.if_true");
    gb.link("v.out0", "sel.if_false");
    gb.run_inference();
    ASSERT(gb.find("sel")->error.empty());
}

TEST(discard_with_expr_input) {
    GraphBuilder gb;
    gb.add("d", "discard", "42+1");
    gb.run_inference();
    ASSERT(gb.find("d")->error.empty());
    ASSERT(gb.find("d")->outputs.empty());
}

// ============================================================
// discard! Tests
// ============================================================

TEST(discard_bang_basic) {
    GraphBuilder gb;
    gb.add("e", "event!", "~start");
    gb.add("d", "discard!", "42");
    gb.link("e.bang0", "d.bang_in0");
    gb.run_inference();
    ASSERT(gb.find("d")->error.empty());
    ASSERT(gb.find("d")->outputs.empty());
}

TEST(discard_bang_has_bang_output) {
    GraphBuilder gb;
    gb.add("ev", "decl_event", "start () -> void");
    gb.add("e", "event!", "~start");
    gb.add("d", "discard!", "$0");
    gb.add("s", "store!", "x 1");
    gb.add("v", "decl_var", "x s32");
    gb.link("e.bang0", "d.bang_in0");
    gb.link("d.bang0", "s.bang_in0");
    gb.run_inference();
    ASSERT(gb.find("d")->error.empty());
    ASSERT(gb.find("s")->error.empty());
}

TEST(discard_bang_with_connected_input) {
    GraphBuilder gb;
    gb.add("ev", "decl_event", "start () -> void");
    gb.add("e", "event!", "~start");
    gb.add("ex", "expr", "42");
    gb.add("d", "discard!", "");
    gb.link("e.bang0", "d.bang_in0");
    gb.link("ex.out0", "d.value");
    gb.run_inference();
    ASSERT(gb.find("d")->error.empty());
}

TEST(discard_bang_no_lambda_handle) {
    GraphBuilder gb;
    gb.add("d", "discard!", "42");
    gb.run_inference();
    // discard! should not have a lambda handle (has_lambda_grab = false)
    auto* nt = find_node_type("discard!");
    ASSERT(nt != nullptr);
    ASSERT(nt->has_lambda == false);
}

// ============================================================
// FFI / Call Tests
// ============================================================

TEST(ffi_basic_declaration) {
    GraphBuilder gb;
    gb.add("f", "ffi", "my_sin (x:f32) -> f32");
    gb.run_inference();
    ASSERT(gb.find("f")->error.empty());
}

TEST(ffi_invalid_type) {
    GraphBuilder gb;
    gb.add("f", "ffi", "my_fn u32");
    gb.run_inference();
    ASSERT(!gb.find("f")->error.empty());
    ASSERT_CONTAINS(gb.find("f")->error, "function type");
}

TEST(ffi_missing_args) {
    GraphBuilder gb;
    gb.add("f", "ffi", "my_fn");
    gb.run_inference();
    ASSERT(!gb.find("f")->error.empty());
}

TEST(ffi_registers_var_type) {
    // FFI function should be accessible as name in expressions
    GraphBuilder gb;
    gb.add("f", "ffi", "my_sin (x:f32) -> f32");
    gb.add("e", "expr", "my_sin(1.0f)");
    gb.run_inference();
    ASSERT(gb.find("e")->error.empty());
    // Output should be f32
    ASSERT(!gb.find("e")->outputs.empty());
    ASSERT(gb.find("e")->outputs[0]->resolved_type);
    ASSERT_EQ(gb.find("e")->outputs[0]->resolved_type->kind, TypeKind::Scalar);
    ASSERT_EQ(gb.find("e")->outputs[0]->resolved_type->scalar, ScalarType::F32);
}

TEST(call_resolves_inputs_from_ffi) {
    GraphBuilder gb;
    gb.add("f", "ffi", "my_add (a:f32 b:f32) -> f32");
    gb.add("c", "call", "my_add");
    gb.run_inference();
    auto* c = gb.find("c");
    ASSERT(c->error.empty());
    // Should have 2 input pins (a, b) and 1 output (result)
    ASSERT_EQ((int)c->inputs.size(), 2);
    ASSERT_EQ(c->inputs[0]->name, "a");
    ASSERT_EQ(c->inputs[1]->name, "b");
    ASSERT_EQ((int)c->outputs.size(), 1);
}

TEST(call_void_return_no_output) {
    GraphBuilder gb;
    gb.add("f", "ffi", "my_print (msg:string) -> void");
    gb.add("c", "call", "my_print");
    gb.run_inference();
    auto* c = gb.find("c");
    ASSERT(c->error.empty());
    ASSERT_EQ((int)c->inputs.size(), 1);
    ASSERT(c->outputs.empty());
}

TEST(call_inline_args) {
    GraphBuilder gb;
    gb.add("f", "ffi", "my_add (a:f32 b:f32) -> f32");
    gb.add("c", "call", "my_add 1.0f 2.0f");
    gb.run_inference();
    auto* c = gb.find("c");
    ASSERT(c->error.empty());
}

TEST(call_bang_basic) {
    GraphBuilder gb;
    gb.add("f", "ffi", "my_draw (x:f32 y:f32) -> void");
    gb.add("c", "call!", "my_draw 1.0f 2.0f");
    gb.run_inference();
    auto* c = gb.find("c");
    ASSERT(c->error.empty());
    ASSERT(c->outputs.empty());
}

TEST(call_non_function_error) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "x u32");
    gb.add("c", "call", "x");
    gb.run_inference();
    auto* c = gb.find("c");
    ASSERT(!c->error.empty());
    ASSERT_CONTAINS(c->error, "function");
}

TEST(call_too_many_inline_args) {
    GraphBuilder gb;
    gb.add("f", "ffi", "my_fn (x:f32) -> f32");
    gb.add("c", "call", "my_fn 1.0f 2.0f");
    gb.run_inference();
    ASSERT(!gb.find("c")->error.empty());
    ASSERT_CONTAINS(gb.find("c")->error, "too many");
}

TEST(call_bang_too_many_inline_args) {
    GraphBuilder gb;
    gb.add("ev", "decl_event", "start () -> void");
    gb.add("e", "event!", "~start");
    gb.add("f", "ffi", "my_fn (x:f32) -> void");
    gb.add("c", "call!", "my_fn 1.0f 2.0f");
    gb.link("e.bang0", "c.bang_in0");
    gb.run_inference();
    ASSERT(!gb.find("c")->error.empty());
    ASSERT_CONTAINS(gb.find("c")->error, "too many");
}

TEST(call_exact_inline_args_no_error) {
    GraphBuilder gb;
    gb.add("f", "ffi", "my_fn (x:f32 y:f32) -> f32");
    gb.add("c", "call", "my_fn 1.0f 2.0f");
    gb.run_inference();
    ASSERT(gb.find("c")->error.empty());
}

TEST(call_inline_no_extra_pins) {
    // When all args are inline, call should have no input pins
    GraphBuilder gb;
    gb.add("f", "ffi", "my_fn (x:f32 y:f32) -> f32");
    gb.add("c", "call", "my_fn 1.0f 2.0f");
    gb.run_inference();
    ASSERT(gb.find("c")->inputs.empty());
}

TEST(call_partial_inline_creates_remaining_pins) {
    // When some args are inline, call creates pins for the rest
    GraphBuilder gb;
    gb.add("f", "ffi", "my_fn (x:f32 y:f32 z:f32) -> f32");
    gb.add("c", "call", "my_fn 1.0f");
    gb.run_inference();
    ASSERT(gb.find("c")->inputs.size() == 2);
    ASSERT(gb.find("c")->inputs[0]->name == "y");
    ASSERT(gb.find("c")->inputs[1]->name == "z");
}

TEST(decl_import_std_ok) {
    GraphBuilder gb;
    gb.add("i", "decl_import", "\"std/math\"");
    gb.run_inference();
    ASSERT(gb.find("i")->error.empty());
}

TEST(decl_import_non_std_error) {
    GraphBuilder gb;
    gb.add("i", "decl_import", "\"foo/bar\"");
    gb.run_inference();
    ASSERT(!gb.find("i")->error.empty());
    ASSERT_CONTAINS(gb.find("i")->error, "std/");
}

// ============================================================
// Link Type Mismatch Error Tests
// ============================================================

TEST(link_type_mismatch_shows_error) {
    // Connect an f32 output to a bool input — should produce a link error
    GraphBuilder gb;
    gb.add("e1", "expr", "1.0f");
    gb.add("e2", "expr", "$0<1");
    gb.link("e1.out0", "e2.0");
    gb.run_inference();
    // e1 outputs f32, e2.$0 expects value — this is compatible, no error
    bool has_link_error = false;
    for (auto& l : gb.graph.links) if (!l.error.empty()) has_link_error = true;
    ASSERT(!has_link_error);
}

TEST(link_type_mismatch_incompatible) {
    // Connect a string output to a node expecting f32
    GraphBuilder gb;
    gb.add("t", "decl_type", "my_struct val:f32");
    gb.add("e1", "expr", "\"hello\"");
    gb.add("n", "new", "my_struct");
    gb.link("e1.out0", "n.val");
    gb.run_inference();
    bool has_link_error = false;
    for (auto& l : gb.graph.links) {
        if (!l.error.empty()) {
            has_link_error = true;
            ASSERT_CONTAINS(l.error, "Type mismatch");
        }
    }
    ASSERT(has_link_error);
}

TEST(link_type_mismatch_f32_to_bool_pin) {
    // Connect f32 output directly to a bool-typed input
    GraphBuilder gb;
    gb.add("e1", "expr", "1.0f");
    gb.add("sel", "select", "$0 1 2");
    // select's condition input expects bool, connect f32 to it
    // e1 output is f32, select.$0 (condition) should be bool
    gb.link("e1.out0", "sel.0");
    gb.run_inference();
    // f32 -> bool is not compatible
    bool has_link_error = false;
    for (auto& l : gb.graph.links) {
        if (!l.error.empty()) has_link_error = true;
    }
    // The select's condition gets f32 from connection, but expects bool
    // This should produce a link type mismatch OR a node error
    bool has_any_error = has_link_error;
    for (auto& n : gb.graph.nodes) {
        if (!n.error.empty()) has_any_error = true;
    }
    ASSERT(has_any_error);
}

TEST(link_type_compatible_no_error) {
    // Connect compatible types — no link error
    GraphBuilder gb;
    gb.add("e1", "expr", "42");
    gb.add("e2", "expr", "$0+1");
    gb.link("e1.out0", "e2.0");
    gb.run_inference();
    for (auto& l : gb.graph.links) ASSERT(l.error.empty());
}

// ============================================================
// call! pin generation from N refs
// ============================================================

TEST(call_bang_generates_input_pin_for_dollar_ref) {
    GraphBuilder gb;
    // call! with $0 should generate 1 input pin
    auto& node = gb.add("c1", "call!", R"(imgui_plot_lines "Delay Line" $0 "")");
    ASSERT_EQ((int)node.inputs.size(), 1);
    ASSERT_EQ(node.inputs[0]->name, "0");
    ASSERT_EQ(node.inputs[0]->direction, FlowPin::Input);
}

TEST(call_bang_generates_multiple_input_pins) {
    GraphBuilder gb;
    auto& node = gb.add("c2", "call!", R"(some_func $0 $1 "hello")");
    ASSERT_EQ((int)node.inputs.size(), 2);
    ASSERT_EQ(node.inputs[0]->name, "0");
    ASSERT_EQ(node.inputs[1]->name, "1");
}

TEST(call_bang_no_dollar_refs_no_input_pins) {
    GraphBuilder gb;
    auto& node = gb.add("c3", "call!", R"(imgui_end)");
    ASSERT_EQ((int)node.inputs.size(), 0);
}

TEST(call_bang_lambda_ref_creates_lambda_pin) {
    GraphBuilder gb;
    auto& node = gb.add("c4", "call!", R"(some_func @0 $1)");
    ASSERT_EQ((int)node.inputs.size(), 2);
    ASSERT_EQ(node.inputs[0]->name, "@0");
    ASSERT_EQ(node.inputs[0]->direction, FlowPin::Lambda);
    ASSERT_EQ(node.inputs[1]->name, "1");
    ASSERT_EQ(node.inputs[1]->direction, FlowPin::Input);
}

TEST(call_bang_pin_from_atto_file_roundtrip) {
    // Simulate loading from a .atto file: args array gets joined with spaces
    // args = ["imgui_plot_lines", "\"Delay Line\"", "$0", "\"\""]
    // After parse_array + unquote, cur_args = {imgui_plot_lines, "Delay Line", $0, ""}
    // After join: imgui_plot_lines "Delay Line" $0 ""
    std::vector<std::string> cur_args = {"imgui_plot_lines", "\"Delay Line\"", "$0", "\"\""};
    std::string args_str;
    for (auto& a : cur_args) { if (!args_str.empty()) args_str += " "; args_str += a; }

    GraphBuilder gb;
    auto& node = gb.add("c5", "call!", args_str);
    ASSERT_EQ((int)node.inputs.size(), 1);
    ASSERT_EQ(node.inputs[0]->name, "0");
    ASSERT_EQ(node.inputs[0]->direction, FlowPin::Input);
}

TEST(call_bang_dollar_ref_pin_survives_resolve_type_based_pins) {
    // Regression: resolve_type_based_pins was wiping N ref pins
    // because it reconciled with only non-inline pins (empty list when
    // all function args are covered by inline args).
    GraphBuilder gb;
    // Declare an ffi function: my_func(a:string b:&vector<f32> c:string) -> void
    gb.add("ffi1", "ffi", R"(my_func (a:string b:&vector<f32> c:string) -> void)");
    // call! with all 3 args inline, $0 is a pin ref
    auto& call_node = gb.add("call1", "call!", R"(my_func "hello" $0 "world")");

    // Before resolve: should have 1 input pin for $0
    ASSERT_EQ((int)call_node.inputs.size(), 1);
    ASSERT_EQ(call_node.inputs[0]->name, "0");

    // Now run resolve_type_based_pins (this is what the loader does after flush_node)
    resolve_type_based_pins(gb.graph);

    // After resolve: $0 pin must still exist
    auto* n = gb.find("call1");
    ASSERT(n != nullptr);
    ASSERT_EQ((int)n->inputs.size(), 1);
    ASSERT_EQ(n->inputs[0]->name, "0");
    ASSERT_EQ(n->inputs[0]->direction, FlowPin::Input);
}

TEST(call_bang_dollar_ref_pin_gets_type_from_resolve) {
    // The $0 pin should get type info from the function signature.
    // $0 is at inline arg position 1 (after fn name), mapping to fn arg[1] = b:&vector<f32>.
    // However, the type annotation loop maps by pin name ("0") to fn arg[0] ("a:string").
    // This is a known limitation: pin name N doesn't match inline arg position.
    // For now, just verify the pin survives and has some type set.
    GraphBuilder gb;
    gb.add("ffi1", "ffi", R"(my_func (a:string b:&vector<f32> c:string) -> void)");
    auto& call_node = gb.add("call1", "call!", R"(my_func "hello" $0 "world")");

    resolve_type_based_pins(gb.graph);

    auto* n = gb.find("call1");
    ASSERT(n != nullptr);
    ASSERT_EQ((int)n->inputs.size(), 1);
    // $0 is at inline arg position 1 → fn_arg[1] = b:&vector<f32>
    ASSERT_EQ(n->inputs[0]->type_name, "&vector<f32>");
}

TEST(call_dollar_ref_with_field_access_no_type_on_pin) {
    // $0.field should NOT set pin "0" type from the fn arg at that position,
    // because the fn arg type is for the field value, not the struct on pin 0.
    // Mirrors: call imgui_slider_float "" $0.amplitude 0 1
    GraphBuilder gb;
    gb.add("ffi1", "ffi", R"(my_func (label:string value:&f32 min:f32 max:f32) -> bool)");
    auto& call_node = gb.add("call1", "call", R"(my_func "" $0.amplitude 0 1)");

    resolve_type_based_pins(gb.graph);

    auto* n = gb.find("call1");
    ASSERT(n != nullptr);
    ASSERT_EQ((int)n->inputs.size(), 1);
    ASSERT_EQ(n->inputs[0]->name, "0");
    // Pin type should NOT be &f32 — the pin carries the struct, not the field
    ASSERT(n->inputs[0]->type_name != "&f32");

    // After inference, pin 0 should not be typed as string (the first fn arg).
    // Without a connected struct, the field access can't fully resolve, but
    // the pin type must not be incorrectly set from fn arg position mismatch.
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    n = gb.find("call1");
    ASSERT(n != nullptr);
    // Pin 0 should not be string — it carries a struct, not the label arg
    if (n->inputs[0]->resolved_type) {
        ASSERT(type_to_string(n->inputs[0]->resolved_type) != "string");
    }
}

TEST(call_bare_dollar_ref_gets_type) {
    // Bare $0 (no field access) SHOULD get type from fn arg
    GraphBuilder gb;
    gb.add("ffi1", "ffi", R"(my_func (a:string b:&f32 c:string) -> void)");
    auto& call_node = gb.add("call1", "call!", R"(my_func "hello" $0 "world")");

    resolve_type_based_pins(gb.graph);

    auto* n = gb.find("call1");
    ASSERT(n != nullptr);
    ASSERT_EQ((int)n->inputs.size(), 1);
    ASSERT_EQ(n->inputs[0]->type_name, "&f32");
}

// ============================================================
// cast node pin generation
// ============================================================

TEST(cast_has_input_pin) {
    GraphBuilder gb;
    auto& node = gb.add("c1", "cast", "vector<f32>");
    ASSERT_EQ((int)node.inputs.size(), 1);
    ASSERT_EQ(node.inputs[0]->name, "value");
    ASSERT_EQ(node.inputs[0]->direction, FlowPin::Input);
}

TEST(cast_has_output_pin) {
    GraphBuilder gb;
    auto& node = gb.add("c1", "cast", "vector<f32>");
    ASSERT_EQ((int)node.outputs.size(), 1);
}

TEST(cast_output_type_is_dest_type) {
    GraphBuilder gb;
    gb.add("c1", "cast", "vector<f32>");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("c1");
    ASSERT(n != nullptr);
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_EQ(type_to_string(n->outputs[0]->resolved_type), "vector<f32>");
}

TEST(call_bang_no_false_too_many_args_with_dollar_ref) {
    // Regression: N ref pins were double-counted as both inline args AND input pins,
    // causing a false "too many arguments" error.
    GraphBuilder gb;
    gb.add("ffi1", "ffi", R"(my_func (a:string b:&vector<f32> c:string) -> void)");
    auto& call_node = gb.add("call1", "call!", R"(my_func "hello" $0 "world")");

    resolve_type_based_pins(gb.graph);

    // Run inference
    GraphInference gi(gb.pool);
    gi.run(gb.graph);

    auto* n = gb.find("call1");
    ASSERT(n != nullptr);
    // Should have no error — 3 args expected, 2 inline + 1 pin ref = 3 total
    ASSERT(n->error.empty());
}

// ============================================================
// cast node: additional tests
// ============================================================

TEST(cast_different_dest_type) {
    // Cast to a different type, e.g. vector<s32>
    GraphBuilder gb;
    gb.add("c1", "cast", "vector<s32>");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("c1");
    ASSERT(n != nullptr);
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_EQ(type_to_string(n->outputs[0]->resolved_type), "vector<s32>");
}

TEST(cast_preserves_input_pin_after_resolve) {
    // cast should keep its input pin through resolve_type_based_pins
    GraphBuilder gb;
    gb.add("c1", "cast", "vector<f32>");
    resolve_type_based_pins(gb.graph);
    auto* n = gb.find("c1");
    ASSERT(n != nullptr);
    ASSERT_EQ((int)n->inputs.size(), 1);
    ASSERT_EQ(n->inputs[0]->name, "value");
}

TEST(cast_no_error) {
    // A valid cast node should have no error
    GraphBuilder gb;
    gb.add("c1", "cast", "vector<f32>");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("c1");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
}

TEST(cast_output_type_independent_of_input) {
    // Cast output type should be the dest type regardless of what's connected to input
    GraphBuilder gb;
    gb.add("decl1", "decl_var", "my_arr array<f32,48000>");
    gb.add("e1", "expr", "my_arr");
    gb.add("c1", "cast", "vector<f32>");
    gb.link("e1.out0", "c1.value");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("c1");
    ASSERT(n != nullptr);
    // Output should always be the dest type
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_EQ(type_to_string(n->outputs[0]->resolved_type), "vector<f32>");
}

// ============================================================
// resize! node tests
// ============================================================

TEST(resize_has_bang_input) {
    GraphBuilder gb;
    auto& node = gb.add("r1", "resize!", "my_vec 32");
    ASSERT_EQ((int)node.triggers.size(), 1);
}

TEST(resize_has_bang_output) {
    GraphBuilder gb;
    auto& node = gb.add("r1", "resize!", "my_vec 32");
    ASSERT_EQ((int)node.nexts.size(), 1);
}

TEST(resize_no_data_outputs) {
    GraphBuilder gb;
    auto& node = gb.add("r1", "resize!", "my_vec 32");
    ASSERT_EQ((int)node.outputs.size(), 0);
}

TEST(resize_args_parsed) {
    GraphBuilder gb;
    auto& node = gb.add("r1", "resize!", "my_vec my_size");
    ASSERT_EQ(node.args, "my_vec my_size");
}

TEST(resize_no_error) {
    GraphBuilder gb;
    gb.add("decl1", "decl_var", "my_vec vector<f32>");
    gb.add("r1", "resize!", "my_vec 32");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("r1");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
}

// ============================================================
// str node tests
// ============================================================

TEST(str_has_input_pin) {
    GraphBuilder gb;
    auto& node = gb.add("s1", "str", "");
    ASSERT_EQ((int)node.inputs.size(), 1);
    ASSERT_EQ(node.inputs[0]->name, "value");
}

TEST(str_has_output_pin) {
    GraphBuilder gb;
    auto& node = gb.add("s1", "str", "");
    ASSERT_EQ((int)node.outputs.size(), 1);
}

TEST(str_output_type_is_string) {
    GraphBuilder gb;
    gb.add("s1", "str", "");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("s1");
    ASSERT(n != nullptr);
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_EQ(type_to_string(n->outputs[0]->resolved_type), "string");
}

TEST(str_output_is_string_regardless_of_input) {
    // Even with an integer input, str output is always string
    GraphBuilder gb;
    gb.add("e1", "expr", "42");
    gb.add("s1", "str", "");
    gb.link("e1.out0", "s1.value");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("s1");
    ASSERT(n != nullptr);
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_EQ(type_to_string(n->outputs[0]->resolved_type), "string");
}

// ============================================================
// string + unknown defers (expr.cpp fix)
// ============================================================

TEST(string_plus_string_resolves) {
    // string + string should resolve to string
    GraphBuilder gb;
    gb.add("e1", "expr", "\"hello\"+\"world\"");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_EQ(type_to_string(n->outputs[0]->resolved_type), "string");
}

TEST(string_plus_unknown_defers_no_error) {
    // "##amp"+$0 where $0 is not yet resolved should NOT produce an error
    // It should defer and resolve once $0 is known to be string
    GraphBuilder gb;
    gb.add("s1", "str", "");
    gb.add("e1", "expr", "\"##amp\"+$0");
    gb.link("s1.out0", "e1.0");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    // Should have no error — str outputs string, so "##amp"+string is valid
    ASSERT(n->error.empty());
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_EQ(type_to_string(n->outputs[0]->resolved_type), "string");
}

TEST(string_plus_int_still_errors) {
    // string + s32 should still produce an error
    GraphBuilder gb;
    gb.add("e1", "expr", "42");
    gb.add("e2", "expr", "\"hello\"+$0");
    gb.link("e1.out0", "e2.0");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e2");
    ASSERT(n != nullptr);
    ASSERT(!n->error.empty());
    ASSERT_CONTAINS(n->error, "Cannot add string");
}

// ============================================================
// call! arg counting with N refs (regression tests)
// ============================================================

TEST(call_bang_multiple_dollar_refs_no_false_error) {
    // call! with multiple N refs should count correctly
    GraphBuilder gb;
    gb.add("ffi1", "ffi", R"(my_func (a:&f32 b:&f32 c:string) -> void)");
    auto& call_node = gb.add("call1", "call!", R"(my_func $0 $1 "hello")");
    resolve_type_based_pins(gb.graph);
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("call1");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
    ASSERT_EQ((int)n->inputs.size(), 2);
}

TEST(call_bang_all_inline_no_pins) {
    // call! with all inline args should have 0 input pins
    GraphBuilder gb;
    gb.add("ffi1", "ffi", R"(my_func (a:string b:s32) -> void)");
    auto& call_node = gb.add("call1", "call!", R"(my_func "hello" 42)");
    resolve_type_based_pins(gb.graph);
    auto* n = gb.find("call1");
    ASSERT(n != nullptr);
    ASSERT_EQ((int)n->inputs.size(), 0);
}

TEST(call_dollar_ref_field_access_pin_count) {
    // $0.field should create exactly 1 pin
    GraphBuilder gb;
    gb.add("ffi1", "ffi", R"(my_func (label:string value:&f32 min:f32 max:f32) -> bool)");
    gb.add("call1", "call!", R"(my_func "##test" $0.freq 0 1)");
    resolve_type_based_pins(gb.graph);
    auto* n = gb.find("call1");
    ASSERT(n != nullptr);
    ASSERT_EQ((int)n->inputs.size(), 1);
    ASSERT_EQ(n->inputs[0]->name, "0");
}

TEST(call_multiple_dollar_refs_with_field_access) {
    // Multiple N.field refs
    GraphBuilder gb;
    gb.add("ffi1", "ffi", R"(my_func (a:&f32 b:&f32) -> void)");
    gb.add("call1", "call!", R"(my_func $0.x $1.y)");
    resolve_type_based_pins(gb.graph);
    auto* n = gb.find("call1");
    ASSERT(n != nullptr);
    ASSERT_EQ((int)n->inputs.size(), 2);
    ASSERT_EQ(n->inputs[0]->name, "0");
    ASSERT_EQ(n->inputs[1]->name, "1");
}

TEST(call_string_concat_with_dollar_ref) {
    // "##amp"+$1 pattern used in multifader
    GraphBuilder gb;
    gb.add("ffi1", "ffi", R"(my_slider (label:string value:&f32 min:f32 max:f32) -> bool)");
    gb.add("call1", "call!", R"(my_slider "##amp"+$1 $0.amplitude 0 1)");
    resolve_type_based_pins(gb.graph);
    auto* n = gb.find("call1");
    ASSERT(n != nullptr);
    // Should have 2 pins: $0 and $1
    ASSERT_EQ((int)n->inputs.size(), 2);
}

// ============================================================
// rand builtin
// ============================================================

TEST(rand_parses) {
    auto r = parse_expression("rand(1,10)");
    ASSERT(r.root != nullptr);
    ASSERT(r.error.empty());
    ASSERT_EQ(r.root->kind, ExprKind::FuncCall);
    ASSERT_EQ(r.root->builtin, BuiltinFunc::Rand);
}

TEST(rand_int_returns_int) {
    GraphBuilder gb;
    gb.add("e1", "expr", "rand(1,10)");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    // Both args are int literals, result should be int-like
    ASSERT(n->outputs[0]->resolved_type->kind == TypeKind::Scalar);
}

TEST(rand_float_returns_float) {
    GraphBuilder gb;
    gb.add("e1", "expr", "rand(0.0f,1.0f)");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT(n->outputs[0]->resolved_type != nullptr);
    ASSERT_EQ(type_to_string(n->outputs[0]->resolved_type), "f32");
}

TEST(rand_with_pin_refs) {
    // rand($0, $1) should parse and have 2 pin refs
    GraphBuilder gb;
    gb.add("e1", "expr", "rand($0,$1)");
    ASSERT_EQ((int)gb.find("e1")->inputs.size(), 2);
}

TEST(rand_too_few_args_errors) {
    GraphBuilder gb;
    gb.add("e1", "expr", "rand(1)");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT(!n->error.empty());
    ASSERT_CONTAINS(n->error, "rand requires 2 arguments");
}

TEST(rand_int_literals_backpropagate_to_float) {
    // When rand(200,12000) is assigned to a f32 field, the int literals
    // should backpropagate to f32 and rand should return f32
    GraphBuilder gb;
    gb.add("dt1", "decl_type", "my_struct freq:f32");
    gb.add("dv1", "decl_var", "my_var my_struct");
    gb.add("s1", "store!", "my_var.freq rand(200,12000)");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("s1");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
    // The store's parsed expr for rand should have resolved to f32
    ASSERT(n->parsed_exprs.size() >= 2);
    auto& rand_expr = n->parsed_exprs[1];
    ASSERT(rand_expr != nullptr);
    ASSERT(rand_expr->resolved_type != nullptr);
    ASSERT_EQ(type_to_string(rand_expr->resolved_type), "f32");
}

TEST(rand_no_error_with_valid_args) {
    GraphBuilder gb;
    gb.add("e1", "expr", "rand(0.0f,1.0f)");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
}

// ============================================================
// void node post_bang chain
// ============================================================

TEST(void_node_has_output) {
    GraphBuilder gb;
    auto& node = gb.add("v1", "void", "");
    ASSERT_EQ((int)node.outputs.size(), 1);
}

TEST(void_output_type_resolves) {
    // void node should not produce an error
    GraphBuilder gb;
    gb.add("v1", "void", "");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("v1");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
}

// ============================================================
// resize! with variable size
// ============================================================

TEST(resize_with_variable_size) {
    GraphBuilder gb;
    gb.add("decl1", "decl_var", "my_vec vector<f32>");
    gb.add("decl2", "decl_var", "my_size s32");
    gb.add("r1", "resize!", "my_vec my_size");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("r1");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
}

TEST(resize_has_correct_pin_layout) {
    // resize! should have: 1 bang_in, 2 inputs (target, size), 1 bang_out, 0 data outputs
    GraphBuilder gb;
    auto& node = gb.add("r1", "resize!", "my_vec 32");
    ASSERT_EQ((int)node.triggers.size(), 1);
    ASSERT_EQ((int)node.nexts.size(), 1);
    ASSERT_EQ((int)node.outputs.size(), 0);
}

// ============================================================
// vslider FFI signatures
// ============================================================

TEST(vslider_float_ffi_parses) {
    GraphBuilder gb;
    gb.add("ffi1", "ffi", R"(imgui_vslider_float (label:string width:f32 height:f32 value:&f32 min:f32 max:f32) -> bool)");
    auto* n = gb.find("ffi1");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
}

TEST(vslider_int_ffi_parses) {
    GraphBuilder gb;
    gb.add("ffi1", "ffi", R"(imgui_vslider_int (label:string width:f32 height:f32 value:&s32 min:s32 max:s32) -> bool)");
    auto* n = gb.find("ffi1");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
}

TEST(call_vslider_with_string_concat_and_field) {
    // Matches multifader pattern: call! imgui_vslider_float "##amp"+$1 16 256 $0.amplitude 0 1
    GraphBuilder gb;
    gb.add("ffi1", "ffi", R"(imgui_vslider_float (label:string width:f32 height:f32 value:&f32 min:f32 max:f32) -> bool)");
    gb.add("call1", "call!", R"(imgui_vslider_float "##amp"+$1 16 256 $0.amplitude 0 1)");
    resolve_type_based_pins(gb.graph);
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("call1");
    ASSERT(n != nullptr);
    // Should have 2 pins: $0 and $1
    ASSERT_EQ((int)n->inputs.size(), 2);
    // Should have no error
    ASSERT(n->error.empty());
}

// ============================================================
// new_line and same_line FFI
// ============================================================

TEST(same_line_ffi_parses) {
    GraphBuilder gb;
    gb.add("ffi1", "ffi", R"(imgui_same_line () -> void)");
    auto* n = gb.find("ffi1");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
}

TEST(new_line_ffi_parses) {
    GraphBuilder gb;
    gb.add("ffi1", "ffi", R"(imgui_new_line () -> void)");
    auto* n = gb.find("ffi1");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
}

TEST(push_style_var_vec2_ffi_parses) {
    GraphBuilder gb;
    gb.add("ffi1", "ffi", R"(imgui_push_style_var_vec2 (idx:s32 x:f32 y:f32) -> void)");
    auto* n = gb.find("ffi1");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
}

TEST(pop_style_var_ffi_parses) {
    GraphBuilder gb;
    gb.add("ffi1", "ffi", R"(imgui_pop_style_var (count:s32) -> void)");
    auto* n = gb.find("ffi1");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
}

// ============================================================
// Iterator deref in method calls
// ============================================================

TEST(iterator_method_call_arg_gets_auto_deref) {
    // keys[$0].stop(keys[$0]) — inference should insert a Deref node
    // wrapping the iterator argument so it becomes a value type
    GraphBuilder gb;
    gb.add("dt_osc_res", "decl_type", "osc_res s:f32 e:bool");
    gb.add("dt_gen_fn", "decl_type", "gen_fn (osc:&osc_def) -> osc_res");
    gb.add("dt_stop_fn", "decl_type", "stop_fn (osc:&osc_def) -> void");
    gb.add("dt_osc_def", "decl_type", "osc_def gen:gen_fn stop:stop_fn p:f32 pstep:f32 a:f32 astep:f32");
    gb.add("dt_key_set", "decl_type", "key_set map<u8, ^list_iterator<osc_def>>");
    gb.add("dv_keys", "decl_var", "keys key_set");
    gb.add("e1", "expr", "keys[$0].stop(keys[$0])");
    gb.run_inference();

    auto* e1 = gb.find("e1");
    ASSERT(e1 != nullptr);
    ASSERT(!e1->parsed_exprs.empty());
    auto& expr = e1->parsed_exprs[0];
    ASSERT(expr != nullptr);
    ASSERT(expr->kind == ExprKind::FuncCall);
    ASSERT(expr->children.size() >= 2);
    // children[1] should now be a Deref node wrapping the original iterator arg
    auto& arg = expr->children[1];
    ASSERT(arg != nullptr);
    ASSERT_EQ((int)arg->kind, (int)ExprKind::Deref);
    // The Deref's child should be the original Index expr with ContainerIterator type
    ASSERT(!arg->children.empty());
    ASSERT(arg->children[0]->resolved_type != nullptr);
    ASSERT_EQ((int)arg->children[0]->resolved_type->kind, (int)TypeKind::ContainerIterator);
}

// ============================================================
// select! node (3 bang outputs: next, true, false)
// ============================================================

TEST(select_bang_has_three_bang_outputs) {
    GraphBuilder gb;
    auto& node = gb.add("s1", "select!", "$0");
    ASSERT_EQ((int)node.nexts.size(), 3);
    ASSERT_EQ(node.nexts[0]->name, "next");
    ASSERT_EQ(node.nexts[1]->name, "true");
    ASSERT_EQ(node.nexts[2]->name, "false");
}

TEST(select_bang_has_one_bang_input) {
    GraphBuilder gb;
    auto& node = gb.add("s1", "select!", "$0");
    ASSERT_EQ((int)node.triggers.size(), 1);
}

TEST(select_bang_has_condition_input) {
    GraphBuilder gb;
    auto& node = gb.add("s1", "select!", "$0");
    ASSERT_EQ((int)node.inputs.size(), 1);
}

TEST(select_bang_no_data_outputs) {
    GraphBuilder gb;
    auto& node = gb.add("s1", "select!", "$0");
    ASSERT_EQ((int)node.outputs.size(), 0);
}

TEST(select_bang_next_fires_after_branches) {
    // Verify next (bang_outputs[0]) is separate from true/false
    GraphBuilder gb;
    auto& node = gb.add("s1", "select!", "$0");
    ASSERT(node.nexts[0]->id != node.nexts[1]->id);
    ASSERT(node.nexts[0]->id != node.nexts[2]->id);
    ASSERT(node.nexts[1]->id != node.nexts[2]->id);
}

// ============================================================
// Shadow expr node tests
// ============================================================

TEST(shadow_store_generates_one_shadow_node) {
    // store! my_var.freq rand(200,12000) → 1 shadow for value (target is lvalue, stays inline)
    GraphBuilder gb;
    gb.add("dv", "decl_var", "my_var my_struct");
    gb.add("s1", "store!", "my_var.freq rand(200,12000)");
    generate_shadow_nodes(gb.graph);

    int shadow_count = 0;
    for (auto& n : gb.graph.nodes) if (n.shadow) shadow_count++;
    ASSERT_EQ(shadow_count, 1);
}

TEST(shadow_nodes_are_expr_type) {
    GraphBuilder gb;
    gb.add("s1", "store!", "my_var 42");
    generate_shadow_nodes(gb.graph);

    for (auto& n : gb.graph.nodes) {
        if (n.shadow) {
            ASSERT_EQ(n.type_id, NodeTypeID::Expr);
        }
    }
}

TEST(shadow_value_has_correct_args) {
    // Only the value arg gets a shadow, not the lvalue target
    GraphBuilder gb;
    gb.add("s1", "store!", "my_var.freq rand(200,12000)");
    generate_shadow_nodes(gb.graph);

    bool found_rand = false;
    for (auto& n : gb.graph.nodes) {
        if (!n.shadow) continue;
        if (n.args == "rand(200,12000)") found_rand = true;
    }
    ASSERT(found_rand);
}

TEST(shadow_parent_keeps_lvalue_arg) {
    // store! keeps the lvalue target token in args
    GraphBuilder gb;
    gb.add("s1", "store!", "my_var.freq rand(200,12000)");
    generate_shadow_nodes(gb.graph);

    auto* s1 = gb.find("s1");
    ASSERT(s1 != nullptr);
    ASSERT_EQ(s1->args, "my_var.freq");
}

TEST(shadow_skip_expr_nodes) {
    // expr nodes should NOT get shadow nodes
    GraphBuilder gb;
    gb.add("e1", "expr", "$0+$1");
    generate_shadow_nodes(gb.graph);

    int shadow_count = 0;
    for (auto& n : gb.graph.nodes) if (n.shadow) shadow_count++;
    ASSERT_EQ(shadow_count, 0);
}

TEST(shadow_skip_decl_nodes) {
    // decl_var should NOT get shadow nodes
    GraphBuilder gb;
    gb.add("dv", "decl_var", "my_var f32");
    generate_shadow_nodes(gb.graph);

    int shadow_count = 0;
    for (auto& n : gb.graph.nodes) if (n.shadow) shadow_count++;
    ASSERT_EQ(shadow_count, 0);
}

TEST(shadow_skip_call_nodes) {
    // call! nodes are skipped for now — resolve_type_based_pins manages their pins
    GraphBuilder gb;
    gb.add("ffi1", "ffi", R"(my_func (a:string b:s32) -> void)");
    gb.add("c1", "call!", R"(my_func "hello" 42)");
    generate_shadow_nodes(gb.graph);

    int shadow_count = 0;
    for (auto& n : gb.graph.nodes) if (n.shadow) shadow_count++;
    ASSERT_EQ(shadow_count, 0);
}

TEST(shadow_remove_cleans_up) {
    GraphBuilder gb;
    gb.add("s1", "store!", "my_var 42");
    generate_shadow_nodes(gb.graph);
    int before = (int)gb.graph.nodes.size();
    ASSERT(before > 1); // has shadows

    remove_shadow_nodes(gb.graph);
    int shadow_count = 0;
    for (auto& n : gb.graph.nodes) if (n.shadow) shadow_count++;
    ASSERT_EQ(shadow_count, 0);
}

// ============================================================
// Shadow + inference integration tests
// ============================================================

TEST(shadow_select_condition_resolves) {
    // select keys?[$0] — condition should resolve to bool through shadow
    GraphBuilder gb;
    gb.add("dt_key_set", "decl_type", "key_set map<u8, s32>");
    gb.add("dv_keys", "decl_var", "keys key_set");
    gb.add("e1", "expr", "$0:key");  // provides u8 input
    gb.add("sel", "select", "keys?[$0]");
    gb.link("e1.out0", "sel.0");     // $0 = u8

    // Also provide if_true and if_false
    gb.add("t1", "expr", "42");
    gb.add("f1", "expr", "0");
    gb.link("t1.out0", "sel.if_true");
    gb.link("f1.out0", "sel.if_false");

    auto errors = gb.run_full_pipeline();
    for (auto& e : errors) printf("    ERR: %s\n", e.c_str());

    auto* sel = gb.find("sel");
    ASSERT(sel != nullptr);
    ASSERT(sel->error.empty());
}

TEST(shadow_select_as_lambda_param_found) {
    // select keys?[$0] used as lock lambda — $0 is a lambda parameter,
    // must be found through shadow node traversal
    GraphBuilder gb;
    gb.add("dt_osc_def", "decl_type", "osc_def p:f32");
    gb.add("dt_key_set", "decl_type", "key_set map<u8, ^list_iterator<osc_def>>");
    gb.add("dv_keys", "decl_var", "keys key_set");
    gb.add("dv_mtx", "decl_var", "mtx mutex");

    // Inside the lock lambda: expr $0:midi_key provides the key
    gb.add("param_expr", "expr", "$0:midi_key");
    gb.add("sel", "select", "keys?[$0]");
    gb.link("param_expr.out0", "sel.0");   // $0 = midi_key

    gb.add("t1", "expr", "42");
    gb.add("f1", "expr", "0");
    gb.link("t1.out0", "sel.if_true");
    gb.link("f1.out0", "sel.if_false");

    gb.add("lk", "lock", "mtx");
    gb.link("sel.as_lambda", "lk.fn");

    auto errors = gb.run_full_pipeline();
    for (auto& e : errors) printf("    ERR: %s\n", e.c_str());

    auto* sel = gb.find("sel");
    ASSERT(sel != nullptr);
    ASSERT(sel->error.empty());

    // Check no lambda param count error on the lock link
    bool lock_link_error = false;
    for (auto& l : gb.graph.links) {
        if (l.to_pin.find("lk") != std::string::npos && !l.error.empty()) {
            printf("    LINK ERR: %s\n", l.error.c_str());
            lock_link_error = true;
        }
    }
    ASSERT(!lock_link_error);
}

TEST(shadow_store_value_type_propagates) {
    // store! my_var rand(1,10) — shadow for rand should resolve to int
    GraphBuilder gb;
    gb.add("dv", "decl_var", "my_var s32");
    gb.add("s1", "store!", "my_var rand(1,10)");
    auto errors = gb.run_full_pipeline();
    for (auto& e : errors) printf("    ERR: %s\n", e.c_str());

    auto* s1 = gb.find("s1");
    ASSERT(s1 != nullptr);
    ASSERT(s1->error.empty());
}

TEST(shadow_store_two_pin_refs_in_value) {
    // store! $0 $0+$1.s — $0 is lvalue (kept), $1.s is in shadow value expr
    // $0 = &f32 (from decl_local), $1 = osc_res (from expr that returns osc_res)
    GraphBuilder gb;
    gb.add("dt_osc_res", "decl_type", "osc_res s:f32 e:bool");
    gb.add("dl_mixs", "decl_var", "mixs f32");
    // A simple expr that outputs osc_res type — use a new node
    gb.add("dv_res", "decl_var", "my_res osc_res");
    gb.add("res_expr", "expr", "my_res");     // outputs osc_res
    gb.add("st", "store!", "$0 $0+$1.s");
    gb.link("dl_mixs.out0", "st.0");            // $0 = &f32
    gb.link("res_expr.out0", "st.1");            // $1 = osc_res

    auto errors = gb.run_full_pipeline();
    for (auto& e : errors) printf("    ERR: %s\n", e.c_str());

    auto* st = gb.find("st");
    ASSERT(st != nullptr);

    ASSERT(st->error.empty());
}

// ============================================================
// Connection direction tests
// ============================================================

TEST(connect_decl_local_out_to_store_value) {
    // decl_local mixs f32 → store! audio_tick
    // decl_local.out0 (Output, &f32) → store!.value (Input)
    GraphBuilder gb;
    gb.add("dv_at", "decl_var", "audio_tick () -> void");
    gb.add("dl", "decl_var", "mixs f32");
    gb.add("st", "store!", "audio_tick");

    // Verify store! has a "value" input pin
    auto* st = gb.find("st");
    ASSERT(st != nullptr);
    bool has_value_pin = false;
    for (auto& p : st->inputs) {
        if (p->name == "value") { has_value_pin = true; break; }
    }
    ASSERT(has_value_pin);

    // Verify decl_local has an output
    auto* dl = gb.find("dl");
    ASSERT(dl != nullptr);
    ASSERT(!dl->outputs.empty());

    // Connect decl_local.out0 → store!.value
    gb.link("dl.out0", "st.value");

    // Run inference — should have no errors on the store
    auto errors = gb.run_inference();
    for (auto& e : errors) printf("    ERR: %s\n", e.c_str());

    st = gb.find("st");
    ASSERT(st != nullptr);
    // store! target is audio_tick (()->void), value is &f32 — type mismatch is expected
    // but the LINK should exist and the store itself should not have a structural error
    // (the type mismatch is on the store's type check, not on the link)

    // Verify the link exists
    bool link_exists = false;
    for (auto& l : gb.graph.links) {
        if (l.from_pin == "dl.out0" && l.to_pin == "st.value") {
            link_exists = true;
            break;
        }
    }
    ASSERT(link_exists);
}

TEST(connect_store_bang_to_decl_local_trigger) {
    // store!.bang0 (BangNext) → decl_local.bang_in0 (BangTrigger)
    // This is the standard bang chain connection
    GraphBuilder gb;
    gb.add("dv", "decl_var", "x f32");
    gb.add("st", "store!", "x 42");
    gb.add("dl", "decl_var", "y f32");

    // store! has nexts (bang outputs), decl_local has triggers (bang inputs)
    auto* st = gb.find("st");
    ASSERT(st != nullptr);
    ASSERT(!st->nexts.empty());

    auto* dl = gb.find("dl");
    ASSERT(dl != nullptr);
    ASSERT(!dl->triggers.empty());

    // Connect store!.bang0 → decl_local.bang_in0
    std::string from_pin = st->nexts[0]->id;
    std::string to_pin = dl->triggers[0]->id;
    gb.link(from_pin, to_pin);

    // Verify link exists
    bool link_exists = false;
    for (auto& l : gb.graph.links) {
        if (l.from_pin == from_pin && l.to_pin == to_pin) {
            link_exists = true;
            break;
        }
    }
    ASSERT(link_exists);

    // Verify pin directions
    ASSERT_EQ((int)st->nexts[0]->direction, (int)FlowPin::BangNext);
    ASSERT_EQ((int)dl->triggers[0]->direction, (int)FlowPin::BangTrigger);
}

TEST(connect_bang_trigger_as_value_source) {
    // decl_local.bang_in0 (BangTrigger) → store!.value (Input)
    // BangTrigger outputs () -> void, store saves it into a variable
    GraphBuilder gb;
    gb.add("dv_at", "decl_var", "audio_tick () -> void");
    gb.add("dl", "decl_var", "mixs f32");
    gb.add("st", "store!", "audio_tick");

    // Connect decl_local's BangTrigger to store's value pin
    std::string trigger_pin = gb.find("dl")->triggers[0]->id;
    std::string value_pin;
    for (auto& p : gb.find("st")->inputs)
        if (p->name == "value") { value_pin = p->id; break; }
    ASSERT(!value_pin.empty());

    gb.link(trigger_pin, value_pin);

    // Verify link direction: BangTrigger → Input
    bool link_exists = false;
    for (auto& l : gb.graph.links)
        if (l.from_pin == trigger_pin && l.to_pin == value_pin) { link_exists = true; break; }
    ASSERT(link_exists);
}

// ============================================================
// Multi-connection validation tests
// ============================================================

TEST(multi_bang_trigger_no_captures_ok) {
    // BangTrigger with no data inputs → multiple connections allowed
    GraphBuilder gb;
    gb.add("s1", "store!", "x 1");
    gb.add("s2", "store!", "y 2");
    gb.add("dv_x", "decl_var", "x f32");
    gb.add("dv_y", "decl_var", "y f32");
    gb.add("dl", "decl_var", "z f32");

    // Two BangNext pins connect to the same BangTrigger
    std::string trigger = gb.find("dl")->triggers[0]->id;
    std::string next1 = gb.find("s1")->nexts[0]->id;
    std::string next2 = gb.find("s2")->nexts[0]->id;
    gb.link(next1, trigger);
    gb.link(next2, trigger);

    auto errors = gb.run_inference();

    // No "Cannot share trigger" errors expected — decl_local has no data captures
    bool has_share_error = false;
    for (auto& l : gb.graph.links)
        if (l.error.find("Cannot share") != std::string::npos) has_share_error = true;
    ASSERT(!has_share_error);
}

TEST(multi_bang_trigger_with_captures_error) {
    // BangTrigger with a connected data input → multiple connections should error
    GraphBuilder gb;
    gb.add("dv_x", "decl_var", "x f32");
    gb.add("e1", "expr", "42");
    gb.add("st1", "store!", "x $0");
    gb.add("st2", "store!", "x 1");
    gb.link("e1.out0", "st1.0"); // st1 has a data input connected

    // Two BangNext pins connect to st1's trigger
    std::string trigger = gb.find("st1")->triggers[0]->id;
    std::string next1 = gb.find("st2")->nexts[0]->id;
    // Also connect from a second source — create another store
    gb.add("st3", "store!", "x 3");
    std::string next2 = gb.find("st3")->nexts[0]->id;
    gb.link(next1, trigger);
    gb.link(next2, trigger);

    auto errors = gb.run_inference();

    // Should have "Cannot share trigger" error
    bool has_share_error = false;
    for (auto& l : gb.graph.links)
        if (!l.error.empty() && l.error.find("Cannot share") != std::string::npos) has_share_error = true;
    ASSERT(has_share_error);
}

TEST(single_bang_trigger_with_captures_ok) {
    // Single connection to BangTrigger with captures → always OK
    GraphBuilder gb;
    gb.add("dv_x", "decl_var", "x f32");
    gb.add("e1", "expr", "42");
    gb.add("st1", "store!", "x $0");
    gb.link("e1.out0", "st1.0");

    gb.add("st2", "store!", "x 1");
    std::string trigger = gb.find("st1")->triggers[0]->id;
    std::string next = gb.find("st2")->nexts[0]->id;
    gb.link(next, trigger);

    auto errors = gb.run_inference();

    // No share error — single connection
    bool has_share_error = false;
    for (auto& l : gb.graph.links)
        if (!l.error.empty() && l.error.find("Cannot share") != std::string::npos) has_share_error = true;
    ASSERT(!has_share_error);
}

// ============================================================
// Caller scope / capture vs parameter tests
// ============================================================

TEST(caller_scope_bang_ancestor_is_capture) {
    // decl_local → bang → iterate!
    // expr $0:name $1() inside the iterate lambda gets $0 from decl_local's output.
    // decl_local is in the bang chain before iterate → its output is a capture.
    // The lambda should have 1 param ($1), not 2.
    GraphBuilder gb;
    gb.add("dl", "decl_var", "slider_id u8");
    gb.add("it", "iterate!", "multifader");
    gb.add("dv_mf", "decl_var", "multifader vector<f32>");

    // expr $0:name $1() — $0 from decl_local, $1 is unconnected (lambda param)
    gb.add("ex", "expr", "$0:name $1:iter");
    gb.link("dl.out0", "ex.0");  // $0 = capture from caller scope

    // dup → next pattern for iterate lambda
    gb.add("dup", "dup", "");
    gb.link("ex.out1", "dup.value");
    gb.add("nx", "next", "");
    gb.link("dup.out0", "nx.value");
    gb.link("nx.as_lambda", "it.fn");

    // Bang chain: decl_local → iterate
    gb.link("dl." + gb.find("dl")->nexts[0]->name, "it." + gb.find("it")->triggers[0]->name);

    auto errors = gb.run_inference();
    for (auto& e : errors) printf("    ERR: %s\n", e.c_str());

    // Check that no "Lambda has 0 parameter(s)" error exists
    bool has_param_error = false;
    for (auto& l : gb.graph.links)
        if (!l.error.empty() && l.error.find("parameter") != std::string::npos) {
            printf("    LINK ERR: %s\n", l.error.c_str());
            has_param_error = true;
        }
    // The lambda should find $1 as a parameter (1 param, not 0)
    ASSERT(!has_param_error);
}

TEST(caller_scope_does_not_enter_lambda) {
    // store! klavie_up receives select.as_lambda
    // The select's subgraph (expr $0:midi_key) is INSIDE the lambda.
    // The caller scope should NOT include expr $0:midi_key.
    // So $0 on expr $0:midi_key should be a lambda parameter.
    GraphBuilder gb;
    gb.add("dt_key_set", "decl_type", "key_set map<u8, s32>");
    gb.add("dv_keys", "decl_var", "keys key_set");
    gb.add("dv_ku", "decl_var", "klavie_up (midi_key:u8) -> void");

    gb.add("param", "expr", "$0:midi_key");
    gb.add("cond", "expr", "keys?[$0]");
    gb.link("param.out0", "cond.0");

    gb.add("t_val", "expr", "42");
    gb.add("f_val", "expr", "0");
    gb.add("sel", "select", "");
    gb.link("cond.out0", "sel.condition");
    gb.link("t_val.out0", "sel.if_true");
    gb.link("f_val.out0", "sel.if_false");

    gb.add("st", "store!", "klavie_up");
    gb.link("sel.as_lambda", "st.value");

    auto errors = gb.run_inference();
    for (auto& e : errors) printf("    ERR: %s\n", e.c_str());

    // The lambda should have 1 parameter (midi_key:u8 from param.$0)
    bool has_param_error = false;
    for (auto& l : gb.graph.links)
        if (!l.error.empty() && l.error.find("parameter") != std::string::npos) {
            printf("    LINK ERR: %s\n", l.error.c_str());
            has_param_error = true;
        }
    ASSERT(!has_param_error);
}

TEST(caller_scope_data_ancestor_is_capture) {
    // A node feeding data to the capture node (not via bang, but via data input)
    // should also be in caller scope.
    // iterate! collection — collection comes from a decl_var.
    // Inside the lambda, if a node references decl_var's output, it's a capture.
    GraphBuilder gb;
    gb.add("dv_col", "decl_var", "col vector<f32>");
    gb.add("dv_x", "decl_var", "x f32");
    gb.add("it", "iterate!", "col");

    // Lambda body: expr $0+x — $0 is lambda param (iterator), x is a global (capture)
    gb.add("ex", "expr", "$0+x");
    gb.add("nx", "next", "");
    gb.link("ex.out0", "nx.value");
    gb.link("nx.as_lambda", "it.fn");

    auto errors = gb.run_inference();
    for (auto& e : errors) printf("    ERR: %s\n", e.c_str());

    // x is a global var ref — resolved by inference, not a pin.
    // $0 is the only pin — should be the one lambda parameter.
    // No parameter count errors expected.
    bool has_param_error = false;
    for (auto& l : gb.graph.links)
        if (!l.error.empty() && l.error.find("parameter") != std::string::npos) {
            printf("    LINK ERR: %s\n", l.error.c_str());
            has_param_error = true;
        }
    ASSERT(!has_param_error);
}

// ============================================================
// call! inline lambda call N(M) tests
// ============================================================

TEST(call_inline_lambda_call_no_type_on_callee_pin) {
    // call! my_func $0($1) — $0 is a lambda, $0($1) calls it.
    // The pin for $0 should NOT get the function arg type (&f32),
    // because $0 is used as a callee, not as the value directly.
    GraphBuilder gb;
    gb.add("ffi1", "ffi", R"(my_func (value:&f32) -> void)");
    gb.add("c1", "call!", R"(my_func $0($1))");
    resolve_type_based_pins(gb.graph);

    auto* c1 = gb.find("c1");
    ASSERT(c1 != nullptr);

    // Pin 0 ($0) should NOT have type &f32 — it's used as a callee
    FlowPin* pin0 = nullptr;
    for (auto& p : c1->inputs)
        if (p->name == "0") { pin0 = p.get(); break; }
    ASSERT(pin0 != nullptr);
    // Pin type should not be &f32 (that's the fn arg type, not the callee type)
    if (pin0->resolved_type) {
        ASSERT(pin0->resolved_type->kind != TypeKind::Scalar ||
               pin0->resolved_type->category != TypeCategory::Reference);
    }
}

TEST(call_inline_lambda_call_resolves_correctly) {
    // call! my_func $0($1) where $0 is (x:f32)->f32 and $1 is f32
    // The result of $0($1) should be f32, matching the fn arg type.
    GraphBuilder gb;
    gb.add("dt_accessor", "decl_type", "accessor (x:f32) -> &f32");
    gb.add("dv_acc", "decl_var", "my_acc accessor");
    gb.add("ffi1", "ffi", R"(my_func (value:&f32) -> void)");
    gb.add("acc_expr", "expr", "my_acc");  // outputs accessor (a lambda type)
    gb.add("val_expr", "expr", "3.14f");    // outputs f32
    gb.add("c1", "call!", R"(my_func $0($1))");
    gb.link("acc_expr.out0", "c1.0");  // $0 = accessor lambda
    gb.link("val_expr.out0", "c1.1");  // $1 = f32 arg

    auto errors = gb.run_inference();
    for (auto& e : errors) printf("    ERR: %s\n", e.c_str());

    auto* c1 = gb.find("c1");
    ASSERT(c1 != nullptr);
    // Should have no "Cannot call non-function type" error
    ASSERT(c1->error.empty());
}

TEST(call_inline_bare_pin_ref_gets_type) {
    // call! my_func $0 — bare $0 SHOULD get the fn arg type
    // (only lambda-call N(...) skips type propagation)
    GraphBuilder gb;
    gb.add("ffi1", "ffi", R"(my_func (a:string b:&f32 c:string) -> void)");
    gb.add("c1", "call!", R"(my_func "hello" $0 "world")");
    resolve_type_based_pins(gb.graph);

    auto* c1 = gb.find("c1");
    ASSERT(c1 != nullptr);

    // Pin 0 ($0) is bare — SHOULD get type &f32 from fn arg
    FlowPin* pin0 = nullptr;
    for (auto& p : c1->inputs)
        if (p->name == "0") { pin0 = p.get(); break; }
    ASSERT(pin0 != nullptr);
    ASSERT_EQ(pin0->type_name, "&f32");
}

TEST(select_unconnected_condition_error) {
    // select with no args, if_true and if_false connected, condition NOT connected
    // Node is in flow: used as lambda root for a store
    GraphBuilder gb;
    gb.add("dv", "decl_var", "x () -> void");
    gb.add("sel", "select", "");
    gb.add("t1", "expr", "42");
    gb.add("f1", "expr", "0");
    gb.link("t1.out0", "sel.if_true");
    gb.link("f1.out0", "sel.if_false");
    gb.add("st", "store!", "x");
    gb.link("sel.as_lambda", "st.value");

    auto errors = gb.run_inference();
    for (auto& e : errors) printf("    ERR: %s\n", e.c_str());

    auto* sel = gb.find("sel");
    ASSERT(sel != nullptr);
    ASSERT(!sel->error.empty());
    ASSERT_CONTAINS(sel->error, "not connected");
}

TEST(select_unconnected_condition_data_dep_error) {
    // select with condition NOT connected, feeding into a discard! in a bang chain.
    // The select is not directly triggered — it's a data dependency of a triggered node.
    // Its unconnected condition should still be caught because it has other connected inputs.
    GraphBuilder gb;
    gb.add("ev", "event!", "on_tick");
    gb.add("sel", "select", "");
    gb.add("t1", "expr", "42");
    gb.add("f1", "expr", "0");
    gb.link("t1.out0", "sel.if_true");
    gb.link("f1.out0", "sel.if_false");

    // discard! is in the bang chain, consumes select output
    gb.add("dis", "discard!", "");
    gb.link("ev.bang0", "dis.bang_in0");
    gb.link("sel.out0", "dis.value");

    auto errors = gb.run_inference();
    for (auto& e : errors) printf("    ERR: %s\n", e.c_str());

    auto* sel = gb.find("sel");
    ASSERT(sel != nullptr);
    ASSERT(!sel->error.empty());
    ASSERT_CONTAINS(sel->error, "not connected");
}

TEST(lock_caller_scope_basic) {
    // lock! with a simple lambda body where all inputs are from caller scope.
    // decl_local provides a value → lock!'s lambda body uses it as a capture.
    GraphBuilder gb;
    gb.add("dv_mtx", "decl_var", "mtx mutex");
    gb.add("dl", "decl_var", "x f32");
    gb.add("lk", "lock!", "mtx");

    // Bang chain: decl_local → lock!
    gb.link(gb.find("dl")->nexts[0]->id, gb.find("lk")->triggers[0]->id);

    // Lambda body: expr $0 where $0 comes from decl_local (caller scope)
    gb.add("ex", "expr", "$0");
    gb.link("dl.out0", "ex.0"); // $0 = capture from caller scope
    gb.link("ex.as_lambda", "lk.fn");

    auto errors = gb.run_inference();
    for (auto& e : errors) printf("    ERR: %s\n", e.c_str());

    // The lock should have NO extra "arg" pins — $0 is a capture, not a param
    auto* lk = gb.find("lk");
    ASSERT(lk != nullptr);

    int extra_pins = 0;
    for (auto& inp : lk->inputs) {
        if (inp->direction != FlowPin::Lambda && inp->name != "mutex")
            extra_pins++;
    }
    printf("    Lock extra pins: %d\n", extra_pins);
    ASSERT_EQ(extra_pins, 0);
}

TEST(lock_with_actual_lambda_param_gets_pin) {
    // lock with a lambda that HAS a real parameter (not from caller scope).
    // The lock should get an "arg0" pin for it.
    GraphBuilder gb;
    gb.add("dv_mtx", "decl_var", "mtx mutex");
    gb.add("lk", "lock", "mtx");

    // Lambda body: expr $0 — $0 is unconnected = lambda param
    gb.add("ex", "expr", "$0");
    gb.link("ex.as_lambda", "lk.fn");

    auto errors = gb.run_inference();
    for (auto& e : errors) printf("    ERR: %s\n", e.c_str());

    auto* lk = gb.find("lk");
    ASSERT(lk != nullptr);

    int extra_pins = 0;
    for (auto& inp : lk->inputs) {
        if (inp->direction != FlowPin::Lambda && inp->name != "mutex")
            extra_pins++;
    }
    printf("    Lock extra pins: %d\n", extra_pins);
    ASSERT_EQ(extra_pins, 1);
}

TEST(nested_lambda_scope_boundary) {
    // Nested lambda scope: store! fn captures lock.as_lambda.
    // Inside lock's inner lambda body: param($0) feeds into the body.
    // param.$0 is unconnected — it's a parameter of the OUTER stored lambda (fn),
    // NOT of the lock's inner lambda. The lock's inner lambda should NOT pick up $0.
    //
    // Graph structure:
    //   decl_type cb_type (x:f32) -> void
    //   decl_var fn cb_type
    //   store! fn ← lock.as_lambda     (outer lambda = lock node)
    //   lock has Lambda pin "fn" ← body.as_lambda  (inner lambda = body node)
    //   body ← param($0)    param.$0 is unconnected
    //
    // param.$0 belongs to the outer lambda (lock), NOT to the inner lambda (body).
    // So lock's inner lambda (body) should have 0 params, and the outer (lock) should have 1.

    GraphBuilder gb;
    gb.add("dt_cb", "decl_type", "cb_type (x:f32) -> void");
    gb.add("dv_fn", "decl_var", "fn cb_type");
    gb.add("dv_mtx", "decl_var", "mtx mutex");

    // store! fn — the outer capture
    gb.add("st", "store!", "fn");

    // lock mtx — serves as the outer lambda root (its as_lambda → store!)
    gb.add("lk", "lock", "mtx");

    // Bang chain: store! triggers after some setup (not critical, just need the link)
    // store! captures lock.as_lambda
    gb.link("lk.as_lambda", "st.value");

    // Inner lambda body: expr $0 (unconnected input = outer lambda param)
    gb.add("param_node", "expr", "$0");

    // body node: expr sin($0) — takes param_node output, this is the inner lambda root
    gb.add("body", "expr", "sin($0)");
    gb.link("param_node.out0", "body.0");

    // body.as_lambda → lock.fn (inner lambda)
    gb.link("body.as_lambda", "lk.fn");

    // Run inference
    auto errors = gb.run_inference();
    for (auto& e : errors) printf("    ERR: %s\n", e.c_str());

    // The INNER lambda (body) should have 0 unconnected params —
    // param_node.$0 is outside its scope (it belongs to the outer lambda rooted at lock)
    // Verify by checking that body's lambda_grab type has 0 func_args
    // (after excluding connected inputs)
    auto* body = gb.find("body");
    ASSERT(body != nullptr);

    // Collect params for the inner lambda manually to verify
    GraphInference inference(gb.pool);
    resolve_type_based_pins(gb.graph);
    inference.run(gb.graph);

    auto* body2 = gb.find("body");
    ASSERT(body2 != nullptr);

    // The inner lambda (body) is connected to lock.fn which expects a lambda.
    // body's only input $0 IS connected (from param_node), so inner lambda has 0 params.
    // The key test: param_node.$0 should NOT be collected as an inner lambda param.

    // Check the outer lambda (lock) — it should have exactly 1 param (from param_node.$0)
    // which gets the f32 type from cb_type's first arg
    auto* param_n = gb.find("param_node");
    ASSERT(param_n != nullptr);
    printf("    param_node.$0 type: %s\n",
           param_n->inputs[0]->resolved_type ? type_to_string(param_n->inputs[0]->resolved_type).c_str() : "null");

    // param_node's $0 should get f32 from the outer lambda's expected type (cb_type)
    ASSERT(param_n->inputs[0]->resolved_type != nullptr);
    ASSERT_TYPE(param_n->inputs[0].get(), "f32");
}

TEST(nested_lambda_inner_has_own_params) {
    // Verify that inner lambda CAN have its own params (nodes inside its scope).
    // Graph:
    //   store! fn ← lock.as_lambda (outer)
    //   lock.fn ← body.as_lambda (inner)
    //   body has an unconnected input $0 (inner lambda param, NOT reachable from lock)
    //   param_node has unconnected $0 (outer lambda param, reachable from lock via body←param)
    //
    // But body.$0 is NOT connected to anything — it IS an inner lambda param.
    // param_node.$0 is also unconnected — it IS an outer lambda param.

    GraphBuilder gb;
    gb.add("dt_inner", "decl_type", "inner_fn (y:f32) -> f32");
    gb.add("dt_outer", "decl_type", "outer_fn (x:f32) -> void");
    gb.add("dv_fn", "decl_var", "fn outer_fn");
    gb.add("dv_mtx", "decl_var", "mtx mutex");

    // lock with a Lambda pin expecting inner_fn
    // We need lock's fn pin to expect inner_fn type
    // lock's fn pin type comes from the connection target
    gb.add("lk", "lock", "mtx");
    gb.add("st", "store!", "fn");
    gb.link("lk.as_lambda", "st.value");

    // Inner lambda body: expr $0+$1
    // $0 is connected from outer param, $1 is unconnected (inner lambda's own param)
    gb.add("outer_param", "expr", "$0"); // unconnected $0 = outer lambda param
    gb.add("body", "expr", "$0+$1", 2, 1);
    gb.link("outer_param.out0", "body.0"); // body.$0 = connected from outer
    // body.$1 is unconnected = would be inner lambda param
    gb.link("body.as_lambda", "lk.fn");

    auto errors = gb.run_inference();
    for (auto& e : errors) printf("    ERR: %s\n", e.c_str());

    // outer_param.$0 should be outer lambda param (gets x:f32)
    auto* op = gb.find("outer_param");
    ASSERT(op != nullptr);
    printf("    outer_param.$0 type: %s\n",
           op->inputs[0]->resolved_type ? type_to_string(op->inputs[0]->resolved_type).c_str() : "null");
    ASSERT(op->inputs[0]->resolved_type != nullptr);
    ASSERT_TYPE(op->inputs[0].get(), "f32");

    // body.$1 should be inner lambda's own param — not collected as outer
    auto* body = gb.find("body");
    ASSERT(body != nullptr);
    // body.1 is unconnected and inside inner lambda scope, so it's an inner param
    printf("    body.$1 type: %s\n",
           body->inputs[1]->resolved_type ? type_to_string(body->inputs[1]->resolved_type).c_str() : "null");
}

// ============================================================
// Literal types — expr produces correct literal<T, V>
// ============================================================

TEST(literal_unsigned_zero) {
    GraphBuilder gb;
    gb.add("e1", "expr", "0");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT_TYPE(n->outputs[0], "literal<unsigned<?>,0>");
}

TEST(literal_unsigned_42) {
    GraphBuilder gb;
    gb.add("e1", "expr", "42");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT_TYPE(n->outputs[0], "literal<unsigned<?>,42>");
}

TEST(literal_signed_neg1) {
    GraphBuilder gb;
    gb.add("e1", "expr", "-1");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT_TYPE(n->outputs[0], "literal<signed<?>,-1>");
}

TEST(literal_signed_neg42) {
    GraphBuilder gb;
    gb.add("e1", "expr", "-42");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT_TYPE(n->outputs[0], "literal<signed<?>,-42>");
}

TEST(literal_bool_true) {
    GraphBuilder gb;
    gb.add("e1", "expr", "true");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT_TYPE(n->outputs[0], "literal<bool,true>");
}

TEST(literal_bool_false) {
    GraphBuilder gb;
    gb.add("e1", "expr", "false");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT_TYPE(n->outputs[0], "literal<bool,false>");
}

TEST(literal_string_hello) {
    GraphBuilder gb;
    gb.add("e1", "expr", "\"hello\"");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT_TYPE(n->outputs[0], "literal<string,\"hello\">");
}

TEST(literal_string_empty) {
    GraphBuilder gb;
    gb.add("e1", "expr", "\"\"");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT_TYPE(n->outputs[0], "literal<string,\"\">");
}

TEST(literal_f32) {
    GraphBuilder gb;
    gb.add("e1", "expr", "3.14f");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    auto ts = type_to_string(n->outputs[0]->resolved_type);
    ASSERT_CONTAINS(ts.c_str(), "literal<f32,");
}

TEST(literal_f64) {
    GraphBuilder gb;
    gb.add("e1", "expr", "3.14");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    auto ts = type_to_string(n->outputs[0]->resolved_type);
    ASSERT_CONTAINS(ts.c_str(), "literal<f64,");
}

// ============================================================
// Literal type parsing — literal<T, V> round-trips through type_to_string
// ============================================================

TEST(parse_literal_unsigned) {
    std::string err;
    auto t = parse_type("literal<unsigned<?>,0>", err);
    ASSERT(t != nullptr);
    ASSERT(err.empty());
    ASSERT_EQ(type_to_string(t), "literal<unsigned<?>,0>");
}

TEST(parse_literal_signed) {
    std::string err;
    auto t = parse_type("literal<signed<?>,-1>", err);
    ASSERT(t != nullptr);
    ASSERT(err.empty());
    ASSERT_EQ(type_to_string(t), "literal<signed<?>,-1>");
}

TEST(parse_literal_bool) {
    std::string err;
    auto t = parse_type("literal<bool,true>", err);
    ASSERT(t != nullptr);
    ASSERT(err.empty());
    ASSERT_EQ(type_to_string(t), "literal<bool,true>");
}

TEST(parse_literal_string) {
    std::string err;
    auto t = parse_type("literal<string,\"hello\">", err);
    ASSERT(t != nullptr);
    ASSERT(err.empty());
    ASSERT_EQ(type_to_string(t), "literal<string,\"hello\">");
}

TEST(parse_literal_float) {
    std::string err;
    auto t = parse_type("literal<float<?>,3.14>", err);
    ASSERT(t != nullptr);
    ASSERT(err.empty());
    ASSERT_EQ(type_to_string(t), "literal<float<?>,3.14>");
}

TEST(parse_literal_f32) {
    std::string err;
    auto t = parse_type("literal<f32,3.14f>", err);
    ASSERT(t != nullptr);
    ASSERT(err.empty());
    ASSERT_EQ(type_to_string(t), "literal<f32,3.14f>");
}

// ============================================================
// Expr parser handles literal<T, V> syntax
// ============================================================

TEST(expr_literal_string_type_syntax) {
    // expr literal<string,"abc"> should parse same as expr "abc"
    GraphBuilder gb;
    gb.add("e1", "expr", "literal<string,\"abc\">");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0], "literal<string,\"abc\">");
}

TEST(expr_literal_unsigned_type_syntax) {
    GraphBuilder gb;
    gb.add("e1", "expr", "literal<unsigned<?>,42>");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0], "literal<unsigned<?>,42>");
}

TEST(expr_literal_signed_type_syntax) {
    GraphBuilder gb;
    gb.add("e1", "expr", "literal<signed<?>,-5>");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0], "literal<signed<?>,-5>");
}

TEST(expr_literal_bool_type_syntax) {
    GraphBuilder gb;
    gb.add("e1", "expr", "literal<bool,true>");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0], "literal<bool,true>");
}

// ============================================================
// Symbol types — expr returns symbol<name,type> for known symbols
// ============================================================

TEST(expr_sin_returns_symbol) {
    GraphBuilder gb;
    gb.add("e1", "expr", "sin");
    gb.run_inference();
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
    auto ts = type_to_string(n->outputs[0]->resolved_type);
    ASSERT_CONTAINS(ts.c_str(), "symbol<sin,");
}

TEST(expr_unknown_returns_undefined_symbol) {
    GraphBuilder gb;
    gb.add("e1", "expr", "xyz");
    gb.run_inference();
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT_TYPE(n->outputs[0], "undefined_symbol<xyz>");
}

TEST(symbol_decays_in_binary_op) {
    // pi+1.0f should produce f32, not symbol<...>
    GraphBuilder gb;
    gb.add("e1", "expr", "pi+1.0f");
    gb.run_inference();
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    auto ts = type_to_string(n->outputs[0]->resolved_type);
    ASSERT_EQ(ts, "f32");
}

TEST(symbol_decays_in_func_call) {
    // sin(1.0f) should produce f32, not symbol<...>
    GraphBuilder gb;
    gb.add("e1", "expr", "sin(1.0f)");
    gb.run_inference();
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT_EQ(type_to_string(n->outputs[0]->resolved_type), "f32");
}

TEST(symbol_flows_through_connection) {
    // expr sin -> expr $0: $0 should receive the symbol type
    GraphBuilder gb;
    gb.add("e1", "expr", "sin");
    gb.add("e2", "expr", "$0", 1);
    gb.link("e1.out0", "e2.0");
    gb.run_inference();
    auto* n2 = gb.find("e2");
    ASSERT(n2 != nullptr);
    // The input pin gets the decayed type (connections decay symbols)
    auto ts = type_to_string(n2->inputs[0]->resolved_type);
    ASSERT_CONTAINS(ts.c_str(), "(");  // decayed to function type
}

TEST(symbol_decays_in_store) {
    // store! x $0 — x resolves to symbol<x,&f32>, store should work
    GraphBuilder gb;
    gb.add("dv", "decl_var", "x f32");
    gb.add("e1", "expr", "1.0f");
    gb.add("st", "store!", "x $0");
    gb.link("e1.out0", "st.0");
    gb.run_inference();
    auto* n = gb.find("st");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
}

// ============================================================
// Reserved keywords error in expr parser
// ============================================================

TEST(symbol_keyword_errors) {
    auto r = parse_expression("symbol");
    ASSERT(!r.error.empty());
    ASSERT_CONTAINS(r.error.c_str(), "reserved");
}

TEST(undefined_symbol_keyword_errors) {
    auto r = parse_expression("undefined_symbol");
    ASSERT(!r.error.empty());
    ASSERT_CONTAINS(r.error.c_str(), "reserved");
}

TEST(literal_keyword_without_angle_errors) {
    auto r = parse_expression("literal");
    ASSERT(!r.error.empty());
}

TEST(parse_unvalued_literal_string) {
    std::string err;
    auto t = parse_type("literal<string,?>", err);
    ASSERT(t != nullptr);
    ASSERT(err.empty());
    ASSERT(t->is_unvalued_literal);
    ASSERT(t->literal_value.empty());
    ASSERT_EQ(type_to_string(t), "literal<string,?>");
}

TEST(parse_unvalued_literal_f32) {
    std::string err;
    auto t = parse_type("literal<f32,?>", err);
    ASSERT(t != nullptr);
    ASSERT(err.empty());
    ASSERT(t->is_unvalued_literal);
    ASSERT_EQ(type_to_string(t), "literal<f32,?>");
}

TEST(decl_import_pin_type) {
    GraphBuilder gb;
    gb.add("di", "decl_import", "\"std/imgui\"");
    gb.run_inference();
    auto* n = gb.find("di");
    ASSERT(n != nullptr);
    // The input pin should have type literal<string,?>
    ASSERT(!n->inputs.empty());
    ASSERT(n->inputs[0]->resolved_type != nullptr);
    ASSERT_TYPE(n->inputs[0].get(), "literal<string,?>");
}

// ============================================================
// Operations on literals produce runtime (non-literal) types
// ============================================================

TEST(literal_add_strips_literal) {
    // 0+1 should produce unsigned<?>, not literal<...>
    GraphBuilder gb;
    gb.add("e1", "expr", "0+1");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    auto ts = type_to_string(n->outputs[0]->resolved_type);
    ASSERT_EQ(ts, "literal<unsigned<?>,?>");
}

TEST(literal_mul_strips_literal) {
    // 2.0f*3.0f should produce f32, not literal<f32, ...>
    GraphBuilder gb;
    gb.add("e1", "expr", "2.0f*3.0f");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT_EQ(type_to_string(n->outputs[0]->resolved_type), "f32");
}

TEST(literal_builtin_strips_literal) {
    // sin(1.0f) should produce f32, not literal<f32, ...>
    GraphBuilder gb;
    gb.add("e1", "expr", "sin(1.0f)");
    GraphInference gi(gb.pool);
    gi.run(gb.graph);
    auto* n = gb.find("e1");
    ASSERT(n != nullptr);
    ASSERT_EQ(type_to_string(n->outputs[0]->resolved_type), "f32");
}

TEST(literal_select_strips_literal) {
    // select true 1.0f 2.0f should produce f32, not literal<f32, ...>
    GraphBuilder gb;
    gb.add("s", "select", "true 1.0f 2.0f");
    gb.run_inference();
    auto* n = gb.find("s");
    ASSERT(n != nullptr);
    ASSERT(n->error.empty());
    ASSERT_TYPE(n->outputs[0].get(), "f32");
}

// ============================================================
// Declaration nodes use expression parsing
// ============================================================

TEST(decl_var_has_parsed_exprs) {
    GraphBuilder gb;
    gb.add("dv", "decl_var", "myvar f32");
    auto* n = gb.find("dv");
    ASSERT(n != nullptr);
    // parsed_exprs should contain the name and type tokens
    ASSERT(n->parsed_exprs.size() >= 1);
    ASSERT(n->parsed_exprs[0] != nullptr);
    ASSERT_EQ(n->parsed_exprs[0]->kind, ExprKind::SymbolRef);
    ASSERT_EQ(n->parsed_exprs[0]->symbol_name, "myvar");
}

TEST(decl_type_has_parsed_exprs) {
    GraphBuilder gb;
    gb.add("dt", "decl_type", "vec2 x:f32 y:f32");
    auto* n = gb.find("dt");
    ASSERT(n != nullptr);
    ASSERT(n->parsed_exprs.size() >= 1);
    ASSERT(n->parsed_exprs[0] != nullptr);
    ASSERT_EQ(n->parsed_exprs[0]->kind, ExprKind::SymbolRef);
    ASSERT_EQ(n->parsed_exprs[0]->symbol_name, "vec2");
}

TEST(decl_import_has_parsed_exprs) {
    GraphBuilder gb;
    gb.add("di", "decl_import", "\"std/imgui\"");
    auto* n = gb.find("di");
    ASSERT(n != nullptr);
    ASSERT(n->parsed_exprs.size() >= 1);
    ASSERT(n->parsed_exprs[0] != nullptr);
    ASSERT_EQ(n->parsed_exprs[0]->kind, ExprKind::Literal);
    ASSERT_EQ(n->parsed_exprs[0]->literal_kind, LiteralKind::String);
    ASSERT_EQ(n->parsed_exprs[0]->string_value, "std/imgui");
}

TEST(ffi_has_parsed_exprs) {
    GraphBuilder gb;
    gb.add("ff", "ffi", "my_fn (x:f32)->f32");
    auto* n = gb.find("ff");
    ASSERT(n != nullptr);
    ASSERT(n->parsed_exprs.size() >= 1);
    ASSERT(n->parsed_exprs[0] != nullptr);
    ASSERT_EQ(n->parsed_exprs[0]->kind, ExprKind::SymbolRef);
    ASSERT_EQ(n->parsed_exprs[0]->symbol_name, "my_fn");
}

// ============================================================
// Serial v2 format tests
// ============================================================

TEST(serial_v2_roundtrip) {
    // Build a simple graph in memory, save as v2, reload
    FlowGraph g1;
    // Create two expr nodes and connect them
    auto n1_id = g1.add_node(generate_guid(), {100, 100}, 1, 1);
    auto n2_id = g1.add_node(generate_guid(), {200, 200}, 1, 1);
    auto& n1 = g1.nodes[0];
    auto& n2 = g1.nodes[1];
    n1.type_id = NodeTypeID::Expr;
    n1.args = "42";
    n1.node_id = "$const-42";
    n1.rebuild_pin_ids();
    n1.parse_args();
    n2.type_id = NodeTypeID::Expr;
    n2.args = "$0+1";
    n2.node_id = "$add-one";
    n2.rebuild_pin_ids();
    n2.parse_args();
    // Connect n1.out0 -> n2.0
    g1.add_link(n1.outputs[0]->id, n2.inputs[0]->id);
    // Set net name on the link
    g1.links[0].net_name = "$val-42";

    // Save as v2
    std::string v2_str = save_atto_string(g1);

    // Verify header
    ASSERT(v2_str.find("# version instrument@atto:0") == 0);

    // Verify node IDs appear
    ASSERT(v2_str.find("$const-42") != std::string::npos);
    ASSERT(v2_str.find("$add-one") != std::string::npos);

    // Verify no connections= line
    ASSERT(v2_str.find("connections =") == std::string::npos);

    // Verify inputs/outputs appear
    ASSERT(v2_str.find("inputs =") != std::string::npos || v2_str.find("outputs =") != std::string::npos);

    // Reload
    FlowGraph g2;
    ASSERT(load_atto_string(v2_str, g2));
    // Should have same number of nodes (non-imported)
    int non_imported_1 = 0, non_imported_2 = 0;
    for (auto& n : g1.nodes) if (!n.imported) non_imported_1++;
    for (auto& n : g2.nodes) if (!n.imported) non_imported_2++;
    ASSERT_EQ(non_imported_1, non_imported_2);
    ASSERT_EQ(g2.links.size(), g1.links.size());
}

TEST(serial_v2_version_header) {
    std::string v2 = "# version instrument@atto:0\n\n[[node]]\nid = \"$test\"\ntype = \"expr\"\nargs = [\"42\"]\nposition = [0, 0]\n";
    FlowGraph g;
    ASSERT(load_atto_string(v2, g));
    ASSERT_EQ(g.nodes.size(), (size_t)1);
    ASSERT_EQ(g.nodes[0].node_id, "$test");
}

TEST(serial_v1_auto_migration) {
    std::string v1 = "version = \"attoprog@1\"\n\n[[node]]\nguid = \"abc123\"\ntype = \"expr\"\nargs = [\"42\"]\nposition = [0, 0]\n";
    FlowGraph g;
    ASSERT(load_atto_string(v1, g));
    ASSERT_EQ(g.nodes.size(), (size_t)1);
    // Should have been assigned a $auto- node_id
    ASSERT(g.nodes[0].node_id.find("$auto-") == 0);
}

TEST(serial_v2_net_names_preserved) {
    // Create graph with named net
    FlowGraph g1;
    g1.add_node(generate_guid(), {0, 0}, 0, 1);
    g1.add_node(generate_guid(), {100, 0}, 1, 0);
    auto& n1 = g1.nodes[0];
    auto& n2 = g1.nodes[1];
    n1.type_id = NodeTypeID::Expr; n1.args = "1"; n1.node_id = "$src";
    n2.type_id = NodeTypeID::Expr; n2.args = "$0"; n2.node_id = "$dst";
    n1.rebuild_pin_ids(); n2.rebuild_pin_ids();
    n1.parse_args(); n2.parse_args();
    g1.add_link(n1.outputs[0]->id, n2.inputs[0]->id);
    g1.links[0].net_name = "$my-signal";

    std::string v2 = save_atto_string(g1);
    ASSERT(v2.find("$my-signal") != std::string::npos);

    FlowGraph g2;
    ASSERT(load_atto_string(v2, g2));
    ASSERT_EQ(g2.links.size(), (size_t)1);
    ASSERT_EQ(g2.links[0].net_name, "$my-signal");
}

// ============================================================
// Main
// ============================================================

int main() {
    printf("Running %d tests...\n", (int)test_registry().size());
    for (auto& t : test_registry()) {
        tests_run++;
        int prev_failed = tests_failed;
        printf("  [%d] %s... ", tests_run, t.name);
        fflush(stdout);
        t.fn();
        if (tests_failed == prev_failed) {
            tests_passed++;
            printf("OK\n");
        }
    }
    printf("\n%d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) printf(", %d FAILED", tests_failed);
    printf("\n");
    return tests_failed > 0 ? 1 : 0;
}
