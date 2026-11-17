/* fl-wm outputs */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

#include "server.h"

static void output_frame(struct wl_listener *listener, void *data) {
	struct flwm_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene_output *scene_output = output->scene_output;
	if (!scene_output) return;
	wlr_scene_output_commit(scene_output, NULL);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
	/* nested backends (wayland/x11) request size changes when their window is resized */
	struct flwm_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
	arrange_all(output->server);
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct flwm_output *output = wl_container_of(listener, output, destroy);
	struct flwm_server *server = output->server;
	switcher_output_gone(server, output);
	for (int i = 0; i < 4; i++) {
		struct flwm_layer_surface *ls, *tmp;
		wl_list_for_each_safe(ls, tmp, &output->layers[i], link) {
			ls->output = NULL;
			wl_list_remove(&ls->link);
			wl_list_init(&ls->link);
			wlr_layer_surface_v1_destroy(ls->layer_surface);
		}
	}
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);
	arrange_all(server);
	output_manager_update(server);
}

static struct flwm_output_config *find_output_config(struct flwm_server *server, const char *name) {
	for (int i = 0; i < server->config.noutputs; i++)
		if (!strcmp(server->config.outputs[i].name, name)) return &server->config.outputs[i];
	return NULL;
}

static void configure_output(struct flwm_server *server, struct wlr_output *wlr_output,
		struct wlr_output_state *state) {
	struct flwm_output_config *oc = find_output_config(server, wlr_output->name);
	wlr_output_state_set_enabled(state, !(oc && oc->disabled));
	struct wlr_output_mode *mode = NULL;
	if (oc && oc->width > 0) {
		struct wlr_output_mode *m;
		wl_list_for_each(m, &wlr_output->modes, link) {
			if (m->width != oc->width || m->height != oc->height) continue;
			if (oc->refresh_mhz && abs(m->refresh - oc->refresh_mhz) > 1500) continue;
			if (!mode || m->refresh > mode->refresh) mode = m;
		}
		if (!mode && wl_list_empty(&wlr_output->modes))
			wlr_output_state_set_custom_mode(state, oc->width, oc->height, oc->refresh_mhz);
	}
	if (!mode) mode = wlr_output_preferred_mode(wlr_output);
	if (mode) wlr_output_state_set_mode(state, mode);
	float scale = server->config.default_scale;
	if (oc && oc->scale > 0) scale = oc->scale;
	if (scale > 0) wlr_output_state_set_scale(state, scale);
	if (oc) wlr_output_state_set_transform(state, oc->transform);
}

static void server_new_output(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	configure_output(server, wlr_output, &state);
	if (!wlr_output_commit_state(wlr_output, &state)) {
		/* fall back to the preferred mode without extras */
		wlr_output_state_finish(&state);
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, true);
		struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
		if (mode) wlr_output_state_set_mode(&state, mode);
		wlr_output_commit_state(wlr_output, &state);
	}
	wlr_output_state_finish(&state);

	struct flwm_output *output = calloc(1, sizeof(*output));
	output->wlr_output = wlr_output;
	output->server = server;
	wlr_output->data = output;
	for (int i = 0; i < 4; i++) wl_list_init(&output->layers[i]);

	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);
	wl_list_insert(server->outputs.prev, &output->link);

	struct flwm_output_config *oc = find_output_config(server, wlr_output->name);
	output->scene_output = wlr_scene_output_create(server->scene, wlr_output);
	if (wlr_output->enabled) {
		struct wlr_output_layout_output *l_output;
		if (oc && oc->has_position)
			l_output = wlr_output_layout_add(server->output_layout, wlr_output, oc->x, oc->y);
		else
			l_output = wlr_output_layout_add_auto(server->output_layout, wlr_output);
		wlr_scene_output_layout_add_output(server->scene_layout, l_output, output->scene_output);
	}

	wlr_log(WLR_INFO, "output %s: %dx%d scale %.2f", wlr_output->name,
		wlr_output->width, wlr_output->height, wlr_output->scale);
	arrange_all(server);
}

static void layout_change(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, layout_change);
	arrange_all(server);
	output_manager_update(server);
}

static void output_manager_apply(struct wl_listener *listener, void *data);
static void output_manager_test(struct wl_listener *listener, void *data);

void output_init(struct flwm_server *server) {
	wl_list_init(&server->outputs);
	server->new_output.notify = server_new_output;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);
	server->layout_change.notify = layout_change;
	wl_signal_add(&server->output_layout->events.change, &server->layout_change);
	server->output_manager = wlr_output_manager_v1_create(server->wl_display);
	server->output_manager_apply.notify = output_manager_apply;
	wl_signal_add(&server->output_manager->events.apply, &server->output_manager_apply);
	server->output_manager_test.notify = output_manager_test;
	wl_signal_add(&server->output_manager->events.test, &server->output_manager_test);
}

