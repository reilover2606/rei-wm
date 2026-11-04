#include "tray.h"

#include <event2/event.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "desktop.h"
#include "icons.h"
#include "confirm.h"
#include "popup.h"
#include "proto/xdg_shell.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"
#include "xwayland.h"

#define HOVER_DELAY_MS_DEFAULT 500   /* The tooltip appears this long after the pointer touches a tile... */
#define FADE_MS_DEFAULT 160          /* ...and fades in over this long. */
/* Longer fades make the animation observable in tests: REI_TRAY_FADE_MS overrides it. */
static int fade_ms(void)
{
	const char *e = getenv("REI_TRAY_FADE_MS");
	return e && atoi(e) > 0 ? atoi(e) : FADE_MS_DEFAULT;
}
#define HOVER_DELAY_MS HOVER_DELAY_MS_DEFAULT
#define FADE_MS fade_ms()
#define TICK_MS 16
#define DRAG_THRESHOLD 6.0
#define MAX_KNOWN_APPS 256
#define TEMP_GROUPS 64

const char *rei_view_title(const struct rei_view *v)
{
	const char *t = v->xwin ? rei_xwindow_title(v->xwin) : v->toplevel->title;
	return t ? t : "";
}

const char *rei_view_app_id(const struct rei_view *v)
{
	const char *a = v->xwin ? rei_xwindow_class(v->xwin) : v->toplevel->app_id;
	return a ? a : "";
}

/* ------------------------------------------------------------------------ */
/* State                                                                    */
/* ------------------------------------------------------------------------ */

struct rei_tray_ui {
	struct rei_server *server;

	/* The user's order of applications (by app id); new ones are appended. */
	char **order;
	size_t n_order;

	/* The window list (shown by the generic popup; these are what its rows stand for). */
	struct rei_view *menu_views[REI_TRAY_MAX_VIEWS];
	int menu_n;

	/* Hover, and the tooltip it earns. */
	bool hover_valid;
	struct rei_output *hover_output;
	char hover_key[96];
	struct rei_rect hover_tile;  /* Global. */
	struct event *hover_timer, *tick_timer;
	struct rei_pixbuf *tip;
	struct rei_rect tip_rect;    /* The picture, global logical. */
	uint64_t tip_start_ns;
	bool tip_on;

	/* A press on a tile, which becomes a drag once the pointer moves. */
	bool press, drag;
	struct rei_output *press_output;
	char press_key[96];
	double press_x, press_y, drag_pos;
};

static void on_hover_timer(evutil_socket_t fd, short what, void *arg);
static void on_tick(evutil_socket_t fd, short what, void *arg);

static struct rei_tray_ui *ui_of(struct rei_server *s)
{
	if (!s->tray_ui) {
		struct rei_tray_ui *u = rei_xcalloc(1, sizeof *u);
		u->server = s;
		u->hover_timer = evtimer_new(s->base, on_hover_timer, u);
		u->tick_timer = evtimer_new(s->base, on_tick, u);
		s->tray_ui = u;
	}
	return s->tray_ui;
}

/* ------------------------------------------------------------------------ */
/* Grouping and order                                                       */
/* ------------------------------------------------------------------------ */

static int order_index(const struct rei_tray_ui *u, const char *id)
{
	for (size_t i = 0; i < u->n_order; i++)
		if (strcmp(u->order[i], id) == 0)
			return (int)i;
	return -1;
}

static void order_append(struct rei_tray_ui *u, const char *id)
{
	if (u->n_order == MAX_KNOWN_APPS) {  /* Forget the oldest. */
		free(u->order[0]);
		memmove(u->order, u->order + 1, (u->n_order - 1) * sizeof *u->order);
		u->n_order--;
	}
	u->order = rei_xrealloc(u->order, (u->n_order + 1) * sizeof *u->order);
	u->order[u->n_order++] = rei_xstrdup(id);
}

void rei_tray_collect(struct rei_server *s, const struct rei_output *o, const struct rei_workspace *only,
		      struct rei_tray_set *out)
{
	struct rei_tray_ui *u = ui_of(s);
	memset(out, 0, sizeof *out);
	if (!only && s->config->toolbar_tray_scope == REI_TRAY_SCOPE_WORKSPACE)
		only = o->workspace;

