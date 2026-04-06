#pragma once
#include <cstring>
#include <cstdint>

// Node type enum — order must match NODE_TYPES array exactly
enum class NodeTypeID : uint8_t {
    Expr,           // 0
    Select,         // 1
    New,            // 2
    Dup,            // 3
    Str,            // 4
    Void,           // 5
    DiscardBang,    // 6
    Discard,        // 7
    DeclType,       // 8
    DeclVar,        // 9
    Decl,           // 10 — compile-time entry point (was DeclLocal)
    DeclEvent,      // 11
    DeclImport,     // 12
    Ffi,            // 13
    Call,           // 14
    CallBang,       // 15
    Erase,          // 16
    OutputMixBang,  // 17
    Append,         // 18
    AppendBang,     // 19
    Store,          // 20
    StoreBang,      // 21
    EventBang,      // 22
    OnKeyDownBang,  // 23
    OnKeyUpBang,    // 24
    SelectBang,     // 25
    ExprBang,       // 26
    EraseBang,      // 27
    Iterate,        // 28
    IterateBang,    // 29
    Next,           // 30
    Lock,           // 31
    LockBang,       // 32
    ResizeBang,     // 33
    Cast,           // 34
    Label,          // 35
    Deref,          // 36 — internal: dereference iterator to value (shadow node only)
    Error,          // 37 — error node: displays original args, no pins (like label)
    COUNT,
    Unknown = 255
};

// Variadic helper for multi-type checks
template<typename... Ts>
constexpr bool is_any_of(NodeTypeID id, Ts... ids) {
    return ((id == ids) || ...);
}

// String accessor for display/serialization
static const char* node_type_str(NodeTypeID id);

// Lookup by string (for deserialization)
static NodeTypeID node_type_id_from_string(const char* name);

// Port descriptor for named/documented ports
enum class PortKind { Data, Lambda };
struct PortDesc { const char* name; const char* desc; PortKind kind = PortKind::Data; const char* type_name = nullptr; };

// Known node type descriptor
struct NodeType {
    NodeTypeID type_id;
    const char* name;
    const char* desc;
    int num_triggers; int inputs; int num_nexts; int outputs;
    bool is_event;
    bool no_post_bang;
    bool has_lambda;
    bool is_declaration;
    const PortDesc* trigger_ports;      // array of num_triggers entries (or nullptr)
    const PortDesc* input_ports;        // array of inputs entries (or nullptr)
    const PortDesc* next_ports;         // array of num_nexts entries (or nullptr)
    const PortDesc* output_ports;       // array of outputs entries (or nullptr)
};

// Port descriptor arrays
static const PortDesc P_VALUE[]      = {{"value", "input value", PortKind::Data, "value"}};
static const PortDesc P_RESULT[]     = {{"result", "result value", PortKind::Data, "value"}};
static const PortDesc P_BANG_TRIG[]  = {{"bang", "trigger output", PortKind::Data, "bang"}};
static const PortDesc P_BANG_IN[]    = {{"bang", "trigger input", PortKind::Data, "bang"}};
static const PortDesc P_KEY_EVENT[]  = {{"on_key_down", "fired on key press", PortKind::Data, "bang"}};
static const PortDesc P_KEY_UP_EVENT[] = {{"on_key_up", "fired on key release", PortKind::Data, "bang"}};
static const PortDesc P_KEY_OUTS[]   = {{"midi_key_number", "MIDI note number", PortKind::Data, "u8"}, {"key_frequency", "frequency in Hz", PortKind::Data, "f32"}};
static const PortDesc P_ITEM[]       = {{"item", "item to add/remove/store", PortKind::Data, "value"}};
static const PortDesc P_STORE_IN[]   = {{"target", "variable/reference to store into", PortKind::Data, "value"}, {"value", "value to store", PortKind::Data, "value"}};
static const PortDesc P_APPEND_IN[]  = {{"target", "collection to append to", PortKind::Data, "value"}, {"value", "value to append", PortKind::Data, "value"}};
static const PortDesc P_ERASE_IN[]   = {{"target", "collection to erase from", PortKind::Data, "value"}, {"key", "key, value, or iterator to erase", PortKind::Data, "value"}};
static const PortDesc P_COND_IN[]    = {{"condition", "boolean condition", PortKind::Data, "bool"}};
static const PortDesc P_COND_BANG[]  = {{"next", "fires after true/false completes", PortKind::Data, "bang"}, {"true", "fires when condition is true", PortKind::Data, "bang"}, {"false", "fires when condition is false", PortKind::Data, "bang"}};
static const PortDesc P_SELECT_IN[]  = {{"condition", "boolean selector", PortKind::Data, "bool"}, {"if_true", "value when true", PortKind::Data, "value"}, {"if_false", "value when false", PortKind::Data, "value"}};
static const PortDesc P_DECL_VAR_IN[]  = {{"name", "variable name (symbol)"}, {"type", "variable type"}};
static const PortDesc P_DECL_VAR_OUT[] = {{"ref", "reference to variable", PortKind::Data, "value"}};
static const PortDesc P_DECL_TYPE_IN[] = {{"name", "type name (symbol)"}, {"type", "type definition"}};
static const PortDesc P_DECL_TYPE_OUT[]= {{"type", "the declared type", PortKind::Data, "value"}};
static const PortDesc P_DECL_SYM_IN[] = {{"name", "symbol name"}};
static const PortDesc P_DECL_IMPORT_IN[] = {{"path", "module path", PortKind::Data, "literal<string,?>"}};
static const PortDesc P_DECL_SYM_TYPE_IN[] = {{"name", "symbol name"}, {"type", "function type"}};
static const PortDesc P_ITERATE_IN[] = {{"collection", "collection to iterate over", PortKind::Data, "collection"}, {"fn", "it=fn(it); while it!=end", PortKind::Lambda, "lambda"}};
static const PortDesc P_LOCK_IN[]   = {{"mutex", "mutex to lock", PortKind::Data, "&mutex"}, {"fn", "body to execute under lock", PortKind::Lambda, "lambda"}};
static const PortDesc P_RESIZE_IN[] = {{"target", "vector to resize", PortKind::Data, "value"}, {"size", "new size", PortKind::Data, "s32"}};

