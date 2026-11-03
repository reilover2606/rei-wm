#include "proto/xdg_shell.h"

#include <stdlib.h>
#include <string.h>

#include "desktop.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"
#include "xdg-shell-server-protocol.h"

#define MAX_UNACKED 8

struct rei_xdg_popup;

struct rei_xdg_surface {
	struct rei_server *server;
	struct wl_resource *resource;
	struct wl_resource *wm_base;
	struct rei_surface *surface;  /* NULL after the wl_surface is destroyed. */
	struct wl_listener surface_destroy;

	struct rei_xdg_toplevel *toplevel;
	struct rei_xdg_popup *popup;

	bool initial_sent, configured, mapped;
	uint32_t sent[MAX_UNACKED];
	int n_sent;

	struct rei_rect geometry, pending_geometry;
	bool has_geometry, has_pending_geometry;
};

struct rei_positioner {
	int32_t w, h;
	struct rei_rect anchor_rect;
	uint32_t anchor, gravity;
	int32_t off_x, off_y;
	bool reactive;
};

struct rei_xdg_popup {
	struct wl_list link;  /* rei_server.popups, creation order = stacking */
	struct wl_resource *resource;
	struct rei_xdg_surface *xs;
	struct rei_xdg_surface *parent;  /* NULL once the parent is gone. */
	struct wl_listener parent_destroy;
	struct rei_positioner pos;
	int32_t x, y;  /* Popup geometry origin relative to parent's geometry origin. */
};

/* ------------------------------------------------------------------------ */
/* Geometry helpers                                                         */
/* ------------------------------------------------------------------------ */

static struct rei_rect xs_geometry(const struct rei_xdg_surface *xs)
{
	if (xs->has_geometry)
		return xs->geometry;
	if (xs->surface)
		return (struct rei_rect){ 0, 0, xs->surface->width, xs->surface->height };
	return (struct rei_rect){ 0 };
}

struct rei_rect rei_xdg_toplevel_geometry(const struct rei_xdg_toplevel *tl)
{
	return xs_geometry(tl->xs);
}

/* Global position of the top-left of the surface's window geometry. */
static bool xs_geometry_origin(const struct rei_xdg_surface *xs, int32_t *x, int32_t *y)
{
	if (xs->toplevel && xs->toplevel->view) {
		*x = xs->toplevel->view->x;
		*y = xs->toplevel->view->y;
		return true;
	}
	if (xs->popup && xs->popup->parent && xs->mapped) {
		int32_t px, py;
		if (!xs_geometry_origin(xs->popup->parent, &px, &py))
			return false;
		*x = px + xs->popup->x;
		*y = py + xs->popup->y;
		return true;
	}
	return false;
}

static const struct rei_xdg_surface *popup_root(const struct rei_xdg_surface *xs)
{
	while (xs && xs->popup)
		xs = xs->popup->parent;
	return xs;
}

/* ------------------------------------------------------------------------ */
/* Configure / ack                                                          */
/* ------------------------------------------------------------------------ */

static void send_xdg_configure(struct rei_xdg_surface *xs)
{
	uint32_t serial = wl_display_next_serial(xs->server->display);
	if (xs->n_sent == MAX_UNACKED) {
		memmove(xs->sent, xs->sent + 1, (MAX_UNACKED - 1) * sizeof xs->sent[0]);
		xs->n_sent--;
	}
	xs->sent[xs->n_sent++] = serial;
	xdg_surface_send_configure(xs->resource, serial);
}

static void send_toplevel_configure(struct rei_xdg_toplevel *tl)
{
	struct wl_array states;
	wl_array_init(&states);
	if (tl->activated) {
		uint32_t *st = wl_array_add(&states, sizeof *st);
		*st = XDG_TOPLEVEL_STATE_ACTIVATED;
	}
	/* 0x0 = "pick your own size". Tiling will set real sizes in milestone 5. */
	xdg_toplevel_send_configure(tl->resource, 0, 0, &states);
	wl_array_release(&states);
	send_xdg_configure(tl->xs);
}

static void send_popup_configure(struct rei_xdg_popup *p)
{
	xdg_popup_send_configure(p->resource, p->x, p->y, p->pos.w, p->pos.h);
	send_xdg_configure(p->xs);
}

