/* fl-power: prints the power mode of the first output whenever it changes
 * (wlr-output-power-management), for the DPMS smoke test.
 *
 *   fl-power SECONDS          print "on" / "off" for SECONDS
 *   fl-power set on|off       set the mode and exit */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>

#include "wlr-output-power-management-unstable-v1-client-protocol.h"

static struct wl_output *output;
static struct zwlr_output_power_manager_v1 *mgr;
static int failed;

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
	if (!strcmp(iface, wl_output_interface.name) && !output)
		output = wl_registry_bind(r, name, &wl_output_interface, 1);
	else if (!strcmp(iface, zwlr_output_power_manager_v1_interface.name))
		mgr = wl_registry_bind(r, name, &zwlr_output_power_manager_v1_interface, 1);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = {global, global_remove};

static void mode(void *d, struct zwlr_output_power_v1 *p, uint32_t m) {
	printf("%s\n", m == ZWLR_OUTPUT_POWER_V1_MODE_ON ? "on" : "off");
	fflush(stdout);
}
static void failed_ev(void *d, struct zwlr_output_power_v1 *p) {
	failed = 1;
	fprintf(stderr, "fl-power: failed\n");
}
static const struct zwlr_output_power_v1_listener power_listener = {mode, failed_ev};

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: fl-power SECONDS | fl-power set on|off\n");
		return 2;
	}
	struct wl_display *d = wl_display_connect(NULL);
	if (!d) return 1;
	wl_registry_add_listener(wl_display_get_registry(d), &reg_listener, NULL);
	wl_display_roundtrip(d);
	if (!mgr || !output) {
		fprintf(stderr, "fl-power: no wlr-output-power-management\n");
		return 1;
	}
	struct zwlr_output_power_v1 *p = zwlr_output_power_manager_v1_get_output_power(mgr, output);
	zwlr_output_power_v1_add_listener(p, &power_listener, NULL);
	if (!strcmp(argv[1], "set") && argc > 2) {
		zwlr_output_power_v1_set_mode(p, !strcmp(argv[2], "on") ? ZWLR_OUTPUT_POWER_V1_MODE_ON
		                                                          : ZWLR_OUTPUT_POWER_V1_MODE_OFF);
		wl_display_roundtrip(d);
		wl_display_roundtrip(d);
		return failed;
	}
	double secs = atof(argv[1]);
	struct timespec t0, now;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		double left = secs - (now.tv_sec - t0.tv_sec) - (now.tv_nsec - t0.tv_nsec) / 1e9;
		if (left <= 0 || failed) break;
		wl_display_flush(d);
		struct pollfd pfd = {wl_display_get_fd(d), POLLIN, 0};
		if (poll(&pfd, 1, (int)(left * 1000)) > 0 && wl_display_dispatch(d) < 0) break;
	}
	return failed;
}
