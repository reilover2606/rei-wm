#include "popup.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"
#include "util/log.h"
#include "util/util.h"

struct rei_popup {
	bool open;
	struct rei_output *output;
	char key[96];
	struct rei_menu_state state;
	struct rei_rect rect;  /* The box, global logical. */
	int margin;
	struct rei_pixbuf *pixbuf;
	rei_popup_pick_fn pick;
	void *data;
};

static struct rei_popup *popup_of(struct rei_server *s)
{
	if (!s->popup)
		s->popup = rei_xcalloc(1, sizeof *s->popup);
	return s->popup;
}

void rei_popup_redraw(struct rei_server *s)
{
	struct rei_popup *p = s->popup;
	if (!p || !p->open)
		return;
	rei_pixbuf_unref(p->pixbuf);
	p->pixbuf = rei_bar_menu_render(&p->state);
	rei_server_schedule_damage(s);
}

void rei_popup_open(struct rei_server *s, struct rei_output *o, const char *key,
		    const struct rei_menu_state *state, rei_popup_pick_fn pick, void *data,
		    struct rei_rect bar, enum rei_bar_position position, struct rei_rect anchor)
{
	struct rei_popup *p = popup_of(s);
	rei_popup_close(s);

	p->open = true;
	p->output = o;
	snprintf(p->key, sizeof p->key, "%s", key);
	p->state = *state;
	p->state.hover = -1;
	p->state.scale = (int)ceil(o->scale);
	p->pick = pick;
	p->data = data;

	int w, h, row_h;
	rei_bar_menu_size(&p->state, &w, &h, &row_h, &p->margin);

	/* Beside the anchor, on the side away from the screen edge the bar sits on. */
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
	p->rect = r;
	/* The rows' centres, for tests and for debugging. */
	{
		char line[512];
		int n = snprintf(line, sizeof line, "popup %s: x=%d w=%d rows:", key, r.x, r.w);
		int last = -1;
		for (int y = 0; y < h && n < (int)sizeof line - 24; y++) {
			int row = rei_bar_menu_row_at(&p->state, y);
			if (row >= 0 && row != last) {
				int y2 = y;
				while (y2 + 1 < h && rei_bar_menu_row_at(&p->state, y2 + 1) == row)
					y2++;
				n += snprintf(line + n, sizeof line - (size_t)n, " [%d y=%d]", row, r.y + (y + y2) / 2);
				last = row;
				y = y2;
			}
		}
		rei_info("%s", line);
	}
	rei_popup_redraw(s);
}

void rei_popup_close(struct rei_server *s)
{
	struct rei_popup *p = s->popup;
	if (!p || !p->open)
		return;
	p->open = false;
	rei_pixbuf_unref(p->pixbuf);
	p->pixbuf = NULL;
	rei_server_schedule_damage(s);
}

bool rei_popup_is_open(const struct rei_server *s)
{
	return s->popup && s->popup->open;
}

bool rei_popup_is_for(const struct rei_server *s, const char *key)
{
	return rei_popup_is_open(s) && strcmp(s->popup->key, key) == 0;
}

static int row_at(const struct rei_popup *p, double x, double y)
{
	const struct rei_rect *r = &p->rect;
	if (x < r->x || y < r->y || x >= r->x + r->w || y >= r->y + r->h)
		return -1;
	return rei_bar_menu_row_at(&p->state, y - r->y);
}

void rei_popup_press(struct rei_server *s, double x, double y)
{
	struct rei_popup *p = s->popup;
	if (!p || !p->open)
		return;
	int row = row_at(p, x, y);
	if (row < 0) {
		rei_popup_close(s);
		return;
	}
	rei_popup_pick_fn pick = p->pick;
	void *data = p->data;
	/* The callback may close the popup, or open another one, so nothing is touched after it. */
	if (pick(s, row, data) && s->popup && s->popup->open && s->popup->pick == pick && s->popup->data == data)
		rei_popup_close(s);
}

void rei_popup_motion(struct rei_server *s, double x, double y)
{
	struct rei_popup *p = s->popup;
	if (!p || !p->open)
		return;
	int row = row_at(p, x, y);
	if (row != p->state.hover) {
		p->state.hover = row;
		rei_popup_redraw(s);
	}
}

void rei_popup_emit(struct rei_server *s, struct rei_scene_snapshot *snap)
{
	struct rei_popup *p = s->popup;
	if (!p || !p->open || !p->pixbuf)
		return;
	int m = p->margin;
	rei_snapshot_add(snap, p->pixbuf, (float)(p->rect.x - m), (float)(p->rect.y - m),
			 (float)(p->rect.w + 2 * m), (float)(p->rect.h + 2 * m), false);
}

struct rei_menu_state *rei_popup_state(struct rei_server *s)
{
	return s->popup && s->popup->open ? &s->popup->state : NULL;
}

void rei_popup_finish(struct rei_server *s)
{
	if (!s->popup)
		return;
	rei_pixbuf_unref(s->popup->pixbuf);
	free(s->popup);
	s->popup = NULL;
}
