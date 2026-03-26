#pragma once
#include <string>

// Interface for editor panes (Editor1 legacy, Editor2 new graph-builder based)
struct IEditorPane {
    virtual ~IEditorPane() = default;

    virtual bool load(const std::string& path) = 0;
    virtual void draw() = 0;

    virtual bool is_loaded() const = 0;
    virtual bool is_dirty() const = 0;
    virtual const std::string& file_path() const = 0;
    virtual const std::string& tab_name() const = 0;
};
