/* fl-wm: X11 programs through Xwayland
 *
 * Normal X11 windows become views like xdg toplevels (decorations, the
 * panel's window list, Alt+Tab, snapping). Override-redirect windows (menus,
 * tooltips, drag icons) place themselves and are only shown, above the
 * windows; the ones that want the keyboard get it while they are mapped.
 * Xwayland starts on the first X11 client (lazy), so a session that never
 * runs one does not pay for it. */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/log.h>
#include <wlr/xcursor.h>
#include <wlr/xwayland.h>
#include <wlr/xwayland/server.h>
#include <wlr/xwayland/shell.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xproto.h>

#include "server.h"

/* ---- wlroots 0.18 / 0.19 / 0.20 ----------------------------------------- */

static void xw_set_maximized(struct wlr_xwayland_surface *xs, bool maximized) {
#if FLWM_WLR_AT_LEAST(0, 19)
	wlr_xwayland_surface_set_maximized(xs, maximized, maximized);
#else
	wlr_xwayland_surface_set_maximized(xs, maximized);
#endif
}

static bool xw_or_wants_focus(struct wlr_xwayland_surface *xs) {
#if FLWM_WLR_AT_LEAST(0, 19)
	return wlr_xwayland_surface_override_redirect_wants_focus(xs);
#else
	return wlr_xwayland_or_surface_wants_focus(xs);
#endif
}

static bool xw_takes_input(struct wlr_xwayland_surface *xs) {
#if FLWM_WLR_AT_LEAST(0, 19)
	return wlr_xwayland_surface_icccm_input_model(xs) != WLR_ICCCM_INPUT_MODEL_NONE;
#else
	return wlr_xwayland_icccm_input_model(xs) != WLR_ICCCM_INPUT_MODEL_NONE;
#endif
}

/* menus, tooltips and the like that are not override-redirect: they place
 * themselves and get no decorations, focus or taskbar entry (wlroots 0.18
 * has no window types, there they are treated like normal windows) */
static bool xw_is_popup(struct wlr_xwayland_surface *xs) {
#if FLWM_WLR_AT_LEAST(0, 19)
	static const enum wlr_xwayland_net_wm_window_type popup_types[] = {
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_MENU,
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DROPDOWN_MENU,
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_POPUP_MENU,
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLTIP,
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_NOTIFICATION,
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_COMBO,
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DND,
	};
	for (size_t i = 0; i < sizeof popup_types / sizeof popup_types[0]; i++)
		if (wlr_xwayland_surface_has_window_type(xs, popup_types[i])) return true;
#else
	(void)xs;
#endif
	return false;
}

static bool xw_is_splash(struct wlr_xwayland_surface *xs) {
#if FLWM_WLR_AT_LEAST(0, 19)
	return wlr_xwayland_surface_has_window_type(xs, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH);
#else
	(void)xs;
	return false;
#endif
}

