/*
 * linux-dmabuf feedback (version 4): a client that asks is told the main device and the usable format/modifier pairs
 * (a table in a memfd plus a tranche of indices into it); a version 3 client still gets the flat list of events. Uses
 * REI_TEST_DMABUF, which makes the headless backend advertise four pairs as if a GPU were behind it.
 */
#define SOCKET "rei-dmabuf-fb-test"
#include "testlib.h"

#include <sys/mman.h>
#include <sys/sysmacros.h>

#include "linux-dmabuf-unstable-v1-client-protocol.h"

static uint32_t global_id(const struct client *c, const char *name, uint32_t *version)
{
	for (int i = 0; i < c->n_globals; i++)
		if (!strcmp(c->globals[i].name, name)) {
			if (version)
				*version = c->globals[i].version;
			return c->globals[i].id;
		}
	CHECK(!"global missing");
	return 0;
}

struct fb {
	bool done;
	int n_done;
	dev_t main_device;
	bool have_table;
	struct { uint32_t format; uint64_t modifier; } table[16];
	size_t n_table;
	int n_tranches;
	dev_t target;
	uint32_t flags;
	uint16_t idx[16];
	size_t n_idx;
};

static void f_done(void *d, struct zwp_linux_dmabuf_feedback_v1 *f)
{
	(void)f;
	struct fb *b = d;
	b->done = true;
	b->n_done++;
}
static void f_table(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, int32_t fd, uint32_t size)
{
	(void)f;
	struct fb *b = d;
	uint8_t *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	CHECK(map != MAP_FAILED);
	b->n_table = size / 16;
	for (size_t i = 0; i < b->n_table && i < 16; i++) {
		memcpy(&b->table[i].format, map + i * 16, 4);
		memcpy(&b->table[i].modifier, map + i * 16 + 8, 8);
	}
	munmap(map, size);
	close(fd);
	b->have_table = true;
}
static void f_main(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, struct wl_array *dev)
{
	(void)f;
	struct fb *b = d;
	CHECK(dev->size == sizeof(dev_t));
	memcpy(&b->main_device, dev->data, sizeof(dev_t));
}
static void f_tranche_done(void *d, struct zwp_linux_dmabuf_feedback_v1 *f)
{
	(void)f;
	((struct fb *)d)->n_tranches++;
}
static void f_target(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, struct wl_array *dev)
{
	(void)f;
	memcpy(&((struct fb *)d)->target, dev->data, sizeof(dev_t));
}
static void f_formats(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, struct wl_array *idx)
{
	(void)f;
	struct fb *b = d;
	b->n_idx = idx->size / 2;
	memcpy(b->idx, idx->data, idx->size < sizeof b->idx ? idx->size : sizeof b->idx);
}
static void f_flags(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, uint32_t flags)
{
	(void)f;
	((struct fb *)d)->flags = flags;
}
static const struct zwp_linux_dmabuf_feedback_v1_listener fl = { f_done, f_table, f_main, f_tranche_done, f_target, f_formats, f_flags };

static int g_modifiers_seen;
static void dm_format(void *d, struct zwp_linux_dmabuf_v1 *z, uint32_t f) { (void)d; (void)z; (void)f; }
static void dm_modifier(void *d, struct zwp_linux_dmabuf_v1 *z, uint32_t f, uint32_t hi, uint32_t lo)
{
	(void)d; (void)z; (void)f; (void)hi; (void)lo;
	g_modifiers_seen++;
}
static const struct zwp_linux_dmabuf_v1_listener dl = { dm_format, dm_modifier };

static void check_feedback(const struct fb *b)
{
	CHECK(b->done && b->have_table);
	CHECK(b->main_device == makedev(226, 128));
	CHECK(b->n_table == 4);
	CHECK(b->n_tranches == 1 && b->target == makedev(226, 128) && b->flags == 0);
	CHECK(b->n_idx == 4);
	bool xrgb_linear = false, xrgb_tiled = false, argb = false;
	for (size_t i = 0; i < b->n_idx; i++) {
		CHECK(b->idx[i] < b->n_table);
		uint32_t f = b->table[b->idx[i]].format;
		uint64_t m = b->table[b->idx[i]].modifier;
		xrgb_linear = xrgb_linear || (f == 0x34325258 && m == 0);   /* XR24 */
		xrgb_tiled = xrgb_tiled || (f == 0x34325258 && m == 0x0100000000000001ull);
		argb = argb || (f == 0x34325241 && m == 0);                   /* AR24 */
	}
	CHECK(xrgb_linear && xrgb_tiled && argb);
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	setenv("REI_TEST_DMABUF", "1", 1);
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-dmabuf-fb-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);
	start_compositor(argv[1], "[toolbar]\nenabled = false\n[[output]]\nname = \"OUT\"\nwidth = 400\nheight = 300\n", true);

	struct client c;
	client_connect(&c);
	uint32_t version = 0;
	uint32_t id = global_id(&c, "zwp_linux_dmabuf_v1", &version);
	CHECK(version == 4);   /* with a main device: feedback */

	/* A version 4 client: feedback, and none of the version 3 modifier events. */
	struct zwp_linux_dmabuf_v1 *z = wl_registry_bind(c.registry, id, &zwp_linux_dmabuf_v1_interface, 4);
	zwp_linux_dmabuf_v1_add_listener(z, &dl, NULL);
	static struct fb def, surf;
	struct zwp_linux_dmabuf_feedback_v1 *f1 = zwp_linux_dmabuf_v1_get_default_feedback(z);
	zwp_linux_dmabuf_feedback_v1_add_listener(f1, &fl, &def);
	struct window w;
	toplevel_create(&c, &w, "W");
	struct zwp_linux_dmabuf_feedback_v1 *f2 = zwp_linux_dmabuf_v1_get_surface_feedback(z, w.surface);
	zwp_linux_dmabuf_feedback_v1_add_listener(f2, &fl, &surf);
	CHECK(wl_display_roundtrip(c.d) >= 0);
	CHECK(wl_display_roundtrip(c.d) >= 0);
	check_feedback(&def);
	check_feedback(&surf);
	CHECK(g_modifiers_seen == 0);

	/* A version 3 client: the flat list. */
	struct zwp_linux_dmabuf_v1 *z3 = wl_registry_bind(c.registry, id, &zwp_linux_dmabuf_v1_interface, 3);
	zwp_linux_dmabuf_v1_add_listener(z3, &dl, NULL);
	CHECK(wl_display_roundtrip(c.d) >= 0);
	CHECK(g_modifiers_seen == 4);

	zwp_linux_dmabuf_feedback_v1_destroy(f1);
	zwp_linux_dmabuf_feedback_v1_destroy(f2);
	CHECK(stop_compositor() == 0);
	puts("dmabuf feedback: ok");
	return 0;
}
