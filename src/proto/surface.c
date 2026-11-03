#include "proto/surface.h"

#include <stdlib.h>
#include <string.h>
#include <wayland-server-protocol.h>

#include "server.h"
#include "proto/linux_dmabuf.h"
#include "util/log.h"
#include "util/util.h"
#include "util/wlsignal.h"

/* ------------------------------------------------------------------------ */
/* Rectangle sets                                                           */
/* ------------------------------------------------------------------------ */

void rei_rects_clear(struct rei_rects *r)
{
	free(r->items);
	r->items = NULL;
	r->n = r->cap = 0;
}

void rei_rects_add(struct rei_rects *r, struct rei_rect rect)
{
	if (rect.w <= 0 || rect.h <= 0)
		return;
	if (r->n == r->cap) {
		r->cap = r->cap ? r->cap * 2 : 4;
		r->items = rei_xrealloc(r->items, r->cap * sizeof *r->items);
	}
	r->items[r->n++] = rect;
}

void rei_rects_subtract(struct rei_rects *r, struct rei_rect hole)
{
	struct rei_rects out = { 0 };
	int32_t hx2 = hole.x + hole.w, hy2 = hole.y + hole.h;

	for (size_t i = 0; i < r->n; i++) {
		struct rei_rect a = r->items[i];
		int32_t ax2 = a.x + a.w, ay2 = a.y + a.h;
		if (hole.x >= ax2 || hx2 <= a.x || hole.y >= ay2 || hy2 <= a.y) {
			rei_rects_add(&out, a);  /* No overlap. */
			continue;
		}
		/* Up to four pieces around the hole: above, below, left, right. */
		int32_t top = hole.y > a.y ? hole.y : a.y;
		int32_t bottom = hy2 < ay2 ? hy2 : ay2;
		rei_rects_add(&out, (struct rei_rect){ a.x, a.y, a.w, hole.y - a.y });
		rei_rects_add(&out, (struct rei_rect){ a.x, hy2, a.w, ay2 - hy2 });
		rei_rects_add(&out, (struct rei_rect){ a.x, top, hole.x - a.x, bottom - top });
		rei_rects_add(&out, (struct rei_rect){ hx2, top, ax2 - hx2, bottom - top });
	}
	rei_rects_clear(r);
	*r = out;
}

/* True if one rectangle alone covers [0,w)x[0,h). A union of rectangles that
 * covers the area is missed, which only costs a blend, never correctness. */
bool rei_rects_cover(const struct rei_rects *r, int32_t w, int32_t h)
{
	for (size_t i = 0; i < r->n; i++) {
		const struct rei_rect *a = &r->items[i];
		if (a->x <= 0 && a->y <= 0 && a->x + a->w >= w && a->y + a->h >= h)
			return true;
	}
	return false;
}

/* ------------------------------------------------------------------------ */
/* wl_region                                                                */
/* ------------------------------------------------------------------------ */

static void region_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void region_add(struct wl_client *c, struct wl_resource *r,
		       int32_t x, int32_t y, int32_t w, int32_t h)
{
	(void)c;
	rei_rects_add(wl_resource_get_user_data(r), (struct rei_rect){ x, y, w, h });
}

static void region_subtract(struct wl_client *c, struct wl_resource *r,
			    int32_t x, int32_t y, int32_t w, int32_t h)
{
	(void)c;
	rei_rects_subtract(wl_resource_get_user_data(r), (struct rei_rect){ x, y, w, h });
}

static const struct wl_region_interface region_impl = {
	.destroy = region_destroy_request,
	.add = region_add,
	.subtract = region_subtract,
};

static void region_resource_destroy(struct wl_resource *r)
{
	struct rei_rects *rects = wl_resource_get_user_data(r);
	rei_rects_clear(rects);
	free(rects);
}

/* ------------------------------------------------------------------------ */
/* Surface state                                                            */
/* ------------------------------------------------------------------------ */

