#pragma once
#include "imgui.h"

struct Editor2Style {
    Editor2Style();

    // Layout
    float node_min_width;
    float node_height;
    float pin_radius;
    float pin_spacing;
    float node_rounding;
    float grid_step;

    // Thickness
    float wire_thickness;
    float node_border;
    float highlight_offset;
    float highlight_thickness;
    float add_pin_line;

    // Hit testing
    float pin_hit_radius_mul;
    float wire_hit_threshold;
    float node_hit_threshold_mul;
    float dismiss_radius;
    float pin_priority_bias;

    // Canvas colors
    ImU32 col_bg;
    ImU32 col_grid;

    // Node colors
    ImU32 col_node;
    ImU32 col_node_sel;
    ImU32 col_node_err;
    ImU32 col_node_border;
    ImU32 col_err_border;
    ImU32 col_text;

    // Pin colors
    ImU32 col_pin_data;
    ImU32 col_pin_bang;
    ImU32 col_pin_lambda;
    ImU32 col_pin_hover;
    ImU32 col_add_pin;
    ImU32 col_add_pin_fg;
    ImU32 col_opt_pin_fg;

    // Wire colors
    ImU32 col_wire;
    ImU32 col_wire_named;
    ImU32 col_wire_lambda;

    // Net label colors
    ImU32 col_label_bg;
    ImU32 col_label_text;

    // Interaction
    float scroll_pan_speed;

    // Tooltip
    float tooltip_scale;
};

extern Editor2Style S;
