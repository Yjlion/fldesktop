/* fl-wm: screen locking
 *
 * While locked, an opaque cover above every other layer hides the session
 * and only the lock client gets input. There are two kinds of lock client:
 *
 *  - fl-lock, started by fl-wm itself ("fl-lock --locker"). Its window gets
 *    the lock role only if it belongs to that very process, and the session
 *    is unlocked only when that process exits with status 0 (it does after a
 *    successful PAM authentication). If it crashes it is started again; the
 *    cover stays up meanwhile.
 *  - any ext-session-lock-v1 client (swaylock, ...). If it goes away without
 *    unlocking, fl-lock takes over.
 *
 * A lock is requested with SIGUSR1 (fl-lock without options sends it), the
 * "lock" key binding action, or the idle timeout ([lock] idle_minutes).
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/util/log.h>

#include "server.h"

struct flwm_lock_surface {
	struct wl_list link; /* flwm_server.lock_surfaces */
	struct flwm_server *server;
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct wlr_scene_tree *tree;
	struct wl_listener destroy;
	struct wl_listener map;
};

static void start_locker(struct flwm_server *server);

static float *cover_color(struct flwm_server *server, float out[4]) {
	uint32_t c = server->config.lock_color;
	out[0] = ((c >> 16) & 255) / 255.0f;
	out[1] = ((c >> 8) & 255) / 255.0f;
	out[2] = (c & 255) / 255.0f;
	out[3] = 1;
	return out;
}

/* the cover spans the whole output layout; lock surfaces go on top */
void lock_arrange(struct flwm_server *server) {
	if (!server->layer_lock) return;
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, NULL, &box);
	float col[4];
	if (!server->lock_cover)
		server->lock_cover = wlr_scene_rect_create(server->layer_lock, box.width, box.height, cover_color(server, col));
	wlr_scene_rect_set_size(server->lock_cover, box.width, box.height);
	wlr_scene_rect_set_color(server->lock_cover, cover_color(server, col));
	wlr_scene_node_set_position(&server->lock_cover->node, box.x, box.y);
	wlr_scene_node_lower_to_bottom(&server->lock_cover->node);
	struct flwm_lock_surface *ls;
	wl_list_for_each(ls, &server->lock_surfaces, link) {
		struct wlr_output *o = ls->lock_surface->output;
		struct wlr_box ob;
		wlr_output_layout_get_box(server->output_layout, o, &ob);
		wlr_scene_node_set_position(&ls->tree->node, ob.x, ob.y);
		wlr_session_lock_surface_v1_configure(ls->lock_surface, ob.width, ob.height);
	}
	struct flwm_view *v = view_find_role(server, FLWM_ROLE_LOCK);
	if (v) view_reposition_shell(v);
}

static void set_locked(struct flwm_server *server, bool locked) {
	if (server->locked == locked) return;
	server->locked = locked;
	wlr_log(WLR_INFO, "%s", locked ? "session locked" : "session unlocked");
	wlr_scene_node_set_enabled(&server->layer_lock->node, locked);
	if (locked) {
		/* nothing below the cover keeps the keyboard or an ongoing grab */
		input_reset_cursor_mode(server);
		server->focused_layer = NULL;
		wlr_seat_keyboard_notify_clear_focus(server->seat);
		wlr_seat_pointer_notify_clear_focus(server->seat);
		lock_arrange(server);
	} else {
		view_focus_next_available(server);
	}
	if (server->idle_notifier) wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, locked ? false : server->idle_inhibitors > 0);
}

/* gives the keyboard to the lock client, if it has a surface yet */
static void focus_lock_client(struct flwm_server *server) {
	struct wlr_surface *s = NULL;
	struct flwm_view *v = view_find_role(server, FLWM_ROLE_LOCK);
	if (v) s = view_surface(v);
	else if (!wl_list_empty(&server->lock_surfaces)) {
		struct flwm_lock_surface *ls = wl_container_of(server->lock_surfaces.next, ls, link);
		if (ls->lock_surface->surface->mapped) s = ls->lock_surface->surface;
	}
	if (!s) return;
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
	if (kb) wlr_seat_keyboard_notify_enter(server->seat, s, kb->keycodes, kb->num_keycodes, &kb->modifiers);
	else wlr_seat_keyboard_notify_enter(server->seat, s, NULL, 0, NULL);
}

void lock_view_mapped(struct flwm_server *server, struct flwm_view *view) {
	(void)view;
	if (server->locked) focus_lock_client(server);
}

bool lock_client_is_locker(struct flwm_server *server, struct wl_client *client) {
	if (server->lock_pid <= 0) return false;
	pid_t pid;
	wl_client_get_credentials(client, &pid, NULL, NULL);
	return pid == server->lock_pid;
}