static void state_init(struct rei_surface_state *st)
{
	memset(st, 0, sizeof *st);
	wl_list_init(&st->frames);
	wl_list_init(&st->buffer_destroy.link);
}

static void frames_destroy_all(struct wl_list *frames)
{
	struct rei_frame_cb *cb, *tmp;
	wl_list_for_each_safe(cb, tmp, frames, link) {
		wl_list_remove(&cb->link);
		wl_resource_set_user_data(cb->resource, NULL);
		free(cb);
	}
}

/* Content is either CPU pixels or a GPU buffer; these keep the pair (and the
 * lease that holds the wl_buffer) together. */
static void content_release(struct rei_server *server, struct rei_pixbuf **pix,
			    struct rei_dmabuf **dmabuf, struct rei_buffer_lease **lease)
{
	rei_pixbuf_unref(*pix);
	rei_dmabuf_unref(*dmabuf);
	rei_buffer_lease_retire(server, *lease);
	*pix = NULL;
	*dmabuf = NULL;
	*lease = NULL;
}

static bool content_size(const struct rei_pixbuf *pix, const struct rei_dmabuf *dmabuf,
			 int32_t *w, int32_t *h)
{
	if (pix) {
		*w = pix->width;
		*h = pix->height;
		return true;
	}
	if (dmabuf) {
		*w = dmabuf->width;
		*h = dmabuf->height;
		return true;
	}
	return false;
}

static void state_clear(struct rei_server *server, struct rei_surface_state *st)
{
	if (st->buffer_set && st->buffer)
		wl_list_remove(&st->buffer_destroy.link);
	content_release(server, &st->content, &st->dmabuf, &st->lease);
	rei_rects_clear(&st->opaque);
	rei_rects_clear(&st->input);
	frames_destroy_all(&st->frames);
	state_init(st);
}

/* Moves everything `src` has set into `dst`, leaving `src` empty. */
static void state_merge(struct rei_server *server, struct rei_surface_state *dst,
			struct rei_surface_state *src)
{
	if (src->content_set) {
		content_release(server, &dst->content, &dst->dmabuf, &dst->lease);
		dst->content = src->content;
		dst->dmabuf = src->dmabuf;
		dst->lease = src->lease;
		dst->content_set = true;
		src->content = NULL;
		src->dmabuf = NULL;
		src->lease = NULL;
	}
	if (src->scale_set) {
		dst->scale = src->scale;
		dst->scale_set = true;
	}
	if (src->transform_set) {
		dst->transform = src->transform;
		dst->transform_set = true;
	}
	if (src->opaque_set) {
		rei_rects_clear(&dst->opaque);
		dst->opaque = src->opaque;
		src->opaque = (struct rei_rects){ 0 };
		dst->opaque_set = true;
	}
	if (src->input_set) {
		rei_rects_clear(&dst->input);
		dst->input = src->input;
		src->input = (struct rei_rects){ 0 };
		dst->input_set = true;
	}
	wl_list_insert_list(dst->frames.prev, &src->frames);
	wl_list_init(&src->frames);

	src->content_set = src->scale_set = src->transform_set = false;
	src->opaque_set = src->input_set = false;
}

/* ------------------------------------------------------------------------ */
/* Subsurface helpers                                                       */
/* ------------------------------------------------------------------------ */

static bool effectively_sync(const struct rei_surface *s)
{
	while (s->sub) {
		if (s->sub->sync)
			return true;
		if (!s->sub->parent)
			return false;
		s = s->sub->parent;
	}
	return false;
}

struct rei_surface *rei_surface_root(struct rei_surface *s)
{
	while (s->sub && s->sub->parent)
		s = s->sub->parent;
	return s;
}

/* ------------------------------------------------------------------------ */
/* Commit                                                                   */
/* ------------------------------------------------------------------------ */

