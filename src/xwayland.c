#include "xwayland.h"

#include <errno.h>
#include <event2/event.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <sys/uio.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>

#include "desktop.h"
#include "proto/surface.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"

/* ------------------------------------------------------------------------ */
/* Types                                                                    */
/* ------------------------------------------------------------------------ */

struct atoms {
	xcb_atom_t wm_protocols, wm_delete_window, wm_take_focus, wm_state, wm_change_state;
	xcb_atom_t wm_normal_hints, wm_transient_for, wm_class, wm_name;
	xcb_atom_t net_wm_name, utf8_string;
	xcb_atom_t net_wm_state, net_wm_state_fullscreen;
	xcb_atom_t net_wm_window_type, type_dialog, type_utility, type_splash, type_toolbar, type_menu;
	xcb_atom_t net_active_window, net_supported, net_supporting_wm_check, net_close_window;
	xcb_atom_t wl_surface_serial;
};

struct rei_xwindow {
	struct wl_list link;  /* rei_xwayland.windows */
	struct rei_xwayland *xw;
	xcb_window_t id;
	bool override_redirect;
	int32_t x, y, w, h;     /* As X11 knows them (global coordinates). */

	bool mapped_x;          /* Mapped on the X11 side. */
	bool mapped_wl;         /* We are showing it. */
	bool has_serial;
	uint64_t serial;
	struct rei_xsurface *xsurface;

	struct rei_view *view;  /* Managed windows, while shown. */
	bool activated;
	bool fullscreen;

	char *title;
	int32_t min_w, min_h, max_w, max_h;
	xcb_window_t transient_for;
	bool dialog_type;
	bool supports_delete, supports_take_focus;
};

struct rei_xsurface {
	struct wl_list link;  /* rei_xwayland.surfaces */
	struct rei_server *server;
	struct rei_surface *surface;  /* NULL once the wl_surface is gone */
	struct wl_listener surface_destroy;
	bool has_serial;
	uint64_t serial;
	struct rei_xwindow *window;
};

struct rei_xwayland {
	struct rei_server *server;
	pid_t pid;
	int display;
	char display_name[16];
	char lock_path[64], socket_path[64];
	int listen_fd[2];
	int wm_fd;           /* Our end of the window manager socket until xcb takes it. */
	int ready_fd;        /* Xwayland writes the display number here when it is up. */
	struct event *ready_event, *xcb_event, *child_event;

	xcb_connection_t *conn;
	xcb_screen_t *screen;
	xcb_window_t wm_window;
	struct atoms atoms;
	struct wl_list windows;   /* rei_xwindow */
	struct wl_list surfaces;  /* rei_xsurface */
	xcb_window_t focused;
	bool running;
};

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static struct rei_xwayland *X(struct rei_server *s) { return s->xwayland; }

static xcb_atom_t intern_reply(xcb_connection_t *c, xcb_intern_atom_cookie_t ck)
{
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, ck, NULL);
	xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
	free(r);
	return a;
}

static void load_atoms(struct rei_xwayland *x)
{
	static const struct { const char *name; size_t off; } names[] = {
#define A(field, str) { str, offsetof(struct atoms, field) }
		A(wm_protocols, "WM_PROTOCOLS"), A(wm_delete_window, "WM_DELETE_WINDOW"),
		A(wm_take_focus, "WM_TAKE_FOCUS"), A(wm_state, "WM_STATE"),
		A(wm_change_state, "WM_CHANGE_STATE"), A(wm_normal_hints, "WM_NORMAL_HINTS"),
		A(wm_transient_for, "WM_TRANSIENT_FOR"), A(wm_class, "WM_CLASS"), A(wm_name, "WM_NAME"),
		A(net_wm_name, "_NET_WM_NAME"), A(utf8_string, "UTF8_STRING"),
		A(net_wm_state, "_NET_WM_STATE"), A(net_wm_state_fullscreen, "_NET_WM_STATE_FULLSCREEN"),
		A(net_wm_window_type, "_NET_WM_WINDOW_TYPE"),
		A(type_dialog, "_NET_WM_WINDOW_TYPE_DIALOG"), A(type_utility, "_NET_WM_WINDOW_TYPE_UTILITY"),
		A(type_splash, "_NET_WM_WINDOW_TYPE_SPLASH"), A(type_toolbar, "_NET_WM_WINDOW_TYPE_TOOLBAR"),
		A(type_menu, "_NET_WM_WINDOW_TYPE_MENU"),
		A(net_active_window, "_NET_ACTIVE_WINDOW"), A(net_supported, "_NET_SUPPORTED"),
		A(net_supporting_wm_check, "_NET_SUPPORTING_WM_CHECK"), A(net_close_window, "_NET_CLOSE_WINDOW"),
		A(wl_surface_serial, "WL_SURFACE_SERIAL"),
#undef A
	};
	enum { N = sizeof names / sizeof names[0] };
	xcb_intern_atom_cookie_t cookies[N];
	for (size_t i = 0; i < N; i++)
		cookies[i] = xcb_intern_atom(x->conn, 0, (uint16_t)strlen(names[i].name), names[i].name);
	for (size_t i = 0; i < N; i++)
		*(xcb_atom_t *)((char *)&x->atoms + names[i].off) = intern_reply(x->conn, cookies[i]);
}

static xcb_get_property_reply_t *get_prop(struct rei_xwayland *x, xcb_window_t w, xcb_atom_t prop,
					  xcb_atom_t type, uint32_t len)
{
	xcb_get_property_cookie_t ck = xcb_get_property(x->conn, 0, w, prop, type, 0, len);
	return xcb_get_property_reply(x->conn, ck, NULL);
}