	struct rei_tray_group *tmp = rei_xcalloc(TEMP_GROUPS, sizeof *tmp);
	size_t n = 0;
	int lost = 0;
	const struct rei_view *active = rei_wm_active_view(s);
	struct rei_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (!v->mapped || !v->ws || v->ws->output != o || (only && v->ws != only))
			continue;
		const char *app = rei_view_app_id(v);
		const char *title = rei_view_title(v);
		const char *key = app[0] ? app : title;  /* Without an app id, the title stands in. */

		struct rei_tray_group *g = NULL;
		for (size_t i = 0; i < n; i++)
			if (strcmp(tmp[i].app_id, key) == 0) {
				g = &tmp[i];
				break;
			}
		if (!g) {
			if (n == TEMP_GROUPS) {
				lost++;
				continue;
			}
			g = &tmp[n++];
			snprintf(g->app_id, sizeof g->app_id, "%s", key);
			g->minimized = true;
			if (order_index(u, key) < 0)
				order_append(u, key);
		}
		g->count++;
		if (g->n_views < REI_TRAY_MAX_VIEWS)
			g->views[g->n_views++] = v;
		g->active = g->active || v == active;
		g->minimized = g->minimized && v->minimized;
	}

	/* The user's order (insertion sort: stable, and n is small). */
	for (size_t i = 1; i < n; i++) {
		struct rei_tray_group g = tmp[i];
		int gi = order_index(u, g.app_id);
		size_t j = i;
		while (j > 0 && order_index(u, tmp[j - 1].app_id) > gi) {
			tmp[j] = tmp[j - 1];
			j--;
		}
		tmp[j] = g;
	}

	for (size_t i = 0; i < n; i++) {
		struct rei_tray_group *g = &tmp[i];
		if (i >= REI_BAR_MAX_TRAY) {  /* Past the list's capacity: only the overflow tile knows them. */
			out->extra_windows += g->count;
			for (int k = 0; k < g->n_views && out->n_extra_views < REI_TRAY_MAX_VIEWS; k++)
				out->extra_views[out->n_extra_views++] = g->views[k];
			continue;
		}
		const char *title = g->n_views ? rei_view_title(g->views[0]) : "";
		if (g->count == 1) {
			snprintf(g->label, sizeof g->label, "%s", title[0] ? title : g->app_id);
		} else {
			const char *name = rei_icon_app_name(g->app_id);
			snprintf(g->label, sizeof g->label, "%s (%d)", name ? name : g->app_id, g->count);
		}
		out->groups[out->n_groups++] = *g;
	}
	out->extra_windows += lost;
	free(tmp);
}

/* ------------------------------------------------------------------------ */
/* Tooltip                                                                  */
/* ------------------------------------------------------------------------ */

static const struct rei_tray_group *find_group(const struct rei_tray_set *set, const char *key)
{
	for (size_t i = 0; i < set->n_groups; i++)
		if (strcmp(set->groups[i].app_id, key) == 0)
			return &set->groups[i];
	return NULL;
}

static void tip_hide(struct rei_tray_ui *u)
{
	evtimer_del(u->hover_timer);
	evtimer_del(u->tick_timer);
	if (u->tip_on || u->tip) {
		rei_pixbuf_unref(u->tip);
		u->tip = NULL;
		u->tip_on = false;
		rei_server_schedule_damage(u->server);
	}
}

static void append_titles(char *text, size_t cap, struct rei_view *const *views, int n, int shown)
{
	for (int i = 0; i < n && i < shown; i++) {
		const char *t = rei_view_title(views[i]);
		size_t used = strlen(text);
		snprintf(text + used, cap - used, "\n  %.100s%s", t[0] ? t : rei_view_app_id(views[i]),
			 views[i]->minimized ? "  (minimized)" : "");
	}
	if (n > shown) {
		size_t used = strlen(text);
		snprintf(text + used, cap - used, "\n  … and %d more", n - shown);
	}
}

