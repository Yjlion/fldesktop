/* fl-wm: the smaller protocols.
 *
 *  - tearing-control: a fullscreen game that asks for it gets async page flips
 *  - content-type: noted (video and games keep the screen on, see lock.c)
 *  - security-context-v1: sandboxed clients (Flatpak) do not see the
 *    privileged globals (screen capture, the clipboard manager, input
 *    injection, layer shell, ...)
 *  - ext-data-control (wlroots 0.19+), xdg-foreign v1 and v2,
 *    alpha-modifier, color-management (0.20+)
 *  - drm-lease: non-desktop outputs (VR headsets) are offered for leasing
 *  - tablet-v2: pen tablets; programs that know tablets get pressure and
 *    tilt, the others a pointer
 */
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/backend.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_drm_lease_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_switch.h>
#include <wlr/types/wlr_tablet_tool.h>
#include <wlr/types/wlr_tablet_v2.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/types/wlr_xdg_foreign_registry.h>
#include <wlr/types/wlr_xdg_foreign_v1.h>
#include <wlr/types/wlr_xdg_foreign_v2.h>
#include <wlr/util/log.h>

#include "compat.h"
#include "server.h"

#if FLWM_WLR_AT_LEAST(0, 19)
#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#endif

static struct {
	struct flwm_server *server;
	struct wlr_security_context_manager_v1 *security;
	struct wlr_tearing_control_manager_v1 *tearing;
	struct wlr_content_type_manager_v1 *content_type;
	struct wlr_drm_lease_v1_manager *drm_lease;
	struct wlr_tablet_manager_v2 *tablets;
	struct wl_list tablets_list; /* struct flwm_tablet.link */
	struct wl_listener tool_axis, tool_proximity, tool_tip, tool_button;
} P;

/* ---- security context -------------------------------------------------------------- */

static const char *const privileged[] = {
	"zwlr_screencopy_manager_v1", "zwlr_export_dmabuf_manager_v1", "zwlr_data_control_manager_v1",
	"ext_data_control_manager_v1", "zwlr_foreign_toplevel_manager_v1", "ext_foreign_toplevel_list_v1",
	"zwlr_layer_shell_v1", "zwp_virtual_keyboard_manager_v1", "zwlr_virtual_pointer_manager_v1",
	"zwp_input_method_manager_v2", "zwlr_gamma_control_manager_v1", "zwlr_output_manager_v1",
	"zwlr_output_power_manager_v1", "ext_session_lock_manager_v1", "ext_image_copy_capture_manager_v1",
	"ext_output_image_capture_source_manager_v1", "ext_foreign_toplevel_image_capture_source_manager_v1",
	"ext_workspace_manager_v1", "wp_security_context_manager_v1", "wp_drm_lease_device_v1",
	"zwp_keyboard_shortcuts_inhibit_manager_v1",
};

static bool global_filter(const struct wl_client *client, const struct wl_global *global, void *data) {
	if (!P.security) return true;
	const struct wlr_security_context_v1_state *ctx = wlr_security_context_manager_v1_lookup_client(P.security, client);
	if (!ctx) return true; /* not sandboxed */
	const char *name = wl_global_get_interface(global)->name;
	for (size_t i = 0; i < sizeof privileged / sizeof privileged[0]; i++)
		if (!strcmp(name, privileged[i])) return false;
	return true;
}

bool protocols_client_sandboxed(struct wl_client *client) {
	return P.security && wlr_security_context_manager_v1_lookup_client(P.security, client);
}

/* ---- tearing ------------------------------------------------------------------------- */

bool protocols_wants_tearing(struct wlr_surface *surface) {
	if (!P.tearing || !surface) return false;
	return wlr_tearing_control_manager_v1_surface_hint_from_surface(P.tearing, surface) ==
		WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC;
}

bool protocols_is_video_or_game(struct wlr_surface *surface) {
	if (!P.content_type || !surface) return false;
	enum wp_content_type_v1_type t = wlr_surface_get_content_type_v1(P.content_type, surface);
	return t == WP_CONTENT_TYPE_V1_TYPE_VIDEO || t == WP_CONTENT_TYPE_V1_TYPE_GAME;
}

/* ---- drm lease ----------------------------------------------------------------------- */

bool protocols_offer_lease(struct wlr_output *output) {
	if (!P.drm_lease || !output->non_desktop) return false;
	bool ok = wlr_drm_lease_v1_manager_offer_output(P.drm_lease, output);
	wlr_log(WLR_INFO, "output %s is a non-desktop display: %s", output->name,
		ok ? "offered for leasing" : "not leasable");
	return true;
}

