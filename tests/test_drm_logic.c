#include <stdlib.h>
#include <string.h>

#include "backend/drm/drm_logic.h"
#include "check.h"

static void test_names(void)
{
	char b[32];
	rei_drm_connector_name(b, sizeof b, 10, 1);  /* DisplayPort */
	CHECK_STR(b, "DP-1");
	rei_drm_connector_name(b, sizeof b, 11, 2);  /* HDMI-A */
	CHECK_STR(b, "HDMI-A-2");
	rei_drm_connector_name(b, sizeof b, 14, 1);  /* eDP */
	CHECK_STR(b, "eDP-1");
	rei_drm_connector_name(b, sizeof b, 9999, 3);
	CHECK_STR(b, "Unknown-3");
}

static void test_refresh(void)
{
	/* 1080p60: 148.5 MHz, 2200 x 1125. */
	CHECK(rei_drm_refresh_mhz(148500, 2200, 1125, false, false, 0) == 60000);
	/* 1080p59.94 */
	int32_t r = rei_drm_refresh_mhz(148352, 2200, 1125, false, false, 0);
	CHECK(r >= 59939 && r <= 59941);
	/* 1080i: 74.25 MHz, reported as the 60 Hz field rate like the kernel does. */
	CHECK(rei_drm_refresh_mhz(74250, 2200, 1125, true, false, 0) == 60000);
	CHECK(rei_drm_refresh_mhz(148500, 2200, 1125, false, true, 0) == 30000);
	CHECK(rei_drm_refresh_mhz(148500, 0, 1125, false, false, 0) == 0);
}

static void test_parse(void)
{
	int32_t w, h, r;
	CHECK(rei_drm_parse_mode("1920x1080", &w, &h, &r) && w == 1920 && h == 1080 && r == 0);
	CHECK(rei_drm_parse_mode("2560x1440@144", &w, &h, &r) && w == 2560 && h == 1440 && r == 144000);
	CHECK(rei_drm_parse_mode("1920x1080@59.94", &w, &h, &r) && r == 59940);
	CHECK(!rei_drm_parse_mode("", &w, &h, &r));
	CHECK(!rei_drm_parse_mode("1920", &w, &h, &r));
	CHECK(!rei_drm_parse_mode("1920x", &w, &h, &r));
	CHECK(!rei_drm_parse_mode("1920x1080@", &w, &h, &r));
	CHECK(!rei_drm_parse_mode("1920x1080@60hz", &w, &h, &r));
	CHECK(!rei_drm_parse_mode("0x1080", &w, &h, &r));
	CHECK(!rei_drm_parse_mode("-5x10", &w, &h, &r));
	CHECK(!rei_drm_parse_mode(NULL, &w, &h, &r));
}

static void test_pick_mode(void)
{
	struct rei_drm_mode m[] = {
		{ 1920, 1080, 60000, true, false, false },   /* 0 */
		{ 1920, 1080, 144000, false, false, false }, /* 1 */
		{ 1920, 1080, 60000, false, true, false },   /* 2: interlaced */
		{ 2560, 1440, 59950, false, false, false },  /* 3 */
		{ 1280, 720, 60000, false, false, false },   /* 4 */
	};
	CHECK(rei_drm_pick_mode(m, 5, 0, 0, 0) == 0);                   /* the preferred one */
	CHECK(rei_drm_pick_mode(m, 5, 1920, 1080, 0) == 1);             /* highest rate */
	CHECK(rei_drm_pick_mode(m, 5, 1920, 1080, 60000) == 0);         /* exact */
	CHECK(rei_drm_pick_mode(m, 5, 1920, 1080, 120000) == 1);        /* nearest */
	CHECK(rei_drm_pick_mode(m, 5, 2560, 1440, 60000) == 3);
	CHECK(rei_drm_pick_mode(m, 5, 800, 600, 0) == -1);

	/* Interlaced only counts when nothing else has the size. */
	struct rei_drm_mode i[] = { { 1920, 1080, 60000, false, true, false } };
	CHECK(rei_drm_pick_mode(i, 1, 1920, 1080, 0) == 0);
	/* No preferred mode: the largest. */
	struct rei_drm_mode np[] = { { 1280, 720, 60000, false, false, false },
				     { 1920, 1080, 30000, false, false, false },
				     { 1920, 1080, 60000, false, false, false } };
	CHECK(rei_drm_pick_mode(np, 3, 0, 0, 0) == 2);
	CHECK(rei_drm_pick_mode(np, 0, 0, 0, 0) == -1);
}