static void xw_configure(struct wlr_xwayland_surface *xs, int x, int y, int w, int h) {
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	if (w > 32767) w = 32767;
	if (h > 32767) h = 32767;
	if (x < -32768) x = -32768;
	if (y < -32768) y = -32768;
	if (x > 32767) x = 32767;
	if (y > 32767) y = 32767;
	wlr_xwayland_surface_configure(xs, (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h);
}

/* ---- what view.c asks of an X11 view -------------------------------------- */

struct wlr_surface *xwayland_view_surface(struct flwm_view *view) {
	return view->xsurface->surface;
}

const char *xwayland_view_title(struct flwm_view *view) {
	return view->xsurface->title;
}

const char *xwayland_view_app_id(struct flwm_view *view) {
	/* WM_CLASS: the class usually matches the .desktop file's StartupWMClass */
	return view->xsurface->class;
}

void xwayland_view_size(struct flwm_view *view, int *w, int *h) {
	struct wlr_xwayland_surface *xs = view->xsurface;
	/* what the client drew; it may not take the size we asked for (size
	 * increments, minimum sizes) */
	if (xs->surface && xs->surface->mapped && xs->surface->current.width > 0) {
		*w = xs->surface->current.width;
		*h = xs->surface->current.height;
	} else {
		*w = xs->width;
		*h = xs->height;
	}
}

void xwayland_view_configure(struct flwm_view *view, int x, int y, int w, int h) {
	xw_configure(view->xsurface, x, y, w, h);
}

void xwayland_view_move(struct flwm_view *view, int x, int y) {
	struct wlr_xwayland_surface *xs = view->xsurface;
	xw_configure(xs, x, y, xs->width, xs->height);
}

void xwayland_view_activate(struct flwm_view *view, bool activated) {
	wlr_xwayland_surface_activate(view->xsurface, activated);
}

void xwayland_view_raise(struct flwm_view *view) {
	wlr_xwayland_surface_restack(view->xsurface, NULL, XCB_STACK_MODE_ABOVE);
}

void xwayland_view_close(struct flwm_view *view) {
	wlr_xwayland_surface_close(view->xsurface);
}

void xwayland_view_set_maximized(struct flwm_view *view, bool maximized) {
	xw_set_maximized(view->xsurface, maximized);
}

void xwayland_view_set_fullscreen(struct flwm_view *view, bool fullscreen) {
	wlr_xwayland_surface_set_fullscreen(view->xsurface, fullscreen);
}

void xwayland_view_set_minimized(struct flwm_view *view, bool minimized) {
	wlr_xwayland_surface_set_minimized(view->xsurface, minimized);
}

struct flwm_view *xwayland_view_parent(struct flwm_view *view) {
	struct wlr_xwayland_surface *parent = view->xsurface->parent;
	if (!parent) return NULL;
	struct flwm_view *v;
	wl_list_for_each(v, &view->server->views, link)
		if (v->type == FLWM_VIEW_XWAYLAND && v->xsurface == parent && v->mapped) return v;
	return NULL;
}

bool xwayland_view_wants_focus(struct flwm_view *view) {
	return xw_takes_input(view->xsurface) && !xw_is_popup(view->xsurface);
}

bool xwayland_view_position_hint(struct flwm_view *view, int *x, int *y) {
	struct wlr_xwayland_surface *xs = view->xsurface;
	*x = xs->x;
	*y = xs->y;
	if (xw_is_popup(xs)) return true;
	/* splash screens: centered by us */
	if (xw_is_splash(xs) || !xs->size_hints) return false;
	if (!(xs->size_hints->flags & (XCB_ICCCM_SIZE_HINT_US_POSITION | XCB_ICCCM_SIZE_HINT_P_POSITION)))
		return false;
	/* many toolkits set a position hint of 0,0 without meaning it */
	if (xs->x == 0 && xs->y == 0) return false;
	return wlr_output_layout_contains_point(view->server->output_layout, NULL, xs->x, xs->y);
}

/* ---- managed X11 windows -------------------------------------------------- */

static void create_unmanaged(struct flwm_server *server, struct wlr_xwayland_surface *xs);
static void schedule_kick(struct flwm_server *server);
static void create_view(struct flwm_server *server, struct wlr_xwayland_surface *xs);

static void update_decorations(struct flwm_view *view) {
	struct wlr_xwayland_surface *xs = view->xsurface;
	/* Motif hints ask for no title or border (Steam, many games) */
	view->ssd = xs->decorations == 0 && !xw_is_popup(xs) && !xw_is_splash(xs);
}

static void x_map(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, map);
	struct wlr_xwayland_surface *xs = view->xsurface;
	view->surface_tree = wlr_scene_subsurface_tree_create(view->tree, xs->surface);
	wl_signal_add(&xs->surface->events.commit, &view->commit);
	update_decorations(view);
#if FLWM_WLR_AT_LEAST(0, 19)
	view->no_taskbar = xs->skip_taskbar || xw_is_popup(xs);
#else
	view->no_taskbar = xw_is_popup(xs);
#endif
	if (!view->placed && !xs->parent) {
		/* start where the client configured itself; place_new_view moves it */
		view->x = xs->x;
		view->y = xs->y;
	}
	view_handle_map(view, xs->maximized_horz && xs->maximized_vert, xs->fullscreen,
		xwayland_view_wants_focus(view));
}

