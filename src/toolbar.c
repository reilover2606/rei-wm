#include "toolbar.h"

#include <event2/event.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "audio.h"
#include "battery.h"
#include "desktop.h"
#include "icons.h"
#include "popup.h"
#include "runprompt.h"
#include "startmenu.h"
#include "tray.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"

struct rei_bar_cache {
	struct rei_bar_state state;
	struct rei_pixbuf *pixbuf;
	struct rei_tray_layout tray;  /* Where the tiles are in the picture above (for hit-testing). */
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

static void build_state_for(struct rei_server *s, struct rei_output *o, const struct rei_workspace *only,
			    struct rei_bar_state *st)
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
	if (cfg->start_enabled) {
		st->start_w = rei_bar_start_extent(st->position, st->thickness);
		snprintf(st->start_label, sizeof st->start_label, "%s", cfg->start_label);
		st->start_open = rei_popup_is_for(s, "start");
		st->start_hover = s->start_hover;
	}
	struct rei_battery_info bat = rei_battery_state(s);
	st->battery_present = bat.present;
	st->battery_percent = bat.percent;
	st->battery_charging = bat.charging;
	st->tray_mode = cfg->toolbar_tray;
	st->tray_min_percent = cfg->toolbar_tray_min_width;
	st->tray_hover = -1;
	st->drag_index = -1;
	if (st->tray_mode != REI_TRAY_OFF) {
		struct rei_tray_set set;
		rei_tray_collect(s, o, only, &set);
		for (size_t i = 0; i < set.n_groups; i++) {
			struct rei_bar_tray_item *it = &st->tray[st->n_tray++];
			snprintf(it->label, sizeof it->label, "%s", set.groups[i].label);
			it->count = set.groups[i].count;
			it->active = set.groups[i].active;
			it->minimized = set.groups[i].minimized;
			const char *title = set.groups[i].n_views ? rei_view_title(set.groups[i].views[0]) : "";
			it->icon = rei_icon_for_app(set.groups[i].app_id, title);
		}
		st->tray_extra_windows = set.extra_windows;
		if (!only) {  /* Hover and drag belong to what is actually on screen. */
			st->tray_hover = rei_tray_hover_index(s, o, &set);
			int pos = 0;
			st->drag_index = rei_tray_drag_index(s, o, &set, &pos);
			st->drag_pos = pos;
		}
	}
	st->styled = true;
	st->theme = cfg->theme;
	st->background = cfg->toolbar_background;
	st->foreground = cfg->toolbar_foreground;
	st->highlight = cfg->toolbar_highlight;
}

static bool cfg_translucent(const struct rei_server *s)
{
	return s->config->theme.e[REI_ELEM_TOOLBAR].opacity < 0.999f;
}

static void build_state(struct rei_server *s, struct rei_output *o, struct rei_bar_state *st)
{
	build_state_for(s, o, NULL, st);
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
		rei_bar_tray_layout(&st, &o->bar->tray);
		if (st.n_tray) {  /* Where the tiles are: read by the tests, handy when debugging. */
			const struct rei_tray_layout *lay = &o->bar->tray;
			struct rei_rect bar = bar_rect(s, o);
			char line[1400];
			size_t used = (size_t)snprintf(line, sizeof line, "tray %s:", o->name);
			for (size_t i = 0; i < lay->n_visible && used < sizeof line - 200; i++)
				used += (size_t)snprintf(line + used, sizeof line - used,
							 " [%d %d,%d %dx%d count=%d active=%d min=%d '%s']", (int)i,
							 bar.x + lay->tile[i].x, bar.y + lay->tile[i].y, lay->tile[i].w,
							 lay->tile[i].h, st.tray[i].count, st.tray[i].active, st.tray[i].minimized,
							 st.tray[i].label);
			if (lay->has_overflow)
				used += (size_t)snprintf(line + used, sizeof line - used, " {overflow %d,%d %dx%d windows=%d}",
							 bar.x + lay->overflow.x, bar.y + lay->overflow.y, lay->overflow.w,
							 lay->overflow.h, lay->overflow_windows);
			rei_debug("%s", line);
		}
	}

	struct rei_rect r = bar_rect(s, o);
	rei_snapshot_add(snap, o->bar->pixbuf, (float)r.x, (float)r.y, (float)r.w, (float)r.h,
			 !(cfg_translucent(s)));
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
		struct rei_rect rb = r;  /* The buttons start after the Start button. */
		int sw = s->config->start_enabled ? rei_bar_start_extent(s->config->toolbar_position,
									   s->config->toolbar_thickness) : 0;
		if (rei_bar_vertical(s->config->toolbar_position)) {
			rb.y += sw;
			rb.h -= sw;
		} else {
			rb.x += sw;
			rb.w -= sw;
		}
		int i = rei_bar_button_at(rb, s->config->toolbar_position, n, x, y);
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

