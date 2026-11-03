#include "render_thread.h"

#include <errno.h>
#include <event2/event.h>
#include <event2/thread.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "messages.h"
#include "render/gles.h"
#include "server.h"
#include "util/log.h"
#include "util/msgq.h"
#include "util/util.h"

struct rei_render {
	struct rei_server *server;
	uint32_t output_id;
	char name[64];
	char log_name[80];

	/* Touched only by the render thread once it starts. */
	struct rei_gles_target target;
	struct rei_render_ops ops;
	void *ops_data;
	bool ops_ready;
	struct rei_gles *gles;

	struct event_base *base;
	struct rei_msgq inbox;
	struct event *timer;
	bool timer_armed;

	struct rei_scene_snapshot *pending;
	bool dirty;
	uint64_t clock_origin_ns;
	uint64_t interval_ns;
	uint64_t seq;

	pthread_t thread;
};

/* Arms the vblank timer for the next refresh boundary, if work is waiting. */
static void arm_timer(struct rei_render *r)
{
	if (r->timer_armed || !r->dirty)
		return;

	uint64_t now = rei_now_ns();
	uint64_t elapsed = now - r->clock_origin_ns;
	uint64_t next = r->clock_origin_ns +
			((elapsed / r->interval_ns) + 1) * r->interval_ns;
	uint64_t wait = next - now;

	struct timeval tv = {
		.tv_sec = (time_t)(wait / 1000000000ull),
		.tv_usec = (suseconds_t)((wait % 1000000000ull) / 1000ull),
	};
	evtimer_add(r->timer, &tv);
	r->timer_armed = true;
}

static bool draw_pending(struct rei_render *r)
{
	if (!r->gles || !r->pending)
		return false;
	return rei_gles_draw(r->gles, r->pending, &r->target);
}

static void on_vblank(evutil_socket_t fd, short what, void *arg)
{
	(void)fd;
	(void)what;
	struct rei_render *r = arg;
	r->timer_armed = false;

	if (!r->dirty || !r->pending)
		return;

	struct rei_scene_snapshot *snap = r->pending;
	bool drawn = draw_pending(r);
	if (r->ops.present && r->ops_ready && (drawn || !r->gles))
		r->ops.present(r->ops_data, drawn ? r->gles : NULL, snap,
			       r->target.width, r->target.height);
	r->dirty = false;

	struct rei_frame_event *ev = rei_xcalloc(1, sizeof *ev);
	ev->output_id = r->output_id;
	ev->seq = ++r->seq;
	ev->present_ns = rei_now_ns();
	ev->scene_serial = snap->serial;
	rei_msgq_post(&r->server->inbox, REI_EMSG_FRAME, ev, free);
}

static bool write_ppm(const char *path, const uint8_t *rgba, int32_t w, int32_t h)
{
	char tmp[300];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return false;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int32_t i = 0; i < w * h; i++)
		fwrite(rgba + (size_t)i * 4, 1, 3, f);
	bool ok = fflush(f) == 0 && !ferror(f);
	ok = (fclose(f) == 0) && ok;
	/* Rename last so a reader polling for the file never sees a partial one. */
	if (ok && rename(tmp, path) != 0)
		ok = false;
	if (!ok)
		unlink(tmp);
	return ok;
}

static void do_screenshot(struct rei_render *r, const char *path)
{
	bool ok = false;
	if (r->gles && r->pending && draw_pending(r)) {
		size_t bytes = (size_t)r->target.width * (size_t)r->target.height * 4;
		uint8_t *px = malloc(bytes);
		if (px && rei_gles_read_pixels(r->gles, px))
			ok = write_ppm(path, px, r->target.width, r->target.height);
		free(px);
	}

	struct rei_screenshot_event *ev = rei_xcalloc(1, sizeof *ev);
	ev->output_id = r->output_id;
	ev->ok = ok;
	snprintf(ev->path, sizeof ev->path, "%s", path);
	rei_msgq_post(&r->server->inbox, REI_EMSG_SCREENSHOT, ev, free);
}

static void on_message(struct rei_msg *m, void *userdata)
{
	struct rei_render *r = userdata;

	switch (m->type) {
	case REI_RMSG_SNAPSHOT:
		/* Newest snapshot wins; an unpresented older one is dropped. */
		rei_snapshot_unref(r->pending);
		r->pending = m->data;
		m->data = NULL;
		r->dirty = true;
		arm_timer(r);
		break;
	case REI_RMSG_GEOMETRY: {
		const struct rei_geometry_msg *g = m->data;
		r->target.width = g->width;
		r->target.height = g->height;
		r->target.origin_x = g->x;
		r->target.origin_y = g->y;
		r->target.scale = g->scale;
		r->dirty = r->pending != NULL;
		arm_timer(r);
		break;
	}
	case REI_RMSG_SCREENSHOT:
		do_screenshot(r, m->data);
		break;
	case REI_RMSG_STOP:
		event_base_loopbreak(r->base);
		break;
	}
}

