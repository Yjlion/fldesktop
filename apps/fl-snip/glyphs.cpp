// fl-snip: the glyphs of the tool and mode buttons (the icon theme has none)
#include "fld/i18n.h"
#include "snip_app.h"

// ---------------------------------------------------------------------------
// Small drawn glyphs for the tool and mode buttons (the icon theme has none)


void draw_arrow_head(double x1, double y1, double x2, double y2, double len) {
  double a = atan2(y2 - y1, x2 - x1);
  double l1 = a + M_PI * 0.83, l2 = a - M_PI * 0.83;
  fl_begin_polygon();
  fl_vertex(x2, y2);
  fl_vertex(x2 + len * cos(l1), y2 + len * sin(l1));
  fl_vertex(x2 + len * cos(l2), y2 + len * sin(l2));
  fl_end_polygon();
}

void draw_glyph(Glyph g, int x, int y, int s, Fl_Color c) {
  fl_color(c);
  fl_line_style(FL_SOLID, 2);
  int m = s / 5;
  int l = x + m, t = y + m, r = x + s - m, b = y + s - m;
  switch (g) {
    case Glyph::Rect:
      fl_line_style(FL_DASH, 2);
      fl_rect(l, t + 2, r - l, b - t - 4);
      break;
    case Glyph::Freeform:
      fl_line_style(FL_DASH, 2);
      fl_begin_loop();
      fl_vertex(l, (t + b) / 2);
      fl_vertex(l + (r - l) / 3, t);
      fl_vertex(r, t + 2);
      fl_vertex(r - 2, b);
      fl_vertex(l + (r - l) / 3, b - 3);
      fl_end_loop();
      break;
    case Glyph::Window:
      fl_rect(l, t, r - l, b - t);
      fl_rectf(l, t, r - l, 4);
      break;
    case Glyph::Full:
      fl_rect(l, t + 1, r - l, b - t - 5);
      fl_line(x + s / 2, b - 4, x + s / 2, b);
      fl_line(x + s / 2 - 4, b, x + s / 2 + 4, b);
      break;
    case Glyph::Pen:
      fl_line(l, b, r, t);
      fl_line(l, b, l + 3, b - 1);
      break;
    case Glyph::Highlighter:
      fl_line_style(FL_SOLID | FL_CAP_SQUARE, s / 4);
      fl_line(l + 2, b - 2, r - 2, t + 2);
      break;
    case Glyph::Line: fl_line(l, b, r, t); break;
    case Glyph::Arrow:
      fl_line(l, b, r - 2, t + 2);
      draw_arrow_head(l, b, r, t, s / 3.0);
      break;
    case Glyph::Box: fl_rect(l, t + 2, r - l, b - t - 4); break;
    case Glyph::Ellipse: fl_arc(l, t + 2, r - l, b - t - 4, 0, 360); break;
    case Glyph::Text:
      fl_font(FL_HELVETICA_BOLD, s - 4);
      fl_draw("T", x, y, s, s, FL_ALIGN_CENTER);
      break;
    case Glyph::Pixelate: {
      int q = (r - l) / 3;
      for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
          if ((i + j) % 2 == 0) fl_rectf(l + i * q, t + j * q, q, q);
      break;
    }
    case Glyph::Crop:
      fl_line(l + 3, t, l + 3, b - 3, r, b - 3);
      fl_line(l, t + 3, r - 3, t + 3, r - 3, b);
      break;
    case Glyph::Eraser:
      fl_begin_loop();
      fl_vertex(l, b - 4);
      fl_vertex(l + (r - l) / 2, t);
      fl_vertex(r, t + 5);
      fl_vertex(l + 5, b);
      fl_end_loop();
      fl_line(l, b, r, b);
      break;
  }
  fl_line_style(0);
}