static void x_unmap(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, unmap);
	view_handle_unmap(view);
	wl_list_remove(&view->commit.link);
	wl_list_init(&view->commit.link);
	/* the surface outlives the X11 window's mapping; a new map creates a new tree */
	if (view->surface_tree) {
		wlr_scene_node_destroy(&view->surface_tree->node);
		view->surface_tree = NULL;
	}
	view->placed = false;
}

static void x_commit(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, commit);
	view_handle_commit(view);
}

static void x_associate(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, x_associate);
	struct wlr_xwayland_surface *xs = view->xsurface;
	view->map.notify = x_map;
	wl_signal_add(&xs->surface->events.map, &view->map);
	view->unmap.notify = x_unmap;
	wl_signal_add(&xs->surface->events.unmap, &view->unmap);
	view->x_associated = true;
}

static void x_dissociate(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, x_dissociate);
	if (!view->x_associated) return;
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	view->x_associated = false;
}

static void remove_view(struct flwm_view *view) {
	if (view->mapped) {
		/* not unmapped first (Xwayland went away) */
		view_handle_unmap(view);
		if (view->surface_tree) wlr_scene_node_destroy(&view->surface_tree->node);
		view->surface_tree = NULL;
	}
	if (view->x_associated) {
		wl_list_remove(&view->map.link);
		wl_list_remove(&view->unmap.link);
	}
	wl_list_remove(&view->commit.link);
	wl_list_remove(&view->x_associate.link);
	wl_list_remove(&view->x_dissociate.link);
	wl_list_remove(&view->x_request_configure.link);
	wl_list_remove(&view->x_request_activate.link);
	wl_list_remove(&view->x_set_decorations.link);
	wl_list_remove(&view->x_set_override_redirect.link);
	view->xsurface->data = NULL;
	view_free(view);
}

static void x_destroy(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, destroy);
	remove_view(view);
}

static void x_request_configure(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, x_request_configure);
	struct wlr_xwayland_surface_configure_event *ev = data;
	if (!view->mapped) {
		/* before mapping the client chooses its size and position */
		xw_configure(view->xsurface, ev->x, ev->y, ev->width, ev->height);
		return;
	}
	if (view->maximized || view->fullscreen) {
		/* keep the size we gave it, but tell it again */
		xwayland_view_move(view, view->x, view->y);
		return;
	}
	/* floating windows may move and resize themselves (games, Wine) */
	xw_configure(view->xsurface, ev->x, ev->y, ev->width, ev->height);
	view_set_position(view, ev->x, ev->y); /* already configured there: no second configure */
	decor_update(view);
}

static void x_request_activate(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, x_request_activate);
	if (view->mapped) view_focus(view);
}

static void x_request_move(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, request_move);
	if (view->mapped) view_begin_interactive(view, FLWM_CURSOR_MOVE, 0);
}

static void x_request_resize(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, request_resize);
	struct wlr_xwayland_resize_event *ev = data;
	if (view->mapped) view_begin_interactive(view, FLWM_CURSOR_RESIZE, ev->edges);
}

static void x_request_maximize(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, request_maximize);
	struct wlr_xwayland_surface *xs = view->xsurface;
	if (view->mapped) view_set_maximized(view, xs->maximized_horz && xs->maximized_vert);
}

static void x_request_minimize(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, request_minimize);
	struct wlr_xwayland_minimize_event *ev = data;
	if (view->mapped) view_set_minimized(view, ev->minimize);
}

static void x_request_fullscreen(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, request_fullscreen);
	if (view->mapped) view_set_fullscreen(view, view->xsurface->fullscreen);
}

static void x_set_title(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, set_title);
	if (view->ftl && view->xsurface->title)
		wlr_foreign_toplevel_handle_v1_set_title(view->ftl, view->xsurface->title);
	capture_view_update(view);
	if (view->mapped) decor_update(view);
}

