/* fl-wm: the fldesktop Wayland compositor */
#ifndef FLWM_SERVER_H
#define FLWM_SERVER_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <xkbcommon/xkbcommon.h>

#include "compat.h"

#define FLWM_MAX_BINDINGS 96
#define FLWM_MAX_AUTOSTART 32

enum flwm_cursor_mode {
	FLWM_CURSOR_PASSTHROUGH,
	FLWM_CURSOR_MOVE,
	FLWM_CURSOR_RESIZE,
	FLWM_CURSOR_PRESSED_BUTTON, /* pressed a titlebar button, waiting for release */
};

/* Special fldesktop shell clients, recognised by app_id + executable */
enum flwm_role {
	FLWM_ROLE_NORMAL = 0,
	FLWM_ROLE_PANEL,
	FLWM_ROLE_DESKTOP,
	FLWM_ROLE_LAUNCHER,
	FLWM_ROLE_OSD, /* notifications / volume popups: overlay, no focus */
	FLWM_ROLE_OSK, /* on-screen keyboard: overlay, never takes keyboard focus */
	FLWM_ROLE_POPUP, /* panel popups (calendar, volume): at the pointer, close on focus loss */
	FLWM_ROLE_NOTIFY, /* notification popups: top right corner, overlay, never takes keyboard focus */
	FLWM_ROLE_LOCK, /* fl-lock started by us: above everything while the session is locked */
	FLWM_ROLE_SNIP, /* fl-snip's selection overlay: over all outputs and the panel, takes the focus */
};

/* What a scene node belongs to (stored in wlr_scene_node.data) */
enum flwm_node_kind {
	FLWM_NODE_VIEW,
	FLWM_NODE_DECOR,
	FLWM_NODE_LAYER,
};

enum flwm_decor_part {
	FLWM_PART_NONE = 0,
	FLWM_PART_TITLE,
	FLWM_PART_CLOSE,
	FLWM_PART_MAXIMIZE,
	FLWM_PART_MINIMIZE,
	FLWM_PART_EDGE, /* resize handle; edges stored separately */
};

/* where a view comes from */
enum flwm_view_type {
	FLWM_VIEW_XDG,
	FLWM_VIEW_XWAYLAND, /* a managed X11 window (xwayland.c) */
};

struct wlr_xwayland;
struct wlr_ext_workspace_manager_v1;
struct wlr_ext_workspace_group_handle_v1;
struct wlr_ext_workspace_handle_v1;
struct wlr_xwayland_surface;

struct flwm_node_desc {
	enum flwm_node_kind kind;
	void *ptr;
	uint32_t edges; /* for FLWM_PART_EDGE */
	enum flwm_decor_part part;
};

enum flwm_action_type {
	FLWM_ACTION_NONE,
	FLWM_ACTION_EXEC,
	FLWM_ACTION_CLOSE,
	FLWM_ACTION_MAXIMIZE,
	FLWM_ACTION_MINIMIZE,
	FLWM_ACTION_FULLSCREEN,
	FLWM_ACTION_CYCLE,
	FLWM_ACTION_CYCLE_BACK,
	FLWM_ACTION_SNAP_LEFT,
	FLWM_ACTION_SNAP_RIGHT,
	FLWM_ACTION_LAUNCHER,
	FLWM_ACTION_QUIT,
	FLWM_ACTION_RELOAD,
	FLWM_ACTION_SHOW_DESKTOP,
	FLWM_ACTION_LOCK,
	FLWM_ACTION_WORKSPACE,          /* arg: 1..N */
	FLWM_ACTION_WORKSPACE_NEXT,
	FLWM_ACTION_WORKSPACE_PREV,
	FLWM_ACTION_MOVE_TO_WORKSPACE,  /* arg: 1..N */
	FLWM_ACTION_MOVE_TO_WORKSPACE_NEXT,
	FLWM_ACTION_MOVE_TO_WORKSPACE_PREV,
	FLWM_ACTION_STICKY,
};

#define FLWM_MAX_WORKSPACES 9

