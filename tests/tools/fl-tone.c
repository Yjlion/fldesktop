/* fl-tone: plays a quiet sine wave through PipeWire for mixer tests.
 *   fl-tone [seconds] [application name]      (0 = forever) */
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#define RATE 48000
#define CHANNELS 2

struct data {
	struct pw_main_loop *loop;
	struct pw_stream *stream;
	double phase;
};

static void on_process(void *userdata) {
	struct data *d = userdata;
	struct pw_buffer *b = pw_stream_dequeue_buffer(d->stream);
	if (!b) return;
	struct spa_buffer *buf = b->buffer;
	float *dst = buf->datas[0].data;
	if (!dst) return;
	int stride = sizeof(float) * CHANNELS;
	int n = buf->datas[0].maxsize / stride;
	if (b->requested && (int)b->requested < n) n = (int)b->requested;
	for (int i = 0; i < n; i++) {
		float v = 0.2f * (float)sin(d->phase);
		d->phase += 2 * M_PI * 440.0 / RATE;
		if (d->phase > 2 * M_PI) d->phase -= 2 * M_PI;
		for (int c = 0; c < CHANNELS; c++) *dst++ = v;
	}
	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = stride;
	buf->datas[0].chunk->size = n * stride;
	pw_stream_queue_buffer(d->stream, b);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_process,
};

static void quit(void *userdata, int sig) { pw_main_loop_quit(((struct data *)userdata)->loop); }
static void timeout(void *userdata, uint64_t expirations) { pw_main_loop_quit(((struct data *)userdata)->loop); }

int main(int argc, char *argv[]) {
	struct data d = {0};
	double secs = argc > 1 ? atof(argv[1]) : 10;
	const char *app = argc > 2 ? argv[2] : "Tone Generator";
	pw_init(&argc, &argv);
	d.loop = pw_main_loop_new(NULL);
	pw_loop_add_signal(pw_main_loop_get_loop(d.loop), SIGINT, quit, &d);
	pw_loop_add_signal(pw_main_loop_get_loop(d.loop), SIGTERM, quit, &d);
	if (secs > 0) {
		struct spa_source *t = pw_loop_add_timer(pw_main_loop_get_loop(d.loop), timeout, &d);
		struct timespec ts = {(time_t)secs, (long)((secs - (time_t)secs) * 1e9)};
		pw_loop_update_timer(pw_main_loop_get_loop(d.loop), t, &ts, NULL, false);
	}
	d.stream = pw_stream_new_simple(pw_main_loop_get_loop(d.loop), "fl-tone",
		pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
			PW_KEY_MEDIA_ROLE, "Music", PW_KEY_APP_NAME, app, PW_KEY_MEDIA_NAME, "440 Hz sine",
			PW_KEY_APP_ICON_NAME, "audio-x-generic", NULL),
		&stream_events, &d);
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[1];
	params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
		&SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32, .channels = CHANNELS, .rate = RATE));
	pw_stream_connect(d.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
		PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS, params, 1);
	pw_main_loop_run(d.loop);
	pw_stream_destroy(d.stream);
	pw_main_loop_destroy(d.loop);
	pw_deinit();
	return 0;
}
