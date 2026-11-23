/* Input methods: relays between programs (text-input-unstable-v3) and an
 * input method such as fcitx5 or IBus (input-method-unstable-v2), so CJK and
 * other composed input works.
 *
 * The text input of the focused surface that is enabled is the active one.
 * The input method gets its state (surrounding text, content type) and sends
 * back preedit and commit strings, which go to that text input. While the
 * input method grabs the keyboard, keys go to it instead of the program,
 * except those of its own virtual keyboard. Its candidate popups are placed
 * below the text cursor. */
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/util/log.h>

#include "server.h"

struct text_input {
	struct wlr_text_input_v3 *ti;
	struct wl_list link;
	struct wl_listener enable, commit, disable, destroy;
};

struct im_popup {
	struct wlr_input_popup_surface_v2 *popup;
	struct wlr_scene_tree *tree;
	struct wl_list link;
	struct wl_listener map, unmap, commit, destroy;
};

static struct {
	struct flwm_server *server;
	struct wlr_text_input_manager_v3 *ti_mgr;
	struct wlr_input_method_manager_v2 *im_mgr;
	struct wl_list text_inputs; /* text_input.link */
	struct wl_list popups;      /* im_popup.link */
	struct wlr_input_method_v2 *im;
	struct wlr_input_method_keyboard_grab_v2 *grab;
	struct wl_listener new_text_input, new_input_method;
	struct wl_listener im_commit, im_grab, im_new_popup, im_destroy, grab_destroy;
	struct wl_listener focus_change;
} R;

static struct wl_client *client_of_surface(struct wlr_surface *s) {
	return s ? wl_resource_get_client(s->resource) : NULL;
}

/* the text input that talks to the input method: focused and enabled */
static struct text_input *active_text_input(void) {
	struct text_input *t;
	wl_list_for_each(t, &R.text_inputs, link)
		if (t->ti->focused_surface && t->ti->current_enabled) return t;
	return NULL;
}

static void send_state(struct wlr_text_input_v3 *ti) {
	struct wlr_input_method_v2 *im = R.im;
	if (ti->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT)
		wlr_input_method_v2_send_surrounding_text(im, ti->current.surrounding.text ? ti->current.surrounding.text : "",
			ti->current.surrounding.cursor, ti->current.surrounding.anchor);
	wlr_input_method_v2_send_text_change_cause(im, ti->current.text_change_cause);
	if (ti->active_features & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE)
		wlr_input_method_v2_send_content_type(im, ti->current.content_type.hint, ti->current.content_type.purpose);
}

/* ---- popups (candidate windows) ------------------------------------------ */

/* layout position of a surface's origin, through the view it belongs to */
static bool surface_origin(struct wlr_surface *surface, int *lx, int *ly) {
	struct wlr_surface *root = wlr_surface_get_root_surface(surface);
	struct flwm_view *v;
	wl_list_for_each(v, &R.server->views, link) {
		if (!v->mapped) continue;
		struct wlr_surface *vs = v->type == FLWM_VIEW_XDG ? v->xdg_toplevel->base->surface : xwayland_view_surface(v);
		if (vs != root) continue;
		return wlr_scene_node_coords(&v->surface_tree->node, lx, ly);
	}
	return false;
}

