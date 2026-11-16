#include "msgbuf.h"

#include <ctype.h>
#include <event2/event.h>
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "input/actions.h"
#include "output.h"
#include "server.h"
#include "toolbar.h"
#include "util/log.h"
#include "util/util.h"

#define MAX_CHARS 4000
#define MAX_LINES 40
#define MARGIN 10          /* from the screen edge and the toolbar */
#define GAP 6              /* between messages */
#define PANEL_PAD 12       /* the stack view's backdrop around the column */
#define FADE_STEP 0.18     /* dimming per newer message */
#define FADE_FLOOR 0.08
#define FADE_OUT_MS 1500   /* the fade at the end of a message's time */
#define TICK_MS 33
#define MAX_PUSHES_PER_COMMAND 20
#define CAPTURE_IDLE_MS 300

struct msg {
	char *text;
	int64_t added_ms;
	struct rei_pixbuf *pix;
	int pw, ph;               /* the picture's logical size */
	int pix_scale, pix_width;
	const void *pix_cfg;      /* the config it was drawn with: a reload redraws it */
	double y;                 /* where it is drawn now (eases towards its target) */
	bool y_set;
};

struct capture {
	struct rei_server *server;
	struct wl_list link;
	int fd;
	struct event *readable, *flush;
	char buf[16384];
	size_t len;
	int pushes;
};

struct rei_messages {
	struct msg *items;     /* oldest first */
	size_t n, cap;
	struct event *tick;
	bool stack_open;
	int scroll;            /* messages hidden at the new end in the stack view */
	struct rei_rect panel; /* the stack view's rectangle, as last drawn */
	struct rei_pixbuf *panel_pix;
	int panel_w, panel_h, panel_scale;
	const void *panel_cfg;
	struct wl_list captures;
};

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static struct rei_messages *state_of(struct rei_server *s)
{
	if (!s->messages) {
		s->messages = rei_xcalloc(1, sizeof *s->messages);
		wl_list_init(&s->messages->captures);
	}
	return s->messages;
}

/* ------------------------------------------------------------------------ */
/* Text                                                                     */
/* ------------------------------------------------------------------------ */

char *rei_msg_sanitize(const char *raw, unsigned long len)
{
	char *clean = rei_xcalloc(len * 4 + 8, 1);   /* a tab becomes four spaces */
	size_t o = 0;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)raw[i];
		if (c == 0x1b) {   /* an escape sequence: colours, cursor moves, titles: not text */
			if (i + 1 < len && raw[i + 1] == '[') {
				i += 2;
				while (i < len && !(raw[i] >= 0x40 && raw[i] <= 0x7e))
					i++;
			} else if (i + 1 < len && raw[i + 1] == ']') {
				i += 2;
				while (i < len && raw[i] != 0x07 && !(raw[i] == 0x1b && i + 1 < len && raw[i + 1] == '\\'))
					i++;
				if (i < len && raw[i] == 0x1b)
					i++;
			} else if (i + 1 < len) {
				i++;
			}
			continue;
		}
		if (c == '\r')
			continue;
		if (c == '\t') {
			memcpy(clean + o, "    ", 4);
			o += 4;
			continue;
		}
		if (c < 0x20 && c != '\n')
			continue;
		clean[o++] = (char)c;
	}
	clean[o] = '\0';

	char *valid = g_utf8_make_valid(clean, (gssize)o);   /* bad bytes become U+FFFD */
	free(clean);
	/* Trim blank space at both ends. */
	char *start = valid;
	while (*start && (isspace((unsigned char)*start)))
		start++;
	size_t n = strlen(start);
	while (n > 0 && isspace((unsigned char)start[n - 1]))
		n--;
	if (n == 0) {
		g_free(valid);
		return NULL;
	}
	/* Cap the lines and the characters, at a whole character. */
	size_t keep = 0;
	int lines = 1;
	bool cut = false;
	while (keep < n) {
		if (start[keep] == '\n' && ++lines > MAX_LINES) {
			cut = true;
			break;
		}
		if (keep >= MAX_CHARS) {
			cut = true;
			break;
		}
		keep++;
	}
	char *out = rei_xcalloc(keep + 8, 1);
	memcpy(out, start, keep);
	out[keep] = '\0';
	rei_utf8_trim(out);
	if (cut)
		strcat(out, " …");
	g_free(valid);
	return out;
}

