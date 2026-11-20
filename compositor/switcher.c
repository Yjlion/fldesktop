/* Alt+Tab window switcher: a panel in the middle of the screen with the
 * windows (icon and title) in focus order. Tab / Shift+Tab and the arrow
 * keys move the selection while Alt is held; releasing Alt (or Enter, or a
 * click) switches to it, Escape cancels. Drawn with cairo/pango into a scene
 * buffer, like the title bars. */
#include "server.h"

#include <cairo.h>
#include <ctype.h>
#include <dirent.h>
#include <drm_fourcc.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/util/log.h>
#ifdef HAVE_RSVG
#include <librsvg/rsvg.h>
#endif

#define CELL_W 132
#define CELL_H 124
#define ICON 64
#define PAD 18
#define TITLE_H 34

/* ---- buffer ------------------------------------------------------------- */

struct sw_buffer {
	struct wlr_buffer base;
	cairo_surface_t *surface;
};

static void sw_buffer_destroy(struct wlr_buffer *b) {
	struct sw_buffer *buf = wl_container_of(b, buf, base);
	flwm_buffer_finish(b);
	cairo_surface_destroy(buf->surface);
	free(buf);
}

static bool sw_buffer_begin(struct wlr_buffer *b, uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct sw_buffer *buf = wl_container_of(b, buf, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) return false;
	*data = cairo_image_surface_get_data(buf->surface);
	*format = DRM_FORMAT_ARGB8888;
	*stride = cairo_image_surface_get_stride(buf->surface);
	return true;
}

static void sw_buffer_end(struct wlr_buffer *b) {}

static const struct wlr_buffer_impl sw_buffer_impl = {
	.destroy = sw_buffer_destroy,
	.begin_data_ptr_access = sw_buffer_begin,
	.end_data_ptr_access = sw_buffer_end,
};

/* ---- settings from the desktop's own config (accent, icon theme) ------- */

static char *config_home(void) {
	const char *x = getenv("XDG_CONFIG_HOME");
	char *p = NULL;
	if (x && *x) {
		if (asprintf(&p, "%s/fldesktop/desktop.ini", x) < 0) p = NULL;
	} else {
		const char *h = getenv("HOME");
		if (asprintf(&p, "%s/.config/fldesktop/desktop.ini", h ? h : "") < 0) p = NULL;
	}
	return p;
}

/* value of [group] key in an ini file, or NULL (malloc'ed) */
static char *ini_get(const char *path, const char *group, const char *key) {
	FILE *f = path ? fopen(path, "r") : NULL;
	if (!f) return NULL;
	char line[1024], section[128] = "";
	char *res = NULL;
	while (!res && fgets(line, sizeof line, f)) {
		char *l = line;
		while (isspace((unsigned char)*l)) l++;
		char *e = l + strlen(l);
		while (e > l && isspace((unsigned char)e[-1])) *--e = 0;
		if (*l == '[') {
			char *c = strchr(l, ']');
			if (c) *c = 0;
			snprintf(section, sizeof section, "%s", l + 1);
			continue;
		}
		char *eq = strchr(l, '=');
		if (!eq || strcasecmp(section, group)) continue;
		*eq = 0;
		char *k = l, *v = eq + 1;
		char *ke = k + strlen(k);
		while (ke > k && isspace((unsigned char)ke[-1])) *--ke = 0;
		while (isspace((unsigned char)*v)) v++;
		if (!strcmp(k, key)) res = strdup(v);
	}
	fclose(f);
	return res;
}

static uint32_t accent_color(void) {
	uint32_t c = 0xff3584e4;
	char *path = config_home();
	char *v = ini_get(path, "Appearance", "Accent");
	if (v && v[0] == '#' && strlen(v) >= 7) c = 0xff000000 | (uint32_t)strtoul(v + 1, NULL, 16);
	free(v);
	free(path);
	return c;
}

/* ---- icons ------------------------------------------------------------- */

static bool file_exists(const char *p) { return p && access(p, R_OK) == 0; }