static void apply_state(struct rei_surface *s, struct rei_surface_state *st)
{
	if (st->content_set) {
		content_release(s->server, &s->content, &s->dmabuf, &s->lease);
		s->content = st->content;
		s->dmabuf = st->dmabuf;
		s->lease = st->lease;
		st->content = NULL;
		st->dmabuf = NULL;
		st->lease = NULL;
		st->content_set = false;
	}
	if (st->scale_set) {
		s->scale = st->scale;
		st->scale_set = false;
	}
	if (st->transform_set) {
		s->transform = st->transform;
		st->transform_set = false;
	}
	int32_t cw, ch;
	if (content_size(s->content, s->dmabuf, &cw, &ch)) {
		s->width = cw / s->scale;
		s->height = ch / s->scale;
	} else {
		s->width = s->height = 0;
	}
	if (st->opaque_set) {
		rei_rects_clear(&s->opaque);
		s->opaque = st->opaque;
		st->opaque = (struct rei_rects){ 0 };
		st->opaque_set = false;
	}
	if (st->input_set) {
		s->input_default = false;
		rei_rects_clear(&s->input);
		s->input = st->input;
		st->input = (struct rei_rects){ 0 };
		st->input_set = false;
	}

	/* The next snapshot built will carry this commit. */
	struct rei_frame_cb *cb;
	wl_list_for_each(cb, &st->frames, link)
		cb->min_serial = s->server->scene_serial + 1;
	wl_list_insert_list(s->frames.prev, &st->frames);
	wl_list_init(&st->frames);

	if (s->role_commit)
		s->role_commit(s, s->role_data);

	/* Committing a parent releases its synchronized children. */
	struct rei_subsurface *sub;
	wl_list_for_each(sub, &s->subs, link) {
		if (sub == s->self_marker || !sub->surface)
			continue;
		if (sub->pending_pos) {
			sub->x = sub->pending_x;
			sub->y = sub->pending_y;
			sub->pending_pos = false;
		}
		if (sub->surface->has_cached) {
			sub->surface->has_cached = false;
			apply_state(sub->surface, &sub->surface->cached);
		}
	}

	rei_server_schedule_damage(s->server);
}

/* Turns the attached wl_buffer into pixels and releases it. */
static bool latch_buffer(struct rei_surface *s)
{
	struct rei_surface_state *p = &s->pending;
	if (!p->buffer_set)
		return true;

	if (!p->buffer) {
		p->content = NULL;
		p->dmabuf = NULL;
		p->lease = NULL;
		p->content_set = true;
		p->buffer_set = false;
		return true;
	}

	int32_t scale = p->scale_set ? p->scale : s->scale;

	struct rei_dmabuf *gpu = rei_dmabuf_from_buffer(p->buffer);
	if (gpu) {
		if (gpu->width % scale != 0 || gpu->height % scale != 0) {
			wl_resource_post_error(s->resource, WL_SURFACE_ERROR_INVALID_SIZE,
					       "buffer size %dx%d is not a multiple of scale %d",
					       gpu->width, gpu->height, scale);
			return false;
		}
		/* The client keeps the buffer until we release it, which waits for
		 * the frame that stops showing it. */
		p->dmabuf = rei_dmabuf_ref(gpu);
		p->lease = rei_buffer_lease_new(p->buffer);
		p->content = NULL;
		wl_list_remove(&p->buffer_destroy.link);
		p->buffer = NULL;
		p->buffer_set = false;
		p->content_set = true;
		return true;
	}

	struct wl_shm_buffer *shm = wl_shm_buffer_get(p->buffer);
	if (!shm) {
		wl_resource_post_error(s->resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "unsupported buffer type (wl_shm and linux-dmabuf only)");
		return false;
	}

	int32_t w = wl_shm_buffer_get_width(shm);
	int32_t h = wl_shm_buffer_get_height(shm);
	int32_t stride = wl_shm_buffer_get_stride(shm);
	uint32_t format = wl_shm_buffer_get_format(shm);
	if (w % scale != 0 || h % scale != 0) {
		wl_resource_post_error(s->resource, WL_SURFACE_ERROR_INVALID_SIZE,
				       "buffer size %dx%d is not a multiple of scale %d",
				       w, h, scale);
		return false;
	}

	struct rei_pixbuf *pix = rei_pixbuf_create(w, h, format == WL_SHM_FORMAT_XRGB8888);
	if (!pix) {
		wl_resource_post_error(s->resource, WL_DISPLAY_ERROR_NO_MEMORY,
				       "cannot allocate %dx%d surface", w, h);
		return false;
	}

	/* begin_access arranges for a client truncating its pool to read as zeros
	 * instead of killing us with SIGBUS. */
	wl_shm_buffer_begin_access(shm);
	const uint8_t *src = wl_shm_buffer_get_data(shm);
	for (int32_t y = 0; y < h; y++)
		memcpy(pix->data + (size_t)y * (size_t)w * 4, src + (size_t)y * (size_t)stride,
		       (size_t)w * 4);
	wl_shm_buffer_end_access(shm);

	/* Everything we need is copied, so the client can reuse its buffer now. */
	wl_buffer_send_release(p->buffer);
	wl_list_remove(&p->buffer_destroy.link);
	p->buffer = NULL;
	p->buffer_set = false;
	p->content = pix;
	p->content_set = true;
	return true;
}

