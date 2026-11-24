/* fl-wm input: keyboards, pointer, key bindings, cursor shapes, DnD */
#define _POSIX_C_SOURCE 200809L
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <ctype.h>
#include <unistd.h>
#include <string.h>
#include <wlr/backend/libinput.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#include <wlr/types/wlr_input_method_v2.h>

#include "server.h"

#define MOD_MASK (WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT | WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT)
#define DOUBLE_CLICK_MS 400

/* ---- hit testing --------------------------------------------------------- */

struct flwm_node_desc *input_desc_at(struct flwm_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	*surface = NULL;
	struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
	if (!node) return NULL;
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
		struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
		if (ss) *surface = ss->surface;
	}
	/* walk up to the first node that carries a descriptor */
	struct wlr_scene_node *n = node;
	while (n && !n->data) n = n->parent ? &n->parent->node : NULL;
	return n ? n->data : NULL;
}

/* ---- actions ------------------------------------------------------------- */

static struct flwm_view *focused_normal(struct flwm_server *server) {
	struct flwm_view *v = view_focused(server);
	return (v && v->role == FLWM_ROLE_NORMAL) ? v : NULL;
}

static void cycle(struct flwm_server *server, bool backwards) {
	/* candidates: mapped normal views in focus order */
	struct flwm_view *list[256];
	int n = 0;
	struct flwm_view *v;
	wl_list_for_each(v, &server->views, link)
		if (view_is_cyclable(v) && n < 256) list[n++] = v;
	if (n < 2) return;
	int cur = 0;
	struct flwm_view *from = server->cycle_current ? server->cycle_current : focused_normal(server);
	for (int i = 0; i < n; i++)
		if (list[i] == from) cur = i;
	int next = backwards ? (cur - 1 + n) % n : (cur + 1) % n;
	if (server->config.switcher) {
		/* the switcher panel: windows are raised only when Alt is released */
		if (switcher_step(server, backwards ? -1 : 1)) server->cycling = true;
		return;
	}
	server->cycling = true;
	server->cycle_current = list[next];
	view_focus(list[next]);
}

static void finish_cycle(struct flwm_server *server) {
	if (!server->cycling) return;
	if (switcher_active(server)) {
		switcher_finish(server, true);
		return;
	}
	server->cycling = false;
	struct flwm_view *v = server->cycle_current;
	server->cycle_current = NULL;
	if (v && v->mapped) {
		wl_list_remove(&v->link);
		wl_list_insert(&server->views, &v->link);
	}
}

static void reload_config(struct flwm_server *server) {
	char *path = server->config.path ? strdup(server->config.path) : config_default_path();
	config_free(&server->config);
	config_init_defaults(&server->config);
	if (!config_load(&server->config, path)) wlr_log(WLR_INFO, "reload: no config at %s", path);
	config_load_global_shortcuts(&server->config);
	free(path);
	input_apply_config(server);
	output_apply_config(server);
	struct flwm_view *v;
	wl_list_for_each(v, &server->views, link) {
		if (!v->mapped) continue;
		if (v->decor.tree) {
			decor_destroy(v);
		}
		decor_update(v);
	}
	workspace_apply_config(server);
	arrange_all(server);
	wlr_log(WLR_INFO, "configuration reloaded");
}

/* The resident launcher's pid, from the pid file fl-launcher --daemon writes
 * ("<pid> daemon"), when that process is fl-launcher; it gets SIGUSR1. */
static bool signal_resident_launcher(void) {
	const char *rt = getenv("XDG_RUNTIME_DIR");
	char path[512], buf[64] = "";
	snprintf(path, sizeof path, "%s/fl-launcher-%u.pid", rt ? rt : "/tmp", (unsigned)getuid());
	FILE *f = fopen(path, "r");
	if (!f) return false;
	size_t n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	if (!strstr(buf, "daemon")) return false;
	int pid = atoi(buf);
	if (pid <= 1) return false;
	char comm_path[64], comm[32] = "";
	snprintf(comm_path, sizeof comm_path, "/proc/%d/comm", pid);
	f = fopen(comm_path, "r");
	if (!f) return false;
	if (!fgets(comm, sizeof comm, f)) comm[0] = 0;
	fclose(f);
	comm[strcspn(comm, "\n")] = 0;
	if (strcmp(comm, "fl-launcher")) return false;
	return kill(pid, SIGUSR1) == 0;
}

