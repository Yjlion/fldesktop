#include "gesture_decide.h"

#include <math.h>

enum flwm_swipe_result flwm_swipe_decide(double dx, double dy, bool natural, bool cancelled) {
	if (cancelled) return FLWM_SWIPE_NONE;
	double ax = fabs(dx), ay = fabs(dy);
	if (ax >= ay) {
		/* mostly sideways, and clearly so */
		if (ax < FLWM_SWIPE_THRESHOLD || ax < 1.5 * ay) return FLWM_SWIPE_NONE;
		bool left = dx < 0;
		return left == natural ? FLWM_SWIPE_WORKSPACE_NEXT : FLWM_SWIPE_WORKSPACE_PREV;
	}
	if (ay < FLWM_SWIPE_THRESHOLD || ay < 1.5 * ax) return FLWM_SWIPE_NONE;
	return dy < 0 ? FLWM_SWIPE_UP : FLWM_SWIPE_DOWN;
}