void rei_xdg_toplevel_set_activated(struct rei_xdg_toplevel *tl, bool activated)
{
	if (tl->activated == activated)
		return;
	tl->activated = activated;
	if (tl->xs->initial_sent)
		send_toplevel_configure(tl);
}

void rei_xdg_toplevel_close(struct rei_xdg_toplevel *tl)
{
	xdg_toplevel_send_close(tl->resource);
}

/* ------------------------------------------------------------------------ */
/* Positioner                                                               */
/* ------------------------------------------------------------------------ */

static void anchor_point(const struct rei_rect *r, uint32_t anchor, int32_t *x, int32_t *y)
{
	*x = r->x + r->w / 2;
	*y = r->y + r->h / 2;
	switch (anchor) {
	case XDG_POSITIONER_ANCHOR_TOP: *y = r->y; break;
	case XDG_POSITIONER_ANCHOR_BOTTOM: *y = r->y + r->h; break;
	case XDG_POSITIONER_ANCHOR_LEFT: *x = r->x; break;
	case XDG_POSITIONER_ANCHOR_RIGHT: *x = r->x + r->w; break;
	case XDG_POSITIONER_ANCHOR_TOP_LEFT: *x = r->x; *y = r->y; break;
	case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT: *x = r->x; *y = r->y + r->h; break;
	case XDG_POSITIONER_ANCHOR_TOP_RIGHT: *x = r->x + r->w; *y = r->y; break;
	case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT: *x = r->x + r->w; *y = r->y + r->h; break;
	default: break;
	}
}

/* Top-left of the popup relative to the parent's geometry origin. Constraint
 * adjustment (sliding/flipping to stay on screen) is not implemented yet. */
static void positioner_compute(const struct rei_positioner *p, int32_t *x, int32_t *y)
{
	int32_t ax, ay;
	anchor_point(&p->anchor_rect, p->anchor, &ax, &ay);
	ax += p->off_x;
	ay += p->off_y;

	switch (p->gravity) {
	case XDG_POSITIONER_GRAVITY_LEFT:
	case XDG_POSITIONER_GRAVITY_TOP_LEFT:
	case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
		ax -= p->w;
		break;
	case XDG_POSITIONER_GRAVITY_RIGHT:
	case XDG_POSITIONER_GRAVITY_TOP_RIGHT:
	case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT:
		break;
	default:
		ax -= p->w / 2;
		break;
	}
	switch (p->gravity) {
	case XDG_POSITIONER_GRAVITY_TOP:
	case XDG_POSITIONER_GRAVITY_TOP_LEFT:
	case XDG_POSITIONER_GRAVITY_TOP_RIGHT:
		ay -= p->h;
		break;
	case XDG_POSITIONER_GRAVITY_BOTTOM:
	case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
	case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT:
		break;
	default:
		ay -= p->h / 2;
		break;
	}
	*x = ax;
	*y = ay;
}

static void positioner_destroy(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void positioner_set_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h)
{
	(void)c;
	struct rei_positioner *p = wl_resource_get_user_data(r);
	if (w <= 0 || h <= 0) {
		wl_resource_post_error(r, XDG_POSITIONER_ERROR_INVALID_INPUT, "size must be positive");
		return;
	}
	p->w = w;
	p->h = h;
}

static void positioner_set_anchor_rect(struct wl_client *c, struct wl_resource *r,
				       int32_t x, int32_t y, int32_t w, int32_t h)
{
	(void)c;
	struct rei_positioner *p = wl_resource_get_user_data(r);
	if (w < 0 || h < 0) {
		wl_resource_post_error(r, XDG_POSITIONER_ERROR_INVALID_INPUT, "negative anchor rect");
		return;
	}
	p->anchor_rect = (struct rei_rect){ x, y, w, h };
}

static void positioner_set_anchor(struct wl_client *c, struct wl_resource *r, uint32_t anchor)
{
	(void)c;
	struct rei_positioner *p = wl_resource_get_user_data(r);
	p->anchor = anchor;
}

static void positioner_set_gravity(struct wl_client *c, struct wl_resource *r, uint32_t gravity)
{
	(void)c;
	struct rei_positioner *p = wl_resource_get_user_data(r);
	p->gravity = gravity;
}

