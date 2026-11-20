/* fl-workspaces: lists and switches workspaces through ext-workspace-v1, for
 * the workspace tests of fl-wm.
 *
 *   fl-workspaces list          prints "workspace NAME active|inactive" for
 *                               each workspace, then "toplevel APP_ID" for each
 *                               window in the panel's list (wlr-foreign-toplevel)
 *   fl-workspaces activate NAME switches to the workspace called NAME
 * Exits 0, 1 on errors, or 3 when the compositor lacks ext-workspace-v1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include "ext-workspace-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#define MAX 64

static struct ext_workspace_manager_v1 *ws_mgr;
static struct zwlr_foreign_toplevel_manager_v1 *ftl_mgr;
static struct {
	struct ext_workspace_handle_v1 *h;
	char name[64];
	int active;
} ws[MAX];
static int nws;
static char app_ids[MAX][128];
static int ntl;

static void ws_id(void *d, struct ext_workspace_handle_v1 *h, const char *id) {}
static void ws_name(void *d, struct ext_workspace_handle_v1 *h, const char *name) {
	snprintf(ws[(long)d].name, sizeof ws[0].name, "%s", name);
}
static void ws_coordinates(void *d, struct ext_workspace_handle_v1 *h, struct wl_array *c) {}
static void ws_state(void *d, struct ext_workspace_handle_v1 *h, uint32_t state) {
	ws[(long)d].active = (state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE) != 0;
}
static void ws_capabilities(void *d, struct ext_workspace_handle_v1 *h, uint32_t caps) {}
static void ws_removed(void *d, struct ext_workspace_handle_v1 *h) { ws[(long)d].h = NULL; }
static const struct ext_workspace_handle_v1_listener ws_listener = {
	ws_id, ws_name, ws_coordinates, ws_state, ws_capabilities, ws_removed};

static void g_caps(void *d, struct ext_workspace_group_handle_v1 *g, uint32_t c) {}
static void g_out_enter(void *d, struct ext_workspace_group_handle_v1 *g, struct wl_output *o) {}
static void g_out_leave(void *d, struct ext_workspace_group_handle_v1 *g, struct wl_output *o) {}
static void g_ws_enter(void *d, struct ext_workspace_group_handle_v1 *g, struct ext_workspace_handle_v1 *w) {}
static void g_ws_leave(void *d, struct ext_workspace_group_handle_v1 *g, struct ext_workspace_handle_v1 *w) {}
static void g_removed(void *d, struct ext_workspace_group_handle_v1 *g) {}
static const struct ext_workspace_group_handle_v1_listener group_listener = {
	g_caps, g_out_enter, g_out_leave, g_ws_enter, g_ws_leave, g_removed};

static void m_group(void *d, struct ext_workspace_manager_v1 *m, struct ext_workspace_group_handle_v1 *g) {
	ext_workspace_group_handle_v1_add_listener(g, &group_listener, NULL);
}
static void m_workspace(void *d, struct ext_workspace_manager_v1 *m, struct ext_workspace_handle_v1 *h) {
	if (nws >= MAX) return;
	ws[nws].h = h;
	ext_workspace_handle_v1_add_listener(h, &ws_listener, (void *)(long)nws);
	nws++;
}
static void m_done(void *d, struct ext_workspace_manager_v1 *m) {}
static void m_finished(void *d, struct ext_workspace_manager_v1 *m) {}
static const struct ext_workspace_manager_v1_listener mgr_listener = {m_group, m_workspace, m_done, m_finished};

static void t_title(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *t) {}
static void t_app_id(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *id) {
	snprintf(app_ids[(long)d], sizeof app_ids[0], "%s", id);
}
static void t_out_enter(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) {}
static void t_out_leave(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) {}
static void t_state(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *s) {}
static void t_done(void *d, struct zwlr_foreign_toplevel_handle_v1 *h) {}
static void t_closed(void *d, struct zwlr_foreign_toplevel_handle_v1 *h) { app_ids[(long)d][0] = 0; }
static void t_parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct zwlr_foreign_toplevel_handle_v1 *p) {}
static const struct zwlr_foreign_toplevel_handle_v1_listener tl_listener = {
	t_title, t_app_id, t_out_enter, t_out_leave, t_state, t_done, t_closed, t_parent};

static void f_toplevel(void *d, struct zwlr_foreign_toplevel_manager_v1 *m, struct zwlr_foreign_toplevel_handle_v1 *h) {
	if (ntl >= MAX) return;
	strcpy(app_ids[ntl], "?");
	zwlr_foreign_toplevel_handle_v1_add_listener(h, &tl_listener, (void *)(long)ntl);
	ntl++;
}
static void f_finished(void *d, struct zwlr_foreign_toplevel_manager_v1 *m) {}
static const struct zwlr_foreign_toplevel_manager_v1_listener ftl_listener = {f_toplevel, f_finished};

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
	if (!strcmp(iface, ext_workspace_manager_v1_interface.name)) {
		ws_mgr = wl_registry_bind(r, name, &ext_workspace_manager_v1_interface, 1);
		ext_workspace_manager_v1_add_listener(ws_mgr, &mgr_listener, NULL);
	} else if (!strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name)) {
		ftl_mgr = wl_registry_bind(r, name, &zwlr_foreign_toplevel_manager_v1_interface, ver < 3 ? ver : 3);
		zwlr_foreign_toplevel_manager_v1_add_listener(ftl_mgr, &ftl_listener, NULL);
	}
}
static void global_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = {global, global_remove};

int main(int argc, char **argv) {
	if (argc < 2 || (strcmp(argv[1], "list") && (strcmp(argv[1], "activate") || argc < 3))) {
		fprintf(stderr, "usage: fl-workspaces list | activate NAME\n");
		return 1;
	}
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "fl-workspaces: cannot connect to the compositor\n");
		return 1;
	}
	wl_registry_add_listener(wl_display_get_registry(dpy), &reg_listener, NULL);
	wl_display_roundtrip(dpy); /* the globals */
	wl_display_roundtrip(dpy); /* their objects */
	wl_display_roundtrip(dpy); /* and the objects' state */
	if (!ws_mgr) {
		fprintf(stderr, "fl-workspaces: the compositor has no ext-workspace-v1\n");
		return 3;
	}
	if (!strcmp(argv[1], "list")) {
		for (int i = 0; i < nws; i++)
			if (ws[i].h) printf("workspace %s %s\n", ws[i].name, ws[i].active ? "active" : "inactive");
		for (int i = 0; i < ntl; i++)
			if (app_ids[i][0]) printf("toplevel %s\n", app_ids[i]);
		return 0;
	}
	for (int i = 0; i < nws; i++) {
		if (ws[i].h && !strcmp(ws[i].name, argv[2])) {
			ext_workspace_handle_v1_activate(ws[i].h);
			ext_workspace_manager_v1_commit(ws_mgr);
			wl_display_roundtrip(dpy);
			return 0;
		}
	}
	fprintf(stderr, "fl-workspaces: no workspace called %s\n", argv[2]);
	return 1;
}
