#include "runprompt.h"

#include <event2/event.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "input/actions.h"
#include "input/input.h"
#include "input/xkb.h"
#include "server.h"
#include "toolbar.h"
#include "util/log.h"
#include "util/util.h"

#define BLINK_MS_DEFAULT 530
#define MAX_HEIGHT_PERCENT 60

struct rei_prompt {
	bool open;
	struct rei_server *server;
	struct rei_output *output;
	char text[REI_PROMPT_MAX_TEXT + 8];
	size_t len;
	bool cursor_on;
	struct event *blink, *repeat;
	uint32_t repeat_code;
	bool repeat_fast;
	struct rei_pixbuf *pixbuf;
	int box_w, box_h, margin;
};

static int blink_ms(void)
{
	const char *e = getenv("REI_PROMPT_BLINK_MS");  /* A test hook. */
	return e && atoi(e) > 0 ? atoi(e) : BLINK_MS_DEFAULT;
}

static void on_blink(evutil_socket_t fd, short what, void *arg);
static void on_repeat(evutil_socket_t fd, short what, void *arg);

static struct rei_prompt *prompt_of(struct rei_server *s)
{
	if (!s->prompt) {
		struct rei_prompt *p = rei_xcalloc(1, sizeof *p);
		p->server = s;
		p->blink = evtimer_new(s->base, on_blink, p);
		p->repeat = evtimer_new(s->base, on_repeat, p);
		s->prompt = p;
	}
	return s->prompt;
}

bool rei_prompt_is_open(const struct rei_server *s)
{
	return s->prompt && s->prompt->open;
}

/* ------------------------------------------------------------------------ */

static void fill_state(const struct rei_prompt *p, struct rei_prompt_state *st, const char *text)
{
	const struct rei_config *cfg = p->server->config;
	memset(st, 0, sizeof *st);
	snprintf(st->text, sizeof st->text, "%s", text);
	st->cursor_on = p->cursor_on;
	st->box_w = p->box_w;
	st->scale = (int)ceil(p->output->scale);
	snprintf(st->font, sizeof st->font, "%s", cfg->start_run_font);
	st->style = cfg->theme.e[REI_ELEM_RUN_PROMPT];
}

static void rebuild(struct rei_prompt *p)
{
	struct rei_prompt_state st;
	fill_state(p, &st, p->text);
	rei_pixbuf_unref(p->pixbuf);
	p->pixbuf = rei_bar_prompt_render(&st, &p->box_h, &p->margin);
	rei_server_schedule_damage(p->server);
}

static void arm_blink(struct rei_prompt *p)
{
	int ms = blink_ms();
	struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
	evtimer_add(p->blink, &tv);
}

static void on_blink(evutil_socket_t fd, short what, void *arg)
{
	(void)fd; (void)what;
	struct rei_prompt *p = arg;
	if (!p->open)
		return;
	p->cursor_on = !p->cursor_on;
	rebuild(p);
	arm_blink(p);
}

/* Typing makes the cursor solid again, and restarts its blinking. */
static void cursor_solid(struct rei_prompt *p)
{
	p->cursor_on = true;
	evtimer_del(p->blink);
	arm_blink(p);
}

void rei_prompt_open(struct rei_server *s)
{
	struct rei_output *o = s->focused_output;
	if (!o && !wl_list_empty(&s->outputs))
		o = wl_container_of(s->outputs.next, o, link);
	if (!o)
		return;
	struct rei_prompt *p = prompt_of(s);
	if (p->open)
		return;
	/* Keys still held (the one that opened it, say) must not stay "down" in the client that had
	 * the keyboard: it would repeat them forever. */
	rei_input_release_all(s, (uint32_t)(rei_now_ns() / 1000000ull));

	p->open = true;
	p->output = o;
	p->text[0] = '\0';
	p->len = 0;
	int32_t ow, oh;
	rei_output_logical_size(o, &ow, &oh);
	int w = (int)(ow * 0.6);
	if (w < 420)
		w = 420;
	if (w > ow - 40)
		w = ow - 40;
	p->box_w = w;
	p->cursor_on = true;
	rebuild(p);
	arm_blink(p);
	rei_info("run prompt: open");
}

void rei_prompt_close(struct rei_server *s)
{
	struct rei_prompt *p = s->prompt;
	if (!p || !p->open)
		return;
	p->open = false;
	evtimer_del(p->blink);
	evtimer_del(p->repeat);
	p->repeat_code = 0;
	rei_pixbuf_unref(p->pixbuf);
	p->pixbuf = NULL;
	rei_server_schedule_damage(s);
}

void rei_prompt_press(struct rei_server *s)
{
	rei_prompt_close(s);
}

/* ------------------------------------------------------------------------ */
/* Editing                                                                  */
/* ------------------------------------------------------------------------ */

/* Adds text unless the box would then be taller than the limit. */
static bool append(struct rei_prompt *p, const char *utf8, size_t n)
{
	if (p->len + n > REI_PROMPT_MAX_TEXT)
		return false;
	char candidate[REI_PROMPT_MAX_TEXT + 8];
	memcpy(candidate, p->text, p->len);
	memcpy(candidate + p->len, utf8, n);
	candidate[p->len + n] = '\0';

	struct rei_prompt_state st;
	fill_state(p, &st, candidate);
	int32_t ow, oh;
	rei_output_logical_size(p->output, &ow, &oh);
	if (rei_bar_prompt_height(&st) > oh * MAX_HEIGHT_PERCENT / 100) {
		rei_debug("run prompt: full; ignoring input");
		return false;
	}
	memcpy(p->text, candidate, p->len + n + 1);
	p->len += n;
	return true;
}