static void surface_commit(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	struct rei_surface *s = wl_resource_get_user_data(resource);

	if (!latch_buffer(s))
		return;

	if (s->sub && effectively_sync(s)) {
		state_merge(s->server, &s->cached, &s->pending);
		s->has_cached = true;
	} else {
		apply_state(s, &s->pending);
	}
}

/* ------------------------------------------------------------------------ */
/* wl_surface requests                                                      */
/* ------------------------------------------------------------------------ */

static void on_pending_buffer_destroyed(struct wl_listener *l, void *data)
{
	(void)data;
	struct rei_surface_state *st = wl_container_of(l, st, buffer_destroy);
	wl_list_remove(&st->buffer_destroy.link);
	wl_list_init(&st->buffer_destroy.link);
	st->buffer = NULL;
	st->buffer_set = false;
}

static void surface_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void surface_attach(struct wl_client *c, struct wl_resource *resource,
			   struct wl_resource *buffer, int32_t x, int32_t y)
{
	(void)c;
	(void)x;
	(void)y;
	struct rei_surface *s = wl_resource_get_user_data(resource);
	struct rei_surface_state *p = &s->pending;

	if (p->buffer_set && p->buffer)
		wl_list_remove(&p->buffer_destroy.link);
	p->buffer = buffer;
	p->buffer_set = true;
	if (buffer) {
		p->buffer_destroy.notify = on_pending_buffer_destroyed;
		wl_resource_add_destroy_listener(buffer, &p->buffer_destroy);
	}
}

static void surface_damage(struct wl_client *c, struct wl_resource *r,
			   int32_t x, int32_t y, int32_t w, int32_t h)
{
	/* The whole buffer is re-uploaded on every commit for now, so damage
	 * carries no information we use. */
	(void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}

static void callback_resource_destroy(struct wl_resource *res)
{
	/* NULL once the surface has already let go of it. */
	struct rei_frame_cb *cb = wl_resource_get_user_data(res);
	if (!cb)
		return;
	wl_list_remove(&cb->link);
	free(cb);
}

static void surface_frame(struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
	struct rei_surface *s = wl_resource_get_user_data(resource);
	struct wl_resource *cb_res = wl_resource_create(client, &wl_callback_interface, 1, id);
	if (!cb_res) {
		wl_client_post_no_memory(client);
		return;
	}
	struct rei_frame_cb *cb = rei_xcalloc(1, sizeof *cb);
	cb->resource = cb_res;
	wl_list_insert(s->pending.frames.prev, &cb->link);
	wl_resource_set_implementation(cb_res, NULL, cb, callback_resource_destroy);
}

static void surface_set_opaque_region(struct wl_client *c, struct wl_resource *resource,
				      struct wl_resource *region)
{
	(void)c;
	struct rei_surface *s = wl_resource_get_user_data(resource);
	rei_rects_clear(&s->pending.opaque);
	if (region) {
		const struct rei_rects *src = wl_resource_get_user_data(region);
		for (size_t i = 0; i < src->n; i++)
			rei_rects_add(&s->pending.opaque, src->items[i]);
	}
	s->pending.opaque_set = true;
}

static void surface_set_input_region(struct wl_client *c, struct wl_resource *resource,
				     struct wl_resource *region)
{
	(void)c;
	struct rei_surface *s = wl_resource_get_user_data(resource);
	rei_rects_clear(&s->pending.input);
	if (region) {
		const struct rei_rects *src = wl_resource_get_user_data(region);
		for (size_t i = 0; i < src->n; i++)
			rei_rects_add(&s->pending.input, src->items[i]);
	} else {
		/* NULL means "infinite": the whole surface. */
		rei_rects_add(&s->pending.input, (struct rei_rect){ INT32_MIN / 2, INT32_MIN / 2,
								    INT32_MAX, INT32_MAX });
	}
	s->pending.input_set = true;
}

static void surface_set_buffer_transform(struct wl_client *c, struct wl_resource *resource,
					 int32_t transform)
{
	(void)c;
	struct rei_surface *s = wl_resource_get_user_data(resource);
	if (transform < 0 || transform > 7) {
		wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_TRANSFORM,
				       "invalid transform %d", transform);
		return;
	}
	s->pending.transform = transform;
	s->pending.transform_set = true;
}

