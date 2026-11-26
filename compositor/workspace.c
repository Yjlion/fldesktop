/* Workspaces: one set shared by all outputs, or with
 * [behaviour] workspaces_per_output one current workspace for each output:
 * switching then changes the output the user is on (the focused window's,
 * else the pointer's) and the others stay. A window belongs to one workspace
 * (or is sticky); windows of the other workspaces are hidden, left out of the
 * panel's window list and of Alt+Tab. cur_workspace is the workspace of the
 * output the user is on, so key bindings, tiling and the pager follow it.
 * With wlroots 0.20 the set is published through ext-workspace-v1 for the
 * panel's pager (its active workspace is that of the output the user is on). */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/util/log.h>

#include "compat.h"
#include "anim.h"
#include "server.h"

#if FLWM_WLR_AT_LEAST(0, 20)
#include <wlr/types/wlr_ext_workspace_v1.h>
#define FLWM_EXT_WORKSPACE 1
#endif

static int count(struct flwm_server *server) {
	int n = server->config.workspaces;
	return n < 1 ? 1 : n > FLWM_MAX_WORKSPACES ? FLWM_MAX_WORKSPACES : n;
}

static bool per_output(struct flwm_server *server) {
	return server->config.workspaces_per_output;
}

/* the current workspace of the output a view is on */
static int workspace_of(struct flwm_view *view) {
	struct flwm_server *server = view->server;
	if (!per_output(server)) return server->cur_workspace;
	struct flwm_output *o = output_for_view(view);
	return o ? o->workspace : server->cur_workspace;
}

bool view_on_current(struct flwm_view *view) {
	if (view->role != FLWM_ROLE_NORMAL) return true;
	return view->sticky || view->workspace == workspace_of(view);
}

/* the output the user is on: the focused window's, else the one switched
 * last (an empty workspace has no window to focus), else the pointer's */
static struct flwm_output *active_output(struct flwm_server *server) {
	struct flwm_view *f = view_focused(server);
	struct flwm_output *o = f && f->role == FLWM_ROLE_NORMAL ? output_for_view(f) : NULL;
	if (!o && server->ws_output) {
		struct flwm_output *it;
		wl_list_for_each(it, &server->outputs, link)
			if (it == server->ws_output) o = it;
	}
	if (!o) o = output_at(server, server->cursor->x, server->cursor->y);
	return o;
}

bool view_is_cyclable(struct flwm_view *view) {
	return view->mapped && view->role == FLWM_ROLE_NORMAL && !view->no_taskbar && view_on_current(view);
}

/* shows or hides a window for the current workspace */
static void update_visibility(struct flwm_view *view) {
	if (!view->mapped || view->role != FLWM_ROLE_NORMAL) return;
	bool here = view_on_current(view);
	struct flwm_output *o = output_for_view(view);
	wlr_log(WLR_DEBUG, "%s: workspace %d on %s, %s", view_title(view) ? view_title(view) : "?", view->workspace + 1,
		o ? o->wlr_output->name : "?", here ? "shown" : "hidden");
	wlr_scene_node_set_enabled(&view->tree->node, here && !view->minimized);
	if (here) view_ftl_create(view);
	else view_ftl_destroy(view);
}

/* ---- ext-workspace-v1 ------------------------------------------------------ */

#ifdef FLWM_EXT_WORKSPACE
static void publish_active(struct flwm_server *server) {
	for (int i = 0; i < server->nworkspace_handles; i++)
		wlr_ext_workspace_handle_v1_set_active(server->workspace_handles[i], i == server->cur_workspace);
}

static void destroy_handles(struct flwm_server *server) {
	for (int i = 0; i < server->nworkspace_handles; i++)
		wlr_ext_workspace_handle_v1_destroy(server->workspace_handles[i]);
	server->nworkspace_handles = 0;
}

