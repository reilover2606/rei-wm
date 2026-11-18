#ifndef REI_SURFACE_H
#define REI_SURFACE_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

#include "geom.h"
#include "dmabuf.h"
#include "pixbuf.h"
#include "snapshot.h"

struct rei_server;
struct rei_surface;
struct rei_subsurface;
struct rei_buffer_lease;

/* A set of rectangles (not normalised: overlaps are allowed). */
struct rei_rects {
	struct rei_rect *items;
	size_t n, cap;
};

void rei_rects_clear(struct rei_rects *r);
void rei_rects_add(struct rei_rects *r, struct rei_rect rect);
void rei_rects_subtract(struct rei_rects *r, struct rei_rect hole);
bool rei_rects_cover(const struct rei_rects *r, int32_t w, int32_t h);

enum rei_surface_role {
	REI_ROLE_NONE = 0,
	REI_ROLE_XDG_TOPLEVEL,
	REI_ROLE_XDG_POPUP,
	REI_ROLE_SUBSURFACE,
	REI_ROLE_CURSOR,
	REI_ROLE_XWAYLAND,
};

struct rei_syncobj_surface;

struct rei_frame_cb {
	struct wl_list link;
	struct wl_resource *resource;
	/* The serial of the first snapshot that carries the commit this callback came with (informational).
	 * A callback is answered by the next frame that reaches a screen, whether or not that frame already shows the
	 * commit, as other compositors do: waiting for the frame that does made a client whose commit landed while
	 * another frame was in flight (a moving pointer is enough) wait a whole extra refresh, halving its frame rate. */
	uint64_t min_serial;
};

/* Double-buffered surface state. */
struct rei_surface_state {
	/* Only meaningful on the pending state, until commit latches it. */
	struct wl_resource *buffer;
	bool buffer_set;
	struct wl_listener buffer_destroy;

	bool content_set;
	struct rei_pixbuf *content;  /* NULL with content_set means "no buffer". */
	struct rei_dmabuf *dmabuf;   /* Instead of `content`, for a GPU buffer. */
	struct rei_buffer_lease *lease;  /* Holds that buffer until it may be released. */
	bool scale_set;
	int32_t scale;
	bool transform_set;
	int32_t transform;
	bool viewport_set;           /* wp_viewport: a new destination and/or source is pending */
	int32_t dst_w, dst_h;        /* destination size; -1 = unset */
	int32_t src_w, src_h;        /* source rectangle size in buffer pixels (rounded); -1 = unset */
	bool opaque_set, input_set;
	struct rei_rects opaque, input;
	struct wl_list frames;  /* rei_frame_cb */
};

struct rei_surface {
	struct rei_server *server;
	struct wl_resource *resource;
	struct wl_list link;  /* rei_server.surfaces */

	struct rei_surface_state pending;
	struct rei_surface_state cached;  /* Held back by a synchronized subsurface. */
	bool has_cached;

	/* Current (committed) state. At most one of content and dmabuf is set. */
	struct rei_pixbuf *content;
	struct rei_dmabuf *dmabuf;
	struct rei_buffer_lease *lease;
	uint64_t content_serial;   /* A new number each time the content changes (a client reuses its GPU buffers). */
	int32_t scale, transform;
	int32_t dst_w, dst_h;   /* wp_viewport destination; -1 when none */
	int32_t src_w, src_h;   /* wp_viewport source size; -1 when none */
	struct wl_resource *viewport;  /* The surface's wp_viewport, if any (at most one). */
	int32_t width, height;  /* Surface-local size = buffer size / scale, or the viewport's destination. */
	struct rei_rects opaque, input;
	bool input_default;  /* No input region set: the whole surface takes input. */
	struct wl_list frames;

	/* Explicit synchronization (proto/syncobj.c): its object on this surface, and the commits waiting for their acquire
	 * points, oldest first. */
	struct rei_syncobj_surface *sync_surface;
	struct wl_list blocked;

	enum rei_surface_role role;
	/* Called after a commit has been applied. */
	void (*role_commit)(struct rei_surface *surface, void *data);
	void *role_data;

	/* Subsurface tree. `subs` is ordered back to front and includes
	 * `self_marker`, which stands for this surface's own content. */
	struct rei_subsurface *sub;  /* Non-NULL if this surface is a subsurface. */
	struct wl_list subs;
	struct rei_subsurface *self_marker;

	struct wl_signal destroy_signal;
	bool dying;   /* Its destroy signal is being emitted: nothing may take focus on it again. */
};

struct rei_subsurface {
	struct wl_list link;  /* parent->subs */
	struct rei_surface *surface;  /* NULL once the surface is destroyed. */
	struct rei_surface *parent;   /* NULL once the parent is destroyed. */
	struct wl_resource *resource;
	int32_t x, y;
	int32_t pending_x, pending_y;
	bool pending_pos;
	bool sync;
	struct wl_listener surface_destroy, parent_destroy;
};

/* Registers wl_compositor and wl_subcompositor. */
bool rei_surface_init(struct rei_server *server);

/* Returns the rei_surface behind a wl_surface resource. */
struct rei_surface *rei_surface_from_resource(struct wl_resource *resource);

/*
 * Claims a role for the surface. Fails (returns false) if it already has a
 * different one; the caller reports the protocol error appropriate to its
 * interface.
 */
bool rei_surface_set_role(struct rei_surface *surface, enum rei_surface_role role);

/* Appends this surface and its subsurfaces to `snap` with the surface's
 * top-left at (ox, oy) in global coordinates. */
void rei_surface_emit(const struct rei_surface *surface, float ox, float oy,
		      struct rei_scene_snapshot *snap);

/*
 * Finds the topmost surface in this surface's subsurface tree that accepts input
 * at global point (x, y), given the tree's top-left at (ox, oy). On a hit,
 * returns true with the surface and the point in its own coordinates.
 */
bool rei_surface_pick(const struct rei_surface *surface, double ox, double oy,
		      double x, double y, struct rei_surface **out, double *sx, double *sy);

/* The surface at the top of a subsurface chain. */
struct rei_surface *rei_surface_root(struct rei_surface *surface);

/* Sends 'done' for callbacks whose commit is covered by a frame of `serial`. */
void rei_surface_fire_frames(struct rei_surface *surface, uint64_t serial, uint32_t time_ms);

#endif