static void surface_set_buffer_scale(struct wl_client *c, struct wl_resource *resource,
				     int32_t scale)
{
	(void)c;
	struct rei_surface *s = wl_resource_get_user_data(resource);
	if (scale < 1) {
		wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_SCALE,
				       "invalid scale %d", scale);
		return;
	}
	s->pending.scale = scale;
	s->pending.scale_set = true;
}

static void surface_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y)
{
	(void)c; (void)r; (void)x; (void)y;
}

static const struct wl_surface_interface surface_impl = {
	.destroy = surface_destroy_request,
	.attach = surface_attach,
	.damage = surface_damage,
	.frame = surface_frame,
	.set_opaque_region = surface_set_opaque_region,
	.set_input_region = surface_set_input_region,
	.commit = surface_commit,
	.set_buffer_transform = surface_set_buffer_transform,
	.set_buffer_scale = surface_set_buffer_scale,
	.damage_buffer = surface_damage,
	.offset = surface_offset,
};

static void surface_resource_destroy(struct wl_resource *resource)
{
	struct rei_surface *s = wl_resource_get_user_data(resource);

	/* Listeners (xdg_surface, subsurface, input focus) may unlink each other as
	 * they react, so this must not be a plain wl_signal_emit(). */
	rei_signal_emit(&s->destroy_signal, s);

	/* Subsurfaces (as parent or child) detach themselves via destroy_signal. */

	wl_list_remove(&s->link);
	state_clear(s->server, &s->pending);
	state_clear(s->server, &s->cached);
	content_release(s->server, &s->content, &s->dmabuf, &s->lease);
	rei_rects_clear(&s->opaque);
	rei_rects_clear(&s->input);
	frames_destroy_all(&s->frames);
	free(s->self_marker);
	struct rei_server *server = s->server;
	free(s);
	rei_server_schedule_damage(server);
}

/* ------------------------------------------------------------------------ */
/* wl_compositor                                                            */
/* ------------------------------------------------------------------------ */

static void compositor_create_surface(struct wl_client *client,
				      struct wl_resource *compositor, uint32_t id)
{
	struct rei_server *server = wl_resource_get_user_data(compositor);
	struct wl_resource *res = wl_resource_create(client, &wl_surface_interface,
						     wl_resource_get_version(compositor), id);
	if (!res) {
		wl_client_post_no_memory(client);
		return;
	}

	struct rei_surface *s = rei_xcalloc(1, sizeof *s);
	s->server = server;
	s->resource = res;
	s->scale = 1;
	s->input_default = true;
	state_init(&s->pending);
	state_init(&s->cached);
	wl_list_init(&s->frames);
	wl_list_init(&s->subs);
	wl_signal_init(&s->destroy_signal);

