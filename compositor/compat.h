/* Small compatibility layer for wlroots 0.18 / 0.19 / 0.20 */
#ifndef FLWM_COMPAT_H
#define FLWM_COMPAT_H

#include <wlr/version.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/interfaces/wlr_buffer.h>

#define FLWM_WLR_AT_LEAST(maj, min) \
	(WLR_VERSION_MAJOR > (maj) || (WLR_VERSION_MAJOR == (maj) && WLR_VERSION_MINOR >= (min)))

/* Window geometry of an xdg surface (surface-local coordinates). */
static inline void flwm_xdg_geometry(struct wlr_xdg_surface *s, struct wlr_box *box) {
#if FLWM_WLR_AT_LEAST(0, 19)
	*box = s->geometry;
#else
	wlr_xdg_surface_get_geometry(s, box);
#endif
}

static inline void flwm_buffer_finish(struct wlr_buffer *b) {
#if FLWM_WLR_AT_LEAST(0, 19)
	wlr_buffer_finish(b);
#else
	(void)b;
#endif
}

#endif