static void create_handles(struct flwm_server *server) {
	destroy_handles(server);
	int n = count(server);
	for (int i = 0; i < n; i++) {
		char name[8];
		snprintf(name, sizeof name, "%d", i + 1);
		struct wlr_ext_workspace_handle_v1 *h = wlr_ext_workspace_handle_v1_create(server->workspace_mgr,
			name, EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
		if (!h) break;
		wlr_ext_workspace_handle_v1_set_group(h, server->workspace_group);
		wlr_ext_workspace_handle_v1_set_name(h, name);
		uint32_t coord = i;
		wlr_ext_workspace_handle_v1_set_coordinates(h, &coord, 1);
		server->workspace_handles[server->nworkspace_handles++] = h;
	}
	publish_active(server);
}

static void workspace_commit(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, workspace_commit);
	struct wlr_ext_workspace_v1_commit_event *ev = data;
	struct wlr_ext_workspace_v1_request *req;
	wl_list_for_each(req, ev->requests, link) {
		if (req->type != WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE || !req->activate.workspace) continue;
		for (int i = 0; i < server->nworkspace_handles; i++)
			if (server->workspace_handles[i] == req->activate.workspace) workspace_switch(server, i);
	}
}
#endif

void workspace_init(struct flwm_server *server) {
	server->cur_workspace = 0;
#ifdef FLWM_EXT_WORKSPACE
	server->workspace_mgr = wlr_ext_workspace_manager_v1_create(server->wl_display, 1);
	if (!server->workspace_mgr) return;
	server->workspace_group = wlr_ext_workspace_group_handle_v1_create(server->workspace_mgr, 0);
	server->workspace_commit.notify = workspace_commit;
	wl_signal_add(&server->workspace_mgr->events.commit, &server->workspace_commit);
	create_handles(server);
#endif
}

void workspace_finish(struct flwm_server *server) {
#ifdef FLWM_EXT_WORKSPACE
	if (!server->workspace_mgr) return;
	wl_list_remove(&server->workspace_commit.link);
	destroy_handles(server);
	if (server->workspace_group) wlr_ext_workspace_group_handle_v1_destroy(server->workspace_group);
	server->workspace_group = NULL;
	server->workspace_mgr = NULL; /* freed with the display */
#endif
}

void workspace_output_added(struct flwm_server *server, struct wlr_output *output) {
#ifdef FLWM_EXT_WORKSPACE
	if (server->workspace_group) wlr_ext_workspace_group_handle_v1_output_enter(server->workspace_group, output);
#else
	(void)server, (void)output;
#endif
}

void workspace_output_removed(struct flwm_server *server, struct wlr_output *output) {
	if (server->ws_output && server->ws_output->wlr_output == output) server->ws_output = NULL;
#ifdef FLWM_EXT_WORKSPACE
	if (server->workspace_group) wlr_ext_workspace_group_handle_v1_output_leave(server->workspace_group, output);
#endif
}

void workspace_apply_config(struct flwm_server *server) {
	int n = count(server);
	/* windows of workspaces that are gone move to the last one */
	struct flwm_view *v;
	wl_list_for_each(v, &server->views, link)
		if (v->workspace >= n) v->workspace = n - 1;
#ifdef FLWM_EXT_WORKSPACE
	if (server->workspace_mgr && server->nworkspace_handles != n) create_handles(server);
#endif
	struct flwm_output *o;
	wl_list_for_each(o, &server->outputs, link)
		if (o->workspace >= n) o->workspace = n - 1;
	if (!per_output(server)) {
		/* back to one set: every output shows the current workspace */
		wl_list_for_each(o, &server->outputs, link) o->workspace = server->cur_workspace;
	}
	if (server->cur_workspace >= n) workspace_switch(server, n - 1);
	wl_list_for_each(v, &server->views, link) update_visibility(v);
}

/* ---- switching and moving ------------------------------------------------- */