bool input_run_action(struct flwm_server *server, enum flwm_action_type action, const char *arg) {
	struct flwm_view *v = focused_normal(server);
	switch (action) {
	case FLWM_ACTION_EXEC:
		if (arg) flwm_spawn(server, arg);
		return true;
	case FLWM_ACTION_CLOSE:
		if (v) view_close(v);
		return true;
	case FLWM_ACTION_MAXIMIZE:
		if (v) view_set_maximized(v, !v->maximized);
		return true;
	case FLWM_ACTION_MINIMIZE:
		if (v) view_set_minimized(v, true);
		return true;
	case FLWM_ACTION_FULLSCREEN:
		if (v) view_set_fullscreen(v, !v->fullscreen);
		return true;
	case FLWM_ACTION_SNAP_LEFT:
	case FLWM_ACTION_SNAP_RIGHT:
	case FLWM_ACTION_SNAP_THIRD_LEFT:
	case FLWM_ACTION_SNAP_THIRD_RIGHT: {
		if (!v) return true;
		enum flwm_snap_dir d = action == FLWM_ACTION_SNAP_LEFT || action == FLWM_ACTION_SNAP_THIRD_LEFT
			? FLWM_SNAP_DIR_LEFT : FLWM_SNAP_DIR_RIGHT;
		bool third = action == FLWM_ACTION_SNAP_THIRD_LEFT || action == FLWM_ACTION_SNAP_THIRD_RIGHT;
		enum flwm_snap n = third ? flwm_snap_next_third(view_snap_state(v), d) : flwm_snap_next(view_snap_state(v), d);
		view_set_snap(v, n);
		/* the rest of the screen: one of the other windows */
		switcher_snap_assist(server, v);
		return true;
	}
	case FLWM_ACTION_OVERVIEW:
		switcher_overview(server);
		return true;
	case FLWM_ACTION_SNAP_UP:
		if (v) view_set_snap(v, flwm_snap_next(view_snap_state(v), FLWM_SNAP_DIR_UP));
		return true;
	case FLWM_ACTION_SNAP_DOWN:
		if (v) view_set_snap(v, flwm_snap_next(view_snap_state(v), FLWM_SNAP_DIR_DOWN));
		return true;
	case FLWM_ACTION_RESIZE_MODE:
		if (v && !v->maximized && !v->fullscreen) {
			view_get_geometry(v, &server->resize_orig);
			server->resize_w = server->resize_orig.width;
			server->resize_h = server->resize_orig.height;
			server->resize_view = v;
			wlr_log(WLR_INFO, "resize mode: arrows resize, Enter keeps, Escape undoes");
		}
		return true;
	case FLWM_ACTION_CYCLE:
		cycle(server, false);
		return true;
	case FLWM_ACTION_CYCLE_BACK:
		cycle(server, true);
		return true;
	case FLWM_ACTION_LAUNCHER:
		/* the resident fl-launcher (--daemon) shows or hides on SIGUSR1, at
		 * once; otherwise fl-launcher toggles: a second instance closes the
		 * running one (FLTK ignores xdg_toplevel.close for borderless windows) */
		if ((!server->config.launcher_cmd || !strcmp(server->config.launcher_cmd, "fl-launcher")) &&
		    signal_resident_launcher())
			return true;
		flwm_spawn(server, server->config.launcher_cmd ? server->config.launcher_cmd : "fl-launcher");
		return true;
	case FLWM_ACTION_QUIT:
		wl_display_terminate(server->wl_display);
		return true;
	case FLWM_ACTION_RELOAD:
		reload_config(server);
		return true;
	case FLWM_ACTION_LOCK:
		lock_request(server);
		return true;
	case FLWM_ACTION_SHOW_DESKTOP:
		views_show_desktop(server);
		return true;
	case FLWM_ACTION_WORKSPACE:
		/* a number past the configured count does nothing (but is still ours) */
		if (arg) workspace_switch(server, atoi(arg) - 1);
		return true;
	case FLWM_ACTION_WORKSPACE_NEXT:
	case FLWM_ACTION_WORKSPACE_PREV: {
		int n = server->config.workspaces < 1 ? 1 : server->config.workspaces;
		int d = action == FLWM_ACTION_WORKSPACE_NEXT ? 1 : -1;
		workspace_switch(server, (server->cur_workspace + d + n) % n);
		return true;
	}
	case FLWM_ACTION_MOVE_TO_WORKSPACE:
		if (v && arg) view_move_to_workspace(v, atoi(arg) - 1);
		return true;
	case FLWM_ACTION_MOVE_TO_WORKSPACE_NEXT:
	case FLWM_ACTION_MOVE_TO_WORKSPACE_PREV: {
		/* the window goes along: move it, then follow it */
		int n = server->config.workspaces < 1 ? 1 : server->config.workspaces;
		int d = action == FLWM_ACTION_MOVE_TO_WORKSPACE_NEXT ? 1 : -1;
		int to = (server->cur_workspace + d + n) % n;
		if (v && to != server->cur_workspace) {
			v->sticky = false;
			v->workspace = to;
			workspace_switch(server, to);
			view_focus(v);
		}
		return true;
	}
	case FLWM_ACTION_STICKY:
		if (v) view_set_sticky(v, !v->sticky);
		return true;
	case FLWM_ACTION_ZOOM_IN:
		zoom_step(server, 1);
		return true;
	case FLWM_ACTION_ZOOM_OUT:
		zoom_step(server, -1);
		return true;
	case FLWM_ACTION_ZOOM_RESET:
		zoom_step(server, 0);
		return true;
	case FLWM_ACTION_PORTAL_SHORTCUT: {
		/* the portal tells the application (ids are plain: see config.c) */
		const char *sep = arg ? strchr(arg, '\x1f') : NULL;
		if (!sep) return true;
		char portal[4200], cmd[4800];
		snprintf(portal, sizeof portal, "%s/xdg-desktop-portal-fl", server->exe_dir);
		if (access(portal, X_OK) != 0) snprintf(portal, sizeof portal, "%s", FLD_PORTAL_EXEC);
		snprintf(cmd, sizeof cmd, "'%s' --activate-shortcut '%.*s' '%s'", portal, (int)(sep - arg), arg, sep + 1);
		flwm_spawn(server, cmd);
		return true;
	}
	default:
		return false;
	}
}

/* ---- keyboard ------------------------------------------------------------ */

static void update_capabilities(struct flwm_server *server);
static void osd_lock_keys(struct flwm_keyboard *keyboard);

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
	struct flwm_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
	struct flwm_server *server = keyboard->server;
	if (server->cycling) {
		uint32_t mods = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
		if (!(mods & server->cycle_modifier)) finish_cycle(server);
	}
	if (!keyboard->wlr_keyboard->keymap) return;
	osd_lock_keys(keyboard);
	struct wlr_input_method_keyboard_grab_v2 *grab = im_keyboard_grab(keyboard->wlr_keyboard);
	if (grab) {
		wlr_input_method_keyboard_grab_v2_set_keyboard(grab, keyboard->wlr_keyboard);
		wlr_input_method_keyboard_grab_v2_send_modifiers(grab, &keyboard->wlr_keyboard->modifiers);
		return;
	}
	wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(server->seat, &keyboard->wlr_keyboard->modifiers);
}

/* keys that work even while the screen is locked or a layer surface (the
 * launcher) has the keyboard: volume, brightness, media */
static bool is_media_key(xkb_keysym_t sym) {
	switch (sym) {
	case XKB_KEY_XF86AudioRaiseVolume: case XKB_KEY_XF86AudioLowerVolume: case XKB_KEY_XF86AudioMute:
	case XKB_KEY_XF86AudioMicMute: case XKB_KEY_XF86MonBrightnessUp: case XKB_KEY_XF86MonBrightnessDown:
	case XKB_KEY_XF86KbdBrightnessUp: case XKB_KEY_XF86KbdBrightnessDown: case XKB_KEY_XF86KbdLightOnOff:
	case XKB_KEY_XF86AudioPlay: case XKB_KEY_XF86AudioPause: case XKB_KEY_XF86AudioStop:
	case XKB_KEY_XF86AudioNext: case XKB_KEY_XF86AudioPrev:
		return true;
	default:
		return false;
	}
}