static void test_crtcs(void)
{
	int out[4];
	/* Two connectors, each can use either of two CRTCs. */
	uint32_t p1[] = { 3, 3 };
	int cur1[] = { -1, -1 };
	CHECK(rei_drm_assign_crtcs(p1, cur1, 2, 2, out) == 2);
	CHECK(out[0] != out[1] && out[0] >= 0 && out[1] >= 0);

	/* A greedy first-fit would give connector 0 CRTC 0 and strand connector 1. */
	uint32_t p2[] = { 3, 1 };
	CHECK(rei_drm_assign_crtcs(p2, cur1, 2, 2, out) == 2);
	CHECK(out[0] == 1 && out[1] == 0);

	/* Running displays stay where they are when possible. */
	int cur3[] = { 1, 0 };
	CHECK(rei_drm_assign_crtcs(p1, cur3, 2, 2, out) == 2);
	CHECK(out[0] == 1 && out[1] == 0);

	/* More connectors than CRTCs: as many as fit. */
	uint32_t p4[] = { 1, 1, 3 };
	int cur4[] = { -1, -1, -1 };
	CHECK(rei_drm_assign_crtcs(p4, cur4, 3, 2, out) == 2);
	int assigned = 0;
	for (int i = 0; i < 3; i++)
		assigned += out[i] >= 0;
	CHECK(assigned == 2);

	/* A connector nothing can drive. */
	uint32_t p5[] = { 0, 1 };
	int cur5[] = { -1, -1 };
	CHECK(rei_drm_assign_crtcs(p5, cur5, 2, 2, out) == 1);
	CHECK(out[0] == -1 && out[1] == 0);
	CHECK(rei_drm_assign_crtcs(p5, cur5, 0, 2, out) == 0);
}

static void test_planes(void)
{
	struct rei_drm_plane_info pl[] = {
		{ REI_DRM_PLANE_PRIMARY, 1 },  /* 0: CRTC 0 */
		{ REI_DRM_PLANE_PRIMARY, 2 },  /* 1: CRTC 1 */
		{ REI_DRM_PLANE_OVERLAY, 3 },  /* 2 */
		{ REI_DRM_PLANE_CURSOR, 1 },   /* 3 */
		{ REI_DRM_PLANE_CURSOR, 2 },   /* 4 */
	};
	CHECK(rei_drm_pick_plane(pl, 5, 0, REI_DRM_PLANE_PRIMARY, 0) == 0);
	CHECK(rei_drm_pick_plane(pl, 5, 1, REI_DRM_PLANE_PRIMARY, 0) == 1);
	CHECK(rei_drm_pick_plane(pl, 5, 1, REI_DRM_PLANE_CURSOR, 0) == 4);
	CHECK(rei_drm_pick_plane(pl, 5, 0, REI_DRM_PLANE_OVERLAY, 0) == 2);
	CHECK(rei_drm_pick_plane(pl, 5, 0, REI_DRM_PLANE_OVERLAY, 1u << 2) == -1);
	CHECK(rei_drm_pick_plane(pl, 5, 2, REI_DRM_PLANE_PRIMARY, 0) == -1);
}

/* Builds an IN_FORMATS blob: formats [XR24, AR24, AB24], one modifier entry
 * covering formats 0-1 with LINEAR, another covering 1-2 with a vendor modifier. */
static size_t build_blob(uint8_t *buf, size_t cap)
{
	memset(buf, 0, cap);
	uint32_t hdr[6] = { 1, 0, 3, 24, 2, 24 + 3 * 4 + 4 };
	memcpy(buf, hdr, sizeof hdr);
	uint32_t fmts[3] = { REI_FORMAT_XRGB8888, REI_FORMAT_ARGB8888, REI_FORMAT_ABGR8888 };
	memcpy(buf + 24, fmts, sizeof fmts);
	size_t off = 24 + 3 * 4 + 4;
	struct { uint64_t formats; uint32_t offset, pad; uint64_t modifier; } m0 = { 0x3, 0, 0, 0 };
	struct { uint64_t formats; uint32_t offset, pad; uint64_t modifier; } m1 = { 0x3, 1, 0, 0x0200000000000001ull };
	memcpy(buf + off, &m0, sizeof m0);
	memcpy(buf + off + sizeof m0, &m1, sizeof m1);
	return off + sizeof m0 + sizeof m1;
}