static void *render_main(void *arg)
{
	struct rei_render *r = arg;
	rei_log_set_thread_name(r->log_name);

	r->gles = rei_gles_create(NULL);
	if (r->gles) {
		rei_info("output %s: rendering with %s", r->name, rei_gles_renderer_name(r->gles));
	} else {
		rei_error("output %s: OpenGL ES unavailable; frames will be paced but not drawn",
			  r->name);
		struct rei_frame_event *ev = rei_xcalloc(1, sizeof *ev);
		ev->output_id = r->output_id;
		rei_msgq_post(&r->server->inbox, REI_EMSG_RENDER_FAILED, ev, free);
	}

	r->ops_ready = true;
	if (r->ops.init && !r->ops.init(r->ops_data)) {
		rei_error("output %s: backend render init failed", r->name);
		r->ops_ready = false;
	}

	r->clock_origin_ns = rei_now_ns();
	/* Without NO_EXIT_ON_EMPTY, libevent returns as soon as no event is
	 * pending, which here is right after the first frame: the wake-up event
	 * in the message queue is deliberately not persistent. */
	event_base_loop(r->base, EVLOOP_NO_EXIT_ON_EMPTY);

	if (r->ops.fini && r->ops_ready)
		r->ops.fini(r->ops_data);
	rei_gles_destroy(r->gles);
	return NULL;
}

struct rei_render *rei_render_create(struct rei_server *server,
				     const struct rei_render_params *params)
{
	struct rei_render *r = rei_xcalloc(1, sizeof *r);
	r->server = server;
	r->output_id = params->output_id;
	snprintf(r->name, sizeof r->name, "%s", params->name);
	snprintf(r->log_name, sizeof r->log_name, "render:%s", r->name);
	r->target = (struct rei_gles_target){
		.width = params->width, .height = params->height,
		.origin_x = params->x, .origin_y = params->y,
		.scale = params->scale > 0 ? params->scale : 1.0,
		.draw_cursor = true,
	};
	r->ops = params->ops;
	r->ops_data = params->ops_data;

	int32_t mhz = params->refresh_mhz > 0 ? params->refresh_mhz : 60000;
	r->interval_ns = 1000000000000ull / (uint64_t)mhz;

	/* The base and its queue are created here and run on the new thread;
	 * libevent permits that once evthread_use_pthreads() has been called,
	 * and it means messages posted before the thread starts are not lost. */
	r->base = event_base_new();
	if (!r->base || !rei_msgq_init(&r->inbox, r->base, on_message, r)) {
		rei_error("cannot create render event loop for %s", r->name);
		if (r->base)
			event_base_free(r->base);
		free(r);
		return NULL;
	}
	r->timer = evtimer_new(r->base, on_vblank, r);

	if (pthread_create(&r->thread, NULL, render_main, r) != 0) {
		rei_error("cannot start render thread for %s", r->name);
		event_free(r->timer);
		rei_msgq_finish(&r->inbox);
		event_base_free(r->base);
		free(r);
		return NULL;
	}
	return r;
}

static void free_snapshot(void *p) { rei_snapshot_unref(p); }

void rei_render_submit(struct rei_render *r, struct rei_scene_snapshot *snap)
{
	rei_msgq_post(&r->inbox, REI_RMSG_SNAPSHOT, rei_snapshot_ref(snap), free_snapshot);
}

void rei_render_set_geometry(struct rei_render *r, int32_t width, int32_t height,
			     int32_t x, int32_t y, double scale)
{
	struct rei_geometry_msg *m = rei_xcalloc(1, sizeof *m);
	*m = (struct rei_geometry_msg){ width, height, x, y, scale };
	rei_msgq_post(&r->inbox, REI_RMSG_GEOMETRY, m, free);
}

void rei_render_screenshot(struct rei_render *r, const char *path)
{
	rei_msgq_post(&r->inbox, REI_RMSG_SCREENSHOT, rei_xstrdup(path), free);
}

void rei_render_destroy(struct rei_render *r)
{
	if (!r)
		return;
	rei_msgq_post(&r->inbox, REI_RMSG_STOP, NULL, NULL);
	pthread_join(r->thread, NULL);

	rei_msgq_finish(&r->inbox);
	event_free(r->timer);
	event_base_free(r->base);
	rei_snapshot_unref(r->pending);
	free(r);
}
