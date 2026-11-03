#ifndef REI_DRM_LOGIC_H
#define REI_DRM_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dmabuf.h"
#include "snapshot.h"

/*
 * The decisions the DRM backend makes, kept free of any device access so they
 * can be unit tested without a GPU: naming, mode choice, CRTC and plane
 * assignment, format negotiation, direct-scanout eligibility and cursor
 * placement.
 */

/* ---- Naming ---- */

/* "DP", "HDMI-A", "eDP", ... for a DRM_MODE_CONNECTOR_* value ("Unknown" if none). */
const char *rei_drm_connector_type_name(uint32_t type);
/* The conventional output name: "DP-1", "HDMI-A-2", "eDP-1". */
void rei_drm_connector_name(char *buf, size_t len, uint32_t type, uint32_t type_id);

/* ---- Modes ---- */

struct rei_drm_mode {
	int32_t width, height;
	int32_t refresh_mhz;
	bool preferred;
	bool interlaced;
	bool doublescan;
};

/* Refresh rate in millihertz from the timings of a drmModeModeInfo. vscan 0 counts as 1. */
int32_t rei_drm_refresh_mhz(uint32_t clock_khz, uint32_t htotal, uint32_t vtotal,
			    bool interlaced, bool doublescan, uint32_t vscan);

/*
 * Chooses a mode. With width == 0 that is the preferred mode (else the largest,
 * fastest one). Otherwise it is the mode with exactly that size and the nearest
 * refresh rate (the highest when refresh_mhz is 0), avoiding interlaced and
 * doublescan modes unless nothing else fits. Returns an index, or -1 if no mode
 * has that size.
 */
int rei_drm_pick_mode(const struct rei_drm_mode *modes, size_t n,
		      int32_t width, int32_t height, int32_t refresh_mhz);

/* ---- CRTC and plane assignment ---- */

/*
 * Gives each connector its own CRTC. `possible[i]` is a bitmask over CRTC
 * indices that connector i can drive (the union over its encoders); `current[i]`
 * is the CRTC it is on now, or -1 — kept if it can be, so a running display is
 * not shuffled. Maximises the number of connectors that get one. `out[i]` is
 * the CRTC index, or -1. Returns the number assigned.
 */
size_t rei_drm_assign_crtcs(const uint32_t *possible, const int *current, size_t n_connectors,
			    size_t n_crtcs, int *out);

enum rei_drm_plane_type {
	REI_DRM_PLANE_OVERLAY = 0,
	REI_DRM_PLANE_PRIMARY = 1,
	REI_DRM_PLANE_CURSOR = 2,
};

struct rei_drm_plane_info {
	enum rei_drm_plane_type type;
	uint32_t possible_crtcs;  /* Bitmask over CRTC indices. */
};

/* The first plane of `type` that works with CRTC `crtc_index` and whose bit is not
 * set in `taken` (a bitmask over plane indices); -1 if none. */
int rei_drm_pick_plane(const struct rei_drm_plane_info *planes, size_t n, size_t crtc_index,
		       enum rei_drm_plane_type type, uint64_t taken);

/* ---- Formats ---- */

/*
 * Reads a plane's IN_FORMATS property blob into (format, modifier) pairs.
 * Returns the number of pairs (0 if the blob is malformed or empty), and the
 * array in *out, which the caller frees.
 */
size_t rei_drm_parse_in_formats(const void *blob, size_t len, struct rei_format_mod **out);

/* ---- Direct scanout ---- */

/* An output's rectangle in the global layout, and its scale. */
struct rei_drm_view_rect {
	float x, y, w, h;   /* Logical. */
	int32_t pixel_w, pixel_h;
};

/*
 * If a single client buffer covers the whole output so completely that
 * composition is unnecessary, returns it: the topmost thing on the output must
 * be an opaque dma-buf of exactly the output's pixel size, drawn 1:1 over the
 * whole output, unclipped and not vertically inverted. Otherwise NULL.
 */
const struct rei_drawable *rei_drm_scanout_candidate(const struct rei_scene_snapshot *snap,
						     const struct rei_drm_view_rect *out);

/* ---- Cursor ---- */

/*
 * Where the hardware cursor goes on an output. `x`,`y` is the cursor image's
 * top-left in global logical coordinates, `w`,`h` its size in pixels. `visible`
 * is false when no part of it is on this output.
 */
void rei_drm_cursor_place(float x, float y, int32_t w, int32_t h,
			  const struct rei_drm_view_rect *out, double scale,
			  int32_t *px, int32_t *py, bool *visible);

#endif
