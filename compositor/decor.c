/* fl-wm server-side decorations: a cairo/pango rendered title bar plus
 * borders and invisible resize handles built from scene rects. */
#define _POSIX_C_SOURCE 200809L
#include <cairo.h>
#include <drm_fourcc.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#include "server.h"

#define HANDLE 6 /* invisible resize handle thickness */
/* soft shadow: rings this far outside the border, each a little fainter.
 * They stay inside the resize handles, which therefore take every click
 * on them (the shadow rects carry no descriptor). */
static const int shadow_spread[3] = {1, 2, 4};
static const float shadow_alpha[3] = {0.10f, 0.06f, 0.035f};

/* ---- a wlr_buffer backed by a cairo image surface ---------------------- */

struct cairo_buffer {
	struct wlr_buffer base;
	cairo_surface_t *surface;
};

static void cairo_buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct cairo_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	flwm_buffer_finish(wlr_buffer);
	cairo_surface_destroy(buf->surface);
	free(buf);
}

static bool cairo_buffer_begin(struct wlr_buffer *wlr_buffer, uint32_t flags,
		void **data, uint32_t *format, size_t *stride) {
	struct cairo_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) return false;
	*data = cairo_image_surface_get_data(buf->surface);
	*format = DRM_FORMAT_ARGB8888;
	*stride = cairo_image_surface_get_stride(buf->surface);
	return true;
}

static void cairo_buffer_end(struct wlr_buffer *wlr_buffer) {}

static const struct wlr_buffer_impl cairo_buffer_impl = {
	.destroy = cairo_buffer_destroy,
	.begin_data_ptr_access = cairo_buffer_begin,
	.end_data_ptr_access = cairo_buffer_end,
};

static struct cairo_buffer *cairo_buffer_create(int width, int height) {
	struct cairo_buffer *buf = calloc(1, sizeof(*buf));
	buf->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	if (cairo_surface_status(buf->surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(buf->surface);
		free(buf);
		return NULL;
	}
	wlr_buffer_init(&buf->base, &cairo_buffer_impl, width, height);
	return buf;
}

/* ---- helpers ------------------------------------------------------------ */

int decor_title_height(struct flwm_server *server) { return server->config.title_height; }
int decor_border(struct flwm_server *server) { return server->config.border_width; }

/* the border: the accent on the focused window (wm.ini accent_border) */
static uint32_t border_for(struct flwm_server *server, bool active) {
	if (active && server->config.accent_border) return 0xff000000 | flwm_accent_color();
	return server->config.border_color;
}

static void set_source_argb(cairo_t *cr, uint32_t c) {
	cairo_set_source_rgba(cr, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0,
		(c & 255) / 255.0, ((c >> 24) & 255) / 255.0);
}

static void argb_to_float(uint32_t c, float out[4]) {
	float a = ((c >> 24) & 255) / 255.0f;
	/* scene rects take premultiplied colors */
	out[0] = ((c >> 16) & 255) / 255.0f * a;
	out[1] = ((c >> 8) & 255) / 255.0f * a;
	out[2] = (c & 255) / 255.0f * a;
	out[3] = a;
}

static float view_scale(struct flwm_view *view) {
	struct flwm_output *o = output_for_view(view);
	return o ? o->wlr_output->scale : 1.0f;
}

static void draw_button(cairo_t *cr, enum flwm_decor_part part, double x, double y, double s,
		uint32_t fg, bool hover, bool maximized) {
	double cx = x + s / 2, cy = y + s / 2;
	if (hover) {
		if (part == FLWM_PART_CLOSE) cairo_set_source_rgb(cr, 0.88, 0.11, 0.14);
		else cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.35);
		cairo_arc(cr, cx, cy, s * 0.36, 0, 2 * M_PI);
		cairo_fill(cr);
		if (part == FLWM_PART_CLOSE) fg = 0xffffffff;
	}
	set_source_argb(cr, fg);
	cairo_set_line_width(cr, 1.5);
	double r = s * 0.14;
	switch (part) {
	case FLWM_PART_CLOSE:
		cairo_move_to(cr, cx - r, cy - r);
		cairo_line_to(cr, cx + r, cy + r);
		cairo_move_to(cr, cx + r, cy - r);
		cairo_line_to(cr, cx - r, cy + r);
		break;
	case FLWM_PART_MAXIMIZE:
		if (maximized) {
			cairo_rectangle(cr, cx - r, cy - r + 2, r * 2 - 2, r * 2 - 2);
			cairo_move_to(cr, cx - r + 2, cy - r);
			cairo_line_to(cr, cx + r, cy - r);
			cairo_line_to(cr, cx + r, cy + r - 2);
		} else {
			cairo_rectangle(cr, cx - r, cy - r, r * 2, r * 2);
		}
		break;
	case FLWM_PART_MINIMIZE:
		cairo_move_to(cr, cx - r, cy + r * 0.6);
		cairo_line_to(cr, cx + r, cy + r * 0.6);
		break;
	default:
		break;
	}
	cairo_stroke(cr);
}

