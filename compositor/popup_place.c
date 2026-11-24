/* fl-wm: where a panel popup goes */
#include "popup_place.h"

#include <stdlib.h>
#include <string.h>

static const char kPopupTitle[] = "fldesktop.panel-popup";

bool flwm_popup_title(const char *title, int *x1, int *x2) {
	*x1 = *x2 = -1;
	if (!title) return false;
	size_t n = strlen(kPopupTitle);
	if (strncmp(title, kPopupTitle, n) != 0) return false;
	if (title[n] == 0) return true;
	if (title[n] != '@') return false;
	char *end;
	long a = strtol(title + n + 1, &end, 10);
	if (end == title + n + 1 || *end != '-') return true; /* a name without a usable range */
	char *end2;
	long b = strtol(end + 1, &end2, 10);
	if (end2 == end + 1 || *end2 != 0 || a < 0 || b < a || b > 100000) return true;
	*x1 = (int)a;
	*x2 = (int)b;
	return true;
}

int flwm_popup_x(int center, int width, int area_x, int area_width) {
	int x = center - width / 2;
	if (x + width > area_x + area_width - 4) x = area_x + area_width - 4 - width;
	if (x < area_x + 4) x = area_x + 4;
	return x;
}
