#pragma once
#include "node_types.h" // for NodeTypeID

// New pin model: flattened inputs/outputs, optional, input_ports_va_args, output_ports_va_args

enum class PortKind2 : uint8_t {
    BangTrigger,  // bang input (rendered as square, top)
    Data,         // data input/output
    Lambda,       // lambda capture (only accepts node refs)
    BangNext,     // bang output (rendered as square)
};

enum class PortPosition2: uint8_t {
    Input,
    Output,
};

struct PortDesc2 {
    const char* name;
    const char* desc;
    PortKind2 kind = PortKind2::Data;
    PortPosition2 position = PortPosition2::Input;
    const char* type_name = nullptr;
    bool optional = false;
    bool va_args = false;
};

enum class NodeKind2 : uint8_t {
    Flow,         // dataflow node — side-bang (right-middle)
    Banged,       // bang trigger input (top) + bang next output (bottom)
    Event,        // event source — bang next output (bottom), no bang input
    Declaration,  // compile-time — bang trigger input (top) + bang next output (bottom)
    Special,      // Label or Error - special handling
};

struct NodeType2 {
    NodeKind2 kind = NodeKind2::Flow;
    NodeTypeID type_id;
    
    const char* name;
    const char* desc;
    
    const PortDesc2* input_ports = nullptr;
    int num_inputs = 0;                        // required input ports
    const PortDesc2* input_optional_ports = nullptr;
    int num_inputs_optional = 0;               // trailing optional input ports
    const PortDesc2* input_ports_va_args = nullptr;  // nullptr = no input_ports_va_args, else template for repeating pins
    
    const PortDesc2* output_ports;
    int num_outputs;
    const PortDesc2* output_ports_va_args = nullptr;  // nullptr = no input_ports_va_args, else template for repeating pins

    int total_inputs() const { return num_inputs + num_inputs_optional; }
    const PortDesc2* input_port(int i) const {
        if (i < num_inputs) return input_ports ? &input_ports[i] : nullptr;
        int oi = i - num_inputs;
        if (oi < num_inputs_optional) return input_optional_ports ? &input_optional_ports[oi] : nullptr;
        return nullptr;
    }
    bool is_banged() const { return kind == NodeKind2::Banged || kind == NodeKind2::Event || kind == NodeKind2::Declaration; }
    bool is_declaration() const { return kind == NodeKind2::Declaration; }
    bool is_flow() const { return kind == NodeKind2::Flow; }
    bool is_special() const { return kind == NodeKind2::Special; }
    bool is_event() const { return kind == NodeKind2::Event; }
};

// ─── Port descriptor arrays ───

// Common outputs
static const PortDesc2 P2_NEXT[] = {
    {.name = "next", .desc = "fires after completion", .kind = PortKind2::BangNext, .position = PortPosition2::Output},
};

static const PortDesc2 P2_NEXT_RESULT[] = {
    {.name = "next", .desc = "fires after completion", .kind = PortKind2::BangNext, .position = PortPosition2::Output},
    {.name = "result", .desc = "result value", .position = PortPosition2::Output},
};

// Common inputs
static const PortDesc2 P2_BANG_IN[] = {
    {.name = "bang_in", .desc = "trigger input", .kind = PortKind2::BangTrigger},
};
static const PortDesc2 P2_VALUE[] = {
    {.name = "value", .desc = "input value"},
};

// expr!
static const PortDesc2 P2_EXPR_BANG_IN[] = {
    {.name = "bang_in", .desc = "trigger input", .kind = PortKind2::BangTrigger},
};

// store!
static const PortDesc2 P2_STORE_BANG_IN[] = {
    {.name = "bang_in", .desc = "trigger", .kind = PortKind2::BangTrigger},
    {.name = "target", .desc = "variable/reference to store into"},
    {.name = "value", .desc = "value to store"},
};
static const PortDesc2 P2_STORE_IN[] = {
    {.name = "target", .desc = "variable/reference to store into"},
    {.name = "value", .desc = "value to store"},
};

