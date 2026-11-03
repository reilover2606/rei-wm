#include "backend/drm/drm_logic.h"

#include <drm_mode.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/util.h"

/* ---- Naming ---- */

const char *rei_drm_connector_type_name(uint32_t type)
{
	switch (type) {
	case DRM_MODE_CONNECTOR_VGA: return "VGA";
	case DRM_MODE_CONNECTOR_DVII: return "DVI-I";
	case DRM_MODE_CONNECTOR_DVID: return "DVI-D";
	case DRM_MODE_CONNECTOR_DVIA: return "DVI-A";
	case DRM_MODE_CONNECTOR_Composite: return "Composite";
	case DRM_MODE_CONNECTOR_SVIDEO: return "SVIDEO";
	case DRM_MODE_CONNECTOR_LVDS: return "LVDS";
	case DRM_MODE_CONNECTOR_Component: return "Component";
	case DRM_MODE_CONNECTOR_9PinDIN: return "DIN";
	case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
	case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
	case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
	case DRM_MODE_CONNECTOR_TV: return "TV";
	case DRM_MODE_CONNECTOR_eDP: return "eDP";
	case DRM_MODE_CONNECTOR_VIRTUAL: return "Virtual";
	case DRM_MODE_CONNECTOR_DSI: return "DSI";
	case DRM_MODE_CONNECTOR_DPI: return "DPI";
	case DRM_MODE_CONNECTOR_WRITEBACK: return "Writeback";
	case DRM_MODE_CONNECTOR_SPI: return "SPI";
	case DRM_MODE_CONNECTOR_USB: return "USB";
	default: return "Unknown";
	}
}

void rei_drm_connector_name(char *buf, size_t len, uint32_t type, uint32_t type_id)
{
	snprintf(buf, len, "%s-%u", rei_drm_connector_type_name(type), type_id);
}

/* ---- Modes ---- */

int32_t rei_drm_refresh_mhz(uint32_t clock_khz, uint32_t htotal, uint32_t vtotal,
			    bool interlaced, bool doublescan, uint32_t vscan)
{
	if (htotal == 0 || vtotal == 0)
		return 0;
	/* Same arithmetic as the kernel's drm_mode_vrefresh(): an interlaced mode
	 * reports its field rate. */
	uint64_t num = (uint64_t)clock_khz * 1000000ull;
	uint64_t den = (uint64_t)htotal * vtotal;
	if (interlaced)
		num *= 2;
	if (doublescan)
		den *= 2;
	if (vscan > 1)
		den *= vscan;
	return (int32_t)((num + den / 2) / den);
}

int rei_drm_pick_mode(const struct rei_drm_mode *modes, size_t n,
		      int32_t width, int32_t height, int32_t refresh_mhz)
{
	int best = -1;

	if (width == 0) {
		for (size_t i = 0; i < n; i++)
			if (modes[i].preferred && !modes[i].interlaced && !modes[i].doublescan)
				return (int)i;
		int64_t best_area = -1;
		for (size_t i = 0; i < n; i++) {
			if (modes[i].interlaced || modes[i].doublescan)
				continue;
			int64_t area = (int64_t)modes[i].width * modes[i].height;
			if (area > best_area || (area == best_area &&
			    modes[i].refresh_mhz > modes[best].refresh_mhz)) {
				best = (int)i;
				best_area = area;
			}
		}
		if (best >= 0)
			return best;
		return n ? 0 : -1;
	}

	/* Score: plain progressive modes first, then closeness of refresh rate. */
	int64_t best_score = INT64_MAX;
	for (size_t i = 0; i < n; i++) {
		if (modes[i].width != width || modes[i].height != height)
			continue;
		int64_t penalty = (modes[i].interlaced || modes[i].doublescan) ? (int64_t)1 << 40 : 0;
		int64_t diff = refresh_mhz > 0 ? llabs((int64_t)modes[i].refresh_mhz - refresh_mhz)
					       : -(int64_t)modes[i].refresh_mhz;
		int64_t score = penalty + diff;
		if (score < best_score) {
			best_score = score;
			best = (int)i;
		}
	}
	return best;
}

/* ---- CRTC and plane assignment ---- */

struct crtc_search {
	const uint32_t *possible;
	const int *current;
	size_t n_conn, n_crtcs;
	int *cur;       /* Working assignment. */
	int *best;
	size_t best_count, best_kept;
};

static void search(struct crtc_search *s, size_t i, uint32_t used, size_t count, size_t kept)
{
	if (count + (s->n_conn - i) < s->best_count)
		return;  /* Cannot beat the best even if everything left is assigned. */
	if (i == s->n_conn) {
		if (count > s->best_count || (count == s->best_count && kept > s->best_kept)) {
			s->best_count = count;
			s->best_kept = kept;
			memcpy(s->best, s->cur, s->n_conn * sizeof *s->best);
		}
		return;
	}

	/* Try the CRTC it is already on first, then the rest in order. */
	int order[32];
	size_t n_order = 0;
	if (s->current[i] >= 0 && (size_t)s->current[i] < s->n_crtcs)
		order[n_order++] = s->current[i];
	for (size_t c = 0; c < s->n_crtcs && c < 32; c++)
		if (!(s->current[i] >= 0 && (int)c == s->current[i]))
			order[n_order++] = (int)c;

	for (size_t k = 0; k < n_order; k++) {
		int c = order[k];
		if (!(s->possible[i] & (1u << c)) || (used & (1u << c)))
			continue;
		s->cur[i] = c;
		search(s, i + 1, used | (1u << c), count + 1, kept + (c == s->current[i]));
	}
	s->cur[i] = -1;
	search(s, i + 1, used, count, kept);
}

