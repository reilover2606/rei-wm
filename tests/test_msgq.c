#include <event2/event.h>
#include <event2/thread.h>
#include <pthread.h>
#include <stdlib.h>

#include "check.h"
#include "util/msgq.h"

#define PRODUCERS 4
#define PER_PRODUCER 5000
#define MSG_DATA 1
#define MSG_DONE 2

struct item {
	int producer;
	int seq;
};

struct consumer {
	struct event_base *base;
	int next_seq[PRODUCERS];
	long received;
	int done_count;
	long frees;
};

static pthread_mutex_t frees_lock = PTHREAD_MUTEX_INITIALIZER;
static long g_frees;

static void counting_free(void *p)
{
	pthread_mutex_lock(&frees_lock);
	g_frees++;
	pthread_mutex_unlock(&frees_lock);
	free(p);
}

static void handler(struct rei_msg *m, void *ud)
{
	struct consumer *c = ud;
	if (m->type == MSG_DATA) {
		struct item *it = m->data;
		/* Messages from one producer must arrive in order. */
		CHECK(it->seq == c->next_seq[it->producer]);
		c->next_seq[it->producer]++;
		c->received++;
	} else if (m->type == MSG_DONE) {
		if (++c->done_count == PRODUCERS)
			event_base_loopexit(c->base, NULL);
	}
}

struct producer_arg {
	struct rei_msgq *q;
	int id;
};

static void *producer(void *arg)
{
	struct producer_arg *pa = arg;
	for (int i = 0; i < PER_PRODUCER; i++) {
		struct item *it = malloc(sizeof *it);
		it->producer = pa->id;
		it->seq = i;
		CHECK(rei_msgq_post(pa->q, MSG_DATA, it, counting_free));
	}
	CHECK(rei_msgq_post(pa->q, MSG_DONE, NULL, NULL));
	return NULL;
}

int main(void)
{
	CHECK(evthread_use_pthreads() == 0);

	struct consumer c = { .base = event_base_new() };
	CHECK(c.base);
	struct rei_msgq q;
	CHECK(rei_msgq_init(&q, c.base, handler, &c));

	pthread_t th[PRODUCERS];
	struct producer_arg args[PRODUCERS];
	for (int i = 0; i < PRODUCERS; i++) {
		args[i] = (struct producer_arg){ &q, i };
		pthread_create(&th[i], NULL, producer, &args[i]);
	}

	/* The loop starts after producers are already posting, so this covers
	 * both messages queued before the loop runs and live wake-ups. */
	event_base_loop(c.base, EVLOOP_NO_EXIT_ON_EMPTY);
	for (int i = 0; i < PRODUCERS; i++)
		pthread_join(th[i], NULL);

	CHECK(c.received == (long)PRODUCERS * PER_PRODUCER);
	CHECK(g_frees == c.received);  /* every payload freed exactly once */

	/* Posting after close fails but still frees the payload. */
	rei_msgq_close(&q);
	long before = g_frees;
	CHECK(!rei_msgq_post(&q, MSG_DATA, malloc(1), counting_free));
	CHECK(g_frees == before + 1);

	rei_msgq_finish(&q);
	event_base_free(c.base);

	/* Messages discarded by close() are freed too. */
	struct event_base *b2 = event_base_new();
	struct rei_msgq q2;
	CHECK(rei_msgq_init(&q2, b2, handler, &c));
	g_frees = 0;
	for (int i = 0; i < 10; i++)
		CHECK(rei_msgq_post(&q2, MSG_DATA, malloc(1), counting_free));
	rei_msgq_finish(&q2);
	CHECK(g_frees == 10);
	event_base_free(b2);

	puts("test_msgq: ok");
	return 0;
}
