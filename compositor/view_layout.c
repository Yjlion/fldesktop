/* fl-wm: where windows go - window rules (rules.h), the last session's
 * places (session.h), tiling (tile.h) and keeping windows above the others. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "server.h"
#include "view_internal.h"

/* ---- window rules (rules.h) --------------------------------------------- */

static void apply_opacity_to_buffer(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
	(void)sx;
	(void)sy;
	wlr_scene_buffer_set_opacity(buffer, *(float *)data);
}

/* the window's surfaces (not its decorations) drawn with view->opacity */
void view_apply_opacity(struct flwm_view *view) {
	if (!view->surface_tree || view->opacity <= 0) return;
	float o = view->opacity;
	wlr_scene_node_for_each_buffer(&view->surface_tree->node, apply_opacity_to_buffer, &o);
}

void view_set_above(struct flwm_view *view, bool above) {
	if (view->role != FLWM_ROLE_NORMAL) return;
	view->above = above;
	if (!view->fullscreen)
		wlr_scene_node_reparent(&view->tree->node, above ? view->server->layer_above : view->server->layer_views);
}

static struct flwm_output *output_named(struct flwm_server *server, const char *name) {
	struct flwm_output *o;
	wl_list_for_each(o, &server->outputs, link)
		if (!strcmp(o->wlr_output->name, name)) return o;
	return NULL;
}

/* the rules for a window that maps for the first time; may change what the
 * client asked for (maximized, fullscreen) and whether it gets the focus */
/* a window of a program fl-autostart reopened: back where it was */
static void apply_session(struct flwm_view *view, bool *maximized, bool *fullscreen, bool *focus) {
	struct flwm_server *server = view->server;
	if (server->nlast_session == 0 || time(NULL) > server->session_until) return;
	struct flwm_session_entry *e = session_take(server->last_session, server->nlast_session, view_app_id(view));
	if (!e) return;
	struct flwm_output *o = output_named(server, e->output);
	if (e->w >= 64 && e->h >= 64) view_resize_to(view, e->w, e->h);
	/* only where an output still is */
	if (output_at(server, e->x + 8, e->y + 8) || o) view_set_position(view, e->x, e->y);
	if (e->workspace > 0 && e->workspace < server->config.workspaces) {
		view_move_to_workspace(view, e->workspace);
		if (!view_on_current(view)) *focus = false;
	}
	*maximized = *maximized || e->maximized;
	*fullscreen = *fullscreen || e->fullscreen;
	wlr_log(WLR_DEBUG, "session: %s restored", e->app_id);
}

/* ---- tiling (tile.h) ---------------------------------------------------- */

/* windows that tile: normal, shown here, not dialogs, not of a fixed size */
static bool tiles(struct flwm_view *v, struct flwm_output *o) {
	if (!v->mapped || v->role != FLWM_ROLE_NORMAL || v->minimized || v->fullscreen || v->sticky) return false;
	if (!view_on_current(v) || view_parent(v) || output_for_view(v) != o) return false;
	if (!is_x11(v)) {
		struct wlr_xdg_toplevel_state *s = &v->xdg_toplevel->current;
		if (s->min_width > 0 && s->min_width == s->max_width && s->min_height > 0 && s->min_height == s->max_height)
			return false;
	}
	return true;
}

void view_tile_arrange(struct flwm_server *server) {
	struct flwm_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		int ws = server->config.workspaces_per_output ? o->workspace : server->cur_workspace;
		if (ws < 0 || ws >= FLWM_MAX_WORKSPACES || !server->tiled[ws]) continue;
		struct flwm_view *list[64], *v;
		int n = 0;
		wl_list_for_each(v, &server->views, link)
			if (n < 64 && tiles(v, o)) list[n++] = v;
		if (n == 0) continue;
		/* the oldest first: it is the master */
		for (int i = 1; i < n; i++)
			for (int j = i; j > 0 && list[j]->serial < list[j - 1]->serial; j--) {
				struct flwm_view *t = list[j];
				list[j] = list[j - 1];
				list[j - 1] = t;
			}
		struct flwm_tile_box boxes[64];
		struct wlr_box ua = o->usable_area;
		tile_layout((struct flwm_tile_box){ua.x, ua.y, ua.width, ua.height}, n, server->config.tile_ratio,
			server->config.tile_gap, boxes);
		for (int i = 0; i < n; i++) {
			if (list[i]->maximized) view_set_maximized(list[i], false);
			list[i]->snap = FLWM_SNAP_NONE;
			struct wlr_box b = {boxes[i].x, boxes[i].y, boxes[i].w, boxes[i].h};
			apply_box(list[i], &b);
		}
	}
}

