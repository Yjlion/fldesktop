/* fl-wm toplevel windows ("views") */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include "xdg-shell-protocol.h"
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#include "server.h"

/* ---- geometry helpers -------------------------------------------------- */

static int deco_top(struct flwm_view *v) {
	return (v->ssd && !v->fullscreen && v->role == FLWM_ROLE_NORMAL)
		? decor_title_height(v->server) + decor_border(v->server) : 0;
}

static int deco_side(struct flwm_view *v) {
	return (v->ssd && !v->fullscreen && v->role == FLWM_ROLE_NORMAL) ? decor_border(v->server) : 0;
}

bool view_is_normal(struct flwm_view *view) {
	return view->role == FLWM_ROLE_NORMAL;
}

static bool is_x11(struct flwm_view *view) {
	return view->type == FLWM_VIEW_XWAYLAND;
}

/* ---- what differs between xdg and X11 windows ---------------------------- */

struct wlr_surface *view_surface(struct flwm_view *view) {
	return is_x11(view) ? xwayland_view_surface(view) : view->xdg_toplevel->base->surface;
}

const char *view_title(struct flwm_view *view) {
	return is_x11(view) ? xwayland_view_title(view) : view->xdg_toplevel->title;
}

const char *view_app_id(struct flwm_view *view) {
	return is_x11(view) ? xwayland_view_app_id(view) : view->xdg_toplevel->app_id;
}

static const char *title_or(struct flwm_view *view, const char *fallback) {
	const char *t = view_title(view);
	return t ? t : fallback;
}

/* the content geometry relative to the surface */
static void content_geometry(struct flwm_view *view, struct wlr_box *geo) {
	if (is_x11(view)) {
		*geo = (struct wlr_box){0, 0, 0, 0};
		xwayland_view_size(view, &geo->width, &geo->height);
	} else {
		flwm_xdg_geometry(view->xdg_toplevel->base, geo);
	}
}

/* ready for configure events (xdg: after the initial commit) */
static bool view_initialized(struct flwm_view *view) {
	return is_x11(view) || view->xdg_toplevel->base->initialized;
}

/* asks the client for a new content size (X11: at the current position) */
static void view_configure_size(struct flwm_view *view, int w, int h) {
	if (is_x11(view)) xwayland_view_configure(view, view->x, view->y, w, h);
	else wlr_xdg_toplevel_set_size(view->xdg_toplevel, w, h);
}

static void view_set_activated(struct flwm_view *view, bool activated) {
	if (is_x11(view)) xwayland_view_activate(view, activated);
	else wlr_xdg_toplevel_set_activated(view->xdg_toplevel, activated);
}

static void view_send_maximized(struct flwm_view *view, bool maximized) {
	if (is_x11(view)) xwayland_view_set_maximized(view, maximized);
	else wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, maximized);
}

static void view_send_fullscreen(struct flwm_view *view, bool fullscreen) {
	if (is_x11(view)) xwayland_view_set_fullscreen(view, fullscreen);
	else wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, fullscreen);
}

/* re-sends the current state (a client asked for the one it has) */
static void view_reconfigure(struct flwm_view *view) {
	if (is_x11(view)) {
		xwayland_view_move(view, view->x, view->y);
	} else {
		wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
	}
}

void view_get_geometry(struct flwm_view *view, struct wlr_box *box) {
	struct wlr_box geo;
	content_geometry(view, &geo);
	box->x = view->x;
	box->y = view->y;
	box->width = geo.width;
	box->height = geo.height;
}

static void update_surface_offset(struct flwm_view *view) {
	if (!view->surface_tree) return; /* an X11 window between unmap and map */
	struct wlr_box geo;
	content_geometry(view, &geo);
	wlr_scene_node_set_position(&view->surface_tree->node, -geo.x, -geo.y);
}

void view_set_position(struct flwm_view *view, int x, int y) {
	bool moved = view->x != x || view->y != y;
	view->x = x;
	view->y = y;
	wlr_scene_node_set_position(&view->tree->node, x, y);
	update_surface_offset(view);
	/* X11 clients place their menus from where they think they are */
	if (is_x11(view) && moved) xwayland_view_move(view, x, y);
}

struct flwm_view *view_focused(struct flwm_server *server) {
	struct wlr_surface *s = server->seat->keyboard_state.focused_surface;
	if (!s) return NULL;
	struct flwm_view *view;
	wl_list_for_each(view, &server->views, link)
		if (view_surface(view) == s) return view;
	return NULL;
}

struct flwm_view *view_find_role(struct flwm_server *server, enum flwm_role role) {
	struct flwm_view *view;
	wl_list_for_each(view, &server->views, link)
		if (view->role == role && view->mapped) return view;
	return NULL;
}

/* ---- shell role detection ------------------------------------------------ */

static const struct {
	const char *app_id;
	const char *exe;
	enum flwm_role role;
} shell_roles[] = {
	{"fldesktop.panel", "fl-panel", FLWM_ROLE_PANEL},
	{"fldesktop.desktop", "fl-desktop", FLWM_ROLE_DESKTOP},
	{"fldesktop.launcher", "fl-launcher", FLWM_ROLE_LAUNCHER},
	{"fldesktop.osd", "fl-panel", FLWM_ROLE_OSD},
	{"fldesktop.osk", "fl-osk", FLWM_ROLE_OSK},
	{"fldesktop.panel-popup", "fl-panel", FLWM_ROLE_POPUP},
	{"fldesktop.notification", "fl-notifyd", FLWM_ROLE_NOTIFY},
	{"fldesktop.lock", "fl-lock", FLWM_ROLE_LOCK},
	{"fldesktop.snip", "fl-snip", FLWM_ROLE_SNIP},
};