// append!
static const PortDesc2 P2_APPEND_BANG_IN[] = {
    {.name = "bang_in", .desc = "trigger", .kind = PortKind2::BangTrigger},
    {.name = "target", .desc = "collection to append to"},
    {.name = "value", .desc = "value to append"},
};
static const PortDesc2 P2_APPEND_IN[] = {
    {.name = "target", .desc = "collection to append to"},
    {.name = "value", .desc = "value to append"},
};

// erase
static const PortDesc2 P2_ERASE_BANG_IN[] = {
    {.name = "bang_in", .desc = "trigger", .kind = PortKind2::BangTrigger},
    {.name = "target", .desc = "collection to erase from"},
    {.name = "key", .desc = "key/value/iterator to erase"},
};
static const PortDesc2 P2_ERASE_IN[] = {
    {.name = "target", .desc = "collection to erase from"},
    {.name = "key", .desc = "key/value/iterator to erase"},
};

// select
static const PortDesc2 P2_SELECT_IN[] = {
    {.name = "condition", .desc = "boolean selector"},
    {.name = "if_true", .desc = "value when true"},
    {.name = "if_false", .desc = "value when false"},
};
static const PortDesc2 P2_SELECT_BANG_IN[] = {
    {.name = "bang_in", .desc = "trigger", .kind = PortKind2::BangTrigger},
    {.name = "condition", .desc = "boolean condition"},
};
static const PortDesc2 P2_SELECT_BANG_OUT[] = {
    {.name = "next", .desc = "fires after branch completes", .kind = PortKind2::BangNext, .position = PortPosition2::Output},
    {.name = "true", .desc = "fires when true", .kind = PortKind2::BangNext, .position = PortPosition2::Output},
    {.name = "false", .desc = "fires when false", .kind = PortKind2::BangNext, .position = PortPosition2::Output},
};

// va_args templates
static const PortDesc2 P2_VA_FIELD = {.name = "field", .desc = "constructor field", .va_args = true};
static const PortDesc2 P2_VA_ARG   = {.name = "arg",   .desc = "function argument", .va_args = true};
static const PortDesc2 P2_VA_PARAM = {.name = "param", .desc = "lambda parameter", .va_args = true};

// va_args outputs
static const PortDesc2 P2_VA_EVENT_OUT = {.name = "args", .desc = "event arguments", .kind = PortKind2::Data , .position = PortPosition2::Output, .va_args = true};

static const PortDesc2 P2_VA_EXPR_OUT = {.name = "expr", .desc = "expression outputs", .kind = PortKind2::Data , .position = PortPosition2::Output, .va_args = true};

// new
static const PortDesc2 P2_NEW_IN[] = {
    {.name = "type", .desc = "type to instantiate"},
};

// call
static const PortDesc2 P2_CALL_IN[] = {
    {.name = "fn", .desc = "function to call"},
};
static const PortDesc2 P2_CALL_BANG_IN[] = {
    {.name = "bang_in", .desc = "trigger", .kind = PortKind2::BangTrigger},
    {.name = "fn", .desc = "function to call"},
};

// iterate
static const PortDesc2 P2_ITERATE_IN[] = {
    {.name = "collection", .desc = "collection to iterate over"},
    {.name = "fn", .desc = "it=fn(it); while it!=end", .kind = PortKind2::Lambda},
};
static const PortDesc2 P2_ITERATE_BANG_IN[] = {
    {.name = "bang_in", .desc = "trigger", .kind = PortKind2::BangTrigger},
    {.name = "collection", .desc = "collection to iterate over"},
    {.name = "fn", .desc = "it=fn(it); while it!=end", .kind = PortKind2::Lambda},
};

