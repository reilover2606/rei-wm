#include "toolbar.h"

#include <event2/event.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "audio.h"
#include "desktop.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"

struct rei_bar_cache {
	struct rei_bar_state state;
	struct rei_pixbuf *pixbuf;
};

static struct rei_rect output_rect(const struct rei_output *o)
{
	int32_t w, h;
	rei_output_logical_size(o, &w, &h);
	return (struct rei_rect){ o->x, o->y, w, h };
}

static bool bar_enabled(const struct rei_server *s)
{
	return s->config->toolbar_enabled;
}

static struct rei_rect bar_rect(const struct rei_server *s, const struct rei_output *o)
{
	return rei_bar_rect(output_rect(o), s->config->toolbar_position, s->config->toolbar_thickness);
}

struct rei_rect rei_toolbar_usable(const struct rei_output *o)
{
	const struct rei_config *cfg = o->server->config;
	if (!cfg->toolbar_enabled)
		return output_rect(o);
	return rei_bar_usable(output_rect(o), cfg->toolbar_position, cfg->toolbar_thickness);
}

/* ------------------------------------------------------------------------ */
/* The buttons: this output's workspaces                                    */
/* ------------------------------------------------------------------------ */

static int collect_buttons(struct rei_server *s, const struct rei_output *o,
			   struct rei_bar_button *out, int cap)
{
	int n = 0;
	struct rei_workspace *ws;
	wl_list_for_each(ws, &s->workspaces, link) {  /* Already sorted by number. */
		if (ws->output != o || n == cap)
			continue;
		out[n].number = ws->number;
		if (o->workspace != ws)
			out[n].kind = REI_BAR_BUTTON_HIDDEN;
		else
			out[n].kind = s->focused_output == o ? REI_BAR_BUTTON_FOCUSED : REI_BAR_BUTTON_VISIBLE;
		n++;
	}
	return n;
}

static void build_state(struct rei_server *s, struct rei_output *o, struct rei_bar_state *st)
{
	const struct rei_config *cfg = s->config;
	memset(st, 0, sizeof *st);  /* Padding included: the state is compared with memcmp. */
	st->position = cfg->toolbar_position;
	st->thickness = cfg->toolbar_thickness;
	struct rei_rect r = output_rect(o);
	st->length = rei_bar_vertical(st->position) ? r.h : r.w;
	st->scale = (int)ceil(o->scale);
	st->n_buttons = collect_buttons(s, o, st->buttons, REI_BAR_MAX_BUTTONS);
	st->hover = -1;
	if (s->bar_hover_output == o) {
		for (int i = 0; i < st->n_buttons; i++) {
			if (st->buttons[i].number == s->bar_hover_ws)
				st->hover = i;
		}
	}
	snprintf(st->clock, sizeof st->clock, "%s", s->clock_text);
	snprintf(st->font, sizeof st->font, "%s", cfg->toolbar_font);
	struct rei_audio_state audio = rei_audio_state(s);
	rei_audio_label(&audio, st->volume, sizeof st->volume);
	/* A narrow vertical bar has no room for the word. */
	if (audio.muted && rei_bar_vertical(st->position))
		snprintf(st->volume, sizeof st->volume, "mute");
	st->volume_muted = audio.muted;
	st->background = cfg->toolbar_background;
	st->foreground = cfg->toolbar_foreground;
	st->highlight = cfg->toolbar_highlight;
}

void rei_toolbar_emit(struct rei_server *s, struct rei_output *o, struct rei_scene_snapshot *snap)
{
	if (!bar_enabled(s))
		return;

	struct rei_bar_state st;
	build_state(s, o, &st);
	if (!o->bar)
		o->bar = rei_xcalloc(1, sizeof *o->bar);
	/* Redraw only when something that shows has changed. */
	if (!o->bar->pixbuf || memcmp(&o->bar->state, &st, sizeof st) != 0) {
		struct rei_pixbuf *pb = rei_bar_render(&st);
		if (!pb)
			return;
		rei_pixbuf_unref(o->bar->pixbuf);
		o->bar->pixbuf = pb;
		o->bar->state = st;
	}

	struct rei_rect r = bar_rect(s, o);
	rei_snapshot_add(snap, o->bar->pixbuf, (float)r.x, (float)r.y, (float)r.w, (float)r.h, true);
}

void rei_toolbar_output_removed(struct rei_output *o)
{
	struct rei_server *s = o->server;
	if (s->bar_hover_output == o) {
		s->bar_hover_output = NULL;
		s->bar_hover_ws = 0;
	}
	if (o->bar) {
		rei_pixbuf_unref(o->bar->pixbuf);
		free(o->bar);
		o->bar = NULL;
	}
}

/* ------------------------------------------------------------------------ */
/* Pointer                                                                  */
/* ------------------------------------------------------------------------ */

