#include "snapshot.h"

#include <stdlib.h>

#include "util/util.h"

struct rei_scene_snapshot *rei_snapshot_create(uint64_t serial)
{
	struct rei_scene_snapshot *s = rei_xcalloc(1, sizeof *s);
	atomic_init(&s->refs, 1);
	s->serial = serial;
	s->clear_color[3] = 1.0f;
	return s;
}

struct rei_scene_snapshot *rei_snapshot_ref(struct rei_scene_snapshot *s)
{
	atomic_fetch_add_explicit(&s->refs, 1, memory_order_relaxed);
	return s;
}

void rei_snapshot_unref(struct rei_scene_snapshot *s)
{
	if (!s)
		return;
	if (atomic_fetch_sub_explicit(&s->refs, 1, memory_order_acq_rel) != 1)
		return;
	for (size_t i = 0; i < s->n_items; i++) {
		rei_pixbuf_unref(s->items[i].pix);
		rei_dmabuf_unref(s->items[i].dmabuf);
	}
	rei_pixbuf_unref(s->cursor.pix);
	free(s->items);
	free(s);
}

void rei_snapshot_add(struct rei_scene_snapshot *s, struct rei_pixbuf *pix,
		      float x, float y, float w, float h, bool opaque)
{
	if (s->n_items == s->cap_items) {
		s->cap_items = s->cap_items ? s->cap_items * 2 : 16;
		s->items = rei_xrealloc(s->items, s->cap_items * sizeof *s->items);
	}
	s->items[s->n_items++] = (struct rei_drawable){
		.pix = rei_pixbuf_ref(pix),
		.x = x, .y = y, .w = w, .h = h,
		.opaque = opaque || pix->opaque,
	};
}

void rei_snapshot_clip_from(struct rei_scene_snapshot *s, size_t first,
			    float x, float y, float w, float h)
{
	for (size_t i = first; i < s->n_items; i++) {
		s->items[i].has_clip = true;
		s->items[i].clip_x = x;
		s->items[i].clip_y = y;
		s->items[i].clip_w = w;
		s->items[i].clip_h = h;
	}
}

void rei_snapshot_add_dmabuf(struct rei_scene_snapshot *s, struct rei_dmabuf *buf,
			     float x, float y, float w, float h, bool opaque)
{
	if (s->n_items == s->cap_items) {
		s->cap_items = s->cap_items ? s->cap_items * 2 : 16;
		s->items = rei_xrealloc(s->items, s->cap_items * sizeof *s->items);
	}
	s->items[s->n_items++] = (struct rei_drawable){
		.dmabuf = rei_dmabuf_ref(buf),
		.x = x, .y = y, .w = w, .h = h,
		.opaque = opaque || buf->opaque,
	};
}

void rei_snapshot_set_cursor(struct rei_scene_snapshot *s, struct rei_pixbuf *pix, float x, float y)
{
	rei_pixbuf_unref(s->cursor.pix);
	s->cursor.pix = pix ? rei_pixbuf_ref(pix) : NULL;
	s->cursor.x = x;
	s->cursor.y = y;
}
