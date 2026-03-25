#include "graphbuilder.h"
#include "args.h"
#include "expr.h"
#include "node_types.h"
#include "shadow.h"
#include "inference.h"
#include "type_utils.h"

FlowNode& GraphBuilder::add(const std::string& guid, const std::string& type, const std::string& args,
                             int num_inputs, int num_outputs) {
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
        auto info = compute_inline_args(args, di);
        if (!info.error.empty()) node.error = info.error;
        int ref_pins = (info.pin_slots.max_slot >= 0) ? (info.pin_slots.max_slot + 1) : 0;
        if (num_inputs >= 0) ref_pins = num_inputs;
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

void GraphBuilder::link(const std::string& from, const std::string& to) {
    graph.add_link(from, to);
}

FlowNode* GraphBuilder::find(const std::string& guid) {
    for (auto& n : graph.nodes) if (n.guid == guid) return &n;
    return nullptr;
}

FlowPin* GraphBuilder::find_pin(const std::string& pin_id) {
    return graph.find_pin(pin_id);
}

std::vector<std::string> GraphBuilder::run_inference() {
    resolve_type_based_pins(graph);
    generate_shadow_nodes(graph);
    GraphInference inference(pool);
    return inference.run(graph);
}

std::vector<std::string> GraphBuilder::run_full_pipeline() {
    resolve_type_based_pins(graph);
    generate_shadow_nodes(graph);
    GraphInference inference(pool);
    return inference.run(graph);
}