static void positioner_set_constraint_adjustment(struct wl_client *c, struct wl_resource *r, uint32_t v)
{
	(void)c; (void)r; (void)v;
}

static void positioner_set_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y)
{
	(void)c;
	struct rei_positioner *p = wl_resource_get_user_data(r);
	p->off_x = x;
	p->off_y = y;
}

static void positioner_set_reactive(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	struct rei_positioner *p = wl_resource_get_user_data(r);
	p->reactive = true;
}

static void positioner_set_parent_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h)
{
	(void)c; (void)r; (void)w; (void)h;
}

static void positioner_set_parent_configure(struct wl_client *c, struct wl_resource *r, uint32_t s)
{
	(void)c; (void)r; (void)s;
}

static const struct xdg_positioner_interface positioner_impl = {
	.destroy = positioner_destroy,
	.set_size = positioner_set_size,
	.set_anchor_rect = positioner_set_anchor_rect,
	.set_anchor = positioner_set_anchor,
	.set_gravity = positioner_set_gravity,
	.set_constraint_adjustment = positioner_set_constraint_adjustment,
	.set_offset = positioner_set_offset,
	.set_reactive = positioner_set_reactive,
	.set_parent_size = positioner_set_parent_size,
	.set_parent_configure = positioner_set_parent_configure,
};

static void positioner_resource_destroy(struct wl_resource *r)
{
	free(wl_resource_get_user_data(r));
}

/* ------------------------------------------------------------------------ */
/* xdg_toplevel                                                             */
/* ------------------------------------------------------------------------ */

static void toplevel_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void toplevel_set_parent(struct wl_client *c, struct wl_resource *r, struct wl_resource *p)
{
	(void)c; (void)r; (void)p;
}

static void toplevel_set_title(struct wl_client *c, struct wl_resource *r, const char *title)
{
	(void)c;
	struct rei_xdg_toplevel *tl = wl_resource_get_user_data(r);
	free(tl->title);
	tl->title = rei_xstrdup(title);
}

static void toplevel_set_app_id(struct wl_client *c, struct wl_resource *r, const char *id)
{
	(void)c;
	struct rei_xdg_toplevel *tl = wl_resource_get_user_data(r);
	free(tl->app_id);
	tl->app_id = rei_xstrdup(id);
}

static void toplevel_show_window_menu(struct wl_client *c, struct wl_resource *r,
				      struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y)
{
	(void)c; (void)r; (void)seat; (void)serial; (void)x; (void)y;
}

static void toplevel_move(struct wl_client *c, struct wl_resource *r,
			  struct wl_resource *seat, uint32_t serial)
{
	/* Interactive move needs pointer input (milestone 4) and the window
	 * manager (milestone 5). */
	(void)c; (void)r; (void)seat; (void)serial;
}

static void toplevel_resize(struct wl_client *c, struct wl_resource *r,
			    struct wl_resource *seat, uint32_t serial, uint32_t edges)
{
	(void)c; (void)r; (void)seat; (void)serial; (void)edges;
}

static void toplevel_set_max_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h)
{
	(void)c;
	struct rei_xdg_toplevel *tl = wl_resource_get_user_data(r);
	if (w < 0 || h < 0) {
		wl_resource_post_error(r, XDG_TOPLEVEL_ERROR_INVALID_SIZE, "negative max size");
		return;
	}
	tl->max_w = w;
	tl->max_h = h;
}

static void toplevel_set_min_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h)
{
	(void)c;
	struct rei_xdg_toplevel *tl = wl_resource_get_user_data(r);
	if (w < 0 || h < 0) {
		wl_resource_post_error(r, XDG_TOPLEVEL_ERROR_INVALID_SIZE, "negative min size");
		return;
	}
	tl->min_w = w;
	tl->min_h = h;
}

/* Maximize/fullscreen/minimize are placeholders until the window manager and
 * decorations exist. A client that asks must still get a configure back, or it
 * may wait forever for the state change. */
static void toplevel_state_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	struct rei_xdg_toplevel *tl = wl_resource_get_user_data(r);
	if (tl->xs->initial_sent)
		send_toplevel_configure(tl);
}