// lock
static const PortDesc2 P2_LOCK_IN[] = {
    {.name = "mutex", .desc = "mutex to lock"},
    {.name = "fn", .desc = "body under lock", .kind = PortKind2::Lambda},
};
static const PortDesc2 P2_LOCK_BANG_IN[] = {
    {.name = "bang_in", .desc = "trigger", .kind = PortKind2::BangTrigger},
    {.name = "mutex", .desc = "mutex to lock"},
    {.name = "fn", .desc = "body under lock", .kind = PortKind2::Lambda},
};

// decl
static const PortDesc2 P2_DECL_TYPE_IN[] = {
    {.name = "bang_in", .desc = "trigger", .kind = PortKind2::BangTrigger},
    {.name = "name", .desc = "type name (symbol)"},
    {.name = "type", .desc = "type definition"},
};
static const PortDesc2 P2_DECL_TYPE_OUT[] = {
    {.name = "next", .desc = "fires after declaration", .kind = PortKind2::BangNext, .position = PortPosition2::Output},
    {.name = "type", .desc = "the declared type", .position = PortPosition2::Output},
};
static const PortDesc2 P2_DECL_VAR_IN[] = {
    {.name = "bang_in", .desc = "trigger", .kind = PortKind2::BangTrigger},
    {.name = "name", .desc = "variable name (symbol)"},
    {.name = "type", .desc = "variable type"},
};
static const PortDesc2 P2_DECL_VAR_OPT_IN[] = {
    {.name = "initial", .desc = "variable initial value", .optional = true},
};
static const PortDesc2 P2_DECL_VAR_OUT[] = {
    {.name = "next", .desc = "fires after declaration", .kind = PortKind2::BangNext, .position = PortPosition2::Output},
    {.name = "ref", .desc = "reference to variable", .position = PortPosition2::Output},
};
static const PortDesc2 P2_DECL_OUT[] = {
    {.name = "next", .desc = "fires to start declarations", .kind = PortKind2::BangNext, .position = PortPosition2::Output},
};
static const PortDesc2 P2_DECL_EVENT_IN[] = {
    {.name = "bang_in", .desc = "trigger", .kind = PortKind2::BangTrigger},
    {.name = "name", .desc = "event name (symbol)"},
    {.name = "type", .desc = "event function type"},
};
static const PortDesc2 P2_DECL_IMPORT_IN[] = {
    {.name = "bang_in", .desc = "trigger", .kind = PortKind2::BangTrigger},
    {.name = "path", .desc = "module path", .type_name = "literal<string,?>"},
};
static const PortDesc2 P2_FFI_IN[] = {
    {.name = "bang_in", .desc = "trigger", .kind = PortKind2::BangTrigger},
    {.name = "name", .desc = "function name (symbol)"},
    {.name = "type", .desc = "function type"},
};

// discard
static const PortDesc2 P2_DISCARD_BANG_IN[] = {
    {.name = "bang_in", .desc = "trigger", .kind = PortKind2::BangTrigger},
    {.name = "value", .desc = "value to discard"},
};

// output_mix!
static const PortDesc2 P2_OUTPUT_MIX_IN[] = {
    {.name = "bang_in", .desc = "trigger", .kind = PortKind2::BangTrigger},
    {.name = "value", .desc = "audio sample to mix"},
};

// resize!
static const PortDesc2 P2_RESIZE_IN[] = {
    {.name = "bang_in", .desc = "trigger", .kind = PortKind2::BangTrigger},
    {.name = "target", .desc = "vector to resize"},
    {.name = "size", .desc = "new size", .type_name = "s32"},
};


// ─── Node type table ───

