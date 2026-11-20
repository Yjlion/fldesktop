/* fl-wm: what a finished touchpad swipe means (no wlroots; unit tested) */
#ifndef FLWM_GESTURE_DECIDE_H
#define FLWM_GESTURE_DECIDE_H

#include <stdbool.h>

enum flwm_swipe_result {
	FLWM_SWIPE_NONE,
	FLWM_SWIPE_WORKSPACE_NEXT,
	FLWM_SWIPE_WORKSPACE_PREV,
	FLWM_SWIPE_UP,
	FLWM_SWIPE_DOWN,
};

/* Distance (libinput's touchpad units, about pixels) a swipe must travel. */
#define FLWM_SWIPE_THRESHOLD 100.0

/* dx, dy: the whole movement of the swipe. With natural, the content follows
 * the fingers: swiping left brings the next workspace in from the right. */
enum flwm_swipe_result flwm_swipe_decide(double dx, double dy, bool natural, bool cancelled);

#endif