void view_save_session(struct flwm_server *server) {
	char path[1024];
	session_path(path, sizeof path);
	if (!server->config.restore_session) {
		remove(path);
		return;
	}
	struct flwm_session_entry e[FLWM_MAX_SESSION];
	int n = 0;
	struct flwm_view *v;
	/* bottom first: windows open again in their stacking order */
	wl_list_for_each_reverse(v, &server->views, link) {
		if (n >= FLWM_MAX_SESSION) break;
		if (!v->mapped || v->role != FLWM_ROLE_NORMAL || !view_app_id(v) || view_parent(v)) continue;
		struct flwm_session_entry *s = &e[n++];
		memset(s, 0, sizeof *s);
		snprintf(s->app_id, sizeof s->app_id, "%s", view_app_id(v));
		struct wlr_box g;
		if (v->maximized || v->fullscreen || v->snap != FLWM_SNAP_NONE) g = v->saved;
		else content_geometry(v, &g), g.x = v->x, g.y = v->y;
		s->x = g.x;
		s->y = g.y;
		s->w = g.width;
		s->h = g.height;
		s->workspace = v->sticky ? 0 : v->workspace;
		s->maximized = v->maximized;
		s->fullscreen = v->fullscreen;
		struct flwm_output *o = output_for_view(v);
		if (o) snprintf(s->output, sizeof s->output, "%s", o->wlr_output->name);
	}
	session_write(path, e, n);
}

void view_apply_rules(struct flwm_view *view, bool *maximized, bool *fullscreen, bool *focus) {
	struct flwm_server *server = view->server;
	if (view->rules_applied) return;
	view->rules_applied = true;
	apply_session(view, maximized, fullscreen, focus);
	if (server->config.nrules == 0) return;
	struct flwm_rule_result r;
	rules_apply(server->config.rules, server->config.nrules, view_app_id(view), view_title(view), &r);
	if (!r.matched) return;
	wlr_log(WLR_DEBUG, "rules: %d match %s", r.matched, view_app_id(view) ? view_app_id(view) : "(no app id)");
	struct flwm_output *o = r.output ? output_named(server, r.output) : NULL;
	if (r.width > 0) view_resize_to(view, r.width, r.height);
	if (o || r.has_position) {
		if (!o) o = output_for_view(view);
		if (o) {
			struct wlr_box ua = o->usable_area, geo;
			content_geometry(view, &geo);
			int w = r.width > 0 ? r.width : geo.width, h = r.height > 0 ? r.height : geo.height;
			int x, y;
			if (r.has_position && !r.center) {
				x = ua.x + r.x;
				y = ua.y + r.y + deco_top(view);
			} else {
				x = ua.x + (ua.width - w) / 2;
				y = ua.y + (ua.height - h + deco_top(view)) / 2;
			}
			view_set_position(view, x, y);
		}
	}
	if (r.workspace > 0 && r.workspace <= server->config.workspaces) {
		view_move_to_workspace(view, r.workspace - 1);
		if (!view_on_current(view)) *focus = false;
	}
	if (r.sticky >= 0) view_set_sticky(view, r.sticky);
	if (r.above >= 0) view_set_above(view, r.above);
	if (r.opacity > 0 && r.opacity < 1) {
		view->opacity = r.opacity;
		view_apply_opacity(view);
	}
	if (r.maximized >= 0) *maximized = r.maximized;
	if (r.fullscreen >= 0) *fullscreen = r.fullscreen;
}

