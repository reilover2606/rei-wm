#include "check.h"
#include "proto/surface.h"

static long area(const struct rei_rects *r)
{
	long a = 0;
	for (size_t i = 0; i < r->n; i++)
		a += (long)r->items[i].w * r->items[i].h;
	return a;
}

int main(void)
{
	struct rei_rects r = { 0 };

	/* Empty and negative rectangles are ignored. */
	rei_rects_add(&r, (struct rei_rect){ 0, 0, 0, 10 });
	rei_rects_add(&r, (struct rei_rect){ 0, 0, 10, -1 });
	CHECK(r.n == 0);

	rei_rects_add(&r, (struct rei_rect){ 0, 0, 100, 50 });
	CHECK(rei_rects_cover(&r, 100, 50));
	CHECK(rei_rects_cover(&r, 10, 10));
	CHECK(!rei_rects_cover(&r, 101, 50));

	/* Punching a hole conserves the remaining area and leaves the hole empty. */
	rei_rects_subtract(&r, (struct rei_rect){ 40, 10, 20, 20 });
	CHECK(area(&r) == 100 * 50 - 20 * 20);
	CHECK(!rei_rects_cover(&r, 100, 50));
	for (size_t i = 0; i < r.n; i++) {
		const struct rei_rect *a = &r.items[i];
		CHECK(a->x + a->w <= 40 || a->x >= 60 || a->y + a->h <= 10 || a->y >= 30);
	}

	/* Subtracting something that doesn't overlap changes nothing. */
	long before = area(&r);
	rei_rects_subtract(&r, (struct rei_rect){ 500, 500, 10, 10 });
	CHECK(area(&r) == before);

	/* A hole bigger than the rectangle removes it entirely. */
	rei_rects_subtract(&r, (struct rei_rect){ -10, -10, 1000, 1000 });
	CHECK(r.n == 0);

	/* Partial overlaps at an edge and a corner. */
	rei_rects_add(&r, (struct rei_rect){ 0, 0, 10, 10 });
	rei_rects_subtract(&r, (struct rei_rect){ 5, 5, 20, 20 });
	CHECK(area(&r) == 100 - 25);
	rei_rects_clear(&r);
	CHECK(r.n == 0 && r.items == NULL);

	puts("test_rects: ok");
	return 0;
}
