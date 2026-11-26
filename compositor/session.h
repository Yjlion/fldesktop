/* fl-wm: remembering the open windows for the next session.
 *
 * At the end of a session ([behaviour] restore_session=true) the normal
 * windows are written to $XDG_STATE_HOME/fldesktop/session.ini: app id,
 * workspace, output, geometry, maximized. fl-autostart starts the programs
 * again at the next login, and fl-wm puts each window that maps with one of
 * the app ids back where it was (each entry is used once). No wlroots here
 * (unit tested). */
#pragma once
#include <stdbool.h>

#define FLWM_MAX_SESSION 64

struct flwm_session_entry {
	char app_id[128];
	char output[32];
	int workspace; /* 0-based */
	int x, y, w, h; /* content geometry in layout coordinates */
	bool maximized, fullscreen;
	bool used;
};

/* the file: $XDG_STATE_HOME/fldesktop/session.ini (or ~/.local/state/...) */
void session_path(char *buf, int size);
bool session_write(const char *path, const struct flwm_session_entry *e, int n);
/* returns the number of entries read (0 when there is no file) */
int session_read(const char *path, struct flwm_session_entry *e, int max);
/* the first unused entry for app_id (marked used), or NULL */
struct flwm_session_entry *session_take(struct flwm_session_entry *e, int n, const char *app_id);