	s->self_marker = rei_xcalloc(1, sizeof *s->self_marker);
	wl_list_insert(&s->subs, &s->self_marker->link);

	wl_list_insert(&server->surfaces, &s->link);
	wl_resource_set_implementation(res, &surface_impl, s, surface_resource_destroy);
}

static void compositor_create_region(struct wl_client *client,
				     struct wl_resource *compositor, uint32_t id)
{
	struct wl_resource *res = wl_resource_create(client, &wl_region_interface,
						     wl_resource_get_version(compositor), id);
	if (!res) {
		wl_client_post_no_memory(client);
		return;
	}
	struct rei_rects *rects = rei_xcalloc(1, sizeof *rects);
	wl_resource_set_implementation(res, &region_impl, rects, region_resource_destroy);
}

static const struct wl_compositor_interface compositor_impl = {
	.create_surface = compositor_create_surface,
	.create_region = compositor_create_region,
};

static void compositor_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *res = wl_resource_create(client, &wl_compositor_interface,
						     (int)version, id);
	if (!res) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(res, &compositor_impl, data, NULL);
}

/* ------------------------------------------------------------------------ */
/* wl_subsurface                                                            */
/* ------------------------------------------------------------------------ */

static void subsurface_unlink(struct rei_subsurface *sub)
{
	if (!wl_list_empty(&sub->link))
		wl_list_remove(&sub->link);
	wl_list_init(&sub->link);
}

static void subsurface_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static void subsurface_set_position(struct wl_client *c, struct wl_resource *r,
				    int32_t x, int32_t y)
{
	(void)c;
	struct rei_subsurface *sub = wl_resource_get_user_data(r);
	sub->pending_x = x;
	sub->pending_y = y;
	sub->pending_pos = true;
}

static struct rei_subsurface *sibling_entry(struct rei_subsurface *sub,
					    struct wl_resource *sibling_surface)
{
	struct rei_surface *sib = wl_resource_get_user_data(sibling_surface);
	if (!sub->parent)
		return NULL;
	if (sib == sub->parent)
		return sub->parent->self_marker;
	if (sib->sub && sib->sub->parent == sub->parent && sib->sub != sub)
		return sib->sub;
	return NULL;
}

static void subsurface_place_above(struct wl_client *c, struct wl_resource *r,
				   struct wl_resource *sibling)
{
	(void)c;
	struct rei_subsurface *sub = wl_resource_get_user_data(r);
	struct rei_subsurface *ref = sibling_entry(sub, sibling);
	if (!ref) {
		wl_resource_post_error(r, WL_SUBSURFACE_ERROR_BAD_SURFACE,
				       "sibling is not a sibling or the parent");
		return;
	}
	subsurface_unlink(sub);
	wl_list_insert(&ref->link, &sub->link);  /* Right after ref = above it. */
	rei_server_schedule_damage(sub->surface ? sub->surface->server : NULL);
}

static void subsurface_place_below(struct wl_client *c, struct wl_resource *r,
				   struct wl_resource *sibling)
{
	(void)c;
	struct rei_subsurface *sub = wl_resource_get_user_data(r);
	struct rei_subsurface *ref = sibling_entry(sub, sibling);
	if (!ref) {
		wl_resource_post_error(r, WL_SUBSURFACE_ERROR_BAD_SURFACE,
				       "sibling is not a sibling or the parent");
		return;
	}
	subsurface_unlink(sub);
	wl_list_insert(ref->link.prev, &sub->link);  /* Right before ref. */
	rei_server_schedule_damage(sub->surface ? sub->surface->server : NULL);
}

static void subsurface_set_sync(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	struct rei_subsurface *sub = wl_resource_get_user_data(r);
	sub->sync = true;
}

static void subsurface_set_desync(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	struct rei_subsurface *sub = wl_resource_get_user_data(r);
	sub->sync = false;
	/* Leaving synchronized mode applies anything the parent was holding back. */
	if (sub->surface && sub->surface->has_cached && !effectively_sync(sub->surface)) {
		sub->surface->has_cached = false;
		apply_state(sub->surface, &sub->surface->cached);
	}
}