/* Caps Lock, Num Lock and layout changes go to the panel's on-screen display */
static void osd_lock_keys(struct flwm_keyboard *keyboard) {
	struct wlr_keyboard *kb = keyboard->wlr_keyboard;
	if (!kb->xkb_state || !kb->keymap) return;
	int caps = xkb_state_mod_name_is_active(kb->xkb_state, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED) > 0;
	int num = xkb_state_mod_name_is_active(kb->xkb_state, XKB_MOD_NAME_NUM, XKB_STATE_MODS_LOCKED) > 0;
	long group = (long)kb->modifiers.group;
	bool first = keyboard->osd_caps < 0;
	char cmd[256] = "";
	if (!first && caps != keyboard->osd_caps)
		snprintf(cmd, sizeof cmd, "fl-panel --action caps-lock %s", caps ? "on" : "off");
	else if (!first && num != keyboard->osd_num)
		snprintf(cmd, sizeof cmd, "fl-panel --action num-lock %s", num ? "on" : "off");
	else if (!first && group != keyboard->osd_group && xkb_keymap_num_layouts(kb->keymap) > 1) {
		const char *name = xkb_keymap_layout_get_name(kb->keymap, (xkb_layout_index_t)group);
		/* quoted for the shell: only harmless characters */
		char safe[128];
		size_t n = 0;
		for (const char *s = name ? name : "?"; *s && n < sizeof safe - 1; s++)
			if ((unsigned char)*s >= 0x80 || isalnum((unsigned char)*s) || strchr(" ()-_.,", *s)) safe[n++] = *s;
		safe[n] = 0;
		snprintf(cmd, sizeof cmd, "fl-panel --action layout '%s'", safe);
	}
	keyboard->osd_caps = caps;
	keyboard->osd_num = num;
	keyboard->osd_group = group;
	if (*cmd && !keyboard->server->locked) flwm_spawn(keyboard->server, cmd);
}

static bool handle_binding(struct flwm_server *server, uint32_t mods, xkb_keysym_t sym) {
	sym = xkb_keysym_to_lower(sym);
	for (int i = 0; i < server->config.nbindings; i++) {
		struct flwm_binding *b = &server->config.bindings[i];
		if (b->sym != sym || b->modifiers != (mods & MOD_MASK)) continue;
		if (b->action == FLWM_ACTION_CYCLE || b->action == FLWM_ACTION_CYCLE_BACK)
			server->cycle_modifier = b->modifiers & ~WLR_MODIFIER_SHIFT;
		return input_run_action(server, b->action, b->arg);
	}
	return false;
}

static void handle_key(struct flwm_keyboard *keyboard, struct wlr_keyboard_key_event *event) {
	struct flwm_server *server = keyboard->server;
	struct wlr_seat *seat = server->seat;
	uint32_t keycode = event->keycode + 8;
	lock_activity(server);
	bool handled = false;

	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		struct xkb_state *state = keyboard->wlr_keyboard->xkb_state;
		const xkb_keysym_t *syms;
		/* VT switching (Ctrl+Alt+Fn yields XF86Switch_VT_n) */
		int nsyms = xkb_state_key_get_syms(state, keycode, &syms);
		for (int i = 0; i < nsyms; i++) {
			if (syms[i] >= XKB_KEY_XF86Switch_VT_1 && syms[i] <= XKB_KEY_XF86Switch_VT_12) {
				if (server->session)
					wlr_session_change_vt(server->session, syms[i] - XKB_KEY_XF86Switch_VT_1 + 1);
				handled = true;
			}
		}
		/* bindings match on the unshifted keysym of the active layout */
		uint32_t mods = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
		/* keyboard resize mode (Super+R) has the keyboard until Enter or Escape */
		if (!handled && server->resize_view) {
			xkb_layout_index_t layout = xkb_state_key_get_layout(state, keycode);
			nsyms = xkb_keymap_key_get_syms_by_level(keyboard->wlr_keyboard->keymap, keycode, layout, 0, &syms);
			xkb_keysym_t sym = nsyms > 0 ? syms[0] : XKB_KEY_NoSymbol;
			int step = (mods & WLR_MODIFIER_SHIFT) ? 8 : 32;
			struct flwm_view *rv = server->resize_view;
			int *w = &server->resize_w, *h = &server->resize_h;
			switch (sym) {
			case XKB_KEY_Left: *w = *w - step < 64 ? 64 : *w - step; break;
			case XKB_KEY_Right: *w += step; break;
			case XKB_KEY_Up: *h = *h - step < 64 ? 64 : *h - step; break;
			case XKB_KEY_Down: *h += step; break;
			case XKB_KEY_Escape:
				view_set_geometry(rv, &server->resize_orig);
				/* fall through */
			case XKB_KEY_Return: case XKB_KEY_KP_Enter:
				server->resize_view = NULL;
				wlr_log(WLR_INFO, "resize mode: done");
				break;
			default: break;
			}
			if (server->resize_view && (sym == XKB_KEY_Left || sym == XKB_KEY_Right || sym == XKB_KEY_Up ||
					sym == XKB_KEY_Down))
				view_resize_to(rv, *w, *h);
			/* everything but modifiers stays here */
			if (!(sym >= XKB_KEY_Shift_L && sym <= XKB_KEY_Hyper_R)) handled = true;
		}
		/* snap assist and the overview close on Super+anything (snapping on
		 * with Super+Left again, Super+Tab closing the overview) */
		if (!handled && switcher_active(server) && server->switcher.mode != FLWM_SW_SWITCH &&
				(mods & WLR_MODIFIER_LOGO)) {
			xkb_layout_index_t layout = xkb_state_key_get_layout(state, keycode);
			nsyms = xkb_keymap_key_get_syms_by_level(keyboard->wlr_keyboard->keymap, keycode, layout, 0, &syms);
			bool tab = nsyms > 0 && syms[0] == XKB_KEY_Tab;
			bool overview = server->switcher.mode == FLWM_SW_OVERVIEW;
			if (!(nsyms > 0 && syms[0] >= XKB_KEY_Shift_L && syms[0] <= XKB_KEY_Hyper_R)) {
				switcher_finish(server, false);
				if (overview && tab) handled = true;
			}
		}
		/* the switcher has the keyboard while it is open */
		if (!handled && switcher_active(server)) {
			xkb_layout_index_t layout = xkb_state_key_get_layout(state, keycode);
			nsyms = xkb_keymap_key_get_syms_by_level(keyboard->wlr_keyboard->keymap, keycode, layout, 0, &syms);
			xkb_keysym_t sym = nsyms > 0 ? syms[0] : XKB_KEY_NoSymbol;
			bool shift = mods & WLR_MODIFIER_SHIFT;
			switch (sym) {
			case XKB_KEY_Escape: switcher_finish(server, false); break;
			case XKB_KEY_Return: case XKB_KEY_KP_Enter: case XKB_KEY_space: switcher_finish(server, true); break;
			case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: switcher_step(server, shift ? -1 : 1); break;
			case XKB_KEY_Right: switcher_step(server, 1); break;
			case XKB_KEY_Left: switcher_step(server, -1); break;
			case XKB_KEY_Down: switcher_step_row(server, 1); break;
			case XKB_KEY_Up: switcher_step_row(server, -1); break;
			default: break;
			}
			/* swallow everything but modifiers (their release ends the switcher) */
			if (!(sym >= XKB_KEY_Shift_L && sym <= XKB_KEY_Hyper_R) && sym != XKB_KEY_ISO_Level3_Shift) handled = true;
		}
		/* while locked no key binding works except the lock's own keys */
		if (!handled && !server->focused_layer && !server->locked) {
			xkb_layout_index_t layout = xkb_state_key_get_layout(state, keycode);
			nsyms = xkb_keymap_key_get_syms_by_level(keyboard->wlr_keyboard->keymap,
				keycode, layout, 0, &syms);
			/* Super+Escape takes the bindings back from (or gives them again to)
			 * a window that inhibits them (a virtual machine, remote desktop) */
			for (int i = 0; i < nsyms && !handled; i++)
				if (syms[i] == XKB_KEY_Escape && (mods & MOD_MASK) == WLR_MODIFIER_LOGO)
					handled = inhibit_shortcuts_toggle(server);
			if (!inhibit_shortcuts_active(server))
				for (int i = 0; i < nsyms && !handled; i++) handled = handle_binding(server, mods, syms[i]);
		} else if (!handled) {
			/* locked, or the launcher has the keyboard: the media keys still work */
			xkb_layout_index_t layout = xkb_state_key_get_layout(state, keycode);
			nsyms = xkb_keymap_key_get_syms_by_level(keyboard->wlr_keyboard->keymap, keycode, layout, 0, &syms);
			for (int i = 0; i < nsyms && !handled; i++)
				if (is_media_key(syms[i])) handled = handle_binding(server, mods, syms[i]);
		}
		/* Ctrl+Alt+BackSpace and friends even when a layer surface owns the keyboard */
		if (!handled && server->focused_layer && (mods & MOD_MASK) == (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) {
			nsyms = xkb_state_key_get_syms(state, keycode, &syms);
			for (int i = 0; i < nsyms; i++)
				if (syms[i] == XKB_KEY_BackSpace || syms[i] == XKB_KEY_Terminate_Server) {
					wl_display_terminate(server->wl_display);
					handled = true;
				}
		}
	}
	/* an input method (fcitx5, IBus) composing text gets the keys first */
	struct wlr_input_method_keyboard_grab_v2 *grab =
		!handled && keyboard->wlr_keyboard->keymap ? im_keyboard_grab(keyboard->wlr_keyboard) : NULL;
	if (grab) {
		wlr_input_method_keyboard_grab_v2_set_keyboard(grab, keyboard->wlr_keyboard);
		wlr_input_method_keyboard_grab_v2_send_key(grab, event->time_msec, event->keycode, event->state);
		return;
	}
	if (!handled && keyboard->wlr_keyboard->keymap) {
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
	}
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
	struct flwm_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct wlr_keyboard_key_event *event = data;
	/* bounce keys drop the key; sticky keys update the modifiers after it */
	if (a11y_key(keyboard->server, keyboard->wlr_keyboard, event)) return;
	handle_key(keyboard, event);
	a11y_key_done(keyboard->server, keyboard->wlr_keyboard, event);
}