static const NodeType NODE_TYPES[] = {
    {NodeTypeID::Expr,          "expr",       "Evaluate expression",                 0,0, 0,1, false,false,true, false, nullptr, nullptr, nullptr, P_RESULT},
    {NodeTypeID::Select,        "select",     "Select value by condition",            0,3, 0,1, false,false,true, false, nullptr, P_SELECT_IN, nullptr, P_RESULT},
    {NodeTypeID::New,           "new",        "Instantiate a type",                   0,0, 0,1, false,false,true, false, nullptr, nullptr, nullptr, P_RESULT},
    {NodeTypeID::Dup,           "dup",        "Duplicate input to output",           0,1, 0,1, false,false,true, false, nullptr, P_VALUE, nullptr, P_RESULT},
    {NodeTypeID::Str,           "str",        "Convert to string",                   0,1, 0,1, false,false,true, false, nullptr, P_VALUE, nullptr, P_RESULT},
    {NodeTypeID::Void,          "void",       "Void result (no-op)",                 0,0, 0,1, false,false,true, false, nullptr, nullptr, nullptr, P_RESULT},
    {NodeTypeID::DiscardBang,   "discard!",   "Discard value, pass bang",             1,1, 1,0, false,true, false,false, P_BANG_IN, P_VALUE, P_BANG_TRIG, nullptr},
    {NodeTypeID::Discard,       "discard",    "Discard input values",               0,1, 0,0, false,false,true, false, nullptr, P_VALUE, nullptr, nullptr},
    {NodeTypeID::DeclType,      "decl_type",  "Declare a type",                      1,2, 1,1, false,true, false,true,  P_BANG_IN, P_DECL_TYPE_IN, P_BANG_TRIG, P_DECL_TYPE_OUT},
    {NodeTypeID::DeclVar,       "decl_var",   "Declare a variable",                  1,2, 1,1, false,true, false,true,  P_BANG_IN, P_DECL_VAR_IN, P_BANG_TRIG, P_DECL_VAR_OUT},
    {NodeTypeID::Decl,          "decl",       "Compile-time entry point",            0,0, 1,0, false,true, false,true,  nullptr, nullptr, P_BANG_TRIG, nullptr},
    {NodeTypeID::DeclEvent,     "decl_event", "Declare event: name fn_type",         1,2, 1,0, false,true, false,true,  P_BANG_IN, P_DECL_SYM_TYPE_IN, P_BANG_TRIG, nullptr},
    {NodeTypeID::DeclImport,    "decl_import","Import module: \"std/module\"",          1,1, 1,0, false,true, false,true,  P_BANG_IN, P_DECL_IMPORT_IN, P_BANG_TRIG, nullptr},
    {NodeTypeID::Ffi,           "ffi",        "Declare external function: name type", 1,2, 1,0, false,true, false,true,  P_BANG_IN, P_DECL_SYM_TYPE_IN, P_BANG_TRIG, nullptr},
    {NodeTypeID::Call,          "call",       "Call function with arguments",         0,0, 0,0, false,false,true, false, nullptr, nullptr, nullptr, nullptr},
    {NodeTypeID::CallBang,      "call!",      "Call function with arguments (bang)",   1,0, 1,0, false,true, false,false, P_BANG_IN, nullptr, P_BANG_TRIG, nullptr},
    {NodeTypeID::Erase,         "erase",      "Erase from collection",               0,2, 0,1, false,false,false,false, nullptr, P_ERASE_IN, nullptr, P_RESULT},
    {NodeTypeID::OutputMixBang, "output_mix!","Mix into audio output",                1,1, 0,0, false,false,false,false, P_BANG_IN, P_VALUE, nullptr, nullptr},
    {NodeTypeID::Append,        "append",     "Append item to collection",            0,2, 0,1, false,false,true, false, nullptr, P_APPEND_IN, nullptr, P_RESULT},
    {NodeTypeID::AppendBang,    "append!",    "Append item to collection",           1,2, 1,1, false,true, true, false, P_BANG_IN, P_APPEND_IN, P_BANG_TRIG, P_RESULT},
    {NodeTypeID::Store,         "store",      "Store value into variable/reference",  0,2, 0,0, false,false,true, false, nullptr, P_STORE_IN, nullptr, nullptr},
    {NodeTypeID::StoreBang,     "store!",     "Store value into variable/reference",  1,2, 1,0, false,true, false,false, P_BANG_IN, P_STORE_IN, P_BANG_TRIG, nullptr},
    {NodeTypeID::EventBang,     "event!",     "Event source (args from decl_event)",  0,0, 1,0, false,true, false,false, nullptr, nullptr, P_BANG_TRIG, nullptr},
    {NodeTypeID::OnKeyDownBang, "on_key_down!","Klavier key press event",             0,0, 1,2, true, true, false,false, nullptr, nullptr, P_KEY_EVENT, P_KEY_OUTS},
    {NodeTypeID::OnKeyUpBang,   "on_key_up!", "Klavier key release event",            0,0, 1,2, true, true, false,false, nullptr, nullptr, P_KEY_UP_EVENT, P_KEY_OUTS},
    {NodeTypeID::SelectBang,    "select!",    "Branch on condition",                   1,1, 3,0, false,true, false,false, P_BANG_IN, P_COND_IN, P_COND_BANG, nullptr},
    {NodeTypeID::ExprBang,      "expr!",      "Evaluate expression on bang",          1,0, 1,0, false,true, false,false, P_BANG_IN, nullptr, P_BANG_TRIG, nullptr},
    {NodeTypeID::EraseBang,     "erase!",     "Erase from collection",               1,2, 1,1, false,true, false,false, P_BANG_IN, P_ERASE_IN, P_BANG_TRIG, P_RESULT},
    {NodeTypeID::Iterate,       "iterate",    "it=first; while it!=end: it=fn(it)",   0,2, 0,0, false,false,true, false, nullptr, P_ITERATE_IN, nullptr, nullptr},
    {NodeTypeID::IterateBang,   "iterate!",   "it=first; while it!=end: it=fn(it)",   1,2, 1,0, false,true, false,false, P_BANG_IN, P_ITERATE_IN, P_BANG_TRIG, nullptr},
    {NodeTypeID::Next,          "next",       "Advance iterator to next element",     0,1, 0,1, false,false,true, false, nullptr, P_VALUE, nullptr, P_RESULT},
    {NodeTypeID::Lock,          "lock",       "Execute lambda under mutex lock",      0,2, 0,0, false,false,true, false, nullptr, P_LOCK_IN, nullptr, nullptr},
    {NodeTypeID::LockBang,      "lock!",      "Execute lambda under mutex lock (bang)",1,2, 1,0, false,true, false,false, P_BANG_IN, P_LOCK_IN, P_BANG_TRIG, nullptr},
    {NodeTypeID::ResizeBang,    "resize!",    "Resize vector",                         1,2, 1,0, false,true, false,false, P_BANG_IN, P_RESIZE_IN, P_BANG_TRIG, nullptr},
    {NodeTypeID::Cast,          "cast",       "Cast value to type",                    0,1, 0,1, false,false,false,false, nullptr, P_VALUE, nullptr, P_RESULT},
    {NodeTypeID::Label,         "label",      "Text label (no connections)",           0,0, 0,0, false,true, false,false, nullptr, nullptr, nullptr, nullptr},
    {NodeTypeID::Deref,         "deref",      "Dereference iterator (internal)",       0,1, 0,1, false,false,false,false, nullptr, P_VALUE, nullptr, P_RESULT},
    {NodeTypeID::Error,         "error",      "Error: invalid node",                   0,0, 0,0, false,false,false,false, nullptr, nullptr, nullptr, nullptr},
};
static constexpr int NUM_NODE_TYPES = sizeof(NODE_TYPES) / sizeof(NODE_TYPES[0]);

static const char* node_type_str(NodeTypeID id) {
    if (static_cast<uint8_t>(id) < NUM_NODE_TYPES) return NODE_TYPES[static_cast<uint8_t>(id)].name;
    return "unknown";
}

static NodeTypeID node_type_id_from_string(const char* name) {
    for (int i = 0; i < NUM_NODE_TYPES; i++)
        if (strcmp(NODE_TYPES[i].name, name) == 0) return NODE_TYPES[i].type_id;
    return NodeTypeID::Unknown;
}

static const NodeType* find_node_type(const char* name) {
    for (int i = 0; i < NUM_NODE_TYPES; i++)
        if (strcmp(NODE_TYPES[i].name, name) == 0) return &NODE_TYPES[i];
    return nullptr;
}

static const NodeType* find_node_type(NodeTypeID id) {
    if (static_cast<uint8_t>(id) < NUM_NODE_TYPES) return &NODE_TYPES[static_cast<uint8_t>(id)];
    return nullptr;
}
