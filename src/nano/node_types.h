#pragma once
#include <cstring>

// Port descriptor for named/documented ports
enum class PortKind { Data, Lambda };
struct PortDesc { const char* name; const char* desc; PortKind kind = PortKind::Data; const char* type_name = nullptr; };

// Known node type descriptor
struct NodeType {
    const char* name;
    const char* desc;
    int bang_inputs; int inputs; int bang_outputs; int outputs;
    bool is_event;
    bool no_post_bang;
    bool has_lambda;
    bool is_declaration;
    const PortDesc* bang_input_ports;   // array of bang_inputs entries (or nullptr)
    const PortDesc* input_ports;        // array of inputs entries (or nullptr)
    const PortDesc* bang_output_ports;  // array of bang_outputs entries (or nullptr)
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
static const PortDesc P_COND_BANG[]  = {{"true", "fires when condition is true", PortKind::Data, "bang"}, {"false", "fires when condition is false", PortKind::Data, "bang"}};
static const PortDesc P_SELECT_IN[]  = {{"condition", "boolean selector", PortKind::Data, "bool"}, {"if_true", "value when true", PortKind::Data, "value"}, {"if_false", "value when false", PortKind::Data, "value"}};
static const PortDesc P_LOCAL_IN[]   = {{"name", "variable name"}, {"type", "variable type"}};
static const PortDesc P_ITERATE_IN[] = {{"collection", "collection to iterate over", PortKind::Data, "collection"}, {"fn", "it=fn(it); while it!=end", PortKind::Lambda, "lambda"}};
static const PortDesc P_LOCK_IN[]   = {{"mutex", "mutex to lock", PortKind::Data, "&mutex"}, {"fn", "body to execute under lock", PortKind::Lambda, "lambda"}};

static const NodeType NODE_TYPES[] = {
    {"expr",   "Evaluate expression",                 0,0, 0,1, false,false,true, false, nullptr, nullptr, nullptr, P_RESULT},
    {"select", "Select value by condition",            0,3, 0,1, false,false,true, false, nullptr, P_SELECT_IN, nullptr, P_RESULT},
    {"new",    "Instantiate a type",                   0,0, 0,1, false,false,true, false, nullptr, nullptr, nullptr, P_RESULT},
    {"dup",    "Duplicate input to output",           0,1, 0,1, false,false,true, false, nullptr, P_VALUE, nullptr, P_RESULT},
    {"void",   "Void result (no-op)",                 0,0, 0,1, false,false,true, false, nullptr, nullptr, nullptr, P_RESULT},
    {"discard!","Discard value, pass bang",             1,1, 1,0, false,true, false,false, P_BANG_IN, P_VALUE, P_BANG_TRIG, nullptr},
    {"discard","Discard input values",               0,1, 0,0, false,false,true, false, nullptr, P_VALUE, nullptr, nullptr},
    {"decl_type", "Declare a type",                    0,0, 0,0, false,false,false,true,  nullptr, nullptr, nullptr, nullptr},
    {"decl_var",  "Declare a variable",                0,0, 0,0, false,false,false,true,  nullptr, nullptr, nullptr, nullptr},
    {"decl_local","Declare local: name type", 1,2, 1,1, false,true, false,false, P_BANG_IN, P_LOCAL_IN, P_BANG_TRIG, P_RESULT},
    {"decl_event","Declare event: name fn_type|(args)->ret", 0,0, 0,0, false,false,false,true, nullptr, nullptr, nullptr, nullptr},
    {"decl_import","Import namespace: std/<module>", 0,0, 0,0, false,false,false,true, nullptr, nullptr, nullptr, nullptr},
    {"ffi",    "Declare external function: name type", 0,0, 0,0, false,false,false,true, nullptr, nullptr, nullptr, nullptr},
    {"call",   "Call function with arguments",         0,0, 0,0, false,false,true, false, nullptr, nullptr, nullptr, nullptr},
    {"call!",  "Call function with arguments (bang)",   1,0, 1,0, false,true, false,false, P_BANG_IN, nullptr, P_BANG_TRIG, nullptr},
    {"erase","Erase from collection",                 0,2, 0,1, false,false,false,false, nullptr, P_ERASE_IN, nullptr, P_RESULT},
    {"output_mix!","Mix into audio output",            1,1, 0,0, false,false,false,false, P_BANG_IN, P_VALUE, nullptr, nullptr},
    {"append", "Append item to collection",            0,2, 0,1, false,false,true, false, nullptr, P_APPEND_IN, nullptr, P_RESULT},
    {"append!","Append item to collection",           1,2, 0,1, false,false,true, false, P_BANG_IN, P_APPEND_IN, nullptr, P_RESULT},
    {"store",  "Store value into variable/reference",  0,2, 0,0, false,false,true, false, nullptr, P_STORE_IN, nullptr, nullptr},
    {"store!", "Store value into variable/reference",  1,2, 1,0, false,true, false,false, P_BANG_IN, P_STORE_IN, P_BANG_TRIG, nullptr},
    {"event!", "Event source (args from decl_event)",  0,0, 1,0, false,true, false,false, nullptr, nullptr, P_BANG_TRIG, nullptr},
    {"on_key_down!","Klavier key press event",        0,0, 1,2, true, true, false,false, nullptr, nullptr, P_KEY_EVENT, P_KEY_OUTS},
    {"on_key_up!",  "Klavier key release event",      0,0, 1,2, true, true, false,false, nullptr, nullptr, P_KEY_UP_EVENT, P_KEY_OUTS},
    {"select!",  "Branch on condition",                 1,1, 2,0, false,true, false,false, P_BANG_IN, P_COND_IN, P_COND_BANG, nullptr},
    {"expr!",  "Evaluate expression on bang",         1,0, 1,0, false,true, false,false, P_BANG_IN, nullptr, P_BANG_TRIG, nullptr},
    {"erase!", "Erase from collection",              1,2, 1,1, false,true, false,false, P_BANG_IN, P_ERASE_IN, P_BANG_TRIG, P_RESULT},
    {"iterate", "it=first; while it!=end: it=fn(it)",  0,2, 0,0, false,false,true, false, nullptr, P_ITERATE_IN, nullptr, nullptr},
    {"iterate!","it=first; while it!=end: it=fn(it)",  1,2, 1,0, false,true, false,false, P_BANG_IN, P_ITERATE_IN, P_BANG_TRIG, nullptr},
    {"next",   "Advance iterator to next element",     0,1, 0,1, false,false,false,false, nullptr, P_VALUE, nullptr, P_RESULT},
    {"lock",   "Execute lambda under mutex lock",      0,2, 0,0, false,false,true, false, nullptr, P_LOCK_IN, nullptr, nullptr},
    {"lock!",  "Execute lambda under mutex lock (bang)",1,2, 1,0, false,true, false,false, P_BANG_IN, P_LOCK_IN, P_BANG_TRIG, nullptr},
    {"cast",   "Cast value to type",                    0,1, 0,1, false,false,false,false, nullptr, P_VALUE, nullptr, P_RESULT},
    {"label",  "Text label (no connections)",          0,0, 0,0, false,true, false,false, nullptr, nullptr, nullptr, nullptr},
};
static constexpr int NUM_NODE_TYPES = sizeof(NODE_TYPES) / sizeof(NODE_TYPES[0]);

static const NodeType* find_node_type(const char* name) {
    for (int i = 0; i < NUM_NODE_TYPES; i++)
        if (strcmp(NODE_TYPES[i].name, name) == 0) return &NODE_TYPES[i];
    return nullptr;
}