/* A virtual keyboard got its keymap: use it right away if no keyboard is active */
static void keyboard_handle_keymap(struct wl_listener *listener, void *data) {
	struct flwm_keyboard *keyboard = wl_container_of(listener, keyboard, keymap);
	struct flwm_server *server = keyboard->server;
	struct wlr_keyboard *active = wlr_seat_get_keyboard(server->seat);
	if ((!active || active == &server->fallback_keyboard) && keyboard->wlr_keyboard->keymap)
		wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	struct flwm_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
	struct flwm_server *server = keyboard->server;
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->keymap.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	/* If the active keyboard goes away (e.g. a wtype or on-screen keyboard
	 * exits), switch to another one. Leaving the seat without a keyboard makes
	 * clients that start now receive keyboard focus without any keymap, which
	 * crashes FLTK. */
	struct wlr_keyboard *active = wlr_seat_get_keyboard(server->seat);
	if (active == keyboard->wlr_keyboard || active == NULL) {
		struct flwm_keyboard *other;
		struct wlr_keyboard *next = &server->fallback_keyboard;
		wl_list_for_each(other, &server->keyboards, link) {
			if (other->wlr_keyboard->keymap) {
				next = other->wlr_keyboard;
				break;
			}
		}
		wlr_seat_set_keyboard(server->seat, next);
	}
	free(keyboard);
	update_capabilities(server);
}

static void keyboard_apply_config(struct flwm_server *server, struct wlr_keyboard *kb) {
	struct flwm_config *c = &server->config;
	struct xkb_rule_names rules = {
		.layout = c->kb_layout, .variant = c->kb_variant,
		.options = c->kb_options, .model = c->kb_model,
	};
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap) {
		wlr_log(WLR_ERROR, "invalid keyboard layout, using defaults");
		keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	}
	wlr_keyboard_set_keymap(kb, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(kb, c->repeat_rate, c->repeat_delay);
}

static void server_new_keyboard(struct flwm_server *server, struct wlr_input_device *device,
		bool is_virtual) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);
	struct flwm_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	keyboard->server = server;
	keyboard->osd_caps = keyboard->osd_num = -1;
	keyboard->osd_group = -1;
	keyboard->wlr_keyboard = wlr_keyboard;
	/* virtual keyboards (wtype, on-screen keyboards) bring their own keymap */
	if (!is_virtual) keyboard_apply_config(server, wlr_keyboard);
	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->keymap.notify = keyboard_handle_keymap;
	wl_signal_add(&wlr_keyboard->events.keymap, &keyboard->keymap);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);
	/* A virtual keyboard has no keymap yet; making it the seat keyboard now
	 * would send clients a "no keymap" event (FLTK aborts on that). It becomes
	 * the seat keyboard with its first key event instead. */
	if (wlr_keyboard->keymap) wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
	wl_list_insert(&server->keyboards, &keyboard->link);
}