/* the data directories, in lookup order */
static int data_dirs(char dirs[][512], int max) {
	int n = 0;
	const char *home = getenv("XDG_DATA_HOME");
	if (home && *home) snprintf(dirs[n++], 512, "%s", home);
	else snprintf(dirs[n++], 512, "%s/.local/share", getenv("HOME") ? getenv("HOME") : "");
	const char *xd = getenv("XDG_DATA_DIRS");
	char *copy = strdup(xd && *xd ? xd : "/usr/local/share:/usr/share");
	char *save = NULL;
	for (char *t = strtok_r(copy, ":", &save); t && n < max; t = strtok_r(NULL, ":", &save))
		snprintf(dirs[n++], 512, "%s", t);
	free(copy);
	return n;
}

/* Icon= of the application's .desktop file (malloc'ed), or NULL */
static char *desktop_icon(const char *app_id, char dirs[][512], int ndirs) {
	if (!app_id || !*app_id) return NULL;
	char path[1100];
	for (int i = 0; i < ndirs; i++) {
		snprintf(path, sizeof path, "%.511s/applications/%.255s.desktop", dirs[i], app_id);
		char *icon = ini_get(path, "Desktop Entry", "Icon");
		if (icon) return icon;
		/* lower case, and the last part of a reverse-DNS id */
		char low[256];
		snprintf(low, sizeof low, "%s", app_id);
		for (char *c = low; *c; c++) *c = (char)tolower((unsigned char)*c);
		snprintf(path, sizeof path, "%.511s/applications/%.255s.desktop", dirs[i], low);
		icon = ini_get(path, "Desktop Entry", "Icon");
		if (icon) return icon;
	}
	/* StartupWMClass= matches */
	for (int i = 0; i < ndirs; i++) {
		char adir[600];
		snprintf(adir, sizeof adir, "%.511s/applications", dirs[i]);
		DIR *d = opendir(adir);
		if (!d) continue;
		struct dirent *de;
		char *found = NULL;
		while (!found && (de = readdir(d))) {
			size_t l = strlen(de->d_name);
			if (l < 9 || strcmp(de->d_name + l - 8, ".desktop")) continue;
			snprintf(path, sizeof path, "%.590s/%.255s", adir, de->d_name);
			char *wm = ini_get(path, "Desktop Entry", "StartupWMClass");
			if (wm && !strcasecmp(wm, app_id)) found = ini_get(path, "Desktop Entry", "Icon");
			free(wm);
		}
		closedir(d);
		if (found) return found;
	}
	return NULL;
}

static char *find_icon_file(const char *name, char dirs[][512], int ndirs, const char *theme) {
	if (!name || !*name) return NULL;
	if (name[0] == '/') return file_exists(name) ? strdup(name) : NULL;
	const char *themes[] = {theme, "fldesktop", "hicolor", "Adwaita", NULL};
	const char *subdirs[] = {"scalable/apps", "256x256/apps", "128x128/apps", "96x96/apps", "64x64/apps",
				 "48x48/apps", "512x512/apps", "32x32/apps", "scalable/mimetypes", "48x48/mimetypes",
				 "64x64/mimetypes", NULL};
	char path[1100];
	for (int t = 0; t < 5; t++) {
		if (!themes[t] || !*themes[t]) continue;
		if (t > 0 && theme && !strcmp(themes[t], theme)) continue;
		for (int i = 0; i < ndirs; i++)
			for (int s = 0; subdirs[s]; s++)
				for (int e = 0; e < 2; e++) {
					snprintf(path, sizeof path, "%.511s/icons/%.100s/%.40s/%.255s.%s", dirs[i], themes[t], subdirs[s], name,
						 e ? "png" : "svg");
					if (file_exists(path)) return strdup(path);
				}
	}
	for (int i = 0; i < ndirs; i++)
		for (int e = 0; e < 2; e++) {
			snprintf(path, sizeof path, "%.511s/pixmaps/%.255s.%s", dirs[i], name, e ? "png" : "svg");
			if (file_exists(path)) return strdup(path);
		}
	return NULL;
}