struct flwm_binding {
	uint32_t modifiers; /* WLR_MODIFIER_* */
	xkb_keysym_t sym;
	enum flwm_action_type action;
	char *arg;
};

#define FLWM_MAX_OUTPUT_CONFIGS 16

struct flwm_output_config {
	char *name;
	float scale;        /* 0 = default */
	int width, height;  /* 0 = preferred */
	int refresh_mhz;    /* 0 = any */
	int x, y;
	bool has_position;
	bool disabled;
	int transform;      /* enum wl_output_transform */
};

struct flwm_config {
	/* appearance */
	int title_height;
	int border_width;
	uint32_t active_title, inactive_title, active_text, inactive_text, border_color;
	char *font; /* pango font description, e.g. "Sans Bold 10" */
	bool panel_top;
	bool focus_follows_mouse;
	/* keyboard */
	char *kb_layout, *kb_variant, *kb_options, *kb_model;
	int repeat_rate, repeat_delay;
	/* pointer */
	bool tap_to_click, natural_scroll;
	bool left_handed, disable_while_typing, middle_emulation;
	char *scroll_method; /* two-finger, edge, none (NULL = device default) */
	double pointer_accel;
	/* cursor */
	char *cursor_theme;
	int cursor_size;
	/* outputs: default scale */
	float default_scale;
	struct flwm_output_config outputs[FLWM_MAX_OUTPUT_CONFIGS];
	int noutputs;
	char *launcher_cmd;
	bool switcher; /* Alt+Tab shows the switcher (else raises each window) */
	bool xwayland; /* run X11 programs through Xwayland (read at start only) */
	int workspaces; /* number of workspaces, 1..FLWM_MAX_WORKSPACES */
	/* touchpad gestures (gesture.c) */
	bool gestures;         /* swipes with gesture_fingers are ours */
	int gesture_fingers;   /* 3..5 */
	bool gesture_natural;  /* sideways: the workspaces follow the fingers */
	struct flwm_gesture_action { enum flwm_action_type type; char *arg; } gesture_up, gesture_down;
	/* screen lock */
	int lock_idle_minutes; /* 0 = never */
	char *lock_cmd;        /* default "fl-lock --locker" */
	uint32_t lock_color;
	/* key bindings & autostart */
	struct flwm_binding bindings[FLWM_MAX_BINDINGS];
	int nbindings;
	char *autostart[FLWM_MAX_AUTOSTART];
	int nautostart;
	char *path; /* loaded file */
};

#define FLWM_SWITCHER_MAX 64

/* the Alt+Tab switcher (switcher.c) */
struct flwm_switcher {
	struct wlr_scene_buffer *node; /* NULL when closed */
	struct flwm_view *views[FLWM_SWITCHER_MAX];
	int n, sel, cols;
	struct flwm_output *output;
	struct wlr_box box; /* layout coordinates */
};

struct flwm_server {
	struct wl_display *wl_display;
	struct wl_event_loop *event_loop;
	struct wlr_backend *backend;
	struct wlr_session *session;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_compositor *compositor;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;

	/* stacking layers, bottom to top */
	struct wlr_scene_tree *layer_background; /* layer-shell background + fl-desktop */
	struct wlr_scene_tree *layer_bottom;
	struct wlr_scene_tree *layer_views;
	struct wlr_scene_tree *layer_top;        /* layer-shell top + fl-panel */
	struct wlr_scene_tree *layer_xunmanaged; /* X11 menus and tooltips (override-redirect) */
	struct wlr_scene_tree *layer_overlay;    /* layer-shell overlay + launcher */
	struct wlr_scene_tree *layer_popups;     /* popups of panel/launcher */

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_list views; /* flwm_view.link, most recently focused first */

	/* X11 programs (xwayland.c); NULL when off or not built in */
	struct wlr_xwayland *xwayland;
	struct wl_listener xwayland_ready;
	struct wl_listener new_xwayland_surface;

	struct wlr_layer_shell_v1 *layer_shell;
	struct wl_listener new_layer_surface;

	struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
	struct wl_listener new_toplevel_decoration;

	struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_mgr;

	/* workspaces (workspace.c): one set shared by all outputs, 0-based */
	int cur_workspace;
	struct wlr_ext_workspace_manager_v1 *workspace_mgr; /* NULL before wlroots 0.20 */
	struct wlr_ext_workspace_group_handle_v1 *workspace_group;
	struct wlr_ext_workspace_handle_v1 *workspace_handles[FLWM_MAX_WORKSPACES];
	int nworkspace_handles;
	struct wl_listener workspace_commit;

	struct wlr_xdg_activation_v1 *xdg_activation;
	struct wl_listener request_activate;

	struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;
	struct wl_listener new_virtual_keyboard;
	struct wlr_virtual_pointer_manager_v1 *virtual_pointer_mgr;
	struct wl_listener new_virtual_pointer;

	struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
	struct wl_listener request_set_cursor_shape;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener pointer_focus_change;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;
	struct wl_listener request_start_drag;
	struct wl_listener start_drag;
	struct wl_list keyboards;
	struct wlr_keyboard fallback_keyboard; /* active when no real keyboard is */
	struct wl_list pointers; /* flwm_pointer.link (for libinput config) */

	enum flwm_cursor_mode cursor_mode;
	struct flwm_view *grabbed_view;
	enum flwm_decor_part pressed_part;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;

	/* Alt+Tab cycling */
	bool cycling;
	uint32_t cycle_modifier;
	struct flwm_view *cycle_current;
	struct flwm_switcher switcher;

	struct flwm_layer_surface *focused_layer; /* exclusive keyboard focus */

	struct wlr_scene_tree *drag_icon;
	struct wl_listener drag_destroy;
	struct flwm_view *hover_view;      /* view whose title buttons show hover */
	uint32_t last_click_time;
	struct flwm_view *last_click_view;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;
	struct wl_listener layout_change;
	struct wlr_output_manager_v1 *output_manager;
	struct wl_listener output_manager_apply;
	struct wl_listener output_manager_test;

	/* screen locking (lock.c) */
	bool locked;
	struct wlr_scene_tree *layer_lock; /* above everything; enabled while locked */
	struct wlr_scene_rect *lock_cover;
	pid_t lock_pid; /* our fl-lock --locker child */
	long lock_started;
	struct wl_event_source *lock_restart_timer, *idle_timer, *sigchld;
	struct wlr_session_lock_manager_v1 *lock_mgr;
	struct wl_listener new_ext_lock;
	struct wlr_session_lock_v1 *ext_lock;
	struct wl_listener ext_new_surface, ext_unlock, ext_destroy;
	struct wl_list lock_surfaces; /* flwm_lock_surface.link */
	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
	struct wl_listener new_idle_inhibitor;
	int idle_inhibitors;

	struct flwm_config config;
	char exe_dir[4096]; /* directory of our own executable */
	bool desktop_shown; /* show-desktop toggled */
};

