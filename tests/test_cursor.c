/*
 * Pointer cursors: the XCursor parser and theme lookup (names, aliases, inheritance, the fallbacks), then
 * end to end in the headless compositor with a theme the test writes itself: the arrow on the desktop,
 * the compositor's own hover shapes (the Start button), a client's named shape (wp_cursor_shape_v1), a
 * client's own picture (wl_pointer.set_cursor), and hiding it. The request ends when the pointer
 * enters another surface.
 */
#define SOCKET "rei-cursor-test"
#include "testlib.h"

#include "cursor-shape-v1-client-protocol.h"
#include "cursor.h"

static char g_themes[256];

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Writes an XCursor file with one image per size in `sizes` (nominal size = width = height), every pixel
 * `argb`, hotspot (hx, hy). */
static void write_xcursor(const char *path, const int *sizes, int n, uint32_t argb, int hx, int hy)
{
	size_t total = 16 + (size_t)n * 12, off = total;
	for (int i = 0; i < n; i++)
		total += 36 + (size_t)sizes[i] * (size_t)sizes[i] * 4;
	uint8_t *f = calloc(1, total);
	CHECK(f);
	put32(f, 0x72756358u); put32(f + 4, 16); put32(f + 8, 0x10000); put32(f + 12, (uint32_t)n);
	for (int i = 0; i < n; i++) {
		uint8_t *toc = f + 16 + i * 12;
		put32(toc, 0xfffd0002u); put32(toc + 4, (uint32_t)sizes[i]); put32(toc + 8, (uint32_t)off);
		uint8_t *c = f + off;
		put32(c, 36); put32(c + 4, 0xfffd0002u); put32(c + 8, (uint32_t)sizes[i]); put32(c + 12, 1);
		put32(c + 16, (uint32_t)sizes[i]); put32(c + 20, (uint32_t)sizes[i]);
		put32(c + 24, (uint32_t)hx); put32(c + 28, (uint32_t)hy); put32(c + 32, 0);
		for (int k = 0; k < sizes[i] * sizes[i]; k++)
			put32(c + 36 + k * 4, argb);
		off += 36 + (size_t)sizes[i] * (size_t)sizes[i] * 4;
	}
	FILE *o = fopen(path, "wb");
	CHECK(o && fwrite(f, 1, total, o) == total);
	fclose(o);
	free(f);
}

static void make_dir(const char *path)
{
	CHECK(mkdir(path, 0700) == 0 || errno == EEXIST);
}

static void test_parser(void)
{
	const int sizes[] = { 8, 16, 32 };
	char path[300];
	snprintf(path, sizeof path, "%s/one", g_themes);
	write_xcursor(path, sizes, 3, 0xff102030, 3, 4);
	size_t len;
	char *raw = slurp(path);
	CHECK(raw);
	len = strlen(raw);   /* not usable: the file holds zero bytes */
	(void)len;
	free(raw);

	FILE *f = fopen(path, "rb");
	CHECK(f);
	uint8_t buf[8192];
	size_t n = fread(buf, 1, sizeof buf, f);
	fclose(f);

	int nominal = 0;
	int32_t hx, hy;
	struct rei_pixbuf *pb = rei_xcursor_parse(buf, n, 20, &nominal, &hx, &hy);
	CHECK(pb && nominal == 16 && pb->width == 16 && pb->height == 16 && hx == 3 && hy == 4);
	CHECK(pb->data[0] == 0x30 && pb->data[1] == 0x20 && pb->data[2] == 0x10 && pb->data[3] == 0xff);
	rei_pixbuf_unref(pb);
	pb = rei_xcursor_parse(buf, n, 100, &nominal, &hx, &hy);
	CHECK(pb && nominal == 32);
	rei_pixbuf_unref(pb);
	pb = rei_xcursor_parse(buf, n, 1, &nominal, &hx, &hy);
	CHECK(pb && nominal == 8);
	rei_pixbuf_unref(pb);

	/* Damaged files are refused, not drawn. */
	CHECK(!rei_xcursor_parse(buf, 10, 16, &nominal, &hx, &hy));
	CHECK(!rei_xcursor_parse(buf, n - 40, 32, &nominal, &hx, &hy) || nominal != 32);
	uint8_t bad[8192];
	memcpy(bad, buf, n);
	bad[0] = 'Y';
	CHECK(!rei_xcursor_parse(bad, n, 16, &nominal, &hx, &hy));
	memcpy(bad, buf, n);
	put32(bad + 16 + 8, 0xfffffff0u);   /* an image offset far outside the file */
	CHECK(!rei_xcursor_parse(bad, n, 8, &nominal, &hx, &hy));
	/* A colour above its alpha is clamped. */
	const int one[] = { 8 };
	snprintf(path, sizeof path, "%s/odd", g_themes);
	write_xcursor(path, one, 1, 0x40ffffff, 0, 0);
	f = fopen(path, "rb");
	n = fread(buf, 1, sizeof buf, f);
	fclose(f);
	pb = rei_xcursor_parse(buf, n, 8, &nominal, &hx, &hy);
	CHECK(pb && pb->data[0] == 0x40 && pb->data[3] == 0x40);
	rei_pixbuf_unref(pb);
}