/* ---- fl-lock as our own child ------------------------------------------- */

static int restart_locker(void *data) {
	struct flwm_server *server = data;
	if (server->locked && !server->ext_lock && server->lock_pid <= 0) start_locker(server);
	return 0;
}

static void start_locker(struct flwm_server *server) {
	const char *cmd = server->config.lock_cmd ? server->config.lock_cmd : "fl-lock --locker";
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log_errno(WLR_ERROR, "fork");
		return;
	}
	if (pid == 0) {
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		setsid();
		/* exec (no shell) so the pid is that of the lock client itself */
		char buf[1024];
		snprintf(buf, sizeof buf, "%s", cmd);
		char *argv[16];
		int n = 0;
		for (char *tok = strtok(buf, " \t"); tok && n < 15; tok = strtok(NULL, " \t")) argv[n++] = tok;
		argv[n] = NULL;
		if (n > 0) execvp(argv[0], argv);
		_exit(127);
	}
	server->lock_pid = pid;
	server->lock_started = time(NULL);
	wlr_log(WLR_INFO, "started the screen locker (pid %d)", (int)pid);
}

/* SIGCHLD: did our locker finish? */
static int handle_sigchld(int sig, void *data) {
	struct flwm_server *server = data;
	(void)sig;
	if (server->lock_pid <= 0) return 0;
	int status;
	pid_t r = waitpid(server->lock_pid, &status, WNOHANG);
	if (r != server->lock_pid) return 0;
	server->lock_pid = 0;
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		if (!server->ext_lock) set_locked(server, false);
		return 0;
	}
	wlr_log(WLR_ERROR, "the screen locker ended without unlocking (status %d); restarting it", status);
	if (server->locked && !server->ext_lock) {
		/* back off if it keeps failing right away */
		int delay = time(NULL) - server->lock_started < 3 ? 2000 : 100;
		wl_event_source_timer_update(server->lock_restart_timer, delay);
	}
	return 0;
}

void lock_request(struct flwm_server *server) {
	if (server->locked) return;
	set_locked(server, true);
	start_locker(server);
}

/* ---- ext-session-lock-v1 ------------------------------------------------- */

static void lock_surface_destroy(struct wl_listener *l, void *data) {
	struct flwm_lock_surface *ls = wl_container_of(l, ls, destroy);
	wl_list_remove(&ls->link);
	wl_list_remove(&ls->destroy.link);
	wl_list_remove(&ls->map.link);
	struct flwm_server *server = ls->server;
	free(ls);
	focus_lock_client(server);
}

static void lock_surface_map(struct wl_listener *l, void *data) {
	struct flwm_lock_surface *ls = wl_container_of(l, ls, map);
	focus_lock_client(ls->server);
}

static void new_lock_surface(struct wl_listener *l, void *data) {
	struct flwm_server *server = wl_container_of(l, server, ext_new_surface);
	struct wlr_session_lock_surface_v1 *s = data;
	struct flwm_lock_surface *ls = calloc(1, sizeof *ls);
	ls->server = server;
	ls->lock_surface = s;
	ls->tree = wlr_scene_subsurface_tree_create(server->layer_lock, s->surface);
	ls->destroy.notify = lock_surface_destroy;
	wl_signal_add(&s->events.destroy, &ls->destroy);
	ls->map.notify = lock_surface_map;
	wl_signal_add(&s->surface->events.map, &ls->map);
	wl_list_insert(&server->lock_surfaces, &ls->link);
	lock_arrange(server);
}

static void ext_lock_finish(struct flwm_server *server) {
	wl_list_remove(&server->ext_new_surface.link);
	wl_list_remove(&server->ext_unlock.link);
	wl_list_remove(&server->ext_destroy.link);
	server->ext_lock = NULL;
}

static void ext_unlock(struct wl_listener *l, void *data) {
	struct flwm_server *server = wl_container_of(l, server, ext_unlock);
	ext_lock_finish(server);
	if (server->lock_pid <= 0) set_locked(server, false);
}

static void ext_destroy(struct wl_listener *l, void *data) {
	struct flwm_server *server = wl_container_of(l, server, ext_destroy);
	/* the lock client went away without unlocking: stay locked, use fl-lock */
	ext_lock_finish(server);
	wlr_log(WLR_ERROR, "the session lock client died; starting fl-lock");
	if (server->locked && server->lock_pid <= 0) start_locker(server);
}

