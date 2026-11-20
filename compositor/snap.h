/* fl-wm: window snapping geometry (no wlroots; unit tested) */
#ifndef FLWM_SNAP_H
#define FLWM_SNAP_H

enum flwm_snap {
	FLWM_SNAP_NONE,
	FLWM_SNAP_LEFT,
	FLWM_SNAP_RIGHT,
	FLWM_SNAP_TOP_LEFT,
	FLWM_SNAP_TOP_RIGHT,
	FLWM_SNAP_BOTTOM_LEFT,
	FLWM_SNAP_BOTTOM_RIGHT,
	FLWM_SNAP_MAXIMIZED,
	FLWM_SNAP_MINIMIZED, /* only from flwm_snap_next: Super+Down on a free window */
};

enum flwm_snap_dir { FLWM_SNAP_DIR_LEFT, FLWM_SNAP_DIR_RIGHT, FLWM_SNAP_DIR_UP, FLWM_SNAP_DIR_DOWN };

struct flwm_rect {
	int x, y, width, height;
};

/* The outer frame of a window snapped into area (the usable area). */
struct flwm_rect flwm_snap_rect(enum flwm_snap snap, struct flwm_rect area);

/* Super+arrow: where a window in state cur goes. Left/Right move between the
 * halves and quarters and back to a free window; Up and Down go from a half
 * to its quarters, from a free window to maximized / minimized and from
 * maximized back to free. */
enum flwm_snap flwm_snap_next(enum flwm_snap cur, enum flwm_snap_dir dir);

/* Dragging a window: the snap for the pointer at px, py on an output whose
 * full area is out. Within edge pixels of the left, right or top border;
 * corners reach corner pixels along the border. */
enum flwm_snap flwm_snap_zone(int px, int py, struct flwm_rect out, int edge, int corner);

#endif