static struct rei_xwindow *find_window(struct rei_xwayland *x, xcb_window_t id)
{
	struct rei_xwindow *w;
	wl_list_for_each(w, &x->windows, link)
		if (w->id == id)
			return w;
	return NULL;
}

/* ------------------------------------------------------------------------ */
/* Window properties                                                        */
/* ------------------------------------------------------------------------ */

static void read_title(struct rei_xwayland *x, struct rei_xwindow *w)
{
	free(w->title);
	w->title = NULL;
	xcb_get_property_reply_t *r = get_prop(x, w->id, x->atoms.net_wm_name, x->atoms.utf8_string, 256);
	if (!r || xcb_get_property_value_length(r) == 0) {
		free(r);
		r = get_prop(x, w->id, x->atoms.wm_name, XCB_ATOM_ANY, 256);
	}
	if (r && xcb_get_property_value_length(r) > 0)
		w->title = rei_xstrndup(xcb_get_property_value(r), (size_t)xcb_get_property_value_length(r));
	free(r);
}

static void read_hints(struct rei_xwayland *x, struct rei_xwindow *w)
{
	/* WM_NORMAL_HINTS: flags, 4 obsolete words, min w/h, max w/h, ... */
	w->min_w = w->min_h = w->max_w = w->max_h = 0;
	xcb_get_property_reply_t *r = get_prop(x, w->id, x->atoms.wm_normal_hints, XCB_ATOM_ANY, 18);
	if (r && xcb_get_property_value_length(r) >= 9 * 4) {
		const uint32_t *v = xcb_get_property_value(r);
		if (v[0] & 16) {  /* PMinSize */
			w->min_w = (int32_t)v[5];
			w->min_h = (int32_t)v[6];
		}
		if (v[0] & 32) {  /* PMaxSize */
			w->max_w = (int32_t)v[7];
			w->max_h = (int32_t)v[8];
		}
	}
	free(r);

	w->transient_for = 0;
	r = get_prop(x, w->id, x->atoms.wm_transient_for, XCB_ATOM_WINDOW, 1);
	if (r && xcb_get_property_value_length(r) >= 4)
		w->transient_for = *(const xcb_window_t *)xcb_get_property_value(r);
	free(r);

	w->dialog_type = false;
	r = get_prop(x, w->id, x->atoms.net_wm_window_type, XCB_ATOM_ATOM, 16);
	if (r) {
		const xcb_atom_t *t = xcb_get_property_value(r);
		int n = xcb_get_property_value_length(r) / 4;
		for (int i = 0; i < n; i++) {
			if (t[i] == x->atoms.type_dialog || t[i] == x->atoms.type_utility ||
			    t[i] == x->atoms.type_splash || t[i] == x->atoms.type_toolbar ||
			    t[i] == x->atoms.type_menu)
				w->dialog_type = true;
		}
	}
	free(r);

	w->supports_delete = w->supports_take_focus = false;
	r = get_prop(x, w->id, x->atoms.wm_protocols, XCB_ATOM_ATOM, 32);
	if (r) {
		const xcb_atom_t *p = xcb_get_property_value(r);
		int n = xcb_get_property_value_length(r) / 4;
		for (int i = 0; i < n; i++) {
			w->supports_delete |= p[i] == x->atoms.wm_delete_window;
			w->supports_take_focus |= p[i] == x->atoms.wm_take_focus;
		}
	}
	free(r);

	/* A window that maps itself fullscreen says so in _NET_WM_STATE. */
	r = get_prop(x, w->id, x->atoms.net_wm_state, XCB_ATOM_ATOM, 16);
	if (r) {
		const xcb_atom_t *st = xcb_get_property_value(r);
		int n = xcb_get_property_value_length(r) / 4;
		for (int i = 0; i < n; i++)
			if (st[i] == x->atoms.net_wm_state_fullscreen)
				w->fullscreen = true;
	}
	free(r);
}

static void write_net_wm_state(struct rei_xwayland *x, struct rei_xwindow *w)
{
	xcb_atom_t state = x->atoms.net_wm_state_fullscreen;
	if (w->fullscreen)
		xcb_change_property(x->conn, XCB_PROP_MODE_REPLACE, w->id, x->atoms.net_wm_state,
				    XCB_ATOM_ATOM, 32, 1, &state);
	else
		xcb_delete_property(x->conn, w->id, x->atoms.net_wm_state);
}

static void set_wm_state(struct rei_xwayland *x, xcb_window_t id, uint32_t state)
{
	uint32_t data[2] = { state, XCB_NONE };  /* 1 normal, 0 withdrawn */
	xcb_change_property(x->conn, XCB_PROP_MODE_REPLACE, id, x->atoms.wm_state, x->atoms.wm_state,
			    32, 2, data);
}

/* ------------------------------------------------------------------------ */
/* Showing and hiding windows                                               */
/* ------------------------------------------------------------------------ */

static bool surface_has_content(const struct rei_surface *s)
{
	return s && (s->content || s->dmabuf);
}

static void window_unshow(struct rei_xwindow *w)
{
	if (!w->mapped_wl)
		return;
	w->mapped_wl = false;
	struct rei_server *server = w->xw->server;
	if (w->view) {
		struct rei_view *v = w->view;
		w->view = NULL;
		rei_view_destroy(v);
	}
	rei_server_schedule_damage(server);
}

static void window_try_show(struct rei_xwindow *w)
{
	if (w->mapped_wl || !w->mapped_x || !w->xsurface || !surface_has_content(w->xsurface->surface))
		return;
	w->mapped_wl = true;
	struct rei_server *server = w->xw->server;
	rei_info("x11 window mapped: \"%s\" 0x%x %dx%d%s", w->title ? w->title : "", w->id,
		 w->xsurface->surface->width, w->xsurface->surface->height,
		 w->override_redirect ? " (override-redirect)" : "");

	if (w->override_redirect) {
		rei_server_schedule_damage(server);
		return;
	}
	w->view = rei_view_create_x(server, w);
	rei_wm_add_view(w->view);
	rei_view_map(w->view);
}

