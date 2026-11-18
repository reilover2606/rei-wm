#include "xwayland_sel.h"

#include <event2/event.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

#include "clipboard.h"
#include "input/input.h"
#include "proto/data_device.h"
#include "proto/primary_selection.h"
#include "proto/seat.h"
#include "proto/surface.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"
#include "xwayland.h"

#define N_PROPS 8
#define MAX_DATA ((size_t)32 << 20)
#define XFER_TIMEOUT_MS 4000
#define MAX_MAPPINGS 24

enum xfer_kind { XF_TARGETS, XF_DATA };

struct xsel_state;

struct mapping {
	char mime[80];
	xcb_atom_t target;
	bool latin1;   /* the target is STRING: Latin-1 bytes, to be made UTF-8 for a Wayland receiver */
};

/* The data of a source standing for an X11 program's selection. */
struct x_source {
	struct xsel_state *st;
	uint64_t gen;
	struct mapping map[MAX_MAPPINGS];
	int n_map;
};

/* A request we made to the X11 owner (its TARGETS, or one type of its data) that has not been answered yet. */
struct xfer {
	struct rei_xsel *sel;
	struct xsel_state *st;
	enum xfer_kind kind;
	xcb_atom_t target;
	int prop;
	int fd;            /* XF_DATA: the Wayland receiver's pipe (our own copy) */
	bool latin1;
	uint64_t gen;      /* XF_TARGETS: the owner it was asked of */
	bool incr;         /* the owner sends it in pieces (INCR): they are collected in buf */
	char *buf;
	size_t len, cap;
	struct event *timeout;
	struct wl_list link;
};

/* A Wayland source's data being read so it can be given to an X11 requester. */
struct serve {
	struct rei_xsel *sel;
	struct xsel_state *st;
	xcb_window_t requestor;
	xcb_atom_t selection, target, property;
	xcb_timestamp_t time;
	bool latin1;
	int fd;
	struct event *ev, *timeout;
	char *buf;
	size_t len, cap;
	struct wl_list link;
};

struct xsel_state {
	struct rei_xsel *sel;
	enum rei_selection_kind kind;
	xcb_atom_t atom;
	xcb_window_t owner;                     /* the X11 owner (never us), or XCB_NONE */
	uint64_t owner_gen;
	struct rei_selection_source *x_source;  /* standing for the owner, while it is the seat's selection */
	bool we_own;
	struct rei_selection_source *served;    /* the Wayland source we answer requests from */
	struct wl_listener served_destroy;
};

struct rei_xsel {
	struct rei_server *server;
	xcb_connection_t *conn;
	xcb_window_t wm_window, root;
	uint8_t xfixes_event;
	struct xsel_state st[2];                /* indexed by enum rei_selection_kind */
	struct wl_list xfers, serves;
	bool prop_used[N_PROPS];
	xcb_atom_t props[N_PROPS];
	xcb_atom_t a_clipboard, a_targets, a_timestamp, a_multiple, a_utf8, a_text, a_text_plain, a_text_utf8, a_incr;
};

static const struct rei_selection_ops x_ops;

static xcb_atom_t intern(xcb_connection_t *c, const char *name)
{
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, (uint16_t)strlen(name), name), NULL);
	xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
	free(r);
	return a;
}

static bool atom_name(xcb_connection_t *c, xcb_atom_t a, char *out, size_t n)
{
	xcb_get_atom_name_reply_t *r = xcb_get_atom_name_reply(c, xcb_get_atom_name(c, a), NULL);
	if (!r)
		return false;
	int len = xcb_get_atom_name_name_length(r);
	if ((size_t)len >= n)
		len = (int)n - 1;
	memcpy(out, xcb_get_atom_name_name(r), (size_t)len);
	out[len] = '\0';
	free(r);
	return true;
}

static struct rei_xsel *xsel_of(struct rei_server *s)
{
	return rei_xwayland_selection(s);
}

