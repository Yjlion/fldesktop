/* fl-wm: remembering the open windows (see session.h) */
#define _POSIX_C_SOURCE 200809L
#include "session.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void session_path(char *buf, int size) {
	const char *st = getenv("XDG_STATE_HOME");
	if (st && *st) snprintf(buf, (size_t)size, "%s/fldesktop/session.ini", st);
	else snprintf(buf, (size_t)size, "%s/.local/state/fldesktop/session.ini", getenv("HOME") ? getenv("HOME") : "/tmp");
}

static void mkdirs(const char *path) {
	char d[1024];
	snprintf(d, sizeof d, "%s", path);
	for (char *p = d + 1; *p; p++) {
		if (*p != '/') continue;
		*p = 0;
		mkdir(d, 0700);
		*p = '/';
	}
}

/* app ids and output names go into an INI file: no newlines or brackets */
static bool clean(const char *s) {
	if (!s || !*s) return false;
	for (; *s; s++)
		if (*s == '\n' || *s == '\r' || *s == '[' || *s == ']' || *s == '=') return false;
	return true;
}

bool session_write(const char *path, const struct flwm_session_entry *e, int n) {
	mkdirs(path);
	char tmp[1100];
	snprintf(tmp, sizeof tmp, "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f) return false;
	fprintf(f, "# the windows open when the last fldesktop session ended (fl-wm)\n");
	int k = 0;
	for (int i = 0; i < n; i++) {
		if (!clean(e[i].app_id)) continue;
		fprintf(f, "\n[window%d]\napp_id=%s\nworkspace=%d\ngeometry=%d,%d,%dx%d\nmaximized=%s\nfullscreen=%s\n", k++,
			e[i].app_id, e[i].workspace + 1, e[i].x, e[i].y, e[i].w, e[i].h, e[i].maximized ? "true" : "false",
			e[i].fullscreen ? "true" : "false");
		if (clean(e[i].output)) fprintf(f, "output=%s\n", e[i].output);
	}
	bool ok = fclose(f) == 0;
	if (!ok || rename(tmp, path) != 0) {
		remove(tmp);
		return false;
	}
	return true;
}

static char *trim(char *s) {
	while (*s == ' ' || *s == '\t') s++;
	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
	return s;
}

int session_read(const char *path, struct flwm_session_entry *e, int max) {
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	int n = 0;
	struct flwm_session_entry *cur = NULL;
	char line[512];
	while (fgets(line, sizeof line, f)) {
		char *l = trim(line);
		if (!*l || *l == '#') continue;
		if (*l == '[') {
			cur = n < max ? &e[n++] : NULL;
			if (cur) memset(cur, 0, sizeof *cur);
			continue;
		}
		char *eq = strchr(l, '=');
		if (!cur || !eq) continue;
		*eq = 0;
		char *k = trim(l), *v = trim(eq + 1);
		if (!strcmp(k, "app_id")) snprintf(cur->app_id, sizeof cur->app_id, "%s", v);
		else if (!strcmp(k, "output")) snprintf(cur->output, sizeof cur->output, "%s", v);
		else if (!strcmp(k, "workspace")) cur->workspace = atoi(v) > 0 ? atoi(v) - 1 : 0;
		else if (!strcmp(k, "geometry")) {
			if (sscanf(v, "%d,%d,%dx%d", &cur->x, &cur->y, &cur->w, &cur->h) != 4) cur->w = cur->h = 0;
		} else if (!strcmp(k, "maximized")) cur->maximized = !strcmp(v, "true");
		else if (!strcmp(k, "fullscreen")) cur->fullscreen = !strcmp(v, "true");
	}
	fclose(f);
	/* entries without an app id are of no use */
	int k = 0;
	for (int i = 0; i < n; i++)
		if (e[i].app_id[0]) e[k++] = e[i];
	return k;
}

struct flwm_session_entry *session_take(struct flwm_session_entry *e, int n, const char *app_id) {
	if (!app_id) return NULL;
	for (int i = 0; i < n; i++)
		if (!e[i].used && !strcmp(e[i].app_id, app_id)) {
			e[i].used = true;
			return &e[i];
		}
	return NULL;
}