/* ------------------------------------------------------------------------ */
/* The history                                                              */
/* ------------------------------------------------------------------------ */

static void msg_free(struct msg *m)
{
	free(m->text);
	rei_pixbuf_unref(m->pix);
}

static void on_tick(evutil_socket_t fd, short what, void *arg);

static void tick_start(struct rei_server *s)
{
	struct rei_messages *m = state_of(s);
	if (!m->tick)
		m->tick = evtimer_new(s->base, on_tick, s);
	struct timeval tv = { 0, TICK_MS * 1000 };
	evtimer_add(m->tick, &tv);
}

void rei_msg_push(struct rei_server *s, const char *text)
{
	if (!s->config->messages_enabled || !text)
		return;
	char *clean = rei_msg_sanitize(text, strlen(text));
	if (!clean)
		return;
	struct rei_messages *m = state_of(s);
	if (m->n == m->cap) {
		m->cap = m->cap ? m->cap * 2 : 32;
		m->items = rei_xrealloc(m->items, m->cap * sizeof *m->items);
	}
	m->items[m->n++] = (struct msg){ .text = clean, .added_ms = now_ms() };
	size_t limit = (size_t)s->config->messages_history;
	while (m->n > limit) {   /* the oldest go */
		msg_free(&m->items[0]);
		memmove(m->items, m->items + 1, (m->n - 1) * sizeof *m->items);
		m->n--;
	}
	if (m->scroll > 0 && m->scroll < (int)m->n - 1)
		m->scroll++;   /* keep what the reader is looking at where it is */
	rei_info("message: %.80s%s", clean, strlen(clean) > 80 ? "…" : "");
	tick_start(s);
	rei_server_schedule_damage(s);
}

/* ------------------------------------------------------------------------ */
/* Capturing a command's output                                             */
/* ------------------------------------------------------------------------ */

static void capture_flush(struct capture *c)
{
	if (c->len == 0)
		return;
	if (c->pushes < MAX_PUSHES_PER_COMMAND) {
		char *clean = rei_msg_sanitize(c->buf, c->len);
		if (clean) {
			rei_msg_push(c->server, clean);
			c->pushes++;
			free(clean);
		}
	}
	c->len = 0;
}

static void capture_free(struct capture *c)
{
	if (c->readable)
		event_free(c->readable);
	if (c->flush)
		event_free(c->flush);
	close(c->fd);
	wl_list_remove(&c->link);
	free(c);
}

static void on_capture_flush(evutil_socket_t fd, short what, void *arg)
{
	(void)fd; (void)what;
	capture_flush(arg);
}

static void on_capture_readable(evutil_socket_t fd, short what, void *arg)
{
	(void)what;
	struct capture *c = arg;
	for (;;) {
		char tmp[4096];
		ssize_t n = read(fd, tmp, sizeof tmp);
		if (n > 0) {
			size_t room = sizeof c->buf - c->len;
			size_t take = (size_t)n < room ? (size_t)n : room;   /* a flood is cut, not kept */
			memcpy(c->buf + c->len, tmp, take);
			c->len += take;
			if (c->len == sizeof c->buf)
				capture_flush(c);
			continue;
		}
		if (n == 0) {   /* the program closed its output: whatever is left is the last message */
			capture_flush(c);
			capture_free(c);
			return;
		}
		if (errno == EINTR)
			continue;
		break;   /* EAGAIN: nothing more for now */
	}
	/* Output often arrives in pieces: show it once it pauses. */
	struct timeval tv = { 0, CAPTURE_IDLE_MS * 1000 };
	evtimer_add(c->flush, &tv);
}

bool rei_msg_run_captured(struct rei_server *s, const char *command)
{
	if (!s->config->messages_enabled)
		return rei_spawn(s, command);
	int fd = -1;
	if (!rei_spawn_capture(s, command, &fd))
		return false;
	struct rei_messages *m = state_of(s);
	struct capture *c = rei_xcalloc(1, sizeof *c);
	c->server = s;
	c->fd = fd;
	c->readable = event_new(s->base, fd, EV_READ | EV_PERSIST, on_capture_readable, c);
	c->flush = evtimer_new(s->base, on_capture_flush, c);
	event_add(c->readable, NULL);
	wl_list_insert(&m->captures, &c->link);
	return true;
}