static bool client_exe_is(struct flwm_server *server, struct wl_client *client, const char *exe) {
	if (getenv("FLWM_TRUST_SHELL")) return true;
	pid_t pid;
	uid_t uid;
	gid_t gid;
	wl_client_get_credentials(client, &pid, &uid, &gid);
	char link[64], path[PATH_MAX];
	snprintf(link, sizeof link, "/proc/%d/exe", (int)pid);
	ssize_t n = readlink(link, path, sizeof path - 1);
	if (n <= 0) return false;
	path[n] = 0;
	char *slash = strrchr(path, '/');
	if (!slash || strcmp(slash + 1, exe) != 0) return false;
	*slash = 0;
	return !strcmp(path, server->exe_dir) || !strcmp(path, FLD_BINDIR);
}

static void detect_role(struct flwm_view *view) {
	/* FLTK does not set an app_id on borderless windows, so the shell clients
	 * may also identify themselves with their title; the executable check
	 * below keeps other programs from claiming a shell role. */
	const char *app_id = view->xdg_toplevel->app_id;
	const char *title = view->xdg_toplevel->title;
	if (view->role_checked || (!app_id && !title)) return;
	view->role_checked = true;
	for (size_t i = 0; i < sizeof shell_roles / sizeof shell_roles[0]; i++) {
		const char *want = shell_roles[i].app_id;
		if (!(app_id && !strcmp(app_id, want)) && !(title && !strcmp(title, want))) continue;
		struct wl_client *client = wl_resource_get_client(view->xdg_toplevel->resource);
		if (!client_exe_is(view->server, client, shell_roles[i].exe)) {
			wlr_log(WLR_INFO, "%s claimed by a foreign program; treating as normal", want);
			return;
		}
		/* the lock role only for the locker fl-wm started itself */
		if (shell_roles[i].role == FLWM_ROLE_LOCK && !lock_client_is_locker(view->server, client)) {
			wlr_log(WLR_INFO, "%s from a process fl-wm did not start; treating as normal", want);
			return;
		}
		view->role = shell_roles[i].role;
		struct wlr_scene_tree *layer = view->server->layer_views;
		switch (view->role) {
		case FLWM_ROLE_DESKTOP: layer = view->server->layer_background; break;
		case FLWM_ROLE_PANEL: layer = view->server->layer_top; break;
		case FLWM_ROLE_LAUNCHER:
		case FLWM_ROLE_OSD:
		case FLWM_ROLE_OSK:
		case FLWM_ROLE_NOTIFY:
		case FLWM_ROLE_POPUP: layer = view->server->layer_overlay; break;
		case FLWM_ROLE_LOCK: layer = view->server->layer_lock; break;
		case FLWM_ROLE_SNIP: layer = view->server->layer_popups; break;
		default: break;
		}
		wlr_scene_node_reparent(&view->tree->node, layer);
		wlr_log(WLR_DEBUG, "shell client %s gets role %d", want, view->role);
		return;
	}
}

/* ---- foreign toplevel (taskbar) ----------------------------------------- */

static void ftl_request_activate(struct wl_listener *l, void *data) {
	struct flwm_view *view = wl_container_of(l, view, ftl_request_activate);
	wlr_log(WLR_DEBUG, "foreign-toplevel activate request for %s", title_or(view, "?"));
	if (view->minimized) view_set_minimized(view, false);
	view_focus(view);
}

static void ftl_request_maximize(struct wl_listener *l, void *data) {
	struct flwm_view *view = wl_container_of(l, view, ftl_request_maximize);
	struct wlr_foreign_toplevel_handle_v1_maximized_event *ev = data;
	view_set_maximized(view, ev->maximized);
}

static void ftl_request_minimize(struct wl_listener *l, void *data) {
	struct flwm_view *view = wl_container_of(l, view, ftl_request_minimize);
	struct wlr_foreign_toplevel_handle_v1_minimized_event *ev = data;
	view_set_minimized(view, ev->minimized);
}

static void ftl_request_fullscreen(struct wl_listener *l, void *data) {
	struct flwm_view *view = wl_container_of(l, view, ftl_request_fullscreen);
	struct wlr_foreign_toplevel_handle_v1_fullscreen_event *ev = data;
	view_set_fullscreen(view, ev->fullscreen);
}

static void ftl_request_close(struct wl_listener *l, void *data) {
	struct flwm_view *view = wl_container_of(l, view, ftl_request_close);
	view_close(view);
}

static void ftl_destroy(struct wl_listener *l, void *data) {
	struct flwm_view *view = wl_container_of(l, view, ftl_destroy);
	wl_list_remove(&view->ftl_request_activate.link);
	wl_list_remove(&view->ftl_request_maximize.link);
	wl_list_remove(&view->ftl_request_minimize.link);
	wl_list_remove(&view->ftl_request_fullscreen.link);
	wl_list_remove(&view->ftl_request_close.link);
	wl_list_remove(&view->ftl_destroy.link);
	view->ftl = NULL;
}

static void ftl_create(struct flwm_view *view) {
	if (view->ftl || view->role != FLWM_ROLE_NORMAL || view->no_taskbar) return;
	view->ftl = wlr_foreign_toplevel_handle_v1_create(view->server->foreign_toplevel_mgr);
	if (!view->ftl) return;
	if (view_title(view))
		wlr_foreign_toplevel_handle_v1_set_title(view->ftl, view_title(view));
	if (view_app_id(view))
		wlr_foreign_toplevel_handle_v1_set_app_id(view->ftl, view_app_id(view));
	view->ftl_request_activate.notify = ftl_request_activate;
	wl_signal_add(&view->ftl->events.request_activate, &view->ftl_request_activate);
	view->ftl_request_maximize.notify = ftl_request_maximize;
	wl_signal_add(&view->ftl->events.request_maximize, &view->ftl_request_maximize);
	view->ftl_request_minimize.notify = ftl_request_minimize;
	wl_signal_add(&view->ftl->events.request_minimize, &view->ftl_request_minimize);
	view->ftl_request_fullscreen.notify = ftl_request_fullscreen;
	wl_signal_add(&view->ftl->events.request_fullscreen, &view->ftl_request_fullscreen);
	view->ftl_request_close.notify = ftl_request_close;
	wl_signal_add(&view->ftl->events.request_close, &view->ftl_request_close);
	view->ftl_destroy.notify = ftl_destroy;
	wl_signal_add(&view->ftl->events.destroy, &view->ftl_destroy);
	struct flwm_output *o = output_for_view(view);
	if (o) wlr_foreign_toplevel_handle_v1_output_enter(view->ftl, o->wlr_output);
}

