/* Workspaces: one set shared by all outputs. A window belongs to one
 * workspace (or is sticky); windows of the other workspaces are hidden, left
 * out of the panel's window list and of Alt+Tab. With wlroots 0.20 the set is
 * published through ext-workspace-v1 for the panel's pager. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/util/log.h>

#include "compat.h"
#include "server.h"

#if FLWM_WLR_AT_LEAST(0, 20)
#include <wlr/types/wlr_ext_workspace_v1.h>
#define FLWM_EXT_WORKSPACE 1
#endif

static int count(struct flwm_server *server) {
	int n = server->config.workspaces;
	return n < 1 ? 1 : n > FLWM_MAX_WORKSPACES ? FLWM_MAX_WORKSPACES : n;
}

bool view_on_current(struct flwm_view *view) {
	if (view->role != FLWM_ROLE_NORMAL) return true;
	return view->sticky || view->workspace == view->server->cur_workspace;
}

bool view_is_cyclable(struct flwm_view *view) {
	return view->mapped && view->role == FLWM_ROLE_NORMAL && !view->no_taskbar && view_on_current(view);
}

/* shows or hides a window for the current workspace */
static void update_visibility(struct flwm_view *view) {
	if (!view->mapped || view->role != FLWM_ROLE_NORMAL) return;
	bool here = view_on_current(view);
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
#ifdef FLWM_EXT_WORKSPACE
	if (server->workspace_group) wlr_ext_workspace_group_handle_v1_output_leave(server->workspace_group, output);
#else
	(void)server, (void)output;
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
	if (server->cur_workspace >= n) workspace_switch(server, n - 1);
}

/* ---- switching and moving ------------------------------------------------- */

void workspace_switch(struct flwm_server *server, int n) {
	if (n < 0 || n >= count(server) || n == server->cur_workspace) return;
	if (server->grabbed_view) input_reset_cursor_mode(server);
	if (switcher_active(server)) switcher_finish(server, false);
	server->cycling = false;
	server->cycle_current = NULL;
	server->cur_workspace = n;
	struct flwm_view *v;
	wl_list_for_each(v, &server->views, link) update_visibility(v);
#ifdef FLWM_EXT_WORKSPACE
	if (server->workspace_mgr) publish_active(server);
#endif
	wlr_log(WLR_INFO, "workspace: %d", n + 1);
	/* the window used last on this workspace (server->views is in focus order) */
	struct flwm_view *focused = view_focused(server);
	if (!focused || focused->role == FLWM_ROLE_NORMAL || focused->role == FLWM_ROLE_DESKTOP)
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
}

void view_set_sticky(struct flwm_view *view, bool sticky) {
	if (view->role != FLWM_ROLE_NORMAL) return;
	view->sticky = sticky;
	/* unsticking keeps the window where the user is */
	view->workspace = view->server->cur_workspace;
	update_visibility(view);
}