/* ---- pointer devices ----------------------------------------------------- */

static void pointer_apply_config(struct flwm_server *server, struct wlr_input_device *device) {
	if (!wlr_input_device_is_libinput(device)) return;
	struct libinput_device *li = wlr_libinput_get_device_handle(device);
	struct flwm_config *c = &server->config;
	if (libinput_device_config_tap_get_finger_count(li) > 0)
		libinput_device_config_tap_set_enabled(li, c->tap_to_click
			? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
	if (libinput_device_config_scroll_has_natural_scroll(li))
		libinput_device_config_scroll_set_natural_scroll_enabled(li, c->natural_scroll);
	if (libinput_device_config_accel_is_available(li))
		libinput_device_config_accel_set_speed(li, c->pointer_accel);
	if (libinput_device_config_left_handed_is_available(li))
		libinput_device_config_left_handed_set(li, c->left_handed);
	if (libinput_device_config_dwt_is_available(li))
		libinput_device_config_dwt_set_enabled(li, c->disable_while_typing
			? LIBINPUT_CONFIG_DWT_ENABLED : LIBINPUT_CONFIG_DWT_DISABLED);
	if (libinput_device_config_middle_emulation_is_available(li))
		libinput_device_config_middle_emulation_set_enabled(li, c->middle_emulation
			? LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED : LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED);
	if (c->scroll_method) {
		enum libinput_config_scroll_method m = LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
		if (!strcmp(c->scroll_method, "two-finger")) m = LIBINPUT_CONFIG_SCROLL_2FG;
		else if (!strcmp(c->scroll_method, "edge")) m = LIBINPUT_CONFIG_SCROLL_EDGE;
		if (libinput_device_config_scroll_get_methods(li) & m || m == LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
			libinput_device_config_scroll_set_method(li, m);
	}
}

static void pointer_destroy(struct wl_listener *listener, void *data) {
	struct flwm_pointer *p = wl_container_of(listener, p, destroy);
	wl_list_remove(&p->destroy.link);
	wl_list_remove(&p->link);
	free(p);
}

static void server_new_pointer(struct flwm_server *server, struct wlr_input_device *device) {
	struct flwm_pointer *p = calloc(1, sizeof(*p));
	p->device = device;
	p->destroy.notify = pointer_destroy;
	wl_signal_add(&device->events.destroy, &p->destroy);
	wl_list_insert(&server->pointers, &p->link);
	pointer_apply_config(server, device);
	wlr_cursor_attach_input_device(server->cursor, device);
}

/* A keyboard is always advertised: when no real keyboard is active the seat
 * uses a fallback keyboard with the configured keymap, so clients never get
 * keyboard focus without a keymap (FLTK crashes on that) and do not lose
 * their wl_keyboard while on-screen / virtual keyboards come and go. */
static void update_capabilities(struct flwm_server *server) {
	wlr_seat_set_capabilities(server->seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device, false);
		break;
	case WLR_INPUT_DEVICE_SWITCH:
		protocols_new_switch(device);
		break;
	case WLR_INPUT_DEVICE_TABLET:
		protocols_new_tablet(device);
		/* the cursor follows the pen */
		/* fall through */
	case WLR_INPUT_DEVICE_POINTER:
	case WLR_INPUT_DEVICE_TOUCH:
		server_new_pointer(server, device);
		break;
	default:
		break;
	}
	update_capabilities(server);
}

static void new_virtual_keyboard(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, new_virtual_keyboard);
	struct wlr_virtual_keyboard_v1 *vk = data;
	server_new_keyboard(server, &vk->keyboard.base, true);
	update_capabilities(server);
}

static void new_virtual_pointer(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, new_virtual_pointer);
	struct wlr_virtual_pointer_v1_new_pointer_event *ev = data;
	server_new_pointer(server, &ev->new_pointer->pointer.base);
	if (ev->suggested_output)
		wlr_cursor_map_input_to_output(server->cursor, &ev->new_pointer->pointer.base, ev->suggested_output);
}

static void load_cursor_theme(struct flwm_server *server) {
	if (server->cursor_mgr) wlr_xcursor_manager_destroy(server->cursor_mgr);
	const char *theme = server->config.cursor_theme ? server->config.cursor_theme : getenv("XCURSOR_THEME");
	if (!theme) theme = "fldesktop-cursors"; /* wlroots falls back to its own cursor if it is missing */
	int size = server->config.cursor_size > 0 ? server->config.cursor_size : 24;
	server->cursor_mgr = wlr_xcursor_manager_create(theme, size);
	char sz[16];
	snprintf(sz, sizeof sz, "%d", size);
	if (theme) setenv("XCURSOR_THEME", theme, 1);
	setenv("XCURSOR_SIZE", sz, 1);
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	xwayland_update_cursor(server);
}

void input_apply_config(struct flwm_server *server) {
	keyboard_apply_config(server, &server->fallback_keyboard);
	struct flwm_keyboard *kb;
	wl_list_for_each(kb, &server->keyboards, link) keyboard_apply_config(server, kb->wlr_keyboard);
	struct flwm_pointer *p;
	wl_list_for_each(p, &server->pointers, link) pointer_apply_config(server, p->device);
	load_cursor_theme(server);
}

/* ---- seat requests --------------------------------------------------------- */

static void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	if (server->cursor_mode != FLWM_CURSOR_PASSTHROUGH) return;
	if (server->seat->pointer_state.focused_client == event->seat_client)
		wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
}

static void request_set_cursor_shape(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, request_set_cursor_shape);
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	if (server->cursor_mode != FLWM_CURSOR_PASSTHROUGH) return;
	if (server->seat->pointer_state.focused_client == event->seat_client)
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, wlr_cursor_shape_v1_name(event->shape));
}

static void seat_pointer_focus_change(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, pointer_focus_change);
	struct wlr_seat_pointer_focus_change_event *event = data;
	if (event->new_surface == NULL && server->cursor_mode == FLWM_CURSOR_PASSTHROUGH)
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	xwayland_update_cursor(server);
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void seat_request_set_primary_selection(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

static void seat_request_start_drag(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, request_start_drag);
	struct wlr_seat_request_start_drag_event *event = data;
	if (wlr_seat_validate_pointer_grab_serial(server->seat, event->origin, event->serial))
		wlr_seat_start_pointer_drag(server->seat, event->drag, event->serial);
	else
		wlr_data_source_destroy(event->drag->source);
}