struct flwm_output *output_at(struct flwm_server *server, double lx, double ly) {
	struct wlr_output *o = wlr_output_layout_output_at(server->output_layout, lx, ly);
	if (o && o->data) return o->data;
	return output_primary(server);
}

struct flwm_output *output_primary(struct flwm_server *server) {
	if (wl_list_empty(&server->outputs)) return NULL;
	/* the output containing 0,0, else the first one */
	struct wlr_output *o = wlr_output_layout_output_at(server->output_layout, 0, 0);
	if (o && o->data) return o->data;
	struct flwm_output *first = wl_container_of(server->outputs.next, first, link);
	return first;
}

struct flwm_output *output_for_view(struct flwm_view *view) {
	struct wlr_box geo;
	view_get_geometry(view, &geo);
	return output_at(view->server, geo.x + geo.width / 2.0, geo.y + geo.height / 2.0);
}

void output_arrange(struct flwm_output *output) {
	struct flwm_server *server = output->server;
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &output->full_area);
	output->usable_area = output->full_area;

	/* the fl-panel lives on the primary output and reserves its height */
	struct flwm_view *panel = view_find_role(server, FLWM_ROLE_PANEL);
	if (panel && panel->mapped && output == output_primary(server)) {
		struct wlr_box geo;
		flwm_xdg_geometry(panel->xdg_toplevel->base, &geo);
		int h = geo.height;
		if (server->config.panel_top) output->usable_area.y += h;
		output->usable_area.height -= h;
	}
	/* a visible on-screen keyboard reserves its height above a bottom panel */
	struct flwm_view *osk = view_find_role(server, FLWM_ROLE_OSK);
	if (osk && osk->mapped && output == output_primary(server)) {
		struct wlr_box geo;
		flwm_xdg_geometry(osk->xdg_toplevel->base, &geo);
		output->usable_area.height -= geo.height;
	}
	layers_arrange(output); /* shrinks usable_area further for exclusive zones */
}

/* ---- wlr-output-management (wlr-randr, kanshi, fl-settings Displays) ---- */

void output_manager_update(struct flwm_server *server) {
	if (!server->output_manager) return;
	struct wlr_output_configuration_v1 *config = wlr_output_configuration_v1_create();
	struct flwm_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		struct wlr_output_configuration_head_v1 *head =
			wlr_output_configuration_head_v1_create(config, o->wlr_output);
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
		if (!wlr_box_empty(&box)) {
			head->state.x = box.x;
			head->state.y = box.y;
		}
	}
	wlr_output_manager_v1_set_configuration(server->output_manager, config);
}

static void apply_output_config(struct flwm_server *server,
		struct wlr_output_configuration_v1 *config, bool test) {
	bool ok = true;
	struct wlr_output_configuration_head_v1 *head;
	wl_list_for_each(head, &config->heads, link) {
		struct wlr_output *wo = head->state.output;
		struct flwm_output *o = wo->data;
		struct wlr_output_state st;
		wlr_output_state_init(&st);
		wlr_output_head_v1_state_apply(&head->state, &st);
		if (test) {
			ok = wlr_output_test_state(wo, &st) && ok;
		} else if (wlr_output_commit_state(wo, &st)) {
			if (head->state.enabled) {
				struct wlr_output_layout_output *lo =
					wlr_output_layout_add(server->output_layout, wo, head->state.x, head->state.y);
				if (o && o->scene_output)
					wlr_scene_output_layout_add_output(server->scene_layout, lo, o->scene_output);
			} else {
				wlr_output_layout_remove(server->output_layout, wo);
			}
		} else {
			ok = false;
		}
		wlr_output_state_finish(&st);
	}
	if (ok) wlr_output_configuration_v1_send_succeeded(config);
	else wlr_output_configuration_v1_send_failed(config);
	wlr_output_configuration_v1_destroy(config);
	if (!test) {
		arrange_all(server);
		output_manager_update(server);
	}
}

static void output_manager_apply(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, output_manager_apply);
	apply_output_config(server, data, false);
}

static void output_manager_test(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, output_manager_test);
	apply_output_config(server, data, true);
}

void output_finish(struct flwm_server *server) {
	wl_list_remove(&server->output_manager_apply.link);
	wl_list_remove(&server->output_manager_test.link);
}

void arrange_all(struct flwm_server *server) {
	struct flwm_output *output;
	wl_list_for_each(output, &server->outputs, link) output_arrange(output);
	struct flwm_view *view;
	wl_list_for_each(view, &server->views, link)
		if (view->role != FLWM_ROLE_NORMAL) view_reposition_shell(view);
	views_reconfigure_maximized(server);
	lock_arrange(server);
}

void output_apply_config(struct flwm_server *server) {
	struct flwm_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		configure_output(server, output->wlr_output, &state);
		wlr_output_commit_state(output->wlr_output, &state);
		wlr_output_state_finish(&state);
	}
	arrange_all(server);
}
