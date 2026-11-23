#include "proto/pointer_constraints.h"

#include <math.h>
#include <stdlib.h>
#include <wayland-server.h>

#include "input/input.h"
#include "pointer-constraints-unstable-v1-server-protocol.h"
#include "proto/surface.h"
#include "relative-pointer-unstable-v1-server-protocol.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"

struct rei_pointer_constraints {
	struct rei_server *server;
	struct wl_list constraints;   /* struct constraint */
	struct wl_list relatives;     /* struct relative */
	struct wl_listener display_destroy;
};

struct constraint {
	struct rei_pointer_constraints *pc;
	struct wl_resource *resource;
	struct rei_surface *surface;   /* NULL once it is gone: the constraint is inert */
	bool lock, oneshot, active;
	bool has_region;
	struct rei_rect region;        /* the bounding box of what the program asked for */
	bool has_hint;
	double hint_x, hint_y;
	struct wl_listener surface_destroy;
	struct wl_list link;
};

struct relative {
	struct wl_resource *resource;
	struct wl_list link;
};

/* ---- constraints ---- */

static void constraint_detach_surface(struct constraint *c)
{
	if (c->surface) {
		wl_list_remove(&c->surface_destroy.link);
		c->surface = NULL;
	}
}

static void send_stop(struct constraint *c)
{
	if (c->lock)
		zwp_locked_pointer_v1_send_unlocked(c->resource);
	else
		zwp_confined_pointer_v1_send_unconfined(c->resource);
}

/* Ends an active constraint; a oneshot one is over for good. A lock's position hint moves the cursor (that is how
 * XWarpPointer is built); `warp` also has the pointer's whole state follow it at once. */
static void deactivate(struct constraint *c, double ox, double oy, bool warp)
{
	if (!c->active)
		return;
	c->active = false;
	send_stop(c);
	if (c->lock && c->has_hint && c->surface) {
		struct rei_server *s = c->pc->server;
		double x = ox + c->hint_x, y = oy + c->hint_y;
		if (warp)
			rei_input_warp(s, x, y);
		else {
			s->cursor_x = x;
			s->cursor_y = y;
			rei_input_clamp_cursor(s);
			rei_server_cursor_moved(s);
		}
	}
	if (c->oneshot)
		constraint_detach_surface(c);   /* (it stays listed but inert; the program destroys the object) */
}

static void on_surface_destroyed(struct wl_listener *l, void *data)
{
	(void)data;
	struct constraint *c = wl_container_of(l, c, surface_destroy);
	wl_list_remove(&c->surface_destroy.link);
	c->surface = NULL;
	c->active = false;
}

static void constraint_resource_destroyed(struct wl_resource *r)
{
	struct constraint *c = wl_resource_get_user_data(r);
	struct rei_server *s = c->pc->server;
	/* Destroying an active lock with a hint is XWarpPointer: the cursor moves there now. */
	struct rei_input *in = s->input;
	bool was_active = c->active;
	struct rei_surface *surface = c->surface;
	wl_list_remove(&c->link);
	c->active = false;
	if (was_active && c->lock && c->has_hint && surface && in) {
		double x = in->focus_ox + c->hint_x, y = in->focus_oy + c->hint_y;
		constraint_detach_surface(c);
		free(c);
		if (!s->shutting_down)
			rei_input_warp(s, x, y);
		return;
	}
	constraint_detach_surface(c);
	free(c);
}

static void set_region_from(struct constraint *c, struct wl_resource *region)
{
	c->has_region = false;
	if (!region)
		return;
	const struct rei_rects *rs = wl_resource_get_user_data(region);
	if (!rs || rs->n == 0)
		return;
	struct rei_rect b = rs->items[0];
	for (size_t i = 1; i < rs->n; i++) {
		int32_t x2 = fmax(b.x + b.w, rs->items[i].x + rs->items[i].w);
		int32_t y2 = fmax(b.y + b.h, rs->items[i].y + rs->items[i].h);
		b.x = fmin(b.x, rs->items[i].x);
		b.y = fmin(b.y, rs->items[i].y);
		b.w = x2 - b.x;
		b.h = y2 - b.y;
	}
	c->region = b;
	c->has_region = true;
}

static void activate(struct constraint *c)
{
	c->active = true;
	if (c->lock)
		zwp_locked_pointer_v1_send_locked(c->resource);
	else
		zwp_confined_pointer_v1_send_confined(c->resource);
}

static void request_destroy(struct wl_client *client, struct wl_resource *r)
{
	(void)client;
	wl_resource_destroy(r);
}

