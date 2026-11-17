#include "idle.h"

#include <event2/event.h>
#include <stdlib.h>

#include "backend/backend.h"
#include "config.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"

struct rei_idle {
	struct rei_server *server;
	struct event *timer;
	uint64_t last_ns;   /* the last input (or inhibitor release) */
	bool asleep;
	int inhibitors;
};

static uint64_t timeout_ms(const struct rei_server *s)
{
	const char *env = getenv("REI_DISPLAY_SLEEP_MS");
	if (env && env[0])
		return (uint64_t)strtoull(env, NULL, 10);
	return (uint64_t)s->config->display_sleep_minutes * 60000ull;
}

static void arm(struct rei_idle *i, uint64_t ms)
{
	struct timeval tv = { (time_t)(ms / 1000), (suseconds_t)(ms % 1000) * 1000 };
	evtimer_add(i->timer, &tv);
}

static void on_timer(evutil_socket_t fd, short what, void *arg)
{
	(void)fd;
	(void)what;
	struct rei_idle *i = arg;
	struct rei_server *s = i->server;
	uint64_t limit = timeout_ms(s);
	if (limit == 0 || i->asleep)
		return;
	uint64_t idle_ms = (rei_now_ns() - i->last_ns) / 1000000ull;
	if (idle_ms < limit) {          /* there was input meanwhile: wait out the rest */
		arm(i, limit - idle_ms);
		return;
	}
	if (i->inhibitors > 0) {        /* something is being watched: check again later */
		arm(i, limit);
		return;
	}
	i->asleep = true;
	rei_info("display: sleeping after %llu ms without input", (unsigned long long)idle_ms);
	rei_backend_set_display_power(s, false);
}

void rei_idle_init(struct rei_server *s)
{
	struct rei_idle *i = rei_xcalloc(1, sizeof *i);
	i->server = s;
	i->last_ns = rei_now_ns();
	i->timer = evtimer_new(s->base, on_timer, i);
	s->idle = i;
	rei_idle_configure(s);
}

void rei_idle_configure(struct rei_server *s)
{
	struct rei_idle *i = s->idle;
	if (!i)
		return;
	evtimer_del(i->timer);
	uint64_t limit = timeout_ms(s);
	if (limit == 0) {
		if (i->asleep) {
			i->asleep = false;
			rei_info("display: awake");
			rei_backend_set_display_power(s, true);
		}
		return;
	}
	i->last_ns = rei_now_ns();
	arm(i, limit);
}

void rei_idle_finish(struct rei_server *s)
{
	struct rei_idle *i = s->idle;
	if (!i)
		return;
	s->idle = NULL;
	if (i->timer)
		event_free(i->timer);
	free(i);
}

bool rei_idle_activity(struct rei_server *s, bool wake_only)
{
	struct rei_idle *i = s->idle;
	if (!i)
		return false;
	i->last_ns = rei_now_ns();
	if (!i->asleep)
		return false;
	i->asleep = false;
	rei_info("display: awake");
	rei_backend_set_display_power(s, true);
	arm(i, timeout_ms(s) ? timeout_ms(s) : 1000);
	return wake_only;
}

void rei_idle_inhibit_add(struct rei_server *s, int delta)
{
	struct rei_idle *i = s->idle;
	if (!i)
		return;
	i->inhibitors += delta;
	if (i->inhibitors < 0)
		i->inhibitors = 0;
	if (delta < 0 && i->inhibitors == 0)
		i->last_ns = rei_now_ns();   /* a fresh interval starts when the last one lets go */
	rei_info("display: %d idle inhibitor%s", i->inhibitors, i->inhibitors == 1 ? "" : "s");
}

bool rei_idle_asleep(const struct rei_server *s)
{
	return s->idle && s->idle->asleep;
}
