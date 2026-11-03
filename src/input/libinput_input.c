#include "input/libinput_input.h"

#include <event2/event.h>
#include <libinput.h>
#include <libudev.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-protocol.h>

#include "input/input.h"
#include "proto/seat.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"

/* One wheel notch, in the surface-local pixels Wayland clients expect. */
#define WHEEL_STEP_PX 15.0

struct rei_libinput {
	struct rei_server *server;
	struct udev *udev;
	struct libinput *li;
	struct event *event;
	struct rei_libinput_ops ops;
	void *ops_data;
	char *seat;
	int n_keyboards, n_pointers;
};

static int open_restricted(const char *path, int flags, void *data)
{
	struct rei_libinput *li = data;
	int fd = li->ops.open(path, flags, li->ops_data);
	return fd >= 0 ? fd : -1;
}

static void close_restricted(int fd, void *data)
{
	struct rei_libinput *li = data;
	li->ops.close(fd, li->ops_data);
}

static const struct libinput_interface iface = {
	.open_restricted = open_restricted,
	.close_restricted = close_restricted,
};

static uint32_t event_ms(struct libinput_event_pointer *p)
{
	return (uint32_t)(libinput_event_pointer_get_time_usec(p) / 1000);
}

static void update_capabilities(struct rei_libinput *li)
{
	uint32_t caps = 0;
	if (li->n_pointers > 0)
		caps |= WL_SEAT_CAPABILITY_POINTER;
	if (li->n_keyboards > 0)
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	rei_seat_set_capabilities(li->server->seat, caps);
}

static void configure_device(struct rei_libinput *li, struct libinput_device *dev)
{
	const struct rei_config *cfg = li->server->config;
	const char *name = libinput_device_get_name(dev);

	bool kb = libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_KEYBOARD);
	bool ptr = libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_POINTER);
	if (kb)
		li->n_keyboards++;
	if (ptr)
		li->n_pointers++;
	rei_info("input device added: '%s'%s%s", name, kb ? " [keyboard]" : "", ptr ? " [pointer]" : "");

	if (libinput_device_config_tap_get_finger_count(dev) > 0)
		libinput_device_config_tap_set_enabled(dev, cfg->tap_to_click
			? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
	if (libinput_device_config_scroll_has_natural_scroll(dev))
		libinput_device_config_scroll_set_natural_scroll_enabled(dev, cfg->natural_scroll);
	if (libinput_device_config_left_handed_is_available(dev))
		libinput_device_config_left_handed_set(dev, cfg->left_handed);
	if (libinput_device_config_accel_is_available(dev))
		libinput_device_config_accel_set_speed(dev, cfg->pointer_accel);
	update_capabilities(li);
}

/* The bounding box of the whole desktop, for absolute devices (tablets, VMs). */
static void layout_bounds(struct rei_server *s, double *x, double *y, double *w, double *h)
{
	int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
	struct rei_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		int32_t ow, oh;
		rei_output_logical_size(o, &ow, &oh);
		x0 = o->x < x0 ? o->x : x0;
		y0 = o->y < y0 ? o->y : y0;
		x1 = o->x + ow > x1 ? o->x + ow : x1;
		y1 = o->y + oh > y1 ? o->y + oh : y1;
	}
	if (x0 > x1) {
		x0 = y0 = 0;
		x1 = y1 = 1;
	}
	*x = x0;
	*y = y0;
	*w = x1 - x0;
	*h = y1 - y0;
}

