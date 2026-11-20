/* xdg-desktop-portal-fl: screen capture into PipeWire for the ScreenCast
 * portal. Plain C: PipeWire's SPA pod macros are C99 and do not build as
 * C++.
 *
 * Captures with ext-image-copy-capture-v1 (fl-wm on wlroots 0.19+; single
 * windows need 0.20) and falls back to wlr-screencopy for whole screens. */
#ifndef FL_CAST_CAPTURE_H
#define FL_CAST_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum flcast_kind { FLCAST_MONITOR = 1, FLCAST_WINDOW = 2 };

struct flcast_output {
	char name[64];  /* connector, e.g. "DP-1" */
	char desc[128];
	int width, height; /* current mode, pixels */
};

struct flcast_window {
	char id[64];    /* ext-foreign-toplevel identifier (this session only) */
	char title[256];
	char app_id[128];
};

struct flcast_sources {
	struct flcast_output *outputs;
	int n_outputs;
	struct flcast_window *windows;
	int n_windows;
	unsigned types; /* FLCAST_MONITOR | FLCAST_WINDOW the compositor can capture */
};

/* Lists what can be shared. Returns 0, or -1 without a Wayland connection. */
int flcast_list_sources(struct flcast_sources *out);
void flcast_sources_free(struct flcast_sources *s);

struct flcast_stream;
/* Called from the capture thread when the shared screen or window is gone. */
typedef void (*flcast_closed_fn)(void *data);

/* Starts capturing `id` (an output name or a window identifier) into a new
 * PipeWire video node. Blocks until the node exists. NULL on failure, with a
 * reason in err. */
struct flcast_stream *flcast_stream_start(enum flcast_kind kind, const char *id, int cursor,
	flcast_closed_fn closed, void *data, char *err, size_t errlen);
uint32_t flcast_stream_node(const struct flcast_stream *s);
void flcast_stream_size(const struct flcast_stream *s, int *w, int *h);
/* Stops and frees. Not from the closed callback (it joins that thread). */
void flcast_stream_stop(struct flcast_stream *s);

#ifdef __cplusplus
}
#endif
#endif
