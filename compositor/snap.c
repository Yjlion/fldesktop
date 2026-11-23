#include "snap.h"

struct flwm_rect flwm_snap_rect(enum flwm_snap snap, struct flwm_rect a) {
	int hw = a.width / 2, hh = a.height / 2;
	struct flwm_rect r = a;
	switch (snap) {
	case FLWM_SNAP_LEFT: r.width = hw; break;
	case FLWM_SNAP_RIGHT: r.x = a.x + a.width - hw; r.width = hw; break;
	case FLWM_SNAP_TOP_LEFT: r.width = hw; r.height = hh; break;
	case FLWM_SNAP_TOP_RIGHT: r.x = a.x + a.width - hw; r.width = hw; r.height = hh; break;
	case FLWM_SNAP_BOTTOM_LEFT: r.y = a.y + a.height - hh; r.width = hw; r.height = hh; break;
	case FLWM_SNAP_BOTTOM_RIGHT:
		r.x = a.x + a.width - hw;
		r.y = a.y + a.height - hh;
		r.width = hw;
		r.height = hh;
		break;
	case FLWM_SNAP_LEFT_THIRD: r.width = a.width / 3; break;
	case FLWM_SNAP_CENTER_THIRD: r.x = a.x + a.width / 3; r.width = a.width - 2 * (a.width / 3); break;
	case FLWM_SNAP_RIGHT_THIRD: r.x = a.x + a.width - a.width / 3; r.width = a.width / 3; break;
	case FLWM_SNAP_LEFT_TWO_THIRDS: r.width = a.width - a.width / 3; break;
	case FLWM_SNAP_RIGHT_TWO_THIRDS: r.x = a.x + a.width / 3; r.width = a.width - a.width / 3; break;
	default: break; /* none, maximized: the whole area */
	}
	return r;
}

enum flwm_snap flwm_snap_next(enum flwm_snap cur, enum flwm_snap_dir dir) {
	switch (dir) {
	case FLWM_SNAP_DIR_LEFT:
		switch (cur) {
		/* again on the left: half, two thirds, one third, half */
		case FLWM_SNAP_LEFT: return FLWM_SNAP_LEFT_TWO_THIRDS;
		case FLWM_SNAP_LEFT_TWO_THIRDS: return FLWM_SNAP_LEFT_THIRD;
		case FLWM_SNAP_LEFT_THIRD: return FLWM_SNAP_LEFT;
		case FLWM_SNAP_CENTER_THIRD: return FLWM_SNAP_LEFT_THIRD;
		case FLWM_SNAP_RIGHT_THIRD: case FLWM_SNAP_RIGHT_TWO_THIRDS:
		case FLWM_SNAP_RIGHT: return FLWM_SNAP_NONE;
		case FLWM_SNAP_TOP_RIGHT: return FLWM_SNAP_TOP_LEFT;
		case FLWM_SNAP_BOTTOM_RIGHT: return FLWM_SNAP_BOTTOM_LEFT;
		case FLWM_SNAP_TOP_LEFT: case FLWM_SNAP_BOTTOM_LEFT: return cur;
		default: return FLWM_SNAP_LEFT;
		}
	case FLWM_SNAP_DIR_RIGHT:
		switch (cur) {
		case FLWM_SNAP_RIGHT: return FLWM_SNAP_RIGHT_TWO_THIRDS;
		case FLWM_SNAP_RIGHT_TWO_THIRDS: return FLWM_SNAP_RIGHT_THIRD;
		case FLWM_SNAP_RIGHT_THIRD: return FLWM_SNAP_RIGHT;
		case FLWM_SNAP_CENTER_THIRD: return FLWM_SNAP_RIGHT_THIRD;
		case FLWM_SNAP_LEFT_THIRD: case FLWM_SNAP_LEFT_TWO_THIRDS:
		case FLWM_SNAP_LEFT: return FLWM_SNAP_NONE;
		case FLWM_SNAP_TOP_LEFT: return FLWM_SNAP_TOP_RIGHT;
		case FLWM_SNAP_BOTTOM_LEFT: return FLWM_SNAP_BOTTOM_RIGHT;
		case FLWM_SNAP_TOP_RIGHT: case FLWM_SNAP_BOTTOM_RIGHT: return cur;
		default: return FLWM_SNAP_RIGHT;
		}
	case FLWM_SNAP_DIR_UP:
		switch (cur) {
		case FLWM_SNAP_LEFT: return FLWM_SNAP_TOP_LEFT;
		case FLWM_SNAP_RIGHT: return FLWM_SNAP_TOP_RIGHT;
		case FLWM_SNAP_BOTTOM_LEFT: return FLWM_SNAP_LEFT;
		case FLWM_SNAP_BOTTOM_RIGHT: return FLWM_SNAP_RIGHT;
		case FLWM_SNAP_TOP_LEFT: case FLWM_SNAP_TOP_RIGHT: return cur;
		default: return FLWM_SNAP_MAXIMIZED;
		}
	case FLWM_SNAP_DIR_DOWN:
		switch (cur) {
		case FLWM_SNAP_LEFT: return FLWM_SNAP_BOTTOM_LEFT;
		case FLWM_SNAP_RIGHT: return FLWM_SNAP_BOTTOM_RIGHT;
		case FLWM_SNAP_TOP_LEFT: return FLWM_SNAP_LEFT;
		case FLWM_SNAP_TOP_RIGHT: return FLWM_SNAP_RIGHT;
		case FLWM_SNAP_BOTTOM_LEFT: case FLWM_SNAP_BOTTOM_RIGHT: return cur;
		case FLWM_SNAP_MAXIMIZED: return FLWM_SNAP_NONE;
		default: return FLWM_SNAP_MINIMIZED;
		}
	}
	return cur;
}