static void x_set_class(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, set_app_id);
	if (view->ftl && view->xsurface->class)
		wlr_foreign_toplevel_handle_v1_set_app_id(view->ftl, view->xsurface->class);
	capture_view_update(view);
}

static void x_set_decorations(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, x_set_decorations);
	update_decorations(view);
	if (view->mapped) {
		view->decor.width = -1;
		decor_update(view);
	}
}

static void x_set_override_redirect(struct wl_listener *listener, void *data) {
	struct flwm_view *view = wl_container_of(listener, view, x_set_override_redirect);
	struct flwm_server *server = view->server;
	struct wlr_xwayland_surface *xs = view->xsurface;
	remove_view(view);
	create_unmanaged(server, xs);
}

static void create_view(struct flwm_server *server, struct wlr_xwayland_surface *xs) {
	struct flwm_view *view = view_new(server, FLWM_VIEW_XWAYLAND);
	view->xsurface = xs;
	view->role_checked = true; /* X11 programs never get a shell role */
	xs->data = view;
	wl_list_init(&view->commit.link);
	view->commit.notify = x_commit;

	view->destroy.notify = x_destroy;
	wl_signal_add(&xs->events.destroy, &view->destroy);
	view->x_associate.notify = x_associate;
	wl_signal_add(&xs->events.associate, &view->x_associate);
	view->x_dissociate.notify = x_dissociate;
	wl_signal_add(&xs->events.dissociate, &view->x_dissociate);
	view->x_request_configure.notify = x_request_configure;
	wl_signal_add(&xs->events.request_configure, &view->x_request_configure);
	view->x_request_activate.notify = x_request_activate;
	wl_signal_add(&xs->events.request_activate, &view->x_request_activate);
	view->request_move.notify = x_request_move;
	wl_signal_add(&xs->events.request_move, &view->request_move);
	view->request_resize.notify = x_request_resize;
	wl_signal_add(&xs->events.request_resize, &view->request_resize);
	view->request_maximize.notify = x_request_maximize;
	wl_signal_add(&xs->events.request_maximize, &view->request_maximize);
	view->request_minimize.notify = x_request_minimize;
	wl_signal_add(&xs->events.request_minimize, &view->request_minimize);
	view->request_fullscreen.notify = x_request_fullscreen;
	wl_signal_add(&xs->events.request_fullscreen, &view->request_fullscreen);
	view->set_title.notify = x_set_title;
	wl_signal_add(&xs->events.set_title, &view->set_title);
	view->set_app_id.notify = x_set_class;
	wl_signal_add(&xs->events.set_class, &view->set_app_id);
	view->x_set_decorations.notify = x_set_decorations;
	wl_signal_add(&xs->events.set_decorations, &view->x_set_decorations);
	view->x_set_override_redirect.notify = x_set_override_redirect;
	wl_signal_add(&xs->events.set_override_redirect, &view->x_set_override_redirect);

	/* a surface that already has its wl_surface (it was override-redirect) */
	if (xs->surface) x_associate(&view->x_associate, NULL);
}

/* ---- override-redirect windows (menus, tooltips) ---------------------------- */

struct flwm_xunmanaged {
	struct flwm_server *server;
	struct wlr_xwayland_surface *xs;
	struct wlr_scene_tree *tree; /* while mapped */
	bool associated;
	struct wl_listener associate, dissociate, map, unmap, set_geometry;
	struct wl_listener request_configure, set_override_redirect, destroy;
};

static void um_map(struct wl_listener *listener, void *data) {
	struct flwm_xunmanaged *um = wl_container_of(listener, um, map);
	struct flwm_server *server = um->server;
	struct wlr_xwayland_surface *xs = um->xs;
	um->tree = wlr_scene_subsurface_tree_create(server->layer_xunmanaged, xs->surface);
	wlr_scene_node_set_position(&um->tree->node, xs->x, xs->y);
	if (!server->locked && !server->focused_layer && xw_or_wants_focus(xs)) {
		struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
		if (kb) wlr_seat_keyboard_notify_enter(server->seat, xs->surface, kb->keycodes, kb->num_keycodes, &kb->modifiers);
		else wlr_seat_keyboard_notify_enter(server->seat, xs->surface, NULL, 0, NULL);
	}
	input_update_pointer_focus(server, 0);
}