static struct xsel_state *state_for_atom(struct rei_xsel *sel, xcb_atom_t atom)
{
	if (atom == sel->a_clipboard)
		return &sel->st[REI_SEL_CLIPBOARD];
	if (atom == XCB_ATOM_PRIMARY)
		return &sel->st[REI_SEL_PRIMARY];
	return NULL;
}

/* Latin-1 <-> UTF-8, for the STRING target. */
static char *latin1_to_utf8(const char *in, size_t len, size_t *out_len)
{
	char *out = rei_xcalloc(len * 2 + 1, 1);
	size_t o = 0;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)in[i];
		if (c < 0x80) {
			out[o++] = (char)c;
		} else {
			out[o++] = (char)(0xc0 | (c >> 6));
			out[o++] = (char)(0x80 | (c & 0x3f));
		}
	}
	*out_len = o;
	return out;
}

static char *utf8_to_latin1(const char *in, size_t len, size_t *out_len)
{
	char *out = rei_xcalloc(len + 1, 1);
	size_t o = 0;
	for (size_t i = 0; i < len;) {
		unsigned char c = (unsigned char)in[i];
		if (c < 0x80) {
			out[o++] = (char)c;
			i++;
		} else if ((c & 0xe0) == 0xc0 && i + 1 < len) {
			unsigned cp = ((unsigned)(c & 0x1f) << 6) | ((unsigned char)in[i + 1] & 0x3f);
			out[o++] = cp < 256 ? (char)cp : '?';
			i += 2;
		} else {
			out[o++] = '?';   /* anything outside Latin-1: one '?' per character */
			i++;
			while (i < len && ((unsigned char)in[i] & 0xc0) == 0x80)
				i++;
		}
	}
	*out_len = o;
	return out;
}

/* ------------------------------------------------------------------------ */
/* An X11 owner's selection -> Wayland                                      */
/* ------------------------------------------------------------------------ */

static int take_prop(struct rei_xsel *sel)
{
	for (int i = 0; i < N_PROPS; i++)
		if (!sel->prop_used[i]) {
			sel->prop_used[i] = true;
			return i;
		}
	return -1;
}

static void xfer_free(struct xfer *f)
{
	wl_list_remove(&f->link);
	if (f->prop >= 0)
		f->sel->prop_used[f->prop] = false;
	if (f->timeout)
		event_free(f->timeout);
	if (f->fd >= 0)
		close(f->fd);
	free(f->buf);
	free(f);
}

static void on_xfer_timeout(evutil_socket_t fd, short what, void *arg)
{
	(void)fd; (void)what;
	rei_debug("xselection: the X11 program did not answer in time");
	xfer_free(arg);
}

static void xfer_rearm(struct xfer *f)
{
	struct timeval tv = { XFER_TIMEOUT_MS / 1000, (XFER_TIMEOUT_MS % 1000) * 1000 };
	evtimer_add(f->timeout, &tv);
}

static struct xfer *xfer_start(struct xsel_state *st, enum xfer_kind kind, xcb_atom_t target, int fd, bool latin1)
{
	struct rei_xsel *sel = st->sel;
	int prop = take_prop(sel);
	if (prop < 0)
		return NULL;
	struct xfer *f = rei_xcalloc(1, sizeof *f);
	f->sel = sel;
	f->st = st;
	f->kind = kind;
	f->target = target;
	f->prop = prop;
	f->fd = fd;
	f->latin1 = latin1;
	f->gen = st->owner_gen;
	f->timeout = evtimer_new(sel->server->base, on_xfer_timeout, f);
	struct timeval tv = { XFER_TIMEOUT_MS / 1000, (XFER_TIMEOUT_MS % 1000) * 1000 };
	evtimer_add(f->timeout, &tv);
	wl_list_insert(&sel->xfers, &f->link);
	xcb_convert_selection(sel->conn, sel->wm_window, st->atom, target, sel->props[prop], XCB_CURRENT_TIME);
	xcb_flush(sel->conn);
	return f;
}

