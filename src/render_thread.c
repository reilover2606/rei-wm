#include "render_thread.h"

#include <event2/event.h>
#include <event2/thread.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "messages.h"
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
	int32_t width, height;
	struct rei_render_ops ops;
	void *ops_data;
	bool ops_ready;

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

static void free_resize(void *p) { free(p); }
static void free_snapshot(void *p) { rei_snapshot_unref(p); }

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

static void on_vblank(evutil_socket_t fd, short what, void *arg)
{
	(void)fd;
	(void)what;
	struct rei_render *r = arg;
	r->timer_armed = false;

	if (!r->dirty || !r->pending)
		return;

	struct rei_scene_snapshot *snap = r->pending;
	if (r->ops.present && r->ops_ready)
		r->ops.present(r->ops_data, snap, r->width, r->height);
	r->dirty = false;

	struct rei_frame_event *ev = rei_xcalloc(1, sizeof *ev);
	ev->output_id = r->output_id;
	ev->seq = ++r->seq;
	ev->present_ns = rei_now_ns();
	ev->scene_serial = snap->serial;
	rei_msgq_post(&r->server->inbox, REI_EMSG_FRAME, ev, free);
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
	case REI_RMSG_RESIZE: {
		struct rei_resize_msg *rs = m->data;
		r->width = rs->width;
		r->height = rs->height;
		r->dirty = r->pending != NULL;
		arm_timer(r);
		break;
	}
	case REI_RMSG_STOP:
		event_base_loopbreak(r->base);
		break;
	}
}

static void *render_main(void *arg)
{
	struct rei_render *r = arg;
	rei_log_set_thread_name(r->log_name);

	if (r->ops.init) {
		r->ops_ready = r->ops.init(r->ops_data);
		if (!r->ops_ready) {
			rei_error("render init failed for %s", r->name);
			struct rei_frame_event *ev = rei_xcalloc(1, sizeof *ev);
			ev->output_id = r->output_id;
			rei_msgq_post(&r->server->inbox, REI_EMSG_RENDER_FAILED, ev, free);
		}
	} else {
		r->ops_ready = true;
	}

	r->clock_origin_ns = rei_now_ns();
	/* Without NO_EXIT_ON_EMPTY, libevent returns as soon as no event is
	 * pending, which here is right after the first frame: the wake-up event
	 * in the message queue is deliberately not persistent. */
	event_base_loop(r->base, EVLOOP_NO_EXIT_ON_EMPTY);

	if (r->ops.fini && r->ops_ready)
		r->ops.fini(r->ops_data);
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
	r->width = params->width;
	r->height = params->height;
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

void rei_render_submit(struct rei_render *r, struct rei_scene_snapshot *snap)
{
	rei_msgq_post(&r->inbox, REI_RMSG_SNAPSHOT, rei_snapshot_ref(snap), free_snapshot);
}

void rei_render_resize(struct rei_render *r, int32_t width, int32_t height)
{
	struct rei_resize_msg *m = rei_xcalloc(1, sizeof *m);
	m->width = width;
	m->height = height;
	rei_msgq_post(&r->inbox, REI_RMSG_RESIZE, m, free_resize);
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