static void drm_lease_request(struct wl_listener *l, void *data) {
	struct wlr_drm_lease_request_v1 *req = data;
	struct wlr_drm_lease_v1 *lease = wlr_drm_lease_request_v1_grant(req);
	if (!lease) wlr_drm_lease_request_v1_reject(req);
}
static struct wl_listener drm_lease_request_l = {.notify = drm_lease_request};

/* ---- tablets ------------------------------------------------------------------------- */

struct flwm_tablet {
	struct wlr_input_device *device;
	struct wlr_tablet_v2_tablet *v2;
	struct wl_list link;
	struct wl_listener destroy;
};

struct flwm_tool {
	struct wlr_tablet_v2_tablet_tool *v2;
	struct wlr_surface *surface; /* the surface it is in proximity of (v2), or NULL: a pointer */
	bool down;
	struct wl_listener destroy;
};

static void tablet_destroy(struct wl_listener *l, void *data) {
	struct flwm_tablet *t = wl_container_of(l, t, destroy);
	wl_list_remove(&t->link);
	wl_list_remove(&t->destroy.link);
	free(t);
}

void protocols_new_tablet(struct wlr_input_device *device) {
	if (!P.tablets) return;
	struct flwm_tablet *t = calloc(1, sizeof *t);
	t->device = device;
	t->v2 = wlr_tablet_create(P.tablets, P.server->seat, device);
	t->destroy.notify = tablet_destroy;
	wl_signal_add(&device->events.destroy, &t->destroy);
	wl_list_insert(&P.tablets_list, &t->link);
}

static struct flwm_tablet *tablet_of(struct wlr_tablet *tablet) {
	struct flwm_tablet *t;
	wl_list_for_each(t, &P.tablets_list, link)
		if (t->device == &tablet->base) return t;
	return NULL;
}

static void tool_destroy(struct wl_listener *l, void *data) {
	struct flwm_tool *t = wl_container_of(l, t, destroy);
	struct wlr_tablet_tool *tool = data;
	tool->data = NULL;
	wl_list_remove(&t->destroy.link);
	free(t);
}

static struct flwm_tool *tool_of(struct wlr_tablet_tool *tool) {
	if (tool->data) return tool->data;
	struct flwm_tool *t = calloc(1, sizeof *t);
	t->v2 = wlr_tablet_tool_create(P.tablets, P.server->seat, tool);
	t->destroy.notify = tool_destroy;
	wl_signal_add(&tool->events.destroy, &t->destroy);
	tool->data = t;
	return t;
}

/* the cursor follows the pen; then: the surface under it, if it knows tablets */
static struct wlr_surface *pen_moved(struct wlr_tablet *tablet, double x, double y, bool has_x, bool has_y,
		uint32_t time, double *sx, double *sy) {
	struct flwm_server *server = P.server;
	wlr_cursor_warp_absolute(server->cursor, &tablet->base, has_x ? x : NAN, has_y ? y : NAN);
	struct wlr_surface *surface = NULL;
	input_desc_at(server, server->cursor->x, server->cursor->y, &surface, sx, sy);
	struct flwm_tablet *t = tablet_of(tablet);
#if FLWM_WLR_AT_LEAST(0, 19)
	if (surface && t && wlr_surface_accepts_tablet_v2(surface, t->v2)) return surface;
#else
	if (surface && t && wlr_surface_accepts_tablet_v2(t->v2, surface)) return surface; /* 0.18: tablet first */
#endif
	return NULL;
}

static void tool_axis(struct wl_listener *l, void *data) {
	struct wlr_tablet_tool_axis_event *ev = data;
	struct flwm_tool *tool = tool_of(ev->tool);
	struct flwm_tablet *tab = tablet_of(ev->tablet);
	lock_activity(P.server);
	double sx = 0, sy = 0;
	bool moved = ev->updated_axes & (WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y);
	struct wlr_surface *surface = tool->surface;
	if (moved) {
		surface = pen_moved(ev->tablet, ev->x, ev->y, ev->updated_axes & WLR_TABLET_TOOL_AXIS_X,
			ev->updated_axes & WLR_TABLET_TOOL_AXIS_Y, ev->time_msec, &sx, &sy);
		/* a pen held down keeps its surface */
		if (tool->down && tool->surface) surface = tool->surface;
	}
	if (surface != tool->surface && !tool->down) {
		if (tool->surface) wlr_send_tablet_v2_tablet_tool_proximity_out(tool->v2);
		if (surface && tab) wlr_send_tablet_v2_tablet_tool_proximity_in(tool->v2, tab->v2, surface);
		tool->surface = surface;
	}
	if (!tool->surface) {
		/* a pointer for programs without tablet support */
		if (moved) input_emulate_motion(P.server, ev->time_msec);
		return;
	}
	if (moved) {
		input_desc_at(P.server, P.server->cursor->x, P.server->cursor->y, &surface, &sx, &sy);
		wlr_send_tablet_v2_tablet_tool_motion(tool->v2, sx, sy);
	}
	if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_PRESSURE) wlr_send_tablet_v2_tablet_tool_pressure(tool->v2, ev->pressure);
	if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_DISTANCE) wlr_send_tablet_v2_tablet_tool_distance(tool->v2, ev->distance);
	if (ev->updated_axes & (WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y))
		wlr_send_tablet_v2_tablet_tool_tilt(tool->v2, ev->tilt_x, ev->tilt_y);
	if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_ROTATION) wlr_send_tablet_v2_tablet_tool_rotation(tool->v2, ev->rotation);
	if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_SLIDER) wlr_send_tablet_v2_tablet_tool_slider(tool->v2, ev->slider);
	if (ev->updated_axes & WLR_TABLET_TOOL_AXIS_WHEEL) wlr_send_tablet_v2_tablet_tool_wheel(tool->v2, ev->wheel_delta, 0);
}

