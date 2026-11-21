// fl-docs: images in documents and comic books, decoded for cairo
#pragma once
#include <cairo.h>

#include <string>

namespace docs {

// A PNG, JPEG, GIF, BMP or SVG as an ARGB32 surface (null when it cannot be
// read). SVG is drawn `svg_scale` times its size.
cairo_surface_t *decode_image(const std::string &data, double svg_scale = 2.0);

// draws the surface into x, y, w, h (points) on white, smoothly scaled
void draw_image(cairo_t *cr, cairo_surface_t *img, double x, double y, double w, double h);

} // namespace docs