static void ftl_update_output(struct flwm_view *view) {
	if (!view->ftl) return;
	struct flwm_output *cur = output_for_view(view), *o;
	wl_list_for_each(o, &view->server->outputs, link) {
		if (o == cur) wlr_foreign_toplevel_handle_v1_output_enter(view->ftl, o->wlr_output);
		else wlr_foreign_toplevel_handle_v1_output_leave(view->ftl, o->wlr_output);
	}
}

/* ---- focus & stacking ------------------------------------------------------ */

void view_raise(struct flwm_view *view) {
	wlr_scene_node_raise_to_top(&view->tree->node);
	if (is_x11(view)) xwayland_view_raise(view);
}

static bool same_client(struct flwm_view *a, struct flwm_view *b) {
	/* all X11 programs share Xwayland's connection; the launcher is never one */
	if (is_x11(a) || is_x11(b)) return false;
	return wl_resource_get_client(a->xdg_toplevel->resource) ==
		wl_resource_get_client(b->xdg_toplevel->resource);
}

void view_focus(struct flwm_view *view) {
	if (!view || !view->mapped) return;
	struct flwm_server *server = view->server;
	/* while locked only the locker gets the keyboard */
	if (server->locked && view->role != FLWM_ROLE_LOCK) return;
	if (server->focused_layer && view->role != FLWM_ROLE_LOCK) return; /* an exclusive layer surface owns the keyboard */
	struct wlr_seat *seat = server->seat;
	struct flwm_view *prev = view_focused(server);
	if (view->minimized) view_set_minimized(view, false);
	if (view->role == FLWM_ROLE_NORMAL || view->role == FLWM_ROLE_LAUNCHER ||
			view->role == FLWM_ROLE_POPUP || view->role == FLWM_ROLE_SNIP)
		view_raise(view);
	if (prev == view) return;
	/* X11 windows without keyboard input (ICCCM "no input") only come up */
	if (is_x11(view) && !xwayland_view_wants_focus(view)) return;

	/* the launcher closes itself when another window gets focus (not the panel,
	 * whose launcher button toggles it) */
	if (prev && prev->role == FLWM_ROLE_LAUNCHER && view->role != FLWM_ROLE_PANEL &&
			!same_client(prev, view)) /* its own dialogs (confirm shut down) keep it */
		view_close(prev);
	/* panel popups close as soon as anything else gets the focus */
	if (prev && prev->role == FLWM_ROLE_POPUP)
		view_close(prev);

	if (prev) {
		view_set_activated(prev, false);
		if (prev->ftl) wlr_foreign_toplevel_handle_v1_set_activated(prev->ftl, false);
	}
	if (!server->cycling) {
		/* while Alt+Tab cycling the focus order stays fixed until the modifier is released */
		wl_list_remove(&view->link);
		wl_list_insert(&server->views, &view->link);
	}
	view_set_activated(view, true);
	if (view->ftl) wlr_foreign_toplevel_handle_v1_set_activated(view->ftl, true);
	wlr_log(WLR_DEBUG, "focus -> %s (role %d)", title_or(view, "?"), view->role);
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	if (keyboard)
		wlr_seat_keyboard_notify_enter(seat, view_surface(view),
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	else
		wlr_seat_keyboard_notify_enter(seat, view_surface(view), NULL, 0, NULL);
	if (prev) decor_update(prev);
	decor_update(view);
}

void view_focus_next_available(struct flwm_server *server) {
	struct flwm_view *v;
	wl_list_for_each(v, &server->views, link) {
		if (v->mapped && !v->minimized && v->role == FLWM_ROLE_NORMAL && !v->no_taskbar) {
			view_focus(v);
			return;
		}
	}
	/* nothing left: give the desktop the keyboard, or clear focus */
	struct flwm_view *desk = view_find_role(server, FLWM_ROLE_DESKTOP);
	if (desk) view_focus(desk);
	else wlr_seat_keyboard_notify_clear_focus(server->seat);
}

/* ---- window states ------------------------------------------------------- */

static void usable_area_for(struct flwm_view *view, struct wlr_box *ua) {
	struct flwm_output *o = output_for_view(view);
	if (o) *ua = o->usable_area;
	else *ua = (struct wlr_box){0, 0, 1280, 720};
}

static void apply_box(struct flwm_view *view, struct wlr_box *box) {
	/* box is the outer frame (decorations included) */
	int T = deco_top(view), B = deco_side(view);
	int w = box->width - 2 * B, h = box->height - T - B;
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	view_configure_size(view, w, h);
	view_set_position(view, box->x + B, box->y + T);
}

static void save_geometry(struct flwm_view *view) {
	struct wlr_box geo;
	view_get_geometry(view, &geo);
	view->saved = geo;
}

static void restore_geometry(struct flwm_view *view) {
	if (view->saved.width <= 0) return;
	view_configure_size(view, view->saved.width, view->saved.height);
	view_set_position(view, view->saved.x, view->saved.y);
}

void view_set_maximized(struct flwm_view *view, bool maximized) {
	if (view->role != FLWM_ROLE_NORMAL || !view_initialized(view)) return;
	if (view->fullscreen) view_set_fullscreen(view, false);
	if (view->maximized == maximized) {
		view_reconfigure(view);
		return;
	}
	if (maximized) {
		save_geometry(view);
		view->maximized = true;
		struct wlr_box ua;
		usable_area_for(view, &ua);
		apply_box(view, &ua);
	} else {
		view->maximized = false;
		restore_geometry(view);
	}
	view_send_maximized(view, maximized);
	if (view->ftl) wlr_foreign_toplevel_handle_v1_set_maximized(view->ftl, maximized);
	view->decor.width = -1; /* force re-render (rounded corners, handles) */
	decor_update(view);
}

void view_snap(struct flwm_view *view, int side) {
	if (view->role != FLWM_ROLE_NORMAL) return;
	if (view->maximized) view_set_maximized(view, false);
	if (view->fullscreen) view_set_fullscreen(view, false);
	struct wlr_box ua;
	usable_area_for(view, &ua);
	if (view->saved.width <= 0) save_geometry(view);
	struct wlr_box box = ua;
	box.width = ua.width / 2;
	if (side > 0) box.x = ua.x + ua.width - box.width;
	apply_box(view, &box);
}

void view_set_minimized(struct flwm_view *view, bool minimized) {
	if (view->role != FLWM_ROLE_NORMAL || view->minimized == minimized) return;
	view->minimized = minimized;
	wlr_scene_node_set_enabled(&view->tree->node, !minimized);
	if (is_x11(view)) xwayland_view_set_minimized(view, minimized);
	if (view->ftl) wlr_foreign_toplevel_handle_v1_set_minimized(view->ftl, minimized);
	if (minimized) {
		if (view_focused(view->server) == view) {
			view_set_activated(view, false);
			if (view->ftl) wlr_foreign_toplevel_handle_v1_set_activated(view->ftl, false);
			wlr_seat_keyboard_notify_clear_focus(view->server->seat);
			/* move to the end of the focus list so it is not picked again */
			wl_list_remove(&view->link);
			wl_list_insert(view->server->views.prev, &view->link);
			view_focus_next_available(view->server);
		}
	} else {
		view->hidden_by_show_desktop = false;
	}
}

void view_set_fullscreen(struct flwm_view *view, bool fullscreen) {
	if (view->role != FLWM_ROLE_NORMAL || !view_initialized(view)) return;
	if (view->fullscreen == fullscreen) {
		view_reconfigure(view);
		return;
	}
	struct flwm_server *server = view->server;
	if (fullscreen) {
		if (!view->maximized) save_geometry(view);
		struct flwm_output *o = output_for_view(view);
		view->fullscreen = true;
		wlr_scene_node_reparent(&view->tree->node, server->layer_top);
		if (o) {
			view_configure_size(view, o->full_area.width, o->full_area.height);
			view_set_position(view, o->full_area.x, o->full_area.y);
		}
	} else {
		view->fullscreen = false;
		wlr_scene_node_reparent(&view->tree->node, server->layer_views);
		if (view->maximized) {
			struct wlr_box ua;
			usable_area_for(view, &ua);
			apply_box(view, &ua);
		} else {
			restore_geometry(view);
		}
	}
	view_send_fullscreen(view, fullscreen);
	if (view->ftl) wlr_foreign_toplevel_handle_v1_set_fullscreen(view->ftl, fullscreen);
	decor_update(view);
}

void view_close(struct flwm_view *view) {
	if (is_x11(view)) xwayland_view_close(view);
	else wlr_xdg_toplevel_send_close(view->xdg_toplevel);
}

void views_reconfigure_maximized(struct flwm_server *server) {
	struct flwm_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (!view->mapped || view->role != FLWM_ROLE_NORMAL) continue;
		if (view->fullscreen) {
			struct flwm_output *o = output_for_view(view);
			if (!o) continue;
			view_configure_size(view, o->full_area.width, o->full_area.height);
			view_set_position(view, o->full_area.x, o->full_area.y);
		} else if (view->maximized) {
			struct wlr_box ua;
			usable_area_for(view, &ua);
			apply_box(view, &ua);
		}
	}
}