static void toplevel_set_fullscreen(struct wl_client *c, struct wl_resource *r, struct wl_resource *o)
{
	(void)o;
	toplevel_state_request(c, r);
}

static void toplevel_set_minimized(struct wl_client *c, struct wl_resource *r)
{
	(void)c; (void)r;
}

static const struct xdg_toplevel_interface toplevel_impl = {
	.destroy = toplevel_destroy_request,
	.set_parent = toplevel_set_parent,
	.set_title = toplevel_set_title,
	.set_app_id = toplevel_set_app_id,
	.show_window_menu = toplevel_show_window_menu,
	.move = toplevel_move,
	.resize = toplevel_resize,
	.set_max_size = toplevel_set_max_size,
	.set_min_size = toplevel_set_min_size,
	.set_maximized = toplevel_state_request,
	.unset_maximized = toplevel_state_request,
	.set_fullscreen = toplevel_set_fullscreen,
	.unset_fullscreen = toplevel_state_request,
	.set_minimized = toplevel_set_minimized,
};

static void unmap_xdg_surface(struct rei_xdg_surface *xs)
{
	if (xs->toplevel && xs->toplevel->view) {
		rei_view_destroy(xs->toplevel->view);
		xs->toplevel->view = NULL;
	}
	xs->mapped = false;
	rei_server_schedule_damage(xs->server);
}

static void toplevel_resource_destroy(struct wl_resource *r)
{
	struct rei_xdg_toplevel *tl = wl_resource_get_user_data(r);
	/* The xdg_surface may already be gone: libwayland destroys a dying client's
	 * objects in id order, and the xdg_surface is created first. */
	if (tl->xs) {
		unmap_xdg_surface(tl->xs);
		tl->xs->toplevel = NULL;
		tl->xs->initial_sent = tl->xs->configured = false;
		tl->xs->n_sent = 0;
	}
	free(tl->title);
	free(tl->app_id);
	free(tl);
}

/* ------------------------------------------------------------------------ */
/* xdg_popup                                                                */
/* ------------------------------------------------------------------------ */

static void popup_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void popup_grab(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial)
{
	/* Dismissing popups on outside clicks needs pointer input (milestone 4). */
	(void)c; (void)r; (void)seat; (void)serial;
}

static void popup_reposition(struct wl_client *c, struct wl_resource *r,
			     struct wl_resource *positioner, uint32_t token)
{
	(void)c;
	struct rei_xdg_popup *p = wl_resource_get_user_data(r);
	p->pos = *(struct rei_positioner *)wl_resource_get_user_data(positioner);
	positioner_compute(&p->pos, &p->x, &p->y);
	xdg_popup_send_repositioned(r, token);
	send_popup_configure(p);
	rei_server_schedule_damage(p->xs->server);
}

static const struct xdg_popup_interface popup_impl = {
	.destroy = popup_destroy_request,
	.grab = popup_grab,
	.reposition = popup_reposition,
};

static void popup_on_parent_destroyed(struct wl_listener *l, void *data)
{
	(void)data;
	struct rei_xdg_popup *p = wl_container_of(l, p, parent_destroy);
	wl_list_remove(&p->parent_destroy.link);
	wl_list_init(&p->parent_destroy.link);
	p->parent = NULL;
}

static void popup_resource_destroy(struct wl_resource *r)
{
	struct rei_xdg_popup *p = wl_resource_get_user_data(r);
	wl_list_remove(&p->link);
	if (!wl_list_empty(&p->parent_destroy.link))
		wl_list_remove(&p->parent_destroy.link);
	if (p->xs) {  /* NULL if the xdg_surface was destroyed first */
		p->xs->mapped = false;
		p->xs->popup = NULL;
		p->xs->initial_sent = p->xs->configured = false;
		p->xs->n_sent = 0;
		rei_server_schedule_damage(p->xs->server);
	}
	free(p);
}

/* ------------------------------------------------------------------------ */
/* xdg_surface                                                              */
/* ------------------------------------------------------------------------ */

