/* fl-wm animations (anim.h). One timer for all windows, every 16 ms while
 * any window animates. The window's tree is offset while it rises and its
 * surfaces' buffers are faded; a window rule's opacity is the end value. */
#define _POSIX_C_SOURCE 200809L
#include "anim.h"

#include <time.h>
#include <wlr/types/wlr_scene.h>

#include "server.h"
#include "view_internal.h"

static int64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_buffer_alpha(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
	(void)sx, (void)sy;
	wlr_scene_buffer_set_opacity(buffer, *(float *)data);
}

static float end_opacity(struct flwm_view *view) {
	return view->opacity > 0 && view->opacity < 1 ? view->opacity : 1.0f;
}

/* draws one step; false when the animation is over */
static bool step(struct flwm_view *view, int64_t now) {
	double e = anim_ease(now - view->anim_start, FLWM_ANIM_MS);
	float a = (float)(e * end_opacity(view));
	if (view->surface_tree) wlr_scene_node_for_each_buffer(&view->surface_tree->node, set_buffer_alpha, &a);
	if (view->decor.title) wlr_scene_buffer_set_opacity(view->decor.title, (float)e);
	int dy = (int)((1.0 - e) * view->anim_rise + 0.5);
	wlr_scene_node_set_position(&view->tree->node, view->x, view->y + dy);
	return e < 1.0;
}

static int tick(void *data) {
	struct flwm_server *server = data;
	int64_t now = now_ms();
	bool more = false;
	struct flwm_view *v;
	wl_list_for_each(v, &server->views, link) {
		if (!v->anim) continue;
		if (step(v, now)) more = true;
		else v->anim = false;
	}
	if (more) wl_event_source_timer_update(server->anim_timer, 16);
	return 0;
}

static void start(struct flwm_view *view, int rise) {
	struct flwm_server *server = view->server;
	if (!server->config.animations || view->role != FLWM_ROLE_NORMAL || !view->mapped) return;
	if (!server->anim_timer) server->anim_timer = wl_event_loop_add_timer(server->event_loop, tick, server);
	if (!server->anim_timer) return;
	view->anim = true;
	view->anim_start = now_ms();
	view->anim_rise = rise;
	step(view, view->anim_start);
	wl_event_source_timer_update(server->anim_timer, 16);
}

void anim_view_open(struct flwm_view *view) { start(view, FLWM_ANIM_RISE); }
void anim_view_show(struct flwm_view *view) {
	if (!view->anim) start(view, 0);
}
bool anim_running(struct flwm_view *view) { return view->anim; }

void anim_view_stop(struct flwm_view *view) {
	if (!view->anim) return;
	view->anim = false;
	view->anim_start = 0;
	step(view, INT64_MAX / 2); /* the end state */
}

void anim_finish(struct flwm_server *server) {
	if (server->anim_timer) wl_event_source_remove(server->anim_timer);
	server->anim_timer = NULL;
}