void views_show_desktop(struct flwm_server *server) {
	bool any_visible = false;
	struct flwm_view *view;
	wl_list_for_each(view, &server->views, link)
		if (view->mapped && view->role == FLWM_ROLE_NORMAL && !view->minimized) any_visible = true;
	if (any_visible) {
		wl_list_for_each(view, &server->views, link) {
			if (view->mapped && view->role == FLWM_ROLE_NORMAL && !view->minimized) {
				view_set_minimized(view, true);
				view->hidden_by_show_desktop = true;
			}
		}
	} else {
		struct flwm_view *tmp;
		wl_list_for_each_safe(view, tmp, &server->views, link)
			if (view->hidden_by_show_desktop) view_set_minimized(view, false);
		view_focus_next_available(server);
	}
}

/* ---- shell role placement ------------------------------------------------ */

static void request_size(struct flwm_view *view, int w, int h) {
	if (view->requested_w == w && view->requested_h == h) return;
	view->requested_w = w;
	view->requested_h = h;
	if (view->xdg_toplevel->base->initialized) wlr_xdg_toplevel_set_size(view->xdg_toplevel, w, h);
}

void view_reposition_shell(struct flwm_view *view) {
	struct flwm_server *server = view->server;
	struct wlr_box geo;
	flwm_xdg_geometry(view->xdg_toplevel->base, &geo);
	struct flwm_output *o;
	switch (view->role) {
	case FLWM_ROLE_PANEL:
		o = output_primary(server);
		if (!o) return;
		/* never send a 0 height: FLTK treats it as a zero sized window and
		 * stops drawing. Before the first buffer let the client choose. */
		if (geo.height > 0) request_size(view, o->full_area.width, geo.height);
		else request_size(view, 0, 0);
		view_set_position(view, o->full_area.x,
			server->config.panel_top ? o->full_area.y
				: o->full_area.y + o->full_area.height - geo.height);
		break;
	case FLWM_ROLE_DESKTOP:
		o = output_primary(server);
		if (!o) return;
		request_size(view, o->full_area.width, o->full_area.height);
		view_set_position(view, o->full_area.x, o->full_area.y);
		break;
	case FLWM_ROLE_LAUNCHER: {
		o = output_at(server, server->cursor->x, server->cursor->y);
		if (!o) return;
		struct wlr_box ua = o->usable_area;
		int y = server->config.panel_top ? ua.y + 4 : ua.y + ua.height - geo.height - 4;
		view_set_position(view, ua.x + 4, y);
		break;
	}
	case FLWM_ROLE_OSD:
		o = output_primary(server);
		if (!o) return;
		view_set_position(view, o->usable_area.x + (o->usable_area.width - geo.width) / 2,
			o->usable_area.y + o->usable_area.height - geo.height - 48);
		break;
	case FLWM_ROLE_POPUP: {
		/* next to the panel, horizontally centred on the pointer */
		o = output_at(server, server->cursor->x, server->cursor->y);
		if (!o) return;
		struct wlr_box ua = o->usable_area;
		int x = (int)server->cursor->x - geo.width / 2;
		if (x + geo.width > ua.x + ua.width - 4) x = ua.x + ua.width - 4 - geo.width;
		if (x < ua.x + 4) x = ua.x + 4;
		int y = server->config.panel_top ? ua.y + 4 : ua.y + ua.height - geo.height - 4;
		if (!view->placed) {
			view_set_position(view, x, y);
			view->placed = true;
		} else {
			view_set_position(view, view->x, y);
		}
		break;
	}
	case FLWM_ROLE_LOCK:
		/* the whole primary output (the others show the cover) */
		o = output_primary(server);
		if (!o) return;
		request_size(view, o->full_area.width, o->full_area.height);
		view_set_position(view, o->full_area.x, o->full_area.y);
		break;
	case FLWM_ROLE_SNIP: {
		/* the whole output layout, over the panel */
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, NULL, &box);
		if (box.width <= 0 || box.height <= 0) return;
		request_size(view, box.width, box.height);
		view_set_position(view, box.x, box.y);
		break;
	}
	case FLWM_ROLE_NOTIFY:
		/* top right corner of the usable area (below a top panel) */
		o = output_primary(server);
		if (!o) return;
		view_set_position(view, o->usable_area.x + o->usable_area.width - geo.width - 8,
			o->usable_area.y + 8);
		break;
	case FLWM_ROLE_OSK: {
		/* docked at the bottom, above a bottom panel */
		o = output_primary(server);
		if (!o) return;
		int panel_h = 0;
		struct flwm_view *panel = view_find_role(server, FLWM_ROLE_PANEL);
		if (panel && !server->config.panel_top) {
			struct wlr_box pg;
			flwm_xdg_geometry(panel->xdg_toplevel->base, &pg);
			panel_h = pg.height;
		}
		view_set_position(view, o->full_area.x + (o->full_area.width - geo.width) / 2,
			o->full_area.y + o->full_area.height - panel_h - geo.height);
		break;
	}
	default:
		break;
	}
}