static void place_popup(struct im_popup *p) {
	struct text_input *t = active_text_input();
	int lx, ly;
	struct wlr_box rect = {0};
	if (t && surface_origin(t->ti->focused_surface, &lx, &ly)) {
		if (t->ti->active_features & WLR_TEXT_INPUT_V3_FEATURE_CURSOR_RECTANGLE)
			rect = t->ti->current.cursor_rectangle;
		lx += rect.x;
		ly += rect.y + rect.height;
	} else {
		lx = (int)R.server->cursor->x;
		ly = (int)R.server->cursor->y + 16;
	}
	/* keep it on the output */
	struct wlr_output *o = wlr_output_layout_output_at(R.server->output_layout, lx, ly);
	if (o) {
		struct wlr_box ob;
		wlr_output_layout_get_box(R.server->output_layout, o, &ob);
		int pw = p->popup->surface->current.width, ph = p->popup->surface->current.height;
		if (lx + pw > ob.x + ob.width) lx = ob.x + ob.width - pw;
		if (ly + ph > ob.y + ob.height) ly -= ph + rect.height; /* above the text instead */
		if (lx < ob.x) lx = ob.x;
		if (ly < ob.y) ly = ob.y;
	}
	wlr_scene_node_set_position(&p->tree->node, lx, ly);
	/* the text cursor relative to the popup */
	wlr_input_popup_surface_v2_send_text_input_rectangle(p->popup,
		&(struct wlr_box){ .x = 0, .y = -rect.height, .width = rect.width, .height = rect.height });
}

static void place_popups(void) {
	struct im_popup *p;
	wl_list_for_each(p, &R.popups, link) place_popup(p);
}

static void popup_map(struct wl_listener *l, void *data) {
	struct im_popup *p = wl_container_of(l, p, map);
	place_popup(p);
	wlr_scene_node_raise_to_top(&p->tree->node);
	wlr_scene_node_set_enabled(&p->tree->node, true);
}

static void popup_unmap(struct wl_listener *l, void *data) {
	struct im_popup *p = wl_container_of(l, p, unmap);
	wlr_scene_node_set_enabled(&p->tree->node, false);
}

static void popup_commit(struct wl_listener *l, void *data) {
	struct im_popup *p = wl_container_of(l, p, commit);
	if (p->popup->surface->mapped) place_popup(p);
}

static void popup_destroy(struct wl_listener *l, void *data) {
	struct im_popup *p = wl_container_of(l, p, destroy);
	wl_list_remove(&p->map.link);
	wl_list_remove(&p->unmap.link);
	wl_list_remove(&p->commit.link);
	wl_list_remove(&p->destroy.link);
	wl_list_remove(&p->link);
	wlr_scene_node_destroy(&p->tree->node);
	free(p);
}

static void im_new_popup(struct wl_listener *l, void *data) {
	struct wlr_input_popup_surface_v2 *popup = data;
	struct im_popup *p = calloc(1, sizeof *p);
	if (!p) return;
	p->popup = popup;
	p->tree = wlr_scene_subsurface_tree_create(R.server->layer_popups, popup->surface);
	if (!p->tree) {
		free(p);
		return;
	}
	wlr_scene_node_set_enabled(&p->tree->node, popup->surface->mapped);
	p->map.notify = popup_map;
	wl_signal_add(&popup->surface->events.map, &p->map);
	p->unmap.notify = popup_unmap;
	wl_signal_add(&popup->surface->events.unmap, &p->unmap);
	p->commit.notify = popup_commit;
	wl_signal_add(&popup->surface->events.commit, &p->commit);
	p->destroy.notify = popup_destroy;
	wl_signal_add(&popup->events.destroy, &p->destroy);
	wl_list_insert(&R.popups, &p->link);
	if (popup->surface->mapped) place_popup(p);
}

/* ---- input method ------------------------------------------------------- */

static void im_commit(struct wl_listener *l, void *data) {
	struct text_input *t = active_text_input();
	struct wlr_input_method_v2 *im = R.im;
	if (!t || !im) return;
	struct wlr_text_input_v3 *ti = t->ti;
	if (im->current.preedit.text)
		wlr_text_input_v3_send_preedit_string(ti, im->current.preedit.text, im->current.preedit.cursor_begin,
			im->current.preedit.cursor_end);
	if (im->current.commit_text) wlr_text_input_v3_send_commit_string(ti, im->current.commit_text);
	if (im->current.delete.before_length || im->current.delete.after_length)
		wlr_text_input_v3_send_delete_surrounding_text(ti, im->current.delete.before_length,
			im->current.delete.after_length);
	wlr_text_input_v3_send_done(ti);
}