static void um_unmap(struct wl_listener *listener, void *data) {
	struct flwm_xunmanaged *um = wl_container_of(listener, um, unmap);
	struct flwm_server *server = um->server;
	if (um->tree) {
		wlr_scene_node_destroy(&um->tree->node);
		um->tree = NULL;
	}
	/* hand the keyboard back to the window under the menu */
	if (server->seat->keyboard_state.focused_surface == um->xs->surface) {
		wlr_seat_keyboard_notify_clear_focus(server->seat);
		if (!server->locked) view_focus_next_available(server);
	}
	input_update_pointer_focus(server, 0);
}

static void um_associate(struct wl_listener *listener, void *data) {
	struct flwm_xunmanaged *um = wl_container_of(listener, um, associate);
	um->map.notify = um_map;
	wl_signal_add(&um->xs->surface->events.map, &um->map);
	um->unmap.notify = um_unmap;
	wl_signal_add(&um->xs->surface->events.unmap, &um->unmap);
	um->associated = true;
}

static void um_dissociate(struct wl_listener *listener, void *data) {
	struct flwm_xunmanaged *um = wl_container_of(listener, um, dissociate);
	if (!um->associated) return;
	wl_list_remove(&um->map.link);
	wl_list_remove(&um->unmap.link);
	um->associated = false;
}

static void um_set_geometry(struct wl_listener *listener, void *data) {
	struct flwm_xunmanaged *um = wl_container_of(listener, um, set_geometry);
	if (um->tree) wlr_scene_node_set_position(&um->tree->node, um->xs->x, um->xs->y);
}

static void um_request_configure(struct wl_listener *listener, void *data) {
	struct flwm_xunmanaged *um = wl_container_of(listener, um, request_configure);
	struct wlr_xwayland_surface_configure_event *ev = data;
	xw_configure(um->xs, ev->x, ev->y, ev->width, ev->height);
}

static void um_remove(struct flwm_xunmanaged *um) {
	if (um->tree) um_unmap(&um->unmap, NULL);
	if (um->associated) {
		wl_list_remove(&um->map.link);
		wl_list_remove(&um->unmap.link);
	}
	wl_list_remove(&um->associate.link);
	wl_list_remove(&um->dissociate.link);
	wl_list_remove(&um->set_geometry.link);
	wl_list_remove(&um->request_configure.link);
	wl_list_remove(&um->set_override_redirect.link);
	wl_list_remove(&um->destroy.link);
	um->xs->data = NULL;
	free(um);
}

static void um_destroy(struct wl_listener *listener, void *data) {
	struct flwm_xunmanaged *um = wl_container_of(listener, um, destroy);
	um_remove(um);
}

static void um_set_override_redirect(struct wl_listener *listener, void *data) {
	struct flwm_xunmanaged *um = wl_container_of(listener, um, set_override_redirect);
	struct flwm_server *server = um->server;
	struct wlr_xwayland_surface *xs = um->xs;
	um_remove(um);
	create_view(server, xs);
}

static void create_unmanaged(struct flwm_server *server, struct wlr_xwayland_surface *xs) {
	struct flwm_xunmanaged *um = calloc(1, sizeof(*um));
	if (!um) return;
	um->server = server;
	um->xs = xs;
	xs->data = um;
	um->associate.notify = um_associate;
	wl_signal_add(&xs->events.associate, &um->associate);
	um->dissociate.notify = um_dissociate;
	wl_signal_add(&xs->events.dissociate, &um->dissociate);
	um->set_geometry.notify = um_set_geometry;
	wl_signal_add(&xs->events.set_geometry, &um->set_geometry);
	um->request_configure.notify = um_request_configure;
	wl_signal_add(&xs->events.request_configure, &um->request_configure);
	um->set_override_redirect.notify = um_set_override_redirect;
	wl_signal_add(&xs->events.set_override_redirect, &um->set_override_redirect);
	um->destroy.notify = um_destroy;
	wl_signal_add(&xs->events.destroy, &um->destroy);
	if (xs->surface) um_associate(&um->associate, NULL);
}