static void on_hover_timer(evutil_socket_t fd, short what, void *arg)
{
	(void)fd; (void)what;
	struct rei_tray_ui *u = arg;
	struct rei_server *s = u->server;
	if (!u->hover_valid || u->drag || u->press || rei_popup_is_open(s))
		return;
	struct rei_output *o = u->hover_output;

	struct rei_tray_set set;
	rei_tray_collect(s, o, NULL, &set);
	char text[1024];
	if (strcmp(u->hover_key, REI_TRAY_OVERFLOW_KEY) == 0) {
		/* Everything not shown as a tile: the windows of the groups past the visible ones. */
		struct rei_view *all[REI_TRAY_MAX_VIEWS * 2];
		int n = 0;
		size_t first = rei_toolbar_tray_visible(s, o, &set);
		for (size_t i = first; i < set.n_groups; i++)
			for (int k = 0; k < set.groups[i].n_views && n < (int)REI_ARRAY_LEN(all); k++)
				all[n++] = set.groups[i].views[k];
		for (int k = 0; k < set.n_extra_views && n < (int)REI_ARRAY_LEN(all); k++)
			all[n++] = set.extra_views[k];
		snprintf(text, sizeof text, "%d more windows", n);
		append_titles(text, sizeof text, all, n, 10);
	} else {
		const struct rei_tray_group *g = find_group(&set, u->hover_key);
		if (!g)
			return;
		if (g->count == 1 && g->n_views == 1) {
			const char *t = rei_view_title(g->views[0]);
			snprintf(text, sizeof text, "%.200s%s", t[0] ? t : g->app_id,
				 g->views[0]->minimized ? "  (minimized)" : "");
		} else {
			const char *name = rei_icon_app_name(g->app_id);
			snprintf(text, sizeof text, "%s: %d windows", name ? name : g->app_id, g->count);
			append_titles(text, sizeof text, g->views, g->n_views, 10);
		}
	}

	rei_debug("tray tooltip: %.*s", (int)strcspn(text, "\n"), text);
	const struct rei_config *cfg = s->config;
	int bw, bh, m;
	rei_pixbuf_unref(u->tip);
	u->tip = rei_bar_tooltip_render(text, cfg->toolbar_font, &cfg->theme.e[REI_ELEM_TOOLTIP],
					(int)ceil(o->scale), &bw, &bh, &m);
	if (!u->tip)
		return;

	/* Beside the tile, away from the screen edge the bar sits on, kept on the screen. */
	struct rei_rect bar = rei_toolbar_rect(s, o), t = u->hover_tile;
	struct rei_rect box = { t.x + t.w / 2 - bw / 2, t.y, bw, bh };
	switch (cfg->toolbar_position) {
	case REI_BAR_TOP: box.y = bar.y + bar.h + 4; break;
	case REI_BAR_BOTTOM: box.y = bar.y - bh - 4; break;
	case REI_BAR_LEFT: box.x = bar.x + bar.w + 4; box.y = t.y + t.h / 2 - bh / 2; break;
	case REI_BAR_RIGHT: box.x = bar.x - bw - 4; box.y = t.y + t.h / 2 - bh / 2; break;
	}
	int32_t ow, oh;
	rei_output_logical_size(o, &ow, &oh);
	if (box.x + box.w > o->x + ow - 2) box.x = o->x + ow - 2 - box.w;
	if (box.y + box.h > o->y + oh - 2) box.y = o->y + oh - 2 - box.h;
	if (box.x < o->x + 2) box.x = o->x + 2;
	if (box.y < o->y + 2) box.y = o->y + 2;
	u->tip_rect = (struct rei_rect){ box.x - m, box.y - m, bw + 2 * m, bh + 2 * m };
	u->tip_start_ns = rei_now_ns();
	u->tip_on = true;

	struct timeval tv = { 0, TICK_MS * 1000 };
	evtimer_add(u->tick_timer, &tv);
	rei_server_schedule_damage(s);
}

static void on_tick(evutil_socket_t fd, short what, void *arg)
{
	(void)fd; (void)what;
	struct rei_tray_ui *u = arg;
	if (!u->tip_on)
		return;
	rei_server_schedule_damage(u->server);
	if (rei_now_ns() - u->tip_start_ns < (uint64_t)FADE_MS * 1000000ull + 40000000ull) {
		struct timeval tv = { 0, TICK_MS * 1000 };
		evtimer_add(u->tick_timer, &tv);
	}
}