static void grab_destroy(struct wl_listener *l, void *data) {
	/* (the signal's data is NULL in some wlroots versions) */
	struct wlr_input_method_keyboard_grab_v2 *grab = R.grab;
	wl_list_remove(&R.grab_destroy.link);
	R.grab = NULL;
	/* the program gets the modifiers the input method had */
	if (grab && grab->keyboard) wlr_seat_keyboard_notify_modifiers(R.server->seat, &grab->keyboard->modifiers);
}

static void im_grab(struct wl_listener *l, void *data) {
	struct wlr_input_method_keyboard_grab_v2 *grab = data;
	R.grab = grab;
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(R.server->seat);
	if (kb) wlr_input_method_keyboard_grab_v2_set_keyboard(grab, kb);
	R.grab_destroy.notify = grab_destroy;
	wl_signal_add(&grab->events.destroy, &R.grab_destroy);
}

static void im_destroy(struct wl_listener *l, void *data) {
	wl_list_remove(&R.im_commit.link);
	wl_list_remove(&R.im_grab.link);
	wl_list_remove(&R.im_new_popup.link);
	wl_list_remove(&R.im_destroy.link);
	R.im = NULL;
	R.grab = NULL; /* its destroy listener ran first */
	struct text_input *t = active_text_input();
	if (t) {
		/* drop what was being composed */
		wlr_text_input_v3_send_preedit_string(t->ti, NULL, 0, 0);
		wlr_text_input_v3_send_done(t->ti);
	}
	wlr_log(WLR_INFO, "input method gone");
}

static void activate(struct text_input *t) {
	if (!R.im || !t) return;
	wlr_input_method_v2_send_activate(R.im);
	send_state(t->ti);
	wlr_input_method_v2_send_done(R.im);
}

static void new_input_method(struct wl_listener *l, void *data) {
	struct wlr_input_method_v2 *im = data;
	if (R.im) {
		wlr_log(WLR_INFO, "a second input method: refused");
		wlr_input_method_v2_send_unavailable(im);
		return;
	}
	R.im = im;
	R.im_commit.notify = im_commit;
	wl_signal_add(&im->events.commit, &R.im_commit);
	R.im_grab.notify = im_grab;
	wl_signal_add(&im->events.grab_keyboard, &R.im_grab);
	R.im_new_popup.notify = im_new_popup;
	wl_signal_add(&im->events.new_popup_surface, &R.im_new_popup);
	R.im_destroy.notify = im_destroy;
	wl_signal_add(&im->events.destroy, &R.im_destroy);
	wlr_log(WLR_INFO, "input method connected");
	activate(active_text_input());
}

/* ---- text inputs --------------------------------------------------------- */

static void ti_enable(struct wl_listener *l, void *data) {
	struct text_input *t = wl_container_of(l, t, enable);
	if (t == active_text_input()) {
		activate(t);
		protocols_text_input(true);
	}
}

static void ti_commit(struct wl_listener *l, void *data) {
	struct text_input *t = wl_container_of(l, t, commit);
	if (!R.im || t != active_text_input()) return;
	send_state(t->ti);
	wlr_input_method_v2_send_done(R.im);
	place_popups();
}

static void ti_disable(struct wl_listener *l, void *data) {
	struct text_input *t = wl_container_of(l, t, disable);
	protocols_text_input(false);
	if (!R.im || !t->ti->focused_surface) return;
	wlr_input_method_v2_send_deactivate(R.im);
	wlr_input_method_v2_send_done(R.im);
}