static void xdg_surface_commit(struct rei_surface *surface, void *data)
{
	struct rei_xdg_surface *xs = data;
	if (!xs->toplevel && !xs->popup)
		return;

	if (xs->has_pending_geometry) {
		xs->geometry = xs->pending_geometry;
		xs->has_geometry = true;
		xs->has_pending_geometry = false;
	}

	bool has_buffer = surface->content != NULL;

	if (!xs->initial_sent) {
		if (has_buffer) {
			wl_resource_post_error(xs->resource, XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
					       "buffer attached before the initial configure");
			return;
		}
		xs->initial_sent = true;
		if (xs->toplevel)
			send_toplevel_configure(xs->toplevel);
		else
			send_popup_configure(xs->popup);
		return;
	}

	if (has_buffer && !xs->configured) {
		wl_resource_post_error(xs->resource, XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
				       "buffer attached before configure was acknowledged");
		return;
	}

	if (has_buffer && !xs->mapped) {
		xs->mapped = true;
		if (xs->toplevel) {
			xs->toplevel->view = rei_view_create(xs->server, xs->toplevel);
			rei_view_map(xs->toplevel->view);
			rei_info("window mapped: \"%s\" (%s) %dx%d",
				 xs->toplevel->title ? xs->toplevel->title : "",
				 xs->toplevel->app_id ? xs->toplevel->app_id : "",
				 surface->width, surface->height);
		}
	} else if (!has_buffer && xs->mapped) {
		unmap_xdg_surface(xs);
		xs->initial_sent = xs->configured = false;
		xs->n_sent = 0;
	}
}

static void xdg_surface_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	struct rei_xdg_surface *xs = wl_resource_get_user_data(r);
	if (xs->toplevel || xs->popup) {
		wl_resource_post_error(r, XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
				       "xdg_surface destroyed before its role object");
		return;
	}
	wl_resource_destroy(r);
}

static void xdg_surface_get_toplevel(struct wl_client *client, struct wl_resource *res, uint32_t id)
{
	struct rei_xdg_surface *xs = wl_resource_get_user_data(res);
	if (xs->toplevel || xs->popup) {
		wl_resource_post_error(res, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
				       "xdg_surface already has a role object");
		return;
	}
	if (!xs->surface || !rei_surface_set_role(xs->surface, REI_ROLE_XDG_TOPLEVEL)) {
		wl_resource_post_error(xs->wm_base, XDG_WM_BASE_ERROR_ROLE,
				       "surface already has a different role");
		return;
	}

	struct wl_resource *tres = wl_resource_create(client, &xdg_toplevel_interface,
						      wl_resource_get_version(res), id);
	if (!tres) {
		wl_client_post_no_memory(client);
		return;
	}
	struct rei_xdg_toplevel *tl = rei_xcalloc(1, sizeof *tl);
	tl->resource = tres;
	tl->xs = xs;
	xs->toplevel = tl;
	wl_resource_set_implementation(tres, &toplevel_impl, tl, toplevel_resource_destroy);

	if (wl_resource_get_version(tres) >= XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION) {
		struct wl_array caps;
		wl_array_init(&caps);  /* None yet: no window menu, maximize, fullscreen or minimize. */
		xdg_toplevel_send_wm_capabilities(tres, &caps);
		wl_array_release(&caps);
	}
}

static void xdg_surface_get_popup(struct wl_client *client, struct wl_resource *res, uint32_t id,
				  struct wl_resource *parent_res, struct wl_resource *positioner_res)
{
	struct rei_xdg_surface *xs = wl_resource_get_user_data(res);
	if (xs->toplevel || xs->popup) {
		wl_resource_post_error(res, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
				       "xdg_surface already has a role object");
		return;
	}
	struct rei_xdg_surface *parent = parent_res ? wl_resource_get_user_data(parent_res) : NULL;
	if (!parent) {
		/* Parentless popups need xdg-foreign style extensions we don't offer. */
		wl_resource_post_error(xs->wm_base, XDG_WM_BASE_ERROR_INVALID_POSITIONER,
				       "popups require a parent");
		return;
	}
	const struct rei_positioner *pos = wl_resource_get_user_data(positioner_res);
	if (pos->w <= 0 || pos->h <= 0) {
		wl_resource_post_error(xs->wm_base, XDG_WM_BASE_ERROR_INVALID_POSITIONER,
				       "positioner has no size");
		return;
	}
	if (!xs->surface || !rei_surface_set_role(xs->surface, REI_ROLE_XDG_POPUP)) {
		wl_resource_post_error(xs->wm_base, XDG_WM_BASE_ERROR_ROLE,
				       "surface already has a different role");
		return;
	}