/* ---- initial placement of normal windows --------------------------------- */

static struct flwm_view *view_from_toplevel(struct flwm_server *server, struct wlr_xdg_toplevel *t) {
	struct flwm_view *v;
	wl_list_for_each(v, &server->views, link)
		if (v->xdg_toplevel == t) return v;
	return NULL;
}

static struct flwm_view *view_parent(struct flwm_view *view) {
	if (is_x11(view)) return xwayland_view_parent(view);
	return view->xdg_toplevel->parent ? view_from_toplevel(view->server, view->xdg_toplevel->parent) : NULL;
}

static void place_new_view(struct flwm_view *view) {
	struct flwm_server *server = view->server;
	struct wlr_box geo;
	content_geometry(view, &geo);
	int T = deco_top(view), B = deco_side(view);
	struct flwm_view *parent = view_parent(view);
	int x, y;
	/* X11 programs that place their own windows (with a position hint) */
	if (is_x11(view) && xwayland_view_position_hint(view, &x, &y)) {
		struct flwm_output *ho = output_at(server, x, y);
		if (ho) {
			struct wlr_box ua = ho->usable_area;
			if (y < ua.y + T) y = ua.y + T;
			if (x + geo.width > ua.x + ua.width) x = ua.x + ua.width - geo.width - B;
			if (x < ua.x + B) x = ua.x + B;
			view_set_position(view, x, y);
			view->placed = true;
			return;
		}
	}
	struct flwm_output *o = parent ? output_for_view(parent)
		: output_at(server, server->cursor->x, server->cursor->y);
	struct wlr_box ua = o ? o->usable_area : (struct wlr_box){0, 0, 1280, 720};
	if (parent) {
		/* dialogs: centered over the parent */
		struct wlr_box pg;
		view_get_geometry(parent, &pg);
		x = pg.x + (pg.width - geo.width) / 2;
		y = pg.y + (pg.height - geo.height) / 2;
	} else {
		x = ua.x + (ua.width - geo.width) / 2;
		y = ua.y + T + (ua.height - T - B - geo.height) / 2;
		/* cascade if another window sits exactly there */
		for (int tries = 0; tries < 20; tries++) {
			bool clash = false;
			struct flwm_view *v;
			wl_list_for_each(v, &server->views, link) {
				if (v != view && v->mapped && v->role == FLWM_ROLE_NORMAL &&
						abs(v->x - x) < 8 && abs(v->y - y) < 8)
					clash = true;
			}
			if (!clash) break;
			x += 32;
			y += 32;
		}
	}
	/* keep the title bar reachable */
	if (x + geo.width > ua.x + ua.width) x = ua.x + ua.width - geo.width - B;
	if (y + geo.height > ua.y + ua.height) y = ua.y + ua.height - geo.height - B;
	if (x < ua.x + B) x = ua.x + B;
	if (y < ua.y + T) y = ua.y + T;
	view_set_position(view, x, y);
	view->placed = true;
}

/* ---- xdg toplevel events ------------------------------------------------- */

