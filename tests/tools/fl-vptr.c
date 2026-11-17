/* fl-vptr: moves the pointer and clicks through wlr-virtual-pointer, for
 * automated UI tests in a headless fl-wm session.
 *
 *   fl-vptr [-s WxH] click X Y [button]   move to X,Y (layout coordinates) and click
 *   fl-vptr [-s WxH] move X Y
 *   fl-vptr [-s WxH] drag X1 Y1 X2 Y2
 *   fl-vptr scroll DY
 * The screen size defaults to 1280x720 (the headless output). */
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct wl_seat *seat;
static struct zwlr_virtual_pointer_manager_v1 *mgr;

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
	if (!strcmp(iface, wl_seat_interface.name) && !seat)
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	else if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name))
		mgr = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = {global, global_remove};

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static struct wl_display *dpy;
static struct zwlr_virtual_pointer_v1 *ptr;
static int W = 1280, H = 720;

static void move_to(int x, int y) {
	zwlr_virtual_pointer_v1_motion_absolute(ptr, now_ms(), x, y, W, H);
	zwlr_virtual_pointer_v1_frame(ptr);
	wl_display_roundtrip(dpy);
}

static void button(int b, int pressed) {
	zwlr_virtual_pointer_v1_button(ptr, now_ms(), b,
		pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
	zwlr_virtual_pointer_v1_frame(ptr);
	wl_display_roundtrip(dpy);
}

static int usage(void) {
	fprintf(stderr, "usage: fl-vptr [-s WxH] click X Y [left|right|middle] | hold X Y MS | move X Y | drag X1 Y1 X2 Y2 | scroll DY\n");
	return 2;
}

int main(int argc, char **argv) {
	int i = 1;
	if (i + 1 < argc && !strcmp(argv[i], "-s")) {
		if (sscanf(argv[i + 1], "%dx%d", &W, &H) != 2) return usage();
		i += 2;
	}
	if (i >= argc) return usage();
	dpy = wl_display_connect(NULL);
	if (!dpy) { fprintf(stderr, "fl-vptr: no Wayland display\n"); return 1; }
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(dpy);
	if (!mgr) { fprintf(stderr, "fl-vptr: compositor lacks wlr-virtual-pointer\n"); return 1; }
	ptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(mgr, seat);
	wl_display_roundtrip(dpy);
	const char *cmd = argv[i++];
	if (!strcmp(cmd, "click") && i + 1 < argc) {
		int b = BTN_LEFT;
		if (i + 2 < argc) b = !strcmp(argv[i + 2], "right") ? BTN_RIGHT : !strcmp(argv[i + 2], "middle") ? BTN_MIDDLE : BTN_LEFT;
		move_to(atoi(argv[i]), atoi(argv[i + 1]));
		usleep(30000);
		button(b, 1);
		usleep(30000);
		button(b, 0);
	} else if (!strcmp(cmd, "hold") && i + 2 < argc) {
		/* press, keep the button down for MS milliseconds, release */
		move_to(atoi(argv[i]), atoi(argv[i + 1]));
		usleep(30000);
		button(BTN_LEFT, 1);
		usleep(atoi(argv[i + 2]) * 1000);
		button(BTN_LEFT, 0);
	} else if (!strcmp(cmd, "move") && i + 1 < argc) {
		move_to(atoi(argv[i]), atoi(argv[i + 1]));
	} else if (!strcmp(cmd, "drag") && i + 3 < argc) {
		int x1 = atoi(argv[i]), y1 = atoi(argv[i + 1]), x2 = atoi(argv[i + 2]), y2 = atoi(argv[i + 3]);
		move_to(x1, y1);
		button(BTN_LEFT, 1);
		for (int s = 1; s <= 10; s++) {
			move_to(x1 + (x2 - x1) * s / 10, y1 + (y2 - y1) * s / 10);
			usleep(15000);
		}
		button(BTN_LEFT, 0);
	} else if (!strcmp(cmd, "scroll") && i < argc) {
		zwlr_virtual_pointer_v1_axis(ptr, now_ms(), WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(atoi(argv[i]) * 15));
		zwlr_virtual_pointer_v1_frame(ptr);
		wl_display_roundtrip(dpy);
	} else {
		return usage();
	}
	usleep(50000);
	zwlr_virtual_pointer_v1_destroy(ptr);
	wl_display_roundtrip(dpy);
	wl_display_disconnect(dpy);
	return 0;
}