static void render_title(struct flwm_view *view) {
	struct flwm_server *server = view->server;
	struct flwm_decor *d = &view->decor;
	int B = decor_border(server), T = decor_title_height(server);
	int W = d->width + 2 * B, H = T + B;
	if (W <= 0 || H <= 0) return;
	float scale = view_scale(view);
	d->scale = scale;
	int pw = (int)ceil(W * scale), ph = (int)ceil(H * scale);
	struct cairo_buffer *buf = cairo_buffer_create(pw, ph);
	if (!buf) return;
	cairo_t *cr = cairo_create(buf->surface);
	cairo_scale(cr, scale, scale);

	struct flwm_config *c = &server->config;
	uint32_t bg = d->active ? c->active_title : c->inactive_title;
	uint32_t fg = d->active ? c->active_text : c->inactive_text;
	double rad = view->maximized ? 0 : 7;

	/* rounded top corners; the outline is the window border */
	cairo_new_path(cr);
	cairo_move_to(cr, 0, H);
	cairo_line_to(cr, 0, rad);
	if (rad > 0) cairo_arc(cr, rad, rad, rad, M_PI, 1.5 * M_PI);
	cairo_line_to(cr, W - rad, 0);
	if (rad > 0) cairo_arc(cr, W - rad, rad, rad, 1.5 * M_PI, 2 * M_PI);
	cairo_line_to(cr, W, H);
	cairo_close_path(cr);
	if (B > 0) {
		set_source_argb(cr, border_for(server, d->active));
		cairo_fill_preserve(cr);
		cairo_new_path(cr);
		double ir = rad > B ? rad - B : 0;
		cairo_move_to(cr, B, H);
		cairo_line_to(cr, B, B + ir);
		if (ir > 0) cairo_arc(cr, B + ir, B + ir, ir, M_PI, 1.5 * M_PI);
		cairo_line_to(cr, W - B - ir, B);
		if (ir > 0) cairo_arc(cr, W - B - ir, B + ir, ir, 1.5 * M_PI, 2 * M_PI);
		cairo_line_to(cr, W - B, H);
		cairo_close_path(cr);
	}
	set_source_argb(cr, bg);
	cairo_fill(cr);

	/* buttons on the right: minimize, maximize, close */
	double bx = W - B - T;
	draw_button(cr, FLWM_PART_CLOSE, bx, B, T, fg, d->hover_button == FLWM_PART_CLOSE, view->maximized);
	bx -= T;
	draw_button(cr, FLWM_PART_MAXIMIZE, bx, B, T, fg, d->hover_button == FLWM_PART_MAXIMIZE, view->maximized);
	bx -= T;
	draw_button(cr, FLWM_PART_MINIMIZE, bx, B, T, fg, d->hover_button == FLWM_PART_MINIMIZE, view->maximized);

	/* title text, centered when there is room, ellipsized otherwise */
	const char *title = view_title(view) ? view_title(view) : "";
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *fd = pango_font_description_from_string(c->font ? c->font : "Sans Bold 10");
	pango_layout_set_font_description(layout, fd);
	pango_font_description_free(fd);
	pango_layout_set_text(layout, title, -1);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	int text_left = B + 12, text_right = (int)bx - 8;
	int avail = text_right - text_left;
	if (avail > 10) {
		pango_layout_set_width(layout, avail * PANGO_SCALE);
		int tw, th;
		pango_layout_get_pixel_size(layout, &tw, &th);
		double tx = (W - tw) / 2.0; /* centered over the whole bar */
		if (tx < text_left) tx = text_left;
		if (tx + tw > text_right) tx = text_right - tw;
		set_source_argb(cr, fg);
		cairo_move_to(cr, tx, B + (T - th) / 2.0);
		pango_cairo_show_layout(cr, layout);
	}
	g_object_unref(layout);
	cairo_destroy(cr);
	cairo_surface_flush(buf->surface);

	wlr_scene_buffer_set_buffer(d->title, &buf->base);
	wlr_scene_buffer_set_dest_size(d->title, W, H);
	wlr_buffer_drop(&buf->base);
	wlr_scene_node_set_position(&d->title->node, -B, -T - B);
}

