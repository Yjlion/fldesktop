/* fl-wm: where a panel popup goes (no wlroots; unit tested) */
#ifndef FLWM_POPUP_PLACE_H
#define FLWM_POPUP_PLACE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The panel names its popups "fldesktop.panel-popup", optionally followed by
 * "@<x1>-<x2>": the x range of the button that opened it, in the panel
 * window. True when `title` is such a name; *x1 and *x2 are the range, or -1
 * when there is none. */
bool flwm_popup_title(const char *title, int *x1, int *x2);

/* The x of a popup `width` wide centred on `center`, kept 4 px inside the
 * area from area_x, area_width wide. */
int flwm_popup_x(int center, int width, int area_x, int area_width);

#ifdef __cplusplus
}
#endif

#endif
