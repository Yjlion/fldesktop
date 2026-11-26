/* fl-wm's wm.ini parser */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "server.h"

/* switcher.c reads desktop.ini; not part of the parser */
bool flwm_desktop_dark(void) { return false; }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	int fd = memfd_create("wm.ini", 0);
	if (fd < 0) return 0;
	if (size && write(fd, data, size) != (ssize_t)size) { close(fd); return 0; }
	char path[64];
	snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
	struct flwm_config c;
	memset(&c, 0, sizeof c);
	config_init_defaults(&c);
	config_load(&c, path);
	config_free(&c);
	close(fd);
	return 0;
}