static void new_ext_lock(struct wl_listener *l, void *data) {
	struct flwm_server *server = wl_container_of(l, server, new_ext_lock);
	struct wlr_session_lock_v1 *lock = data;
	if (server->ext_lock || server->locked) {
		/* only one lock at a time: refuse (the client gets "finished") */
		wlr_session_lock_v1_destroy(lock);
		return;
	}
	server->ext_lock = lock;
	server->ext_new_surface.notify = new_lock_surface;
	wl_signal_add(&lock->events.new_surface, &server->ext_new_surface);
	server->ext_unlock.notify = ext_unlock;
	wl_signal_add(&lock->events.unlock, &server->ext_unlock);
	server->ext_destroy.notify = ext_destroy;
	wl_signal_add(&lock->events.destroy, &server->ext_destroy);
	set_locked(server, true);
	wlr_session_lock_v1_send_locked(lock);
}

/* ---- idle timeout --------------------------------------------------------- */

static int idle_timeout(void *data) {
	struct flwm_server *server = data;
	if (server->idle_inhibitors == 0 && !server->locked) lock_request(server);
	return 0;
}

static int blank_timeout(void *data) {
	struct flwm_server *server = data;
	if (server->idle_inhibitors == 0) output_idle_blank(server);
	return 0;
}

void lock_activity(struct flwm_server *server) {
	if (server->idle_notifier && server->seat) wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
	int min = server->config.lock_idle_minutes;
	wl_event_source_timer_update(server->idle_timer, min > 0 ? min * 60000 : 0);
	if (server->blanked) output_idle_wake(server);
	if (server->blank_timer) {
		wl_event_source_timer_update(server->blank_timer, server->config.blank_ms);
	}
}

struct flwm_inhibitor {
	struct flwm_server *server;
	struct wl_listener destroy;
};

static void inhibitor_destroy(struct wl_listener *l, void *data) {
	struct flwm_inhibitor *in = wl_container_of(l, in, destroy);
	struct flwm_server *server = in->server;
	server->idle_inhibitors--;
	wl_list_remove(&in->destroy.link);
	free(in);
	wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, server->idle_inhibitors > 0 && !server->locked);
	lock_activity(server);
}

static void new_inhibitor(struct wl_listener *l, void *data) {
	struct flwm_server *server = wl_container_of(l, server, new_idle_inhibitor);
	struct wlr_idle_inhibitor_v1 *inhibitor = data;
	struct flwm_inhibitor *in = calloc(1, sizeof *in);
	in->server = server;
	in->destroy.notify = inhibitor_destroy;
	wl_signal_add(&inhibitor->events.destroy, &in->destroy);
	server->idle_inhibitors++;
	wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, !server->locked);
}

/* ---- setup ---------------------------------------------------------------- */

void lock_init(struct flwm_server *server) {
	wl_list_init(&server->lock_surfaces);
	server->layer_lock = wlr_scene_tree_create(&server->scene->tree);
	wlr_scene_node_set_enabled(&server->layer_lock->node, false);
	server->lock_mgr = wlr_session_lock_manager_v1_create(server->wl_display);
	server->new_ext_lock.notify = new_ext_lock;
	wl_signal_add(&server->lock_mgr->events.new_lock, &server->new_ext_lock);
	server->idle_notifier = wlr_idle_notifier_v1_create(server->wl_display);
	server->idle_inhibit_mgr = wlr_idle_inhibit_v1_create(server->wl_display);
	server->new_idle_inhibitor.notify = new_inhibitor;
	wl_signal_add(&server->idle_inhibit_mgr->events.new_inhibitor, &server->new_idle_inhibitor);
	server->idle_timer = wl_event_loop_add_timer(server->event_loop, idle_timeout, server);
	server->blank_timer = wl_event_loop_add_timer(server->event_loop, blank_timeout, server);
	server->lock_restart_timer = wl_event_loop_add_timer(server->event_loop, restart_locker, server);
	server->sigchld = wl_event_loop_add_signal(server->event_loop, SIGCHLD, handle_sigchld, server);
	lock_activity(server);
}

void lock_finish(struct flwm_server *server) {
	wl_list_remove(&server->new_ext_lock.link);
	wl_list_remove(&server->new_idle_inhibitor.link);
	if (server->ext_lock) ext_lock_finish(server);
	wl_event_source_remove(server->idle_timer);
	wl_event_source_remove(server->blank_timer);
	wl_event_source_remove(server->lock_restart_timer);
	wl_event_source_remove(server->sigchld);
	if (server->lock_pid > 0) kill(server->lock_pid, SIGTERM);
	/* the scene goes away before the outputs do (they call lock_arrange) */
	server->layer_lock = NULL;
	server->lock_cover = NULL;
}