void decor_create(struct flwm_view *view) {
	struct flwm_decor *d = &view->decor;
	if (d->tree) return;
	d->tree = wlr_scene_tree_create(view->tree);
	wlr_scene_node_lower_to_bottom(&d->tree->node);
	float transparent[4] = {0, 0, 0, 0};
	/* created first: below the borders and the title */
	for (int i = 2; i >= 0; i--) {
		float a = shadow_alpha[i];
		float col[4] = {0, 0, 0, a}; /* premultiplied black */
		d->shadow[i] = wlr_scene_rect_create(d->tree, 1, 1, col);
		wlr_scene_node_set_enabled(&d->shadow[i]->node, false);
	}
	for (int i = 0; i < 8; i++) {
		d->handle[i] = wlr_scene_rect_create(d->tree, 1, 1, transparent);
		d->handle_desc[i].kind = FLWM_NODE_DECOR;
		d->handle_desc[i].ptr = view;
		d->handle_desc[i].part = FLWM_PART_EDGE;
		d->handle[i]->node.data = &d->handle_desc[i];
	}
	static const uint32_t edges[8] = {
		WLR_EDGE_TOP, WLR_EDGE_BOTTOM, WLR_EDGE_LEFT, WLR_EDGE_RIGHT,
		WLR_EDGE_TOP | WLR_EDGE_LEFT, WLR_EDGE_TOP | WLR_EDGE_RIGHT,
		WLR_EDGE_BOTTOM | WLR_EDGE_LEFT, WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT,
	};
	for (int i = 0; i < 8; i++) d->handle_desc[i].edges = edges[i];
	float bc[4];
	argb_to_float(view->server->config.border_color, bc);
	for (int i = 0; i < 4; i++) {
		d->border[i] = wlr_scene_rect_create(d->tree, 1, 1, bc);
		d->border[i]->node.data = &d->title_desc; /* clicking a border acts like the title */
	}
	d->title = wlr_scene_buffer_create(d->tree, NULL);
	d->title_desc.kind = FLWM_NODE_DECOR;
	d->title_desc.ptr = view;
	d->title_desc.part = FLWM_PART_TITLE;
	d->title->node.data = &d->title_desc;
	d->width = d->height = -1;
}

void decor_destroy(struct flwm_view *view) {
	struct flwm_decor *d = &view->decor;
	if (d->tree) wlr_scene_node_destroy(&d->tree->node);
	free(d->title_text);
	memset(d, 0, sizeof *d);
}