struct flwm_output {
	struct wl_list link;
	struct flwm_server *server;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;
	struct wlr_box full_area;   /* layout coordinates */
	struct wlr_box usable_area; /* minus panel/exclusive zones */
	struct wl_list layers[4];   /* flwm_layer_surface.link per zwlr layer */
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

struct flwm_decor {
	struct wlr_scene_tree *tree;
	struct wlr_scene_buffer *title;     /* rendered title bar */
	struct wlr_scene_rect *border[4];   /* visible 1px border: top/bottom/left/right */
	struct wlr_scene_rect *handle[8];   /* invisible resize handles */
	struct flwm_node_desc title_desc;
	struct flwm_node_desc handle_desc[8];
	int width, height;                  /* content size the decor was built for */
	bool active;
	char *title_text;
	float scale;
	int hover_button; /* enum flwm_decor_part under the pointer */
};

struct flwm_view {
	struct wl_list link;
	struct flwm_server *server;
	enum flwm_view_type type;
	struct wlr_xdg_toplevel *xdg_toplevel;    /* FLWM_VIEW_XDG */
	struct wlr_xwayland_surface *xsurface;    /* FLWM_VIEW_XWAYLAND */
	struct wlr_scene_tree *tree;          /* positioned at content top-left */
	struct wlr_scene_tree *surface_tree;
	struct flwm_node_desc desc;
	struct flwm_decor decor;
	bool ssd;          /* we draw decorations */
	bool mapped;
	bool minimized, maximized, fullscreen;
	bool placed;       /* initial position chosen */
	bool hidden_by_show_desktop;
	bool no_taskbar;   /* not in the panel's window list (X11 menus, splash screens) */
	int workspace;     /* 0-based; see view_on_current() */
	bool sticky;       /* shown on every workspace */
	int requested_w, requested_h; /* last size we asked a shell client for */
	int committed_w, committed_h; /* last committed size of a shell client */
	enum flwm_role role;
	bool role_checked;
	int x, y;          /* content (window geometry) position, layout coords */
	struct wlr_box saved; /* geometry before maximize/fullscreen/snap */
	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	struct wlr_foreign_toplevel_handle_v1 *ftl;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_minimize;
	struct wl_listener request_fullscreen;
	struct wl_listener set_title;
	struct wl_listener set_app_id;

	struct wl_listener decoration_request_mode;
	struct wl_listener decoration_destroy;

	/* X11 windows only */
	struct wl_listener x_associate;
	struct wl_listener x_dissociate;
	struct wl_listener x_request_configure;
	struct wl_listener x_request_activate;
	struct wl_listener x_set_decorations;
	struct wl_listener x_set_override_redirect;
	bool x_associated;

