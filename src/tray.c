#include "tray.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "desktop.h"
#include "icons.h"
#include "proto/xdg_shell.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"
#include "xwayland.h"

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

size_t rei_tray_collect(struct rei_server *s, const struct rei_output *o, struct rei_tray_group *out,
			size_t cap)
{
	size_t n = 0;
	const struct rei_view *active = rei_wm_active_view(s);
	struct rei_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (!v->mapped || !v->ws || v->ws->output != o)
			continue;
		const char *app = rei_view_app_id(v);
		const char *title = rei_view_title(v);
		/* Windows of one application share an app id; without one, a title stands in. */
		const char *key = app[0] ? app : title;

		struct rei_tray_group *g = NULL;
		for (size_t i = 0; i < n; i++)
			if (strcmp(out[i].app_id, key) == 0) {
				g = &out[i];
				break;
			}
		if (!g) {
			if (n == cap)
				continue;
			g = &out[n++];
			memset(g, 0, sizeof *g);
			snprintf(g->app_id, sizeof g->app_id, "%s", key);
			g->minimized = true;
		}
		g->count++;
		if (g->n_views < REI_TRAY_MAX_VIEWS)
			g->views[g->n_views++] = v;
		g->active = g->active || v == active;
		g->minimized = g->minimized && v->minimized;
	}

	for (size_t i = 0; i < n; i++) {
		struct rei_tray_group *g = &out[i];
		const char *title = g->n_views ? rei_view_title(g->views[0]) : "";
		if (g->count == 1) {
			snprintf(g->label, sizeof g->label, "%s", title[0] ? title : g->app_id);
		} else {
			const char *name = rei_icon_app_name(g->app_id);
			snprintf(g->label, sizeof g->label, "%s (%d)", name ? name : g->app_id, g->count);
		}
	}
	return n;
}

/* ------------------------------------------------------------------------ */
/* The window list                                                          */
/* ------------------------------------------------------------------------ */

struct rei_tray_menu {
	bool open;
	struct rei_output *output;
	char app_id[96];
	struct rei_view *views[REI_TRAY_MAX_VIEWS];
	int n;
	struct rei_rect rect;  /* Global, logical. */
	int row_h;
	struct rei_menu_state state;
	struct rei_pixbuf *pixbuf;
};

static struct rei_tray_menu *menu_of(struct rei_server *s)
{
	if (!s->tray_menu)
		s->tray_menu = rei_xcalloc(1, sizeof *s->tray_menu);
	return s->tray_menu;
}

static void menu_redraw(struct rei_server *s)
{
	struct rei_tray_menu *m = s->tray_menu;
	rei_pixbuf_unref(m->pixbuf);
	m->pixbuf = rei_bar_menu_render(&m->state);
	rei_server_schedule_damage(s);
}

void rei_tray_menu_open(struct rei_server *s, struct rei_output *o, const struct rei_tray_group *g,
			struct rei_rect bar, enum rei_bar_position position, struct rei_rect anchor)
{
	struct rei_tray_menu *m = menu_of(s);
	const struct rei_config *cfg = s->config;

	m->open = true;
	m->output = o;
	snprintf(m->app_id, sizeof m->app_id, "%s", g->app_id);
	m->n = g->n_views;
	memcpy(m->views, g->views, sizeof g->views);

	struct rei_menu_state *st = &m->state;
	memset(st, 0, sizeof *st);
	st->n_rows = g->n_views;
	for (int i = 0; i < g->n_views; i++) {
		const struct rei_view *v = g->views[i];
		const char *title = rei_view_title(v);
		snprintf(st->rows[i].label, sizeof st->rows[i].label, "%.80s%s", title[0] ? title : g->app_id,
			 v->minimized ? "  (minimized)" : "");
		st->rows[i].minimized = v->minimized;
	}
	st->hover = -1;
	st->scale = (int)ceil(o->scale);
	snprintf(st->font, sizeof st->font, "%s", cfg->toolbar_font);
	st->background = cfg->toolbar_background;
	st->foreground = cfg->toolbar_foreground;
	st->highlight = cfg->toolbar_highlight;

