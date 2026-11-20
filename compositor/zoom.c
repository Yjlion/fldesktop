/* Screen magnifier: Super+= / Super+- zoom in and out, Super+0 back to 1x.
 *
 * Each output's frame is rendered as usual, then the part around the pointer
 * is scaled up into a scratch buffer and copied back before the commit. The
 * part is placed so that the point under the pointer stays under it: the
 * (unscaled) hardware cursor points at what it magnifies, and clicks land
 * where they appear to. */
#include <stdlib.h>
#include <wlr/render/allocator.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>

#include "server.h"

static const double steps[] = {1.0, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0};
#define NSTEPS (int)(sizeof steps / sizeof steps[0])

static void damage_all(struct flwm_server *server) {
	struct flwm_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (!o->scene_output) continue;
		wlr_damage_ring_add_whole(&o->scene_output->damage_ring);
		wlr_output_schedule_frame(o->wlr_output);
	}
}

void zoom_step(struct flwm_server *server, int dir) {
	double z = server->zoom < 1 ? 1 : server->zoom;
	double next = z;
	if (dir > 0) {
		for (int i = 0; i < NSTEPS; i++)
			if (steps[i] > z + 1e-6) {
				next = steps[i];
				break;
			}
	} else if (dir < 0) {
		for (int i = NSTEPS - 1; i >= 0; i--)
			if (steps[i] < z - 1e-6) {
				next = steps[i];
				break;
			}
	} else {
		next = 1;
	}
	if (next == server->zoom) return;
	server->zoom = next;
	wlr_log(WLR_INFO, "zoom: %.1fx", next);
	damage_all(server);
}

void zoom_cursor_moved(struct flwm_server *server) {
	if (server->zoom > 1) damage_all(server);
}

/* The source rectangle (buffer pixels) for zoom z around point p (buffer
 * pixels) in a buffer of size n: p stays where it is. */
static double src_start(double p, double z, int n) {
	double len = n / z;
	double s = p - p / z;
	if (s < 0) s = 0;
	if (s > n - len) s = n - len;
	return s;
}

static void drop_scratch(struct flwm_output *o) {
	if (o->zoom_buffer) wlr_buffer_drop(o->zoom_buffer);
	o->zoom_buffer = NULL;
}

bool zoom_output_frame(struct flwm_output *o) {
	struct flwm_server *server = o->server;
	if (server->zoom <= 1) {
		if (o->zoom_buffer) {
			/* zoom just ended: the next frame redraws everything */
			drop_scratch(o);
			wlr_damage_ring_add_whole(&o->scene_output->damage_ring);
		}
		return false;
	}
	struct wlr_output *wo = o->wlr_output;
	if (!wo->swapchain || wo->transform != WL_OUTPUT_TRANSFORM_NORMAL) return false;
	/* the whole picture changes with every pointer motion */
	wlr_damage_ring_add_whole(&o->scene_output->damage_ring);
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	if (!wlr_scene_output_build_state(o->scene_output, &state, NULL) || !state.buffer) {
		wlr_output_state_finish(&state);
		return true;
	}
	struct wlr_buffer *frame = state.buffer;
	int w = frame->width, h = frame->height;
	if (o->zoom_buffer && (o->zoom_buffer->width != w || o->zoom_buffer->height != h)) drop_scratch(o);
	if (!o->zoom_buffer)
		o->zoom_buffer = wlr_allocator_create_buffer(server->allocator, w, h, &wo->swapchain->format);
	struct wlr_texture *src = wlr_texture_from_buffer(server->renderer, frame);
	if (!o->zoom_buffer || !src) {
		if (src) wlr_texture_destroy(src);
		wlr_output_commit_state(wo, &state);
		wlr_output_state_finish(&state);
		return true;
	}
	/* the pointer in buffer pixels */
	double lx = server->cursor->x, ly = server->cursor->y;
	wlr_output_layout_output_coords(server->output_layout, wo, &lx, &ly);
	double px = lx * wo->scale, py = ly * wo->scale;
	if (px < 0) px = 0;
	if (py < 0) py = 0;
	if (px > w) px = w;
	if (py > h) py = h;
	double z = server->zoom;
	struct wlr_fbox box = {src_start(px, z, w), src_start(py, z, h), w / z, h / z};

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(server->renderer, o->zoom_buffer, NULL);
	if (pass) {
		wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
			.texture = src, .src_box = box, .dst_box = {0, 0, w, h},
			.filter_mode = WLR_SCALE_FILTER_BILINEAR, .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
		});
		wlr_render_pass_submit(pass);
		/* and back into the frame that is committed */
		struct wlr_texture *big = wlr_texture_from_buffer(server->renderer, o->zoom_buffer);
		pass = big ? wlr_renderer_begin_buffer_pass(server->renderer, frame, NULL) : NULL;
		if (pass) {
			wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
				.texture = big, .dst_box = {0, 0, w, h},
				.filter_mode = WLR_SCALE_FILTER_NEAREST, .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
			});
			wlr_render_pass_submit(pass);
		}
		if (big) wlr_texture_destroy(big);
	}
	wlr_texture_destroy(src);
	/* the whole frame is new */
	pixman_region32_t all;
	pixman_region32_init_rect(&all, 0, 0, w, h);
	wlr_output_state_set_damage(&state, &all);
	pixman_region32_fini(&all);
	wlr_output_commit_state(wo, &state);
	wlr_output_state_finish(&state);
	return true;
}

void zoom_output_gone(struct flwm_output *o) { drop_scratch(o); }