static void x_source_free(struct rei_selection_source *src)
{
	struct x_source *xs = src->ops_data;
	if (xs->st->x_source == src)
		xs->st->x_source = NULL;
	free(xs);
	rei_selection_source_free(src);
}

static void x_send(struct rei_selection_source *src, const char *mime, int fd)
{
	struct x_source *xs = src->ops_data;
	struct xsel_state *st = xs->st;
	if (st->owner == XCB_NONE || xs->gen != st->owner_gen)
		return;   /* the owner has gone: nothing to give */
	for (int i = 0; i < xs->n_map; i++) {
		if (strcmp(xs->map[i].mime, mime) != 0)
			continue;
		int copy = dup(fd);
		if (copy < 0)
			return;
		if (!xfer_start(st, XF_DATA, xs->map[i].target, copy, xs->map[i].latin1))
			close(copy);
		return;
	}
}

static void x_cancelled(struct rei_selection_source *src)
{
	x_source_free(src);
}

static const struct rei_selection_ops x_ops = { x_send, x_cancelled };

static void add_map(struct x_source *xs, struct rei_selection_source *src, const char *mime, xcb_atom_t target, bool latin1)
{
	if (xs->n_map >= MAX_MAPPINGS || strlen(mime) >= sizeof xs->map[0].mime || rei_selection_source_has_mime(src, mime))
		return;
	snprintf(xs->map[xs->n_map].mime, sizeof xs->map[0].mime, "%s", mime);
	xs->map[xs->n_map].target = target;
	xs->map[xs->n_map].latin1 = latin1;
	xs->n_map++;
	rei_selection_source_add_mime(src, mime);
}

/* The owner's TARGETS arrived: offer what it has to the Wayland side. */
static void targets_arrived(struct xsel_state *st, const xcb_atom_t *atoms, int n)
{
	struct rei_xsel *sel = st->sel;
	bool utf8 = false, str = false, text = false;
	for (int i = 0; i < n; i++) {
		utf8 = utf8 || atoms[i] == sel->a_utf8 || atoms[i] == sel->a_text_utf8;
		str = str || atoms[i] == XCB_ATOM_STRING;
		text = text || atoms[i] == sel->a_text || atoms[i] == sel->a_text_plain;
	}
	struct rei_selection_source *src = rei_selection_source_new_text(sel->server, st->kind, "", 0, false);
	/* new_text offers the text types; start again with none, and make our own list */
	wl_array_release(&src->mime_types);
	wl_array_init(&src->mime_types);
	src->n_mimes = 0;
	free(src->text);
	src->text = NULL;
	src->text_len = 0;
	struct x_source *xs = rei_xcalloc(1, sizeof *xs);
	xs->st = st;
	xs->gen = st->owner_gen;
	if (utf8 || str || text) {
		xcb_atom_t best = utf8 ? sel->a_utf8 : (str ? XCB_ATOM_STRING : sel->a_text);
		bool latin1 = !utf8 && str;
		add_map(xs, src, "text/plain;charset=utf-8", best, latin1);
		add_map(xs, src, "text/plain", best, latin1);
		add_map(xs, src, "UTF8_STRING", best, latin1);
		add_map(xs, src, "STRING", best, latin1);
		add_map(xs, src, "TEXT", best, latin1);
	}
	for (int i = 0; i < n; i++) {
		char name[80];
		if (atoms[i] == XCB_NONE || !atom_name(sel->conn, atoms[i], name, sizeof name))
			continue;
		if (strchr(name, '/') && strncmp(name, "text/plain", 10) != 0)
			add_map(xs, src, name, atoms[i], false);
	}
	if (xs->n_map == 0) {
		free(xs);
		wl_array_release(&src->mime_types);
		free(src);
		return;
	}
	src->ops = &x_ops;
	src->ops_data = xs;
	st->x_source = src;
	rei_debug("xselection: an X11 program owns the %s (%d types)", st->kind == REI_SEL_CLIPBOARD ? "clipboard" : "primary selection",
		  xs->n_map);
	if (st->kind == REI_SEL_CLIPBOARD) {
		rei_data_device_set_selection(sel->server, src);
	} else {
		rei_primary_selection_set(sel->server, src);
		rei_clipboard_primary_set(sel->server, src);
	}
}