static const NodeType2 NODE_TYPES2[] = {
    {
        .type_id = NodeTypeID::Expr,
        .name = "expr",
        .desc = "Evaluate expression",
        .output_ports = P2_NEXT,
        .num_outputs = 1,
        .output_ports_va_args = &P2_VA_EXPR_OUT,
    },
    {
        .type_id = NodeTypeID::Select,
        .name = "select",
        .desc = "Select value by condition",
        .input_ports = P2_SELECT_IN,
        .num_inputs = 3,
        .output_ports = P2_NEXT_RESULT,
        .num_outputs = 2,
    },
    {
        .type_id = NodeTypeID::New,
        .name = "new",
        .desc = "Instantiate a type",
        .input_ports = P2_NEW_IN,
        .num_inputs = 1,
        .input_ports_va_args = &P2_VA_FIELD,
        .output_ports = P2_NEXT_RESULT,
        .num_outputs = 2,
    },
    {
        .type_id = NodeTypeID::Dup,
        .name = "dup",
        .desc = "Duplicate input to output",
        .input_ports = P2_VALUE,
        .num_inputs = 1,
        .output_ports = P2_NEXT_RESULT,
        .num_outputs = 2,
    },
    {
        .type_id = NodeTypeID::Str,
        .name = "str",
        .desc = "Convert to string",
        .input_ports = P2_VALUE,
        .num_inputs = 1,
        .output_ports = P2_NEXT_RESULT,
        .num_outputs = 2,
    },
    {
        .type_id = NodeTypeID::Void,
        .name = "void",
        .desc = "Void result",
        .output_ports = P2_NEXT_RESULT,
        .num_outputs = 2,
    },
    {
        .kind = NodeKind2::Banged,
        .type_id = NodeTypeID::DiscardBang,
        .name = "discard!",
        .desc = "Discard value, pass bang",
        .input_ports = P2_DISCARD_BANG_IN,
        .num_inputs = 2,
        .output_ports = P2_NEXT,
        .num_outputs = 1,
    },
    {
        .type_id = NodeTypeID::Discard,
        .name = "discard",
        .desc = "Discard input values",
        .input_ports = P2_VALUE,
        .num_inputs = 1,
        .output_ports = P2_NEXT,
        .num_outputs = 1
    },
    {
        .kind = NodeKind2::Declaration,
        .type_id = NodeTypeID::DeclType,
        .name = "decl_type",
        .desc = "Declare a type",
        .input_ports = P2_DECL_TYPE_IN,
        .num_inputs = 3,
        .output_ports = P2_DECL_TYPE_OUT,
        .num_outputs = 2,
    },
    {
        .kind = NodeKind2::Declaration,
        .type_id = NodeTypeID::DeclVar,
        .name = "decl_var",
        .desc = "Declare a variable",
        .input_ports = P2_DECL_VAR_IN,
        .num_inputs = 3,
        .input_optional_ports = P2_DECL_VAR_OPT_IN,
        .num_inputs_optional = 1,
        .output_ports = P2_DECL_VAR_OUT,
        .num_outputs = 2,
    },
    {
        .kind = NodeKind2::Declaration,
        .type_id = NodeTypeID::Decl,
        .name = "decl",
        .desc = "Compile-time entry point",
        .output_ports = P2_DECL_OUT,
        .num_outputs = 1,
    },
    {
        .kind = NodeKind2::Declaration,
        .type_id = NodeTypeID::DeclEvent,
        .name = "decl_event",
        .desc = "Declare event",
        .input_ports = P2_DECL_EVENT_IN,
        .num_inputs = 3,
        .output_ports = P2_NEXT,
        .num_outputs = 1,
    },
    {
        .kind = NodeKind2::Declaration,
        .type_id = NodeTypeID::DeclImport,
        .name = "decl_import",
        .desc = "Import module",
        .input_ports = P2_DECL_IMPORT_IN,
        .num_inputs = 2,
        .output_ports = P2_NEXT,
        .num_outputs = 1,
    },
    {
        .kind = NodeKind2::Declaration,
        .type_id = NodeTypeID::Ffi,
        .name = "ffi",
        .desc = "Declare external function",
        .input_ports = P2_FFI_IN,
        .num_inputs = 3,
        .output_ports = P2_NEXT,
        .num_outputs = 1,
    },
    {
        .type_id = NodeTypeID::Call,
        .name = "call",
        .desc = "Call function",
        .input_ports = P2_CALL_IN,
        .num_inputs = 1,
        .input_ports_va_args = &P2_VA_ARG,
        .output_ports = P2_NEXT_RESULT,
        .num_outputs = 2,
    },
    {
        .kind = NodeKind2::Banged,
        .type_id = NodeTypeID::CallBang,
        .name = "call!",
        .desc = "Call function (bang)",
        .input_ports = P2_CALL_BANG_IN,
        .num_inputs = 2,
        .input_ports_va_args = &P2_VA_ARG,
        .output_ports = P2_NEXT_RESULT,
        .num_outputs = 2,
    },
    {
        .type_id = NodeTypeID::Erase,
        .name = "erase",
        .desc = "Erase from collection",
        .input_ports = P2_ERASE_IN,
        .num_inputs = 2,
        .output_ports = P2_NEXT_RESULT,
        .num_outputs = 2,
    },
    {
        .kind = NodeKind2::Banged,
        .type_id = NodeTypeID::OutputMixBang,
        .name = "output_mix!",
        .desc = "Mix into audio output",
        .input_ports = P2_OUTPUT_MIX_IN,
        .num_inputs = 2,
    },
    {
        .type_id = NodeTypeID::Append,
        .name = "append",
        .desc = "Append to collection",
        .input_ports = P2_APPEND_IN,
        .num_inputs = 2,
        .output_ports = P2_NEXT_RESULT,
        .num_outputs = 2,
    },
    {
        .kind = NodeKind2::Banged,
        .type_id = NodeTypeID::AppendBang,
        .name = "append!",
        .desc = "Append to collection (bang)",
        .input_ports = P2_APPEND_BANG_IN,
        .num_inputs = 3,
        .output_ports = P2_NEXT_RESULT,
        .num_outputs = 2,
    },
    {
        .type_id = NodeTypeID::Store,
        .name = "store",
        .desc = "Store value",
        .input_ports = P2_STORE_IN,
        .num_inputs = 2,
        .output_ports = P2_NEXT,
        .num_outputs = 1
    },
    {
        .kind = NodeKind2::Banged,
        .type_id = NodeTypeID::StoreBang,
        .name = "store!",
        .desc = "Store value (bang)",
        .input_ports = P2_STORE_BANG_IN,
        .num_inputs = 3,
        .output_ports = P2_NEXT,
        .num_outputs = 1,
    },
    {
        .kind = NodeKind2::Event,
        .type_id = NodeTypeID::EventBang,
        .name = "event!",
        .desc = "Event source",
        .output_ports = P2_NEXT,
        .num_outputs = 1,
        .output_ports_va_args = &P2_VA_EVENT_OUT,
    },
    {
        .kind = NodeKind2::Special,
        .type_id = NodeTypeID::OnKeyDownBang,
        .name = "on_key_down!",
        .desc = "(removed)",
    },
    {
        .kind = NodeKind2::Special,
        .type_id = NodeTypeID::OnKeyUpBang,
        .name = "on_key_up!",
        .desc = "(removed)",
    },
    {
        .kind = NodeKind2::Banged,
        .type_id = NodeTypeID::SelectBang,
        .name = "select!",
        .desc = "Branch on condition",
        .input_ports = P2_SELECT_BANG_IN,
        .num_inputs = 2,
        .output_ports = P2_SELECT_BANG_OUT,
        .num_outputs = 3,
    },
    {
        .kind = NodeKind2::Banged,
        .type_id = NodeTypeID::ExprBang,
        .name = "expr!",
        .desc = "Evaluate expression on bang",
        .input_ports = P2_EXPR_BANG_IN,
        .num_inputs = 1,
        .output_ports = P2_NEXT,
        .num_outputs = 1,
        .output_ports_va_args = &P2_VA_EXPR_OUT,
    },
    {
        .kind = NodeKind2::Banged,
        .type_id = NodeTypeID::EraseBang,
        .name = "erase!",
        .desc = "Erase from collection (bang)",
        .input_ports = P2_ERASE_BANG_IN,
        .num_inputs = 3,
        .output_ports = P2_NEXT_RESULT,
        .num_outputs = 2,
    },
    {
        .type_id = NodeTypeID::Iterate,
        .name = "iterate",
        .desc = "Iterate collection",
        .input_ports = P2_ITERATE_IN,
        .num_inputs = 2,
        .output_ports = P2_NEXT,
        .num_outputs = 1
    },
    {
        .kind = NodeKind2::Banged,
        .type_id = NodeTypeID::IterateBang,
        .name = "iterate!",
        .desc = "Iterate collection (bang)",
        .input_ports = P2_ITERATE_BANG_IN,
        .num_inputs = 3,
        .output_ports = P2_NEXT,
        .num_outputs = 1,
    },
    {
        .type_id = NodeTypeID::Next,
        .name = "next",
        .desc = "Advance iterator",
        .input_ports = P2_VALUE,
        .num_inputs = 1,
        .output_ports = P2_NEXT_RESULT,
        .num_outputs = 2,
    },
    {
        .type_id = NodeTypeID::Lock,
        .name = "lock",
        .desc = "Execute under mutex lock",
        .input_ports = P2_LOCK_IN,
        .num_inputs = 2,
        .input_ports_va_args = &P2_VA_PARAM,
        .output_ports = P2_NEXT,
        .num_outputs = 1,
    },
    {
        .kind = NodeKind2::Banged,
        .type_id = NodeTypeID::LockBang,
        .name = "lock!",
        .desc = "Execute under mutex lock (bang)",
        .input_ports = P2_LOCK_BANG_IN,
        .num_inputs = 3,
        .input_ports_va_args = &P2_VA_PARAM,
        .output_ports = P2_NEXT,
        .num_outputs = 1,
    },
    {
        .kind = NodeKind2::Banged,
        .type_id = NodeTypeID::ResizeBang,
        .name = "resize!",
        .desc = "Resize vector",
        .input_ports = P2_RESIZE_IN,
        .num_inputs = 3,
        .output_ports = P2_NEXT,
        .num_outputs = 1,
    },
    {
        .type_id = NodeTypeID::Cast,
        .name = "cast",
        .desc = "Cast value to type",
        .input_ports = P2_VALUE,
        .num_inputs = 1,
        .output_ports = P2_NEXT_RESULT,
        .num_outputs = 2
    },
    {
        .kind = NodeKind2::Special,
        .type_id = NodeTypeID::Label,
        .name = "label",
        .desc = "Text label",
    },
    {
        .type_id = NodeTypeID::Deref,
        .name = "deref",
        .desc = "Dereference iterator (internal)",
        .input_ports = P2_VALUE,
        .num_inputs = 1,
        .output_ports = P2_NEXT_RESULT,
        .num_outputs = 2,
    },
    {
        .kind = NodeKind2::Special,
        .type_id = NodeTypeID::Error,
        .name = "error",
        .desc = "Error: invalid node",
    },
};

static constexpr int NUM_NODE_TYPES2 = sizeof(NODE_TYPES2) / sizeof(NODE_TYPES2[0]);

static const NodeType2* find_node_type2(NodeTypeID id) {
    auto idx = static_cast<uint8_t>(id);
    if (idx < NUM_NODE_TYPES2) return &NODE_TYPES2[idx];
    return nullptr;
}

static const NodeType2* find_node_type2(const char* name) {
    for (int i = 0; i < NUM_NODE_TYPES2; i++)
        if (strcmp(NODE_TYPES2[i].name, name) == 0) return &NODE_TYPES2[i];
    return nullptr;
}