static void test_in_formats(void)
{
	uint8_t blob[256];
	size_t len = build_blob(blob, sizeof blob);
	struct rei_format_mod *list;
	size_t n = rei_drm_parse_in_formats(blob, len, &list);
	CHECK(n == 4);
	CHECK(rei_format_mod_supported(list, n, REI_FORMAT_XRGB8888, REI_MOD_LINEAR));
	CHECK(rei_format_mod_supported(list, n, REI_FORMAT_ARGB8888, REI_MOD_LINEAR));
	CHECK(!rei_format_mod_supported(list, n, REI_FORMAT_ABGR8888, REI_MOD_LINEAR));
	CHECK(rei_format_mod_supported(list, n, REI_FORMAT_ARGB8888, 0x0200000000000001ull));
	CHECK(rei_format_mod_supported(list, n, REI_FORMAT_ABGR8888, 0x0200000000000001ull));
	CHECK(!rei_format_mod_supported(list, n, REI_FORMAT_XRGB8888, 0x0200000000000001ull));

	struct rei_format_mod b[] = { { REI_FORMAT_ARGB8888, 0x0200000000000001ull },
				      { REI_FORMAT_XRGB8888, REI_MOD_LINEAR },
				      { REI_FORMAT_XBGR8888, REI_MOD_LINEAR } };
	struct rei_format_mod *both;
	size_t nb = rei_format_mod_intersect(list, n, b, 3, &both);
	CHECK(nb == 2);
	free(both);
	CHECK(rei_format_mod_intersect(list, n, NULL, 0, &both) == 0 && both == NULL);
	free(list);

	/* Truncated and hostile blobs are rejected, not trusted. */
	CHECK(rei_drm_parse_in_formats(blob, 10, &list) == 0);
	CHECK(rei_drm_parse_in_formats(blob, len - 8, &list) == 0);
	CHECK(rei_drm_parse_in_formats(NULL, 0, &list) == 0);
	uint32_t bad[6] = { 2, 0, 1, 24, 1, 28 };
	memcpy(blob, bad, sizeof bad);
	CHECK(rei_drm_parse_in_formats(blob, len, &list) == 0);
	uint32_t huge[6] = { 1, 0, 0xffffffffu, 24, 0xffffffffu, 24 };
	memcpy(blob, huge, sizeof huge);
	CHECK(rei_drm_parse_in_formats(blob, len, &list) == 0);
}

static struct rei_dmabuf *mkbuf(int w, int h, bool invert)
{
	struct rei_dmabuf proto = {
		.width = w, .height = h, .format = REI_FORMAT_XRGB8888, .modifier = REI_MOD_LINEAR,
		.n_planes = 1, .fd = { -1, -1, -1, -1 }, .stride = { (uint32_t)w * 4 },
		.y_invert = invert, .opaque = true,
	};
	return rei_dmabuf_create(&proto);
}