static bool focus_is_x11(struct rei_server *s)
{
	struct rei_surface *focus = s->input ? s->input->keyboard_focus : NULL;
	return focus && rei_xwayland_view_of(rei_surface_root(focus)) != NULL;
}

/* Asks the owner what it can give. */
static void adopt(struct xsel_state *st)
{
	if (st->owner == XCB_NONE || st->we_own)
		return;
	struct rei_selection_source *current = st->kind == REI_SEL_CLIPBOARD ? st->sel->server->seat->selection
									      : st->sel->server->seat->primary;
	if (st->x_source && st->x_source == current && ((struct x_source *)st->x_source->ops_data)->gen == st->owner_gen)
		return;   /* this owner's selection is already offered */
	xfer_start(st, XF_TARGETS, st->sel->a_targets, -1, false);
}

static void on_fixes_notify(struct rei_xsel *sel, const xcb_xfixes_selection_notify_event_t *e)
{
	struct xsel_state *st = state_for_atom(sel, e->selection);
	if (!st)
		return;
	if (e->owner == sel->wm_window)
		return;   /* us: the Wayland side's selection */
	st->owner = e->owner;
	st->owner_gen++;
	if (e->owner == XCB_NONE) {
		if (st->x_source) {   /* the selection is gone: so is what stood for it */
			if (st->kind == REI_SEL_CLIPBOARD)
				rei_data_device_set_selection(sel->server, NULL);
			else
				rei_primary_selection_set(sel->server, NULL);
		}
		return;
	}
	if (focus_is_x11(sel->server))
		adopt(st);
}

static struct xfer *find_xfer(struct rei_xsel *sel, xcb_atom_t selection, xcb_atom_t target)
{
	struct xfer *f;
	wl_list_for_each(f, &sel->xfers, link)
		if (f->st->atom == selection && f->target == target)
			return f;
	return NULL;
}

/* A piece of an INCR transfer has arrived (the owner put it in our property): collect it; an empty one ends it. */
static void on_property_notify(struct rei_xsel *sel, const xcb_property_notify_event_t *e)
{
	if (e->window != sel->wm_window || e->state != XCB_PROPERTY_NEW_VALUE)
		return;
	struct xfer *f;
	wl_list_for_each(f, &sel->xfers, link)
		if (f->incr && sel->props[f->prop] == e->atom)
			goto found;
	return;
found:;
	xcb_get_property_reply_t *r = xcb_get_property_reply(
		sel->conn, xcb_get_property(sel->conn, 1, sel->wm_window, e->atom, XCB_GET_PROPERTY_TYPE_ANY, 0,
					    (uint32_t)(MAX_DATA / 4)), NULL);
	if (!r) {
		xfer_free(f);
		return;
	}
	size_t n = (size_t)xcb_get_property_value_length(r);
	if (n == 0) {   /* the end */
		if (f->fd >= 0 && f->len > 0) {
			if (f->latin1) {
				size_t m;
				char *u = latin1_to_utf8(f->buf, f->len, &m);
				rei_selection_write_async(sel->server, f->fd, u, m);
				free(u);
			} else {
				rei_selection_write_async(sel->server, f->fd, f->buf, f->len);
			}
		}
		free(r);
		xfer_free(f);
		return;
	}
	if (f->len + n > MAX_DATA) {
		rei_debug("xselection: a selection of more than %zu MiB is not transferred", MAX_DATA >> 20);
		free(r);
		xfer_free(f);
		return;
	}
	if (f->len + n > f->cap) {
		f->cap = (f->len + n) * 2;
		f->buf = rei_xrealloc(f->buf, f->cap);
	}
	memcpy(f->buf + f->len, xcb_get_property_value(r), n);
	f->len += n;
	xfer_rearm(f);
	free(r);
}

