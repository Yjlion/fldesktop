/* fl-wm: tiling layout (see tile.h) */
#include "tile.h"

void tile_layout(struct flwm_tile_box a, int n, double ratio, int gap, struct flwm_tile_box *out) {
	if (n <= 0) return;
	if (ratio < 0.2) ratio = 0.2;
	if (ratio > 0.8) ratio = 0.8;
	int x = a.x + gap, y = a.y + gap, w = a.w - 2 * gap, h = a.h - 2 * gap;
	if (n == 1) {
		out[0] = (struct flwm_tile_box){x, y, w, h};
		return;
	}
	int mw = (int)((w - gap) * ratio);
	out[0] = (struct flwm_tile_box){x, y, mw, h};
	int sx = x + mw + gap, sw = w - mw - gap, k = n - 1;
	int each = (h - gap * (k - 1)) / k;
	for (int i = 0; i < k; i++) {
		int sy = y + i * (each + gap);
		/* the last one takes what rounding left */
		int sh = i == k - 1 ? y + h - sy : each;
		out[i + 1] = (struct flwm_tile_box){sx, sy, sw, sh};
	}
}