struct rei_output *rei_toolbar_output_at(struct rei_server *s, double x, double y)
{
	if (!bar_enabled(s))
		return NULL;
	struct rei_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		struct rei_rect r = bar_rect(s, o);
		if (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h)
			return o;
	}
	return NULL;
}

bool rei_toolbar_start_at(struct rei_server *s, double x, double y)
{
	if (!s->config->start_enabled)
		return false;
	struct rei_output *o = rei_toolbar_output_at(s, x, y);
	if (!o || (o->workspace && o->workspace->fullscreen))
		return false;
	struct rei_rect r = bar_rect(s, o);
	int sw = rei_bar_start_extent(s->config->toolbar_position, s->config->toolbar_thickness);
	double along = rei_bar_vertical(s->config->toolbar_position) ? y - r.y : x - r.x;
	return along < sw;
}

void rei_toolbar_start_click(struct rei_server *s, struct rei_output *o)
{
	if (rei_popup_is_for(s, "start")) {
		rei_popup_close(s);
		return;
	}
	struct rei_rect bar = bar_rect(s, o);
	int sw = rei_bar_start_extent(s->config->toolbar_position, s->config->toolbar_thickness);
	struct rei_rect anchor = rei_bar_vertical(s->config->toolbar_position)
		? (struct rei_rect){ bar.x, bar.y, bar.w, sw } : (struct rei_rect){ bar.x, bar.y, sw, bar.h };
	rei_startmenu_open(s, o, bar, s->config->toolbar_position, anchor);
}

struct rei_rect rei_toolbar_rect(const struct rei_server *s, const struct rei_output *o)
{
	return bar_rect(s, o);
}

/* The bar state and tray layout that were last drawn for an output, else fresh ones. */
static void current_layout(struct rei_server *s, struct rei_output *o, struct rei_bar_state *st,
			   struct rei_tray_layout *lay)
{
	if (o->bar && o->bar->pixbuf) {
		*st = o->bar->state;
		*lay = o->bar->tray;
		return;
	}
	build_state(s, o, st);
	rei_bar_tray_layout(st, lay);
}

bool rei_toolbar_tray_target(struct rei_server *s, double x, double y, struct rei_output **out, int *kind,
			     int *index, struct rei_rect *tile)
{
	if (!bar_enabled(s) || s->config->toolbar_tray == REI_TRAY_OFF)
		return false;
	struct rei_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		if (o->workspace && o->workspace->fullscreen)
			continue;
		struct rei_rect r = bar_rect(s, o);
		if (x < r.x || y < r.y || x >= r.x + r.w || y >= r.y + r.h)
			continue;
		struct rei_bar_state st;
		struct rei_tray_layout lay;
		current_layout(s, o, &st, &lay);
		double lx = x - r.x, ly = y - r.y;
		for (size_t i = 0; i < lay.n_visible; i++) {
			const struct rei_rect *t = &lay.tile[i];
			if (lx >= t->x && ly >= t->y && lx < t->x + t->w && ly < t->y + t->h) {
				*out = o;
				*kind = REI_TRAY_TARGET_TILE;
				*index = (int)i;
				*tile = (struct rei_rect){ r.x + t->x, r.y + t->y, t->w, t->h };
				return true;
			}
		}
		if (lay.has_overflow) {
			const struct rei_rect *t = &lay.overflow;
			if (lx >= t->x && ly >= t->y && lx < t->x + t->w && ly < t->y + t->h) {
				*out = o;
				*kind = REI_TRAY_TARGET_OVERFLOW;
				*index = -1;
				*tile = (struct rei_rect){ r.x + t->x, r.y + t->y, t->w, t->h };
				return true;
			}
		}
		return false;
	}
	return false;
}