static const struct wl_subsurface_interface subsurface_impl = {
	.destroy = subsurface_destroy_request,
	.set_position = subsurface_set_position,
	.place_above = subsurface_place_above,
	.place_below = subsurface_place_below,
	.set_sync = subsurface_set_sync,
	.set_desync = subsurface_set_desync,
};

static void subsurface_on_surface_destroyed(struct wl_listener *l, void *data)
{
	(void)data;
	struct rei_subsurface *sub = wl_container_of(l, sub, surface_destroy);
	wl_list_remove(&sub->surface_destroy.link);
	wl_list_init(&sub->surface_destroy.link);
	subsurface_unlink(sub);
	sub->surface = NULL;
}

static void subsurface_on_parent_destroyed(struct wl_listener *l, void *data)
{
	(void)data;
	struct rei_subsurface *sub = wl_container_of(l, sub, parent_destroy);
	wl_list_remove(&sub->parent_destroy.link);
	wl_list_init(&sub->parent_destroy.link);
	sub->parent = NULL;
}

static void subsurface_resource_destroy(struct wl_resource *r)
{
	struct rei_subsurface *sub = wl_resource_get_user_data(r);
	subsurface_unlink(sub);
	if (!wl_list_empty(&sub->surface_destroy.link))
		wl_list_remove(&sub->surface_destroy.link);
	if (!wl_list_empty(&sub->parent_destroy.link))
		wl_list_remove(&sub->parent_destroy.link);
	if (sub->surface) {
		/* The surface keeps its role but is no longer displayed. */
		sub->surface->sub = NULL;
		if (sub->surface->has_cached) {
			sub->surface->has_cached = false;
			state_clear(sub->surface->server, &sub->surface->cached);
		}
	}
	struct rei_server *server = sub->surface ? sub->surface->server : NULL;
	free(sub);
	rei_server_schedule_damage(server);
}

static void subcompositor_destroy_request(struct wl_client *c, struct wl_resource *r)
{
	(void)c;
	wl_resource_destroy(r);
}

static bool is_ancestor(const struct rei_surface *maybe_ancestor, const struct rei_surface *s)
{
	for (; s; s = s->sub ? s->sub->parent : NULL) {
		if (s == maybe_ancestor)
			return true;
	}
	return false;
}

static void subcompositor_get_subsurface(struct wl_client *client, struct wl_resource *res,
					 uint32_t id, struct wl_resource *surface_res,
					 struct wl_resource *parent_res)
{
	struct rei_surface *surface = wl_resource_get_user_data(surface_res);
	struct rei_surface *parent = wl_resource_get_user_data(parent_res);

	if (surface == parent || is_ancestor(surface, parent)) {
		wl_resource_post_error(res, WL_SUBCOMPOSITOR_ERROR_BAD_PARENT,
				       "a surface cannot be its own ancestor");
		return;
	}
	if (!rei_surface_set_role(surface, REI_ROLE_SUBSURFACE)) {
		wl_resource_post_error(res, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
				       "surface already has a role");
		return;
	}

	struct wl_resource *sres = wl_resource_create(client, &wl_subsurface_interface,
						      wl_resource_get_version(res), id);
	if (!sres) {
		wl_client_post_no_memory(client);
		return;
	}

	struct rei_subsurface *sub = rei_xcalloc(1, sizeof *sub);
	sub->surface = surface;
	sub->parent = parent;
	sub->resource = sres;
	sub->sync = true;  /* Subsurfaces start synchronized. */
	surface->sub = sub;

	sub->surface_destroy.notify = subsurface_on_surface_destroyed;
	wl_signal_add(&surface->destroy_signal, &sub->surface_destroy);
	sub->parent_destroy.notify = subsurface_on_parent_destroyed;
	wl_signal_add(&parent->destroy_signal, &sub->parent_destroy);

