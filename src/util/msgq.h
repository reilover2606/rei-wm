#ifndef REI_MSGQ_H
#define REI_MSGQ_H

#include <pthread.h>
#include <stdbool.h>

struct event;
struct event_base;

/*
 * A multi-producer, single-consumer message queue that wakes a libevent loop.
 *
 * This is the only way threads talk to each other in rei-wm: any thread may
 * post, and the owning thread's event loop calls the handler. Messages are
 * delivered in the order they were posted (per producer).
 *
 * libevent must have been told about threads (evthread_use_pthreads()) before
 * the event base was created, otherwise cross-thread wake-ups are unsafe.
 */

struct rei_msg {
	struct rei_msg *next;
	int type;
	void *data;               /* Owned by the message. */
	void (*destroy)(void *);  /* Frees data; may be NULL (data not owned). */
};

typedef void (*rei_msgq_handler)(struct rei_msg *msg, void *userdata);

struct rei_msgq {
	pthread_mutex_t lock;
	struct rei_msg *head, *tail;
	struct event *wake;
	bool closed;
	rei_msgq_handler handler;
	void *userdata;
};

/* Must be called on the thread that will run `base`. */
bool rei_msgq_init(struct rei_msgq *q, struct event_base *base,
		   rei_msgq_handler handler, void *userdata);

/* Thread-safe. Takes ownership of `data` (destroyed via `destroy`) whether or
 * not the post succeeds. Returns false if the queue has been closed. */
bool rei_msgq_post(struct rei_msgq *q, int type, void *data,
		   void (*destroy)(void *));

/* Rejects further posts and discards anything not yet delivered. */
void rei_msgq_close(struct rei_msgq *q);

/* Closes the queue and releases its resources. Call after the loop that
 * services it has stopped. */
void rei_msgq_finish(struct rei_msgq *q);

#endif
