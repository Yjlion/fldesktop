/* fl-wm: window rules (see rules.h) */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "rules.h"

#include <ctype.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void rule_init(struct flwm_rule *r) {
	memset(r, 0, sizeof *r);
	r->maximized = r->fullscreen = r->sticky = r->above = -1;
}

static int tri(const char *v) {
	if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcmp(v, "1") || !strcasecmp(v, "on")) return 1;
	if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcmp(v, "0") || !strcasecmp(v, "off")) return 0;
	return -1;
}

static void set_str(char **dst, const char *v) {
	free(*dst);
	*dst = *v ? strdup(v) : NULL;
}

bool rule_set(struct flwm_rule *r, const char *key, const char *v) {
	if (!strcmp(key, "app_id")) set_str(&r->app_id, v);
	else if (!strcmp(key, "title")) set_str(&r->title, v);
	else if (!strcmp(key, "output")) set_str(&r->output, v);
	else if (!strcmp(key, "workspace")) {
		char *end;
		long n = strtol(v, &end, 10);
		if (*end || n < 1 || n > 99) return false;
		r->workspace = (int)n;
	} else if (!strcmp(key, "size")) {
		int w, h;
		char extra;
		if (sscanf(v, "%dx%d%c", &w, &h, &extra) != 2 || w < 50 || h < 50 || w > 16384 || h > 16384)
			return false;
		r->width = w;
		r->height = h;
	} else if (!strcmp(key, "position")) {
		int x, y;
		char extra;
		if (!strcasecmp(v, "center") || !strcasecmp(v, "centre")) {
			r->has_position = true;
			r->center = true;
		} else if (sscanf(v, "%d,%d%c", &x, &y, &extra) == 2 && x > -16384 && y > -16384 && x < 16384 && y < 16384) {
			r->has_position = true;
			r->center = false;
			r->x = x;
			r->y = y;
		} else {
			return false;
		}
	} else if (!strcmp(key, "maximized") || !strcmp(key, "fullscreen") || !strcmp(key, "sticky") ||
		   !strcmp(key, "always_on_top")) {
		int t = tri(v);
		if (t < 0) return false;
		if (!strcmp(key, "maximized")) r->maximized = t;
		else if (!strcmp(key, "fullscreen")) r->fullscreen = t;
		else if (!strcmp(key, "sticky")) r->sticky = t;
		else r->above = t;
	} else if (!strcmp(key, "opacity")) {
		char *end;
		float o = strtof(v, &end);
		if (*end || !(o >= 0.1f && o <= 1.0f)) return false;
		r->opacity = o;
	} else {
		return false;
	}
	return true;
}

void rule_free(struct flwm_rule *r) {
	free(r->app_id);
	free(r->title);
	free(r->output);
	rule_init(r);
}

static bool glob(const char *pat, const char *s) {
	if (!pat) return true;
	return fnmatch(pat, s ? s : "", FNM_CASEFOLD) == 0;
}

bool rule_matches(const struct flwm_rule *r, const char *app_id, const char *title) {
	if (!r->app_id && !r->title) return false; /* a rule must say what it is for */
	return glob(r->app_id, app_id) && glob(r->title, title);
}

void rules_apply(const struct flwm_rule *rules, int n, const char *app_id, const char *title,
	struct flwm_rule_result *out) {
	memset(out, 0, sizeof *out);
	out->maximized = out->fullscreen = out->sticky = out->above = -1;
	for (int i = 0; i < n; i++) {
		const struct flwm_rule *r = &rules[i];
		if (!rule_matches(r, app_id, title)) continue;
		out->matched++;
		if (r->workspace) out->workspace = r->workspace;
		if (r->output) out->output = r->output;
		if (r->width) {
			out->width = r->width;
			out->height = r->height;
		}
		if (r->has_position) {
			out->has_position = true;
			out->center = r->center;
			out->x = r->x;
			out->y = r->y;
		}
		if (r->maximized >= 0) out->maximized = r->maximized;
		if (r->fullscreen >= 0) out->fullscreen = r->fullscreen;
		if (r->sticky >= 0) out->sticky = r->sticky;
		if (r->above >= 0) out->above = r->above;
		if (r->opacity > 0) out->opacity = r->opacity;
	}
}
