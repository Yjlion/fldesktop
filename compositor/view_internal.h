/* fl-wm: what view.c shares with view_layout.c (not for the rest of fl-wm) */
#pragma once
#include <stdbool.h>
#include "server.h"

int deco_top(struct flwm_view *v);  /* title bar + border, 0 without decorations */
int deco_side(struct flwm_view *v); /* the border */
bool is_x11(struct flwm_view *view);
/* the content geometry relative to the surface */
void content_geometry(struct flwm_view *view, struct wlr_box *geo);
/* box: the outer frame (decorations included) */
void apply_box(struct flwm_view *view, struct wlr_box *box);
struct flwm_view *view_parent(struct flwm_view *view);

/* view_layout.c: the window rules and the last session's places, when a
 * window first maps; may change maximized, fullscreen and focus */
void view_apply_rules(struct flwm_view *view, bool *maximized, bool *fullscreen, bool *focus);
/* view_layout.c: a window rule's opacity, again after a commit */
void view_apply_opacity(struct flwm_view *view);
