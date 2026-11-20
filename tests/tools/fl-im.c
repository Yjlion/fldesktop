/* fl-im: a fake input method (input-method-unstable-v2) for the input method
 * smoke test. It prints what fl-wm tells it, one event per line:
 *   activate / deactivate / unavailable / key <code> <state>
 * and on every activation commits TEXT to the program with the focus.
 * With --grab it grabs the keyboard after the first activation.
 *
 *   fl-im [--grab] SECONDS TEXT */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "input-method-unstable-v2-client-protocol.h"

static struct wl_seat *seat;
static struct zwp_input_method_manager_v2 *mgr;
static struct zwp_input_method_v2 *im;
static struct zwp_input_method_keyboard_grab_v2 *grab;
static const char *text;
static int want_grab, active, pending_activate;
static uint32_t serial;

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
	if (!strcmp(iface, wl_seat_interface.name) && !seat)
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	else if (!strcmp(iface, zwp_input_method_manager_v2_interface.name))
		mgr = wl_registry_bind(r, name, &zwp_input_method_manager_v2_interface, 1);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = {global, global_remove};

static void say(const char *s) {
	printf("%s\n", s);
	fflush(stdout);
}

static void g_keymap(void *d, struct zwp_input_method_keyboard_grab_v2 *g, uint32_t fmt, int32_t fd, uint32_t size) {
	close(fd);
}
static void g_key(void *d, struct zwp_input_method_keyboard_grab_v2 *g, uint32_t s, uint32_t t, uint32_t key,
		uint32_t state) {
	printf("key %u %u\n", key, state);
	fflush(stdout);
}
static void g_mods(void *d, struct zwp_input_method_keyboard_grab_v2 *g, uint32_t s, uint32_t a, uint32_t b,
		uint32_t c, uint32_t e) {}
static void g_repeat(void *d, struct zwp_input_method_keyboard_grab_v2 *g, int32_t r, int32_t dl) {}
static const struct zwp_input_method_keyboard_grab_v2_listener grab_listener = {g_keymap, g_key, g_mods, g_repeat};

static void im_activate(void *d, struct zwp_input_method_v2 *m) { pending_activate = 1; }
static void im_deactivate(void *d, struct zwp_input_method_v2 *m) { pending_activate = -1; }
static void im_surrounding(void *d, struct zwp_input_method_v2 *m, const char *t, uint32_t c, uint32_t a) {}
static void im_cause(void *d, struct zwp_input_method_v2 *m, uint32_t c) {}
static void im_content(void *d, struct zwp_input_method_v2 *m, uint32_t h, uint32_t p) {}
static void im_done(void *d, struct zwp_input_method_v2 *m) {
	serial++;
	if (pending_activate == 1 && !active) {
		active = 1;
		say("activate");
		zwp_input_method_v2_commit_string(im, text);
		zwp_input_method_v2_commit(im, serial);
		if (want_grab && !grab) {
			grab = zwp_input_method_v2_grab_keyboard(im);
			zwp_input_method_keyboard_grab_v2_add_listener(grab, &grab_listener, NULL);
		}
	} else if (pending_activate == -1 && active) {
		active = 0;
		say("deactivate");
	}
	pending_activate = 0;
}
static void im_unavailable(void *d, struct zwp_input_method_v2 *m) { say("unavailable"); }
static const struct zwp_input_method_v2_listener im_listener = {
	im_activate, im_deactivate, im_surrounding, im_cause, im_content, im_done, im_unavailable,
};

int main(int argc, char **argv) {
	int i = 1;
	if (i < argc && !strcmp(argv[i], "--grab")) {
		want_grab = 1;
		i++;
	}
	if (argc - i < 2) {
		fprintf(stderr, "usage: fl-im [--grab] SECONDS TEXT\n");
		return 2;
	}
	double secs = atof(argv[i]);
	text = argv[i + 1];
	struct wl_display *d = wl_display_connect(NULL);
	if (!d) return 1;
	wl_registry_add_listener(wl_display_get_registry(d), &reg_listener, NULL);
	wl_display_roundtrip(d);
	if (!mgr || !seat) {
		fprintf(stderr, "fl-im: no input-method-unstable-v2\n");
		return 1;
	}
	im = zwp_input_method_manager_v2_get_input_method(mgr, seat);
	zwp_input_method_v2_add_listener(im, &im_listener, NULL);
	say("ready");
	struct timespec t0, now;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		double left = secs - (now.tv_sec - t0.tv_sec) - (now.tv_nsec - t0.tv_nsec) / 1e9;
		if (left <= 0) break;
		wl_display_flush(d);
		struct pollfd p = {wl_display_get_fd(d), POLLIN, 0};
		if (poll(&p, 1, (int)(left * 1000)) > 0 && wl_display_dispatch(d) < 0) break;
	}
	return 0;
}