/* ---- the Xwayland server ---------------------------------------------------- */

static void new_xwayland_surface(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, new_xwayland_surface);
	struct wlr_xwayland_surface *xs = data;
	schedule_kick(server);
	if (xs->override_redirect) create_unmanaged(server, xs);
	else create_view(server, xs);
}

#if FLWM_WLR_AT_LEAST(0, 20)
/* the root window cursor for 0.20, which takes a wlr_buffer */
struct pixel_buffer {
	struct wlr_buffer base;
	void *data;
	size_t stride;
};

static void pixel_buffer_destroy(struct wlr_buffer *b) {
	struct pixel_buffer *p = wl_container_of(b, p, base);
	flwm_buffer_finish(b);
	free(p->data);
	free(p);
}

static bool pixel_buffer_begin(struct wlr_buffer *b, uint32_t flags, void **data,
		uint32_t *format, size_t *stride) {
	struct pixel_buffer *p = wl_container_of(b, p, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) return false;
	*data = p->data;
	*format = DRM_FORMAT_ARGB8888;
	*stride = p->stride;
	return true;
}

static void pixel_buffer_end(struct wlr_buffer *b) {
	(void)b;
}

static const struct wlr_buffer_impl pixel_buffer_impl = {
	.destroy = pixel_buffer_destroy,
	.begin_data_ptr_access = pixel_buffer_begin,
	.end_data_ptr_access = pixel_buffer_end,
};
#endif

void xwayland_update_cursor(struct flwm_server *server) {
	/* X11 windows without a cursor of their own show the root window's */
	if (!server->xwayland || !server->cursor_mgr) return;
	if (!wlr_xcursor_manager_load(server->cursor_mgr, 1)) return;
	struct wlr_xcursor *xc = wlr_xcursor_manager_get_xcursor(server->cursor_mgr, "default", 1);
	if (!xc || xc->image_count == 0) return;
	struct wlr_xcursor_image *img = xc->images[0];
#if FLWM_WLR_AT_LEAST(0, 20)
	struct pixel_buffer *p = calloc(1, sizeof(*p));
	if (!p) return;
	p->stride = img->width * 4;
	p->data = malloc(p->stride * img->height);
	if (!p->data) {
		free(p);
		return;
	}
	memcpy(p->data, img->buffer, p->stride * img->height);
	wlr_buffer_init(&p->base, &pixel_buffer_impl, img->width, img->height);
	wlr_xwayland_set_cursor(server->xwayland, &p->base, img->hotspot_x, img->hotspot_y);
	wlr_buffer_drop(&p->base);
#else
	wlr_xwayland_set_cursor(server->xwayland, img->buffer, img->width * 4, img->width, img->height,
		img->hotspot_x, img->hotspot_y);
#endif
}

/* wlroots' window manager waits for X11 replies in some of its callbacks
 * (pairing a new wl_surface with its X11 window). X11 events that arrive
 * meanwhile land in libxcb's queue, and as its socket is then not readable
 * again the window manager does not look at them until other X11 traffic
 * comes: a new window could hang unmapped until the next X11 client starts.
 * For a moment after each new window we send a request now and then whose
 * reply wakes it up (wlroots 0.20 lets us reach its connection). This cures
 * nearly all such hangs; in the headless tests about one window in a hundred
 * still waits until the next X11 activity, which this does not reach. */
static struct flwm_server *kick_server;
static struct wl_event_source *kick_timer;
static int kick_step;
static struct wl_listener kick_new_surface, kick_shell_surface;
static const int kick_delays_ms[] = {20, 50, 100, 200, 400, 800};