	int w, h, row_h;
	rei_bar_menu_size(st, &w, &h, &row_h);
	m->row_h = row_h;

	/* Beside the tile, on the side away from the screen's edge the bar sits on. */
	struct rei_rect r = { anchor.x, anchor.y, w, h };
	switch (position) {
	case REI_BAR_TOP: r.y = bar.y + bar.h + 2; break;
	case REI_BAR_BOTTOM: r.y = bar.y - h - 2; break;
	case REI_BAR_LEFT: r.x = bar.x + bar.w + 2; break;
	case REI_BAR_RIGHT: r.x = bar.x - w - 2; break;
	}
	int32_t ow, oh;
	rei_output_logical_size(o, &ow, &oh);
	if (r.x + r.w > o->x + ow) r.x = o->x + ow - r.w;
	if (r.y + r.h > o->y + oh) r.y = o->y + oh - r.h;
	if (r.x < o->x) r.x = o->x;
	if (r.y < o->y) r.y = o->y;
	m->rect = r;

	menu_redraw(s);
}

void rei_tray_menu_close(struct rei_server *s)
{
	struct rei_tray_menu *m = s->tray_menu;
	if (!m || !m->open)
		return;
	m->open = false;
	rei_pixbuf_unref(m->pixbuf);
	m->pixbuf = NULL;
	rei_server_schedule_damage(s);
}

bool rei_tray_menu_is_open(const struct rei_server *s)
{
	return s->tray_menu && s->tray_menu->open;
}

bool rei_tray_menu_is_for(const struct rei_server *s, const char *app_id)
{
	return rei_tray_menu_is_open(s) && strcmp(s->tray_menu->app_id, app_id) == 0;
}

static int row_at(const struct rei_tray_menu *m, double x, double y)
{
	if (x < m->rect.x || y < m->rect.y || x >= m->rect.x + m->rect.w || y >= m->rect.y + m->rect.h)
		return -1;
	int row = (int)((y - m->rect.y - 1) / m->row_h);
	return row >= 0 && row < m->n ? row : -1;
}

void rei_tray_menu_press(struct rei_server *s, double x, double y)
{
	struct rei_tray_menu *m = s->tray_menu;
	if (!m || !m->open)
		return;
	int row = row_at(m, x, y);
	struct rei_view *picked = row >= 0 ? m->views[row] : NULL;
	rei_tray_menu_close(s);
	if (picked)
		rei_wm_activate_view(picked);
}

void rei_tray_menu_motion(struct rei_server *s, double x, double y)
{
	struct rei_tray_menu *m = s->tray_menu;
	if (!m || !m->open)
		return;
	int row = row_at(m, x, y);
	if (row != m->state.hover) {
		m->state.hover = row;
		menu_redraw(s);
	}
}

void rei_tray_menu_emit(struct rei_server *s, struct rei_scene_snapshot *snap)
{
	struct rei_tray_menu *m = s->tray_menu;
	if (!m || !m->open || !m->pixbuf)
		return;
	rei_snapshot_add(snap, m->pixbuf, (float)m->rect.x, (float)m->rect.y, (float)m->rect.w,
			 (float)m->rect.h, true);
}

void rei_tray_menu_view_gone(struct rei_server *s, const struct rei_view *v)
{
	struct rei_tray_menu *m = s->tray_menu;
	if (!m || !m->open)
		return;
	for (int i = 0; i < m->n; i++)
		if (m->views[i] == v) {
			rei_tray_menu_close(s);
			return;
		}
}

void rei_tray_finish(struct rei_server *s)
{
	if (s->tray_menu) {
		rei_pixbuf_unref(s->tray_menu->pixbuf);
		free(s->tray_menu);
		s->tray_menu = NULL;
	}
	rei_icons_cleanup();
}
