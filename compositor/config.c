/* fl-wm configuration: ~/.config/fldesktop/wm.ini */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wayland-server-protocol.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/log.h>

#include "server.h"

static char *xstrdup(const char *s) { return s ? strdup(s) : NULL; }

static void set_str(char **dst, const char *v) {
	free(*dst);
	*dst = xstrdup(v);
}

static uint32_t parse_color(const char *s, uint32_t def) {
	if (!s || s[0] != '#') return def;
	size_t n = strlen(s + 1);
	unsigned long v = strtoul(s + 1, NULL, 16);
	if (n == 6) return 0xff000000u | (uint32_t)v;
	if (n == 8) return (uint32_t)((v & 0xff) << 24 | (v >> 8)); /* #rrggbbaa -> argb */
	return def;
}

static bool parse_bool(const char *s) {
	return !strcasecmp(s, "true") || !strcmp(s, "1") || !strcasecmp(s, "yes") || !strcasecmp(s, "on");
}

static char *trim(char *s) {
	while (isspace((unsigned char)*s)) s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
	return s;
}

/* "name [argument]" -> action; *arg_out is malloc'ed (or NULL) */
static bool parse_action(const char *action, enum flwm_action_type *type_out, char **arg_out) {
	char abuf[512];
	snprintf(abuf, sizeof abuf, "%s", action);
	char *a = trim(abuf);
	char *arg = NULL;
	char *sp = strchr(a, ' ');
	if (sp) {
		*sp = 0;
		arg = trim(sp + 1);
	}
	static const struct { const char *name; enum flwm_action_type type; } actions[] = {
		{"exec", FLWM_ACTION_EXEC}, {"close", FLWM_ACTION_CLOSE},
		{"maximize", FLWM_ACTION_MAXIMIZE}, {"minimize", FLWM_ACTION_MINIMIZE},
		{"fullscreen", FLWM_ACTION_FULLSCREEN}, {"cycle", FLWM_ACTION_CYCLE},
		{"cycle-back", FLWM_ACTION_CYCLE_BACK}, {"snap-left", FLWM_ACTION_SNAP_LEFT},
		{"snap-right", FLWM_ACTION_SNAP_RIGHT}, {"snap-up", FLWM_ACTION_SNAP_UP},
		{"snap-down", FLWM_ACTION_SNAP_DOWN}, {"resize-mode", FLWM_ACTION_RESIZE_MODE},
		{"launcher", FLWM_ACTION_LAUNCHER},
		{"quit", FLWM_ACTION_QUIT}, {"reload", FLWM_ACTION_RELOAD}, {"lock", FLWM_ACTION_LOCK},
		{"show-desktop", FLWM_ACTION_SHOW_DESKTOP},
		{"workspace", FLWM_ACTION_WORKSPACE}, {"workspace-next", FLWM_ACTION_WORKSPACE_NEXT},
		{"workspace-prev", FLWM_ACTION_WORKSPACE_PREV},
		{"move-to-workspace", FLWM_ACTION_MOVE_TO_WORKSPACE},
		{"move-to-workspace-next", FLWM_ACTION_MOVE_TO_WORKSPACE_NEXT},
		{"move-to-workspace-prev", FLWM_ACTION_MOVE_TO_WORKSPACE_PREV},
		{"sticky", FLWM_ACTION_STICKY},
		{"zoom-in", FLWM_ACTION_ZOOM_IN}, {"zoom-out", FLWM_ACTION_ZOOM_OUT},
		{"zoom-reset", FLWM_ACTION_ZOOM_RESET},
		{"portal-shortcut", FLWM_ACTION_PORTAL_SHORTCUT},
		{"overview", FLWM_ACTION_OVERVIEW},
		{"snap-third-left", FLWM_ACTION_SNAP_THIRD_LEFT}, {"snap-third-right", FLWM_ACTION_SNAP_THIRD_RIGHT},
	};
	enum flwm_action_type type = FLWM_ACTION_NONE;
	for (size_t i = 0; i < sizeof actions / sizeof actions[0]; i++)
		if (!strcasecmp(a, actions[i].name)) type = actions[i].type;
	if (type == FLWM_ACTION_NONE) {
		wlr_log(WLR_ERROR, "config: unknown action '%s'", a);
		return false;
	}
	*type_out = type;
	*arg_out = xstrdup(arg);
	return true;
}

