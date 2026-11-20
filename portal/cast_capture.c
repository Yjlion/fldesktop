/* xdg-desktop-portal-fl: screen capture into PipeWire (see cast_capture.h)
 *
 * Each stream has its own Wayland connection and a thread that asks the
 * compositor for one frame at a time; the compositor answers when something
 * changed, so an idle screen costs nothing. Frames are copied into a staging
 * buffer and the PipeWire stream, which drives itself, is triggered to hand
 * them out. */
#define _GNU_SOURCE
#include "cast_capture.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/param.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

#include "wlr-screencopy-unstable-v1-client-protocol.h"
#ifdef FLCAST_HAVE_EXT
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#endif

#define MAX_OUTPUTS 16
#define MAX_TOPLEVELS 128

/* ---- Wayland connection and globals ------------------------------------ */

struct out {
	struct wl_output *wl;
	uint32_t gid;
	char name[64], desc[128];
	int w, h;
};

struct top {
	struct conn *c;
#ifdef FLCAST_HAVE_EXT
	struct ext_foreign_toplevel_handle_v1 *h;
#endif
	char id[64], title[256], app_id[128];
	int closed;
};

struct conn {
	struct wl_display *d;
	struct wl_registry *reg;
	struct wl_shm *shm;
	struct zwlr_screencopy_manager_v1 *scm;
	uint32_t scm_version;
#ifdef FLCAST_HAVE_EXT
	struct ext_image_copy_capture_manager_v1 *icm;
	struct ext_output_image_capture_source_manager_v1 *osm;
	struct ext_foreign_toplevel_image_capture_source_manager_v1 *tsm;
	struct ext_foreign_toplevel_list_v1 *tl;
#endif
	int want_toplevels;
	struct out outs[MAX_OUTPUTS];
	int n_outs;
	struct top *tops[MAX_TOPLEVELS];
	int n_tops;
};

static void copy_str(char *dst, size_t n, const char *src) {
	snprintf(dst, n, "%s", src ? src : "");
}

static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
		int32_t sub, const char *make, const char *model, int32_t tr) {
	(void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub; (void)tr;
	struct out *out = d;
	if (!out->desc[0]) snprintf(out->desc, sizeof out->desc, "%s %s", make, model);
}
static void out_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t r) {
	(void)o; (void)r;
	struct out *out = d;
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		out->w = w;
		out->h = h;
	}
}
static void out_done(void *d, struct wl_output *o) { (void)d; (void)o; }
static void out_scale(void *d, struct wl_output *o, int32_t f) { (void)d; (void)o; (void)f; }
static void out_name(void *d, struct wl_output *o, const char *name) {
	(void)o;
	struct out *out = d;
	copy_str(out->name, sizeof out->name, name);
}
static void out_description(void *d, struct wl_output *o, const char *desc) {
	(void)o;
	struct out *out = d;
	copy_str(out->desc, sizeof out->desc, desc);
}
static const struct wl_output_listener out_listener = {
	.geometry = out_geometry, .mode = out_mode, .done = out_done, .scale = out_scale,
	.name = out_name, .description = out_description,
};

#ifdef FLCAST_HAVE_EXT
static void top_closed(void *d, struct ext_foreign_toplevel_handle_v1 *h) {
	(void)h;
	((struct top *)d)->closed = 1;
}
static void top_done(void *d, struct ext_foreign_toplevel_handle_v1 *h) { (void)d; (void)h; }
static void top_title(void *d, struct ext_foreign_toplevel_handle_v1 *h, const char *s) {
	(void)h;
	struct top *t = d;
	copy_str(t->title, sizeof t->title, s);
}
static void top_app_id(void *d, struct ext_foreign_toplevel_handle_v1 *h, const char *s) {
	(void)h;
	struct top *t = d;
	copy_str(t->app_id, sizeof t->app_id, s);
}
static void top_identifier(void *d, struct ext_foreign_toplevel_handle_v1 *h, const char *s) {
	(void)h;
	struct top *t = d;
	copy_str(t->id, sizeof t->id, s);
}
static const struct ext_foreign_toplevel_handle_v1_listener top_listener = {
	.closed = top_closed, .done = top_done, .title = top_title, .app_id = top_app_id,
	.identifier = top_identifier,
};