static void window_destroy(struct rei_xwindow *w)
{
	window_unshow(w);
	if (w->xsurface)
		w->xsurface->window = NULL;
	wl_list_remove(&w->link);
	free(w->title);
	free(w);
}

/* ------------------------------------------------------------------------ */
/* Matching X11 windows with Wayland surfaces                               */
/* ------------------------------------------------------------------------ */

static void try_associate(struct rei_xwayland *x, struct rei_xwindow *w)
{
	if (w->xsurface || !w->has_serial)
		return;
	struct rei_xsurface *xs;
	wl_list_for_each(xs, &x->surfaces, link) {
		if (xs->has_serial && xs->serial == w->serial && !xs->window && xs->surface) {
			xs->window = w;
			w->xsurface = xs;
			window_try_show(w);
			return;
		}
	}
}

static void try_associate_surface(struct rei_xwayland *x, struct rei_xsurface *xs)
{
	if (xs->window || !xs->has_serial)
		return;
	struct rei_xwindow *w;
	wl_list_for_each(w, &x->windows, link) {
		if (w->has_serial && w->serial == xs->serial && !w->xsurface) {
			w->xsurface = xs;
			xs->window = w;
			window_try_show(w);
			return;
		}
	}
}

static void xsurface_commit(struct rei_surface *surface, void *data)
{
	struct rei_xsurface *xs = data;
	(void)surface;
	if (!xs->window)
		return;
	if (surface_has_content(xs->surface))
		window_try_show(xs->window);
	else
		window_unshow(xs->window);
}

static void xsurface_surface_destroyed(struct wl_listener *l, void *data)
{
	(void)data;
	struct rei_xsurface *xs = wl_container_of(l, xs, surface_destroy);
	if (xs->window)
		window_unshow(xs->window);
	wl_list_remove(&xs->surface_destroy.link);
	wl_list_init(&xs->surface_destroy.link);
	xs->surface = NULL;
}

struct rei_xsurface *rei_xsurface_create(struct rei_server *server, struct rei_surface *surface)
{
	struct rei_xsurface *xs = rei_xcalloc(1, sizeof *xs);
	xs->server = server;
	xs->surface = surface;
	rei_surface_set_role(surface, REI_ROLE_XWAYLAND);
	surface->role_data = xs;
	surface->role_commit = xsurface_commit;
	xs->surface_destroy.notify = xsurface_surface_destroyed;
	wl_signal_add(&surface->destroy_signal, &xs->surface_destroy);
	if (server->xwayland)
		wl_list_insert(server->xwayland->surfaces.prev, &xs->link);
	else
		wl_list_init(&xs->link);
	return xs;
}

void rei_xsurface_set_serial(struct rei_xsurface *xs, uint64_t serial)
{
	xs->serial = serial;
	xs->has_serial = true;
	if (xs->server->xwayland)
		try_associate_surface(xs->server->xwayland, xs);
}

void rei_xsurface_destroy(struct rei_xsurface *xs)
{
	if (xs->window) {
		window_unshow(xs->window);
		xs->window->xsurface = NULL;
	}
	if (xs->surface) {
		wl_list_remove(&xs->surface_destroy.link);
		xs->surface->role_commit = NULL;
		xs->surface->role_data = NULL;
	}
	if (!wl_list_empty(&xs->link))
		wl_list_remove(&xs->link);
	free(xs);
}

/* ------------------------------------------------------------------------ */
/* X11 events                                                               */
/* ------------------------------------------------------------------------ */

static struct rei_xwindow *window_create(struct rei_xwayland *x, xcb_window_t id, bool or,
					 int32_t px, int32_t py, int32_t pw, int32_t ph)
{
	struct rei_xwindow *w = rei_xcalloc(1, sizeof *w);
	w->xw = x;
	w->id = id;
	w->override_redirect = or;
	w->x = px;
	w->y = py;
	w->w = pw;
	w->h = ph;
	wl_list_insert(x->windows.prev, &w->link);
	/* Hear about its properties and size changes. */
	uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
	xcb_change_window_attributes(x->conn, id, XCB_CW_EVENT_MASK, &mask);
	return w;
}

static void on_create_notify(struct rei_xwayland *x, const xcb_create_notify_event_t *e)
{
	if (e->parent != x->screen->root || find_window(x, e->window))
		return;
	window_create(x, e->window, e->override_redirect, e->x, e->y, e->width, e->height);
}

static void on_map_request(struct rei_xwayland *x, const xcb_map_request_event_t *e)
{
	struct rei_xwindow *w = find_window(x, e->window);
	if (!w) {
		w = window_create(x, e->window, false, 0, 0, 1, 1);
	}
	read_title(x, w);
	read_hints(x, w);
	set_wm_state(x, w->id, 1);
	write_net_wm_state(x, w);
	w->mapped_x = true;
	xcb_map_window(x->conn, w->id);
	xcb_flush(x->conn);
	window_try_show(w);
}

static void on_map_notify(struct rei_xwayland *x, const xcb_map_notify_event_t *e)
{
	struct rei_xwindow *w = find_window(x, e->window);
	if (!w || !w->override_redirect)
		return;
	/* It may have been moved before we started listening to it. */
	xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(x->conn, xcb_get_geometry(x->conn, w->id), NULL);
	if (g) {
		w->x = g->x;
		w->y = g->y;
		w->w = g->width;
		w->h = g->height;
		free(g);
	}
	w->mapped_x = true;
	window_try_show(w);
}