static void add_binding(struct flwm_config *c, const char *combo, const char *action) {
	if (c->nbindings >= FLWM_MAX_BINDINGS) return;
	uint32_t mods = 0;
	xkb_keysym_t sym = XKB_KEY_NoSymbol;
	char buf[128];
	snprintf(buf, sizeof buf, "%s", combo);
	char *save = NULL;
	for (char *tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
		tok = trim(tok);
		if (!strcasecmp(tok, "Super") || !strcasecmp(tok, "Logo") || !strcasecmp(tok, "Mod4"))
			mods |= WLR_MODIFIER_LOGO;
		else if (!strcasecmp(tok, "Alt") || !strcasecmp(tok, "Mod1")) mods |= WLR_MODIFIER_ALT;
		else if (!strcasecmp(tok, "Ctrl") || !strcasecmp(tok, "Control")) mods |= WLR_MODIFIER_CTRL;
		else if (!strcasecmp(tok, "Shift")) mods |= WLR_MODIFIER_SHIFT;
		else {
			sym = xkb_keysym_from_name(tok, XKB_KEYSYM_NO_FLAGS);
			if (sym == XKB_KEY_NoSymbol)
				sym = xkb_keysym_from_name(tok, XKB_KEYSYM_CASE_INSENSITIVE);
		}
	}
	if (sym == XKB_KEY_NoSymbol) {
		wlr_log(WLR_ERROR, "config: unknown key in binding '%s'", combo);
		return;
	}
	/* keysyms are compared lower-case so Super+Shift+Q matches 'q' */
	sym = xkb_keysym_to_lower(sym);

	enum flwm_action_type type;
	char *arg;
	if (!parse_action(action, &type, &arg)) return;
	/* replace an existing binding for the same combo */
	for (int i = 0; i < c->nbindings; i++) {
		struct flwm_binding *b = &c->bindings[i];
		if (b->modifiers == mods && b->sym == sym) {
			free(b->arg);
			b->action = type;
			b->arg = arg;
			return;
		}
	}
	struct flwm_binding *b = &c->bindings[c->nbindings++];
	b->modifiers = mods;
	b->sym = sym;
	b->action = type;
	b->arg = arg;
}

/* [environment] KEY=value sets a variable for the programs started from now
 * on; "KEY=" unsets it and "KEY=@inherit" restores the value fl-wm started
 * with (Settings uses this for the proxy variables). */
#define MAX_ENV 64
static struct { char *key, *orig; bool had; } env_orig[MAX_ENV];
static int nenv_orig;

static void env_apply(const char *k, const char *v) {
	if (!*k || strchr(k, '=')) return;
	int i;
	for (i = 0; i < nenv_orig; i++)
		if (!strcmp(env_orig[i].key, k)) break;
	if (i == nenv_orig) {
		if (nenv_orig == MAX_ENV) return;
		const char *o = getenv(k);
		env_orig[i].key = strdup(k);
		env_orig[i].had = o != NULL;
		env_orig[i].orig = o ? strdup(o) : NULL;
		nenv_orig++;
	}
	if (!strcmp(v, "@inherit")) {
		if (env_orig[i].had) setenv(k, env_orig[i].orig, 1);
		else unsetenv(k);
	} else if (!*v) {
		unsetenv(k);
	} else {
		setenv(k, v, 1);
	}
}

