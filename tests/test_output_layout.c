#include <wayland-util.h>

#include "check.h"
#include "output.h"

static struct rei_output mk(int w, int h, double scale, bool pinned, int x, int y)
{
	struct rei_output o = { .width = w, .height = h, .scale = scale,
				.has_position = pinned, .x = x, .y = y };
	return o;
}

int main(void)
{
	struct wl_list list;
	wl_list_init(&list);

	/* Unpinned outputs tile left to right in list order. */
	struct rei_output a = mk(800, 600, 1.0, false, 0, 0);
	struct rei_output b = mk(1024, 768, 1.0, false, 0, 0);
	wl_list_insert(list.prev, &a.link);
	wl_list_insert(list.prev, &b.link);
	rei_outputs_layout(&list);
	CHECK(a.x == 0 && a.y == 0);
	CHECK(b.x == 800 && b.y == 0);

	/* Scale shrinks the logical size, which is what neighbours abut. */
	struct rei_output c = mk(3840, 2160, 2.0, false, 0, 0);
	wl_list_insert(list.prev, &c.link);
	rei_outputs_layout(&list);
	CHECK(c.x == 800 + 1024);
	int32_t w, h;
	rei_output_logical_size(&c, &w, &h);
	CHECK(w == 1920 && h == 1080);

	/* Pinned outputs keep their spot and unpinned ones go after them. */
	struct rei_output d = mk(1000, 1000, 1.0, true, 5000, -300);
	wl_list_insert(list.prev, &d.link);
	rei_outputs_layout(&list);
	CHECK(d.x == 5000 && d.y == -300);
	CHECK(a.x == 6000 && b.x == 6800 && c.x == 7824);

	/* Removing an output closes the gap. */
	wl_list_remove(&a.link);
	rei_outputs_layout(&list);
	CHECK(b.x == 6000);

	puts("test_output_layout: ok");
	return 0;
}
