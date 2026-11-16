#include "snapshot.h"

#include <stdlib.h>
#include <string.h>

#include "shader.h"
#include "util/util.h"

struct rei_scene_snapshot *rei_snapshot_create(uint64_t serial)
{
	struct rei_scene_snapshot *s = rei_xcalloc(1, sizeof *s);
	atomic_init(&s->refs, 1);
	s->serial = serial;
	s->clear_color[3] = 1.0f;
	return s;
}

struct rei_scene_snapshot *rei_snapshot_clone(const struct rei_scene_snapshot *from, uint64_t serial)
{
	struct rei_scene_snapshot *s = rei_snapshot_create(serial);
	memcpy(s->clear_color, from->clear_color, sizeof s->clear_color);
	s->items = rei_xcalloc(from->n_items ? from->n_items : 1, sizeof *s->items);
	s->n_items = s->cap_items = from->n_items;
	memcpy(s->items, from->items, from->n_items * sizeof *s->items);
	s->blur_from = from->blur_from;
	s->blur_radius = from->blur_radius;
	s->blur_dim = from->blur_dim;
	memcpy(s->blink, from->blink, sizeof s->blink);
	s->n_blink = from->n_blink;
	s->animated = from->animated;
	for (size_t i = 0; i < s->n_items; i++) {
		if (s->items[i].pix)
			rei_pixbuf_ref(s->items[i].pix);
		if (s->items[i].dmabuf)
			rei_dmabuf_ref(s->items[i].dmabuf);
	}
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

/* Records what an effect needs from the event thread: a redraw at each blink, or all the time. */
static void note_fx(struct rei_scene_snapshot *s, const struct rei_fx *fx)
{
	if (rei_shader_uses_time(fx->shader))
		s->animated = true;
	if (fx->blink <= 0)
		return;
	for (int i = 0; i < s->n_blink; i++)
		if (s->blink[i] == fx->blink)
			return;
	if (s->n_blink < REI_SNAPSHOT_MAX_BLINK)
		s->blink[s->n_blink++] = fx->blink;
	else
		s->animated = true;   /* too many different rhythms to track: just keep drawing */
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
		.alpha = 1.0f,
		.content = pix->id,
	};
	/* Its text layers go right over it, through their shaders. */
	for (int i = 0; i < pix->n_layers; i++) {
		if (s->n_items == s->cap_items) {
			s->cap_items *= 2;
			s->items = rei_xrealloc(s->items, s->cap_items * sizeof *s->items);
		}
		s->items[s->n_items++] = (struct rei_drawable){
			.pix = rei_pixbuf_ref(pix->layers[i].pix),
			.x = x, .y = y, .w = w, .h = h,
			.alpha = 1.0f,
			.fx = pix->layers[i].fx,
			.content = pix->layers[i].pix->id,
		};
		note_fx(s, &pix->layers[i].fx);
	}
}

void rei_snapshot_set_fx_from(struct rei_scene_snapshot *s, size_t first, const struct rei_fx *fx)
{
	if (!fx->shader)
		return;
	for (size_t i = first; i < s->n_items; i++) {
		if (s->items[i].fx.shader)
			continue;   /* (a text layer keeps its own) */
		s->items[i].fx = *fx;
	}
	if (first < s->n_items)
		note_fx(s, fx);
}

void rei_snapshot_set_alpha_from(struct rei_scene_snapshot *s, size_t first, float alpha)
{
	for (size_t i = first; i < s->n_items; i++) {
		s->items[i].alpha *= alpha;
		if (s->items[i].alpha < 1.0f)
			s->items[i].opaque = false;
	}
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
		.alpha = 1.0f,
	};
}

void rei_snapshot_set_cursor(struct rei_scene_snapshot *s, struct rei_pixbuf *pix, float x, float y, float w, float h)
{
	rei_pixbuf_unref(s->cursor.pix);
	s->cursor.pix = pix ? rei_pixbuf_ref(pix) : NULL;
	s->cursor.x = x;
	s->cursor.y = y;
	s->cursor.w = w;
	s->cursor.h = h;
}
