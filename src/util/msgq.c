#include "util/msgq.h"

#include <event2/event.h>
#include <stdlib.h>

#include "util/util.h"

static void msg_free(struct rei_msg *m)
{
	if (m->destroy && m->data)
		m->destroy(m->data);
	free(m);
}

static void list_free(struct rei_msg *m)
{
	while (m) {
		struct rei_msg *next = m->next;
		msg_free(m);
		m = next;
	}
}

static void wake_cb(evutil_socket_t fd, short what, void *arg)
{
	(void)fd;
	(void)what;
	struct rei_msgq *q = arg;

	for (;;) {
		pthread_mutex_lock(&q->lock);
		struct rei_msg *list = q->head;
		q->head = q->tail = NULL;
		pthread_mutex_unlock(&q->lock);

		if (!list)
			return;

		while (list) {
			struct rei_msg *m = list;
			list = m->next;
			q->handler(m, q->userdata);
			msg_free(m);
		}
	}
}

bool rei_msgq_init(struct rei_msgq *q, struct event_base *base,
		   rei_msgq_handler handler, void *userdata)
{
	q->head = q->tail = NULL;
	q->closed = false;
	q->handler = handler;
	q->userdata = userdata;
	pthread_mutex_init(&q->lock, NULL);

	/* Not persistent: event_active() fires it once per activation. An
	 * activation that lands while the callback runs simply queues another
	 * (possibly empty) callback, so no wake-up is ever lost. */
	q->wake = event_new(base, -1, 0, wake_cb, q);
	if (!q->wake) {
		pthread_mutex_destroy(&q->lock);
		return false;
	}
	return true;
}

bool rei_msgq_post(struct rei_msgq *q, int type, void *data,
		   void (*destroy)(void *))
{
	struct rei_msg *m = rei_xcalloc(1, sizeof *m);
	m->type = type;
	m->data = data;
	m->destroy = destroy;

	pthread_mutex_lock(&q->lock);
	if (q->closed) {
		pthread_mutex_unlock(&q->lock);
		msg_free(m);
		return false;
	}
	if (q->tail)
		q->tail->next = m;
	else
		q->head = m;
	q->tail = m;
	pthread_mutex_unlock(&q->lock);

	event_active(q->wake, 0, 0);
	return true;
}

void rei_msgq_close(struct rei_msgq *q)
{
	pthread_mutex_lock(&q->lock);
	q->closed = true;
	struct rei_msg *list = q->head;
	q->head = q->tail = NULL;
	pthread_mutex_unlock(&q->lock);
	list_free(list);
}

void rei_msgq_finish(struct rei_msgq *q)
{
	rei_msgq_close(q);
	if (q->wake) {
		event_free(q->wake);
		q->wake = NULL;
	}
	pthread_mutex_destroy(&q->lock);
}