static void list_toplevel(void *d, struct ext_foreign_toplevel_list_v1 *l,
		struct ext_foreign_toplevel_handle_v1 *h) {
	(void)l;
	struct conn *c = d;
	if (c->n_tops >= MAX_TOPLEVELS) {
		ext_foreign_toplevel_handle_v1_destroy(h);
		return;
	}
	struct top *t = calloc(1, sizeof *t);
	t->c = c;
	t->h = h;
	ext_foreign_toplevel_handle_v1_add_listener(h, &top_listener, t);
	c->tops[c->n_tops++] = t;
}
static void list_finished(void *d, struct ext_foreign_toplevel_list_v1 *l) { (void)d; (void)l; }
static const struct ext_foreign_toplevel_list_v1_listener list_listener = {
	.toplevel = list_toplevel, .finished = list_finished,
};
#endif

static void reg_global(void *d, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver) {
	struct conn *c = d;
	if (!strcmp(iface, wl_shm_interface.name)) {
		c->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, wl_output_interface.name) && c->n_outs < MAX_OUTPUTS) {
		struct out *o = &c->outs[c->n_outs++];
		memset(o, 0, sizeof *o);
		o->gid = name;
		o->wl = wl_registry_bind(reg, name, &wl_output_interface, ver < 4 ? ver : 4);
		wl_output_add_listener(o->wl, &out_listener, o);
	} else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name)) {
		c->scm_version = ver < 3 ? ver : 3;
		c->scm = wl_registry_bind(reg, name, &zwlr_screencopy_manager_v1_interface, c->scm_version);
#ifdef FLCAST_HAVE_EXT
	} else if (!strcmp(iface, ext_image_copy_capture_manager_v1_interface.name)) {
		c->icm = wl_registry_bind(reg, name, &ext_image_copy_capture_manager_v1_interface, 1);
	} else if (!strcmp(iface, ext_output_image_capture_source_manager_v1_interface.name)) {
		c->osm = wl_registry_bind(reg, name, &ext_output_image_capture_source_manager_v1_interface, 1);
	} else if (!strcmp(iface, ext_foreign_toplevel_image_capture_source_manager_v1_interface.name)) {
		c->tsm = wl_registry_bind(reg, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
	} else if (!strcmp(iface, ext_foreign_toplevel_list_v1_interface.name) && c->want_toplevels) {
		c->tl = wl_registry_bind(reg, name, &ext_foreign_toplevel_list_v1_interface, 1);
		ext_foreign_toplevel_list_v1_add_listener(c->tl, &list_listener, c);
#endif
	}
}
static void reg_remove(void *d, struct wl_registry *reg, uint32_t name) { (void)d; (void)reg; (void)name; }
static const struct wl_registry_listener reg_listener = { .global = reg_global, .global_remove = reg_remove };

static int conn_open(struct conn *c, int want_toplevels) {
	memset(c, 0, sizeof *c);
	c->want_toplevels = want_toplevels;
	c->d = wl_display_connect(NULL);
	if (!c->d) return -1;
	c->reg = wl_display_get_registry(c->d);
	wl_registry_add_listener(c->reg, &reg_listener, c);
	/* globals; then output names and toplevels; then the toplevels' details */
	for (int i = 0; i < 3; i++)
		if (wl_display_roundtrip(c->d) < 0) return -1;
	return 0;
}

static void conn_close(struct conn *c) {
	if (!c->d) return;
	for (int i = 0; i < c->n_tops; i++) {
#ifdef FLCAST_HAVE_EXT
		ext_foreign_toplevel_handle_v1_destroy(c->tops[i]->h);
#endif
		free(c->tops[i]);
	}
#ifdef FLCAST_HAVE_EXT
	if (c->tl) ext_foreign_toplevel_list_v1_destroy(c->tl);
	if (c->tsm) ext_foreign_toplevel_image_capture_source_manager_v1_destroy(c->tsm);
	if (c->osm) ext_output_image_capture_source_manager_v1_destroy(c->osm);
	if (c->icm) ext_image_copy_capture_manager_v1_destroy(c->icm);
#endif
	if (c->scm) zwlr_screencopy_manager_v1_destroy(c->scm);
	for (int i = 0; i < c->n_outs; i++) {
		if (wl_output_get_version(c->outs[i].wl) >= 3) wl_output_release(c->outs[i].wl);
		else wl_output_destroy(c->outs[i].wl);
	}
	if (c->shm) wl_shm_destroy(c->shm);
	wl_registry_destroy(c->reg);
	wl_display_disconnect(c->d);
	c->d = NULL;
}

