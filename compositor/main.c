/* fl-wm: the fldesktop Wayland compositor (wlroots based) */
#define _POSIX_C_SOURCE 200809L
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>

#include "server.h"

void flwm_spawn(struct flwm_server *server, const char *cmd) {
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log_errno(WLR_ERROR, "fork");
		return;
	}
	if (pid == 0) {
		/* double fork so the child is reparented and never becomes our zombie */
		setsid();
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", cmd, (void *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	waitpid(pid, NULL, 0);
}

static int handle_signal(int sig, void *data) {
	struct flwm_server *server = data;
	if (sig == SIGHUP) {
		input_run_action(server, FLWM_ACTION_RELOAD, NULL);
		return 0;
	}
	if (sig == SIGUSR1) {
		lock_request(server);
		return 0;
	}
	wl_display_terminate(server->wl_display);
	return 0;
}

static void usage(const char *argv0) {
	printf("Usage: %s [options]\n"
		"  -c, --config FILE    configuration file (default ~/.config/fldesktop/wm.ini)\n"
		"  -s, --startup CMD    run CMD after startup (instead of [autostart])\n"
		"  -n, --no-autostart   do not run the [autostart] programs\n"
		"  -d, --debug          verbose logging\n"
		"  -v, --version        print version\n", argv0);
}

static void find_exe_dir(struct flwm_server *server) {
	char path[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
	if (n <= 0) {
		snprintf(server->exe_dir, sizeof server->exe_dir, "%s", FLD_BINDIR);
		return;
	}
	path[n] = 0;
	char *slash = strrchr(path, '/');
	if (slash) *slash = 0;
	snprintf(server->exe_dir, sizeof server->exe_dir, "%s", path);
}

int main(int argc, char *argv[]) {
	const char *config_path = NULL, *startup_cmd = NULL;
	bool autostart = true;
	enum wlr_log_importance log_level = WLR_INFO;
	static const struct option opts[] = {
		{"config", required_argument, NULL, 'c'},
		{"startup", required_argument, NULL, 's'},
		{"no-autostart", no_argument, NULL, 'n'},
		{"debug", no_argument, NULL, 'd'},
		{"version", no_argument, NULL, 'v'},
		{"help", no_argument, NULL, 'h'},
		{0, 0, 0, 0},
	};
	int c;
	while ((c = getopt_long(argc, argv, "c:s:ndvh", opts, NULL)) != -1) {
		switch (c) {
		case 'c': config_path = optarg; break;
		case 's': startup_cmd = optarg; break;
		case 'n': autostart = false; break;
		case 'd': log_level = WLR_DEBUG; break;
		case 'v': printf("fl-wm %s\n", FLD_VERSION); return 0;
		default: usage(argv[0]); return c == 'h' ? 0 : 1;
		}
	}
	wlr_log_init(log_level, NULL);

	struct flwm_server server = {0};
	find_exe_dir(&server);
	config_init_defaults(&server.config);
	char *default_path = config_default_path();
	const char *candidates[] = {config_path, default_path, "/etc/xdg/fldesktop/wm.ini",
		FLD_DATADIR "/wm.ini"};
	for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
		if (!candidates[i]) continue;
		if (config_load(&server.config, candidates[i])) {
			wlr_log(WLR_INFO, "loaded configuration %s", candidates[i]);
			break;
		}
		if (i == 0) wlr_log(WLR_ERROR, "cannot read %s", candidates[i]);
	}
	if (!server.config.path) server.config.path = strdup(default_path);
	free(default_path);

	/* children get a clean environment describing this session */
	setenv("XDG_CURRENT_DESKTOP", "fldesktop", 0);
	setenv("XDG_SESSION_DESKTOP", "fldesktop", 0);
	setenv("XDG_SESSION_TYPE", "wayland", 1);
	setenv("MOZ_ENABLE_WAYLAND", "1", 0);
	setenv("QT_QPA_PLATFORM", "wayland;xcb", 0);
	setenv("SDL_VIDEODRIVER", "wayland", 0);
	setenv("_JAVA_AWT_WM_NONREPARENTING", "1", 0);
	unsetenv("DISPLAY");
	char pidbuf[32];
	snprintf(pidbuf, sizeof pidbuf, "%d", (int)getpid());
	setenv("FLWM_PID", pidbuf, 1);
	/* the PATH lookup finds our own helpers even when running from a build dir */
	const char *old_path = getenv("PATH");
	char *newpath = malloc(strlen(server.exe_dir) + (old_path ? strlen(old_path) : 0) + 2);
	sprintf(newpath, "%s:%s", server.exe_dir, old_path ? old_path : "/usr/bin:/bin");
	setenv("PATH", newpath, 1);
	free(newpath);
	signal(SIGPIPE, SIG_IGN);

	server.wl_display = wl_display_create();
	server.event_loop = wl_display_get_event_loop(server.wl_display);
	server.backend = wlr_backend_autocreate(server.event_loop, &server.session);
	if (!server.backend) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		return 1;
	}
	server.renderer = wlr_renderer_autocreate(server.backend);
	if (!server.renderer) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return 1;
	}
	wlr_renderer_init_wl_display(server.renderer, server.wl_display);
	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (!server.allocator) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		return 1;
	}

	server.compositor = wlr_compositor_create(server.wl_display, 6, server.renderer);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);
	wlr_primary_selection_v1_device_manager_create(server.wl_display);
	wlr_data_control_manager_v1_create(server.wl_display);
	wlr_viewporter_create(server.wl_display);
	wlr_single_pixel_buffer_manager_v1_create(server.wl_display);
	wlr_fractional_scale_manager_v1_create(server.wl_display, 1);