/* ------------------------------------------------------------------------ */
/* Hover                                                                    */
/* ------------------------------------------------------------------------ */

void rei_tray_hover_clear(struct rei_server *s)
{
	struct rei_tray_ui *u = s->tray_ui;
	if (!u)
		return;
	if (u->hover_valid) {
		u->hover_valid = false;
		rei_server_schedule_damage(s);
	}
	tip_hide(u);
}

int rei_tray_hover_index(const struct rei_server *s, const struct rei_output *o, const struct rei_tray_set *set)
{
	const struct rei_tray_ui *u = s->tray_ui;
	if (!u || !u->hover_valid || u->hover_output != o)
		return -1;
	if (strcmp(u->hover_key, REI_TRAY_OVERFLOW_KEY) == 0)
		return REI_TRAY_HOVER_OVERFLOW;
	for (size_t i = 0; i < set->n_groups; i++)
		if (strcmp(set->groups[i].app_id, u->hover_key) == 0)
			return (int)i;
	return -1;
}

static void update_hover(struct rei_tray_ui *u, double x, double y)
{
	struct rei_server *s = u->server;
	struct rei_output *o = NULL;
	int kind = 0, index = -1;
	struct rei_rect tile = { 0 };
	bool over = rei_toolbar_tray_target(s, x, y, &o, &kind, &index, &tile);

	char key[96] = "";
	if (over) {
		if (kind == REI_TRAY_TARGET_OVERFLOW) {
			snprintf(key, sizeof key, "%s", REI_TRAY_OVERFLOW_KEY);
		} else {
			struct rei_tray_set set;
			rei_tray_collect(s, o, NULL, &set);
			if (index >= 0 && (size_t)index < set.n_groups)
				snprintf(key, sizeof key, "%s", set.groups[index].app_id);
			else
				over = false;
		}
	}

	if (!over) {
		rei_tray_hover_clear(s);
		return;
	}
	u->hover_tile = tile;
	if (u->hover_valid && u->hover_output == o && strcmp(u->hover_key, key) == 0)
		return;  /* Still on the same tile. */

	/* A new tile: highlight it now, and start the tooltip's half-second. */
	tip_hide(u);
	u->hover_valid = true;
	u->hover_output = o;
	snprintf(u->hover_key, sizeof u->hover_key, "%s", key);
	rei_server_schedule_damage(s);
	if (!rei_popup_is_open(s) && !u->drag && !u->press) {
		struct timeval tv = { 0, HOVER_DELAY_MS * 1000 };
		evtimer_add(u->hover_timer, &tv);
	}
}

bool rei_tray_key_for(struct rei_server *s, struct rei_output *o, int index, char *key, size_t n)
{
	if (index == REI_TRAY_HOVER_OVERFLOW) {
		snprintf(key, n, "%s", REI_TRAY_OVERFLOW_KEY);
		return true;
	}
	struct rei_tray_set set;
	rei_tray_collect(s, o, NULL, &set);
	if (index < 0 || (size_t)index >= set.n_groups)
		return false;
	snprintf(key, n, "%s", set.groups[index].app_id);
	return true;
}

/* ------------------------------------------------------------------------ */
/* Dragging                                                                 */
/* ------------------------------------------------------------------------ */

void rei_tray_press(struct rei_server *s, struct rei_output *o, const char *key, double x, double y)
{
	struct rei_tray_ui *u = ui_of(s);
	u->press = true;
	u->drag = false;
	u->press_output = o;
	snprintf(u->press_key, sizeof u->press_key, "%s", key);
	u->press_x = x;
	u->press_y = y;
	tip_hide(u);  /* A click is not hovering. */
}

bool rei_tray_dragging(const struct rei_server *s)
{
	return s->tray_ui && s->tray_ui->drag;
}