/* draws the image file into a size x size square at x, y */
static bool draw_icon_file(cairo_t *cr, const char *path, double x, double y, double size) {
	size_t l = strlen(path);
	if (l > 4 && !strcasecmp(path + l - 4, ".png")) {
		cairo_surface_t *img = cairo_image_surface_create_from_png(path);
		if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
			cairo_surface_destroy(img);
			return false;
		}
		int w = cairo_image_surface_get_width(img), h = cairo_image_surface_get_height(img);
		double s = size / (w > h ? w : h);
		cairo_save(cr);
		cairo_translate(cr, x + (size - w * s) / 2, y + (size - h * s) / 2);
		cairo_scale(cr, s, s);
		cairo_set_source_surface(cr, img, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
		cairo_restore(cr);
		cairo_surface_destroy(img);
		return true;
	}
#ifdef HAVE_RSVG
	GError *err = NULL;
	RsvgHandle *h = rsvg_handle_new_from_file(path, &err);
	if (!h) {
		if (err) g_error_free(err);
		return false;
	}
	RsvgRectangle vp = {x, y, size, size};
	bool ok = rsvg_handle_render_document(h, cr, &vp, &err);
	if (err) g_error_free(err);
	g_object_unref(h);
	return ok;
#else
	return false;
#endif
}

struct icon_cache_entry {
	char *app_id;
	char *path; /* NULL: none found */
	struct icon_cache_entry *next;
};
static struct icon_cache_entry *icon_cache;

static const char *icon_for(const char *app_id) {
	const char *key = app_id ? app_id : "";
	for (struct icon_cache_entry *e = icon_cache; e; e = e->next)
		if (!strcmp(e->app_id, key)) return e->path;
	char dirs[16][512];
	int nd = data_dirs(dirs, 16);
	char *cfg = config_home();
	char *theme = ini_get(cfg, "General", "IconTheme");
	free(cfg);
	char *name = desktop_icon(app_id, dirs, nd);
	char *path = find_icon_file(name ? name : key, dirs, nd, theme);
	if (!path) path = find_icon_file("application-x-executable", dirs, nd, theme);
	free(name);
	free(theme);
	struct icon_cache_entry *e = calloc(1, sizeof *e);
	e->app_id = strdup(key);
	e->path = path;
	e->next = icon_cache;
	icon_cache = e;
	return path;
}

/* ---- drawing ----------------------------------------------------------- */

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
	cairo_close_path(cr);
}

static void set_argb(cairo_t *cr, uint32_t c, double alpha) {
	cairo_set_source_rgba(cr, ((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0,
			      alpha * ((c >> 24) & 0xff) / 255.0);
}

static void draw_text(cairo_t *cr, const char *font, const char *text, double x, double y, double w, int lines,
		      bool center) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *fd = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, fd);
	pango_font_description_free(fd);
	pango_layout_set_text(layout, text, -1);
	pango_layout_set_width(layout, (int)(w * PANGO_SCALE));
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	pango_layout_set_height(layout, -lines);
	if (center) pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
	cairo_move_to(cr, x, y);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
}

static void sw_layout(struct flwm_switcher *s, int out_w, int *cols, int *rows, int *W, int *H) {
	int maxcols = (int)((out_w * 0.9 - 2 * PAD) / CELL_W);
	if (maxcols < 1) maxcols = 1;
	*cols = s->n < maxcols ? s->n : maxcols;
	*rows = (s->n + *cols - 1) / *cols;
	*W = *cols * CELL_W + 2 * PAD;
	*H = *rows * CELL_H + 2 * PAD + TITLE_H;
}

