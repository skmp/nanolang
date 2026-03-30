#pragma once
#include <memory>

struct GraphBuilder;

// Interface for editor panes — views into a GraphBuilder
struct IEditorPane {
    virtual ~IEditorPane() = default;

    virtual void draw() = 0;
    virtual const char* type_name() const = 0;
    virtual std::shared_ptr<GraphBuilder> get_graph_builder() const = 0;
};