static void test_theme_lookup(void)
{
	char a[600], b[700];
	/* "base" has the modern names' older spellings; "kid" inherits from it and has its own text cursor. */
	snprintf(a, sizeof a, "%s/base", g_themes); make_dir(a);
	snprintf(a, sizeof a, "%s/base/cursors", g_themes); make_dir(a);
	const int s24[] = { 24 };
	snprintf(b, sizeof b, "%s/left_ptr", a); write_xcursor(b, s24, 1, 0xffff0000, 1, 1);
	snprintf(b, sizeof b, "%s/hand2", a); write_xcursor(b, s24, 1, 0xff00ff00, 5, 2);
	snprintf(a, sizeof a, "%s/kid", g_themes); make_dir(a);
	snprintf(a, sizeof a, "%s/kid/cursors", g_themes); make_dir(a);
	snprintf(b, sizeof b, "%s/text", a); write_xcursor(b, s24, 1, 0xff0000ff, 12, 12);
	snprintf(b, sizeof b, "%s/kid/index.theme", g_themes);
	write_text(b, "[Icon Theme]\nName=Kid\nInherits=base\n");

	setenv("XCURSOR_PATH", g_themes, 1);
	size_t n;
	char **names = rei_cursor_list_themes(&n);
	CHECK(n >= 2);
	bool base = false, kid = false;
	for (size_t i = 0; i < n; i++) {
		base |= !strcmp(names[i], "base");
		kid |= !strcmp(names[i], "kid");
	}
	CHECK(base && kid);
	rei_cursor_free_names(names, n);

	struct rei_cursor_theme *t = rei_cursor_theme_open("kid", 24, 1);
	CHECK(!strcmp(rei_cursor_theme_name(t), "kid"));
	const struct rei_cursor_image *i = rei_cursor_theme_shape(t, REI_SHAPE_TEXT);
	CHECK(i->pixbuf->data[0] == 0xff && i->hotspot_x == 12 && i->width == 24);   /* its own */
	i = rei_cursor_theme_shape(t, REI_SHAPE_POINTER);
	CHECK(i->pixbuf->data[1] == 0xff && i->hotspot_x == 5);                      /* hand2, inherited */
	i = rei_cursor_theme_shape(t, REI_SHAPE_DEFAULT);
	CHECK(i->pixbuf->data[2] == 0xff && i->hotspot_y == 1);                      /* left_ptr, inherited */
	i = rei_cursor_theme_shape(t, REI_SHAPE_ZOOM_IN);                            /* nobody has it: the arrow */
	CHECK(i->pixbuf->data[2] == 0xff);
	rei_cursor_theme_close(t);

	/* A bigger pointer is asked for from a bigger picture and shown at the logical size. */
	t = rei_cursor_theme_open("kid", 24, 2);   /* 48 pixels wanted, only a 24px picture exists: still 24 logical */
	i = rei_cursor_theme_shape(t, REI_SHAPE_TEXT);
	CHECK(i->pixbuf->width == 24 && i->width == 24 && i->hotspot_x == 12);
	rei_cursor_theme_close(t);
	t = rei_cursor_theme_open("kid", 48, 1);   /* stretched to 48 */
	i = rei_cursor_theme_shape(t, REI_SHAPE_TEXT);
	CHECK(i->width == 48 && i->hotspot_x == 24);
	rei_cursor_theme_close(t);

	/* A theme that is not installed: another one is used, and the arrow is still there. */
	t = rei_cursor_theme_open("no-such-theme", 24, 1);
	CHECK(rei_cursor_theme_shape(t, REI_SHAPE_DEFAULT)->pixbuf != NULL);
	rei_cursor_theme_close(t);

	CHECK(rei_cursor_shape_from_name("ew-resize") == REI_SHAPE_EW_RESIZE);
	CHECK(!strcmp(rei_cursor_shape_name(REI_SHAPE_TEXT), "text"));
	CHECK(rei_cursor_shape_from_name("bogus") == REI_SHAPE_NONE);
}

/* ---- end to end ---- */

static struct wp_cursor_shape_device_v1 *g_dev;

static uint32_t enter_serial(const struct client *c)
{
	const struct ev *e = ev_last(c, EV_PTR_ENTER);
	CHECK(e);
	return e->a;
}

static void expect_cursor(const char *what, int x, int y, int r, int g, int b)
{
	struct ppm img = screenshot();
	expect_px(&img, x, y, r, g, b, 8, what);
	free(img.rgb);
}

