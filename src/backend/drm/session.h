#ifndef REI_DRM_SESSION_H
#define REI_DRM_SESSION_H

#include <stdbool.h>

struct event_base;
struct rei_session;

/*
 * A login session on a seat, through libseat (which talks to seatd or
 * systemd-logind). It is what lets an ordinary user open the GPU and input
 * devices, and hands them back when the user switches to another VT.
 */
struct rei_session_callbacks {
	/* The seat is ours (again): device fds work. */
	void (*enable)(void *data);
	/* The seat was taken away (VT switch): stop using device fds. The session
	 * acknowledges this itself once the callback returns. */
	void (*disable)(void *data);
};

/* Opens the seat and waits briefly for it to become active. NULL on failure,
 * with the reason logged. */
struct rei_session *rei_session_create(struct event_base *base,
				       const struct rei_session_callbacks *cb, void *data);
void rei_session_destroy(struct rei_session *s);

bool rei_session_active(const struct rei_session *s);
const char *rei_session_seat_name(const struct rei_session *s);

/* Opens a device node. Returns an fd (>= 0) and a device id for closing, or -1. */
int rei_session_open(struct rei_session *s, const char *path, int *device_id);
void rei_session_close(struct rei_session *s, int device_id, int fd);

/* Asks the system to switch to virtual terminal `vt` (1-12). */
bool rei_session_switch_vt(struct rei_session *s, unsigned vt);

#endif