size_t rei_drm_assign_crtcs(const uint32_t *possible, const int *current, size_t n_connectors,
			    size_t n_crtcs, int *out)
{
	if (n_connectors == 0)
		return 0;
	if (n_crtcs > 32)
		n_crtcs = 32;
	int *cur = rei_xcalloc(n_connectors, sizeof *cur);
	for (size_t i = 0; i < n_connectors; i++) {
		cur[i] = -1;
		out[i] = -1;
	}
	struct crtc_search s = {
		.possible = possible, .current = current, .n_conn = n_connectors,
		.n_crtcs = n_crtcs, .cur = cur, .best = out,
	};
	search(&s, 0, 0, 0, 0);
	free(cur);
	return s.best_count;
}

int rei_drm_pick_plane(const struct rei_drm_plane_info *planes, size_t n, size_t crtc_index,
		       enum rei_drm_plane_type type, uint64_t taken)
{
	for (size_t i = 0; i < n && i < 64; i++) {
		if (planes[i].type != type || (taken & ((uint64_t)1 << i)))
			continue;
		if (planes[i].possible_crtcs & (1u << crtc_index))
			return (int)i;
	}
	return -1;
}

/* ---- Formats ---- */

size_t rei_drm_parse_in_formats(const void *blob, size_t len, struct rei_format_mod **out)
{
	*out = NULL;
	const uint8_t *base = blob;
	if (!blob || len < sizeof(struct drm_format_modifier_blob))
		return 0;

	struct drm_format_modifier_blob hdr;
	memcpy(&hdr, base, sizeof hdr);
	if (hdr.version != FORMAT_BLOB_CURRENT)
		return 0;

	/* Every offset and count comes from the kernel, but check them anyway. */
	uint64_t formats_end = (uint64_t)hdr.formats_offset + (uint64_t)hdr.count_formats * 4;
	uint64_t mods_end = (uint64_t)hdr.modifiers_offset +
			    (uint64_t)hdr.count_modifiers * sizeof(struct drm_format_modifier);
	if (formats_end > len || mods_end > len)
		return 0;

	size_t cap = 16, count = 0;
	struct rei_format_mod *list = rei_xcalloc(cap, sizeof *list);

	for (uint32_t m = 0; m < hdr.count_modifiers; m++) {
		struct drm_format_modifier mod;
		memcpy(&mod, base + hdr.modifiers_offset + (size_t)m * sizeof mod, sizeof mod);
		for (uint32_t bit = 0; bit < 64; bit++) {
			if (!(mod.formats & ((uint64_t)1 << bit)))
				continue;
			uint64_t idx = (uint64_t)mod.offset + bit;
			if (idx >= hdr.count_formats)
				continue;
			uint32_t fmt;
			memcpy(&fmt, base + hdr.formats_offset + idx * 4, 4);
			if (count == cap) {
				cap *= 2;
				list = rei_xrealloc(list, cap * sizeof *list);
			}
			list[count++] = (struct rei_format_mod){ fmt, mod.modifier };
		}
	}
	if (count == 0) {
		free(list);
		return 0;
	}
	*out = list;
	return count;
}

/* ---- Direct scanout ---- */

static bool rects_intersect(float ax, float ay, float aw, float ah,
			    float bx, float by, float bw, float bh)
{
	return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

const struct rei_drawable *rei_drm_scanout_candidate(const struct rei_scene_snapshot *snap,
						     const struct rei_drm_view_rect *out)
{
	/* The topmost drawable that touches this output decides: anything above
	 * the candidate would have to be composed over it. */
	for (size_t i = snap->n_items; i-- > 0;) {
		const struct rei_drawable *d = &snap->items[i];
		if (!rects_intersect(d->x, d->y, d->w, d->h, out->x, out->y, out->w, out->h))
			continue;

		if (!d->dmabuf || !d->opaque || d->pix)
			return NULL;
		if (d->x != out->x || d->y != out->y || d->w != out->w || d->h != out->h)
			return NULL;
		if (d->dmabuf->width != out->pixel_w || d->dmabuf->height != out->pixel_h)
			return NULL;
		if (d->dmabuf->y_invert)
			return NULL;
		if (d->has_clip && (d->clip_x > d->x || d->clip_y > d->y ||
				    d->clip_x + d->clip_w < d->x + d->w ||
				    d->clip_y + d->clip_h < d->y + d->h))
			return NULL;
		return d;
	}
	return NULL;
}

/* ---- Cursor ---- */

void rei_drm_cursor_place(float x, float y, int32_t w, int32_t h,
			  const struct rei_drm_view_rect *out, double scale,
			  int32_t *px, int32_t *py, bool *visible)
{
	double dx = ((double)x - (double)out->x) * scale;
	double dy = ((double)y - (double)out->y) * scale;
	*px = (int32_t)floor(dx);
	*py = (int32_t)floor(dy);
	*visible = *px < out->pixel_w && *py < out->pixel_h && *px + w > 0 && *py + h > 0;
}