static void kick_xwm(struct flwm_server *server) {
#if FLWM_WLR_AT_LEAST(0, 20)
	if (!server->xwayland) return;
	xcb_connection_t *conn = wlr_xwayland_get_xwm_connection(server->xwayland);
	if (!conn) return;
	xcb_get_input_focus_cookie_t cookie = xcb_get_input_focus(conn);
	xcb_discard_reply(conn, cookie.sequence);
	xcb_flush(conn);
#else
	(void)server;
#endif
}

static int kick_timer_fn(void *data) {
	kick_xwm(data);
	if (++kick_step < (int)(sizeof kick_delays_ms / sizeof kick_delays_ms[0]))
		wl_event_source_timer_update(kick_timer, kick_delays_ms[kick_step]);
	return 0;
}

static void schedule_kick(struct flwm_server *server) {
#if FLWM_WLR_AT_LEAST(0, 20)
	if (!kick_timer) kick_timer = wl_event_loop_add_timer(server->event_loop, kick_timer_fn, server);
	if (!kick_timer) return;
	kick_step = 0; /* (re)start the series */
	wl_event_source_timer_update(kick_timer, kick_delays_ms[0]);
#else
	(void)server;
#endif
}

static void kick_on_surface(struct wl_listener *listener, void *data) {
	struct flwm_server *server = kick_server;
	if (!server || !server->xwayland || !server->xwayland->server) return;
	if (listener == &kick_new_surface) {
		/* only Xwayland's own surfaces */
		struct wlr_surface *surface = data;
		if (!surface->resource || wl_resource_get_client(surface->resource) != server->xwayland->server->client)
			return;
	}
	schedule_kick(server);
}

static void xwayland_ready(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, xwayland_ready);
	wlr_log(WLR_INFO, "Xwayland is ready on DISPLAY=%s", server->xwayland->display_name);
	wlr_xwayland_set_seat(server->xwayland, server->seat);
}

void xwayland_init(struct flwm_server *server) {
	wl_list_init(&server->xwayland_ready.link);
	wl_list_init(&server->new_xwayland_surface.link);
	if (!server->config.xwayland) {
		wlr_log(WLR_INFO, "Xwayland is turned off ([behaviour] xwayland=false)");
		return;
	}
	server->xwayland = wlr_xwayland_create(server->wl_display, server->compositor, true);
	if (!server->xwayland) {
		wlr_log(WLR_ERROR, "cannot start Xwayland (is it installed?): X11 programs will not run");
		return;
	}
	server->xwayland_ready.notify = xwayland_ready;
	wl_signal_add(&server->xwayland->events.ready, &server->xwayland_ready);
	server->new_xwayland_surface.notify = new_xwayland_surface;
	wl_signal_add(&server->xwayland->events.new_surface, &server->new_xwayland_surface);
	kick_server = server;
	kick_new_surface.notify = kick_on_surface;
	wl_signal_add(&server->compositor->events.new_surface, &kick_new_surface);
	kick_shell_surface.notify = kick_on_surface;
	wl_signal_add(&server->xwayland->shell_v1->events.new_surface, &kick_shell_surface);
	/* set before Xwayland is ready: wlroots applies it while starting its
	 * window manager. Setting it in the ready handler waits for replies that
	 * can swallow the first window's MapRequest, which then hangs unmapped. */
	xwayland_update_cursor(server);
	setenv("DISPLAY", server->xwayland->display_name, 1);
	wlr_log(WLR_INFO, "X11 programs use DISPLAY=%s (Xwayland starts with the first one)",
		server->xwayland->display_name);
}

const char *xwayland_display(struct flwm_server *server) {
	return server->xwayland ? server->xwayland->display_name : NULL;
}

void xwayland_finish(struct flwm_server *server) {
	if (!server->xwayland) return;
	wl_list_remove(&server->xwayland_ready.link);
	wl_list_remove(&server->new_xwayland_surface.link);
	wl_list_remove(&kick_new_surface.link);
	wl_list_remove(&kick_shell_surface.link);
	if (kick_timer) wl_event_source_remove(kick_timer);
	kick_timer = NULL;
	kick_server = NULL;
	wlr_xwayland_destroy(server->xwayland);
	server->xwayland = NULL;
}