static void sw_render(struct flwm_server *server) {
	struct flwm_switcher *s = &server->switcher;
	if (!s->node || s->n == 0) return;
	struct flwm_output *o = s->output;
	if (!o) return;
	float scale = o->wlr_output->scale > 0 ? o->wlr_output->scale : 1;
	int cols, rows, W, H;
	sw_layout(s, o->full_area.width, &cols, &rows, &W, &H);
	int pw = (int)ceil(W * scale), ph = (int)ceil(H * scale);
	struct sw_buffer *buf = calloc(1, sizeof *buf);
	buf->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
	if (cairo_surface_status(buf->surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(buf->surface);
		free(buf);
		return;
	}
	wlr_buffer_init(&buf->base, &sw_buffer_impl, pw, ph);
	cairo_t *cr = cairo_create(buf->surface);
	cairo_scale(cr, scale, scale);

	/* panel */
	rounded_rect(cr, 0.5, 0.5, W - 1, H - 1, 14);
	cairo_set_source_rgba(cr, 0.12, 0.125, 0.14, 0.94);
	cairo_fill_preserve(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);

	uint32_t accent = accent_color();
	const char *font = server->config.font ? server->config.font : "Sans Bold 10";
	/* the item font: the configured family, regular weight */
	PangoFontDescription *fd = pango_font_description_from_string(font);
	pango_font_description_set_weight(fd, PANGO_WEIGHT_NORMAL);
	char *small_font = pango_font_description_to_string(fd);
	pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
	pango_font_description_set_size(fd, pango_font_description_get_size(fd) * 6 / 5);
	char *big_font = pango_font_description_to_string(fd);
	pango_font_description_free(fd);

	for (int i = 0; i < s->n; i++) {
		struct flwm_view *v = s->views[i];
		int cx = PAD + (i % cols) * CELL_W, cy = PAD + (i / cols) * CELL_H;
		if (i == s->sel) {
			rounded_rect(cr, cx + 4, cy + 2, CELL_W - 8, CELL_H - 4, 10);
			set_argb(cr, accent, 0.85);
			cairo_fill(cr);
		}
		const char *path = icon_for(view_app_id(v));
		double ix = cx + (CELL_W - ICON) / 2.0, iy = cy + 12;
		if (v->minimized) cairo_push_group(cr);
		bool drawn = path && draw_icon_file(cr, path, ix, iy, ICON);
		if (!drawn) {
			/* a tile with the first letter of the title */
			rounded_rect(cr, ix + 4, iy + 4, ICON - 8, ICON - 8, 12);
			set_argb(cr, accent, 1);
			cairo_fill(cr);
			const char *t = view_title(v) && *view_title(v) ? view_title(v) : "?";
			char letter[8] = {0};
			int len = 1;
			while (t[len] && ((unsigned char)t[len] & 0xc0) == 0x80 && len < 4) len++;
			memcpy(letter, t, len);
			cairo_set_source_rgb(cr, 1, 1, 1);
			draw_text(cr, "Sans Bold 26", letter, ix, iy + 12, ICON, 1, true);
		}
		if (v->minimized) {
			cairo_pop_group_to_source(cr);
			cairo_paint_with_alpha(cr, 0.45);
		}
		const char *title = view_title(v) ? view_title(v) : "";
		if (!*title && view_app_id(v)) title = view_app_id(v);
		cairo_set_source_rgba(cr, 1, 1, 1, v->minimized && i != s->sel ? 0.55 : 0.95);
		draw_text(cr, small_font, title, cx + 8, iy + ICON + 8, CELL_W - 16, 2, true);
	}
	/* the selected window's whole title */
	if (s->sel >= 0 && s->sel < s->n) {
		struct flwm_view *v = s->views[s->sel];
		const char *title = view_title(v) && *view_title(v) ? view_title(v)
			: view_app_id(v) ? view_app_id(v) : "";
		char line[600];
		snprintf(line, sizeof line, "%s%s", title, v->minimized ? "  (minimized)" : "");
		cairo_set_source_rgba(cr, 1, 1, 1, 1);
		draw_text(cr, big_font, line, PAD, H - PAD - TITLE_H + 8, W - 2 * PAD, 1, true);
	}
	g_free(small_font);
	g_free(big_font);
	cairo_destroy(cr);
	cairo_surface_flush(buf->surface);
	wlr_scene_buffer_set_buffer(s->node, &buf->base);
	wlr_scene_buffer_set_dest_size(s->node, W, H);
	wlr_buffer_drop(&buf->base);
	s->box.x = o->full_area.x + (o->full_area.width - W) / 2;
	s->box.y = o->full_area.y + (o->full_area.height - H) / 2;
	s->box.width = W;
	s->box.height = H;
	s->cols = cols;
	wlr_scene_node_set_position(&s->node->node, s->box.x, s->box.y);
}

/* ---- the switcher ------------------------------------------------------- */

bool switcher_active(struct flwm_server *server) { return server->switcher.node != NULL; }

/* Opens the switcher (or moves the selection when open). false when there
 * is nothing to switch between. */
bool switcher_step(struct flwm_server *server, int dir) {
	struct flwm_switcher *s = &server->switcher;
	if (!s->node) {
		s->n = 0;
		struct flwm_view *v;
		wl_list_for_each(v, &server->views, link)
			if (view_is_cyclable(v) && s->n < FLWM_SWITCHER_MAX) s->views[s->n++] = v;
		if (s->n < 2) return false;
		s->output = output_at(server, server->cursor->x, server->cursor->y);
		if (!s->output) s->output = output_primary(server);
		if (!s->output) return false;
		/* the focused window is first: start on the one before it */
		s->sel = 0;
		s->node = wlr_scene_buffer_create(server->layer_popups, NULL);
		wlr_scene_node_raise_to_top(&s->node->node);
	}
	s->sel = ((s->sel + dir) % s->n + s->n) % s->n;
	sw_render(server);
	return true;
}

/* Moves the selection to another row (arrow keys up / down). */
void switcher_step_row(struct flwm_server *server, int dir) {
	struct flwm_switcher *s = &server->switcher;
	if (!s->node || s->cols <= 0) return;
	int n = s->sel + dir * s->cols;
	if (n >= 0 && n < s->n) {
		s->sel = n;
		sw_render(server);
	}
}

static void sw_close(struct flwm_server *server) {
	struct flwm_switcher *s = &server->switcher;
	if (s->node) wlr_scene_node_destroy(&s->node->node);
	s->node = NULL;
	s->n = 0;
	s->output = NULL;
}

/* Closes the switcher; with `commit` the selected window gets the focus. */
void switcher_finish(struct flwm_server *server, bool commit) {
	struct flwm_switcher *s = &server->switcher;
	if (!s->node) return;
	struct flwm_view *v = commit && s->sel >= 0 && s->sel < s->n ? s->views[s->sel] : NULL;
	sw_close(server);
	server->cycling = false;
	server->cycle_current = NULL;
	if (v && v->mapped) {
		view_focus(v); /* also un-minimizes */
		wl_list_remove(&v->link);
		wl_list_insert(&server->views, &v->link);
	}
}

/* A click while the switcher is open: picks the item under the pointer, or
 * cancels outside of it. */
void switcher_click(struct flwm_server *server, double lx, double ly) {
	struct flwm_switcher *s = &server->switcher;
	if (!s->node) return;
	int x = (int)lx - s->box.x - PAD, y = (int)ly - s->box.y - PAD;
	if (x >= 0 && y >= 0 && s->cols > 0) {
		int i = (y / CELL_H) * s->cols + x / CELL_W;
		if (x / CELL_W < s->cols && i < s->n) {
			s->sel = i;
			switcher_finish(server, true);
			return;
		}
	}
	switcher_finish(server, false);
}

/* A window went away while the switcher is open. */
void switcher_view_gone(struct flwm_server *server, struct flwm_view *view) {
	struct flwm_switcher *s = &server->switcher;
	if (!s->node) return;
	int j = 0;
	for (int i = 0; i < s->n; i++) {
		if (s->views[i] == view) {
			if (i < s->sel) s->sel--;
			continue;
		}
		s->views[j++] = s->views[i];
	}
	s->n = j;
	if (s->n < 2) {
		sw_close(server);
		server->cycling = false;
		return;
	}
	if (s->sel >= s->n) s->sel = s->n - 1;
	sw_render(server);
}

/* The output went away. */
void switcher_output_gone(struct flwm_server *server, struct flwm_output *o) {
	if (server->switcher.output == o) {
		sw_close(server);
		server->cycling = false;
	}
}