static void on_selection_notify(struct rei_xsel *sel, const xcb_selection_notify_event_t *e)
{
	struct xfer *f = find_xfer(sel, e->selection, e->target);
	if (!f)
		return;
	if (e->property == XCB_NONE) {   /* the owner refuses (or no longer has it) */
		xfer_free(f);
		return;
	}
	xcb_get_property_reply_t *r = xcb_get_property_reply(
		sel->conn, xcb_get_property(sel->conn, 1, sel->wm_window, e->property, XCB_GET_PROPERTY_TYPE_ANY, 0,
					    (uint32_t)(MAX_DATA / 4)), NULL);
	if (r && r->type == sel->a_incr && f->kind == XF_DATA) {   /* in pieces: each one arrives as a property change */
		f->incr = true;
		xfer_rearm(f);
		free(r);
		return;
	}
	if (!r || r->type == XCB_NONE || r->type == sel->a_incr) {
		free(r);
		xfer_free(f);
		return;
	}
	const char *value = xcb_get_property_value(r);
	size_t len = (size_t)xcb_get_property_value_length(r);   /* (already in bytes) */
	if (f->kind == XF_TARGETS) {
		if (f->gen == f->st->owner_gen && r->format == 32 && focus_is_x11(sel->server))
			targets_arrived(f->st, (const xcb_atom_t *)value, (int)(len / 4));
	} else if (f->fd >= 0) {
		if (f->latin1) {
			size_t n;
			char *u = latin1_to_utf8(value, len, &n);
			rei_selection_write_async(sel->server, f->fd, u, n);
			free(u);
		} else {
			rei_selection_write_async(sel->server, f->fd, value, len);
		}
	}
	free(r);
	xfer_free(f);
}

/* ------------------------------------------------------------------------ */
/* A Wayland selection -> X11 programs                                      */
/* ------------------------------------------------------------------------ */

