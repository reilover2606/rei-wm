#include "proto/selection.h"

#include <errno.h>
#include <event2/event.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "primary-selection-unstable-v1-server-protocol.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"
#include "util/wlsignal.h"

#include <wayland-server-protocol.h>

/* The plain-text types, best first. */
static const char *const text_mimes[] = {
	"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT",
};

struct rei_selection_source *rei_selection_source_new_text(struct rei_server *server, enum rei_selection_kind kind,
							   const char *text, size_t len, bool mirror)
{
	struct rei_selection_source *src = rei_xcalloc(1, sizeof *src);
	src->server = server;
	src->kind = kind;
	wl_array_init(&src->mime_types);
	wl_signal_init(&src->destroy_signal);
	src->text = rei_xcalloc(len + 1, 1);
	memcpy(src->text, text, len);
	src->text_len = len;
	src->mirror = mirror;
	for (size_t i = 0; i < REI_ARRAY_LEN(text_mimes); i++)
		rei_selection_source_add_mime(src, text_mimes[i]);
	return src;
}

void rei_selection_source_add_mime(struct rei_selection_source *src, const char *mime)
{
	size_t len = strlen(mime) + 1;
	char *dst = wl_array_add(&src->mime_types, len);
	memcpy(dst, mime, len);
	src->n_mimes++;
}

bool rei_selection_source_has_mime(const struct rei_selection_source *src, const char *mime)
{
	const char *m = src->mime_types.data;
	for (size_t i = 0; i < src->n_mimes; i++, m += strlen(m) + 1)
		if (strcmp(m, mime) == 0)
			return true;
	return false;
}

const char *rei_selection_source_text_mime(const struct rei_selection_source *src)
{
	for (size_t i = 0; i < REI_ARRAY_LEN(text_mimes); i++)
		if (rei_selection_source_has_mime(src, text_mimes[i]))
			return text_mimes[i];
	return NULL;
}

/* ---- writing a compositor-owned source's text to a receiver, without ever blocking the event loop ---- */

struct sender {
	int fd;
	char *data;
	size_t len, off;
	struct event *ev;
};

static void sender_free(struct sender *s)
{
	if (s->ev)
		event_free(s->ev);
	close(s->fd);
	free(s->data);
	free(s);
}

static void sender_step(evutil_socket_t fd, short what, void *arg)
{
	(void)fd; (void)what;
	struct sender *s = arg;
	while (s->off < s->len) {
		ssize_t n = write(s->fd, s->data + s->off, s->len - s->off);
		if (n > 0) {
			s->off += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			event_add(s->ev, NULL);
			return;
		}
		break;   /* the receiver went away */
	}
	sender_free(s);
}

void rei_selection_write_async(struct rei_server *server, int fd, const void *data, size_t len)
{
	struct sender *s = rei_xcalloc(1, sizeof *s);
	s->fd = dup(fd);
	if (s->fd < 0) {
		free(s);
		return;
	}
	fcntl(s->fd, F_SETFL, fcntl(s->fd, F_GETFL) | O_NONBLOCK);
	s->data = rei_xcalloc(len + 1, 1);
	if (len)
		memcpy(s->data, data, len);
	s->len = len;
	s->ev = event_new(server->base, s->fd, EV_WRITE, sender_step, s);
	sender_step(s->fd, 0, s);   /* as much as fits at once; the rest when the receiver has read some */
}

static void send_text(struct rei_selection_source *src, int fd)
{
	rei_selection_write_async(src->server, fd, src->text, src->text_len);
}

void rei_selection_source_send(struct rei_selection_source *src, const char *mime, int fd)
{
	if (!rei_selection_source_has_mime(src, mime))
		return;
	if (src->ops) {
		src->ops->send(src, mime, fd);
		return;
	}
	if (!src->resource) {
		send_text(src, fd);
		return;
	}
	if (src->kind == REI_SEL_CLIPBOARD)
		wl_data_source_send_send(src->resource, mime, fd);
	else
		zwp_primary_selection_source_v1_send_send(src->resource, mime, fd);
}

void rei_selection_source_cancel(struct rei_selection_source *src)
{
	if (src->resource) {
		if (src->kind == REI_SEL_CLIPBOARD)
			wl_data_source_send_cancelled(src->resource);
		else
			zwp_primary_selection_source_v1_send_cancelled(src->resource);
	} else if (src->ops) {
		src->ops->cancelled(src);
	} else {
		rei_selection_source_free(src);
	}
}

void rei_selection_source_free(struct rei_selection_source *src)
{
	rei_signal_emit(&src->destroy_signal, src);
	wl_array_release(&src->mime_types);
	free(src->text);
	free(src);
}