int rei_tray_drag_index(const struct rei_server *s, const struct rei_output *o, const struct rei_tray_set *set,
			int *pos)
{
	const struct rei_tray_ui *u = s->tray_ui;
	if (!u || !u->drag || u->press_output != o)
		return -1;
	for (size_t i = 0; i < set->n_groups; i++) {
		if (strcmp(set->groups[i].app_id, u->press_key) == 0) {
			*pos = (int)u->drag_pos;
			return (int)i;
		}
	}
	return -1;
}

void rei_tray_pointer_moved(struct rei_server *s, double x, double y)
{
	rei_confirm_motion(s, x, y);
	bool over_start = rei_toolbar_start_at(s, x, y);
	bool over_mode = rei_toolbar_mode_at(s, x, y);
	if (over_mode != s->mode_hover) {
		s->mode_hover = over_mode;
		rei_server_schedule_damage(s);
	}
	if (over_start != s->start_hover) {
		s->start_hover = over_start;
		rei_server_schedule_damage(s);
	}
	if (!s->tray_ui && s->config->toolbar_tray == REI_TRAY_OFF)
		return;
	struct rei_tray_ui *u = ui_of(s);
	rei_popup_motion(s, x, y);  /* The open list's hover row follows the pointer. */

	if (u->press && strcmp(u->press_key, REI_TRAY_OVERFLOW_KEY) != 0) {
		if (!u->drag && hypot(x - u->press_x, y - u->press_y) > DRAG_THRESHOLD) {
			u->drag = true;
			rei_popup_close(s);
			tip_hide(u);
		}
		if (u->drag) {
			struct rei_rect bar = rei_toolbar_rect(s, u->press_output);
			bool vertical = s->config->toolbar_position == REI_BAR_LEFT ||
					s->config->toolbar_position == REI_BAR_RIGHT;
			u->drag_pos = vertical ? y - bar.y : x - bar.x;
			rei_server_schedule_damage(s);
			return;
		}
	}
	update_hover(u, x, y);
}

bool rei_tray_release(struct rei_server *s)
{
	struct rei_tray_ui *u = s->tray_ui;
	if (!u || !u->press)
		return false;
	bool was_drag = u->drag;
	if (was_drag) {
		struct rei_output *o = u->press_output;
		struct rei_tray_set set;
		rei_tray_collect(s, o, NULL, &set);
		size_t visible = 0;
		int drop = rei_toolbar_tray_drop_index(s, o, &visible);
		int from = -1;
		for (size_t i = 0; i < set.n_groups; i++)
			if (strcmp(set.groups[i].app_id, u->press_key) == 0)
				from = (int)i;
		if (from >= 0 && from < (int)visible && drop >= 0) {
			/* The new order: the visible groups as shown (dragged one moved), then the rest. */
			const char *ids[REI_BAR_MAX_TRAY + 1];
			size_t n = 0;
			for (size_t i = 0; i < visible; i++)
				if ((int)i != from)
					ids[n++] = set.groups[i].app_id;
			for (size_t j = n; j > (size_t)drop; j--)
				ids[j] = ids[j - 1];
			ids[drop] = set.groups[from].app_id;
			n++;
			for (size_t i = visible; i < set.n_groups; i++)
				ids[n++] = set.groups[i].app_id;

			char **old = u->order;
			size_t n_old = u->n_order;
			char **fresh = rei_xcalloc(n + n_old + 1, sizeof *fresh);
			size_t nf = 0;
			for (size_t i = 0; i < n; i++)
				fresh[nf++] = rei_xstrdup(ids[i]);
			for (size_t i = 0; i < n_old; i++) {  /* Applications not showing keep their place at the end. */
				bool dup = false;
				for (size_t k = 0; k < n && !dup; k++)
					dup = strcmp(ids[k], old[i]) == 0;
				if (dup)
					free(old[i]);
				else
					fresh[nf++] = old[i];
			}
			free(old);
			u->order = fresh;
			u->n_order = nf;
		}
	}
	u->press = false;
	u->drag = false;
	rei_server_schedule_damage(s);
	return was_drag;
}

/* ------------------------------------------------------------------------ */
/* The window list (a generic popup whose rows are windows)                 */
/* ------------------------------------------------------------------------ */

