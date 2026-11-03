#ifndef REI_SNAPSHOT_H
#define REI_SNAPSHOT_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dmabuf.h"
#include "pixbuf.h"

/* One textured rectangle, in global (logical) layout coordinates. */
struct rei_drawable {
	struct rei_pixbuf *pix;      /* Exactly one of pix and dmabuf is set; holds a reference. */
	struct rei_dmabuf *dmabuf;
	float x, y, w, h;
	bool opaque;             /* Safe to draw without blending (and alpha is 1). */
	float alpha;             /* Overall transparency, 0..1 (1: as is). */
	bool has_clip;           /* Drawn only inside `clip` (global coordinates). */
	float clip_x, clip_y, clip_w, clip_h;
};

/*
 * An immutable, reference-counted description of everything on screen at one
 * instant: a flat list of drawables, back to front (wallpaper, windows, popups,
 * cursor). The event thread builds one whenever the scene changes and hands a
 * reference to each render thread, so rendering never touches live compositor
 * state and needs no locks.
 */
struct rei_scene_snapshot {
	atomic_int refs;
	uint64_t serial;
	float clear_color[4];  /* RGBA, behind everything. */

	/* The pointer image. Kept apart from the drawables so an output with a
	 * hardware cursor can leave it out of the picture entirely. Drawn last, on
	 * any output that has no hardware cursor of its own. */
	struct {
		struct rei_pixbuf *pix;  /* NULL: no cursor. Holds a reference. */
		float x, y;              /* Top-left, global coordinates (hotspot applied). */
	} cursor;

	struct rei_drawable *items;
	size_t n_items, cap_items;
};

struct rei_scene_snapshot *rei_snapshot_create(uint64_t serial);
struct rei_scene_snapshot *rei_snapshot_ref(struct rei_scene_snapshot *s);
void rei_snapshot_unref(struct rei_scene_snapshot *s);

/* Restricts every drawable added since index `first` to a rectangle. Used to keep
 * a tiled window inside its slot while its client catches up with a resize. */
void rei_snapshot_clip_from(struct rei_scene_snapshot *s, size_t first,
			    float x, float y, float w, float h);

/* A client dma-buf instead of CPU pixels. `flip` is the buffer's own y_invert. */
void rei_snapshot_add_dmabuf(struct rei_scene_snapshot *s, struct rei_dmabuf *buf,
			     float x, float y, float w, float h, bool opaque);

void rei_snapshot_set_cursor(struct rei_scene_snapshot *s, struct rei_pixbuf *pix, float x, float y);

/* Makes everything added since index `first` translucent (multiplied onto what each already has). */
void rei_snapshot_set_alpha_from(struct rei_scene_snapshot *s, size_t first, float alpha);

/* Only valid while the snapshot is still being built (before it is shared). */
void rei_snapshot_add(struct rei_scene_snapshot *s, struct rei_pixbuf *pix,
		      float x, float y, float w, float h, bool opaque);

#endif
