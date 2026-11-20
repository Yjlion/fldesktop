/* fl-xclient: a minimal X11 client for the Xwayland tests of fl-wm.
 *
 *   fl-xclient [-t SECONDS] [--menu] [--nodecor] [--maximize] [--pos X,Y] [--size WxH]
 *
 * Opens a window titled "fl-xclient" (WM_CLASS fl-xclient / FlXclient) and
 * prints what happens to it, one line each:
 *   mapped
 *   configure X Y W H     (the geometry the window manager gave it)
 *   focus-in / focus-out
 *   menu-mapped           (--menu: an override-redirect window shown and hidden)
 *   (--maximize: asks to be maximized through _NET_WM_STATE once it is mapped)
 *   delete-window         (the window manager asked it to close; it exits 0)
 * It exits after SECONDS (default 5) with status 0, or 1 when it cannot
 * connect to the X server. */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xcb/xcb.h>

static xcb_connection_t *c;

static xcb_atom_t atom(const char *name) {
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, strlen(name), name), NULL);
	xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
	free(r);
	return a;
}

static double now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
	double seconds = 5;
	int menu = 0, nodecor = 0, maximize = 0, has_pos = 0, x = 0, y = 0, w = 320, h = 200;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-t") && i + 1 < argc) seconds = atof(argv[++i]);
		else if (!strcmp(argv[i], "--menu")) menu = 1;
		else if (!strcmp(argv[i], "--nodecor")) nodecor = 1;
		else if (!strcmp(argv[i], "--maximize")) maximize = 1;
		else if (!strcmp(argv[i], "--pos") && i + 1 < argc) has_pos = sscanf(argv[++i], "%d,%d", &x, &y) == 2;
		else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &w, &h);
		else {
			fprintf(stderr, "usage: fl-xclient [-t SECONDS] [--menu] [--nodecor] [--maximize] [--pos X,Y] [--size WxH]\n");
			return 2;
		}
	}
	c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c)) {
		fprintf(stderr, "fl-xclient: cannot open the display\n");
		return 1;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;

	xcb_window_t win = xcb_generate_id(c);
	uint32_t values[] = {0x3584e4, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
		XCB_EVENT_MASK_FOCUS_CHANGE};
	xcb_create_window(c, XCB_COPY_FROM_PARENT, win, screen->root, x, y, w, h, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, 10, "fl-xclient");
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, 21,
		"fl-xclient\0FlXclient");
	xcb_atom_t wm_protocols = atom("WM_PROTOCOLS"), wm_delete = atom("WM_DELETE_WINDOW");
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, wm_protocols, XCB_ATOM_ATOM, 32, 1, &wm_delete);
	if (has_pos) {
		/* WM_NORMAL_HINTS with PPosition (bit 2) and PSize (bit 3) */
		uint32_t hints[18] = {0};
		hints[0] = (1 << 2) | (1 << 3);
		hints[1] = x;
		hints[2] = y;
		hints[3] = w;
		hints[4] = h;
		xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_NORMAL_HINTS,
			XCB_ATOM_WM_SIZE_HINTS, 32, 18, hints);
	}
	if (nodecor) {
		/* _MOTIF_WM_HINTS: flags = decorations, decorations = none */
		uint32_t motif[5] = {2, 0, 0, 0, 0};
		xcb_atom_t a = atom("_MOTIF_WM_HINTS");
		xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, a, a, 32, 5, motif);
	}
	xcb_map_window(c, win);
	xcb_flush(c);

	xcb_window_t popup = XCB_WINDOW_NONE;
	int popup_state = 0; /* 0 none, 1 shown, 2 hidden */
	double start = now(), mapped_at = 0;
	struct pollfd pfd = {xcb_get_file_descriptor(c), POLLIN, 0};
	while (now() - start < seconds) {
		if (menu && mapped_at > 0 && popup_state == 0 && now() - mapped_at > 0.5) {
			popup = xcb_generate_id(c);
			uint32_t pv[] = {0xe5a50a, 1, XCB_EVENT_MASK_STRUCTURE_NOTIFY};
			xcb_create_window(c, XCB_COPY_FROM_PARENT, popup, screen->root, 40, 40, 120, 80, 0,
				XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
				XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, pv);
			xcb_map_window(c, popup);
			xcb_flush(c);
			popup_state = 1;
		} else if (popup_state == 1 && now() - mapped_at > 1.5) {
			xcb_unmap_window(c, popup);
			xcb_destroy_window(c, popup);
			xcb_flush(c);
			popup_state = 2;
		}
		poll(&pfd, 1, 50);
		xcb_generic_event_t *ev;
		while ((ev = xcb_poll_for_event(c))) {
			switch (ev->response_type & 0x7f) {
			case XCB_MAP_NOTIFY: {
				xcb_map_notify_event_t *m = (void *)ev;
				if (m->window == win) {
					printf("mapped\n");
					mapped_at = now();
					if (maximize) {
						/* _NET_WM_STATE_ADD (1) both maximized states */
						xcb_client_message_event_t cm = {0};
						cm.response_type = XCB_CLIENT_MESSAGE;
						cm.format = 32;
						cm.window = win;
						cm.type = atom("_NET_WM_STATE");
						cm.data.data32[0] = 1;
						cm.data.data32[1] = atom("_NET_WM_STATE_MAXIMIZED_VERT");
						cm.data.data32[2] = atom("_NET_WM_STATE_MAXIMIZED_HORZ");
						cm.data.data32[3] = 1;
						xcb_send_event(c, 0, screen->root, XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
							XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT, (const char *)&cm);
						xcb_flush(c);
					}
				} else if (m->window == popup) {
					printf("menu-mapped\n");
				}
				break;
			}
			case XCB_CONFIGURE_NOTIFY: {
				xcb_configure_notify_event_t *cn = (void *)ev;
				if (cn->window == win)
					printf("configure %d %d %d %d\n", cn->x, cn->y, cn->width, cn->height);
				break;
			}
			case XCB_FOCUS_IN:
				if (((xcb_focus_in_event_t *)ev)->event == win) printf("focus-in\n");
				break;
			case XCB_FOCUS_OUT:
				if (((xcb_focus_out_event_t *)ev)->event == win) printf("focus-out\n");
				break;
			case XCB_CLIENT_MESSAGE: {
				xcb_client_message_event_t *cm = (void *)ev;
				if (cm->type == wm_protocols && cm->data.data32[0] == wm_delete) {
					printf("delete-window\n");
					free(ev);
					xcb_disconnect(c);
					return 0;
				}
				break;
			}
			}
			free(ev);
		}
		if (xcb_connection_has_error(c)) {
			printf("disconnected\n");
			return 1;
		}
	}
	xcb_disconnect(c);
	return 0;
}
