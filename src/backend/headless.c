#include <errno.h>
#include <event2/event.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <wayland-server-protocol.h>

#include "backend/backend.h"
#include "input/input.h"
#include "proto/linux_dmabuf.h"
#include "proto/syncobj.h"
#include "proto/seat.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"

/*
 * Outputs with no display behind them: frames are "presented" on a simulated
 * vblank and discarded. Used for development without a GPU and for automated
 * tests; it is also the only backend that works in the cloud sandbox.
 *
 * It has virtual input devices: if REI_HEADLESS_INPUT names a FIFO, lines
 * written to it are injected as input events:
 *
 *   key <evdev-code> <0|1>        motion_abs <x> <y>     motion <dx> <dy>
 *   button <code> <0|1>           axis <v|h> <value>     release_all
 *   sync <token>                  (logs "input sync <token>" once reached)
 */

struct headless_backend {
	struct rei_backend base;
	int fifo_fd;
	struct event *fifo_event;
	char buf[4096];
	size_t len;
};

/* ---- a fake sync object backend for tests: a "timeline" is a file (a memfd the test client made) whose first 8 bytes
 * are the last signaled point; waiting is polling it every few milliseconds ---- */

#define FAKE_MAX 32
static struct { int fd; bool used; } g_fake_tl[FAKE_MAX];
static struct { int efd; uint32_t handle; uint64_t point; bool used; } g_fake_wait[FAKE_MAX];
static struct event *g_fake_timer;

static uint64_t fake_read(uint32_t handle)
{
	uint64_t v = 0;
	if (handle >= 1 && handle <= FAKE_MAX && g_fake_tl[handle - 1].used)
		if (pread(g_fake_tl[handle - 1].fd, &v, sizeof v, 0) != (ssize_t)sizeof v)
			v = 0;
	return v;
}

static void fake_poll(evutil_socket_t fd, short what, void *arg)
{
	(void)fd; (void)what;
	bool any = false;
	for (int i = 0; i < FAKE_MAX; i++) {
		if (!g_fake_wait[i].used)
			continue;
		if (fake_read(g_fake_wait[i].handle) >= g_fake_wait[i].point) {
			uint64_t one = 1;
			if (write(g_fake_wait[i].efd, &one, sizeof one) < 0) { /* the waiter is gone */ }
			g_fake_wait[i].used = false;
		} else {
			any = true;
		}
	}
	if (any) {
		struct timeval tv = { 0, 5000 };
		evtimer_add(g_fake_timer, &tv);
	}
	(void)arg;
}

static int fake_import(void *ctx, int fd, uint32_t *handle)
{
	(void)ctx;
	for (int i = 0; i < FAKE_MAX; i++)
		if (!g_fake_tl[i].used) {
			g_fake_tl[i].fd = dup(fd);
			if (g_fake_tl[i].fd < 0)
				return -1;
			g_fake_tl[i].used = true;
			*handle = (uint32_t)i + 1;
			return 0;
		}
	return -1;
}

static void fake_release(void *ctx, uint32_t handle)
{
	(void)ctx;
	if (handle >= 1 && handle <= FAKE_MAX && g_fake_tl[handle - 1].used) {
		close(g_fake_tl[handle - 1].fd);
		g_fake_tl[handle - 1].used = false;
	}
}

static int fake_wait(void *ctx, uint32_t handle, uint64_t point, int efd)
{
	struct rei_server *s = ctx;
	if (fake_read(handle) >= point) {
		uint64_t one = 1;
		return write(efd, &one, sizeof one) == (ssize_t)sizeof one ? 0 : -1;
	}
	for (int i = 0; i < FAKE_MAX; i++)
		if (!g_fake_wait[i].used) {
			g_fake_wait[i].efd = efd;
			g_fake_wait[i].handle = handle;
			g_fake_wait[i].point = point;
			g_fake_wait[i].used = true;
			if (!g_fake_timer)
				g_fake_timer = evtimer_new(s->base, fake_poll, NULL);
			struct timeval tv = { 0, 5000 };
			evtimer_add(g_fake_timer, &tv);
			return 0;
		}
	return -1;
}

static int fake_signal(void *ctx, uint32_t handle, uint64_t point)
{
	(void)ctx;
	if (handle < 1 || handle > FAKE_MAX || !g_fake_tl[handle - 1].used)
		return -1;
	uint64_t cur = fake_read(handle);
	if (point < cur)
		point = cur;
	return pwrite(g_fake_tl[handle - 1].fd, &point, sizeof point, 0) == (ssize_t)sizeof point ? 0 : -1;
}

static const struct rei_syncobj_ops fake_syncobj_ops = { fake_import, fake_release, fake_wait, fake_signal, true };

static uint32_t now_ms(void)
{
	return (uint32_t)(rei_now_ns() / 1000000ull);
}