static void send_notify(struct rei_xsel *sel, xcb_window_t requestor, xcb_atom_t selection, xcb_atom_t target,
			xcb_atom_t property, xcb_timestamp_t time)
{
	xcb_selection_notify_event_t ev = { .response_type = XCB_SELECTION_NOTIFY, .time = time, .requestor = requestor,
					    .selection = selection, .target = target, .property = property };
	xcb_send_event(sel->conn, 0, requestor, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
	xcb_flush(sel->conn);
}

static void serve_free(struct serve *sv)
{
	wl_list_remove(&sv->link);
	if (sv->ev)
		event_free(sv->ev);
	if (sv->timeout)
		event_free(sv->timeout);
	if (sv->fd >= 0)
		close(sv->fd);
	free(sv->buf);
	free(sv);
}

static void serve_fail(struct serve *sv)
{
	send_notify(sv->sel, sv->requestor, sv->selection, sv->target, XCB_NONE, sv->time);
	serve_free(sv);
}

static void serve_finish(struct serve *sv)
{
	struct rei_xsel *sel = sv->sel;
	size_t len = sv->len;
	char *data = sv->buf;
	char *converted = NULL;
	if (sv->latin1) {
		converted = utf8_to_latin1(sv->buf, sv->len, &len);
		data = converted;
	}
	uint32_t max = xcb_get_maximum_request_length(sel->conn) * 4;
	if (max > 100 && len > max - 100) {   /* would need INCR */
		rei_debug("xselection: %zu bytes are too large to hand to an X11 program in one piece", len);
		free(converted);
		serve_fail(sv);
		return;
	}
	xcb_atom_t type = sv->target == XCB_ATOM_STRING ? XCB_ATOM_STRING : sel->a_utf8;
	if (sv->target != XCB_ATOM_STRING && sv->target != sel->a_utf8 && sv->target != sel->a_text)
		type = sv->target;   /* a MIME type: its data under its own name */
	xcb_change_property(sel->conn, XCB_PROP_MODE_REPLACE, sv->requestor, sv->property, type, 8, (uint32_t)len, data);
	free(converted);
	send_notify(sel, sv->requestor, sv->selection, sv->target, sv->property, sv->time);
	serve_free(sv);
}

static void on_serve_readable(evutil_socket_t fd, short what, void *arg)
{
	struct serve *sv = arg;
	if (what & EV_TIMEOUT) {
		serve_fail(sv);
		return;
	}
	for (;;) {
		if (sv->len + 4096 > sv->cap) {
			sv->cap = sv->cap ? sv->cap * 2 : 8192;
			sv->buf = rei_xrealloc(sv->buf, sv->cap);
		}
		ssize_t n = read(fd, sv->buf + sv->len, sv->cap - sv->len);
		if (n > 0) {
			sv->len += (size_t)n;
			if (sv->len > MAX_DATA) {
				serve_fail(sv);
				return;
			}
			continue;
		}
		if (n == 0) {
			serve_finish(sv);
			return;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			struct timeval tv = { XFER_TIMEOUT_MS / 1000, (XFER_TIMEOUT_MS % 1000) * 1000 };
			event_add(sv->ev, &tv);
			return;
		}
		serve_fail(sv);
		return;
	}
}

static void on_selection_request(struct rei_xsel *sel, const xcb_selection_request_event_t *e)
{
	struct xsel_state *st = state_for_atom(sel, e->selection);
	xcb_atom_t property = e->property ? e->property : e->target;
	if (!st || !st->we_own || !st->served) {
		rei_debug("xselection: a request for %s is refused (we_own %d, source %d)", st ? (st->kind == REI_SEL_CLIPBOARD ? "the clipboard" : "the primary selection") : "?",
			  st ? st->we_own : 0, st ? st->served != NULL : 0);
		send_notify(sel, e->requestor, e->selection, e->target, XCB_NONE, e->time);
		return;
	}
	struct rei_selection_source *src = st->served;
	const char *text_mime = rei_selection_source_text_mime(src);

	if (e->target == sel->a_targets) {
		xcb_atom_t list[MAX_MAPPINGS + 8];
		int n = 0;
		list[n++] = sel->a_targets;
		list[n++] = sel->a_timestamp;
		if (text_mime) {
			list[n++] = sel->a_utf8;
			list[n++] = XCB_ATOM_STRING;
			list[n++] = sel->a_text;
			list[n++] = sel->a_text_utf8;
			list[n++] = sel->a_text_plain;
		}
		const char *m = src->mime_types.data;
		for (size_t i = 0; i < src->n_mimes && n < MAX_MAPPINGS + 8; i++, m += strlen(m) + 1)
			if (strchr(m, '/') && strncmp(m, "text/plain", 10) != 0) {
				xcb_atom_t a = intern(sel->conn, m);
				if (a != XCB_NONE)
					list[n++] = a;
			}
		xcb_change_property(sel->conn, XCB_PROP_MODE_REPLACE, e->requestor, property, XCB_ATOM_ATOM, 32, (uint32_t)n, list);
		send_notify(sel, e->requestor, e->selection, e->target, property, e->time);
		return;
	}
	if (e->target == sel->a_timestamp) {
		uint32_t t = 0;
		xcb_change_property(sel->conn, XCB_PROP_MODE_REPLACE, e->requestor, property, XCB_ATOM_INTEGER, 32, 1, &t);
		send_notify(sel, e->requestor, e->selection, e->target, property, e->time);
		return;
	}

	const char *mime = NULL;
	bool latin1 = false;
	if (e->target == sel->a_utf8 || e->target == sel->a_text || e->target == sel->a_text_utf8 ||
	    e->target == sel->a_text_plain || e->target == XCB_ATOM_STRING) {
		mime = text_mime;
		latin1 = e->target == XCB_ATOM_STRING;
	} else {
		char name[80];
		if (atom_name(sel->conn, e->target, name, sizeof name) && rei_selection_source_has_mime(src, name)) {
			static char keep[80];   /* (used at once, below) */
			snprintf(keep, sizeof keep, "%s", name);
			mime = keep;
		}
	}
	if (!mime) {
		send_notify(sel, e->requestor, e->selection, e->target, XCB_NONE, e->time);
		return;
	}
	int fds[2];
	if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
		send_notify(sel, e->requestor, e->selection, e->target, XCB_NONE, e->time);
		return;
	}
	struct serve *sv = rei_xcalloc(1, sizeof *sv);
	sv->sel = sel;
	sv->st = st;
	sv->requestor = e->requestor;
	sv->selection = e->selection;
	sv->target = e->target;
	sv->property = property;
	sv->time = e->time;
	sv->latin1 = latin1;
	sv->fd = fds[0];
	sv->ev = event_new(sel->server->base, sv->fd, EV_READ | EV_PERSIST, on_serve_readable, sv);
	struct timeval tv = { XFER_TIMEOUT_MS / 1000, (XFER_TIMEOUT_MS % 1000) * 1000 };
	event_add(sv->ev, &tv);
	wl_list_insert(&sel->serves, &sv->link);
	rei_selection_source_send(src, mime, fds[1]);
	close(fds[1]);
}

