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
		{"snap-right", FLWM_ACTION_SNAP_RIGHT}, {"launcher", FLWM_ACTION_LAUNCHER},
		{"quit", FLWM_ACTION_QUIT}, {"reload", FLWM_ACTION_RELOAD},
		{"show-desktop", FLWM_ACTION_SHOW_DESKTOP},
	};
	enum flwm_action_type type = FLWM_ACTION_NONE;
	for (size_t i = 0; i < sizeof actions / sizeof actions[0]; i++)
		if (!strcasecmp(a, actions[i].name)) type = actions[i].type;
	if (type == FLWM_ACTION_NONE) {
		wlr_log(WLR_ERROR, "config: unknown action '%s'", a);
		return;
	}
	/* replace an existing binding for the same combo */
	for (int i = 0; i < c->nbindings; i++) {
		struct flwm_binding *b = &c->bindings[i];
		if (b->modifiers == mods && b->sym == sym) {
			free(b->arg);
			b->action = type;
			b->arg = xstrdup(arg);
			return;
		}
	}
	struct flwm_binding *b = &c->bindings[c->nbindings++];
	b->modifiers = mods;
	b->sym = sym;
	b->action = type;
	b->arg = xstrdup(arg);
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

	add_binding(c, "Super+Return", "exec fl-term");
	add_binding(c, "Super+t", "exec fl-term");
	add_binding(c, "Super+e", "exec fl-files");
	add_binding(c, "Super+space", "launcher");
	add_binding(c, "Super+d", "show-desktop");
	add_binding(c, "Super+q", "close");
	add_binding(c, "Alt+F4", "close");
	add_binding(c, "Super+Up", "maximize");
	add_binding(c, "Super+Down", "minimize");
	add_binding(c, "Super+Left", "snap-left");
	add_binding(c, "Super+Right", "snap-right");
	add_binding(c, "Super+f", "fullscreen");
	add_binding(c, "F11", "fullscreen");
	add_binding(c, "Alt+Tab", "cycle");
	add_binding(c, "Alt+Shift+Tab", "cycle-back");
	add_binding(c, "Super+Shift+r", "reload");
	add_binding(c, "Ctrl+Alt+BackSpace", "quit");
	add_binding(c, "Print", "exec fl-screenshot");
	add_binding(c, "Shift+Print", "exec fl-screenshot --area");

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
			else if (!strcmp(k, "launcher")) set_str(&c->launcher_cmd, v);
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
		} else if (!strcasecmp(section, "cursor")) {
			if (!strcmp(k, "theme")) set_str(&c->cursor_theme, v);
			else if (!strcmp(k, "size")) c->cursor_size = atoi(v);
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

void config_free(struct flwm_config *c) {
	free(c->font);
	free(c->kb_layout);
	free(c->kb_variant);
	free(c->kb_options);
	free(c->kb_model);
	free(c->cursor_theme);
	free(c->launcher_cmd);
	free(c->scroll_method);
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