bool rei_toolbar_tray_at(struct rei_server *s, double x, double y, struct rei_output **out, int *index)
{
	int kind;
	struct rei_rect tile;
	if (!rei_toolbar_tray_target(s, x, y, out, &kind, index, &tile))
		return false;
	if (kind == REI_TRAY_TARGET_OVERFLOW)
		*index = REI_TRAY_HOVER_OVERFLOW;
	return true;
}

size_t rei_toolbar_tray_visible(struct rei_server *s, struct rei_output *o, const struct rei_tray_set *set)
{
	(void)set;
	struct rei_bar_state st;
	struct rei_tray_layout lay;
	current_layout(s, o, &st, &lay);
	return lay.n_visible;
}

int rei_toolbar_tray_drop_index(struct rei_server *s, struct rei_output *o, size_t *visible)
{
	struct rei_bar_state st;
	build_state(s, o, &st);
	struct rei_tray_layout lay;
	rei_bar_tray_layout(&st, &lay);
	*visible = lay.n_visible;
	return lay.drop_index;
}

int rei_toolbar_overflow_windows(struct rei_server *s, struct rei_output *o, const struct rei_workspace *ws)
{
	if (!bar_enabled(s) || s->config->toolbar_tray == REI_TRAY_OFF)
		return 0;
	struct rei_bar_state st;
	build_state_for(s, o, ws, &st);
	struct rei_tray_layout lay;
	rei_bar_tray_layout(&st, &lay);
	return lay.has_overflow ? lay.overflow_windows : 0;
}

void rei_toolbar_tray_click(struct rei_server *s, struct rei_output *o, int index)
{
	struct rei_tray_set set;
	rei_tray_collect(s, o, NULL, &set);
	struct rei_bar_state st;
	struct rei_tray_layout lay;
	current_layout(s, o, &st, &lay);
	struct rei_rect bar = bar_rect(s, o);

	if (index == REI_TRAY_HOVER_OVERFLOW) {
		if (!lay.has_overflow)
			return;
		if (rei_tray_menu_is_for(s, REI_TRAY_OVERFLOW_KEY)) {
			rei_tray_menu_close(s);
			return;
		}
		struct rei_view *all[REI_TRAY_MAX_VIEWS];
		int n = 0;
		for (size_t i = lay.n_visible; i < set.n_groups; i++)
			for (int k = 0; k < set.groups[i].n_views && n < REI_TRAY_MAX_VIEWS; k++)
				all[n++] = set.groups[i].views[k];
		for (int k = 0; k < set.n_extra_views && n < REI_TRAY_MAX_VIEWS; k++)
			all[n++] = set.extra_views[k];
		struct rei_rect tile = { bar.x + lay.overflow.x, bar.y + lay.overflow.y, lay.overflow.w, lay.overflow.h };
		rei_tray_menu_open(s, o, REI_TRAY_OVERFLOW_KEY, all, n, bar, s->config->toolbar_position, tile);
		return;
	}
	if (index < 0 || (size_t)index >= set.n_groups || (size_t)index >= lay.n_visible)
		return;
	struct rei_tray_group *g = &set.groups[index];

	if (g->count == 1 && g->n_views == 1) {
		rei_tray_menu_close(s);
		struct rei_view *v = g->views[0];
		/* Like other desktops: the focused window's tile hides it; any other tile brings it. */
		if (!v->minimized && g->active && rei_view_visible(v))
			rei_wm_minimize_view(v);
		else
			rei_wm_activate_view(v);
		return;
	}

	if (rei_tray_menu_is_for(s, g->app_id)) {
		rei_tray_menu_close(s);
		return;
	}
	struct rei_rect tile = { bar.x + lay.tile[index].x, bar.y + lay.tile[index].y, lay.tile[index].w,
				 lay.tile[index].h };
	rei_tray_menu_open(s, o, g->app_id, g->views, g->n_views, bar, s->config->toolbar_position, tile);
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
	rei_prompt_finish(s);
	rei_startmenu_finish(s);
	rei_popup_finish(s);
	rei_tray_finish(s);
	rei_bar_render_cleanup();
}