static void handle_event(struct rei_libinput *li, struct libinput_event *ev)
{
	struct rei_server *s = li->server;

	switch (libinput_event_get_type(ev)) {
	case LIBINPUT_EVENT_DEVICE_ADDED:
		configure_device(li, libinput_event_get_device(ev));
		break;
	case LIBINPUT_EVENT_DEVICE_REMOVED: {
		struct libinput_device *dev = libinput_event_get_device(ev);
		if (libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_KEYBOARD))
			li->n_keyboards--;
		if (libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_POINTER))
			li->n_pointers--;
		rei_info("input device removed: '%s'", libinput_device_get_name(dev));
		update_capabilities(li);
		break;
	}
	case LIBINPUT_EVENT_KEYBOARD_KEY: {
		struct libinput_event_keyboard *k = libinput_event_get_keyboard_event(ev);
		rei_input_key(s, (uint32_t)(libinput_event_keyboard_get_time_usec(k) / 1000),
			      libinput_event_keyboard_get_key(k),
			      libinput_event_keyboard_get_key_state(k) == LIBINPUT_KEY_STATE_PRESSED);
		break;
	}
	case LIBINPUT_EVENT_POINTER_MOTION: {
		struct libinput_event_pointer *p = libinput_event_get_pointer_event(ev);
		rei_input_pointer_motion(s, event_ms(p), libinput_event_pointer_get_dx(p),
					 libinput_event_pointer_get_dy(p));
		break;
	}
	case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
		struct libinput_event_pointer *p = libinput_event_get_pointer_event(ev);
		double bx, by, bw, bh;
		layout_bounds(s, &bx, &by, &bw, &bh);
		rei_input_pointer_motion_abs(s, event_ms(p),
			bx + libinput_event_pointer_get_absolute_x_transformed(p, (uint32_t)bw),
			by + libinput_event_pointer_get_absolute_y_transformed(p, (uint32_t)bh));
		break;
	}
	case LIBINPUT_EVENT_POINTER_BUTTON: {
		struct libinput_event_pointer *p = libinput_event_get_pointer_event(ev);
		rei_input_pointer_button(s, event_ms(p), libinput_event_pointer_get_button(p),
			libinput_event_pointer_get_button_state(p) == LIBINPUT_BUTTON_STATE_PRESSED);
		break;
	}
	case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
	case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
	case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS: {
		struct libinput_event_pointer *p = libinput_event_get_pointer_event(ev);
		bool wheel = libinput_event_get_type(ev) == LIBINPUT_EVENT_POINTER_SCROLL_WHEEL;
		for (int h = 0; h < 2; h++) {
			enum libinput_pointer_axis axis = h ? LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL
							    : LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL;
			if (!libinput_event_pointer_has_axis(p, axis))
				continue;
			double v = wheel ? libinput_event_pointer_get_scroll_value_v120(p, axis) / 120.0 *
						   WHEEL_STEP_PX
					 : libinput_event_pointer_get_scroll_value(p, axis);
			if (v != 0.0)
				rei_input_pointer_axis(s, event_ms(p), h != 0, v);
		}
		break;
	}
	default:
		break;
	}
}

static void on_readable(evutil_socket_t fd, short what, void *arg)
{
	(void)fd;
	(void)what;
	struct rei_libinput *li = arg;
	if (libinput_dispatch(li->li) < 0) {
		rei_warn("libinput: dispatch failed");
		return;
	}
	struct libinput_event *ev;
	while ((ev = libinput_get_event(li->li))) {
		handle_event(li, ev);
		libinput_event_destroy(ev);
	}
	rei_server_schedule_flush(li->server);
}

struct rei_libinput *rei_libinput_create(struct rei_server *server, const char *seat,
					 const struct rei_libinput_ops *ops, void *data)
{
	struct rei_libinput *li = rei_xcalloc(1, sizeof *li);
	li->server = server;
	li->ops = *ops;
	li->ops_data = data;
	li->seat = rei_xstrdup(seat);

	li->udev = udev_new();
	if (!li->udev) {
		rei_error("libinput: cannot create a udev context");
		goto fail;
	}
	li->li = libinput_udev_create_context(&iface, li, li->udev);
	if (!li->li) {
		rei_error("libinput: cannot create a context");
		goto fail;
	}
	if (libinput_udev_assign_seat(li->li, li->seat) != 0) {
		rei_error("libinput: cannot assign seat '%s'", li->seat);
		goto fail;
	}

	li->event = event_new(server->base, libinput_get_fd(li->li), EV_READ | EV_PERSIST,
			      on_readable, li);
	event_add(li->event, NULL);
	/* The initial DEVICE_ADDED events are already queued; handle them now so the
	 * seat has its capabilities before the first client binds it. */
	on_readable(-1, 0, li);
	if (li->n_keyboards == 0)
		rei_warn("libinput: no keyboard found on seat '%s'; is this user in the 'input' "
			 "group, or is the session missing seat access?", li->seat);
	return li;

fail:
	rei_libinput_destroy(li);
	return NULL;
}

void rei_libinput_destroy(struct rei_libinput *li)
{
	if (!li)
		return;
	if (li->event)
		event_free(li->event);
	if (li->li)
		libinput_unref(li->li);
	if (li->udev)
		udev_unref(li->udev);
	free(li->seat);
	free(li);
}

void rei_libinput_suspend(struct rei_libinput *li)
{
	libinput_suspend(li->li);
	/* Nothing will arrive for the keys that were down; don't leave them stuck. */
	rei_input_release_all(li->server, (uint32_t)(rei_now_ns() / 1000000ull));
}

bool rei_libinput_resume(struct rei_libinput *li)
{
	if (libinput_resume(li->li) != 0) {
		rei_warn("libinput: cannot resume input devices");
		return false;
	}
	on_readable(-1, 0, li);
	return true;
}