static void backspace(struct rei_prompt *p)
{
	if (p->len == 0)
		return;
	do {
		p->len--;
	} while (p->len > 0 && ((unsigned char)p->text[p->len] & 0xC0) == 0x80);   /* a whole UTF-8 character */
	p->text[p->len] = '\0';
}

static void delete_word(struct rei_prompt *p)
{
	while (p->len > 0 && p->text[p->len - 1] == ' ')
		p->len--;
	while (p->len > 0 && p->text[p->len - 1] != ' ')
		p->len--;
	p->text[p->len] = '\0';
}

static void run(struct rei_prompt *p)
{
	struct rei_server *s = p->server;
	char *cmd = p->text;
	while (*cmd == ' ' || *cmd == '\t')
		cmd++;
	size_t n = strlen(cmd);
	while (n > 0 && (cmd[n - 1] == ' ' || cmd[n - 1] == '\t'))
		cmd[--n] = '\0';
	char copy[REI_PROMPT_MAX_TEXT + 8];
	snprintf(copy, sizeof copy, "%s", cmd);
	rei_prompt_close(s);
	if (copy[0]) {
		rei_info("run prompt: %s", copy);
		rei_spawn(s, copy);
	}
}

/* Performs one key's effect. Returns true if holding the key should repeat it. */
static bool apply(struct rei_prompt *p, uint32_t code)
{
	struct rei_server *s = p->server;
	const struct rei_xkb *xkb = s->input->xkb;
	xkb_keysym_t sym = rei_xkb_sym(xkb, code);
	uint32_t mods = rei_xkb_mods(xkb);

	switch (sym) {
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		run(p);
		return false;
	case XKB_KEY_Escape:
		rei_prompt_close(s);
		return false;
	case XKB_KEY_BackSpace:
		if (mods & REI_MOD_CTRL)
			delete_word(p);
		else
			backspace(p);
		cursor_solid(p);
		rebuild(p);
		return true;
	default:
		break;
	}

	if (mods & REI_MOD_CTRL) {
		xkb_keysym_t lower = xkb_keysym_to_lower(sym);
		if (lower == XKB_KEY_u) {
			p->len = 0;
			p->text[0] = '\0';
		} else if (lower == XKB_KEY_w) {
			delete_word(p);
		} else {
			return false;
		}
		cursor_solid(p);
		rebuild(p);
		return false;
	}
	if (mods & (REI_MOD_ALT | REI_MOD_LOGO))
		return false;

	char buf[16];
	size_t n = rei_xkb_utf8(xkb, code, buf, sizeof buf);
	/* Printable only: control characters (Tab, ...) are not text. */
	if (n == 0 || (unsigned char)buf[0] < 0x20 || (unsigned char)buf[0] == 0x7f)
		return false;
	append(p, buf, n);
	cursor_solid(p);
	rebuild(p);
	return true;
}

static void on_repeat(evutil_socket_t fd, short what, void *arg)
{
	(void)fd; (void)what;
	struct rei_prompt *p = arg;
	if (!p->open || p->repeat_code == 0)
		return;
	if (!apply(p, p->repeat_code) || !p->open) {
		p->repeat_code = 0;
		return;
	}
	int rate = p->server->config->repeat_rate;
	int ms = rate > 0 ? 1000 / rate : 40;
	struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
	evtimer_add(p->repeat, &tv);
}

void rei_prompt_key(struct rei_server *s, uint32_t code, bool pressed)
{
	struct rei_prompt *p = s->prompt;
	if (!p || !p->open)
		return;
	if (!pressed) {
		if (code == p->repeat_code) {
			p->repeat_code = 0;
			evtimer_del(p->repeat);
		}
		return;
	}
	if (!apply(p, code) || !p->open)
		return;
	/* A held key repeats, as it would in any application. */
	const struct rei_config *cfg = s->config;
	if (cfg->repeat_rate > 0) {
		p->repeat_code = code;
		struct timeval tv = { cfg->repeat_delay / 1000, (cfg->repeat_delay % 1000) * 1000 };
		evtimer_add(p->repeat, &tv);
	}
}

/* ------------------------------------------------------------------------ */

void rei_prompt_emit(struct rei_server *s, struct rei_scene_snapshot *snap)
{
	struct rei_prompt *p = s->prompt;
	if (!p || !p->open || !p->pixbuf)
		return;
	int32_t ow, oh;
	rei_output_logical_size(p->output, &ow, &oh);
	/* Centred on the screen both ways, whatever its height. */
	float x = (float)p->output->x + (float)(ow - p->box_w) / 2.0f;
	float y = (float)p->output->y + (float)(oh - p->box_h) / 2.0f;
	rei_snapshot_add(snap, p->pixbuf, x - (float)p->margin, y - (float)p->margin,
			 (float)(p->box_w + 2 * p->margin), (float)(p->box_h + 2 * p->margin), false);
}

void rei_prompt_finish(struct rei_server *s)
{
	struct rei_prompt *p = s->prompt;
	if (!p)
		return;
	if (p->blink)
		event_free(p->blink);
	if (p->repeat)
		event_free(p->repeat);
	rei_pixbuf_unref(p->pixbuf);
	free(p);
	s->prompt = NULL;
}