	struct wl_listener ftl_request_activate;
	struct wl_listener ftl_request_maximize;
	struct wl_listener ftl_request_minimize;
	struct wl_listener ftl_request_fullscreen;
	struct wl_listener ftl_request_close;
	struct wl_listener ftl_destroy;
};

struct flwm_popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct flwm_layer_surface {
	struct wl_list link; /* flwm_output.layers[layer] */
	struct flwm_server *server;
	struct flwm_output *output;
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_scene_layer_surface_v1 *scene;
	struct wlr_scene_tree *popup_tree;
	struct flwm_node_desc desc;
	bool mapped;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener new_popup;
	struct wl_listener output_destroy;
};

struct flwm_keyboard {
	struct wl_list link;
	struct flwm_server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener keymap;
	struct wl_listener destroy;
};

struct flwm_pointer {
	struct wl_list link;
	struct wlr_input_device *device;
	struct wl_listener destroy;
};

/* main.c */
void flwm_spawn(struct flwm_server *server, const char *cmd);

/* config.c */
void config_init_defaults(struct flwm_config *c);
bool config_load(struct flwm_config *c, const char *path);
void config_free(struct flwm_config *c);
char *config_default_path(void);

/* output.c */
void output_init(struct flwm_server *server);
struct flwm_output *output_at(struct flwm_server *server, double lx, double ly);

/* switcher.c */
bool switcher_active(struct flwm_server *server);
bool switcher_step(struct flwm_server *server, int dir);
void switcher_step_row(struct flwm_server *server, int dir);
void switcher_finish(struct flwm_server *server, bool commit);
void switcher_click(struct flwm_server *server, double lx, double ly);
void switcher_view_gone(struct flwm_server *server, struct flwm_view *view);
/* writes the normal windows (layout x y w h, then the title; topmost first)
 * to $XDG_RUNTIME_DIR/fl-wm-windows, for fl-snip's window mode (SIGUSR2) */
void view_dump_windows(struct flwm_server *server);
void switcher_output_gone(struct flwm_server *server, struct flwm_output *o);
struct flwm_output *output_primary(struct flwm_server *server);
struct flwm_output *output_for_view(struct flwm_view *view);
void output_arrange(struct flwm_output *output);
void arrange_all(struct flwm_server *server);
void output_apply_config(struct flwm_server *server);
void output_manager_update(struct flwm_server *server);
void output_finish(struct flwm_server *server);
int shell_reserved_bottom(struct flwm_server *server, struct flwm_output *output);

/* view.c */
void view_init(struct flwm_server *server);
/* the parts shared by xdg and X11 windows (xwayland.c uses them) */
struct flwm_view *view_new(struct flwm_server *server, enum flwm_view_type type);
void view_handle_map(struct flwm_view *view, bool maximized, bool fullscreen, bool focus);
void view_handle_unmap(struct flwm_view *view);
void view_handle_commit(struct flwm_view *view);
void view_free(struct flwm_view *view); /* removes the common listeners */
struct wlr_surface *view_surface(struct flwm_view *view);
const char *view_title(struct flwm_view *view);  /* may be NULL */
const char *view_app_id(struct flwm_view *view); /* may be NULL */
void view_focus(struct flwm_view *view);
void view_raise(struct flwm_view *view);
void view_set_position(struct flwm_view *view, int x, int y);
void view_get_geometry(struct flwm_view *view, struct wlr_box *box);
void view_close(struct flwm_view *view);
void view_set_maximized(struct flwm_view *view, bool maximized);
void view_set_minimized(struct flwm_view *view, bool minimized);
void view_set_fullscreen(struct flwm_view *view, bool fullscreen);
void view_snap(struct flwm_view *view, int side); /* -1 left, +1 right */
void view_begin_interactive(struct flwm_view *view, enum flwm_cursor_mode mode, uint32_t edges);
struct flwm_view *view_focused(struct flwm_server *server);
struct flwm_view *view_find_role(struct flwm_server *server, enum flwm_role role);
void view_focus_next_available(struct flwm_server *server);
void view_reposition_shell(struct flwm_view *view);
void view_update_decor(struct flwm_view *view);
bool view_is_normal(struct flwm_view *view);
void views_reconfigure_maximized(struct flwm_server *server);
void views_show_desktop(struct flwm_server *server);
void view_popup_create(struct wlr_xdg_popup *xdg_popup, struct wlr_scene_tree *parent_tree);
/* the panel's window list handle (destroyed while the view's workspace is hidden) */
void view_ftl_create(struct flwm_view *view);
void view_ftl_destroy(struct flwm_view *view);

/* workspace.c */
void workspace_init(struct flwm_server *server);
void workspace_finish(struct flwm_server *server);
void workspace_apply_config(struct flwm_server *server); /* after a reload */
void workspace_output_added(struct flwm_server *server, struct wlr_output *output);
void workspace_output_removed(struct flwm_server *server, struct wlr_output *output);
bool view_on_current(struct flwm_view *view);
/* a window Alt+Tab, the panel and focus-next may pick */
bool view_is_cyclable(struct flwm_view *view);
void workspace_switch(struct flwm_server *server, int n); /* 0-based */
void view_move_to_workspace(struct flwm_view *view, int n);
void view_set_sticky(struct flwm_view *view, bool sticky);

/* decor.c */
void decor_create(struct flwm_view *view);
void decor_update(struct flwm_view *view);
void decor_destroy(struct flwm_view *view);
void decor_set_hover(struct flwm_view *view, enum flwm_decor_part part);
enum flwm_decor_part decor_part_at(struct flwm_view *view, double lx, double ly);
int decor_title_height(struct flwm_server *server);
int decor_border(struct flwm_server *server);

/* xwayland.c (stubs below when built without Xwayland) */
#ifdef FLWM_XWAYLAND
void xwayland_init(struct flwm_server *server);
void xwayland_finish(struct flwm_server *server);
void xwayland_update_cursor(struct flwm_server *server);
const char *xwayland_display(struct flwm_server *server); /* NULL when not running */
struct wlr_surface *xwayland_view_surface(struct flwm_view *view);
const char *xwayland_view_title(struct flwm_view *view);
const char *xwayland_view_app_id(struct flwm_view *view);
void xwayland_view_size(struct flwm_view *view, int *w, int *h);
void xwayland_view_configure(struct flwm_view *view, int x, int y, int w, int h);
void xwayland_view_move(struct flwm_view *view, int x, int y); /* keeps the size last asked for */
void xwayland_view_activate(struct flwm_view *view, bool activated);
void xwayland_view_raise(struct flwm_view *view);
void xwayland_view_close(struct flwm_view *view);
void xwayland_view_set_maximized(struct flwm_view *view, bool maximized);
void xwayland_view_set_fullscreen(struct flwm_view *view, bool fullscreen);
void xwayland_view_set_minimized(struct flwm_view *view, bool minimized);
struct flwm_view *xwayland_view_parent(struct flwm_view *view);
bool xwayland_view_wants_focus(struct flwm_view *view);
bool xwayland_view_position_hint(struct flwm_view *view, int *x, int *y);
#else
static inline void xwayland_init(struct flwm_server *s) { (void)s; }
static inline void xwayland_finish(struct flwm_server *s) { (void)s; }
static inline void xwayland_update_cursor(struct flwm_server *s) { (void)s; }
static inline const char *xwayland_display(struct flwm_server *s) { (void)s; return NULL; }
static inline struct wlr_surface *xwayland_view_surface(struct flwm_view *v) { (void)v; return NULL; }
static inline const char *xwayland_view_title(struct flwm_view *v) { (void)v; return NULL; }
static inline const char *xwayland_view_app_id(struct flwm_view *v) { (void)v; return NULL; }
static inline void xwayland_view_size(struct flwm_view *v, int *w, int *h) { (void)v; *w = *h = 0; }
static inline void xwayland_view_configure(struct flwm_view *v, int x, int y, int w, int h) { (void)v; (void)x; (void)y; (void)w; (void)h; }
static inline void xwayland_view_move(struct flwm_view *v, int x, int y) { (void)v; (void)x; (void)y; }
static inline void xwayland_view_activate(struct flwm_view *v, bool a) { (void)v; (void)a; }
static inline void xwayland_view_raise(struct flwm_view *v) { (void)v; }
static inline void xwayland_view_close(struct flwm_view *v) { (void)v; }
static inline void xwayland_view_set_maximized(struct flwm_view *v, bool m) { (void)v; (void)m; }
static inline void xwayland_view_set_fullscreen(struct flwm_view *v, bool f) { (void)v; (void)f; }
static inline void xwayland_view_set_minimized(struct flwm_view *v, bool m) { (void)v; (void)m; }
static inline struct flwm_view *xwayland_view_parent(struct flwm_view *v) { (void)v; return NULL; }
static inline bool xwayland_view_wants_focus(struct flwm_view *v) { (void)v; return true; }
static inline bool xwayland_view_position_hint(struct flwm_view *v, int *x, int *y) { (void)v; (void)x; (void)y; return false; }
#endif

/* layer.c */
void layer_init(struct flwm_server *server);
void layers_arrange(struct flwm_output *output);
void layer_focus_none(struct flwm_server *server);
void layer_clicked(struct flwm_layer_surface *ls);

/* input.c */
/* lock.c */
void lock_init(struct flwm_server *server);
void lock_finish(struct flwm_server *server);
void lock_request(struct flwm_server *server);
void lock_arrange(struct flwm_server *server);
void lock_activity(struct flwm_server *server);

/* gesture.c */
void gesture_init(struct flwm_server *server);
void gesture_finish(void);
void lock_view_mapped(struct flwm_server *server, struct flwm_view *view);
bool lock_client_is_locker(struct flwm_server *server, struct wl_client *client);

void input_init(struct flwm_server *server);
void input_finish(struct flwm_server *server);
void input_apply_config(struct flwm_server *server);
void input_reset_cursor_mode(struct flwm_server *server);
void input_update_pointer_focus(struct flwm_server *server, uint32_t time);
struct flwm_node_desc *input_desc_at(struct flwm_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy);
bool input_run_action(struct flwm_server *server, enum flwm_action_type action, const char *arg);

#endif