static void on_unmap_notify(struct rei_xwayland *x, const xcb_unmap_notify_event_t *e)
{
	struct rei_xwindow *w = find_window(x, e->window);
	if (!w || e->event != x->screen->root)
		return;
	w->mapped_x = false;
	window_unshow(w);
	if (!w->override_redirect)
		set_wm_state(x, w->id, 0);
}

static void on_destroy_notify(struct rei_xwayland *x, const xcb_destroy_notify_event_t *e)
{
	struct rei_xwindow *w = find_window(x, e->window);
	if (!w)
		return;
	if (x->focused == w->id)
		x->focused = XCB_NONE;
	window_destroy(w);
}

/* Tells a client where its window really is (ICCCM 4.1.5). */
static void send_synthetic_configure(struct rei_xwayland *x, struct rei_xwindow *w)
{
	xcb_configure_notify_event_t ev = {
		.response_type = XCB_CONFIGURE_NOTIFY,
		.event = w->id, .window = w->id, .above_sibling = XCB_NONE,
		.x = (int16_t)w->x, .y = (int16_t)w->y,
		.width = (uint16_t)w->w, .height = (uint16_t)w->h,
		.border_width = 0, .override_redirect = 0,
	};
	xcb_send_event(x->conn, 0, w->id, XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&ev);
}

static void on_configure_request(struct rei_xwayland *x, const xcb_configure_request_event_t *e)
{
	struct rei_xwindow *w = find_window(x, e->window);
	if (w && w->view) {
		/* A managed, shown window is placed by the window manager alone. */
		send_synthetic_configure(x, w);
		return;
	}
	/* Anything else gets what it asked for. */
	uint32_t values[5];
	int n = 0;
	uint16_t mask = 0;
	if (e->value_mask & XCB_CONFIG_WINDOW_X) { mask |= XCB_CONFIG_WINDOW_X; values[n++] = (uint32_t)(int32_t)e->x; }
	if (e->value_mask & XCB_CONFIG_WINDOW_Y) { mask |= XCB_CONFIG_WINDOW_Y; values[n++] = (uint32_t)(int32_t)e->y; }
	if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH) { mask |= XCB_CONFIG_WINDOW_WIDTH; values[n++] = e->width; }
	if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT) { mask |= XCB_CONFIG_WINDOW_HEIGHT; values[n++] = e->height; }
	if (n)
		xcb_configure_window(x->conn, e->window, mask, values);
	if (w) {
		if (e->value_mask & XCB_CONFIG_WINDOW_X) w->x = e->x;
		if (e->value_mask & XCB_CONFIG_WINDOW_Y) w->y = e->y;
		if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH) w->w = e->width;
		if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT) w->h = e->height;
	}
}

static void on_configure_notify(struct rei_xwayland *x, const xcb_configure_notify_event_t *e)
{
	struct rei_xwindow *w = find_window(x, e->window);
	if (!w || (e->event != e->window && e->event != x->screen->root))
		return;
	w->x = e->x;
	w->y = e->y;
	w->w = e->width;
	w->h = e->height;
	if (w->override_redirect)
		rei_server_schedule_damage(x->server);
}

static void on_property_notify(struct rei_xwayland *x, const xcb_property_notify_event_t *e)
{
	struct rei_xwindow *w = find_window(x, e->window);
	if (!w)
		return;
	if (e->atom == x->atoms.net_wm_name || e->atom == x->atoms.wm_name) {
		read_title(x, w);
	} else if (e->atom == x->atoms.wm_normal_hints || e->atom == x->atoms.wm_protocols) {
		read_hints(x, w);
	}
}

static void on_client_message(struct rei_xwayland *x, const xcb_client_message_event_t *e)
{
	if (e->type == x->atoms.wl_surface_serial) {
		struct rei_xwindow *w = find_window(x, e->window);
		if (!w)
			return;
		w->serial = ((uint64_t)e->data.data32[1] << 32) | e->data.data32[0];
		w->has_serial = true;
		try_associate(x, w);
	} else if (e->type == x->atoms.net_wm_state) {
		struct rei_xwindow *w = find_window(x, e->window);
		if (!w || (e->data.data32[1] != x->atoms.net_wm_state_fullscreen &&
			   e->data.data32[2] != x->atoms.net_wm_state_fullscreen))
			return;
		uint32_t action = e->data.data32[0];  /* 0 remove, 1 add, 2 toggle */
		bool want = action == 2 ? !w->fullscreen : action == 1;
		if (w->view)
			rei_wm_set_fullscreen(w->view, want);
		else
			w->fullscreen = want;
	} else if (e->type == x->atoms.net_close_window) {
		struct rei_xwindow *w = find_window(x, e->window);
		if (w)
			rei_xwindow_close(w);
	}
}

static void handle_event(struct rei_xwayland *x, xcb_generic_event_t *ev)
{
	switch (ev->response_type & ~0x80) {
	case XCB_CREATE_NOTIFY: on_create_notify(x, (void *)ev); break;
	case XCB_MAP_REQUEST: on_map_request(x, (void *)ev); break;
	case XCB_MAP_NOTIFY: on_map_notify(x, (void *)ev); break;
	case XCB_UNMAP_NOTIFY: on_unmap_notify(x, (void *)ev); break;
	case XCB_DESTROY_NOTIFY: on_destroy_notify(x, (void *)ev); break;
	case XCB_CONFIGURE_REQUEST: on_configure_request(x, (void *)ev); break;
	case XCB_CONFIGURE_NOTIFY: on_configure_notify(x, (void *)ev); break;
	case XCB_PROPERTY_NOTIFY: on_property_notify(x, (void *)ev); break;
	case XCB_CLIENT_MESSAGE: on_client_message(x, (void *)ev); break;
	default: break;
	}
}

static void teardown(struct rei_xwayland *x, const char *why);

