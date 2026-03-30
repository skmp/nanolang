#include "tooltip_renderer.h"
#include "node_renderer.h"

void tooltip_add_diamond(const AddPinHover& hover) {
    ImGui::BeginTooltip();
    ImGui::SetWindowFontScale(S.tooltip_scale);
    ImGui::Text("add %s", hover.va_port ? hover.va_port->name : "arg");
    ImGui::EndTooltip();
}

void tooltip_input_pin(const VisualPin& pin) {
    ImGui::BeginTooltip();
    ImGui::SetWindowFontScale(S.tooltip_scale);
    if (pin.arg->port())
        ImGui::Text("%s", pin.arg->name().c_str());
    else if (pin.kind == VisualPinKind::Remap)
        ImGui::Text("$%d", pin.arg->remap_idx());
    ImGui::EndTooltip();
}

void tooltip_output_pin(const VisualPin& pin, int visual_index) {
    ImGui::BeginTooltip();
    ImGui::SetWindowFontScale(S.tooltip_scale);
    if (pin.arg->port())
        ImGui::Text("%s", pin.arg->name().c_str());
    else
        ImGui::Text("out%d", visual_index);
    ImGui::EndTooltip();
}

void tooltip_side_bang() {
    ImGui::BeginTooltip();
    ImGui::SetWindowFontScale(S.tooltip_scale);
    ImGui::Text("post_bang");
    ImGui::EndTooltip();
}

void tooltip_node_body(const FlowNodeBuilderPtr& node) {
    ImGui::BeginTooltip();
    ImGui::SetWindowFontScale(S.tooltip_scale);
    ImGui::Text("id: %s", node->id().c_str());
    auto show_args = [](const char* label, const ParsedArgs2* pa) {
        if (!pa) return;
        ImGui::Text("%s (%d):", label, pa->size());
        for (int i = 0; i < pa->size(); i++) {
            auto a = (*pa)[i];
            if (auto n = a->as_net())
                ImGui::Text("  [%d] net: %s", i, n->first().c_str());
            else if (auto e = a->as_expr())
                ImGui::Text("  [%d] expr: %s", i, e->expr().c_str());
            else if (auto s = a->as_string())
                ImGui::Text("  [%d] str: %s", i, s->value().c_str());
            else if (auto v = a->as_number())
                ImGui::Text("  [%d] num: %g", i, v->value());
        }
    };
    show_args("parsed_args", node->parsed_args.get());
    if (node->parsed_va_args && !node->parsed_va_args->empty())
        show_args("parsed_va_args", node->parsed_va_args.get());
    if (!node->remaps.empty()) {
        ImGui::Text("remaps (%d):", (int)node->remaps.size());
        for (int i = 0; i < (int)node->remaps.size(); i++) {
            if (auto n = node->remaps[i]->as_net())
                ImGui::Text("  $%d -> %s", i, n->first().c_str());
        }
    }
    ImGui::EndTooltip();
}

void tooltip_wire(const WireInfo& w) {
    ImGui::BeginTooltip();
    ImGui::SetWindowFontScale(S.tooltip_scale);
    if (w.is_lambda())
        ImGui::Text("lambda: %s", w.src_id.c_str());
    else
        ImGui::Text("net: %s", w.net_id.c_str());
    ImGui::Text("src: %s", w.src_id.c_str());
    ImGui::Text("dst: %s", w.dst_id.c_str());
    ImGui::EndTooltip();
}