static void handle_line(struct headless_backend *hb, char *line)
{
	struct rei_server *s = hb->base.server;
	uint32_t t = now_ms();
	unsigned code, down;
	double a, b;
	char word[16], tok[64];

	if (sscanf(line, "key %u %u", &code, &down) == 2)
		rei_input_key(s, t, code, down != 0);
	else if (sscanf(line, "motion_abs %lf %lf", &a, &b) == 2)
		rei_input_pointer_motion_abs(s, t, a, b);
	else if (sscanf(line, "motion %lf %lf", &a, &b) == 2)
		rei_input_pointer_motion(s, t, a, b);
	else if (sscanf(line, "button %u %u", &code, &down) == 2)
		rei_input_pointer_button(s, t, code, down != 0);
	else if (sscanf(line, "axis %15s %lf", word, &a) == 2)
		rei_input_pointer_axis(s, t, word[0] == 'h', a);
	else if (!strncmp(line, "release_all", 11))
		rei_input_release_all(s, t);
	else if (sscanf(line, "sync %63s", tok) == 1)
		rei_info("input sync %s", tok);
	else if (line[0])
		rei_warn("headless input: ignoring '%s'", line);
	rei_server_schedule_flush(s);
}

static void on_fifo(evutil_socket_t fd, short what, void *arg)
{
	(void)what;
	struct headless_backend *hb = arg;
	ssize_t n = read(fd, hb->buf + hb->len, sizeof hb->buf - hb->len - 1);
	if (n <= 0)
		return;
	hb->len += (size_t)n;
	hb->buf[hb->len] = '\0';

	char *start = hb->buf, *nl;
	while ((nl = strchr(start, '\n'))) {
		*nl = '\0';
		handle_line(hb, start);
		start = nl + 1;
	}
	hb->len = strlen(start);
	memmove(hb->buf, start, hb->len + 1);
	if (hb->len >= sizeof hb->buf - 1)
		hb->len = 0;  /* An overlong line: drop it rather than wedge. */
}

/* REI_HEADLESS_VSYNC=1 runs the headless outputs through the vsync-driven render
 * path real displays use (draw, queue a "flip", wait for it to complete), with
 * the flip completing after one refresh period. It exists so the automated tests
 * can exercise that path without a GPU. */
static enum rei_render_result headless_render(void *data, struct rei_gles *gles,
					      const struct rei_scene_snapshot *snap,
					      const struct rei_gles_target *target)
{
	(void)data;
	return rei_gles_draw(gles, snap, target) ? REI_RENDER_TIMED : REI_RENDER_FAILED;
}

static bool headless_start(struct rei_backend *b)
{
	struct headless_backend *hb = (struct headless_backend *)b;

	size_t n = rei_backend_output_count(b->server);
	for (size_t i = 0; i < n; i++) {
		char name[64];
		struct rei_output_params p;
		rei_backend_output_params(b->server, i, "HEADLESS", name, sizeof name, &p);
		const char *vsync = getenv("REI_HEADLESS_VSYNC");
		if (vsync && vsync[0] == '1')
			p.render_ops.render = headless_render;
		if (!rei_server_add_output(b->server, &p))
			return false;
	}

	/* A test hook: advertise linux-dmabuf (with feedback) as if a GPU were behind the outputs, so the protocol can be
	 * tested without one. No buffer can actually be imported. */
	if (getenv("REI_TEST_DMABUF")) {
		static const struct rei_format_mod fake[] = {
			{ REI_FORMAT_XRGB8888, REI_MOD_LINEAR }, { REI_FORMAT_XRGB8888, 0x0100000000000001ull },
			{ REI_FORMAT_ARGB8888, REI_MOD_LINEAR }, { REI_FORMAT_ABGR8888, REI_MOD_LINEAR },
		};
		rei_linux_dmabuf_init(b->server, fake, REI_ARRAY_LEN(fake), makedev(226, 128));
	}

	/* A test hook: explicit synchronization with a fake kernel (see above), shm buffers allowed. */
	if (getenv("REI_TEST_SYNCOBJ"))
		rei_syncobj_init(b->server, &fake_syncobj_ops, b->server);

	rei_seat_set_capabilities(b->server->seat,
				  WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);

	const char *path = getenv("REI_HEADLESS_INPUT");
	if (path && path[0]) {
		/* O_RDWR keeps a writer open, so the FIFO never reports end-of-file
		 * when a client of it closes. */
		hb->fifo_fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (hb->fifo_fd < 0) {
			rei_error("headless: cannot open input FIFO %s: %s", path, strerror(errno));
			return false;
		}
		hb->fifo_event = event_new(b->server->base, hb->fifo_fd, EV_READ | EV_PERSIST,
					   on_fifo, hb);
		event_add(hb->fifo_event, NULL);
		rei_info("headless: reading input events from %s", path);
	}
	return true;
}

static void headless_destroy(struct rei_backend *b)
{
	struct headless_backend *hb = (struct headless_backend *)b;
	struct rei_server *s = b->server;
	if (hb->fifo_event)
		event_free(hb->fifo_event);
	if (hb->fifo_fd > 0)
		close(hb->fifo_fd);
	while (!wl_list_empty(&s->outputs)) {
		struct rei_output *o = wl_container_of(s->outputs.next, o, link);
		rei_server_remove_output(s, o);
	}
	free(hb);
}

static const struct rei_backend_impl headless_impl = {
	.name = "headless",
	.start = headless_start,
	.destroy = headless_destroy,
};

struct rei_backend *rei_headless_backend_create(struct rei_server *server)
{
	struct headless_backend *hb = rei_xcalloc(1, sizeof *hb);
	hb->base.impl = &headless_impl;
	hb->base.server = server;
	return &hb->base;
}