static void on_xcb_readable(evutil_socket_t fd, short what, void *arg)
{
	(void)fd;
	(void)what;
	struct rei_xwayland *x = arg;
	xcb_generic_event_t *ev;
	while (x->conn && (ev = xcb_poll_for_event(x->conn))) {
		handle_event(x, ev);
		free(ev);
	}
	if (x->conn && xcb_connection_has_error(x->conn))
		teardown(x, "the X11 connection closed");
	else if (x->conn)
		xcb_flush(x->conn);
	rei_server_schedule_flush(x->server);
}

/* ------------------------------------------------------------------------ */
/* Window manager interface                                                 */
/* ------------------------------------------------------------------------ */

struct rei_rect rei_xwindow_geometry(const struct rei_xwindow *w)
{
	const struct rei_surface *s = w->xsurface ? w->xsurface->surface : NULL;
	if (s)
		return (struct rei_rect){ 0, 0, s->width, s->height };
	return (struct rei_rect){ 0, 0, w->w, w->h };
}

void rei_xwindow_configure(struct rei_xwindow *w, int32_t x, int32_t y, int32_t cw, int32_t ch,
			   bool fullscreen)
{
	struct rei_xwayland *xw = w->xw;
	if (!xw->conn)
		return;
	if (cw < 1) cw = w->w > 0 ? w->w : 1;
	if (ch < 1) ch = w->h > 0 ? w->h : 1;

	if (w->fullscreen != fullscreen) {
		w->fullscreen = fullscreen;
		write_net_wm_state(xw, w);
	}
	if (w->x != x || w->y != y || w->w != cw || w->h != ch) {
		uint32_t v[4] = { (uint32_t)x, (uint32_t)y, (uint32_t)cw, (uint32_t)ch };
		xcb_configure_window(xw->conn, w->id,
				     XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
				     XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, v);
		w->x = x;
		w->y = y;
		w->w = cw;
		w->h = ch;
	}
	send_synthetic_configure(xw, w);
	xcb_flush(xw->conn);
}

void rei_xwindow_set_activated(struct rei_xwindow *w, bool activated)
{
	struct rei_xwayland *x = w->xw;
	if (w->activated == activated || !x->conn)
		return;
	w->activated = activated;
	if (activated) {
		xcb_set_input_focus(x->conn, XCB_INPUT_FOCUS_POINTER_ROOT, w->id, XCB_CURRENT_TIME);
		xcb_change_property(x->conn, XCB_PROP_MODE_REPLACE, x->screen->root,
				    x->atoms.net_active_window, XCB_ATOM_WINDOW, 32, 1, &w->id);
		if (w->supports_take_focus) {
			xcb_client_message_event_t m = {
				.response_type = XCB_CLIENT_MESSAGE, .format = 32, .window = w->id,
				.type = x->atoms.wm_protocols,
			};
			m.data.data32[0] = x->atoms.wm_take_focus;
			m.data.data32[1] = XCB_CURRENT_TIME;
			xcb_send_event(x->conn, 0, w->id, XCB_EVENT_MASK_NO_EVENT, (const char *)&m);
		}
		x->focused = w->id;
	} else if (x->focused == w->id) {
		xcb_set_input_focus(x->conn, XCB_INPUT_FOCUS_POINTER_ROOT, XCB_NONE, XCB_CURRENT_TIME);
		x->focused = XCB_NONE;
	}
	xcb_flush(x->conn);
}

void rei_xwindow_close(struct rei_xwindow *w)
{
	struct rei_xwayland *x = w->xw;
	if (!x->conn)
		return;
	if (w->supports_delete) {
		xcb_client_message_event_t m = {
			.response_type = XCB_CLIENT_MESSAGE, .format = 32, .window = w->id,
			.type = x->atoms.wm_protocols,
		};
		m.data.data32[0] = x->atoms.wm_delete_window;
		m.data.data32[1] = XCB_CURRENT_TIME;
		xcb_send_event(x->conn, 0, w->id, XCB_EVENT_MASK_NO_EVENT, (const char *)&m);
	} else {
		xcb_kill_client(x->conn, w->id);
	}
	xcb_flush(x->conn);
}

struct rei_surface *rei_xwindow_surface(const struct rei_xwindow *w)
{
	return w->xsurface ? w->xsurface->surface : NULL;
}

bool rei_xwindow_wants_float(const struct rei_xwindow *w)
{
	bool fixed = w->min_w > 0 && w->min_h > 0 && w->min_w == w->max_w && w->min_h == w->max_h;
	return w->transient_for != 0 || w->dialog_type || fixed;
}

bool rei_xwindow_wants_fullscreen(const struct rei_xwindow *w)
{
	return w->fullscreen;
}

void rei_xwindow_set_fullscreen(struct rei_xwindow *w, bool on)
{
	w->fullscreen = on;
	if (w->xw->conn) {
		write_net_wm_state(w->xw, w);
		xcb_flush(w->xw->conn);
	}
}

void rei_xwindow_emit(const struct rei_view *view, struct rei_scene_snapshot *snap)
{
	struct rei_surface *s = rei_xwindow_surface(view->xwin);
	if (s)
		rei_surface_emit(s, (float)view->x, (float)view->y, snap);
}

bool rei_xwindow_pick(const struct rei_view *view, double x, double y, struct rei_surface **out,
		      double *sx, double *sy)
{
	struct rei_surface *s = rei_xwindow_surface(view->xwin);
	return s && rei_surface_pick(s, (double)view->x, (double)view->y, x, y, out, sx, sy);
}

/* ---- Surfaces, for the rest of the compositor ---- */

static struct rei_xsurface *xsurface_of_root(struct rei_surface *root)
{
	if (root->role != REI_ROLE_XWAYLAND || !root->role_data)
		return NULL;
	return root->role_data;
}

