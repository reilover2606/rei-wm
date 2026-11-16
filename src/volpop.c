#include "volpop.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "volume.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"

struct rei_volpop {
	bool open;
	struct rei_output *output;
	struct rei_rect box;          /* where the box sits on the screen (logical) */
	struct rei_pixbuf *pixbuf;
	int margin;
	bool dragging;
	int hover;                    /* 0 none, 1 the speaker button, 2 the slider */
	struct rei_volpop_state shown;  /* what the picture shows now */
};

bool rei_volpop_is_open(const struct rei_server *s)
{
	return s->volpop && s->volpop->open;
}

static void fill_state(struct rei_server *s, struct rei_volpop *p, struct rei_volpop_state *st)
{
	const struct rei_config *cfg = s->config;
	struct rei_audio_state a = rei_volume_state(s);
	memset(st, 0, sizeof *st);
	st->percent = a.percent;
	st->max = rei_volume_is_mpd(s) ? 100 : (cfg->max_volume > 0 ? cfg->max_volume : 100);   /* (MPD tops out at 100) */
	st->muted = a.muted;
	st->hover = p->hover;
	st->scale = (int)ceil(p->output->scale);
	snprintf(st->font, sizeof st->font, "%s", cfg->toolbar_font);
	st->box = cfg->theme.e[REI_ELEM_MENU];
	st->row = cfg->theme.e[REI_ELEM_MENU_ROW];
}

static void rebuild(struct rei_server *s)
{
	struct rei_volpop *p = s->volpop;
	struct rei_volpop_state st;
	fill_state(s, p, &st);
	rei_pixbuf_unref(p->pixbuf);
	p->pixbuf = rei_bar_volpop_render(&st, &p->margin);
	p->shown = st;
	rei_server_schedule_damage(s);
}

void rei_volpop_open(struct rei_server *s, struct rei_output *o, struct rei_rect bar,
		     enum rei_bar_position pos, struct rei_rect anchor)
{
	if (!s->volpop)
		s->volpop = rei_xcalloc(1, sizeof *s->volpop);
	struct rei_volpop *p = s->volpop;
	rei_volpop_close(s);
	p->open = true;
	p->output = o;
	p->dragging = false;
	p->hover = 0;

	struct rei_rect r = { 0, 0, REI_VPOP_W, REI_VPOP_H };
	switch (pos) {
	case REI_BAR_TOP:
	case REI_BAR_BOTTOM:
		r.x = anchor.x + anchor.w / 2 - r.w / 2;
		r.y = pos == REI_BAR_TOP ? bar.y + bar.h + 2 : bar.y - r.h - 2;
		break;
	case REI_BAR_LEFT:
	case REI_BAR_RIGHT:
		r.x = pos == REI_BAR_LEFT ? bar.x + bar.w + 2 : bar.x - r.w - 2;
		r.y = anchor.y + anchor.h / 2 - r.h / 2;
		break;
	}
	int32_t ow, oh;
	rei_output_logical_size(o, &ow, &oh);
	if (r.x + r.w > o->x + ow) r.x = o->x + ow - r.w;
	if (r.y + r.h > o->y + oh) r.y = o->y + oh - r.h;
	if (r.x < o->x) r.x = o->x;
	if (r.y < o->y) r.y = o->y;
	p->box = r;
	rei_info("volume popup: x=%d y=%d w=%d h=%d", r.x, r.y, r.w, r.h);
	rebuild(s);
}

void rei_volpop_close(struct rei_server *s)
{
	struct rei_volpop *p = s->volpop;
	if (!p || !p->open)
		return;
	p->open = false;
	p->dragging = false;
	rei_pixbuf_unref(p->pixbuf);
	p->pixbuf = NULL;
	rei_server_schedule_damage(s);
}

bool rei_volpop_contains(const struct rei_server *s, double x, double y)
{
	if (!rei_volpop_is_open(s))
		return false;
	const struct rei_rect *b = &s->volpop->box;
	return x >= b->x && y >= b->y && x < b->x + b->w && y < b->y + b->h;
}

/* The volume (percent) a pointer x position on the slider stands for. */
static int percent_at(struct rei_server *s, double x)
{
	struct rei_volpop *p = s->volpop;
	struct rei_rect button, track;
	rei_bar_volpop_layout(&button, &track);
	int max = rei_volume_is_mpd(s) ? 100 : (s->config->max_volume > 0 ? s->config->max_volume : 100);
	double f = (x - (p->box.x + track.x)) / (double)track.w;
	f = f < 0 ? 0 : f > 1 ? 1 : f;
	return (int)lround(f * max);
}

static void set_percent(struct rei_server *s, int percent)
{
	struct rei_audio_state a = rei_volume_state(s);
	if (!a.available)
		return;
	rei_volume_change(s, percent - a.percent);
	rebuild(s);
}

bool rei_volpop_press(struct rei_server *s, double x, double y)
{
	if (!rei_volpop_contains(s, x, y))
		return false;
	struct rei_volpop *p = s->volpop;
	struct rei_rect button, track;
	rei_bar_volpop_layout(&button, &track);
	double lx = x - p->box.x, ly = y - p->box.y;
	if (lx >= button.x && lx < button.x + button.w && ly >= button.y && ly < button.y + button.h) {
		rei_volume_toggle_mute(s);
		rebuild(s);
	} else if (lx >= track.x - 8 && lx < track.x + track.w + 8) {
		p->dragging = true;
		set_percent(s, percent_at(s, x));
	}
	return true;
}

void rei_volpop_motion(struct rei_server *s, double x, double y)
{
	struct rei_volpop *p = s->volpop;
	if (!p || !p->open)
		return;
	if (p->dragging) {
		set_percent(s, percent_at(s, x));
		return;
	}
	struct rei_rect button, track;
	rei_bar_volpop_layout(&button, &track);
	double lx = x - p->box.x, ly = y - p->box.y;
	int hover = 0;
	if (rei_volpop_contains(s, x, y)) {
		if (lx >= button.x && lx < button.x + button.w)
			hover = 1;
		else if (lx >= track.x - 8)
			hover = 2;
	}
	if (hover != p->hover) {
		p->hover = hover;
		(void)ly;
		rebuild(s);
	}
}

void rei_volpop_release(struct rei_server *s)
{
	if (s->volpop)
		s->volpop->dragging = false;
}

void rei_volpop_emit(struct rei_server *s, struct rei_scene_snapshot *snap)
{
	struct rei_volpop *p = s->volpop;
	if (!p || !p->open)
		return;
	/* The volume may have moved (media keys, another program): redraw if what it shows differs. */
	struct rei_audio_state a = rei_volume_state(s);
	if (!a.available) {
		rei_volpop_close(s);
		return;
	}
	if (a.percent != p->shown.percent || a.muted != p->shown.muted)
		rebuild(s);
	if (!p->pixbuf)
		return;
	rei_snapshot_add(snap, p->pixbuf, (float)(p->box.x - p->margin), (float)(p->box.y - p->margin),
			 (float)(p->box.w + 2 * p->margin), (float)(p->box.h + 2 * p->margin), false);
}

void rei_volpop_finish(struct rei_server *s)
{
	if (!s->volpop)
		return;
	rei_pixbuf_unref(s->volpop->pixbuf);
	free(s->volpop);
	s->volpop = NULL;
}

int rei_volpop_hover(const struct rei_server *s)
{
	return rei_volpop_is_open(s) ? s->volpop->hover : 0;
}