static void ti_destroy(struct wl_listener *l, void *data) {
	struct text_input *t = wl_container_of(l, t, destroy);
	if (R.im && t->ti->current_enabled && t->ti->focused_surface) {
		wlr_input_method_v2_send_deactivate(R.im);
		wlr_input_method_v2_send_done(R.im);
	}
	wl_list_remove(&t->enable.link);
	wl_list_remove(&t->commit.link);
	wl_list_remove(&t->disable.link);
	wl_list_remove(&t->destroy.link);
	wl_list_remove(&t->link);
	free(t);
}

static void new_text_input(struct wl_listener *l, void *data) {
	struct wlr_text_input_v3 *ti = data;
	struct text_input *t = calloc(1, sizeof *t);
	if (!t) return;
	t->ti = ti;
	t->enable.notify = ti_enable;
	wl_signal_add(&ti->events.enable, &t->enable);
	t->commit.notify = ti_commit;
	wl_signal_add(&ti->events.commit, &t->commit);
	t->disable.notify = ti_disable;
	wl_signal_add(&ti->events.disable, &t->disable);
	t->destroy.notify = ti_destroy;
	wl_signal_add(&ti->events.destroy, &t->destroy);
	wl_list_insert(&R.text_inputs, &t->link);
	/* it belongs to the program that has the keyboard */
	struct wlr_surface *focused = R.server->seat->keyboard_state.focused_surface;
	if (focused && client_of_surface(focused) == wl_resource_get_client(ti->resource))
		wlr_text_input_v3_send_enter(ti, focused);
}

/* keyboard focus moved: the text inputs follow it */
static void focus_change(struct wl_listener *l, void *data) {
	struct wlr_seat_keyboard_focus_change_event *ev = data;
	struct text_input *t;
	struct text_input *was_active = active_text_input();
	wl_list_for_each(t, &R.text_inputs, link) {
		if (t->ti->focused_surface && t->ti->focused_surface != ev->new_surface) {
			if (t == was_active && R.im) {
				wlr_input_method_v2_send_deactivate(R.im);
				wlr_input_method_v2_send_done(R.im);
			}
			wlr_text_input_v3_send_leave(t->ti);
		}
		if (ev->new_surface && !t->ti->focused_surface &&
				client_of_surface(ev->new_surface) == wl_resource_get_client(t->ti->resource))
			wlr_text_input_v3_send_enter(t->ti, ev->new_surface);
	}
}

/* ---- keyboard ------------------------------------------------------------- */

struct wlr_input_method_keyboard_grab_v2 *im_keyboard_grab(struct wlr_keyboard *kb) {
	if (!R.grab || !R.im) return NULL;
	/* the input method types through its own virtual keyboard: never back to it */
	struct wlr_virtual_keyboard_v1 *vk = wlr_input_device_get_virtual_keyboard(&kb->base);
	if (vk && wl_resource_get_client(vk->resource) == wl_resource_get_client(R.im->resource)) return NULL;
	return R.grab;
}

void im_init(struct flwm_server *server) {
	R.server = server;
	wl_list_init(&R.text_inputs);
	wl_list_init(&R.popups);
	R.ti_mgr = wlr_text_input_manager_v3_create(server->wl_display);
	R.im_mgr = wlr_input_method_manager_v2_create(server->wl_display);
	R.new_text_input.notify = new_text_input;
	R.new_input_method.notify = new_input_method;
#if FLWM_WLR_AT_LEAST(0, 20)
	wl_signal_add(&R.ti_mgr->events.new_text_input, &R.new_text_input);
	wl_signal_add(&R.im_mgr->events.new_input_method, &R.new_input_method);
#else
	wl_signal_add(&R.ti_mgr->events.text_input, &R.new_text_input);
	wl_signal_add(&R.im_mgr->events.input_method, &R.new_input_method);
#endif
	R.focus_change.notify = focus_change;
	wl_signal_add(&server->seat->keyboard_state.events.focus_change, &R.focus_change);
}

void im_finish(void) {
	wl_list_remove(&R.new_text_input.link);
	wl_list_remove(&R.new_input_method.link);
	wl_list_remove(&R.focus_change.link);
}