struct rei_view *rei_xwayland_view_of(struct rei_surface *surface)
{
	struct rei_xsurface *xs = xsurface_of_root(rei_surface_root(surface));
	return xs && xs->window ? xs->window->view : NULL;
}

bool rei_xwayland_surface_rect(struct rei_surface *root, struct rei_rect *out)
{
	struct rei_xsurface *xs = xsurface_of_root(root);
	if (!xs || !xs->window || !xs->window->mapped_wl)
		return false;
	struct rei_xwindow *w = xs->window;
	if (w->override_redirect) {
		*out = (struct rei_rect){ w->x, w->y, root->width, root->height };
		return true;
	}
	if (!w->view || !rei_view_visible(w->view))
		return false;
	*out = (struct rei_rect){ w->view->x, w->view->y, root->width, root->height };
	return true;
}

bool rei_xwayland_surface_hidden(struct rei_surface *root)
{
	struct rei_xsurface *xs = xsurface_of_root(root);
	return xs && xs->window && xs->window->view && !rei_view_visible(xs->window->view);
}

void rei_xwayland_emit_unmanaged(struct rei_server *server, struct rei_scene_snapshot *snap)
{
	struct rei_xwayland *x = X(server);
	if (!x)
		return;
	struct rei_xwindow *w;
	wl_list_for_each(w, &x->windows, link) {
		if (w->override_redirect && w->mapped_wl && w->xsurface && w->xsurface->surface)
			rei_surface_emit(w->xsurface->surface, (float)w->x, (float)w->y, snap);
	}
}

bool rei_xwayland_pick_unmanaged(struct rei_server *server, double px, double py,
				 struct rei_surface **out, double *sx, double *sy)
{
	struct rei_xwayland *x = X(server);
	if (!x)
		return false;
	struct rei_xwindow *w;
	wl_list_for_each_reverse(w, &x->windows, link) {
		if (w->override_redirect && w->mapped_wl && w->xsurface && w->xsurface->surface &&
		    rei_surface_pick(w->xsurface->surface, (double)w->x, (double)w->y, px, py, out, sx, sy))
			return true;
	}
	return false;
}

/* ------------------------------------------------------------------------ */
/* Starting and stopping Xwayland                                           */
/* ------------------------------------------------------------------------ */

const char *rei_xwayland_display_name(const struct rei_server *server)
{
	const struct rei_xwayland *x = server->xwayland;
	return x && x->running ? x->display_name : NULL;
}

static void close_fd(int *fd)
{
	if (*fd >= 0)
		close(*fd);
	*fd = -1;
}

/* Claims display :n by creating its lock file and listening sockets. */
static bool claim_display(struct rei_xwayland *x, int n)
{
	snprintf(x->lock_path, sizeof x->lock_path, "/tmp/.X%d-lock", n);
	int lock = open(x->lock_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444);
	if (lock < 0) {
		/* A lock left by a process that no longer exists is stale. */
		FILE *f = fopen(x->lock_path, "r");
		int pid = 0;
		if (f) {
			if (fscanf(f, "%d", &pid) != 1)
				pid = 0;
			fclose(f);
		}
		if (pid > 0 && kill(pid, 0) != 0 && errno == ESRCH) {
			unlink(x->lock_path);
			lock = open(x->lock_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444);
		}
		if (lock < 0)
			return false;
	}
	char pidtxt[16];
	int len = snprintf(pidtxt, sizeof pidtxt, "%10d\n", (int)getpid());
	if (write(lock, pidtxt, (size_t)len) < 0) { /* best effort */ }
	close(lock);

	mkdir("/tmp/.X11-unix", 01777);
	chmod("/tmp/.X11-unix", 01777);
	snprintf(x->socket_path, sizeof x->socket_path, "/tmp/.X11-unix/X%d", n);
	unlink(x->socket_path);

	for (int i = 0; i < 2; i++) {
		int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		struct sockaddr_un addr = { .sun_family = AF_UNIX };
		socklen_t len2;
		if (i == 0) {  /* The abstract socket, which most clients try first. */
			addr.sun_path[0] = '\0';
			int l = snprintf(addr.sun_path + 1, sizeof addr.sun_path - 1, "%s", x->socket_path);
			len2 = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)l);
		} else {
			snprintf(addr.sun_path, sizeof addr.sun_path, "%s", x->socket_path);
			len2 = (socklen_t)sizeof addr;
		}
		if (fd < 0 || bind(fd, (struct sockaddr *)&addr, len2) != 0 || listen(fd, 128) != 0) {
			if (fd >= 0)
				close(fd);
			close_fd(&x->listen_fd[0]);
			unlink(x->lock_path);
			return false;
		}
		x->listen_fd[i] = fd;
	}
	x->display = n;
	snprintf(x->display_name, sizeof x->display_name, ":%d", n);
	return true;
}

static void release_display(struct rei_xwayland *x)
{
	close_fd(&x->listen_fd[0]);
	close_fd(&x->listen_fd[1]);
	if (x->display >= 0) {
		unlink(x->socket_path);
		unlink(x->lock_path);
	}
	x->display = -1;
}

static void teardown(struct rei_xwayland *x, const char *why)
{
	if (!x->running && !x->conn)
		return;
	rei_warn("xwayland: stopped (%s); X11 programs can no longer run", why);
	x->running = false;
	if (x->xcb_event) {
		event_free(x->xcb_event);
		x->xcb_event = NULL;
	}
	struct rei_xwindow *w, *tmp;
	wl_list_for_each_safe(w, tmp, &x->windows, link)
		window_destroy(w);
	if (x->conn) {
		xcb_disconnect(x->conn);
		x->conn = NULL;
	}
	if (x->pid > 0)
		kill(x->pid, SIGTERM);
}