void workspace_sync(struct flwm_server *server) {
	if (!per_output(server)) return;
	struct flwm_output *o = active_output(server);
	if (!o || o->workspace == server->cur_workspace) return;
	server->cur_workspace = o->workspace;
#ifdef FLWM_EXT_WORKSPACE
	if (server->workspace_mgr) publish_active(server);
#endif
}

void workspace_view_moved(struct flwm_view *view) {
	if (!per_output(view->server) || view->role != FLWM_ROLE_NORMAL || view->sticky) return;
	struct flwm_output *o = output_for_view(view);
	if (o) view->workspace = o->workspace;
}

void workspace_switch(struct flwm_server *server, int n) {
	workspace_sync(server);
	if (n < 0 || n >= count(server) || n == server->cur_workspace) return;
	if (server->grabbed_view) input_reset_cursor_mode(server);
	if (switcher_active(server)) switcher_finish(server, false);
	server->cycling = false;
	server->cycle_current = NULL;
	struct flwm_output *o;
	if (per_output(server)) {
		o = active_output(server);
		if (o) o->workspace = n;
		server->ws_output = o;
		wlr_log(WLR_DEBUG, "workspace %d on %s", n + 1, o ? o->wlr_output->name : "?");
	} else {
		wl_list_for_each(o, &server->outputs, link) o->workspace = n;
	}
	server->cur_workspace = n;
	struct flwm_view *v;
	wl_list_for_each(v, &server->views, link) {
		bool was = v->tree->node.enabled;
		update_visibility(v);
		if (!was && v->tree->node.enabled) anim_view_show(v);
	}
#ifdef FLWM_EXT_WORKSPACE
	if (server->workspace_mgr) publish_active(server);
#endif
	wlr_log(WLR_INFO, "workspace: %d", n + 1);
	view_tile_arrange(server);
	/* the window used last on this workspace (server->views is in focus order) */
	struct flwm_view *focused = view_focused(server);
	if (focused && focused->role != FLWM_ROLE_NORMAL && focused->role != FLWM_ROLE_DESKTOP) return;
	if (per_output(server)) {
		/* the focus stays on the output that switched, even with nothing on it */
		struct flwm_output *on = server->ws_output;
		wl_list_for_each(v, &server->views, link)
			if (view_is_cyclable(v) && !v->minimized && output_for_view(v) == on) {
				view_focus(v);
				return;
			}
		if (focused && focused->role == FLWM_ROLE_NORMAL && output_for_view(focused) != on)
			wlr_seat_keyboard_notify_clear_focus(server->seat);
		else if (focused && !view_on_current(focused))
			wlr_seat_keyboard_notify_clear_focus(server->seat);
		return;
	}
	view_focus_next_available(server);
}

/* takes the keyboard from a window that was just hidden */
static void unfocus_hidden(struct flwm_view *view) {
	struct flwm_server *server = view->server;
	if (view_focused(server) != view) return;
	wlr_seat_keyboard_notify_clear_focus(server->seat);
	/* move it to the end of the focus list so it is not picked again */
	wl_list_remove(&view->link);
	wl_list_insert(server->views.prev, &view->link);
	view_focus_next_available(server);
}

void view_move_to_workspace(struct flwm_view *view, int n) {
	if (view->role != FLWM_ROLE_NORMAL || n < 0 || n >= count(view->server)) return;
	view->sticky = false;
	view->workspace = n;
	update_visibility(view);
	wlr_log(WLR_DEBUG, "%s moves to workspace %d", view_title(view) ? view_title(view) : "?", n + 1);
	if (!view_on_current(view)) unfocus_hidden(view);
	view_tile_arrange(view->server);
}

void view_set_sticky(struct flwm_view *view, bool sticky) {
	if (view->role != FLWM_ROLE_NORMAL) return;
	view->sticky = sticky;
	/* unsticking keeps the window where the user is */
	view->workspace = workspace_of(view);
	update_visibility(view);
}
