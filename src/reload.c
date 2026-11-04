#include "reload.h"

#include <errno.h>
#include <event2/event.h>
#include <libgen.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "backend/backend.h"
#include "config.h"
#include "desktop.h"
#include "input/input.h"
#include "proto/seat.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"

static bool outputs_differ(const struct rei_config *a, const struct rei_config *b)
{
	if (a->n_outputs != b->n_outputs)
		return true;
	for (size_t i = 0; i < a->n_outputs; i++) {
		const struct rei_output_config *x = &a->outputs[i], *y = &b->outputs[i];
		if (strcmp(x->name ? x->name : "", y->name ? y->name : "") || x->width != y->width ||
		    x->height != y->height || x->refresh_mhz != y->refresh_mhz || x->scale != y->scale ||
		    x->has_position != y->has_position || x->x != y->x || x->y != y->y ||
		    x->enabled != y->enabled || x->has_mode != y->has_mode)
			return true;
	}
	return false;
}

bool rei_reload_config(struct rei_server *s, char *err, size_t errlen)
{
	const struct rei_config *old = s->config;
	if (!old->path) {
		snprintf(err, errlen, "the running config did not come from a file");
		return false;
	}

	struct rei_config *cfg = rei_config_load(old->path, false, err, errlen);
	if (!cfg)
		return false;

	struct rei_input_reload in;
	if (!rei_input_prepare_reload(s, cfg, &in, err, errlen)) {
		rei_config_free(cfg);
		return false;
	}

	if (strcmp(old->backend, cfg->backend))
		rei_warn("config: [general] backend changed; restart rei-wm to use it");
	if (outputs_differ(old, cfg))
		rei_warn("config: [[output]] changed; restart rei-wm to apply it");
	bool look_changed = strcmp(old->wallpaper_mode, cfg->wallpaper_mode) ||
			    strcmp(old->wallpaper_path ? old->wallpaper_path : "",
				   cfg->wallpaper_path ? cfg->wallpaper_path : "") ||
			    old->background_rgb != cfg->background_rgb;

	/* Commit: from here on nothing can fail. */
	s->old_configs = rei_xrealloc(s->old_configs, (s->n_old_configs + 1) * sizeof *s->old_configs);
	s->old_configs[s->n_old_configs++] = cfg;  /* Freed at exit with the others. */
	s->config = cfg;
	s->float_new_set = false;   /* The file's new_windows applies again. */
	rei_input_commit_reload(s, &in);

	rei_seat_keyboards_reconfigured(s->seat, in.keymap_changed);
	if (s->backend && s->backend->impl->config_changed)
		s->backend->impl->config_changed(s->backend);
	if (look_changed)
		rei_server_reload_look(s);
	rei_wm_arrange_all(s);  /* Gaps, notch height and toolbar size all move windows. */
	rei_server_schedule_damage(s);
	rei_server_schedule_flush(s);
	rei_info("config reloaded from %s", cfg->path ? cfg->path : "?");
	return true;
}

static void do_reload(struct rei_server *s)
{
	char err[512];
	if (!rei_reload_config(s, err, sizeof err))
		rei_error("config not reloaded (the previous settings stay in effect): %s", err);
}

static void on_reload_timer(evutil_socket_t fd, short what, void *arg)
{
	(void)fd;
	(void)what;
	do_reload(arg);
}

static void on_sighup(evutil_socket_t sig, short what, void *arg)
{
	(void)sig;
	(void)what;
	rei_info("SIGHUP: reloading the config");
	do_reload(arg);
}

static void on_inotify(evutil_socket_t fd, short what, void *arg)
{
	(void)what;
	struct rei_server *s = arg;
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t n = read(fd, buf, sizeof buf);
	if (n <= 0)
		return;

	char *base = rei_xstrdup(s->config->path);
	const char *want = basename(base);
	bool hit = false;
	for (char *p = buf; p < buf + n;) {
		const struct inotify_event *e = (const struct inotify_event *)p;
		if (e->len && strcmp(e->name, want) == 0)
			hit = true;
		p += sizeof *e + e->len;
	}
	free(base);
	if (hit) {
		/* A save is often several writes and a rename: wait for it to settle. */
		struct timeval tv = { .tv_sec = 0, .tv_usec = 150000 };
		evtimer_add(s->reload_timer, &tv);
	}
}

void rei_reload_start(struct rei_server *s)
{
	s->inotify_fd = -1;
	s->sighup_event = evsignal_new(s->base, SIGHUP, on_sighup, s);
	if (s->sighup_event)
		event_add(s->sighup_event, NULL);
	s->reload_timer = evtimer_new(s->base, on_reload_timer, s);

	if (!s->config->path)
		return;  /* Defaults only: nothing to watch. */
	char *dir_copy = rei_xstrdup(s->config->path);
	const char *dir = dirname(dir_copy);
	s->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (s->inotify_fd >= 0 &&
	    inotify_add_watch(s->inotify_fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE) >= 0) {
		s->inotify_event = event_new(s->base, s->inotify_fd, EV_READ | EV_PERSIST, on_inotify, s);
		event_add(s->inotify_event, NULL);
		rei_info("watching %s for changes", s->config->path);
	} else {
		rei_warn("cannot watch the config for changes (%s); send SIGHUP to reload", strerror(errno));
		if (s->inotify_fd >= 0)
			close(s->inotify_fd);
		s->inotify_fd = -1;
	}
	free(dir_copy);
}

void rei_reload_stop(struct rei_server *s)
{
	if (s->inotify_event)
		event_free(s->inotify_event);
	if (s->reload_timer)
		event_free(s->reload_timer);
	if (s->sighup_event)
		event_free(s->sighup_event);
	s->inotify_event = s->reload_timer = s->sighup_event = NULL;
	if (s->inotify_fd >= 0)
		close(s->inotify_fd);
	s->inotify_fd = -1;
	for (size_t i = 0; i < s->n_old_configs; i++)
		rei_config_free(s->old_configs[i]);
	free(s->old_configs);
	s->old_configs = NULL;
	s->n_old_configs = 0;
}
