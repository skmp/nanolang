#pragma once
#include "editor_pane.h"
#include "visual_editor.h"
#include <memory>
#include <vector>
#include <string>

class NetsEditor : public IEditorPane, public VisualEditor {
public:
    NetsEditor(const std::shared_ptr<GraphBuilder>& gb,
               const std::shared_ptr<AttoEditorSharedState>& shared);

    // IEditorPane
    void draw() override;
    const char* type_name() const override { return "nets"; }
    std::shared_ptr<GraphBuilder> get_graph_builder() const override { return gb_; }

protected:
    // VisualEditor hooks
    void draw_content(const CanvasFrame& frame) override;
    HoverItem do_detect_hover(ImVec2 mouse, ImVec2 canvas_origin) override;
    void do_draw_hover_effects(ImDrawList* dl, ImVec2 canvas_origin, const HoverItem& hover) override;
    FlowNodeBuilderPtr hover_to_node(const HoverItem& item) override;
    bool test_drag_overlap(const FlowNodeBuilderPtr& sel, float nx, float ny) override;
    std::vector<BoxTestNode> get_box_test_nodes() override;

private:
    std::shared_ptr<GraphBuilder> gb_;

    // Per-row layout for each net
    struct NetRow {
        NetBuilderPtr net;
        NodeId net_id;

        // Source node
        FlowNodeBuilderPtr src_node;
        NodeLayout src_layout;
        VisualPinMap src_vpm;
        std::string src_display;
        int src_output_pin;     // output pin index connected to this net
        bool src_is_bang;       // source is a bang (node below line)

        // Destination nodes (sorted alphabetically)
        struct Dest {
            FlowNodeBuilderPtr node;
            NodeLayout layout;
            VisualPinMap vpm;
            std::string display;
            int input_pin;      // visual input pin index receiving this net
            bool is_bang;       // destination is a bang trigger
        };
        std::vector<Dest> dests;

        float row_y;            // canvas-space Y baseline
        float label_x;          // screen-space X for net label
    };

    std::vector<NetRow> rows_;

    // All wires for hit-testing
    std::vector<WireInfo> all_wires_;

    // All rendered node instances for hit-testing
    struct RenderedNode {
        FlowNodeBuilderPtr node;
        NodeLayout layout;
        VisualPinMap vpm;
        std::string display;
    };
    std::vector<RenderedNode> rendered_nodes_;

    void rebuild_layout(ImVec2 canvas_origin);
};

// Factory
std::shared_ptr<IEditorPane> make_nets_editor(
    const std::shared_ptr<GraphBuilder>& gb,
    const std::shared_ptr<AttoEditorSharedState>& shared);