void config_init_defaults(struct flwm_config *c) {
	memset(c, 0, sizeof *c);
	c->title_height = 30;
	c->border_width = 1;
	c->active_title = 0xff2b2d31;
	c->inactive_title = 0xffe8e9eb;
	c->active_text = 0xffffffff;
	c->inactive_text = 0xff6b6f76;
	c->border_color = 0xff7a7f87;
	c->font = strdup("Sans Bold 10");
	c->repeat_rate = 25;
	c->repeat_delay = 600;
	c->tap_to_click = true;
	c->disable_while_typing = true;
	c->cursor_size = 24;
	c->default_scale = 1.0f;
	c->launcher_cmd = strdup("fl-launcher");
	c->switcher = true;
	c->snap_assist = true;
	c->osk_in_tablet_mode = true;
	c->xwayland = true;
	c->workspaces = 4;
	c->gestures = true;
	c->gesture_fingers = 3;
	c->gesture_natural = true;
	c->gesture_up.type = FLWM_ACTION_LAUNCHER;
	c->gesture_down.type = FLWM_ACTION_SHOW_DESKTOP;
	c->drag_snap = true;
	c->lock_color = 0xff1e1f22;
	c->blank_ms = 10 * 60000;

	add_binding(c, "Super+Return", "exec fl-term");
	add_binding(c, "Super+t", "exec fl-term");
	add_binding(c, "Super+e", "exec fl-files");
	add_binding(c, "Super+space", "launcher");
	add_binding(c, "Super+d", "show-desktop");
	add_binding(c, "Super+q", "close");
	add_binding(c, "Alt+F4", "close");
	add_binding(c, "Super+Up", "snap-up");
	add_binding(c, "Super+Down", "snap-down");
	add_binding(c, "Super+r", "resize-mode");
	add_binding(c, "Super+Left", "snap-left");
	add_binding(c, "Super+Right", "snap-right");
	add_binding(c, "Super+Alt+Left", "snap-third-left");
	add_binding(c, "Super+Alt+Right", "snap-third-right");
	add_binding(c, "Super+Tab", "overview");
	add_binding(c, "Super+f", "fullscreen");
	add_binding(c, "Super+equal", "zoom-in");
	add_binding(c, "Super+plus", "zoom-in");
	add_binding(c, "Super+KP_Add", "zoom-in");
	add_binding(c, "Super+minus", "zoom-out");
	add_binding(c, "Super+KP_Subtract", "zoom-out");
	add_binding(c, "Super+0", "zoom-reset");
	add_binding(c, "Alt+Tab", "cycle");
	add_binding(c, "Alt+Shift+Tab", "cycle-back");
	add_binding(c, "Super+Shift+r", "reload");
	add_binding(c, "Super+l", "lock");
	add_binding(c, "Ctrl+Alt+BackSpace", "quit");
	add_binding(c, "Print", "exec fl-snip --full");
	add_binding(c, "Shift+Print", "exec fl-snip --area");
	add_binding(c, "Super+Shift+s", "exec fl-snip --snip");
	add_binding(c, "Ctrl+Shift+Escape", "exec fl-taskmgr");
	/* media, volume and brightness keys: fl-panel does them and shows the
	 * on-screen display */
	static const char *const media[][2] = {
		{"XF86AudioRaiseVolume", "volume-up"}, {"XF86AudioLowerVolume", "volume-down"},
		{"XF86AudioMute", "volume-mute"}, {"XF86AudioMicMute", "mic-mute"},
		{"XF86MonBrightnessUp", "brightness-up"}, {"XF86MonBrightnessDown", "brightness-down"},
		{"XF86KbdBrightnessUp", "kbd-brightness-up"}, {"XF86KbdBrightnessDown", "kbd-brightness-down"},
		{"XF86KbdLightOnOff", "kbd-brightness-toggle"},
		{"XF86AudioPlay", "media-play-pause"}, {"XF86AudioPause", "media-pause"},
		{"XF86AudioStop", "media-stop"}, {"XF86AudioNext", "media-next"},
		{"XF86AudioPrev", "media-previous"},
	};
	for (size_t i = 0; i < sizeof media / sizeof media[0]; i++) {
		char action[96];
		snprintf(action, sizeof action, "exec fl-panel --action %s", media[i][1]);
		add_binding(c, media[i][0], action);
	}
	/* workspaces: Super+N goes there, Super+Shift+N sends the window there */
	for (int i = 1; i <= FLWM_MAX_WORKSPACES; i++) {
		char combo[32], action[32];
		snprintf(combo, sizeof combo, "Super+%d", i);
		snprintf(action, sizeof action, "workspace %d", i);
		add_binding(c, combo, action);
		snprintf(combo, sizeof combo, "Super+Shift+%d", i);
		snprintf(action, sizeof action, "move-to-workspace %d", i);
		add_binding(c, combo, action);
	}
	add_binding(c, "Super+Ctrl+Right", "workspace-next");
	add_binding(c, "Super+Ctrl+Left", "workspace-prev");
	add_binding(c, "Super+Ctrl+Shift+Right", "move-to-workspace-next");
	add_binding(c, "Super+Ctrl+Shift+Left", "move-to-workspace-prev");

	c->autostart[c->nautostart++] = strdup("fl-desktop");
	c->autostart[c->nautostart++] = strdup("fl-panel");
	c->autostart[c->nautostart++] = strdup("fl-autostart");
}