static void locked_set_hint(struct wl_client *client, struct wl_resource *r, wl_fixed_t x, wl_fixed_t y)
{
	(void)client;
	struct constraint *c = wl_resource_get_user_data(r);
	c->has_hint = true;
	c->hint_x = wl_fixed_to_double(x);
	c->hint_y = wl_fixed_to_double(y);
}

static void locked_set_region(struct wl_client *client, struct wl_resource *r, struct wl_resource *region)
{
	(void)client;
	set_region_from(wl_resource_get_user_data(r), region);
}

static const struct zwp_locked_pointer_v1_interface locked_impl = {
	.destroy = request_destroy,
	.set_cursor_position_hint = locked_set_hint,
	.set_region = locked_set_region,
};

static const struct zwp_confined_pointer_v1_interface confined_impl = {
	.destroy = request_destroy,
	.set_region = locked_set_region,
};

static void create_constraint(struct wl_client *client, struct wl_resource *manager, uint32_t id,
			      struct wl_resource *surface_res, struct wl_resource *region, uint32_t lifetime, bool lock)
{
	struct rei_pointer_constraints *pc = wl_resource_get_user_data(manager);
	struct rei_surface *surface = rei_surface_from_resource(surface_res);
	struct constraint *other;
	wl_list_for_each(other, &pc->constraints, link) {
		if (other->surface == surface) {
			wl_resource_post_error(manager, ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED,
					       "the surface already has a pointer constraint");
			return;
		}
	}
	struct constraint *c = rei_xcalloc(1, sizeof *c);
	c->pc = pc;
	c->lock = lock;
	c->oneshot = lifetime == ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT;
	c->resource = wl_resource_create(client, lock ? &zwp_locked_pointer_v1_interface : &zwp_confined_pointer_v1_interface,
					 wl_resource_get_version(manager), id);
	if (!c->resource) {
		free(c);
		wl_client_post_no_memory(client);
		return;
	}
	c->surface = surface;
	c->surface_destroy.notify = on_surface_destroyed;
	wl_signal_add(&surface->destroy_signal, &c->surface_destroy);
	set_region_from(c, region);
	wl_list_insert(&pc->constraints, &c->link);
	wl_resource_set_implementation(c->resource, lock ? (const void *)&locked_impl : (const void *)&confined_impl, c,
				       constraint_resource_destroyed);
	/* It starts at once if the surface already has the pointer and the keyboard. */
	struct rei_input *in = pc->server->input;
	rei_debug("pointer %s requested (%s)", lock ? "lock" : "confinement", c->oneshot ? "oneshot" : "persistent");
	if (in && in->pointer_focus == surface && in->keyboard_focus == surface)
		activate(c);
}

static void manager_lock_pointer(struct wl_client *client, struct wl_resource *r, uint32_t id, struct wl_resource *surface,
				 struct wl_resource *pointer, struct wl_resource *region, uint32_t lifetime)
{
	(void)pointer;
	create_constraint(client, r, id, surface, region, lifetime, true);
}

static void manager_confine_pointer(struct wl_client *client, struct wl_resource *r, uint32_t id,
				    struct wl_resource *surface, struct wl_resource *pointer, struct wl_resource *region,
				    uint32_t lifetime)
{
	(void)pointer;
	create_constraint(client, r, id, surface, region, lifetime, false);
}

static const struct zwp_pointer_constraints_v1_interface constraints_impl = {
	.destroy = request_destroy,
	.lock_pointer = manager_lock_pointer,
	.confine_pointer = manager_confine_pointer,
};

static void bind_constraints(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(client, &zwp_pointer_constraints_v1_interface, (int)version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &constraints_impl, data, NULL);
}

/* A constraint holds while its surface has both the pointer and the keyboard: a program that has lost the keyboard (another
 * window took focus) is not using the mouse either, and a locked pointer would leave nobody able to click it back. */
void rei_pointer_constraints_update(struct rei_server *s, double old_ox, double old_oy)
{
	struct rei_pointer_constraints *pc = s->pconstraints;
	struct rei_input *in = s->input;
	if (!pc || !in)
		return;
	struct constraint *c, *tmp;
	wl_list_for_each_safe(c, tmp, &pc->constraints, link) {
		bool want = c->surface && c->surface == in->pointer_focus && c->surface == in->keyboard_focus;
		if (c->active && !want) {
			rei_debug("pointer %s ends (pointer focus %s, keyboard focus %s)", c->lock ? "lock" : "confinement",
				  c->surface == in->pointer_focus ? "kept" : "gone", c->surface == in->keyboard_focus ? "kept" : "gone");
			deactivate(c, old_ox, old_oy, false);
		} else if (!c->active && want) {
			rei_debug("pointer %s starts", c->lock ? "lock" : "confinement");
			activate(c);
		}
	}
}