static void on_selection_clear(struct rei_xsel *sel, const xcb_selection_clear_event_t *e)
{
	struct xsel_state *st = state_for_atom(sel, e->selection);
	if (!st || !st->we_own)
		return;
	st->we_own = false;
	if (st->served) {
		wl_list_remove(&st->served_destroy.link);
		wl_list_init(&st->served_destroy.link);
		st->served = NULL;
	}
}

static void on_served_destroyed(struct wl_listener *l, void *data)
{
	(void)data;
	struct xsel_state *st = wl_container_of(l, st, served_destroy);
	wl_list_remove(&st->served_destroy.link);
	wl_list_init(&st->served_destroy.link);
	/* We still own the X11 selection, but have nothing to answer from until the Wayland side says what replaces it
	 * (rei_xsel_wayland_changed follows at once). Giving the ownership up here would send us a SelectionClear
	 * that arrives after we have taken it again. */
	st->served = NULL;
}

void rei_xsel_wayland_changed(struct rei_server *server, enum rei_selection_kind kind)
{
	struct rei_xsel *sel = xsel_of(server);
	if (!sel)
		return;
	struct xsel_state *st = &sel->st[kind];
	struct rei_selection_source *src = kind == REI_SEL_CLIPBOARD ? server->seat->selection : server->seat->primary;
	if (st->served) {
		wl_list_remove(&st->served_destroy.link);
		wl_list_init(&st->served_destroy.link);
		st->served = NULL;
	}
	if (src && src->ops == &x_ops)
		return;   /* an X11 program's own selection: it keeps the X11 side */
	if (src) {
		xcb_set_selection_owner(sel->conn, sel->wm_window, st->atom, XCB_CURRENT_TIME);
		xcb_flush(sel->conn);
		st->we_own = true;
		st->owner = XCB_NONE;
		st->served = src;
		st->served_destroy.notify = on_served_destroyed;
		wl_signal_add(&src->destroy_signal, &st->served_destroy);
	} else if (st->we_own) {
		st->we_own = false;
		xcb_set_selection_owner(sel->conn, XCB_NONE, st->atom, XCB_CURRENT_TIME);
		xcb_flush(sel->conn);
	}
}

void rei_xsel_focus_changed(struct rei_server *server)
{
	struct rei_xsel *sel = xsel_of(server);
	if (!sel || !focus_is_x11(server))
		return;
	for (int k = 0; k < 2; k++)
		adopt(&sel->st[k]);
}

/* ------------------------------------------------------------------------ */

