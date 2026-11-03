#include "backend/drm/session.h"

#include <errno.h>
#include <event2/event.h>
#include <libseat.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/log.h"
#include "util/util.h"

struct rei_session {
	struct libseat *seat;
	struct event *event;
	struct rei_session_callbacks cb;
	void *data;
	bool active;
	bool destroying;
};

static void seat_enable(struct libseat *seat, void *userdata)
{
	(void)seat;
	struct rei_session *s = userdata;
	s->active = true;
	rei_info("session: seat enabled");
	if (s->cb.enable)
		s->cb.enable(s->data);
}

static void seat_disable(struct libseat *seat, void *userdata)
{
	struct rei_session *s = userdata;
	s->active = false;
	rei_info("session: seat disabled (another session has the display)");
	if (s->cb.disable)
		s->cb.disable(s->data);
	/* Acknowledge, so the system can hand the devices to whoever is next. */
	libseat_disable_seat(seat);
}

static const struct libseat_seat_listener seat_listener = {
	.enable_seat = seat_enable,
	.disable_seat = seat_disable,
};

static void log_handler(enum libseat_log_level level, const char *fmt, va_list args)
{
	char buf[512];
	vsnprintf(buf, sizeof buf, fmt, args);
	switch (level) {
	case LIBSEAT_LOG_LEVEL_ERROR: rei_error("libseat: %s", buf); break;
	case LIBSEAT_LOG_LEVEL_INFO: rei_info("libseat: %s", buf); break;
	default: rei_debug("libseat: %s", buf); break;
	}
}

static void on_seat_readable(evutil_socket_t fd, short what, void *arg)
{
	(void)fd;
	(void)what;
	struct rei_session *s = arg;
	if (libseat_dispatch(s->seat, 0) < 0 && errno != EAGAIN)
		rei_error("session: libseat_dispatch failed: %s", strerror(errno));
}

struct rei_session *rei_session_create(struct event_base *base,
				       const struct rei_session_callbacks *cb, void *data)
{
	libseat_set_log_handler(log_handler);
	libseat_set_log_level(LIBSEAT_LOG_LEVEL_INFO);

	struct rei_session *s = rei_xcalloc(1, sizeof *s);
	s->cb = *cb;
	s->data = data;
	s->seat = libseat_open_seat(&seat_listener, s);
	if (!s->seat) {
		rei_error("session: cannot open a seat (%s). Run from a text console on a "
			  "system with logind or seatd, or as root.", strerror(errno));
		free(s);
		return NULL;
	}

	/* The first enable arrives shortly after opening; wait for it. */
	for (int waited = 0; !s->active && waited < 5000; waited += 100) {
		if (libseat_dispatch(s->seat, 100) < 0) {
			rei_error("session: libseat_dispatch failed: %s", strerror(errno));
			break;
		}
	}
	if (!s->active) {
		rei_error("session: the seat never became active. Is this session on the "
			  "foreground VT, and is another compositor already running?");
		libseat_close_seat(s->seat);
		free(s);
		return NULL;
	}

	int fd = libseat_get_fd(s->seat);
	s->event = event_new(base, fd, EV_READ | EV_PERSIST, on_seat_readable, s);
	event_add(s->event, NULL);
	return s;
}

void rei_session_destroy(struct rei_session *s)
{
	if (!s)
		return;
	s->destroying = true;
	if (s->event)
		event_free(s->event);
	libseat_close_seat(s->seat);
	free(s);
}

bool rei_session_active(const struct rei_session *s)
{
	return s->active;
}

const char *rei_session_seat_name(const struct rei_session *s)
{
	const char *name = libseat_seat_name(s->seat);
	return name ? name : "seat0";
}

int rei_session_open(struct rei_session *s, const char *path, int *device_id)
{
	int fd = -1;
	int id = libseat_open_device(s->seat, path, &fd);
	if (id < 0) {
		rei_error("session: cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	*device_id = id;
	return fd;
}

void rei_session_close(struct rei_session *s, int device_id, int fd)
{
	libseat_close_device(s->seat, device_id);
	if (fd >= 0)
		close(fd);
}

bool rei_session_switch_vt(struct rei_session *s, unsigned vt)
{
	if (libseat_switch_session(s->seat, (int)vt) < 0) {
		rei_warn("session: cannot switch to VT %u: %s", vt, strerror(errno));
		return false;
	}
	return true;
}