static void drag_destroy(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, drag_destroy);
	wl_list_remove(&server->drag_destroy.link);
	if (server->drag_icon) wlr_scene_node_destroy(&server->drag_icon->node);
	server->drag_icon = NULL;
}

static void seat_start_drag(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, start_drag);
	struct wlr_drag *drag = data;
	server->drag_destroy.notify = drag_destroy;
	wl_signal_add(&drag->events.destroy, &server->drag_destroy);
	if (drag->icon) {
		server->drag_icon = wlr_scene_drag_icon_create(server->layer_popups, drag->icon);
		wlr_scene_node_set_position(&server->drag_icon->node, server->cursor->x, server->cursor->y);
	}
}

/* ---- cursor ---------------------------------------------------------------- */

/* While a window is dragged: the pointer at a screen edge or corner shows
 * where it will snap; releasing the button there snaps it. */
static void update_snap_preview(struct flwm_server *server) {
	struct flwm_view *view = server->grabbed_view;
	enum flwm_snap zone = FLWM_SNAP_NONE;
	struct flwm_output *o = output_at(server, server->cursor->x, server->cursor->y);
	if (server->config.drag_snap && view && view->role == FLWM_ROLE_NORMAL && o) {
		struct wlr_box f = o->full_area;
		zone = flwm_snap_zone((int)server->cursor->x, (int)server->cursor->y,
			(struct flwm_rect){f.x, f.y, f.width, f.height}, 8, 64);
	}
	server->drag_snap = zone;
	if (zone == FLWM_SNAP_NONE) {
		if (server->snap_preview) wlr_scene_node_set_enabled(&server->snap_preview->node, false);
		return;
	}
	struct wlr_box u = o->usable_area;
	struct flwm_rect r = flwm_snap_rect(zone, (struct flwm_rect){u.x, u.y, u.width, u.height});
	if (!server->snap_preview || !server->snap_preview->node.enabled) {
		/* the accent colour, translucent (premultiplied); read once per showing */
		uint32_t c = flwm_accent_color();
		float color[4] = {((c >> 16) & 0xff) / 255.0f * 0.35f, ((c >> 8) & 0xff) / 255.0f * 0.35f,
			(c & 0xff) / 255.0f * 0.35f, 0.35f};
		if (!server->snap_preview)
			server->snap_preview = wlr_scene_rect_create(server->layer_views, r.width, r.height, color);
		else
			wlr_scene_rect_set_color(server->snap_preview, color);
	}
	struct wlr_scene_rect *p = server->snap_preview;
	wlr_scene_rect_set_size(p, r.width, r.height);
	wlr_scene_node_set_position(&p->node, r.x, r.y);
	wlr_scene_node_reparent(&p->node, view->tree->node.parent);
	wlr_scene_node_place_below(&p->node, &view->tree->node);
	wlr_scene_node_set_enabled(&p->node, true);
}

void input_reset_cursor_mode(struct flwm_server *server) {
	if (server->snap_preview) wlr_scene_node_set_enabled(&server->snap_preview->node, false);
	if (server->cursor_mode == FLWM_CURSOR_RESIZE && server->grabbed_view &&
			server->grabbed_view->type == FLWM_VIEW_XDG)
		wlr_xdg_toplevel_set_resizing(server->grabbed_view->xdg_toplevel, false);
	server->cursor_mode = FLWM_CURSOR_PASSTHROUGH;
	server->grabbed_view = NULL;
	server->pressed_part = FLWM_PART_NONE;
}

static void process_cursor_resize(struct flwm_server *server) {
	struct flwm_view *view = server->grabbed_view;
	double bx = server->cursor->x - server->grab_x;
	double by = server->cursor->y - server->grab_y;
	struct wlr_box g = server->grab_geobox;
	int left = g.x, right = g.x + g.width, top = g.y, bottom = g.y + g.height;
	const int min = 64;
	if (server->resize_edges & WLR_EDGE_TOP) {
		top = (int)by;
		if (top > bottom - min) top = bottom - min;
	} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
		bottom = (int)by;
		if (bottom < top + min) bottom = top + min;
	}
	if (server->resize_edges & WLR_EDGE_LEFT) {
		left = (int)bx;
		if (left > right - min) left = right - min;
	} else if (server->resize_edges & WLR_EDGE_RIGHT) {
		right = (int)bx;
		if (right < left + min) right = left + min;
	}
	if (view->type == FLWM_VIEW_XWAYLAND) {
		/* X11 windows take their new position along with the size */
		view->x = left;
		view->y = top;
		wlr_scene_node_set_position(&view->tree->node, left, top);
		xwayland_view_configure(view, left, top, right - left, bottom - top);
		return;
	}
	/* position is fixed up on commit for top/left edges (see view_commit) */
	wlr_xdg_toplevel_set_size(view->xdg_toplevel, right - left, bottom - top);
}

static const char *edge_cursor(uint32_t edges) {
	switch (edges) {
	case WLR_EDGE_TOP: return "n-resize";
	case WLR_EDGE_BOTTOM: return "s-resize";
	case WLR_EDGE_LEFT: return "w-resize";
	case WLR_EDGE_RIGHT: return "e-resize";
	case WLR_EDGE_TOP | WLR_EDGE_LEFT: return "nw-resize";
	case WLR_EDGE_TOP | WLR_EDGE_RIGHT: return "ne-resize";
	case WLR_EDGE_BOTTOM | WLR_EDGE_LEFT: return "sw-resize";
	case WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT: return "se-resize";
	}
	return "default";
}

static void set_hover(struct flwm_server *server, struct flwm_view *view, enum flwm_decor_part part) {
	if (server->hover_view && server->hover_view != view)
		decor_set_hover(server->hover_view, FLWM_PART_NONE);
	server->hover_view = view;
	if (view) decor_set_hover(view, part);
}

