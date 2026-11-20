/* fl-toplevel: a plain window for compositor tests. Prints events, one per
 * line:
 *   enter main|dialog          the keyboard focus came to a window
 *   key N STATE                a key reached it
 *   inhibit active|inactive    keyboard-shortcuts-inhibit state
 *
 *   fl-toplevel [--inhibit] [--modal] [--icon NAME] SECONDS
 * --inhibit asks for the key bindings; --modal also opens a modal dialog
 * (xdg-dialog-v1) over the window; --icon names the window's icon
 * (xdg-toplevel-icon-v1). */
#define _GNU_SOURCE
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#ifdef HAVE_XDG_DIALOG
#include "xdg-dialog-v1-client-protocol.h"
#endif
#ifdef HAVE_TOPLEVEL_ICON
#include "xdg-toplevel-icon-v1-client-protocol.h"
#endif

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm_base;
static struct zwp_keyboard_shortcuts_inhibit_manager_v1 *inhibit_mgr;
#ifdef HAVE_XDG_DIALOG
static struct xdg_wm_dialog_v1 *dialog_mgr;
#endif
#ifdef HAVE_TOPLEVEL_ICON
static struct xdg_toplevel_icon_manager_v1 *icon_mgr;
#endif

struct win {
	const char *name;
	int w, h;
	uint32_t color;
	struct wl_surface *surface;
	struct xdg_surface *xs;
	struct xdg_toplevel *top;
	int configured;
};
static struct win main_win = {"main", 320, 240, 0xff3366cc};
static struct win dialog_win = {"dialog", 200, 120, 0xffcc6633};

static void say(const char *fmt, const char *a, unsigned b, unsigned c) {
	printf(fmt, a, b, c);
	printf("\n");
	fflush(stdout);
}

static struct wl_buffer *make_buffer(int w, int h, uint32_t color) {
	int stride = w * 4, size = stride * h;
	int fd = memfd_create("fl-toplevel", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) return NULL;
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (int i = 0; i < w * h; i++) px[i] = color;
	munmap(px, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return b;
}

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial) {
	struct win *w = d;
	xdg_surface_ack_configure(xs, serial);
	if (!w->configured) {
		w->configured = 1;
		wl_surface_attach(w->surface, make_buffer(w->w, w->h, w->color), 0, 0);
		wl_surface_damage_buffer(w->surface, 0, 0, w->w, w->h);
	}
	wl_surface_commit(w->surface);
}
static const struct xdg_surface_listener xs_listener = {xs_configure};
static void top_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) {}
static void top_close(void *d, struct xdg_toplevel *t) {}
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) {}
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) {}
static const struct xdg_toplevel_listener top_listener = {top_configure, top_close, top_bounds, top_caps};

static void make_window(struct win *w, struct xdg_toplevel *parent) {
	w->surface = wl_compositor_create_surface(compositor);
	w->xs = xdg_wm_base_get_xdg_surface(wm_base, w->surface);
	xdg_surface_add_listener(w->xs, &xs_listener, w);
	w->top = xdg_surface_get_toplevel(w->xs);
	xdg_toplevel_add_listener(w->top, &top_listener, w);
	xdg_toplevel_set_title(w->top, w->name);
	xdg_toplevel_set_app_id(w->top, "fl-toplevel-test");
	if (parent) xdg_toplevel_set_parent(w->top, parent);
}

static void ping(void *d, struct xdg_wm_base *b, uint32_t serial) { xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener base_listener = {ping};

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t s) { close(fd); }
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *surf, struct wl_array *keys) {
	say("enter %s", surf == dialog_win.surface ? "dialog" : "main", 0, 0);
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *surf) {}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key, uint32_t state) {
	say("key%s %u %u", "", key, state);
}
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t g) {}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t dl) {}
static const struct wl_keyboard_listener kb_listener = {kb_keymap, kb_enter, kb_leave, kb_key, kb_mods, kb_repeat};