void view_handle_map(struct flwm_view *view, bool maximized, bool fullscreen, bool focus) {
	struct flwm_server *server = view->server;
	if (!is_x11(view)) detect_role(view);
	view->mapped = true;
	wlr_log(WLR_DEBUG, "map: app_id=%s title=%s role=%d%s",
		view_app_id(view) ? view_app_id(view) : "(null)", title_or(view, "(null)"), view->role,
		is_x11(view) ? " (X11)" : "");
	wlr_scene_node_set_enabled(&view->tree->node, true);
	if (view->role != FLWM_ROLE_NORMAL) {
		/* FLTK's borderless windows only redraw after a second configure
		 * (Weston sends two). Shell windows that never get the focus (the
		 * on-screen keyboard, notifications) would otherwise get just the
		 * initial one and never show anything but their first frame. */
		wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
		view_reposition_shell(view);
		if (view->role == FLWM_ROLE_PANEL || view->role == FLWM_ROLE_OSK) arrange_all(server);
		if (view->role == FLWM_ROLE_LAUNCHER || view->role == FLWM_ROLE_POPUP || view->role == FLWM_ROLE_SNIP)
			view_focus(view);
		if (view->role == FLWM_ROLE_DESKTOP && !view_focused(server)) view_focus(view);
		if (view->role == FLWM_ROLE_LOCK) {
			view_focus(view);
			lock_view_mapped(server, view);
		}
		return;
	}
	if (!view->placed) place_new_view(view);
	ftl_create(view);
	decor_update(view);
	if (maximized) view_set_maximized(view, true);
	if (fullscreen) view_set_fullscreen(view, true);
	if (focus) view_focus(view);
	else view_raise(view);
}

static void view_map(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, map);
	view_handle_map(view, view->xdg_toplevel->requested.maximized,
		view->xdg_toplevel->requested.fullscreen, true);
}

void view_handle_unmap(struct flwm_view *view) {
	struct flwm_server *server = view->server;
	wlr_log(WLR_DEBUG, "unmap: %s", title_or(view, "?"));
	if (view == server->grabbed_view) input_reset_cursor_mode(server);
	if (server->cycle_current == view) server->cycle_current = NULL;
	switcher_view_gone(server, view);
	if (server->hover_view == view) server->hover_view = NULL;
	if (server->last_click_view == view) server->last_click_view = NULL;
	bool was_focused = view_focused(server) == view;
	view->mapped = false;
	/* the decorations are ours, not the surface's: hide them with it */
	wlr_scene_node_set_enabled(&view->tree->node, false);
	if (view->ftl) {
		wlr_foreign_toplevel_handle_v1_destroy(view->ftl);
		view->ftl = NULL;
	}
	if (view->role == FLWM_ROLE_PANEL || view->role == FLWM_ROLE_OSK) arrange_all(server);
	if (was_focused) {
		wlr_seat_keyboard_notify_clear_focus(server->seat);
		wl_list_remove(&view->link);
		wl_list_insert(server->views.prev, &view->link);
		view_focus_next_available(server);
	}
}

static void view_unmap(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, unmap);
	view_handle_unmap(view);
}

static void view_commit(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, commit);
	struct wlr_xdg_surface *base = view->xdg_toplevel->base;
	if (base->initial_commit) {
		detect_role(view);
		if (view->decoration) {
			wlr_xdg_toplevel_decoration_v1_set_mode(view->decoration,
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
			view->ssd = true;
		}
		switch (view->role) {
		case FLWM_ROLE_PANEL:
		case FLWM_ROLE_DESKTOP:
			view->requested_w = view->requested_h = -1;
			view_reposition_shell(view);
			break;
		default: {
			wlr_xdg_toplevel_set_size(view->xdg_toplevel, 0, 0);
			struct flwm_output *o = output_at(view->server, view->server->cursor->x,
				view->server->cursor->y);
			if (o && wl_resource_get_version(view->xdg_toplevel->resource) >=
					XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION)
				wlr_xdg_toplevel_set_bounds(view->xdg_toplevel,
					o->usable_area.width - 2 * decor_border(view->server),
					o->usable_area.height - deco_top(view) - decor_border(view->server));
			break;
		}
		}
		return;
	}
	view_handle_commit(view);
}

void view_handle_commit(struct flwm_view *view) {
	update_surface_offset(view);
	if (!view->mapped) return;
	if (view->role != FLWM_ROLE_NORMAL) {
		struct wlr_box geo;
		content_geometry(view, &geo);
		if (view->role == FLWM_ROLE_PANEL || view->role == FLWM_ROLE_OSK) {
			/* size may have changed: re-arrange usable areas */
			if (geo.width != view->committed_w || geo.height != view->committed_h) {
				view->committed_w = geo.width;
				view->committed_h = geo.height;
				arrange_all(view->server);
			} else {
				view_reposition_shell(view);
			}
		} else {
			view_reposition_shell(view);
		}
		return;
	}
	/* while resizing from the top/left edge keep the opposite edge anchored */
	/* (X11 windows are configured with their new position right away) */
	struct flwm_server *server = view->server;
	if (server->cursor_mode == FLWM_CURSOR_RESIZE && server->grabbed_view == view && !is_x11(view)) {
		struct wlr_box geo;
		content_geometry(view, &geo);
		int x = view->x, y = view->y;
		if (server->resize_edges & WLR_EDGE_LEFT)
			x = server->grab_geobox.x + server->grab_geobox.width - geo.width;
		if (server->resize_edges & WLR_EDGE_TOP)
			y = server->grab_geobox.y + server->grab_geobox.height - geo.height;
		if (x != view->x || y != view->y) view_set_position(view, x, y);
	}
	decor_update(view);
	ftl_update_output(view);
}

static void view_destroy(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, destroy);
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->commit.link);
	if (view->decoration) {
		wl_list_remove(&view->decoration_request_mode.link);
		wl_list_remove(&view->decoration_destroy.link);
	}
	view_free(view);
}