	wl_list_insert(parent->subs.prev, &sub->link);  /* Topmost. */
	wl_resource_set_implementation(sres, &subsurface_impl, sub, subsurface_resource_destroy);
}

static const struct wl_subcompositor_interface subcompositor_impl = {
	.destroy = subcompositor_destroy_request,
	.get_subsurface = subcompositor_get_subsurface,
};

static void subcompositor_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *res = wl_resource_create(client, &wl_subcompositor_interface,
						     (int)version, id);
	if (!res) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(res, &subcompositor_impl, data, NULL);
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

bool rei_surface_init(struct rei_server *server)
{
	return wl_global_create(server->display, &wl_compositor_interface, 6, server,
				compositor_bind) &&
	       wl_global_create(server->display, &wl_subcompositor_interface, 1, server,
				subcompositor_bind);
}

struct rei_surface *rei_surface_from_resource(struct wl_resource *resource)
{
	return wl_resource_get_user_data(resource);
}

bool rei_surface_set_role(struct rei_surface *s, enum rei_surface_role role)
{
	if (s->role != REI_ROLE_NONE && s->role != role)
		return false;
	s->role = role;
	return true;
}

void rei_surface_emit(const struct rei_surface *s, float ox, float oy,
		      struct rei_scene_snapshot *snap)
{
	const struct rei_subsurface *entry;
	wl_list_for_each(entry, &s->subs, link) {
		if (entry == s->self_marker) {
			if (s->content) {
				bool opaque = s->content->opaque ||
					      rei_rects_cover(&s->opaque, s->width, s->height);
				rei_snapshot_add(snap, s->content, ox, oy,
						 (float)s->width, (float)s->height, opaque);
			} else if (s->dmabuf) {
				bool opaque = s->dmabuf->opaque ||
					      rei_rects_cover(&s->opaque, s->width, s->height);
				rei_snapshot_add_dmabuf(snap, s->dmabuf, ox, oy,
							(float)s->width, (float)s->height, opaque);
			}
		} else if (entry->surface) {
			rei_surface_emit(entry->surface, ox + (float)entry->x,
					 oy + (float)entry->y, snap);
		}
	}
}

static bool input_accepts(const struct rei_surface *s, double lx, double ly)
{
	if (s->input_default)
		return true;
	for (size_t i = 0; i < s->input.n; i++) {
		const struct rei_rect *r = &s->input.items[i];
		if (lx >= r->x && lx < (double)r->x + r->w && ly >= r->y && ly < (double)r->y + r->h)
			return true;
	}
	return false;
}

bool rei_surface_pick(const struct rei_surface *s, double ox, double oy,
		      double x, double y, struct rei_surface **out, double *sx, double *sy)
{
	/* Back to front is the drawing order, so walk it in reverse. */
	const struct rei_subsurface *entry;
	wl_list_for_each_reverse(entry, &s->subs, link) {
		if (entry == s->self_marker) {
			double lx = x - ox, ly = y - oy;
			if ((s->content || s->dmabuf) && lx >= 0 && ly >= 0 && lx < s->width && ly < s->height &&
			    input_accepts(s, lx, ly)) {
				*out = (struct rei_surface *)s;
				*sx = lx;
				*sy = ly;
				return true;
			}
		} else if (entry->surface &&
			   rei_surface_pick(entry->surface, ox + entry->x, oy + entry->y,
					    x, y, out, sx, sy)) {
			return true;
		}
	}
	return false;
}

void rei_surface_fire_frames(struct rei_surface *s, uint64_t serial, uint32_t time_ms)
{
	struct rei_frame_cb *cb, *tmp;
	wl_list_for_each_safe(cb, tmp, &s->frames, link) {
		if (cb->min_serial > serial)
			continue;
		struct wl_resource *res = cb->resource;
		wl_list_remove(&cb->link);
		wl_resource_set_user_data(res, NULL);
		free(cb);
		wl_callback_send_done(res, time_ms);
		wl_resource_destroy(res);
	}
}