void input_update_pointer_focus(struct flwm_server *server, uint32_t time) {
	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct flwm_node_desc *desc = input_desc_at(server, server->cursor->x, server->cursor->y,
		&surface, &sx, &sy);
	if (desc && desc->kind == FLWM_NODE_DECOR) {
		struct flwm_view *view = desc->ptr;
		if (desc->part == FLWM_PART_EDGE) {
			wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, edge_cursor(desc->edges));
			set_hover(server, NULL, FLWM_PART_NONE);
		} else {
			wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	xwayland_update_cursor(server);
			set_hover(server, view, decor_part_at(view, server->cursor->x, server->cursor->y));
		}
		wlr_seat_pointer_clear_focus(server->seat);
		return;
	}
	set_hover(server, NULL, FLWM_PART_NONE);
	if (!desc) wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	if (surface) {
		wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(server->seat, time, sx, sy);
		if (server->config.focus_follows_mouse && desc && desc->kind == FLWM_NODE_VIEW) {
			struct flwm_view *view = desc->ptr;
			if (view->role == FLWM_ROLE_NORMAL && view_focused(server) != view) view_focus(view);
		}
	} else {
		wlr_seat_pointer_clear_focus(server->seat);
	}
}

/* Hot corners: the pointer pushed into a corner runs its action, once until
 * it leaves the corner again. */
static void hot_corners(struct flwm_server *server) {
	if (server->locked) return;
	struct flwm_output *o = output_at(server, server->cursor->x, server->cursor->y);
	if (!o) return;
	struct wlr_box b = o->full_area;
	double x = server->cursor->x, y = server->cursor->y;
	bool left = x <= b.x + 1, right = x >= b.x + b.width - 2;
	bool top = y <= b.y + 1, bottom = y >= b.y + b.height - 2;
	int corner = top && left ? 0 : top && right ? 1 : bottom && left ? 2 : bottom && right ? 3 : -1;
	if (corner < 0) {
		/* away from all corners: armed again */
		bool near = (x < b.x + 24 || x > b.x + b.width - 24) && (y < b.y + 24 || y > b.y + b.height - 24);
		if (!near) server->hot_corner_fired = -1;
		return;
	}
	if (corner == server->hot_corner_fired) return;
	struct flwm_gesture_action *a = &server->config.hot_corner[corner];
	if (a->type == FLWM_ACTION_NONE) return;
	server->hot_corner_fired = corner;
	input_run_action(server, a->type, a->arg);
}

static void process_cursor_motion(struct flwm_server *server, uint32_t time) {
	zoom_cursor_moved(server);
	if (server->drag_icon)
		wlr_scene_node_set_position(&server->drag_icon->node, server->cursor->x, server->cursor->y);
	switch (server->cursor_mode) {
	case FLWM_CURSOR_MOVE:
		view_set_position(server->grabbed_view, (int)(server->cursor->x - server->grab_x),
			(int)(server->cursor->y - server->grab_y));
		update_snap_preview(server);
		return;
	case FLWM_CURSOR_RESIZE:
		process_cursor_resize(server);
		return;
	case FLWM_CURSOR_PRESSED_BUTTON:
		if (server->grabbed_view) {
			enum flwm_decor_part p = decor_part_at(server->grabbed_view, server->cursor->x, server->cursor->y);
			decor_set_hover(server->grabbed_view, p == server->pressed_part ? p : FLWM_PART_NONE);
		}
		return;
	default:
		/* a hot corner (the switcher) would take the release that ends a drag */
		if (!server->seat->drag) hot_corners(server);
		input_update_pointer_focus(server, time);
	}
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	lock_activity(server);
	wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	lock_activity(server);
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
	process_cursor_motion(server, event->time_msec);
}

static uint32_t edges_for_point(struct flwm_view *view, double lx, double ly) {
	struct wlr_box g;
	view_get_geometry(view, &g);
	uint32_t e = 0;
	e |= (lx < g.x + g.width / 2.0) ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
	e |= (ly < g.y + g.height / 2.0) ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
	return e;
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	lock_activity(server);
	/* a click picks a window in the switcher (or closes it) */
	if (switcher_active(server)) {
		if (event->state == WL_POINTER_BUTTON_STATE_PRESSED)
			switcher_click(server, server->cursor->x, server->cursor->y);
		return;
	}

	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		enum flwm_cursor_mode mode = server->cursor_mode;
		if (mode == FLWM_CURSOR_PRESSED_BUTTON && server->grabbed_view) {
			struct flwm_view *view = server->grabbed_view;
			enum flwm_decor_part part = decor_part_at(view, server->cursor->x, server->cursor->y);
			enum flwm_decor_part pressed = server->pressed_part;
			input_reset_cursor_mode(server);
			if (part == pressed) {
				if (part == FLWM_PART_CLOSE) view_close(view);
				else if (part == FLWM_PART_MAXIMIZE) view_set_maximized(view, !view->maximized);
				else if (part == FLWM_PART_MINIMIZE) view_set_minimized(view, true);
			}
			input_update_pointer_focus(server, event->time_msec);
			return;
		}
		struct flwm_view *dropped = mode == FLWM_CURSOR_MOVE ? server->grabbed_view : NULL;
		enum flwm_snap snap = server->drag_snap;
		server->drag_snap = FLWM_SNAP_NONE;
		input_reset_cursor_mode(server);
		if (dropped && snap != FLWM_SNAP_NONE) {
			view_set_snap(dropped, snap);
			/* unsnapping brings it back where the drag began */
			dropped->saved = server->drag_from;
			switcher_snap_assist(server, dropped);
		}
		if (mode == FLWM_CURSOR_PASSTHROUGH)
			wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
		else
			input_update_pointer_focus(server, event->time_msec);
		return;
	}

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct flwm_node_desc *desc = input_desc_at(server, server->cursor->x, server->cursor->y,
		&surface, &sx, &sy);

	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
	uint32_t mods = kb ? wlr_keyboard_get_modifiers(kb) : 0;

	/* An open popup menu holds a grab that only sees clicks passed to the
	 * seat. Clicks we handle ourselves (title bars, borders, Super+drag)
	 * would leave it open, and a client in its menu loop ignores a close
	 * request, so end the grab first: the popups get popup_done and close. */
	struct wlr_seat_pointer_state *ps = &server->seat->pointer_state;
	bool ours = !surface || (desc && desc->kind == FLWM_NODE_DECOR) || (mods & WLR_MODIFIER_LOGO);
	if (ps->grab != ps->default_grab && !server->locked && ours && !server->seat->drag)
		wlr_seat_pointer_end_grab(server->seat);
	/* menus without a grab (FLTK's): a click elsewhere closes them too */
	struct flwm_view *menu_owner = view_focused(server);
	if (!server->locked && view_has_menus(menu_owner)) {
		struct flwm_view *target = desc && (desc->kind == FLWM_NODE_VIEW || desc->kind == FLWM_NODE_DECOR)
			? desc->ptr : NULL;
		if (ours || target != menu_owner) view_dismiss_menus(menu_owner);
	}

	if (server->locked) {
		/* the cover swallows clicks; the lock client gets its own */
		struct flwm_view *lv = desc && desc->kind == FLWM_NODE_VIEW ? desc->ptr : NULL;
		if (lv && lv->role == FLWM_ROLE_LOCK) view_focus(lv);
		if (surface) wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
		return;
	}
	if (desc && desc->kind == FLWM_NODE_LAYER) {
		layer_clicked(desc->ptr);
		wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
		return;
	}
	/* clicking a window takes the keyboard back from an on-demand layer surface */
	if (server->focused_layer && server->focused_layer->layer_surface->current.keyboard_interactive !=
			ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)
		server->focused_layer = NULL;

	struct flwm_view *view = desc ? desc->ptr : NULL;
	/* the on-screen keyboard and notifications get pointer events but never
	 * the keyboard focus */
	if (view && view->role != FLWM_ROLE_OSK && view->role != FLWM_ROLE_NOTIFY && view->role != FLWM_ROLE_OSD)
		view_focus(view);

	/* Super + drag moves / resizes any normal window */
	if (view && view->role == FLWM_ROLE_NORMAL && (mods & WLR_MODIFIER_LOGO)) {
		if (event->button == BTN_LEFT) {
			view_begin_interactive(view, FLWM_CURSOR_MOVE, 0);
			return;
		}
		if (event->button == BTN_RIGHT) {
			view_begin_interactive(view, FLWM_CURSOR_RESIZE,
				edges_for_point(view, server->cursor->x, server->cursor->y));
			return;
		}
	}

	if (desc && desc->kind == FLWM_NODE_DECOR) {
		if (desc->part == FLWM_PART_EDGE) {
			view_begin_interactive(view, FLWM_CURSOR_RESIZE, desc->edges);
			return;
		}
		enum flwm_decor_part part = decor_part_at(view, server->cursor->x, server->cursor->y);
		if (event->button != BTN_LEFT) {
			if (event->button == BTN_MIDDLE) {
				/* middle click on the title sends the window to the back */
				wlr_scene_node_lower_to_bottom(&view->tree->node);
			}
			return;
		}
		if (part == FLWM_PART_CLOSE || part == FLWM_PART_MAXIMIZE || part == FLWM_PART_MINIMIZE) {
			server->cursor_mode = FLWM_CURSOR_PRESSED_BUTTON;
			server->grabbed_view = view;
			server->pressed_part = part;
			return;
		}
		/* title: double click toggles maximize, otherwise start moving */
		if (server->last_click_view == view && event->time_msec - server->last_click_time < DOUBLE_CLICK_MS) {
			server->last_click_view = NULL;
			view_set_maximized(view, !view->maximized);
			return;
		}
		server->last_click_view = view;
		server->last_click_time = event->time_msec;
		view_begin_interactive(view, FLWM_CURSOR_MOVE, 0);
		return;
	}

	wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
}