bool rei_toolbar_hit(struct rei_server *s, double x, double y, struct rei_output **out, int *workspace)
{
	if (!bar_enabled(s))
		return false;
	struct rei_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		if (o->workspace && o->workspace->fullscreen)
			continue;  /* A fullscreen window covers the bar. */
		struct rei_rect r = bar_rect(s, o);
		if (x < r.x || y < r.y || x >= r.x + r.w || y >= r.y + r.h)
			continue;

		struct rei_bar_button buttons[REI_BAR_MAX_BUTTONS];
		int n = collect_buttons(s, o, buttons, REI_BAR_MAX_BUTTONS);
		int i = rei_bar_button_at(r, s->config->toolbar_position, n, x, y);
		*out = o;
		*workspace = i >= 0 ? buttons[i].number : 0;
		return true;
	}
	return false;
}

bool rei_toolbar_volume_at(struct rei_server *s, double x, double y)
{
	if (!bar_enabled(s))
		return false;
	struct rei_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		if (o->workspace && o->workspace->fullscreen)
			continue;
		struct rei_rect r = bar_rect(s, o);
		if (x < r.x || y < r.y || x >= r.x + r.w || y >= r.y + r.h)
			continue;
		struct rei_bar_state st;
		build_state(s, o, &st);
		struct rei_rect v;
		if (!rei_bar_volume_rect(&st, &v))
			return false;
		double lx = x - r.x, ly = y - r.y;
		return lx >= v.x && ly >= v.y && lx < v.x + v.w && ly < v.y + v.h;
	}
	return false;
}

void rei_toolbar_set_hover(struct rei_server *s, struct rei_output *o, int workspace)
{
	if (s->bar_hover_output == o && s->bar_hover_ws == workspace)
		return;
	s->bar_hover_output = o;
	s->bar_hover_ws = workspace;
	rei_server_schedule_damage(s);
}

void rei_toolbar_scroll(struct rei_server *s, struct rei_output *o, int direction)
{
	struct rei_bar_button buttons[REI_BAR_MAX_BUTTONS];
	int n = collect_buttons(s, o, buttons, REI_BAR_MAX_BUTTONS);
	int current = -1;
	for (int i = 0; i < n; i++) {
		if (buttons[i].kind != REI_BAR_BUTTON_HIDDEN)
			current = i;
	}
	int next = current + (direction > 0 ? 1 : -1);
	if (current < 0 || next < 0 || next >= n)
		return;  /* Like i3: stop at the ends rather than wrap around. */
	rei_wm_goto_workspace(s, buttons[next].number);
}

/* ------------------------------------------------------------------------ */
/* The clock                                                                */
/* ------------------------------------------------------------------------ */

static void format_time(const char *fmt, time_t t, char *out, size_t n)
{
	struct tm tm;
	localtime_r(&t, &tm);
	if (strftime(out, n, fmt, &tm) == 0)
		out[0] = '\0';
}

/*
 * Arms the timer for the moment the clock's text next changes, which depends on
 * the format: every second for "%H:%M:%S", at each minute boundary for "%H:%M".
 * Found by formatting successive seconds until the text differs, so any format
 * works without being parsed.
 */
static void arm_clock(struct rei_server *s)
{
	struct timeval now;
	gettimeofday(&now, NULL);
	char base[64], next[64];
	format_time(s->config->toolbar_clock_format, now.tv_sec, base, sizeof base);

	int ahead = 60;  /* If nothing changes within a minute, look again then. */
	for (int k = 1; k <= 60; k++) {
		format_time(s->config->toolbar_clock_format, now.tv_sec + k, next, sizeof next);
		if (strcmp(base, next) != 0) {
			ahead = k;
			break;
		}
	}

	/* Time until second `now.tv_sec + ahead` begins, plus a few ms of slack so
	 * the formatted time has certainly rolled over when we look. */
	long wait_us = (long)ahead * 1000000L - now.tv_usec + 5000;
	struct timeval tv = { wait_us / 1000000L, wait_us % 1000000L };
	evtimer_add(s->clock_event, &tv);
}

static void update_clock(struct rei_server *s)
{
	char text[64];
	format_time(s->config->toolbar_clock_format, time(NULL), text, sizeof text);
	if (strcmp(text, s->clock_text) != 0) {
		snprintf(s->clock_text, sizeof s->clock_text, "%s", text);
		rei_server_schedule_damage(s);
	}
}

static void on_clock(evutil_socket_t fd, short what, void *arg)
{
	(void)fd;
	(void)what;
	struct rei_server *s = arg;
	update_clock(s);
	arm_clock(s);
}

void rei_toolbar_init(struct rei_server *s)
{
	if (!bar_enabled(s) || !s->config->toolbar_clock_format[0])
		return;
	s->clock_event = evtimer_new(s->base, on_clock, s);
	update_clock(s);
	arm_clock(s);
}

void rei_toolbar_finish(struct rei_server *s)
{
	if (s->clock_event) {
		event_free(s->clock_event);
		s->clock_event = NULL;
	}
	rei_bar_render_cleanup();
}