static void move(int x, int y)
{
	inject("motion_abs %d %d", x, y);
	settle_all();
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-cursor-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);
	snprintf(g_themes, sizeof g_themes, "%s/themes", g_dir);
	make_dir(g_themes);

	test_parser();
	test_theme_lookup();

	/* The compositor's theme: red arrow, blue text beam, green hand; all 8x8 with the hotspot in the corner. */
	char a[600], b[700];
	snprintf(a, sizeof a, "%s/tt", g_themes); make_dir(a);
	snprintf(a, sizeof a, "%s/tt/cursors", g_themes); make_dir(a);
	const int s8[] = { 8 };
	snprintf(b, sizeof b, "%s/left_ptr", a); write_xcursor(b, s8, 1, 0xffff0000, 0, 0);
	snprintf(b, sizeof b, "%s/xterm", a); write_xcursor(b, s8, 1, 0xff0000ff, 0, 0);
	snprintf(b, sizeof b, "%s/hand2", a); write_xcursor(b, s8, 1, 0xff00ff00, 0, 0);
	setenv("XCURSOR_PATH", g_themes, 1);

	char config[400];
	snprintf(config, sizeof config,
		 "[cursor]\ntheme = \"tt\"\nsize = 8\n[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n", OUT_W, OUT_H);
	start_compositor(argv[1], config, true);

	struct client c;
	client_connect(&c);
	g_clients[0] = &c;
	client_bind_pointer(&c);
	uint32_t mgr_id = 0;
	for (int i = 0; i < c.n_globals; i++)
		if (!strcmp(c.globals[i].name, "wp_cursor_shape_manager_v1"))
			mgr_id = c.globals[i].id;
	CHECK(mgr_id);
	struct wp_cursor_shape_manager_v1 *mgr = wl_registry_bind(c.registry, mgr_id, &wp_cursor_shape_manager_v1_interface, 1);
	g_dev = wp_cursor_shape_manager_v1_get_pointer(mgr, c.pointer);
	CHECK(wl_display_roundtrip(c.d) >= 0);
	CHECK(log_has("cursor: theme 'tt', size 8"));

	/* The desktop: the theme's arrow. */
	move(300, 400);
	expect_cursor("arrow on the desktop", 303, 403, 255, 0, 0);

	/* The compositor's own button: the hand. */
	move(12, 14);
	expect_cursor("hand over the Start button", 15, 17, 0, 255, 0);
	move(300, 400);
	expect_cursor("arrow again", 303, 403, 255, 0, 0);

	/* A client's window. */
	struct window w;
	open_win(&c, &w, "cursor", 0xff303030);
	move(400, 300);
	CHECK(ev_count(&c, EV_PTR_ENTER) >= 1);
	expect_cursor("arrow until the client asks", 403, 303, 255, 0, 0);

	wp_cursor_shape_device_v1_set_shape(g_dev, enter_serial(&c), WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
	CHECK(wl_display_roundtrip(c.d) >= 0);
	settle_all();
	expect_cursor("the client's text beam", 403, 303, 0, 0, 255);
	move(420, 320);
	expect_cursor("it stays while the pointer is in the window", 423, 323, 0, 0, 255);

	/* The compositor's widgets win over the client's request while the pointer is on them. */
	move(12, 14);
	expect_cursor("hand over the Start button again", 15, 17, 0, 255, 0);
	/* Coming back in is a new enter: the request has ended. */
	move(400, 300);
	expect_cursor("a new enter starts from the arrow", 403, 303, 255, 0, 0);

	/* A picture: 6x6 yellow, hotspot (2,2), so it covers (398..403, 298..303). */
	struct wl_surface *cs = wl_compositor_create_surface(c.compositor);
	wl_surface_attach(cs, make_buffer(&c, 6, 6, WL_SHM_FORMAT_ARGB8888, 0xffffff00), 0, 0);
	wl_surface_damage(cs, 0, 0, 6, 6);
	wl_surface_commit(cs);
	wl_pointer_set_cursor(c.pointer, enter_serial(&c), cs, 2, 2);
	CHECK(wl_display_roundtrip(c.d) >= 0);
	settle_all();
	expect_cursor("the client's own picture", 400, 300, 255, 255, 0);
	expect_cursor("its hotspot", 398, 298, 255, 255, 0);
	expect_cursor("past its edge", 405, 305, 0x30, 0x30, 0x30);

	/* Hidden. */
	wl_pointer_set_cursor(c.pointer, enter_serial(&c), NULL, 0, 0);
	CHECK(wl_display_roundtrip(c.d) >= 0);
	settle_all();
	expect_cursor("hidden", 400, 300, 0x30, 0x30, 0x30);

	/* Not the client under the pointer: ignored. */
	move(400, 595);   /* the margin below the window: no client has the pointer */
	wp_cursor_shape_device_v1_set_shape(g_dev, 1, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
	CHECK(wl_display_roundtrip(c.d) >= 0);
	settle_all();
	expect_cursor("a request while the pointer is elsewhere is ignored", 403, 595, 255, 0, 0);
	move(400, 300);   /* and it does not linger: back in the window it is the arrow again */
	expect_cursor("not remembered", 403, 303, 255, 0, 0);

	CHECK(stop_compositor() == 0);
	printf("test_cursor: ok\n");
	return 0;
}
