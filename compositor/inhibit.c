/* keyboard-shortcuts-inhibit-unstable-v1: virtual machines, remote desktops
 * and games ask for the key bindings (Super, Alt+Tab) to go to them while
 * they have the keyboard focus. fl-wm grants it; Super+Escape takes the
 * bindings back for that window (and gives them again). VT switching and
 * Ctrl+Alt+BackSpace always work. */
#include <stdlib.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "server.h"

struct inhibitor {
	struct wlr_keyboard_shortcuts_inhibitor_v1 *in;
	bool escaped; /* the user took the bindings back with Super+Escape */
	struct wl_list link;
	struct wl_listener destroy;
};

static struct {
	struct flwm_server *server;
	struct wlr_keyboard_shortcuts_inhibit_manager_v1 *mgr;
	struct wl_list inhibitors; /* inhibitor.link */
	struct wl_listener new_inhibitor, focus_change;
} I;

static struct inhibitor *for_surface(struct wlr_surface *s) {
	if (!s) return NULL;
	struct inhibitor *in;
	wl_list_for_each(in, &I.inhibitors, link)
		if (in->in->surface == s) return in;
	return NULL;
}

static void set_active(struct inhibitor *in, bool on) {
	if (on == in->in->active) return;
	if (on) wlr_keyboard_shortcuts_inhibitor_v1_activate(in->in);
	else wlr_keyboard_shortcuts_inhibitor_v1_deactivate(in->in);
	wlr_log(WLR_INFO, "key bindings %s", on ? "go to the window" : "are fl-wm's");
}

static void inhibitor_destroy(struct wl_listener *l, void *data) {
	struct inhibitor *in = wl_container_of(l, in, destroy);
	wl_list_remove(&in->destroy.link);
	wl_list_remove(&in->link);
	free(in);
}

static void new_inhibitor(struct wl_listener *l, void *data) {
	struct wlr_keyboard_shortcuts_inhibitor_v1 *w = data;
	struct inhibitor *in = calloc(1, sizeof *in);
	if (!in) return;
	in->in = w;
	in->destroy.notify = inhibitor_destroy;
	wl_signal_add(&w->events.destroy, &in->destroy);
	wl_list_insert(&I.inhibitors, &in->link);
	if (w->surface == I.server->seat->keyboard_state.focused_surface) set_active(in, true);
}

static void focus_change(struct wl_listener *l, void *data) {
	struct wlr_seat_keyboard_focus_change_event *ev = data;
	struct inhibitor *in;
	wl_list_for_each(in, &I.inhibitors, link)
		set_active(in, in->in->surface == ev->new_surface && !in->escaped);
}

bool inhibit_shortcuts_active(struct flwm_server *server) {
	struct inhibitor *in = for_surface(server->seat->keyboard_state.focused_surface);
	return in && in->in->active;
}

bool inhibit_shortcuts_toggle(struct flwm_server *server) {
	struct inhibitor *in = for_surface(server->seat->keyboard_state.focused_surface);
	if (!in) return false;
	in->escaped = in->in->active;
	set_active(in, !in->escaped);
	return true;
}

void inhibit_init(struct flwm_server *server) {
	I.server = server;
	wl_list_init(&I.inhibitors);
	I.mgr = wlr_keyboard_shortcuts_inhibit_v1_create(server->wl_display);
	I.new_inhibitor.notify = new_inhibitor;
	wl_signal_add(&I.mgr->events.new_inhibitor, &I.new_inhibitor);
	I.focus_change.notify = focus_change;
	wl_signal_add(&server->seat->keyboard_state.events.focus_change, &I.focus_change);
}

void inhibit_finish(void) {
	wl_list_remove(&I.new_inhibitor.link);
	wl_list_remove(&I.focus_change.link);
}