/*
 * Xwayland only starts accepting other X11 clients once the window manager has
 * redirected the root's subwindows with the Composite extension (this is how a
 * rootless X server knows its window manager is in place). libxcb-composite is not
 * required for the one request we need, so it is sent by hand.
 */
static bool composite_redirect_root(struct rei_xwayland *x)
{
	xcb_query_extension_cookie_t qc = xcb_query_extension(x->conn, 9, "Composite");
	xcb_query_extension_reply_t *q = xcb_query_extension_reply(x->conn, qc, NULL);
	if (!q || !q->present) {
		free(q);
		return false;
	}
	uint8_t major = q->major_opcode;
	free(q);

	struct {
		uint8_t major_opcode, minor_opcode;
		uint16_t length;
		uint32_t window;
		uint8_t update, pad[3];
	} req = { .major_opcode = major, .minor_opcode = 2 /* RedirectSubwindows */, .length = 3,
		  .window = x->screen->root, .update = 1 /* Manual */ };
	struct iovec parts[3] = { { 0 }, { 0 }, { &req, sizeof req } };
	xcb_protocol_request_t xreq = { .count = 1, .ext = NULL, .opcode = major, .isvoid = 1 };
	xcb_void_cookie_t ck = { xcb_send_request(x->conn, XCB_REQUEST_CHECKED, &parts[2], &xreq) };
	xcb_generic_error_t *err = xcb_request_check(x->conn, ck);
	free(err);
	return err == NULL;
}

static void become_window_manager(struct rei_xwayland *x)
{
	x->conn = xcb_connect_to_fd(x->wm_fd, NULL);
	x->wm_fd = -1;  /* xcb owns it now. */
	if (!x->conn || xcb_connection_has_error(x->conn)) {
		rei_error("xwayland: cannot talk to the X server");
		teardown(x, "connection failed");
		return;
	}
	x->screen = xcb_setup_roots_iterator(xcb_get_setup(x->conn)).data;
	load_atoms(x);

	/* Becoming the window manager is claiming SubstructureRedirect on the root. */
	uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
			XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_void_cookie_t ck = xcb_change_window_attributes_checked(x->conn, x->screen->root,
								    XCB_CW_EVENT_MASK, &mask);
	xcb_generic_error_t *err = xcb_request_check(x->conn, ck);
	if (err) {
		free(err);
		rei_error("xwayland: another window manager is running on the X server");
		teardown(x, "no window manager role");
		return;
	}

	if (!composite_redirect_root(x)) {
		rei_error("xwayland: the X server has no usable Composite extension");
		teardown(x, "Composite redirect failed");
		return;
	}

	x->wm_window = xcb_generate_id(x->conn);
	xcb_create_window(x->conn, XCB_COPY_FROM_PARENT, x->wm_window, x->screen->root, -1, -1, 1, 1, 0,
			  XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0, NULL);
	xcb_change_property(x->conn, XCB_PROP_MODE_REPLACE, x->wm_window, x->atoms.net_supporting_wm_check,
			    XCB_ATOM_WINDOW, 32, 1, &x->wm_window);
	xcb_change_property(x->conn, XCB_PROP_MODE_REPLACE, x->wm_window, x->atoms.net_wm_name,
			    x->atoms.utf8_string, 8, 6, "rei-wm");
	xcb_change_property(x->conn, XCB_PROP_MODE_REPLACE, x->screen->root, x->atoms.net_supporting_wm_check,
			    XCB_ATOM_WINDOW, 32, 1, &x->wm_window);
	xcb_atom_t supported[] = { x->atoms.net_wm_state, x->atoms.net_wm_state_fullscreen,
				   x->atoms.net_active_window, x->atoms.net_close_window,
				   x->atoms.net_supporting_wm_check, x->atoms.net_wm_window_type };
	xcb_change_property(x->conn, XCB_PROP_MODE_REPLACE, x->screen->root, x->atoms.net_supported,
			    XCB_ATOM_ATOM, 32, REI_ARRAY_LEN(supported), supported);
	/* Claim the manager selections of a window manager and compositing manager. */
	xcb_intern_atom_cookie_t c1 = xcb_intern_atom(x->conn, 0, 5, "WM_S0");
	xcb_intern_atom_cookie_t c2 = xcb_intern_atom(x->conn, 0, 12, "_NET_WM_CM_S0");
	xcb_atom_t wm_s0 = intern_reply(x->conn, c1), cm_s0 = intern_reply(x->conn, c2);
	xcb_set_selection_owner(x->conn, x->wm_window, wm_s0, XCB_CURRENT_TIME);
	xcb_set_selection_owner(x->conn, x->wm_window, cm_s0, XCB_CURRENT_TIME);
	xcb_flush(x->conn);

	x->xcb_event = event_new(x->server->base, xcb_get_file_descriptor(x->conn), EV_READ | EV_PERSIST,
				 on_xcb_readable, x);
	event_add(x->xcb_event, NULL);
	x->running = true;
	rei_info("xwayland: ready on DISPLAY=%s", x->display_name);
	on_xcb_readable(-1, 0, x);  /* Anything that arrived while we were setting up. */
}

static void on_ready(evutil_socket_t fd, short what, void *arg)
{
	(void)what;
	struct rei_xwayland *x = arg;
	char buf[32];
	ssize_t n = read(fd, buf, sizeof buf);
	if (n <= 0)
		return;  /* Closed: the child exited; on_child reports that. */
	event_free(x->ready_event);
	x->ready_event = NULL;
	close_fd(&x->ready_fd);
	become_window_manager(x);
}

static void on_child(evutil_socket_t sig, short what, void *arg)
{
	(void)sig;
	(void)what;
	struct rei_xwayland *x = arg;
	if (x->pid <= 0)
		return;
	int status;
	if (waitpid(x->pid, &status, WNOHANG) != x->pid)
		return;
	x->pid = -1;
	teardown(x, "the Xwayland process exited");
}

