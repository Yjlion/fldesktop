/* fl-wm: tiling. A workspace can tile its windows instead of stacking them
 * (Super+T, or [behaviour] tiling=true for all): the first window takes the
 * left part (master_ratio), the others share the right part, one above the
 * other. Dialogs and windows of a fixed size float. No wlroots here (unit
 * tested). */
#pragma once

struct flwm_tile_box {
	int x, y, w, h;
};

/* the boxes (outer frames, decorations included) of n windows in area;
 * gap pixels between them and at the edges */
void tile_layout(struct flwm_tile_box area, int n, double master_ratio, int gap, struct flwm_tile_box *out);