void decor_update(struct flwm_view *view) {
	struct flwm_decor *d = &view->decor;
	bool show = view->ssd && !view->fullscreen && view->role == FLWM_ROLE_NORMAL;
	if (!show) {
		if (d->tree) wlr_scene_node_set_enabled(&d->tree->node, false);
		return;
	}
	if (!d->tree) decor_create(view);
	wlr_scene_node_set_enabled(&d->tree->node, true);

	struct wlr_box geo;
	view_get_geometry(view, &geo);
	int w = geo.width, h = geo.height;
	struct flwm_server *server = view->server;
	int B = decor_border(server), T = decor_title_height(server);
	bool active = view_focused(server) == view;
	const char *title = view_title(view) ? view_title(view) : "";
	bool title_changed = !d->title_text || strcmp(d->title_text, title) != 0;
	float scale = view_scale(view);

	bool shadowed = server->config.shadows && !view->maximized && view->snap == FLWM_SNAP_NONE;
	if (w == d->width && h == d->height && active == d->active && !title_changed &&
			scale == d->scale && shadowed == d->shadowed)
		return;

	bool size_changed = w != d->width || h != d->height || shadowed != d->shadowed;
	bool active_changed = active != d->active;
	d->width = w;
	d->height = h;
	d->active = active;
	if (title_changed) {
		free(d->title_text);
		d->title_text = strdup(title);
	}

	if (size_changed) {
		/* borders: top unused (drawn in the title), bottom, left, right */
		wlr_scene_node_set_enabled(&d->border[0]->node, false);
		wlr_scene_rect_set_size(d->border[1], w + 2 * B, B);
		wlr_scene_node_set_position(&d->border[1]->node, -B, h);
		wlr_scene_rect_set_size(d->border[2], B, h);
		wlr_scene_node_set_position(&d->border[2]->node, -B, 0);
		wlr_scene_rect_set_size(d->border[3], B, h);
		wlr_scene_node_set_position(&d->border[3]->node, w, 0);
		for (int i = 1; i < 4; i++) wlr_scene_node_set_enabled(&d->border[i]->node, B > 0);

		int top = -T - B - HANDLE, left = -B - HANDLE;
		int full_w = w + 2 * B, full_h = h + T + 2 * B;
		/* top, bottom, left, right */
		wlr_scene_rect_set_size(d->handle[0], full_w, HANDLE);
		wlr_scene_node_set_position(&d->handle[0]->node, -B, top);
		wlr_scene_rect_set_size(d->handle[1], full_w, HANDLE);
		wlr_scene_node_set_position(&d->handle[1]->node, -B, h + B);
		wlr_scene_rect_set_size(d->handle[2], HANDLE, full_h);
		wlr_scene_node_set_position(&d->handle[2]->node, left, -T - B);
		wlr_scene_rect_set_size(d->handle[3], HANDLE, full_h);
		wlr_scene_node_set_position(&d->handle[3]->node, w + B, -T - B);
		/* corners (a bit larger so they are easy to grab) */
		int C = HANDLE * 2;
		wlr_scene_rect_set_size(d->handle[4], C, C);
		wlr_scene_node_set_position(&d->handle[4]->node, left, top);
		wlr_scene_rect_set_size(d->handle[5], C, C);
		wlr_scene_node_set_position(&d->handle[5]->node, w + B + HANDLE - C, top);
		wlr_scene_rect_set_size(d->handle[6], C, C);
		wlr_scene_node_set_position(&d->handle[6]->node, left, h + B + HANDLE - C);
		wlr_scene_rect_set_size(d->handle[7], C, C);
		wlr_scene_node_set_position(&d->handle[7]->node, w + B + HANDLE - C, h + B + HANDLE - C);
		for (int i = 0; i < 8; i++)
			wlr_scene_node_set_enabled(&d->handle[i]->node, !view->maximized);
		/* corners above edges */
		for (int i = 4; i < 8; i++) wlr_scene_node_raise_to_top(&d->handle[i]->node);

		/* the shadow: sides and bottom, starting below the rounded title corners */
		d->shadowed = shadowed;
		for (int i = 0; i < 3; i++) {
			int s = shadow_spread[i], sy = -T - B + 8;
			wlr_scene_rect_set_size(d->shadow[i], w + 2 * (B + s), h + B + s - sy);
			wlr_scene_node_set_position(&d->shadow[i]->node, -B - s, sy);
			wlr_scene_node_set_enabled(&d->shadow[i]->node, shadowed);
		}
	}
	if (size_changed || active_changed) {
		float bc[4];
		argb_to_float(border_for(server, active), bc);
		for (int i = 0; i < 4; i++) wlr_scene_rect_set_color(d->border[i], bc);
	}
	render_title(view);
}

void decor_set_hover(struct flwm_view *view, enum flwm_decor_part part) {
	if (part != FLWM_PART_CLOSE && part != FLWM_PART_MAXIMIZE && part != FLWM_PART_MINIMIZE)
		part = FLWM_PART_NONE;
	if (view->decor.hover_button == (int)part) return;
	view->decor.hover_button = part;
	if (view->decor.tree && view->ssd) render_title(view);
}

enum flwm_decor_part decor_part_at(struct flwm_view *view, double lx, double ly) {
	struct flwm_server *server = view->server;
	int B = decor_border(server), T = decor_title_height(server);
	double rx = lx - (view->x - B);
	double ry = ly - (view->y - T - B);
	int W = view->decor.width + 2 * B;
	if (ry < B || ry >= T + B) return FLWM_PART_TITLE;
	double from_right = W - B - rx;
	if (from_right >= 0 && from_right < T) return FLWM_PART_CLOSE;
	if (from_right >= T && from_right < 2 * T) return FLWM_PART_MAXIMIZE;
	if (from_right >= 2 * T && from_right < 3 * T) return FLWM_PART_MINIMIZE;
	return FLWM_PART_TITLE;
}
