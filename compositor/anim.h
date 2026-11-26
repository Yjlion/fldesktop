/* fl-wm animations: a window that opens rises a little and fades in, and the
 * windows a workspace switch shows fade in. They take 160 ms, eased out.
 * [appearance] animations=false (Settings > Accessibility > Reduce motion)
 * turns them off. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct flwm_server;
struct flwm_view;

#define FLWM_ANIM_MS 160
#define FLWM_ANIM_RISE 16 /* px a new window rises */

/* 0..1 for elapsed/duration, eased out (cubic); pure, unit tested */
static inline double anim_ease(int64_t elapsed_ms, int duration_ms) {
	if (duration_ms <= 0 || elapsed_ms >= duration_ms) return 1.0;
	if (elapsed_ms <= 0) return 0.0;
	double u = 1.0 - (double)elapsed_ms / duration_ms;
	return 1.0 - u * u * u;
}

void anim_view_open(struct flwm_view *view); /* it was mapped */
void anim_view_show(struct flwm_view *view); /* a workspace switch shows it */
bool anim_running(struct flwm_view *view);
void anim_view_stop(struct flwm_view *view); /* unmapped or destroyed */
void anim_finish(struct flwm_server *server);