#if FLWM_WLR_AT_LEAST(0, 19)
	wlr_presentation_create(server.wl_display, server.backend, 2);
#else
	wlr_presentation_create(server.wl_display, server.backend);
#endif
	wlr_export_dmabuf_manager_v1_create(server.wl_display);
	wlr_screencopy_manager_v1_create(server.wl_display);
	wlr_gamma_control_manager_v1_create(server.wl_display);
	wlr_relative_pointer_manager_v1_create(server.wl_display);
	wlr_pointer_constraints_v1_create(server.wl_display);
	/* KDE's protocol, used by some toolkits to ask for server side decorations */
	struct wlr_server_decoration_manager *sdm = wlr_server_decoration_manager_create(server.wl_display);
	wlr_server_decoration_manager_set_default_mode(sdm, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);

	server.output_layout = wlr_output_layout_create(server.wl_display);
	wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);

	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);
	server.layer_background = wlr_scene_tree_create(&server.scene->tree);
	server.layer_bottom = wlr_scene_tree_create(&server.scene->tree);
	server.layer_views = wlr_scene_tree_create(&server.scene->tree);
	server.layer_top = wlr_scene_tree_create(&server.scene->tree);
	server.layer_overlay = wlr_scene_tree_create(&server.scene->tree);
	server.layer_popups = wlr_scene_tree_create(&server.scene->tree);
	lock_init(&server); /* its layer goes above all the others */

	output_init(&server);
	view_init(&server);
	layer_init(&server);
	input_init(&server);

	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_backend_destroy(server.backend);
		return 1;
	}
	if (!wlr_backend_start(server.backend)) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}
	setenv("WAYLAND_DISPLAY", socket, true);
	wlr_log(WLR_INFO, "fl-wm %s running on WAYLAND_DISPLAY=%s", FLD_VERSION, socket);

	struct wl_event_source *sig_int = wl_event_loop_add_signal(server.event_loop, SIGINT, handle_signal, &server);
	struct wl_event_source *sig_term = wl_event_loop_add_signal(server.event_loop, SIGTERM, handle_signal, &server);
	struct wl_event_source *sig_hup = wl_event_loop_add_signal(server.event_loop, SIGHUP, handle_signal, &server);
	struct wl_event_source *sig_usr1 = wl_event_loop_add_signal(server.event_loop, SIGUSR1, handle_signal, &server);

	/* export the session to D-Bus / systemd activated services when available */
	flwm_spawn(&server, "command -v dbus-update-activation-environment >/dev/null && "
		"dbus-update-activation-environment --systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP "
		"XDG_SESSION_TYPE FLWM_PID 2>/dev/null");

	if (startup_cmd) flwm_spawn(&server, startup_cmd);
	else if (autostart)
		for (int i = 0; i < server.config.nautostart; i++) flwm_spawn(&server, server.config.autostart[i]);

	wl_display_run(server.wl_display);

	wl_event_source_remove(sig_int);
	wl_event_source_remove(sig_term);
	wl_event_source_remove(sig_hup);
	wl_event_source_remove(sig_usr1);
	lock_finish(&server);
	wl_display_destroy_clients(server.wl_display);

	/* wlroots asserts that no listeners remain when objects are destroyed */
	struct wl_listener *listeners[] = {
		&server.new_xdg_toplevel, &server.new_xdg_popup, &server.new_layer_surface,
		&server.new_toplevel_decoration, &server.request_activate,
		&server.request_set_cursor_shape, &server.cursor_motion,
		&server.cursor_motion_absolute, &server.cursor_button, &server.cursor_axis,
		&server.cursor_frame, &server.new_input, &server.request_cursor,
		&server.pointer_focus_change, &server.request_set_selection,
		&server.request_set_primary_selection, &server.request_start_drag,
		&server.start_drag, &server.new_output, &server.layout_change,
		&server.new_virtual_keyboard, &server.new_virtual_pointer,
	};
	for (size_t i = 0; i < sizeof listeners / sizeof listeners[0]; i++)
		wl_list_remove(&listeners[i]->link);
	input_finish(&server);
	output_finish(&server);

	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.wl_display);
	config_free(&server.config);
	return 0;
}