/* ------------------------------------------------------------------------ */
/* Showing it                                                               */
/* ------------------------------------------------------------------------ */

static const struct rei_output *target_output(struct rei_server *s)
{
	const struct rei_output *o = s->focused_output;
	if (!o && !wl_list_empty(&s->outputs))
		o = wl_container_of(s->outputs.next, o, link);
	return o;
}

static void ensure_picture(struct rei_server *s, struct msg *msg, int scale)
{
	const struct rei_config *cfg = s->config;
	if (msg->pix && msg->pix_scale == scale && msg->pix_width == cfg->messages_width && msg->pix_cfg == cfg)
		return;
	rei_pixbuf_unref(msg->pix);
	struct rei_message_state st;
	memset(&st, 0, sizeof st);
	snprintf(st.text, sizeof st.text, "%s", msg->text);
	snprintf(st.font, sizeof st.font, "%s", cfg->messages_font);
	st.scale = scale;
	st.width = cfg->messages_width;
	st.align_right = cfg->messages_right;
	st.style = cfg->theme.e[REI_ELEM_MESSAGES];
	msg->pix = rei_bar_message_render(&st, &msg->pw, &msg->ph);
	msg->pix_scale = scale;
	msg->pix_width = cfg->messages_width;
	msg->pix_cfg = cfg;
}

/* Is any message still within its time? */
static bool any_live(struct rei_server *s, int64_t now)
{
	struct rei_messages *m = s->messages;
	int64_t life = (int64_t)(s->config->messages_duration * 1000);
	return m && m->n > 0 && now - m->items[m->n - 1].added_ms < life;
}

static void on_tick(evutil_socket_t fd, short what, void *arg)
{
	(void)fd; (void)what;
	struct rei_server *s = arg;
	struct rei_messages *m = s->messages;
	if (!m)
		return;
	rei_server_schedule_damage(s);   /* the fade and the slide advance with each frame */
	/* Keep going while something is on screen or still sliding. */
	if (any_live(s, now_ms()) || m->stack_open)
		tick_start(s);
	else {
		/* One last pass was scheduled above; the animation is over. */
	}
}

static void emit_one(struct rei_scene_snapshot *snap, struct msg *msg, float x, float y, float alpha)
{
	if (!msg->pix || alpha <= 0.003f)
		return;
	size_t first = snap->n_items;
	rei_snapshot_add(snap, msg->pix, x, y, (float)msg->pw, (float)msg->ph, false);
	if (alpha < 0.999f)
		rei_snapshot_set_alpha_from(snap, first, alpha);
}