static struct flwm_output_config *output_config(struct flwm_config *c, const char *name) {
	for (int i = 0; i < c->noutputs; i++)
		if (!strcmp(c->outputs[i].name, name)) return &c->outputs[i];
	if (c->noutputs >= FLWM_MAX_OUTPUT_CONFIGS) return NULL;
	struct flwm_output_config *o = &c->outputs[c->noutputs++];
	memset(o, 0, sizeof *o);
	o->name = strdup(name);
	return o;
}

static int parse_transform(const char *v) {
	if (!strcmp(v, "90")) return WL_OUTPUT_TRANSFORM_90;
	if (!strcmp(v, "180")) return WL_OUTPUT_TRANSFORM_180;
	if (!strcmp(v, "270")) return WL_OUTPUT_TRANSFORM_270;
	if (!strcmp(v, "flipped")) return WL_OUTPUT_TRANSFORM_FLIPPED;
	return WL_OUTPUT_TRANSFORM_NORMAL;
}

bool config_load(struct flwm_config *c, const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) return false;
	set_str(&c->path, path);
	char line[1024], section[128] = "";
	bool autostart_reset = false;
	while (fgets(line, sizeof line, f)) {
		char *l = trim(line);
		if (!*l || *l == '#' || *l == ';') continue;
		if (*l == '[') {
			char *e = strchr(l, ']');
			if (e) *e = 0;
			snprintf(section, sizeof section, "%s", l + 1);
			continue;
		}
		char *eq = strchr(l, '=');
		if (!eq) continue;
		*eq = 0;
		char *k = trim(l), *v = trim(eq + 1);

		if (!strcasecmp(section, "appearance")) {
			if (!strcmp(k, "title_height")) c->title_height = atoi(v);
			else if (!strcmp(k, "border_width")) c->border_width = atoi(v);
			else if (!strcmp(k, "font")) set_str(&c->font, v);
			else if (!strcmp(k, "active_title")) c->active_title = parse_color(v, c->active_title);
			else if (!strcmp(k, "inactive_title")) c->inactive_title = parse_color(v, c->inactive_title);
			else if (!strcmp(k, "active_text")) c->active_text = parse_color(v, c->active_text);
			else if (!strcmp(k, "inactive_text")) c->inactive_text = parse_color(v, c->inactive_text);
			else if (!strcmp(k, "border_color")) c->border_color = parse_color(v, c->border_color);
		} else if (!strcasecmp(section, "behaviour") || !strcasecmp(section, "behavior")) {
			if (!strcmp(k, "panel_position")) c->panel_top = !strcasecmp(v, "top");
			else if (!strcmp(k, "focus_follows_mouse")) c->focus_follows_mouse = parse_bool(v);
			else if (!strcmp(k, "drag_snap")) c->drag_snap = parse_bool(v);
			else if (!strcmp(k, "launcher")) set_str(&c->launcher_cmd, v);
			else if (!strcmp(k, "switcher")) c->switcher = parse_bool(v);
			else if (!strcmp(k, "snap_assist")) c->snap_assist = parse_bool(v);
			else if (!strcmp(k, "osk_in_tablet_mode")) c->osk_in_tablet_mode = parse_bool(v);
			else if (!strncmp(k, "hot_corner_", 11)) {
				static const char *names[] = {"top_left", "top_right", "bottom_left", "bottom_right"};
				for (int i = 0; i < 4; i++)
					if (!strcmp(k + 11, names[i])) {
						struct flwm_gesture_action *h = &c->hot_corner[i];
						free(h->arg);
						h->arg = NULL;
						h->type = FLWM_ACTION_NONE;
						if (*v && strcasecmp(v, "none")) parse_action(v, &h->type, &h->arg);
					}
			}
			else if (!strcmp(k, "xwayland")) c->xwayland = parse_bool(v);
			else if (!strcmp(k, "workspaces")) {
				int n = atoi(v);
				c->workspaces = n < 1 ? 1 : n > FLWM_MAX_WORKSPACES ? FLWM_MAX_WORKSPACES : n;
			}
		} else if (!strcasecmp(section, "keyboard")) {
			if (!strcmp(k, "layout")) set_str(&c->kb_layout, v);
			else if (!strcmp(k, "variant")) set_str(&c->kb_variant, v);
			else if (!strcmp(k, "options")) set_str(&c->kb_options, v);
			else if (!strcmp(k, "model")) set_str(&c->kb_model, v);
			else if (!strcmp(k, "repeat_rate")) c->repeat_rate = atoi(v);
			else if (!strcmp(k, "repeat_delay")) c->repeat_delay = atoi(v);
		} else if (!strcasecmp(section, "pointer")) {
			if (!strcmp(k, "tap_to_click")) c->tap_to_click = parse_bool(v);
			else if (!strcmp(k, "natural_scroll")) c->natural_scroll = parse_bool(v);
			else if (!strcmp(k, "accel")) c->pointer_accel = atof(v);
			else if (!strcmp(k, "left_handed")) c->left_handed = parse_bool(v);
			else if (!strcmp(k, "disable_while_typing")) c->disable_while_typing = parse_bool(v);
			else if (!strcmp(k, "middle_emulation")) c->middle_emulation = parse_bool(v);
			else if (!strcmp(k, "scroll_method")) set_str(&c->scroll_method, v);
		} else if (!strcasecmp(section, "gestures")) {
			if (!strcmp(k, "enabled")) c->gestures = parse_bool(v);
			else if (!strcmp(k, "fingers")) {
				int n = atoi(v);
				c->gesture_fingers = n < 3 ? 3 : n > 5 ? 5 : n;
			} else if (!strcmp(k, "natural")) c->gesture_natural = parse_bool(v);
			else if (!strcmp(k, "swipe_up") || !strcmp(k, "swipe_down")) {
				struct flwm_gesture_action *g = k[6] == 'u' ? &c->gesture_up : &c->gesture_down;
				free(g->arg);
				g->arg = NULL;
				g->type = FLWM_ACTION_NONE;
				if (*v && strcasecmp(v, "none")) parse_action(v, &g->type, &g->arg);
			}
		} else if (!strcasecmp(section, "cursor")) {
			if (!strcmp(k, "theme")) set_str(&c->cursor_theme, v);
			else if (!strcmp(k, "size")) c->cursor_size = atoi(v);
		} else if (!strcasecmp(section, "lock")) {
			if (!strcmp(k, "idle_minutes")) c->lock_idle_minutes = atoi(v);
			else if (!strcmp(k, "command")) set_str(&c->lock_cmd, v);
			else if (!strcmp(k, "color")) c->lock_color = parse_color(v, c->lock_color);
		} else if (!strcasecmp(section, "power")) {
			if (!strcmp(k, "blank_minutes")) {
				double m = atof(v);
				c->blank_ms = m > 0 ? (int)(m * 60000) : 0;
			}
		} else if (!strcasecmp(section, "accessibility")) {
			if (!strcmp(k, "sticky_keys")) c->sticky_keys = parse_bool(v);
			else if (!strcmp(k, "bounce_keys")) {
				int ms = atoi(v);
				c->bounce_keys_ms = ms < 0 ? 0 : ms > 2000 ? 2000 : ms;
			}
		} else if (!strcasecmp(section, "environment")) {
			env_apply(k, v);
		} else if (!strcasecmp(section, "output")) {
			if (!strcmp(k, "scale")) c->default_scale = (float)atof(v);
		} else if (!strncasecmp(section, "output:", 7)) {
			struct flwm_output_config *o = output_config(c, section + 7);
			if (!o) continue;
			if (!strcmp(k, "scale")) o->scale = (float)atof(v);
			else if (!strcmp(k, "mode")) {
				double hz = 0;
				if (sscanf(v, "%dx%d@%lf", &o->width, &o->height, &hz) >= 2)
					o->refresh_mhz = (int)(hz * 1000);
			} else if (!strcmp(k, "position")) {
				o->has_position = sscanf(v, "%d,%d", &o->x, &o->y) == 2;
			} else if (!strcmp(k, "enabled")) o->disabled = !parse_bool(v);
			else if (!strcmp(k, "transform")) o->transform = parse_transform(v);
		} else if (!strcasecmp(section, "keys")) {
			/* "defaults=false" drops the built-in bindings, so the file can
			 * hold the complete list (fl-settings writes it this way) */
			if (!strcasecmp(k, "defaults")) {
				if (!parse_bool(v)) {
					for (int i = 0; i < c->nbindings; i++) free(c->bindings[i].arg);
					c->nbindings = 0;
				}
				continue;
			}
			add_binding(c, k, v);
		} else if (!strcasecmp(section, "autostart")) {
			if (!autostart_reset) {
				/* an [autostart] section replaces the defaults */
				for (int i = 0; i < c->nautostart; i++) free(c->autostart[i]);
				c->nautostart = 0;
				autostart_reset = true;
			}
			if (*v && c->nautostart < FLWM_MAX_AUTOSTART)
				c->autostart[c->nautostart++] = strdup(v);
		}
	}
	fclose(f);
	return true;
}

