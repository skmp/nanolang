#include "editor_style.h"

Editor2Style::Editor2Style()
    // Layout
    : node_min_width(80.0f)
    , node_height(40.0f)
    , pin_radius(5.0f)
    , pin_spacing(16.0f)
    , node_rounding(4.0f)
    , grid_step(20.0f)
    // Thickness
    , wire_thickness(2.5f)
    , node_border(1.0f)
    , highlight_offset(2.0f)
    , highlight_thickness(2.0f)
    , add_pin_line(1.5f)
    // Hit testing
    , pin_hit_radius_mul(2.5f)
    , wire_hit_threshold(30.0f)
    , node_hit_threshold_mul(6.f)
    , dismiss_radius(20.0f)
    , pin_priority_bias(1e6f)
    // Canvas colors
    , col_bg(IM_COL32(30, 30, 40, 255))
    , col_grid(IM_COL32(50, 50, 60, 255))
    // Node colors
    , col_node(IM_COL32(50, 55, 75, 220))
    , col_node_sel(IM_COL32(80, 90, 130, 255))
    , col_node_err(IM_COL32(130, 40, 40, 220))
    , col_node_border(IM_COL32(80, 80, 100, 255))
    , col_err_border(IM_COL32(255, 80, 80, 255))
    , col_text(IM_COL32(220, 220, 220, 255))
    // Pin colors
    , col_pin_data(IM_COL32(100, 200, 100, 255))
    , col_pin_bang(IM_COL32(255, 200, 80, 255))
    , col_pin_lambda(IM_COL32(180, 130, 255, 255))
    , col_pin_hover(IM_COL32(255, 255, 255, 255))
    , col_add_pin(IM_COL32(120, 120, 140, 180))
    , col_add_pin_fg(IM_COL32(200, 200, 220, 220))
    , col_opt_pin_fg(IM_COL32(30, 30, 40, 255))
    // Wire colors
    , col_wire(IM_COL32(200, 200, 100, 200))
    , col_wire_named(IM_COL32(200, 200, 100, 120))
    , col_wire_lambda(IM_COL32(180, 130, 255, 200))
    // Net label colors
    , col_label_bg(IM_COL32(30, 30, 40, 200))
    , col_label_text(IM_COL32(180, 220, 255, 255))
    // Interaction
    , scroll_pan_speed(120.0f)
    // Tooltip
    , tooltip_scale(1.0f)
{
}

Editor2Style S;