void rei_msg_emit(struct rei_server *s, struct rei_scene_snapshot *snap)
{
	struct rei_messages *m = s->messages;
	const struct rei_config *cfg = s->config;
	if (!m || m->n == 0 || !cfg->messages_enabled)
		return;
	const struct rei_output *o = target_output(s);
	if (!o)
		return;
	int scale = (int)ceil(o->scale > 0 ? o->scale : 1.0);
	struct rei_rect usable = rei_toolbar_usable(o);
	int col_w = cfg->messages_width + 2 * REI_MSG_PAD;
	float x = cfg->messages_right ? (float)(usable.x + usable.w - MARGIN - col_w) : (float)(usable.x + MARGIN);
	float bottom = (float)(usable.y + usable.h - MARGIN);

	if (m->stack_open) {
		/* Everything, at full strength, in a column with a backdrop; newest at the bottom. */
		int pw = col_w + 2 * PANEL_PAD, ph = usable.h - 2 * MARGIN;
		if (ph < 80)
			return;
		float px = cfg->messages_right ? (float)(usable.x + usable.w - MARGIN - pw) : (float)(usable.x + MARGIN);
		float py = (float)(usable.y + MARGIN);
		m->panel = (struct rei_rect){ (int)px, (int)py, pw, ph };
		if (!m->panel_pix || m->panel_w != pw || m->panel_h != ph || m->panel_scale != scale || m->panel_cfg != cfg) {
			rei_pixbuf_unref(m->panel_pix);
			m->panel_pix = rei_bar_message_panel_render(&cfg->theme.e[REI_ELEM_MESSAGES], scale, pw, ph);
			m->panel_w = pw;
			m->panel_h = ph;
			m->panel_scale = scale;
			m->panel_cfg = cfg;
		}
		if (m->panel_pix)
			rei_snapshot_add(snap, m->panel_pix, px, py, (float)pw, (float)ph, false);
		float y = py + (float)ph - PANEL_PAD;
		float top = py + PANEL_PAD;
		int skip = m->scroll;
		for (long i = (long)m->n - 1; i >= 0; i--) {
			if (skip > 0) {
				skip--;
				continue;
			}
			struct msg *msg = &m->items[i];
			ensure_picture(s, msg, scale);
			if (!msg->pix)
				continue;
			y -= (float)msg->ph;
			if (y < top)
				break;   /* it does not fit whole: stop here, scrolling brings it in */
			emit_one(snap, msg, px + PANEL_PAD, y, 1.0f);
			y -= GAP;
		}
		return;
	}

	/* The compact column: the messages still within their time, newest at the bottom. */
	int64_t now = now_ms();
	int64_t life = (int64_t)(cfg->messages_duration * 1000);
	float fade_ms = (float)(FADE_OUT_MS < life / 2 ? FADE_OUT_MS : life / 2);
	float y = bottom;
	int rank = 0;
	for (long i = (long)m->n - 1; i >= 0 && rank < cfg->messages_max_visible; i--) {
		struct msg *msg = &m->items[i];
		int64_t age = now - msg->added_ms;
		if (age >= life)
			break;   /* older ones are older still */
		ensure_picture(s, msg, scale);
		if (!msg->pix)
			continue;
		float target = y - (float)msg->ph;
		if (target < (float)usable.y)
			break;
		if (!msg->y_set) {
			msg->y = target + 24;   /* slides up into place */
			msg->y_set = true;
		}
		msg->y += (target - msg->y) * 0.3;   /* eased: each frame closes a third of the gap */
		if (fabs(target - msg->y) < 0.3)
			msg->y = target;
		float by_rank = (float)(1.0 - FADE_STEP * rank);
		if (by_rank < FADE_FLOOR)
			by_rank = FADE_FLOOR;
		float remaining = (float)(life - age);
		float by_time = remaining >= fade_ms ? 1.0f : remaining / fade_ms;
		emit_one(snap, msg, x, (float)msg->y, by_rank * by_time);
		y = target - GAP;
		rank++;
	}
	if (any_live(s, now) && m->tick) {   /* (the timer re-arms itself) */
	}
}

/* ------------------------------------------------------------------------ */
/* The stack view                                                           */
/* ------------------------------------------------------------------------ */

void rei_msg_toggle_stack(struct rei_server *s)
{
	if (!s->config->messages_enabled)
		return;
	struct rei_messages *m = state_of(s);
	m->stack_open = !m->stack_open;
	m->scroll = 0;
	if (m->stack_open)
		tick_start(s);
	rei_server_schedule_damage(s);
}

bool rei_msg_stack_open(const struct rei_server *s)
{
	return s->messages && s->messages->stack_open;
}

void rei_msg_stack_close(struct rei_server *s)
{
	if (s->messages && s->messages->stack_open) {
		s->messages->stack_open = false;
		rei_server_schedule_damage(s);
	}
}

bool rei_msg_stack_at(const struct rei_server *s, double x, double y)
{
	if (!rei_msg_stack_open(s))
		return false;
	const struct rei_rect *p = &s->messages->panel;
	return x >= p->x && y >= p->y && x < p->x + p->w && y < p->y + p->h;
}

void rei_msg_stack_scroll(struct rei_server *s, double value)
{
	struct rei_messages *m = s->messages;
	if (!m || !m->stack_open || value == 0)
		return;
	m->scroll += value < 0 ? 1 : -1;   /* wheel up: toward older messages */
	if (m->scroll < 0)
		m->scroll = 0;
	if (m->scroll > (int)m->n - 1)
		m->scroll = (int)m->n - 1;
	rei_server_schedule_damage(s);
}

void rei_msg_finish(struct rei_server *s)
{
	struct rei_messages *m = s->messages;
	if (!m)
		return;
	struct capture *c, *tmp;
	wl_list_for_each_safe(c, tmp, &m->captures, link)
		capture_free(c);
	if (m->tick)
		event_free(m->tick);
	for (size_t i = 0; i < m->n; i++)
		msg_free(&m->items[i]);
	free(m->items);
	rei_pixbuf_unref(m->panel_pix);
	free(m);
	s->messages = NULL;
}