	struct wl_resource *pres = wl_resource_create(client, &xdg_popup_interface,
						      wl_resource_get_version(res), id);
	if (!pres) {
		wl_client_post_no_memory(client);
		return;
	}
	struct rei_xdg_popup *p = rei_xcalloc(1, sizeof *p);
	p->resource = pres;
	p->xs = xs;
	p->parent = parent;
	p->pos = *pos;
	positioner_compute(&p->pos, &p->x, &p->y);
	xs->popup = p;

	p->parent_destroy.notify = popup_on_parent_destroyed;
	wl_resource_add_destroy_listener(parent->resource, &p->parent_destroy);
	wl_list_insert(xs->server->popups.prev, &p->link);
	wl_resource_set_implementation(pres, &popup_impl, p, popup_resource_destroy);
}

static void xdg_surface_set_window_geometry(struct wl_client *c, struct wl_resource *r,
					    int32_t x, int32_t y, int32_t w, int32_t h)
{
	(void)c;
	struct rei_xdg_surface *xs = wl_resource_get_user_data(r);
	if (!xs->toplevel && !xs->popup) {
		wl_resource_post_error(r, XDG_SURFACE_ERROR_NOT_CONSTRUCTED, "no role object yet");
		return;
	}
	if (w <= 0 || h <= 0)
		return;  /* The spec says to ignore an invalid size. */
	xs->pending_geometry = (struct rei_rect){ x, y, w, h };
	xs->has_pending_geometry = true;
}

static void xdg_surface_ack_configure(struct wl_client *c, struct wl_resource *r, uint32_t serial)
{
	(void)c;
	struct rei_xdg_surface *xs = wl_resource_get_user_data(r);
	for (int i = 0; i < xs->n_sent; i++) {
		if (xs->sent[i] != serial)
			continue;
		/* Acking a serial also covers every earlier configure. */
		memmove(xs->sent, xs->sent + i + 1, (size_t)(xs->n_sent - i - 1) * sizeof xs->sent[0]);
		xs->n_sent -= i + 1;
		xs->configured = true;
		return;
	}
	wl_resource_post_error(r, XDG_SURFACE_ERROR_INVALID_SERIAL,
			       "serial %u does not match any pending configure", serial);
}

static const struct xdg_surface_interface xdg_surface_impl = {
	.destroy = xdg_surface_destroy_request,
	.get_toplevel = xdg_surface_get_toplevel,
	.get_popup = xdg_surface_get_popup,
	.set_window_geometry = xdg_surface_set_window_geometry,
	.ack_configure = xdg_surface_ack_configure,
};

static void xdg_surface_on_surface_destroyed(struct wl_listener *l, void *data)
{
	(void)data;
	struct rei_xdg_surface *xs = wl_container_of(l, xs, surface_destroy);
	wl_list_remove(&xs->surface_destroy.link);
	wl_list_init(&xs->surface_destroy.link);
	unmap_xdg_surface(xs);
	xs->surface = NULL;
}

static void xdg_surface_resource_destroy(struct wl_resource *r)
{
	struct rei_xdg_surface *xs = wl_resource_get_user_data(r);

	/* Take the window off the screen and cut the role object loose; it may
	 * outlive us (see toplevel_resource_destroy). */
	unmap_xdg_surface(xs);
	if (xs->toplevel)
		xs->toplevel->xs = NULL;
	if (xs->popup) {
		xs->popup->xs = NULL;
		/* Stop it being drawn; its own destroy handler tolerates a second
		 * unlink because the link is re-initialised. */
		wl_list_remove(&xs->popup->link);
		wl_list_init(&xs->popup->link);
	}
	if (xs->surface) {
		wl_list_remove(&xs->surface_destroy.link);
		xs->surface->role_commit = NULL;
		xs->surface->role_data = NULL;
	}
	free(xs);
}

/* ------------------------------------------------------------------------ */
/* xdg_wm_base                                                              */
/* ------------------------------------------------------------------------ */