void rei_pointer_constraints_focus(struct rei_server *s, struct rei_surface *surface, double ox, double oy)
{
	(void)surface;
	rei_pointer_constraints_update(s, ox, oy);
}

bool rei_pointer_locked(const struct rei_server *s)
{
	const struct rei_pointer_constraints *pc = s->pconstraints;
	if (!pc)
		return false;
	const struct constraint *c;
	wl_list_for_each(c, &pc->constraints, link)
		if (c->active && c->lock)
			return true;
	return false;
}

void rei_pointer_confine(const struct rei_server *s, double ox, double oy, double *x, double *y)
{
	const struct rei_pointer_constraints *pc = s->pconstraints;
	if (!pc)
		return;
	const struct constraint *c;
	wl_list_for_each(c, &pc->constraints, link) {
		if (!c->active || c->lock || !c->surface)
			continue;
		double x0 = 0, y0 = 0, x1 = c->surface->width, y1 = c->surface->height;
		if (c->has_region) {
			x0 = fmax(x0, c->region.x);
			y0 = fmax(y0, c->region.y);
			x1 = fmin(x1, c->region.x + c->region.w);
			y1 = fmin(y1, c->region.y + c->region.h);
		}
		if (x1 <= x0 || y1 <= y0)
			return;
		*x = fmin(fmax(*x, ox + x0), ox + x1 - 1);
		*y = fmin(fmax(*y, oy + y0), oy + y1 - 1);
		return;
	}
}

/* ---- relative motion ---- */

static void relative_resource_destroyed(struct wl_resource *r)
{
	struct relative *rel = wl_resource_get_user_data(r);
	wl_list_remove(&rel->link);
	free(rel);
}

static const struct zwp_relative_pointer_v1_interface relative_impl = {
	.destroy = request_destroy,
};

static void relative_get(struct wl_client *client, struct wl_resource *manager, uint32_t id, struct wl_resource *pointer)
{
	(void)pointer;
	struct rei_pointer_constraints *pc = wl_resource_get_user_data(manager);
	struct relative *rel = rei_xcalloc(1, sizeof *rel);
	rel->resource = wl_resource_create(client, &zwp_relative_pointer_v1_interface, wl_resource_get_version(manager), id);
	if (!rel->resource) {
		free(rel);
		wl_client_post_no_memory(client);
		return;
	}
	wl_list_insert(&pc->relatives, &rel->link);
	wl_resource_set_implementation(rel->resource, &relative_impl, rel, relative_resource_destroyed);
}

static const struct zwp_relative_pointer_manager_v1_interface relative_manager_impl = {
	.destroy = request_destroy,
	.get_relative_pointer = relative_get,
};

static void bind_relative(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(client, &zwp_relative_pointer_manager_v1_interface, (int)version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &relative_manager_impl, data, NULL);
}

void rei_relative_pointer_motion(struct rei_server *s, struct rei_surface *surface, uint64_t time_usec, double dx,
				 double dy, double ux, double uy)
{
	struct rei_pointer_constraints *pc = s->pconstraints;
	if (!pc || !surface)
		return;
	struct wl_client *client = wl_resource_get_client(surface->resource);
	struct relative *rel;
	wl_list_for_each(rel, &pc->relatives, link) {
		if (wl_resource_get_client(rel->resource) != client)
			continue;
		zwp_relative_pointer_v1_send_relative_motion(rel->resource, (uint32_t)(time_usec >> 32),
							     (uint32_t)(time_usec & 0xffffffffu), wl_fixed_from_double(dx),
							     wl_fixed_from_double(dy), wl_fixed_from_double(ux),
							     wl_fixed_from_double(uy));
	}
}

/* ---- setup ---- */

static void on_display_destroy(struct wl_listener *l, void *data)
{
	(void)data;
	struct rei_pointer_constraints *pc = wl_container_of(l, pc, display_destroy);
	pc->server->pconstraints = NULL;
	free(pc);
}

bool rei_pointer_constraints_init(struct rei_server *s)
{
	struct rei_pointer_constraints *pc = rei_xcalloc(1, sizeof *pc);
	pc->server = s;
	wl_list_init(&pc->constraints);
	wl_list_init(&pc->relatives);
	pc->display_destroy.notify = on_display_destroy;
	wl_display_add_destroy_listener(s->display, &pc->display_destroy);
	s->pconstraints = pc;
	return wl_global_create(s->display, &zwp_pointer_constraints_v1_interface, 1, pc, bind_constraints) &&
	       wl_global_create(s->display, &zwp_relative_pointer_manager_v1_interface, 1, pc, bind_relative);
}