static void tool_proximity(struct wl_listener *l, void *data) {
	struct wlr_tablet_tool_proximity_event *ev = data;
	struct flwm_tool *tool = tool_of(ev->tool);
	struct flwm_tablet *tab = tablet_of(ev->tablet);
	if (ev->state == WLR_TABLET_TOOL_PROXIMITY_OUT) {
		if (tool->surface) wlr_send_tablet_v2_tablet_tool_proximity_out(tool->v2);
		tool->surface = NULL;
		tool->down = false;
		return;
	}
	double sx, sy;
	struct wlr_surface *surface = pen_moved(ev->tablet, ev->x, ev->y, true, true, ev->time_msec, &sx, &sy);
	if (surface && tab) {
		wlr_send_tablet_v2_tablet_tool_proximity_in(tool->v2, tab->v2, surface);
		wlr_send_tablet_v2_tablet_tool_motion(tool->v2, sx, sy);
		tool->surface = surface;
	} else {
		input_emulate_motion(P.server, ev->time_msec);
	}
}

static void tool_tip(struct wl_listener *l, void *data) {
	struct wlr_tablet_tool_tip_event *ev = data;
	struct flwm_tool *tool = tool_of(ev->tool);
	bool down = ev->state == WLR_TABLET_TOOL_TIP_DOWN;
	lock_activity(P.server);
	if (tool->surface) {
		tool->down = down;
		if (down) {
			wlr_send_tablet_v2_tablet_tool_down(tool->v2);
			/* the window it touches gets the focus, as with a click */
			struct wlr_surface *s = NULL;
			double sx, sy;
			struct flwm_node_desc *desc = input_desc_at(P.server, P.server->cursor->x, P.server->cursor->y, &s, &sx, &sy);
			if (desc && desc->kind == FLWM_NODE_VIEW) view_focus(desc->ptr);
		} else {
			wlr_send_tablet_v2_tablet_tool_up(tool->v2);
		}
		return;
	}
	input_emulate_button(P.server, ev->time_msec, BTN_LEFT, down);
}