enum flwm_snap flwm_snap_next_third(enum flwm_snap cur, enum flwm_snap_dir dir) {
	if (dir == FLWM_SNAP_DIR_LEFT) {
		switch (cur) {
		case FLWM_SNAP_RIGHT_THIRD: return FLWM_SNAP_CENTER_THIRD;
		case FLWM_SNAP_CENTER_THIRD: case FLWM_SNAP_LEFT_THIRD: return FLWM_SNAP_LEFT_THIRD;
		default: return FLWM_SNAP_LEFT_THIRD;
		}
	}
	if (dir == FLWM_SNAP_DIR_RIGHT) {
		switch (cur) {
		case FLWM_SNAP_LEFT_THIRD: return FLWM_SNAP_CENTER_THIRD;
		case FLWM_SNAP_CENTER_THIRD: case FLWM_SNAP_RIGHT_THIRD: return FLWM_SNAP_RIGHT_THIRD;
		default: return FLWM_SNAP_RIGHT_THIRD;
		}
	}
	return flwm_snap_next(cur, dir);
}

enum flwm_snap flwm_snap_complement(enum flwm_snap s) {
	switch (s) {
	case FLWM_SNAP_LEFT: return FLWM_SNAP_RIGHT;
	case FLWM_SNAP_RIGHT: return FLWM_SNAP_LEFT;
	case FLWM_SNAP_LEFT_THIRD: return FLWM_SNAP_RIGHT_TWO_THIRDS;
	case FLWM_SNAP_RIGHT_THIRD: return FLWM_SNAP_LEFT_TWO_THIRDS;
	case FLWM_SNAP_LEFT_TWO_THIRDS: return FLWM_SNAP_RIGHT_THIRD;
	case FLWM_SNAP_RIGHT_TWO_THIRDS: return FLWM_SNAP_LEFT_THIRD;
	default: return FLWM_SNAP_NONE;
	}
}

enum flwm_snap flwm_snap_zone(int px, int py, struct flwm_rect o, int edge, int corner) {
	int left = px < o.x + edge, right = px >= o.x + o.width - edge;
	int top = py < o.y + edge;
	int near_top = py < o.y + corner, near_bottom = py >= o.y + o.height - corner;
	int near_left = px < o.x + corner, near_right = px >= o.x + o.width - corner;
	if (left || right) {
		if (near_top) return left ? FLWM_SNAP_TOP_LEFT : FLWM_SNAP_TOP_RIGHT;
		if (near_bottom) return left ? FLWM_SNAP_BOTTOM_LEFT : FLWM_SNAP_BOTTOM_RIGHT;
		return left ? FLWM_SNAP_LEFT : FLWM_SNAP_RIGHT;
	}
	if (top) {
		if (near_left) return FLWM_SNAP_TOP_LEFT;
		if (near_right) return FLWM_SNAP_TOP_RIGHT;
		return FLWM_SNAP_MAXIMIZED;
	}
	return FLWM_SNAP_NONE;
}
