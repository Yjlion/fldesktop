/* Touchpad gestures.
 *
 * Swipes with [gestures] fingers (3 by default) belong to fl-wm: sideways
 * they switch workspaces, up and down they run [gestures] swipe_up and
 * swipe_down (the launcher and "show desktop" by default). Everything else
 * (pinches, holds, swipes with other finger counts) goes to the window under
 * the pointer through pointer-gestures-unstable-v1. */
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/util/log.h>

#include "gesture_decide.h"
#include "server.h"

static struct {
	struct flwm_server *server;
	struct wlr_pointer_gestures_v1 *gestures;
	struct wl_listener swipe_begin, swipe_update, swipe_end;
	struct wl_listener pinch_begin, pinch_update, pinch_end;
	struct wl_listener hold_begin, hold_end;
	bool ours; /* the current swipe is fl-wm's */
	double dx, dy;
} g;

static bool swipe_is_ours(uint32_t fingers) {
	struct flwm_server *s = g.server;
	return s->config.gestures && (int)fingers == s->config.gesture_fingers && !s->locked &&
		s->cursor_mode == FLWM_CURSOR_PASSTHROUGH;
}

static void run(const struct flwm_gesture_action *a) {
	if (a->type != FLWM_ACTION_NONE) input_run_action(g.server, a->type, a->arg);
}

static void swipe_begin(struct wl_listener *l, void *data) {
	struct wlr_pointer_swipe_begin_event *ev = data;
	lock_activity(g.server);
	g.ours = swipe_is_ours(ev->fingers);
	g.dx = g.dy = 0;
	if (!g.ours)
		wlr_pointer_gestures_v1_send_swipe_begin(g.gestures, g.server->seat, ev->time_msec, ev->fingers);
}

static void swipe_update(struct wl_listener *l, void *data) {
	struct wlr_pointer_swipe_update_event *ev = data;
	lock_activity(g.server);
	if (g.ours) {
		g.dx += ev->dx;
		g.dy += ev->dy;
		return;
	}
	wlr_pointer_gestures_v1_send_swipe_update(g.gestures, g.server->seat, ev->time_msec, ev->dx, ev->dy);
}

static void swipe_end(struct wl_listener *l, void *data) {
	struct wlr_pointer_swipe_end_event *ev = data;
	if (!g.ours) {
		wlr_pointer_gestures_v1_send_swipe_end(g.gestures, g.server->seat, ev->time_msec, ev->cancelled);
		return;
	}
	g.ours = false;
	struct flwm_config *c = &g.server->config;
	switch (flwm_swipe_decide(g.dx, g.dy, c->gesture_natural, ev->cancelled)) {
	case FLWM_SWIPE_WORKSPACE_NEXT:
		input_run_action(g.server, FLWM_ACTION_WORKSPACE_NEXT, NULL);
		break;
	case FLWM_SWIPE_WORKSPACE_PREV:
		input_run_action(g.server, FLWM_ACTION_WORKSPACE_PREV, NULL);
		break;
	case FLWM_SWIPE_UP:
		run(&c->gesture_up);
		break;
	case FLWM_SWIPE_DOWN:
		run(&c->gesture_down);
		break;
	case FLWM_SWIPE_NONE:
		break;
	}
}

static void pinch_begin(struct wl_listener *l, void *data) {
	struct wlr_pointer_pinch_begin_event *ev = data;
	lock_activity(g.server);
	wlr_pointer_gestures_v1_send_pinch_begin(g.gestures, g.server->seat, ev->time_msec, ev->fingers);
}

static void pinch_update(struct wl_listener *l, void *data) {
	struct wlr_pointer_pinch_update_event *ev = data;
	lock_activity(g.server);
	wlr_pointer_gestures_v1_send_pinch_update(g.gestures, g.server->seat, ev->time_msec, ev->dx, ev->dy,
		ev->scale, ev->rotation);
}

static void pinch_end(struct wl_listener *l, void *data) {
	struct wlr_pointer_pinch_end_event *ev = data;
	wlr_pointer_gestures_v1_send_pinch_end(g.gestures, g.server->seat, ev->time_msec, ev->cancelled);
}

static void hold_begin(struct wl_listener *l, void *data) {
	struct wlr_pointer_hold_begin_event *ev = data;
	lock_activity(g.server);
	wlr_pointer_gestures_v1_send_hold_begin(g.gestures, g.server->seat, ev->time_msec, ev->fingers);
}

static void hold_end(struct wl_listener *l, void *data) {
	struct wlr_pointer_hold_end_event *ev = data;
	wlr_pointer_gestures_v1_send_hold_end(g.gestures, g.server->seat, ev->time_msec, ev->cancelled);
}

void gesture_init(struct flwm_server *server) {
	g.server = server;
	g.gestures = wlr_pointer_gestures_v1_create(server->wl_display);
	struct wlr_cursor *c = server->cursor;
#define LISTEN(name) \
	g.name.notify = name; \
	wl_signal_add(&c->events.name, &g.name)
	LISTEN(swipe_begin);
	LISTEN(swipe_update);
	LISTEN(swipe_end);
	LISTEN(pinch_begin);
	LISTEN(pinch_update);
	LISTEN(pinch_end);
	LISTEN(hold_begin);
	LISTEN(hold_end);
#undef LISTEN
}

void gesture_finish(void) {
	struct wl_listener *ls[] = {
		&g.swipe_begin, &g.swipe_update, &g.swipe_end, &g.pinch_begin, &g.pinch_update,
		&g.pinch_end, &g.hold_begin, &g.hold_end,
	};
	for (size_t i = 0; i < sizeof ls / sizeof ls[0]; i++) wl_list_remove(&ls[i]->link);
}