void view_free(struct flwm_view *view) {
	struct flwm_server *server = view->server;
	/* a client that dies (or crashes) may not unmap first */
	if (view == server->grabbed_view) {
		server->grabbed_view = NULL; /* its toplevel is going: no configure */
		input_reset_cursor_mode(server);
	}
	if (server->cycle_current == view) server->cycle_current = NULL;
	switcher_view_gone(server, view);
	if (server->hover_view == view) server->hover_view = NULL;
	if (server->last_click_view == view) server->last_click_view = NULL;
	if (view->ftl) wlr_foreign_toplevel_handle_v1_destroy(view->ftl);
	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->request_move.link);
	wl_list_remove(&view->request_resize.link);
	wl_list_remove(&view->request_maximize.link);
	wl_list_remove(&view->request_minimize.link);
	wl_list_remove(&view->request_fullscreen.link);
	wl_list_remove(&view->set_title.link);
	wl_list_remove(&view->set_app_id.link);
	wl_list_remove(&view->link);
	/* wlroots destroys the surface's scene nodes, but view->tree and the
	 * decorations in it are ours; their node data points at this view */
	wlr_scene_node_destroy(&view->tree->node);
	view->decor.tree = NULL;
	free(view->decor.title_text);
	free(view);
}

static void view_request_move(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, request_move);
	if (view->role == FLWM_ROLE_NORMAL) view_begin_interactive(view, FLWM_CURSOR_MOVE, 0);
}

static void view_request_resize(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct flwm_view *view = wl_container_of(listener, view, request_resize);
	if (view->role == FLWM_ROLE_NORMAL)
		view_begin_interactive(view, FLWM_CURSOR_RESIZE, event->edges);
}

static void view_request_maximize(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, request_maximize);
	if (!view->mapped) {
		if (view->xdg_toplevel->base->initialized)
			wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
		return; /* handled on map */
	}
	view_set_maximized(view, view->xdg_toplevel->requested.maximized);
}

static void view_request_minimize(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, request_minimize);
	if (view->mapped) view_set_minimized(view, true);
}

static void view_request_fullscreen(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, request_fullscreen);
	if (!view->mapped) {
		if (view->xdg_toplevel->base->initialized)
			wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
		return;
	}
	view_set_fullscreen(view, view->xdg_toplevel->requested.fullscreen);
}

static void view_set_title(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, set_title);
	if (view->ftl && view_title(view))
		wlr_foreign_toplevel_handle_v1_set_title(view->ftl, view_title(view));
	if (view->mapped) decor_update(view);
}

static void view_set_app_id(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, set_app_id);
	if (view->ftl && view_app_id(view))
		wlr_foreign_toplevel_handle_v1_set_app_id(view->ftl, view_app_id(view));
}

struct flwm_view *view_new(struct flwm_server *server, enum flwm_view_type type) {
	struct flwm_view *view = calloc(1, sizeof(*view));
	view->server = server;
	view->type = type;
	view->tree = wlr_scene_tree_create(server->layer_views);
	view->desc.kind = FLWM_NODE_VIEW;
	view->desc.ptr = view;
	view->tree->node.data = &view->desc;
	wlr_scene_node_set_enabled(&view->tree->node, false);
	wl_list_insert(server->views.prev, &view->link);
	return view;
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct flwm_view *view = view_new(server, FLWM_VIEW_XDG);
	view->xdg_toplevel = xdg_toplevel;
	view->surface_tree = wlr_scene_xdg_surface_create(view->tree, xdg_toplevel->base);
	xdg_toplevel->base->data = view->surface_tree;

	view->map.notify = view_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &view->map);
	view->unmap.notify = view_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &view->unmap);
	view->commit.notify = view_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &view->commit);
	view->destroy.notify = view_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &view->destroy);
	view->request_move.notify = view_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &view->request_move);
	view->request_resize.notify = view_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &view->request_resize);
	view->request_maximize.notify = view_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &view->request_maximize);
	view->request_minimize.notify = view_request_minimize;
	wl_signal_add(&xdg_toplevel->events.request_minimize, &view->request_minimize);
	view->request_fullscreen.notify = view_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &view->request_fullscreen);
	view->set_title.notify = view_set_title;
	wl_signal_add(&xdg_toplevel->events.set_title, &view->set_title);
	view->set_app_id.notify = view_set_app_id;
	wl_signal_add(&xdg_toplevel->events.set_app_id, &view->set_app_id);
}

/* ---- popups ------------------------------------------------------------- */

static void popup_unconstrain(struct flwm_popup *popup) {
	struct wlr_xdg_popup *xdg_popup = popup->xdg_popup;
	/* find the toplevel at the root of the popup chain */
	struct wlr_surface *parent = xdg_popup->parent;
	struct wlr_xdg_toplevel *top = NULL;
	while (parent) {
		struct wlr_xdg_popup *pp = wlr_xdg_popup_try_from_wlr_surface(parent);
		if (!pp) {
			top = wlr_xdg_toplevel_try_from_wlr_surface(parent);
			break;
		}
		parent = pp->parent;
	}
	if (!top || !top->base->data) return;
	struct wlr_scene_tree *tree = top->base->data; /* the toplevel's surface tree */
	struct wlr_scene_node *n = &tree->node;
	while (n && !(n->data && ((struct flwm_node_desc *)n->data)->kind == FLWM_NODE_VIEW))
		n = n->parent ? &n->parent->node : NULL;
	if (!n) return;
	struct flwm_view *view = ((struct flwm_node_desc *)n->data)->ptr;
	int lx, ly; /* toplevel surface origin in layout coordinates */
	wlr_scene_node_coords(&tree->node, &lx, &ly);
	struct wlr_box geo;
	view_get_geometry(view, &geo);
	struct flwm_output *o = output_at(view->server, geo.x + geo.width / 2.0, geo.y + geo.height / 2.0);
	if (!o) return;
	struct wlr_box box = o->full_area;
	box.x -= lx;
	box.y -= ly;
	wlr_xdg_popup_unconstrain_from_box(xdg_popup, &box);
}