void input_emulate_motion(struct flwm_server *server, uint32_t time) {
	process_cursor_motion(server, time);
}

void input_emulate_button(struct flwm_server *server, uint32_t time, uint32_t button, bool pressed) {
	struct wlr_pointer_button_event ev = {0};
	ev.time_msec = time;
	ev.button = button;
	ev.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
	server_cursor_button(&server->cursor_button, &ev);
	wlr_seat_pointer_notify_frame(server->seat);
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	lock_activity(server);
	wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation, event->delta,
		event->delta_discrete, event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	struct flwm_server *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

void input_init(struct flwm_server *server) {
	server->cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
	load_cursor_theme(server);

	server->cursor_mode = FLWM_CURSOR_PASSTHROUGH;
	server->cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
	server->cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server->cursor->events.motion_absolute, &server->cursor_motion_absolute);
	server->cursor_button.notify = server_cursor_button;
	wl_signal_add(&server->cursor->events.button, &server->cursor_button);
	server->cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
	server->cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);

	wl_list_init(&server->keyboards);
	wl_list_init(&server->pointers);
	server->new_input.notify = server_new_input;
	wl_signal_add(&server->backend->events.new_input, &server->new_input);

	server->seat = wlr_seat_create(server->wl_display, "seat0");
	static const struct wlr_keyboard_impl fallback_impl = {.name = "fl-wm-fallback-keyboard"};
	wlr_keyboard_init(&server->fallback_keyboard, &fallback_impl, "fl-wm fallback keyboard");
	keyboard_apply_config(server, &server->fallback_keyboard);
	wlr_seat_set_keyboard(server->seat, &server->fallback_keyboard);
	server->request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server->seat->events.request_set_cursor, &server->request_cursor);
	server->pointer_focus_change.notify = seat_pointer_focus_change;
	wl_signal_add(&server->seat->pointer_state.events.focus_change, &server->pointer_focus_change);
	server->request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server->seat->events.request_set_selection, &server->request_set_selection);
	server->request_set_primary_selection.notify = seat_request_set_primary_selection;
	wl_signal_add(&server->seat->events.request_set_primary_selection,
		&server->request_set_primary_selection);
	server->request_start_drag.notify = seat_request_start_drag;
	wl_signal_add(&server->seat->events.request_start_drag, &server->request_start_drag);
	server->start_drag.notify = seat_start_drag;
	wl_signal_add(&server->seat->events.start_drag, &server->start_drag);

	server->virtual_keyboard_mgr = wlr_virtual_keyboard_manager_v1_create(server->wl_display);
	server->new_virtual_keyboard.notify = new_virtual_keyboard;
	wl_signal_add(&server->virtual_keyboard_mgr->events.new_virtual_keyboard, &server->new_virtual_keyboard);
	server->virtual_pointer_mgr = wlr_virtual_pointer_manager_v1_create(server->wl_display);
	server->new_virtual_pointer.notify = new_virtual_pointer;
	wl_signal_add(&server->virtual_pointer_mgr->events.new_virtual_pointer, &server->new_virtual_pointer);

	update_capabilities(server);

	server->cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(server->wl_display, 1);
	server->request_set_cursor_shape.notify = request_set_cursor_shape;
	wl_signal_add(&server->cursor_shape_mgr->events.request_set_shape, &server->request_set_cursor_shape);
}

void input_finish(struct flwm_server *server) {
	wlr_seat_set_keyboard(server->seat, NULL);
	wlr_keyboard_finish(&server->fallback_keyboard);
}
