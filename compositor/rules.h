/* fl-wm: window rules, [rule:<name>] sections in wm.ini.
 *
 *   [rule:terminal]
 *   app_id = fl-term          # glob (fnmatch, case-insensitive); either or both
 *   title = *htop*
 *   workspace = 2             # 1-based
 *   output = HDMI-A-1
 *   size = 900x600
 *   position = center         # or X,Y in the output's usable area
 *   maximized = true
 *   fullscreen = false
 *   sticky = true             # on every workspace
 *   always_on_top = true
 *   opacity = 0.9             # 0.1 .. 1
 *
 * Rules are applied in file order when a window first maps; later rules
 * override what earlier ones set. No wlroots here (unit tested).
 */
#pragma once
#include <stdbool.h>

#define FLWM_MAX_RULES 64

struct flwm_rule {
	char *app_id, *title; /* globs; NULL matches anything */
	int workspace;        /* 0 = not set, else 1-based */
	char *output;
	int width, height;    /* 0 = not set */
	bool has_position, center;
	int x, y;
	signed char maximized, fullscreen, sticky, above; /* -1 = not set */
	float opacity;        /* 0 = not set */
};

/* what the matching rules ask for (the same fields, merged) */
struct flwm_rule_result {
	int workspace;
	const char *output;
	int width, height;
	bool has_position, center;
	int x, y;
	signed char maximized, fullscreen, sticky, above;
	float opacity;
	int matched; /* number of rules that matched */
};

void rule_init(struct flwm_rule *r);
/* one key of a [rule:...] section; false for an unknown key or bad value */
bool rule_set(struct flwm_rule *r, const char *key, const char *value);
void rule_free(struct flwm_rule *r);
bool rule_matches(const struct flwm_rule *r, const char *app_id, const char *title);
void rules_apply(const struct flwm_rule *rules, int n, const char *app_id, const char *title,
	struct flwm_rule_result *out);
