#include "clipboard.h"

#include <errno.h>
#include <event2/event.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "desktop.h"
#include "input/input.h"
#include "proto/data_device.h"
#include "proto/selection.h"
#include "proto/surface.h"
#include "server.h"
#include "tray.h"
#include "util/log.h"
#include "util/util.h"

#define MAX_TEXT ((size_t)8 << 20)
#define FETCH_TIMEOUT_MS 3000

/* A terminal's highlighted text being read (it arrives through a pipe the terminal, or Xwayland for it, writes). */
struct fetch {
	struct rei_server *server;
	int fd;
	struct event *ev;
	char *buf;
	size_t len, cap;
};

struct rei_clipboard {
	struct fetch *pending;
};

static void fetch_free(struct fetch *f)
{
	if (f->ev)
		event_free(f->ev);
	if (f->fd >= 0)
		close(f->fd);
	free(f->buf);
	free(f);
}

static void fetch_done(struct fetch *f, bool ok)
{
	struct rei_server *s = f->server;
	if (s->clipboard && s->clipboard->pending == f)
		s->clipboard->pending = NULL;
	if (ok && f->len > 0) {
		struct rei_selection_source *src = rei_selection_source_new_text(s, REI_SEL_CLIPBOARD, f->buf, f->len, true);
		rei_debug("clipboard: %zu bytes highlighted in a terminal are now the clipboard", f->len);
		rei_data_device_set_selection(s, src);
	}
	fetch_free(f);
}

static void on_readable(evutil_socket_t fd, short what, void *arg)
{
	struct fetch *f = arg;
	if (what & EV_TIMEOUT) {
		rei_debug("clipboard: the terminal did not give its selection in time");
		fetch_done(f, false);
		return;
	}
	for (;;) {
		if (f->len + 4096 > f->cap) {
			f->cap = f->cap ? f->cap * 2 : 8192;
			f->buf = rei_xrealloc(f->buf, f->cap);
		}
		ssize_t n = read(fd, f->buf + f->len, f->cap - f->len);
		if (n > 0) {
			f->len += (size_t)n;
			if (f->len > MAX_TEXT) {
				fetch_done(f, false);
				return;
			}
			continue;
		}
		if (n == 0) {
			fetch_done(f, true);
			return;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			struct timeval tv = { FETCH_TIMEOUT_MS / 1000, (FETCH_TIMEOUT_MS % 1000) * 1000 };
			event_add(f->ev, &tv);
			return;
		}
		fetch_done(f, false);
		return;
	}
}

/* Is the window being typed into a terminal? */
static bool focus_is_terminal(struct rei_server *s)
{
	struct rei_surface *focus = s->input ? s->input->keyboard_focus : NULL;
	if (!focus)
		return false;
	struct rei_view *v = rei_wm_view_for_surface(s, rei_surface_root(focus));
	if (!v)
		return false;
	return rei_app_list_matches(s->config->clipboard_terminals, rei_view_app_id(v));
}

void rei_clipboard_primary_set(struct rei_server *s, struct rei_selection_source *src)
{
	if (!s->config->clipboard_terminal_to_clipboard || src->mirror || !focus_is_terminal(s))
		return;
	const char *mime = rei_selection_source_text_mime(src);
	if (!mime)
		return;
	if (!s->clipboard)
		s->clipboard = rei_xcalloc(1, sizeof *s->clipboard);
	if (s->clipboard->pending) {   /* a newer highlight replaces one still being read */
		fetch_free(s->clipboard->pending);
		s->clipboard->pending = NULL;
	}
	int fds[2];
	if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0)
		return;
	struct fetch *f = rei_xcalloc(1, sizeof *f);
	f->server = s;
	f->fd = fds[0];
	f->ev = event_new(s->base, f->fd, EV_READ | EV_PERSIST, on_readable, f);
	struct timeval tv = { FETCH_TIMEOUT_MS / 1000, (FETCH_TIMEOUT_MS % 1000) * 1000 };
	event_add(f->ev, &tv);
	s->clipboard->pending = f;
	rei_selection_source_send(src, mime, fds[1]);
	close(fds[1]);   /* (the terminal has its own copy of the descriptor now) */
}

void rei_clipboard_finish(struct rei_server *s)
{
	if (!s->clipboard)
		return;
	if (s->clipboard->pending)
		fetch_free(s->clipboard->pending);
	free(s->clipboard);
	s->clipboard = NULL;
}
