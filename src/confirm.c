#include "confirm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "input/input.h"
#include "input/xkb.h"
#include "server.h"
#include "toolbar.h"
#include "util/log.h"
#include "util/util.h"

struct rei_confirm {
	bool open;
	struct rei_output *output;
	struct rei_confirm_state st;
	rei_confirm_fn on_confirm;
	void *data;
	struct rei_pixbuf *pixbuf;
	int margin;
};

bool rei_confirm_is_open(const struct rei_server *s)
{
	return s->confirm && s->confirm->open;
}

static void rebuild(struct rei_server *s)
{
	struct rei_confirm *c = s->confirm;
	rei_pixbuf_unref(c->pixbuf);
	c->pixbuf = rei_bar_confirm_render(&c->st, &c->margin);
	rei_server_schedule_damage(s);
}

void rei_confirm_open(struct rei_server *s, const char *title, const char *message, const char *confirm_label,
		      rei_confirm_fn on_confirm, void *data)
{
	struct rei_output *o = s->focused_output;
	if (!o && !wl_list_empty(&s->outputs))
		o = wl_container_of(s->outputs.next, o, link);
	if (!o)
		return;
	if (!s->confirm)
		s->confirm = rei_xcalloc(1, sizeof *s->confirm);
	struct rei_confirm *c = s->confirm;
	rei_input_release_all(s, (uint32_t)(rei_now_ns() / 1000000ull));
	c->open = true;
	c->output = o;
	c->on_confirm = on_confirm;
	c->data = data;
	memset(&c->st, 0, sizeof c->st);
	snprintf(c->st.title, sizeof c->st.title, "%s", title);
	snprintf(c->st.message, sizeof c->st.message, "%s", message);
	snprintf(c->st.confirm_label, sizeof c->st.confirm_label, "%s", confirm_label);
	c->st.selected = 0;
	c->st.hover = -1;
	c->st.scale = (int)ceil(o->scale);
	snprintf(c->st.font, sizeof c->st.font, "%s", s->config->toolbar_font);
	c->st.box = s->config->theme.e[REI_ELEM_DIALOG];
	c->st.button = s->config->theme.e[REI_ELEM_START_BUTTON];
	rebuild(s);
	rei_info("confirm: %s", title);
}

void rei_confirm_close(struct rei_server *s)
{
	struct rei_confirm *c = s->confirm;
	if (!c || !c->open)
		return;
	c->open = false;
	rei_pixbuf_unref(c->pixbuf);
	c->pixbuf = NULL;
	rei_server_schedule_damage(s);
}

static void finish_with(struct rei_server *s, bool yes)
{
	struct rei_confirm *c = s->confirm;
	rei_confirm_fn fn = c->on_confirm;
	void *data = c->data;
	rei_confirm_close(s);
	if (yes && fn) {
		rei_info("confirm: yes");
		fn(s, data);
	} else {
		rei_info("confirm: cancelled");
	}
}

/* The box's top-left, logical. */
static void box_origin(const struct rei_confirm *c, double *x, double *y)
{
	int32_t ow, oh;
	rei_output_logical_size(c->output, &ow, &oh);
	*x = c->output->x + (ow - REI_DIALOG_W) / 2.0;
	*y = c->output->y + (oh - REI_DIALOG_H) / 2.0;
}

static int button_at(const struct rei_confirm *c, double px, double py)
{
	double bx, by;
	box_origin(c, &bx, &by);
	struct rei_rect b[2];
	rei_bar_confirm_buttons(b);
	for (int i = 0; i < 2; i++)
		if (px >= bx + b[i].x && px < bx + b[i].x + b[i].w && py >= by + b[i].y && py < by + b[i].y + b[i].h)
			return i;
	return -1;
}

void rei_confirm_key(struct rei_server *s, uint32_t code, bool pressed)
{
	if (!rei_confirm_is_open(s) || !pressed)
		return;
	struct rei_confirm *c = s->confirm;
	xkb_keysym_t sym = rei_xkb_sym(s->input->xkb, code);
	switch (sym) {
	case XKB_KEY_Escape:
		finish_with(s, false);
		break;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
	case XKB_KEY_space:
		finish_with(s, c->st.selected == 1);
		break;
	case XKB_KEY_Tab:
	case XKB_KEY_ISO_Left_Tab:
	case XKB_KEY_Left:
	case XKB_KEY_Right:
		c->st.selected = !c->st.selected;
		rebuild(s);
		break;
	default:
		break;
	}
}

void rei_confirm_press(struct rei_server *s, double x, double y)
{
	if (!rei_confirm_is_open(s))
		return;
	int b = button_at(s->confirm, x, y);
	if (b >= 0)
		finish_with(s, b == 1);
	/* A press elsewhere does nothing: the dialog is modal. */
}

void rei_confirm_motion(struct rei_server *s, double x, double y)
{
	if (!rei_confirm_is_open(s))
		return;
	struct rei_confirm *c = s->confirm;
	int b = button_at(c, x, y);
	if (b != c->st.hover) {
		c->st.hover = b;
		rebuild(s);
	}
}

void rei_confirm_emit(struct rei_server *s, struct rei_scene_snapshot *snap)
{
	struct rei_confirm *c = s->confirm;
	if (!c || !c->open || !c->pixbuf)
		return;
	double x, y;
	box_origin(c, &x, &y);
	rei_snapshot_add(snap, c->pixbuf, (float)x - c->margin, (float)y - c->margin,
			 (float)(REI_DIALOG_W + 2 * c->margin), (float)(REI_DIALOG_H + 2 * c->margin), false);
}

void rei_confirm_finish(struct rei_server *s)
{
	if (!s->confirm)
		return;
	rei_pixbuf_unref(s->confirm->pixbuf);
	free(s->confirm);
	s->confirm = NULL;
}
