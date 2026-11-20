/* Screen and window capture for screen sharing (ext-image-copy-capture-v1).
 *
 * wlroots 0.19 adds capture of outputs; 0.20 adds capture of single windows
 * through ext-foreign-toplevel-list-v1. wlr-screencopy (main.c) stays for
 * grim and older clients. */
#include "server.h"

#if FLWM_WLR_AT_LEAST(0, 19)
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/util/log.h>
#endif
#if FLWM_WLR_AT_LEAST(0, 20)
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#endif

#if FLWM_WLR_AT_LEAST(0, 20)
static struct wlr_ext_foreign_toplevel_list_v1 *toplevel_list;
static struct wl_listener new_toplevel_request;
static struct wl_listener toplevel_mgr_destroy;

static const char *app_id_of(struct flwm_view *view) {
	const char *s = view_app_id(view);
	return s ? s : "";
}

static const char *title_of(struct flwm_view *view) {
	const char *s = view_title(view);
	return s ? s : "";
}

/* Each window is drawn into a scene of its own for capture, so a shared
 * window shows none of the windows above it. */
static void handle_toplevel_request(struct wl_listener *l, void *data) {
	struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *req = data;
	struct flwm_view *view = req->toplevel_handle->data;
	if (!view || !view->capture_scene) return;
	struct flwm_server *server = view->server;
	if (!view->capture_source) {
		view->capture_source = wlr_ext_image_capture_source_v1_create_with_scene_node(
			&view->capture_scene->tree.node, server->event_loop, server->allocator,
			server->renderer);
		if (!view->capture_source) return;
	}
	wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(req, view->capture_source);
}

/* wlroots asserts that nobody listens any more when the display goes */
static void handle_toplevel_mgr_destroy(struct wl_listener *l, void *data) {
	wl_list_remove(&new_toplevel_request.link);
	wl_list_remove(&toplevel_mgr_destroy.link);
}
#endif

void capture_init(struct flwm_server *server) {
#if FLWM_WLR_AT_LEAST(0, 19)
	wlr_ext_image_copy_capture_manager_v1_create(server->wl_display, 1);
	wlr_ext_output_image_capture_source_manager_v1_create(server->wl_display, 1);
#endif
#if FLWM_WLR_AT_LEAST(0, 20)
	toplevel_list = wlr_ext_foreign_toplevel_list_v1_create(server->wl_display, 1);
	struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1 *m =
		wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(server->wl_display, 1);
	new_toplevel_request.notify = handle_toplevel_request;
	wl_signal_add(&m->events.new_request, &new_toplevel_request);
	toplevel_mgr_destroy.notify = handle_toplevel_mgr_destroy;
	wl_signal_add(&m->events.destroy, &toplevel_mgr_destroy);
#else
	(void)server;
#endif
}

void capture_view_map(struct flwm_view *view) {
#if FLWM_WLR_AT_LEAST(0, 20)
	if (view->ext_ftl || view->role != FLWM_ROLE_NORMAL || view->no_taskbar) return;
	struct wlr_scene *scene = wlr_scene_create();
	if (!scene) return;
	struct wlr_scene_tree *tree = NULL;
	if (view->type == FLWM_VIEW_XDG)
		tree = wlr_scene_xdg_surface_create(&scene->tree, view->xdg_toplevel->base);
	else if (xwayland_view_surface(view))
		tree = wlr_scene_subsurface_tree_create(&scene->tree, xwayland_view_surface(view));
	if (!tree) {
		wlr_scene_node_destroy(&scene->tree.node);
		return;
	}
	struct wlr_ext_foreign_toplevel_handle_v1_state st = {
		.title = title_of(view), .app_id = app_id_of(view),
	};
	view->ext_ftl = wlr_ext_foreign_toplevel_handle_v1_create(toplevel_list, &st);
	if (!view->ext_ftl) {
		wlr_scene_node_destroy(&scene->tree.node);
		return;
	}
	view->ext_ftl->data = view;
	view->capture_scene = scene;
#else
	(void)view;
#endif
}

void capture_view_update(struct flwm_view *view) {
#if FLWM_WLR_AT_LEAST(0, 20)
	if (!view->ext_ftl) return;
	struct wlr_ext_foreign_toplevel_handle_v1_state st = {
		.title = title_of(view), .app_id = app_id_of(view),
	};
	wlr_ext_foreign_toplevel_handle_v1_update_state(view->ext_ftl, &st);
#else
	(void)view;
#endif
}

void capture_view_unmap(struct flwm_view *view) {
#if FLWM_WLR_AT_LEAST(0, 20)
	if (view->ext_ftl) {
		view->ext_ftl->data = NULL;
		wlr_ext_foreign_toplevel_handle_v1_destroy(view->ext_ftl);
		view->ext_ftl = NULL;
	}
	if (view->capture_scene) {
		/* the capture source goes with its scene node */
		wlr_scene_node_destroy(&view->capture_scene->tree.node);
		view->capture_scene = NULL;
		view->capture_source = NULL;
	}
#else
	(void)view;
#endif
}