static unsigned conn_types(const struct conn *c) {
	unsigned t = 0;
	if (!c->shm) return 0;
	if (c->scm) t |= FLCAST_MONITOR;
#ifdef FLCAST_HAVE_EXT
	if (c->icm && c->osm) t |= FLCAST_MONITOR;
	if (c->icm && c->tsm && c->tl) t |= FLCAST_WINDOW;
#endif
	return t;
}

int flcast_list_sources(struct flcast_sources *s) {
	memset(s, 0, sizeof *s);
	struct conn c;
	if (conn_open(&c, 1) < 0) {
		if (c.d) conn_close(&c);
		return -1;
	}
	s->types = conn_types(&c);
	s->outputs = calloc(c.n_outs ? c.n_outs : 1, sizeof *s->outputs);
	for (int i = 0; i < c.n_outs; i++) {
		if (!c.outs[i].name[0]) continue;
		struct flcast_output *o = &s->outputs[s->n_outputs++];
		copy_str(o->name, sizeof o->name, c.outs[i].name);
		copy_str(o->desc, sizeof o->desc, c.outs[i].desc);
		o->width = c.outs[i].w;
		o->height = c.outs[i].h;
	}
	s->windows = calloc(c.n_tops ? c.n_tops : 1, sizeof *s->windows);
	if (s->types & FLCAST_WINDOW) {
		for (int i = 0; i < c.n_tops; i++) {
			struct top *t = c.tops[i];
			if (t->closed || !t->id[0]) continue;
			struct flcast_window *w = &s->windows[s->n_windows++];
			copy_str(w->id, sizeof w->id, t->id);
			copy_str(w->title, sizeof w->title, t->title);
			copy_str(w->app_id, sizeof w->app_id, t->app_id);
		}
	}
	conn_close(&c);
	return 0;
}

void flcast_sources_free(struct flcast_sources *s) {
	free(s->outputs);
	free(s->windows);
	memset(s, 0, sizeof *s);
}

/* ---- the stream --------------------------------------------------------- */

struct flcast_stream {
	struct conn c;
	enum flcast_kind kind;
	int cursor;
	int use_ext;
	struct wl_output *output; /* wlr-screencopy */

#ifdef FLCAST_HAVE_EXT
	struct ext_image_capture_source_v1 *src;
	struct ext_image_copy_capture_session_v1 *sess;
	struct ext_image_copy_capture_frame_v1 *frame;
#endif
	struct zwlr_screencopy_frame_v1 *scframe;

	/* buffer constraints from the compositor */
	int cw, ch, cstride;
	uint32_t cfmt;
	int crank; /* preference of cfmt, lower is better; 0 = none yet */
	int constraints_done;
	int y_invert;

	/* frame state */
	int frame_ready, frame_failed, source_gone;

	/* our shm buffer */
	struct wl_buffer *buf;
	void *map;
	size_t map_size;
	int bw, bh, bstride;
	uint32_t bfmt;

	/* PipeWire (fields below are guarded by the thread loop's lock) */
	struct pw_thread_loop *loop;
	struct pw_context *ctx;
	struct pw_core *core;
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	uint32_t node;
	int pw_error, streaming;
	uint8_t *staging;
	int sw, sh, sstride;
	uint32_t spa_fmt;
	int have_frame;
	uint64_t seq;

	pthread_t th;
	int thread_started;
	int stop_pipe[2];
	flcast_closed_fn closed;
	void *data;
};

/* wl_shm format -> rank (1 best) and the matching SPA format */
static int fmt_rank(uint32_t f, uint32_t *spa) {
	switch (f) {
	case WL_SHM_FORMAT_XRGB8888: if (spa) *spa = SPA_VIDEO_FORMAT_BGRx; return 1;
	case WL_SHM_FORMAT_ARGB8888: if (spa) *spa = SPA_VIDEO_FORMAT_BGRA; return 2;
	case WL_SHM_FORMAT_XBGR8888: if (spa) *spa = SPA_VIDEO_FORMAT_RGBx; return 3;
	case WL_SHM_FORMAT_ABGR8888: if (spa) *spa = SPA_VIDEO_FORMAT_RGBA; return 4;
	default: return 0;
	}
}

static void offer_format(struct flcast_stream *s, uint32_t fmt) {
	int r = fmt_rank(fmt, NULL);
	if (r && (!s->crank || r < s->crank)) {
		s->crank = r;
		s->cfmt = fmt;
	}
}