/* an id for the shell: letters, digits and . _ - only */
static bool plain_id(const char *s) {
	if (!*s || strlen(s) > 200) return false;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && !strchr("._-", *s)) return false;
	return true;
}

void config_load_global_shortcuts(struct flwm_config *c) {
	char path[1024];
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && *xdg) snprintf(path, sizeof path, "%s/fldesktop/global-shortcuts.ini", xdg);
	else snprintf(path, sizeof path, "%s/.config/fldesktop/global-shortcuts.ini", getenv("HOME") ? getenv("HOME") : "/");
	FILE *f = fopen(path, "r");
	if (!f) return;
	char line[1024], app[256] = "";
	while (fgets(line, sizeof line, f)) {
		char *l = trim(line);
		if (!*l || *l == '#' || *l == ';') continue;
		if (*l == '[') {
			char *e = strchr(l, ']');
			if (e) *e = 0;
			snprintf(app, sizeof app, "%s", l + 1);
			continue;
		}
		char *eq = strchr(l, '=');
		if (!eq || !*app) continue;
		*eq = 0;
		char *id = trim(l), *v = trim(eq + 1);
		char *bar = strchr(v, '|');
		if (bar) *bar = 0;
		char *combo = trim(v);
		if (!*combo || !plain_id(app) || !plain_id(id)) continue;
		/* parse the combination like a binding, but keep what is bound already */
		struct flwm_config probe;
		memset(&probe, 0, sizeof probe);
		add_binding(&probe, combo, "close");
		if (probe.nbindings != 1) continue;
		bool taken = false;
		for (int i = 0; i < c->nbindings; i++)
			if (c->bindings[i].modifiers == probe.bindings[0].modifiers && c->bindings[i].sym == probe.bindings[0].sym)
				taken = true;
		free(probe.bindings[0].arg);
		if (taken) {
			wlr_log(WLR_INFO, "global shortcut %s of %s: %s is taken", id, app, combo);
			continue;
		}
		char action[512];
		snprintf(action, sizeof action, "portal-shortcut %s\x1f%s", app, id);
		add_binding(c, combo, action);
	}
	fclose(f);
}

void config_free(struct flwm_config *c) {
	free(c->font);
	free(c->kb_layout);
	free(c->kb_variant);
	free(c->kb_options);
	free(c->kb_model);
	free(c->cursor_theme);
	free(c->launcher_cmd);
	free(c->lock_cmd);
	free(c->scroll_method);
	free(c->gesture_up.arg);
	free(c->gesture_down.arg);
	for (int i = 0; i < 4; i++) free(c->hot_corner[i].arg);
	free(c->path);
	for (int i = 0; i < c->nbindings; i++) free(c->bindings[i].arg);
	for (int i = 0; i < c->nautostart; i++) free(c->autostart[i]);
	for (int i = 0; i < c->noutputs; i++) free(c->outputs[i].name);
	memset(c, 0, sizeof *c);
}

char *config_default_path(void) {
	char buf[1024];
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && *xdg) snprintf(buf, sizeof buf, "%s/fldesktop/wm.ini", xdg);
	else snprintf(buf, sizeof buf, "%s/.config/fldesktop/wm.ini", getenv("HOME") ? getenv("HOME") : "/");
	return strdup(buf);
}