static void popup_commit(struct wl_listener *listener, void *data) {
	struct flwm_popup *popup = wl_container_of(listener, popup, commit);
	if (popup->xdg_popup->base->initial_commit) {
		popup_unconstrain(popup);
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

static void popup_destroy(struct wl_listener *listener, void *data) {
	struct flwm_popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

void view_popup_create(struct wlr_xdg_popup *xdg_popup, struct wlr_scene_tree *parent_tree) {
	struct flwm_popup *popup = calloc(1, sizeof(*popup));
	popup->xdg_popup = xdg_popup;
	xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

	popup->commit.notify = popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
	popup->destroy.notify = popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
	struct wlr_xdg_popup *xdg_popup = data;
	/* popups of layer surfaces get their parent later; layer.c handles them */
	if (!xdg_popup->parent) return;
	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	if (!parent || !parent->data) return;
	view_popup_create(xdg_popup, parent->data);
}

/* ---- xdg-decoration ------------------------------------------------------ */

static void decoration_request_mode(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, decoration_request_mode);
	if (view->xdg_toplevel->base->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(view->decoration,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	view->ssd = true;
}

static void decoration_destroy(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, decoration_destroy);
	wl_list_remove(&view->decoration_request_mode.link);
	wl_list_remove(&view->decoration_destroy.link);
	view->decoration = NULL;
	view->ssd = false;
	if (view->mapped) decor_update(view);
}

static void server_new_toplevel_decoration(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, new_toplevel_decoration);
	struct wlr_xdg_toplevel_decoration_v1 *deco = data;
	struct flwm_view *view = view_from_toplevel(server, deco->toplevel);
	if (!view) return;
	view->decoration = deco;
	view->decoration_request_mode.notify = decoration_request_mode;
	wl_signal_add(&deco->events.request_mode, &view->decoration_request_mode);
	view->decoration_destroy.notify = decoration_destroy;
	wl_signal_add(&deco->events.destroy, &view->decoration_destroy);
	view->ssd = true;
	if (deco->toplevel->base->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(deco, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

/* ---- xdg-activation ------------------------------------------------------ */

static void request_activate(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, request_activate);
	struct wlr_xdg_activation_v1_request_activate_event *ev = data;
	struct wlr_xdg_toplevel *t = wlr_xdg_toplevel_try_from_wlr_surface(ev->surface);
	if (!t) return;
	struct flwm_view *view = view_from_toplevel(server, t);
	wlr_log(WLR_DEBUG, "xdg-activation request for %s", view ? title_or(view, "?") : "?");
	if (view && view->mapped) view_focus(view);
}

void view_init(struct flwm_server *server) {
	wl_list_init(&server->views);
	server->xdg_shell = wlr_xdg_shell_create(server->wl_display, 6);
	server->new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);
	server->new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_xdg_popup);

	server->xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(server->wl_display);
	server->new_toplevel_decoration.notify = server_new_toplevel_decoration;
	wl_signal_add(&server->xdg_decoration_mgr->events.new_toplevel_decoration,
		&server->new_toplevel_decoration);

	server->foreign_toplevel_mgr = wlr_foreign_toplevel_manager_v1_create(server->wl_display);

	server->xdg_activation = wlr_xdg_activation_v1_create(server->wl_display);
	server->request_activate.notify = request_activate;
	wl_signal_add(&server->xdg_activation->events.request_activate, &server->request_activate);
}

/* ---- interactive move / resize ------------------------------------------- */

void view_begin_interactive(struct flwm_view *view, enum flwm_cursor_mode mode, uint32_t edges) {
	struct flwm_server *server = view->server;
	if (view->fullscreen) return;
	if (mode == FLWM_CURSOR_RESIZE && view->maximized) return;
	if (mode == FLWM_CURSOR_MOVE && view->maximized) {
		/* un-maximize, keeping the pointer at the same relative x in the title */
		struct wlr_box cur;
		view_get_geometry(view, &cur);
		double rel = cur.width > 0 ? (server->cursor->x - cur.x) / cur.width : 0.5;
		view_set_maximized(view, false);
		int w = view->saved.width > 0 ? view->saved.width : cur.width;
		int x = (int)(server->cursor->x - rel * w);
		int y = (int)server->cursor->y + deco_top(view) / 2;
		view_set_position(view, x, y);
	}
	server->grabbed_view = view;
	server->cursor_mode = mode;
	if (mode == FLWM_CURSOR_MOVE) {
		server->grab_x = server->cursor->x - view->x;
		server->grab_y = server->cursor->y - view->y;
	} else {
		struct wlr_box geo;
		view_get_geometry(view, &geo);
		double border_x = geo.x + ((edges & WLR_EDGE_RIGHT) ? geo.width : 0);
		double border_y = geo.y + ((edges & WLR_EDGE_BOTTOM) ? geo.height : 0);
		server->grab_x = server->cursor->x - border_x;
		server->grab_y = server->cursor->y - border_y;
		server->grab_geobox = geo;
		server->resize_edges = edges;
		if (!is_x11(view)) wlr_xdg_toplevel_set_resizing(view->xdg_toplevel, true);
	}
}

void view_dump_windows(struct flwm_server *server) {
	const char *dir = getenv("XDG_RUNTIME_DIR");
	if (!dir) return;
	char path[PATH_MAX], tmp[PATH_MAX + 8];
	snprintf(path, sizeof path, "%s/fl-wm-windows", dir);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) return;
	struct flwm_view *v;
	/* server->views is in stacking order, topmost first */
	wl_list_for_each(v, &server->views, link) {
		if (!v->mapped || v->minimized || v->role != FLWM_ROLE_NORMAL || v->no_taskbar) continue;
		struct wlr_box geo;
		content_geometry(v, &geo);
		int T = deco_top(v), B = deco_side(v);
		const char *title = title_or(v, "");
		fprintf(f, "%d %d %d %d ", v->x - B, v->y - T, geo.width + 2 * B, geo.height + T + B);
		for (const char *c = title; *c; c++) fputc(*c == '\n' ? ' ' : *c, f);
		fputc('\n', f);
	}
	fclose(f);
	rename(tmp, path);
}