static void wm_base_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void wm_base_create_positioner(struct wl_client *client, struct wl_resource *res, uint32_t id)
{
	struct wl_resource *pres = wl_resource_create(client, &xdg_positioner_interface,
						      wl_resource_get_version(res), id);
	if (!pres) {
		wl_client_post_no_memory(client);
		return;
	}
	struct rei_positioner *p = rei_xcalloc(1, sizeof *p);
	wl_resource_set_implementation(pres, &positioner_impl, p, positioner_resource_destroy);
}

static void wm_base_get_xdg_surface(struct wl_client *client, struct wl_resource *res,
				    uint32_t id, struct wl_resource *surface_res)
{
	struct rei_surface *surface = rei_surface_from_resource(surface_res);
	if (surface->role_commit || surface->content || surface->pending.buffer_set) {
		/* Already has an xdg_surface, or already has content (not allowed). */
		wl_resource_post_error(res, XDG_WM_BASE_ERROR_ROLE,
				       "surface already has an xdg_surface or a buffer");
		return;
	}

	struct wl_resource *xres = wl_resource_create(client, &xdg_surface_interface,
						      wl_resource_get_version(res), id);
	if (!xres) {
		wl_client_post_no_memory(client);
		return;
	}
	struct rei_xdg_surface *xs = rei_xcalloc(1, sizeof *xs);
	xs->server = wl_resource_get_user_data(res);
	xs->resource = xres;
	xs->wm_base = res;
	xs->surface = surface;

	xs->surface_destroy.notify = xdg_surface_on_surface_destroyed;
	wl_signal_add(&surface->destroy_signal, &xs->surface_destroy);
	surface->role_commit = xdg_surface_commit;
	surface->role_data = xs;
	wl_resource_set_implementation(xres, &xdg_surface_impl, xs, xdg_surface_resource_destroy);
}

static void wm_base_pong(struct wl_client *c, struct wl_resource *r, uint32_t serial)
{
	(void)c; (void)r; (void)serial;
}

static const struct xdg_wm_base_interface wm_base_impl = {
	.destroy = wm_base_destroy_request,
	.create_positioner = wm_base_create_positioner,
	.get_xdg_surface = wm_base_get_xdg_surface,
	.pong = wm_base_pong,
};

static void wm_base_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *res = wl_resource_create(client, &xdg_wm_base_interface, (int)version, id);
	if (!res) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(res, &wm_base_impl, data, NULL);
}

bool rei_xdg_shell_init(struct rei_server *server)
{
	return wl_global_create(server->display, &xdg_wm_base_interface, 5, server, wm_base_bind) != NULL;
}

/* ------------------------------------------------------------------------ */
/* Scene                                                                    */
/* ------------------------------------------------------------------------ */

static void emit_xs(const struct rei_xdg_surface *xs, struct rei_scene_snapshot *snap)
{
	int32_t gx, gy;
	if (!xs->surface || !xs->mapped || !xs_geometry_origin(xs, &gx, &gy))
		return;
	struct rei_rect g = xs_geometry(xs);
	rei_surface_emit(xs->surface, (float)(gx - g.x), (float)(gy - g.y), snap);
}

void rei_xdg_emit_view(const struct rei_view *view, struct rei_scene_snapshot *snap)
{
	emit_xs(view->toplevel->xs, snap);

	/* Popups are kept in creation order, so a popup always follows its parent. */
	const struct rei_xdg_popup *p;
	wl_list_for_each(p, &view->server->popups, link) {
		if (p->xs && popup_root(p->xs) == view->toplevel->xs)
			emit_xs(p->xs, snap);
	}
}

bool rei_xdg_surface_rect(struct rei_surface *root, struct rei_rect *out)
{
	if (root->role != REI_ROLE_XDG_TOPLEVEL && root->role != REI_ROLE_XDG_POPUP)
		return false;
	if (!root->role_data)
		return false;
	const struct rei_xdg_surface *xs = root->role_data;
	int32_t gx, gy;
	if (!xs->mapped || !xs_geometry_origin(xs, &gx, &gy))
		return false;
	struct rei_rect g = xs_geometry(xs);
	*out = (struct rei_rect){ gx - g.x, gy - g.y, root->width, root->height };
	return true;
}