static void inh_active(void *d, struct zwp_keyboard_shortcuts_inhibitor_v1 *i) { say("inhibit active%s", "", 0, 0); }
static void inh_inactive(void *d, struct zwp_keyboard_shortcuts_inhibitor_v1 *i) { say("inhibit inactive%s", "", 0, 0); }
static const struct zwp_keyboard_shortcuts_inhibitor_v1_listener inh_listener = {inh_active, inh_inactive};

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
	if (!strcmp(iface, wl_compositor_interface.name)) compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, wl_seat_interface.name) && !seat) seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm_base, &base_listener, NULL);
	} else if (!strcmp(iface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name))
		inhibit_mgr = wl_registry_bind(r, name, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
#ifdef HAVE_XDG_DIALOG
	else if (!strcmp(iface, xdg_wm_dialog_v1_interface.name))
		dialog_mgr = wl_registry_bind(r, name, &xdg_wm_dialog_v1_interface, 1);
#endif
#ifdef HAVE_TOPLEVEL_ICON
	else if (!strcmp(iface, xdg_toplevel_icon_manager_v1_interface.name))
		icon_mgr = wl_registry_bind(r, name, &xdg_toplevel_icon_manager_v1_interface, 1);
#endif
}
static void global_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = {global, global_remove};

int main(int argc, char **argv) {
	int inhibit = 0, modal = 0;
	const char *icon = NULL;
	double secs = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--inhibit")) inhibit = 1;
		else if (!strcmp(argv[i], "--modal")) modal = 1;
		else if (!strcmp(argv[i], "--icon") && i + 1 < argc) icon = argv[++i];
		else secs = atof(argv[i]);
	}
	struct wl_display *d = wl_display_connect(NULL);
	if (!d) return 1;
	wl_registry_add_listener(wl_display_get_registry(d), &reg_listener, NULL);
	wl_display_roundtrip(d);
	if (!compositor || !shm || !seat || !wm_base) return 1;
	struct wl_keyboard *kb = wl_seat_get_keyboard(seat);
	wl_keyboard_add_listener(kb, &kb_listener, NULL);
	make_window(&main_win, NULL);
#ifdef HAVE_TOPLEVEL_ICON
	if (icon && icon_mgr) {
		struct xdg_toplevel_icon_v1 *ic = xdg_toplevel_icon_manager_v1_create_icon(icon_mgr);
		xdg_toplevel_icon_v1_set_name(ic, icon);
		xdg_toplevel_icon_manager_v1_set_icon(icon_mgr, main_win.top, ic);
		xdg_toplevel_icon_v1_destroy(ic);
	} else if (icon) {
		fprintf(stderr, "fl-toplevel: no xdg-toplevel-icon\n");
	}
#else
	(void)icon;
#endif
	wl_surface_commit(main_win.surface);
	if (inhibit) {
		if (!inhibit_mgr) {
			fprintf(stderr, "fl-toplevel: no keyboard-shortcuts-inhibit\n");
			return 1;
		}
		struct zwp_keyboard_shortcuts_inhibitor_v1 *in =
			zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(inhibit_mgr, main_win.surface, seat);
		zwp_keyboard_shortcuts_inhibitor_v1_add_listener(in, &inh_listener, NULL);
	}
	wl_display_roundtrip(d);
	wl_display_roundtrip(d);
	if (modal) {
#ifdef HAVE_XDG_DIALOG
		if (!dialog_mgr) {
			fprintf(stderr, "fl-toplevel: no xdg-dialog\n");
			return 3;
		}
		make_window(&dialog_win, main_win.top);
		struct xdg_dialog_v1 *dl = xdg_wm_dialog_v1_get_xdg_dialog(dialog_mgr, dialog_win.top);
		xdg_dialog_v1_set_modal(dl);
		wl_surface_commit(dialog_win.surface);
#else
		fprintf(stderr, "fl-toplevel: built without xdg-dialog\n");
		return 3;
#endif
	}
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