static void test_scanout(void)
{
	struct rei_drm_view_rect out = { 0, 0, 1920, 1080, 1920, 1080 };
	struct rei_drm_view_rect out2 = { 1920, 0, 1920, 1080, 1920, 1080 };

	struct rei_scene_snapshot *s = rei_snapshot_create(1);
	struct rei_pixbuf *wall = rei_pixbuf_create(1, 1, true);
	rei_snapshot_add(s, wall, 0, 0, 1920, 1080, true);
	struct rei_dmabuf *full = mkbuf(1920, 1080, false);
	rei_snapshot_add_dmabuf(s, full, 0, 0, 1920, 1080, true);

	const struct rei_drawable *d = rei_drm_scanout_candidate(s, &out);
	CHECK(d && d->dmabuf == full);
	/* The other output shows nothing of it, and nothing is a candidate there. */
	CHECK(rei_drm_scanout_candidate(s, &out2) == NULL);

	/* Anything drawn over it (a toolbar, a popup) rules scanout out. */
	rei_snapshot_add(s, wall, 10, 10, 50, 50, false);
	CHECK(rei_drm_scanout_candidate(s, &out) == NULL);
	rei_snapshot_unref(s);

	/* Wrong size, non-opaque, inverted: not eligible. */
	struct rei_dmabuf *small = mkbuf(1280, 720, false);
	s = rei_snapshot_create(2);
	rei_snapshot_add_dmabuf(s, small, 0, 0, 1920, 1080, true);
	CHECK(rei_drm_scanout_candidate(s, &out) == NULL);
	rei_snapshot_unref(s);

	/* An ARGB buffer may have transparency, so it cannot be scanned out alone. */
	struct rei_dmabuf argb_proto = {
		.width = 1920, .height = 1080, .format = REI_FORMAT_ARGB8888,
		.modifier = REI_MOD_LINEAR, .n_planes = 1, .fd = { -1, -1, -1, -1 },
		.stride = { 1920 * 4 }, .opaque = false,
	};
	struct rei_dmabuf *argb = rei_dmabuf_create(&argb_proto);
	s = rei_snapshot_create(3);
	rei_snapshot_add_dmabuf(s, argb, 0, 0, 1920, 1080, false);
	CHECK(rei_drm_scanout_candidate(s, &out) == NULL);
	rei_snapshot_unref(s);

	struct rei_dmabuf *inv = mkbuf(1920, 1080, true);
	s = rei_snapshot_create(4);
	rei_snapshot_add_dmabuf(s, inv, 0, 0, 1920, 1080, true);
	CHECK(rei_drm_scanout_candidate(s, &out) == NULL);
	rei_snapshot_unref(s);

	/* A clip that cuts it off; a client-side buffer that is not a dma-buf. */
	s = rei_snapshot_create(5);
	rei_snapshot_add_dmabuf(s, full, 0, 0, 1920, 1080, true);
	rei_snapshot_clip_from(s, 0, 0, 0, 1000, 1000);
	CHECK(rei_drm_scanout_candidate(s, &out) == NULL);
	rei_snapshot_unref(s);

	s = rei_snapshot_create(6);
	rei_snapshot_add(s, wall, 0, 0, 1920, 1080, true);
	CHECK(rei_drm_scanout_candidate(s, &out) == NULL);
	rei_snapshot_unref(s);

	/* A window on the second output does not disturb the first. */
	s = rei_snapshot_create(7);
	rei_snapshot_add_dmabuf(s, full, 0, 0, 1920, 1080, true);
	rei_snapshot_add(s, wall, 1920, 0, 400, 300, false);
	CHECK(rei_drm_scanout_candidate(s, &out) != NULL);
	rei_snapshot_unref(s);

	rei_pixbuf_unref(wall);
	rei_dmabuf_unref(full);
	rei_dmabuf_unref(small);
	rei_dmabuf_unref(inv);
	rei_dmabuf_unref(argb);
}

static void test_cursor(void)
{
	struct rei_drm_view_rect out = { 1920, 0, 1920, 1080, 1920, 1080 };
	int32_t x, y;
	bool vis;
	rei_drm_cursor_place(2000.5f, 100.0f, 24, 24, &out, 1.0, &x, &y, &vis);
	CHECK(vis && x == 80 && y == 100);
	/* Straddling the left edge: still visible, with a negative position. */
	rei_drm_cursor_place(1910.0f, 5.0f, 24, 24, &out, 1.0, &x, &y, &vis);
	CHECK(vis && x == -10);
	/* Entirely on the neighbouring output. */
	rei_drm_cursor_place(1000.0f, 5.0f, 24, 24, &out, 1.0, &x, &y, &vis);
	CHECK(!vis);
	rei_drm_cursor_place(1900.0f, 5.0f, 24, 24, &out, 1.0, &x, &y, &vis);
	CHECK(vis && x == -20);  /* ends 4 px into the output */
	rei_drm_cursor_place(1890.0f, 5.0f, 24, 24, &out, 1.0, &x, &y, &vis);
	CHECK(!vis);
}

int main(void)
{
	test_names();
	test_refresh();
	test_parse();
	test_pick_mode();
	test_crtcs();
	test_planes();
	test_in_formats();
	test_scanout();
	test_cursor();
	puts("drm_logic: ok");
	return 0;
}