void rei_xwayland_start(struct rei_server *server)
{
	const char *off = getenv("REI_NO_XWAYLAND");  /* Used by the automated tests. */
	if (!server->config->xwayland || (off && off[0] == '1'))
		return;

	struct rei_xwayland *x = rei_xcalloc(1, sizeof *x);
	x->server = server;
	x->display = -1;
	x->listen_fd[0] = x->listen_fd[1] = x->wm_fd = x->ready_fd = -1;
	x->pid = -1;
	wl_list_init(&x->windows);
	wl_list_init(&x->surfaces);

	int wl_sv[2] = { -1, -1 }, wm_sv[2] = { -1, -1 }, ready[2] = { -1, -1 };
	bool claimed = false;
	for (int n = 0; n < 64 && !claimed; n++)
		claimed = claim_display(x, n);
	if (!claimed) {
		rei_warn("xwayland: no free X display number; X11 programs will not work");
		goto fail;
	}
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wl_sv) != 0 ||
	    socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wm_sv) != 0 ||
	    pipe2(ready, O_CLOEXEC) != 0) {
		rei_warn("xwayland: cannot create sockets: %s", strerror(errno));
		goto fail;
	}

	/* Everything the child needs is prepared before forking. */
	char a_disp[16], a_l0[16], a_l1[16], a_wm[16], a_ready[16], env_ws[32];
	snprintf(a_disp, sizeof a_disp, ":%d", x->display);
	snprintf(a_l0, sizeof a_l0, "%d", x->listen_fd[0]);
	snprintf(a_l1, sizeof a_l1, "%d", x->listen_fd[1]);
	snprintf(a_wm, sizeof a_wm, "%d", wm_sv[1]);
	snprintf(a_ready, sizeof a_ready, "%d", ready[1]);
	snprintf(env_ws, sizeof env_ws, "WAYLAND_SOCKET=%d", wl_sv[1]);
	char *argv[] = { "Xwayland", a_disp, "-rootless", "-noreset", "-listenfd", a_l0, "-listenfd", a_l1,
			 "-wm", a_wm, "-displayfd", a_ready, NULL };


	extern char **environ;
	size_t ne = 0;
	while (environ[ne])
		ne++;
	char **env = rei_xcalloc(ne + 2, sizeof *env);
	size_t out = 0;
	for (size_t i = 0; i < ne; i++)
		if (strncmp(environ[i], "WAYLAND_DISPLAY=", 16) && strncmp(environ[i], "DISPLAY=", 8) &&
		    strncmp(environ[i], "WAYLAND_SOCKET=", 15))
			env[out++] = environ[i];
	env[out++] = env_ws;

	pid_t pid = fork();
	if (pid < 0) {
		rei_warn("xwayland: fork failed: %s", strerror(errno));
		free(env);
		goto fail;
	}
	if (pid == 0) {
		/* Hand the descriptors Xwayland needs across exec. */
		int keep[] = { x->listen_fd[0], x->listen_fd[1], wm_sv[1], ready[1], wl_sv[1] };
		for (size_t i = 0; i < REI_ARRAY_LEN(keep); i++) {
			int flags = fcntl(keep[i], F_GETFD);
			fcntl(keep[i], F_SETFD, flags & ~FD_CLOEXEC);
		}
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		execvpe("Xwayland", argv, env);
		_exit(127);
	}
	free(env);
	x->pid = pid;

	close(wl_sv[1]);
	close(wm_sv[1]);
	close(ready[1]);
	x->wm_fd = wm_sv[0];
	x->ready_fd = ready[0];
	/* The child holds the listening sockets now. */
	close_fd(&x->listen_fd[0]);
	close_fd(&x->listen_fd[1]);

	server->xwayland = x;
	server->xwayland_client = wl_client_create(server->display, wl_sv[0]);
	if (!server->xwayland_client || !rei_xwayland_shell_init(server)) {
		rei_warn("xwayland: cannot register with the Wayland server");
		rei_xwayland_stop(server);
		return;
	}
	x->ready_event = event_new(server->base, x->ready_fd, EV_READ | EV_PERSIST, on_ready, x);
	event_add(x->ready_event, NULL);
	x->child_event = evsignal_new(server->base, SIGCHLD, on_child, x);
	event_add(x->child_event, NULL);
	rei_info("xwayland: starting on display %s", x->display_name);
	return;

fail:
	close_fd(&wl_sv[0]); close_fd(&wl_sv[1]);
	close_fd(&wm_sv[0]); close_fd(&wm_sv[1]);
	close_fd(&ready[0]); close_fd(&ready[1]);
	release_display(x);
	free(x);
}

void rei_xwayland_stop(struct rei_server *server)
{
	struct rei_xwayland *x = server->xwayland;
	if (!x)
		return;
	server->xwayland = NULL;

	x->running = false;
	if (x->ready_event)
		event_free(x->ready_event);
	if (x->xcb_event)
		event_free(x->xcb_event);
	if (x->child_event)
		event_free(x->child_event);
	struct rei_xwindow *w, *tmp;
	wl_list_for_each_safe(w, tmp, &x->windows, link)
		window_destroy(w);
	struct rei_xsurface *xs, *xtmp;
	wl_list_for_each_safe(xs, xtmp, &x->surfaces, link) {
		wl_list_remove(&xs->link);
		wl_list_init(&xs->link);
	}
	if (x->conn)
		xcb_disconnect(x->conn);
	close_fd(&x->wm_fd);
	close_fd(&x->ready_fd);
	if (x->pid > 0) {
		kill(x->pid, SIGTERM);
		int status;
		waitpid(x->pid, &status, 0);
	}
	release_display(x);
	free(x);
}