#ifdef FLCAST_HAVE_EXT
static void sess_buffer_size(void *d, struct ext_image_copy_capture_session_v1 *x, uint32_t w, uint32_t h) {
	(void)x;
	struct flcast_stream *s = d;
	s->cw = (int)w;
	s->ch = (int)h;
	s->cstride = (int)w * 4;
}
static void sess_shm_format(void *d, struct ext_image_copy_capture_session_v1 *x, uint32_t f) {
	(void)x;
	offer_format(d, f);
}
static void sess_dmabuf_device(void *d, struct ext_image_copy_capture_session_v1 *x, struct wl_array *a) {
	(void)d; (void)x; (void)a;
}
static void sess_dmabuf_format(void *d, struct ext_image_copy_capture_session_v1 *x, uint32_t f,
		struct wl_array *m) {
	(void)d; (void)x; (void)f; (void)m;
}
static void sess_done(void *d, struct ext_image_copy_capture_session_v1 *x) {
	(void)x;
	((struct flcast_stream *)d)->constraints_done = 1;
}
static void sess_stopped(void *d, struct ext_image_copy_capture_session_v1 *x) {
	(void)x;
	((struct flcast_stream *)d)->source_gone = 1;
}
static const struct ext_image_copy_capture_session_v1_listener sess_listener = {
	.buffer_size = sess_buffer_size, .shm_format = sess_shm_format,
	.dmabuf_device = sess_dmabuf_device, .dmabuf_format = sess_dmabuf_format,
	.done = sess_done, .stopped = sess_stopped,
};

static void fr_transform(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t t) {
	(void)d; (void)f; (void)t;
}
static void fr_damage(void *d, struct ext_image_copy_capture_frame_v1 *f, int32_t x, int32_t y, int32_t w,
		int32_t h) {
	(void)d; (void)f; (void)x; (void)y; (void)w; (void)h;
}
static void fr_time(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t hi, uint32_t lo, uint32_t ns) {
	(void)d; (void)f; (void)hi; (void)lo; (void)ns;
}
static void fr_ready(void *d, struct ext_image_copy_capture_frame_v1 *f) {
	(void)f;
	((struct flcast_stream *)d)->frame_ready = 1;
}
static void fr_failed(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t reason) {
	(void)f;
	struct flcast_stream *s = d;
	s->frame_failed = 1;
	if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED) s->source_gone = 1;
}
static const struct ext_image_copy_capture_frame_v1_listener fr_listener = {
	.transform = fr_transform, .damage = fr_damage, .presentation_time = fr_time,
	.ready = fr_ready, .failed = fr_failed,
};
#endif

static void sc_buffer(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fmt, uint32_t w, uint32_t h,
		uint32_t stride) {
	(void)f;
	struct flcast_stream *s = d;
	int r = fmt_rank(fmt, NULL);
	if (r && (!s->crank || r < s->crank)) {
		s->crank = r;
		s->cfmt = fmt;
		s->cw = (int)w;
		s->ch = (int)h;
		s->cstride = (int)stride;
	}
	if (s->c.scm_version < 3) s->constraints_done = 1;
}
static void sc_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t flags) {
	(void)f;
	((struct flcast_stream *)d)->y_invert = (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0;
}
static void sc_ready(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t hi, uint32_t lo, uint32_t ns) {
	(void)f; (void)hi; (void)lo; (void)ns;
	((struct flcast_stream *)d)->frame_ready = 1;
}
static void sc_failed(void *d, struct zwlr_screencopy_frame_v1 *f) {
	(void)f;
	((struct flcast_stream *)d)->frame_failed = 1;
}
static void sc_damage(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t x, uint32_t y, uint32_t w,
		uint32_t h) {
	(void)d; (void)f; (void)x; (void)y; (void)w; (void)h;
}
static void sc_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fmt, uint32_t w, uint32_t h) {
	(void)d; (void)f; (void)fmt; (void)w; (void)h;
}
static void sc_buffer_done(void *d, struct zwlr_screencopy_frame_v1 *f) {
	(void)f;
	((struct flcast_stream *)d)->constraints_done = 1;
}
static const struct zwlr_screencopy_frame_v1_listener sc_listener = {
	.buffer = sc_buffer, .flags = sc_flags, .ready = sc_ready, .failed = sc_failed,
	.damage = sc_damage, .linux_dmabuf = sc_dmabuf, .buffer_done = sc_buffer_done,
};

static void free_buffer(struct flcast_stream *s) {
	if (s->buf) wl_buffer_destroy(s->buf);
	if (s->map) munmap(s->map, s->map_size);
	s->buf = NULL;
	s->map = NULL;
	s->bw = s->bh = 0;
}