static void tool_button(struct wl_listener *l, void *data) {
	struct wlr_tablet_tool_button_event *ev = data;
	struct flwm_tool *tool = tool_of(ev->tool);
	if (tool->surface) {
		wlr_send_tablet_v2_tablet_tool_button(tool->v2, ev->button,
			ev->state == WLR_BUTTON_PRESSED ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED
							: ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
		return;
	}
	/* the pen's lower button is a right click */
	input_emulate_button(P.server, ev->time_msec, ev->button == BTN_STYLUS ? BTN_RIGHT : BTN_MIDDLE,
		ev->state == WLR_BUTTON_PRESSED);
}

/* ---- tablet mode --------------------------------------------------------------------- */

/* A convertible folded into a tablet (libinput's tablet mode switch): the
 * on-screen keyboard comes up when a text field is focused and goes when
 * none is ([behaviour] osk_in_tablet_mode). */
static bool tablet_mode;
static struct wl_event_source *osk_hide_timer;

static int osk_hide(void *data) {
	flwm_spawn(P.server, "fl-osk --hide");
	return 0;
}

void protocols_text_input(bool focused) {
	if (!tablet_mode || !P.server->config.osk_in_tablet_mode) return;
	if (!osk_hide_timer) osk_hide_timer = wl_event_loop_add_timer(P.server->event_loop, osk_hide, NULL);
	if (focused) {
		wl_event_source_timer_update(osk_hide_timer, 0);
		flwm_spawn(P.server, "fl-osk --show");
	} else {
		/* focus going from one field to the next: no flicker */
		wl_event_source_timer_update(osk_hide_timer, 400);
	}
}

bool protocols_tablet_mode(void) { return tablet_mode; }

struct flwm_switch {
	struct wl_listener toggle, destroy;
};

static void switch_toggle(struct wl_listener *l, void *data) {
	struct wlr_switch_toggle_event *ev = data;
	if (ev->switch_type != WLR_SWITCH_TYPE_TABLET_MODE) return;
	bool on = ev->switch_state == WLR_SWITCH_STATE_ON;
	if (on == tablet_mode) return;
	tablet_mode = on;
	wlr_log(WLR_INFO, "tablet mode %s", on ? "on" : "off");
	if (!on && P.server->config.osk_in_tablet_mode) flwm_spawn(P.server, "fl-osk --hide");
}

static void switch_destroy(struct wl_listener *l, void *data) {
	struct flwm_switch *s = wl_container_of(l, s, destroy);
	wl_list_remove(&s->toggle.link);
	wl_list_remove(&s->destroy.link);
	free(s);
}

void protocols_new_switch(struct wlr_input_device *device) {
	struct wlr_switch *sw = wlr_switch_from_input_device(device);
	struct flwm_switch *s = calloc(1, sizeof *s);
	s->toggle.notify = switch_toggle;
	wl_signal_add(&sw->events.toggle, &s->toggle);
	s->destroy.notify = switch_destroy;
	wl_signal_add(&device->events.destroy, &s->destroy);
}

/* ---- setup --------------------------------------------------------------------------- */

void protocols_init(struct flwm_server *server) {
	P.server = server;
	struct wl_display *d = server->wl_display;
	P.security = wlr_security_context_manager_v1_create(d);
	wl_display_set_global_filter(d, global_filter, NULL);
	P.tearing = wlr_tearing_control_manager_v1_create(d, 1);
	P.content_type = wlr_content_type_manager_v1_create(d, 1);
	struct wlr_xdg_foreign_registry *reg = wlr_xdg_foreign_registry_create(d);
	wlr_xdg_foreign_v1_create(d, reg);
	wlr_xdg_foreign_v2_create(d, reg);
	wlr_alpha_modifier_v1_create(d);
#if FLWM_WLR_AT_LEAST(0, 19)
	wlr_ext_data_control_manager_v1_create(d, 1);
#endif
#if FLWM_WLR_AT_LEAST(0, 20)
	/* colour management: the scene renders sRGB and BT.709-ish content and
	 * tags; clients may describe their surfaces parametrically */
	static const enum wp_color_manager_v1_render_intent intents[] = {WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL};
	static const enum wp_color_manager_v1_transfer_function tfs[] = {
		WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ,
		WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR};
	static const enum wp_color_manager_v1_primaries prims[] = {
		WP_COLOR_MANAGER_V1_PRIMARIES_SRGB, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020};
	struct wlr_color_manager_v1_options opts = {
		.features = {.parametric = true},
		.render_intents = intents, .render_intents_len = 1,
		.transfer_functions = tfs, .transfer_functions_len = sizeof tfs / sizeof tfs[0],
		.primaries = prims, .primaries_len = sizeof prims / sizeof prims[0],
	};
	if (!wlr_color_manager_v1_create(d, 1, &opts)) wlr_log(WLR_INFO, "no colour management");
#endif
	P.drm_lease = wlr_drm_lease_v1_manager_create(d, server->backend);
	if (P.drm_lease) wl_signal_add(&P.drm_lease->events.request, &drm_lease_request_l);
	P.tablets = wlr_tablet_v2_create(d);
	wl_list_init(&P.tablets_list);
	P.tool_axis.notify = tool_axis;
	wl_signal_add(&server->cursor->events.tablet_tool_axis, &P.tool_axis);
	P.tool_proximity.notify = tool_proximity;
	wl_signal_add(&server->cursor->events.tablet_tool_proximity, &P.tool_proximity);
	P.tool_tip.notify = tool_tip;
	wl_signal_add(&server->cursor->events.tablet_tool_tip, &P.tool_tip);
	P.tool_button.notify = tool_button;
	wl_signal_add(&server->cursor->events.tablet_tool_button, &P.tool_button);
}

void protocols_finish(struct flwm_server *server) {
	wl_list_remove(&P.tool_axis.link);
	wl_list_remove(&P.tool_proximity.link);
	wl_list_remove(&P.tool_tip.link);
	wl_list_remove(&P.tool_button.link);
	if (P.drm_lease) wl_list_remove(&drm_lease_request_l.link);
}