static bool menu_pick(struct rei_server *s, int row, void *data)
{
	struct rei_tray_ui *u = data;
	if (row >= 0 && row < u->menu_n) {
		struct rei_view *picked = u->menu_views[row];
		rei_popup_close(s);
		rei_wm_activate_view(picked);
	}
	return true;
}

void rei_tray_menu_open(struct rei_server *s, struct rei_output *o, const char *key,
			struct rei_view *const *views, int n_views, struct rei_rect bar,
			enum rei_bar_position position, struct rei_rect anchor)
{
	struct rei_tray_ui *u = ui_of(s);
	const struct rei_config *cfg = s->config;
	tip_hide(u);

	u->menu_n = n_views > REI_MENU_MAX_ROWS ? REI_MENU_MAX_ROWS : n_views;
	if (u->menu_n > REI_TRAY_MAX_VIEWS)
		u->menu_n = REI_TRAY_MAX_VIEWS;
	memcpy(u->menu_views, views, (size_t)u->menu_n * sizeof *views);

	struct rei_menu_state st;
	memset(&st, 0, sizeof st);
	st.n_rows = u->menu_n;
	for (int i = 0; i < st.n_rows; i++) {
		const struct rei_view *v = u->menu_views[i];
		const char *title = rei_view_title(v);
		snprintf(st.rows[i].label, sizeof st.rows[i].label, "%.80s%s",
			 title[0] ? title : rei_view_app_id(v), v->minimized ? "  (minimized)" : "");
		st.rows[i].minimized = v->minimized;
	}
	st.hover = -1;
	snprintf(st.font, sizeof st.font, "%s", cfg->toolbar_font);
	st.menu = cfg->theme.e[REI_ELEM_MENU];
	st.row = cfg->theme.e[REI_ELEM_MENU_ROW];
	rei_popup_open(s, o, key, &st, menu_pick, u, bar, position, anchor);
}

void rei_tray_menu_close(struct rei_server *s)
{
	rei_popup_close(s);
}

bool rei_tray_menu_is_open(const struct rei_server *s)
{
	return rei_popup_is_open(s);
}

bool rei_tray_menu_is_for(const struct rei_server *s, const char *key)
{
	return rei_popup_is_for(s, key);
}

void rei_tray_menu_press(struct rei_server *s, double x, double y)
{
	rei_popup_press(s, x, y);
}

void rei_tray_menu_view_gone(struct rei_server *s, const struct rei_view *v)
{
	struct rei_tray_ui *u = s->tray_ui;
	if (!u || !rei_popup_is_open(s))
		return;
	/* Only a window list holds windows; the Start menu's keys never start like these. */
	for (int i = 0; i < u->menu_n; i++)
		if (u->menu_views[i] == v && !rei_popup_is_for(s, "start")) {
			rei_popup_close(s);
			return;
		}
}

/* ------------------------------------------------------------------------ */

void rei_tray_emit(struct rei_server *s, struct rei_scene_snapshot *snap)
{
	struct rei_tray_ui *u = s->tray_ui;
	if (!u)
		return;
	if (u->tip_on && u->tip) {
		double t = (double)(rei_now_ns() - u->tip_start_ns) / ((double)FADE_MS * 1e6);
		t = t < 0 ? 0 : t > 1 ? 1 : t;
		double eased = 1.0 - (1.0 - t) * (1.0 - t);  /* ease-out */
		size_t first = snap->n_items;
		rei_snapshot_add(snap, u->tip, (float)u->tip_rect.x, (float)u->tip_rect.y, (float)u->tip_rect.w,
				 (float)u->tip_rect.h, false);
		rei_snapshot_set_alpha_from(snap, first, (float)eased);
	}
}

void rei_tray_finish(struct rei_server *s)
{
	struct rei_tray_ui *u = s->tray_ui;
	if (u) {
		rei_pixbuf_unref(u->tip);
		if (u->hover_timer)
			event_free(u->hover_timer);
		if (u->tick_timer)
			event_free(u->tick_timer);
		for (size_t i = 0; i < u->n_order; i++)
			free(u->order[i]);
		free(u->order);
		free(u);
		s->tray_ui = NULL;
	}
	rei_icons_cleanup();
}