static int alloc_buffer(struct flcast_stream *s) {
	free_buffer(s);
	if (s->cw <= 0 || s->ch <= 0 || !s->crank) return -1;
	int stride = s->cstride > 0 ? s->cstride : s->cw * 4;
	size_t size = (size_t)stride * (size_t)s->ch;
	int fd = memfd_create("fl-cast", MFD_CLOEXEC);
	if (fd < 0) return -1;
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return -1;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(s->c.shm, fd, (int32_t)size);
	s->buf = wl_shm_pool_create_buffer(pool, 0, s->cw, s->ch, stride, s->cfmt);
	wl_shm_pool_destroy(pool);
	close(fd);
	s->map = map;
	s->map_size = size;
	s->bw = s->cw;
	s->bh = s->ch;
	s->bstride = stride;
	s->bfmt = s->cfmt;
	return 0;
}

static int buffer_matches(const struct flcast_stream *s) {
	return s->buf && s->bw == s->cw && s->bh == s->ch && s->bfmt == s->cfmt &&
		s->bstride == (s->cstride > 0 ? s->cstride : s->cw * 4);
}

/* ---- PipeWire ----------------------------------------------------------- */

static const struct spa_pod *build_format(struct spa_pod_builder *b, const struct flcast_stream *s) {
	return spa_pod_builder_add_object(b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_Id(s->spa_fmt),
		SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE((uint32_t)s->sw, (uint32_t)s->sh)),
		SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(0, 1)),
		SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(
			&SPA_FRACTION(60, 1), &SPA_FRACTION(1, 1), &SPA_FRACTION(60, 1)));
}

static void on_state(void *d, enum pw_stream_state old, enum pw_stream_state state, const char *error) {
	(void)old;
	struct flcast_stream *s = d;
	if (state == PW_STREAM_STATE_ERROR) {
		fprintf(stderr, "xdg-desktop-portal-fl: PipeWire stream error: %s\n", error ? error : "?");
		s->pw_error = 1;
	}
	if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING)
		s->node = pw_stream_get_node_id(s->stream);
	s->streaming = state == PW_STREAM_STATE_STREAMING;
	if (s->streaming && s->have_frame) pw_stream_trigger_process(s->stream);
	pw_thread_loop_signal(s->loop, false);
}

static void on_param_changed(void *d, uint32_t id, const struct spa_pod *param) {
	struct flcast_stream *s = d;
	if (id != SPA_PARAM_Format || !param) return;
	struct spa_video_info_raw info;
	if (spa_format_video_raw_parse(param, &info) < 0) return;
	uint8_t tmp[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(tmp, sizeof tmp);
	const struct spa_pod *params[2];
	params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size, SPA_POD_Int(s->sstride * s->sh),
		SPA_PARAM_BUFFERS_stride, SPA_POD_Int(s->sstride),
		SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
	params[1] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
	pw_stream_update_params(s->stream, params, 2);
}

static void on_process(void *d) {
	struct flcast_stream *s = d;
	struct pw_buffer *pb = pw_stream_dequeue_buffer(s->stream);
	if (!pb) return;
	struct spa_buffer *buf = pb->buffer;
	struct spa_data *sd = &buf->datas[0];
	if (!sd->data || !s->staging) {
		pw_stream_queue_buffer(s->stream, pb);
		return;
	}
	size_t need = (size_t)s->sstride * (size_t)s->sh;
	size_t n = need < sd->maxsize ? need : sd->maxsize;
	memcpy(sd->data, s->staging, n);
	sd->chunk->offset = 0;
	sd->chunk->size = (uint32_t)n;
	sd->chunk->stride = s->sstride;
	sd->chunk->flags = s->have_frame ? 0 : SPA_CHUNK_FLAG_CORRUPTED;
	struct spa_meta_header *h = spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof *h);
	if (h) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		h->pts = (int64_t)ts.tv_sec * SPA_NSEC_PER_SEC + ts.tv_nsec;
		h->flags = 0;
		h->seq = s->seq++;
		h->dts_offset = 0;
	}
	pw_stream_queue_buffer(s->stream, pb);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state,
	.param_changed = on_param_changed,
	.process = on_process,
};

/* (Re)sizes the staging buffer after the shm buffer and, when the stream
 * exists, offers PipeWire the new format. */