bool rei_xsel_handle_event(struct rei_xsel *sel, xcb_generic_event_t *ev)
{
	uint8_t type = ev->response_type & ~0x80;
	if (type == sel->xfixes_event + XCB_XFIXES_SELECTION_NOTIFY) {
		on_fixes_notify(sel, (const xcb_xfixes_selection_notify_event_t *)ev);
		return true;
	}
	switch (type) {
	case XCB_PROPERTY_NOTIFY:
		on_property_notify(sel, (const xcb_property_notify_event_t *)ev);
		return false;   /* (the window manager's own handling of property changes goes on) */
	case XCB_SELECTION_NOTIFY:
		on_selection_notify(sel, (const xcb_selection_notify_event_t *)ev);
		return true;
	case XCB_SELECTION_REQUEST:
		on_selection_request(sel, (const xcb_selection_request_event_t *)ev);
		return true;
	case XCB_SELECTION_CLEAR:
		on_selection_clear(sel, (const xcb_selection_clear_event_t *)ev);
		return true;
	default:
		return false;
	}
}

struct rei_xsel *rei_xsel_create(struct rei_server *server, xcb_connection_t *conn, xcb_window_t wm_window,
				 xcb_window_t root)
{
	const xcb_query_extension_reply_t *ext = xcb_get_extension_data(conn, &xcb_xfixes_id);
	if (!ext || !ext->present) {
		rei_warn("xselection: the X server has no XFixes extension: X11 programs cannot copy to or paste from others");
		return NULL;
	}
	xcb_discard_reply(conn, xcb_xfixes_query_version(conn, 5, 0).sequence);
	struct rei_xsel *sel = rei_xcalloc(1, sizeof *sel);
	sel->server = server;
	sel->conn = conn;
	sel->wm_window = wm_window;
	sel->root = root;
	sel->xfixes_event = ext->first_event;
	wl_list_init(&sel->xfers);
	wl_list_init(&sel->serves);
	sel->a_clipboard = intern(conn, "CLIPBOARD");
	sel->a_targets = intern(conn, "TARGETS");
	sel->a_timestamp = intern(conn, "TIMESTAMP");
	sel->a_multiple = intern(conn, "MULTIPLE");
	sel->a_utf8 = intern(conn, "UTF8_STRING");
	sel->a_text = intern(conn, "TEXT");
	sel->a_text_plain = intern(conn, "text/plain");
	sel->a_text_utf8 = intern(conn, "text/plain;charset=utf-8");
	sel->a_incr = intern(conn, "INCR");
	for (int i = 0; i < N_PROPS; i++) {
		char name[24];
		snprintf(name, sizeof name, "_REI_SELECTION_%d", i);
		sel->props[i] = intern(conn, name);
	}
	for (int k = 0; k < 2; k++) {
		struct xsel_state *st = &sel->st[k];
		st->sel = sel;
		st->kind = (enum rei_selection_kind)k;
		st->atom = k == REI_SEL_CLIPBOARD ? sel->a_clipboard : XCB_ATOM_PRIMARY;
		wl_list_init(&st->served_destroy.link);
		xcb_xfixes_select_selection_input(conn, root, st->atom,
						  XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
						  XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
						  XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE);
	}
	uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;   /* (the pieces of an INCR transfer arrive as property changes) */
	xcb_change_window_attributes(conn, wm_window, XCB_CW_EVENT_MASK, &mask);
	xcb_flush(conn);
	return sel;
}

void rei_xsel_destroy(struct rei_xsel *sel)
{
	if (!sel)
		return;
	struct xfer *f, *ft;
	wl_list_for_each_safe(f, ft, &sel->xfers, link)
		xfer_free(f);
	struct serve *sv, *st2;
	wl_list_for_each_safe(sv, st2, &sel->serves, link)
		serve_free(sv);
	for (int k = 0; k < 2; k++) {
		struct xsel_state *st = &sel->st[k];
		if (st->served) {
			wl_list_remove(&st->served_destroy.link);
			st->served = NULL;
		}
		if (st->x_source) {   /* the seat still points at it: take it away before it is freed */
			if (k == REI_SEL_CLIPBOARD)
				rei_data_device_set_selection(sel->server, NULL);
			else
				rei_primary_selection_set(sel->server, NULL);
		}
	}
	free(sel);
}
