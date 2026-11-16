/* fl-wm: wlr-layer-shell support (for third-party bars, wallpapers, launchers,
 * notification daemons and lock screens) */
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/util/log.h>

#include "server.h"

static struct wlr_scene_tree *tree_for_layer(struct flwm_server *server,
		enum zwlr_layer_shell_v1_layer layer) {
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return server->layer_background;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM: return server->layer_bottom;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP: return server->layer_top;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY: return server->layer_overlay;
	}
	return server->layer_top;
}

static void arrange_pass(struct flwm_output *output, bool exclusive) {
	static const enum zwlr_layer_shell_v1_layer order[] = {
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
		ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
	};
	for (int i = 0; i < 4; i++) {
		struct flwm_layer_surface *ls;
		wl_list_for_each(ls, &output->layers[order[i]], link) {
			struct wlr_layer_surface_v1 *s = ls->layer_surface;
			if (!s->initialized) continue;
			if ((s->current.exclusive_zone > 0) != exclusive) continue;
			wlr_scene_layer_surface_v1_configure(ls->scene, &output->full_area, &output->usable_area);
		}
	}
}

void layers_arrange(struct flwm_output *output) {
	arrange_pass(output, true);
	arrange_pass(output, false);
}

static void focus_layer(struct flwm_layer_surface *ls) {
	struct flwm_server *server = ls->server;
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
	server->focused_layer = ls;
	struct wlr_surface *s = ls->layer_surface->surface;
	if (kb) wlr_seat_keyboard_notify_enter(server->seat, s, kb->keycodes, kb->num_keycodes, &kb->modifiers);
	else wlr_seat_keyboard_notify_enter(server->seat, s, NULL, 0, NULL);
}

void layer_focus_none(struct flwm_server *server) {
	if (!server->focused_layer) return;
	server->focused_layer = NULL;
	view_focus_next_available(server);
}

static void layer_map(struct wl_listener *listener, void *data) {
	struct flwm_layer_surface *ls = wl_container_of(listener, ls, map);
	ls->mapped = true;
	struct wlr_layer_surface_v1 *s = ls->layer_surface;
	if (s->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE &&
			s->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP)
		focus_layer(ls);
	if (ls->output) arrange_all(ls->server);
}

static void layer_unmap(struct wl_listener *listener, void *data) {
	struct flwm_layer_surface *ls = wl_container_of(listener, ls, unmap);
	ls->mapped = false;
	if (ls->server->focused_layer == ls) layer_focus_none(ls->server);
	if (ls->output) arrange_all(ls->server);
}

static void layer_commit(struct wl_listener *listener, void *data) {
	struct flwm_layer_surface *ls = wl_container_of(listener, ls, commit);
	struct wlr_layer_surface_v1 *s = ls->layer_surface;
	if (!ls->output) return;
	if (s->initial_commit || s->current.committed) {
		/* the layer may have changed */
		struct wlr_scene_tree *want = tree_for_layer(ls->server, s->current.layer);
		if (ls->scene->tree->node.parent != want) {
			wlr_scene_node_reparent(&ls->scene->tree->node, want);
			wl_list_remove(&ls->link);
			wl_list_insert(&ls->output->layers[s->current.layer], &ls->link);
		}
		if (s->initial_commit) {
			/* send the first configure */
			struct wlr_box full = ls->output->full_area, usable = ls->output->usable_area;
			wlr_scene_layer_surface_v1_configure(ls->scene, &full, &usable);
		}
		if (s->current.committed) arrange_all(ls->server);
	}
}

static void layer_destroy(struct wl_listener *listener, void *data) {
	struct flwm_layer_surface *ls = wl_container_of(listener, ls, destroy);
	if (ls->server->focused_layer == ls) layer_focus_none(ls->server);
	wl_list_remove(&ls->link);
	wl_list_remove(&ls->map.link);
	wl_list_remove(&ls->unmap.link);
	wl_list_remove(&ls->commit.link);
	wl_list_remove(&ls->destroy.link);
	wl_list_remove(&ls->new_popup.link);
	free(ls);
}

static void layer_new_popup(struct wl_listener *listener, void *data) {
	struct flwm_layer_surface *ls = wl_container_of(listener, ls, new_popup);
	struct wlr_xdg_popup *popup = data;
	view_popup_create(popup, ls->scene->tree);
}

static void server_new_layer_surface(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, new_layer_surface);
	struct wlr_layer_surface_v1 *s = data;
	if (!s->output) {
		struct flwm_output *o = output_at(server, server->cursor->x, server->cursor->y);
		if (!o) {
			wlr_layer_surface_v1_destroy(s);
			return;
		}
		s->output = o->wlr_output;
	}
	struct flwm_output *output = s->output->data;
	struct flwm_layer_surface *ls = calloc(1, sizeof(*ls));
	ls->server = server;
	ls->output = output;
	ls->layer_surface = s;
	ls->scene = wlr_scene_layer_surface_v1_create(tree_for_layer(server, s->pending.layer), s);
	ls->desc.kind = FLWM_NODE_LAYER;
	ls->desc.ptr = ls;
	ls->scene->tree->node.data = &ls->desc;
	s->data = ls;
	wl_list_insert(&output->layers[s->pending.layer], &ls->link);

	ls->map.notify = layer_map;
	wl_signal_add(&s->surface->events.map, &ls->map);
	ls->unmap.notify = layer_unmap;
	wl_signal_add(&s->surface->events.unmap, &ls->unmap);
	ls->commit.notify = layer_commit;
	wl_signal_add(&s->surface->events.commit, &ls->commit);
	ls->destroy.notify = layer_destroy;
	wl_signal_add(&s->events.destroy, &ls->destroy);
	ls->new_popup.notify = layer_new_popup;
	wl_signal_add(&s->events.new_popup, &ls->new_popup);
}

void layer_init(struct flwm_server *server) {
	server->layer_shell = wlr_layer_shell_v1_create(server->wl_display, 4);
	server->new_layer_surface.notify = server_new_layer_surface;
	wl_signal_add(&server->layer_shell->events.new_surface, &server->new_layer_surface);
}

/* called by input.c when a layer surface is clicked */
void layer_clicked(struct flwm_layer_surface *ls) {
	if (ls->layer_surface->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE)
		focus_layer(ls);
}