static int pw_set_format(struct flcast_stream *s) {
	uint32_t spa = 0;
	fmt_rank(s->bfmt, &spa);
	pw_thread_loop_lock(s->loop);
	int changed = s->sw != s->bw || s->sh != s->bh || s->sstride != s->bstride || s->spa_fmt != spa;
	if (changed) {
		uint8_t *st = calloc(1, (size_t)s->bstride * (size_t)s->bh);
		if (!st) {
			pw_thread_loop_unlock(s->loop);
			return -1;
		}
		free(s->staging);
		s->staging = st;
		s->sw = s->bw;
		s->sh = s->bh;
		s->sstride = s->bstride;
		s->spa_fmt = spa;
		s->have_frame = 0;
		if (s->stream) {
			uint8_t tmp[1024];
			struct spa_pod_builder b = SPA_POD_BUILDER_INIT(tmp, sizeof tmp);
			const struct spa_pod *p = build_format(&b, s);
			pw_stream_update_params(s->stream, &p, 1);
		}
	}
	pw_thread_loop_unlock(s->loop);
	return 0;
}

static void deliver(struct flcast_stream *s) {
	pw_thread_loop_lock(s->loop);
	if (s->staging && s->sw == s->bw && s->sh == s->bh && s->sstride == s->bstride) {
		if (s->y_invert) {
			for (int y = 0; y < s->bh; y++)
				memcpy(s->staging + (size_t)y * s->sstride,
					(uint8_t *)s->map + (size_t)(s->bh - 1 - y) * s->bstride, (size_t)s->bstride);
		} else {
			memcpy(s->staging, s->map, (size_t)s->bstride * (size_t)s->bh);
		}
		s->have_frame = 1;
		if (s->streaming) pw_stream_trigger_process(s->stream);
	}
	pw_thread_loop_unlock(s->loop);
}

static void pw_init_once(void) { pw_init(NULL, NULL); }

