/* Keyboard accessibility ([accessibility] in wm.ini):
 *
 * sticky_keys: a modifier (Shift, Ctrl, Alt, Super) pressed and released on
 *   its own applies to the next key, so shortcuts can be typed one key at a
 *   time; pressed twice it stays on until pressed again.
 * bounce_keys=MS: a key pressed again within MS milliseconds of being
 *   released is ignored (for hands that tremble). */
#include <string.h>
#include <time.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "server.h"

#define MAX_KEYS 768

static struct {
	uint32_t released_at[MAX_KEYS]; /* ms; for bounce keys */
	bool swallowed[MAX_KEYS];       /* a bounced press: its release goes too */
	xkb_mod_mask_t pending;         /* a modifier pressed with nothing else yet */
	xkb_mod_mask_t latched, locked; /* sticky modifiers we set */
	bool dirty;                     /* to be given to xkb after this key */
} A;

/* the modifier a key is, as an xkb mask (0: none) */
static xkb_mod_mask_t key_modifier(struct wlr_keyboard *kb, uint32_t keycode) {
	const xkb_keysym_t *syms;
	xkb_layout_index_t layout = xkb_state_key_get_layout(kb->xkb_state, keycode + 8);
	int n = xkb_keymap_key_get_syms_by_level(kb->keymap, keycode + 8, layout, 0, &syms);
	const char *name = NULL;
	for (int i = 0; i < n && !name; i++) {
		switch (syms[i]) {
		case XKB_KEY_Shift_L: case XKB_KEY_Shift_R: name = XKB_MOD_NAME_SHIFT; break;
		case XKB_KEY_Control_L: case XKB_KEY_Control_R: name = XKB_MOD_NAME_CTRL; break;
		case XKB_KEY_Alt_L: case XKB_KEY_Alt_R: case XKB_KEY_Meta_L: case XKB_KEY_Meta_R: name = XKB_MOD_NAME_ALT; break;
		case XKB_KEY_Super_L: case XKB_KEY_Super_R: name = XKB_MOD_NAME_LOGO; break;
		default: break;
		}
	}
	if (!name) return 0;
	xkb_mod_index_t idx = xkb_keymap_mod_get_index(kb->keymap, name);
	return idx == XKB_MOD_INVALID ? 0 : (xkb_mod_mask_t)1 << idx;
}

static void apply_sticky(struct wlr_keyboard *kb) {
	wlr_keyboard_notify_modifiers(kb, kb->modifiers.depressed, A.latched | A.locked, A.locked, kb->modifiers.group);
}

bool a11y_key(struct flwm_server *server, struct wlr_keyboard *kb, const struct wlr_keyboard_key_event *ev) {
	struct flwm_config *c = &server->config;
	uint32_t k = ev->keycode;
	bool pressed = ev->state == WL_KEYBOARD_KEY_STATE_PRESSED;
	if (k < MAX_KEYS && c->bounce_keys_ms > 0) {
		/* our own clock: virtual keyboards may send any time stamp */
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		uint32_t now = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
		if (now == 0) now = 1;
		if (pressed && A.released_at[k] && now - A.released_at[k] < (uint32_t)c->bounce_keys_ms) {
			A.swallowed[k] = true;
			return true;
		}
		if (!pressed) {
			A.released_at[k] = now;
			if (A.swallowed[k]) {
				A.swallowed[k] = false;
				return true;
			}
		}
	}
	if (!c->sticky_keys || !kb->keymap) return false;
	xkb_mod_mask_t mod = key_modifier(kb, k);
	if (pressed) {
		/* a modifier alone so far, or something else pressed with it */
		A.pending = mod;
		if (!mod && A.latched) {
			/* this key uses the latch; it goes once the key has gone out
			 * (xkb does not clear modifiers latched from outside) */
			A.latched = 0;
			A.dirty = true;
		}
		return false;
	}
	if (mod && A.pending == mod) {
		/* released on its own: latch, then lock, then off */
		if (A.locked & mod) A.locked &= ~mod;
		else if (A.latched & mod) {
			A.latched &= ~mod;
			A.locked |= mod;
		} else A.latched |= mod;
		A.pending = 0;
		A.dirty = true;
		return false;
	}
	A.pending = 0;
	return false;
}

/* after the key went through: puts the sticky modifiers back into xkb */
void a11y_key_done(struct flwm_server *server, struct wlr_keyboard *kb, const struct wlr_keyboard_key_event *ev) {
	(void)ev;
	if (!server->config.sticky_keys || !kb->keymap || !A.dirty) return;
	A.dirty = false;
	apply_sticky(kb);
}

void a11y_reset(void) { memset(&A, 0, sizeof A); }