static int pw_start(struct flcast_stream *s, char *err, size_t errlen) {
	static pthread_once_t once = PTHREAD_ONCE_INIT;
	pthread_once(&once, pw_init_once);
	s->node = SPA_ID_INVALID;
	s->loop = pw_thread_loop_new("fl-cast", NULL);
	if (!s->loop) goto fail;
	s->ctx = pw_context_new(pw_thread_loop_get_loop(s->loop), NULL, 0);
	if (!s->ctx || pw_thread_loop_start(s->loop) < 0) goto fail;
	if (pw_set_format(s) < 0) goto fail;
	pw_thread_loop_lock(s->loop);
	s->core = pw_context_connect(s->ctx, NULL, 0);
	if (!s->core) {
		pw_thread_loop_unlock(s->loop);
		snprintf(err, errlen, "cannot connect to PipeWire");
		return -1;
	}
	s->stream = pw_stream_new(s->core, "fl-screencast",
		pw_properties_new(PW_KEY_MEDIA_CLASS, "Video/Source", PW_KEY_MEDIA_TYPE, "Video",
			PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", NULL));
	pw_stream_add_listener(s->stream, &s->stream_listener, &stream_events, s);
	uint8_t tmp[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(tmp, sizeof tmp);
	const struct spa_pod *p = build_format(&b, s);
	pw_stream_connect(s->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
		PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS, &p, 1);
	while (s->node == SPA_ID_INVALID && !s->pw_error)
		if (pw_thread_loop_timed_wait(s->loop, 5) != 0) break;
	int ok = s->node != SPA_ID_INVALID;
	pw_thread_loop_unlock(s->loop);
	if (!ok) {
		snprintf(err, errlen, "the PipeWire stream did not start");
		return -1;
	}
	return 0;
fail:
	snprintf(err, errlen, "cannot set up PipeWire");
	return -1;
}

/* ---- capture thread ----------------------------------------------------- */

/* Reads and dispatches Wayland events once, waiting up to timeout_ms.
 * -1 when asked to stop or the connection broke. */
static int pump(struct flcast_stream *s, int timeout_ms) {
	struct wl_display *d = s->c.d;
	while (wl_display_prepare_read(d) != 0)
		if (wl_display_dispatch_pending(d) < 0) return -1;
	if (wl_display_flush(d) < 0 && errno != EAGAIN) {
		wl_display_cancel_read(d);
		return -1;
	}
	struct pollfd p[2] = {
		{ .fd = wl_display_get_fd(d), .events = POLLIN },
		{ .fd = s->stop_pipe[0], .events = POLLIN },
	};
	int r = poll(p, 2, timeout_ms);
	if ((r < 0 && errno != EINTR) || p[1].revents) {
		wl_display_cancel_read(d);
		return -1;
	}
	if (r > 0 && (p[0].revents & (POLLIN | POLLHUP | POLLERR))) {
		if (wl_display_read_events(d) < 0) return -1;
	} else {
		wl_display_cancel_read(d);
	}
	return wl_display_dispatch_pending(d) < 0 ? -1 : 0;
}

static void request_frame(struct flcast_stream *s) {
	s->frame_ready = s->frame_failed = 0;
#ifdef FLCAST_HAVE_EXT
	if (s->use_ext) {
		s->frame = ext_image_copy_capture_session_v1_create_frame(s->sess);
		ext_image_copy_capture_frame_v1_add_listener(s->frame, &fr_listener, s);
		ext_image_copy_capture_frame_v1_attach_buffer(s->frame, s->buf);
		ext_image_copy_capture_frame_v1_damage_buffer(s->frame, 0, 0, s->bw, s->bh);
		ext_image_copy_capture_frame_v1_capture(s->frame);
		return;
	}
#endif
	zwlr_screencopy_frame_v1_copy_with_damage(s->scframe, s->buf);
}

static void drop_frame(struct flcast_stream *s) {
#ifdef FLCAST_HAVE_EXT
	if (s->frame) ext_image_copy_capture_frame_v1_destroy(s->frame);
	s->frame = NULL;
#endif
	if (s->scframe) zwlr_screencopy_frame_v1_destroy(s->scframe);
	s->scframe = NULL;
}

/* wlr-screencopy: a new frame object per capture; waits for its buffer
 * constraints */
static int sc_begin(struct flcast_stream *s) {
	s->constraints_done = 0;
	s->crank = 0;
	s->y_invert = 0;
	s->scframe = zwlr_screencopy_manager_v1_capture_output(s->c.scm, s->cursor, s->output);
	zwlr_screencopy_frame_v1_add_listener(s->scframe, &sc_listener, s);
	while (!s->constraints_done && !s->frame_failed)
		if (pump(s, -1) < 0) return -1;
	return s->crank ? 0 : -1;
}

static void *capture_thread(void *data) {
	struct flcast_stream *s = data;
	int failures = 0;
	int first = 1;
	for (;;) {
		if (!s->use_ext && !first) {
			if (sc_begin(s) < 0) {
				if (s->frame_failed && ++failures < 20) {
					drop_frame(s);
					s->frame_failed = 0;
					if (pump(s, 100) < 0) break;
					continue;
				}
				s->source_gone = s->frame_failed;
				break;
			}
		}
		first = 0;
		if (!buffer_matches(s)) {
			if (alloc_buffer(s) < 0 || pw_set_format(s) < 0) {
				s->source_gone = 1;
				break;
			}
		}
		request_frame(s);
		while (!s->frame_ready && !s->frame_failed && !s->source_gone)
			if (pump(s, -1) < 0) goto out;
		if (s->frame_ready) {
			deliver(s);
			failures = 0;
		} else if (!s->source_gone && ++failures > 20) {
			s->source_gone = 1;
		} else if (!s->source_gone && s->use_ext) {
			/* new buffer constraints are on their way */
			if (pump(s, 50) < 0) goto out;
		}
		drop_frame(s);
		if (s->source_gone) break;
		/* ext: the compositor resends constraints (buffer_constraints
		 * failure); they are applied by buffer_matches() above */
	}
out:
	drop_frame(s);
	if (s->source_gone && s->closed) s->closed(s->data);
	return NULL;
}

/* ---- public ------------------------------------------------------------- */

static struct wl_output *find_output(struct conn *c, const char *name) {
	for (int i = 0; i < c->n_outs; i++)
		if (!strcmp(c->outs[i].name, name)) return c->outs[i].wl;
	return NULL;
}

#ifdef FLCAST_HAVE_EXT
static struct ext_foreign_toplevel_handle_v1 *find_toplevel(struct conn *c, const char *id) {
	for (int i = 0; i < c->n_tops; i++)
		if (!c->tops[i]->closed && !strcmp(c->tops[i]->id, id)) return c->tops[i]->h;
	return NULL;
}
#endif

static void free_stream(struct flcast_stream *s) {
	if (s->loop) pw_thread_loop_stop(s->loop);
	if (s->stream) pw_stream_destroy(s->stream);
	if (s->core) pw_core_disconnect(s->core);
	if (s->ctx) pw_context_destroy(s->ctx);
	if (s->loop) pw_thread_loop_destroy(s->loop);
	free(s->staging);
	drop_frame(s);
#ifdef FLCAST_HAVE_EXT
	if (s->sess) ext_image_copy_capture_session_v1_destroy(s->sess);
	if (s->src) ext_image_capture_source_v1_destroy(s->src);
#endif
	free_buffer(s);
	if (s->c.d) conn_close(&s->c);
	if (s->stop_pipe[0] >= 0) close(s->stop_pipe[0]);
	if (s->stop_pipe[1] >= 0) close(s->stop_pipe[1]);
	free(s);
}

struct flcast_stream *flcast_stream_start(enum flcast_kind kind, const char *id, int cursor,
		flcast_closed_fn closed, void *data, char *err, size_t errlen) {
	struct flcast_stream *s = calloc(1, sizeof *s);
	if (!s) return NULL;
	s->stop_pipe[0] = s->stop_pipe[1] = -1;
	s->kind = kind;
	s->cursor = cursor ? 1 : 0;
	s->closed = closed;
	s->data = data;
	if (pipe2(s->stop_pipe, O_CLOEXEC) < 0 || conn_open(&s->c, kind == FLCAST_WINDOW) < 0 || !s->c.shm) {
		snprintf(err, errlen, "no Wayland connection");
		goto fail;
	}
#ifdef FLCAST_HAVE_EXT
	uint32_t opts = s->cursor ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0;
	if (kind == FLCAST_WINDOW) {
		struct ext_foreign_toplevel_handle_v1 *h = find_toplevel(&s->c, id);
		if (!s->c.icm || !s->c.tsm || !h) {
			snprintf(err, errlen, "the window is gone");
			goto fail;
		}
		s->src = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(s->c.tsm, h);
	} else if (s->c.icm && s->c.osm) {
		struct wl_output *o = find_output(&s->c, id);
		if (!o) {
			snprintf(err, errlen, "no screen %s", id);
			goto fail;
		}
		s->src = ext_output_image_capture_source_manager_v1_create_source(s->c.osm, o);
	}
	if (s->src) {
		s->use_ext = 1;
		s->sess = ext_image_copy_capture_manager_v1_create_session(s->c.icm, s->src, opts);
		ext_image_copy_capture_session_v1_add_listener(s->sess, &sess_listener, s);
		for (int i = 0; i < 3 && !s->constraints_done && !s->source_gone; i++)
			wl_display_roundtrip(s->c.d);
		if (!s->constraints_done || !s->crank || s->source_gone) {
			snprintf(err, errlen, "the compositor offers no usable buffer format");
			goto fail;
		}
	}
#else
	if (kind == FLCAST_WINDOW) {
		snprintf(err, errlen, "window capture is not supported");
		goto fail;
	}
#endif
	if (!s->use_ext) {
		s->output = find_output(&s->c, id);
		if (!s->c.scm || !s->output) {
			snprintf(err, errlen, "cannot capture screen %s", id);
			goto fail;
		}
		s->scframe = zwlr_screencopy_manager_v1_capture_output(s->c.scm, s->cursor, s->output);
		zwlr_screencopy_frame_v1_add_listener(s->scframe, &sc_listener, s);
		for (int i = 0; i < 3 && !s->constraints_done && !s->frame_failed; i++)
			wl_display_roundtrip(s->c.d);
		if (!s->crank) {
			snprintf(err, errlen, "the compositor offers no usable buffer format");
			goto fail;
		}
	}
	if (alloc_buffer(s) < 0) {
		snprintf(err, errlen, "out of memory");
		goto fail;
	}
	if (pw_start(s, err, errlen) < 0) goto fail;
	if (pthread_create(&s->th, NULL, capture_thread, s) != 0) {
		snprintf(err, errlen, "cannot start a thread");
		goto fail;
	}
	s->thread_started = 1;
	return s;
fail:
	free_stream(s);
	return NULL;
}

uint32_t flcast_stream_node(const struct flcast_stream *s) { return s->node; }

void flcast_stream_size(const struct flcast_stream *s, int *w, int *h) {
	*w = s->bw;
	*h = s->bh;
}

void flcast_stream_stop(struct flcast_stream *s) {
	if (!s) return;
	if (s->thread_started) {
		char c = 1;
		ssize_t r = write(s->stop_pipe[1], &c, 1);
		(void)r;
		pthread_join(s->th, NULL);
	}
	free_stream(s);
}
