#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "layout.h"

static struct rei_rect R(int x, int y, int w, int h) { return (struct rei_rect){ x, y, w, h }; }

static void expect_rect(const struct rei_node *n, int x, int y, int w, int h)
{
	if (n->rect.x != x || n->rect.y != y || n->rect.w != w || n->rect.h != h) {
		fprintf(stderr, "rect: got (%d,%d %dx%d), expected (%d,%d %dx%d)\n",
			n->rect.x, n->rect.y, n->rect.w, n->rect.h, x, y, w, h);
		exit(1);
	}
}

/* Leaves in tree order, as the integers stored in their data pointers. */
static void expect_order(struct rei_tree *t, const char *expect)
{
	struct rei_node *leaves[64];
	size_t n = rei_tree_leaves(t, leaves, 64);
	char got[65] = { 0 };
	for (size_t i = 0; i < n; i++)
		got[i] = (char)(intptr_t)leaves[i]->data;
	if (strcmp(got, expect) != 0) {
		fprintf(stderr, "leaf order: got '%s', expected '%s'\n", got, expect);
		exit(1);
	}
	CHECK(rei_tree_check(t));
}

static struct rei_node *add(struct rei_tree *t, struct rei_node *after, char name)
{
	return rei_tree_add_leaf(t, after, (void *)(intptr_t)name);
}

static void test_single_and_two(void)
{
	struct rei_tree *t = rei_tree_create(REI_LAYOUT_SPLIT_H);
	struct rei_node *a = add(t, NULL, 'a');
	rei_tree_layout(t, R(10, 20, 100, 50), 10);
	expect_rect(a, 10, 20, 100, 50);  /* a lone window fills the area */

	struct rei_node *b = add(t, a, 'b');
	rei_tree_layout(t, R(10, 20, 100, 50), 10);
	expect_rect(a, 10, 20, 45, 50);   /* (100-10)/2 each, a gap between */
	expect_rect(b, 65, 20, 45, 50);
	expect_order(t, "ab");

	/* Odd widths: sizes differ by one pixel and still sum exactly. */
	rei_tree_layout(t, R(0, 0, 101, 40), 10);
	CHECK(a->rect.w + b->rect.w + 10 == 101);
	CHECK(abs(a->rect.w - b->rect.w) <= 1);
	CHECK(a->rect.x + a->rect.w + 10 == b->rect.x);

	/* No gap: the windows tile the area exactly. */
	rei_tree_layout(t, R(0, 0, 101, 40), 0);
	CHECK(a->rect.w + b->rect.w == 101 && b->rect.x == a->rect.w);
	rei_tree_destroy(t);
}

static void test_three_and_nested(void)
{
	struct rei_tree *t = rei_tree_create(REI_LAYOUT_SPLIT_H);
	struct rei_node *a = add(t, NULL, 'a');
	struct rei_node *b = add(t, a, 'b');
	struct rei_node *c = add(t, b, 'c');
	rei_tree_layout(t, R(0, 0, 320, 100), 10);
	expect_rect(a, 0, 0, 100, 100);
	expect_rect(b, 110, 0, 100, 100);
	expect_rect(c, 220, 0, 100, 100);

	/* Split b vertically and add d: b and d stack in b's slot. */
	rei_tree_split(t, b, REI_LAYOUT_SPLIT_V);
	struct rei_node *d = add(t, b, 'd');
	rei_tree_layout(t, R(0, 0, 320, 100), 10);
	expect_rect(a, 0, 0, 100, 100);
	expect_rect(b, 110, 0, 100, 45);
	expect_rect(d, 110, 55, 100, 45);
	expect_rect(c, 220, 0, 100, 100);
	expect_order(t, "abdc");
	rei_tree_destroy(t);
}

static void test_weights(void)
{
	struct rei_tree *t = rei_tree_create(REI_LAYOUT_SPLIT_H);
	struct rei_node *a = add(t, NULL, 'a');
	struct rei_node *b = add(t, a, 'b');
	a->weight = 2.0;
	b->weight = 1.0;
	rei_tree_layout(t, R(0, 0, 300, 10), 0);
	expect_rect(a, 0, 0, 200, 10);
	expect_rect(b, 200, 0, 100, 10);

	/* A newcomer gets a typical share and existing proportions survive. */
	struct rei_node *c = add(t, b, 'c');
	CHECK(c->weight == 1.5);
	rei_tree_layout(t, R(0, 0, 450, 10), 0);
	CHECK(a->rect.w == 200 && b->rect.w == 100 && c->rect.w == 150);
	rei_tree_destroy(t);
}

static void test_remove_and_collapse(void)
{
	struct rei_tree *t = rei_tree_create(REI_LAYOUT_SPLIT_H);
	struct rei_node *a = add(t, NULL, 'a');
	struct rei_node *b = add(t, a, 'b');
	rei_tree_split(t, b, REI_LAYOUT_SPLIT_V);
	struct rei_node *c = add(t, b, 'c');
	expect_order(t, "abc");

	/* Removing c leaves the V split with one child, which must collapse. */
	rei_tree_remove_leaf(t, c);
	expect_order(t, "ab");
	rei_tree_layout(t, R(0, 0, 210, 100), 10);
	expect_rect(b, 110, 0, 100, 100);  /* b is back to a full-height column */

	/* Emptying the tree leaves a usable, empty root. */
	rei_tree_remove_leaf(t, a);
	rei_tree_remove_leaf(t, b);
	CHECK(rei_tree_leaf_count(t) == 0 && rei_tree_check(t));
	struct rei_node *d = add(t, NULL, 'd');
	rei_tree_layout(t, R(0, 0, 50, 50), 10);
	expect_rect(d, 0, 0, 50, 50);
	rei_tree_destroy(t);
}

static void test_split_semantics(void)
{
	/* A lone window: splitting just changes the root's direction. */
	struct rei_tree *t = rei_tree_create(REI_LAYOUT_SPLIT_H);
	struct rei_node *a = add(t, NULL, 'a');
	rei_tree_split(t, a, REI_LAYOUT_SPLIT_V);
	struct rei_node *b = add(t, a, 'b');
	rei_tree_layout(t, R(0, 0, 100, 210), 10);
	expect_rect(a, 0, 0, 100, 100);   /* stacked, not side by side */
	expect_rect(b, 0, 110, 100, 100);
	CHECK(rei_tree_check(t));
	rei_tree_destroy(t);
}

static void test_move(void)
{
	struct rei_tree *t = rei_tree_create(REI_LAYOUT_SPLIT_H);
	struct rei_node *a = add(t, NULL, 'a');
	struct rei_node *b = add(t, a, 'b');
	struct rei_node *c = add(t, b, 'c');
	expect_order(t, "abc");

	/* Swap past a sibling; the edge stops it. */
	CHECK(rei_tree_move(t, a, REI_DIR_RIGHT));
	expect_order(t, "bac");
	CHECK(rei_tree_move(t, a, REI_DIR_RIGHT));
	expect_order(t, "bca");
	CHECK(!rei_tree_move(t, a, REI_DIR_RIGHT));
	expect_order(t, "bca");
	CHECK(rei_tree_move(t, c, REI_DIR_LEFT));
	expect_order(t, "cba");  /* c swapped with b */
	rei_tree_destroy(t);

	/* Moving across the other axis wraps the row into a column. */
	t = rei_tree_create(REI_LAYOUT_SPLIT_H);
	a = add(t, NULL, 'a');
	b = add(t, a, 'b');
	CHECK(rei_tree_move(t, b, REI_DIR_DOWN));
	CHECK(rei_tree_check(t));
	rei_tree_layout(t, R(0, 0, 100, 210), 10);
	/* a on top, b below it. */
	CHECK(a->rect.y < b->rect.y);
	CHECK(a->rect.w == 100 && b->rect.w == 100);
	expect_order(t, "ab");
	rei_tree_destroy(t);

	/* Into a neighbouring container, then back out. */
	t = rei_tree_create(REI_LAYOUT_SPLIT_H);
	a = add(t, NULL, 'a');
	b = add(t, a, 'b');
	c = add(t, b, 'c');
	rei_tree_split(t, c, REI_LAYOUT_SPLIT_V);
	struct rei_node *d = add(t, c, 'd');   /* root: a b [c / d] */
	expect_order(t, "abcd");
	CHECK(rei_tree_move(t, b, REI_DIR_RIGHT));  /* b dives into the [c/d] column */
	CHECK(rei_tree_check(t));
	rei_tree_layout(t, R(0, 0, 210, 210), 10);
	CHECK(b->rect.x == c->rect.x && b->rect.x == d->rect.x);  /* now in that column */
	CHECK(rei_tree_move(t, b, REI_DIR_LEFT));    /* and out to the left of it */
	CHECK(rei_tree_check(t));
	rei_tree_layout(t, R(0, 0, 210, 210), 10);
	CHECK(b->rect.x < c->rect.x);
	rei_tree_destroy(t);
}

static void test_separate_gaps(void)
{
	/* Side-by-side windows use gap_x, stacked ones use gap_y. */
	struct rei_tree *t = rei_tree_create(REI_LAYOUT_SPLIT_H);
	struct rei_node *a = add(t, NULL, 'a');
	struct rei_node *b = add(t, a, 'b');
	rei_tree_split(t, b, REI_LAYOUT_SPLIT_V);
	struct rei_node *c = add(t, b, 'c');   /* a | [b / c] */
	rei_tree_layout_xy(t, R(0, 0, 210, 150), 10, 30);
	expect_rect(a, 0, 0, 100, 150);
	expect_rect(b, 110, 0, 100, 60);       /* (150 - 30) / 2 */
	expect_rect(c, 110, 90, 100, 60);
	/* The single-gap form is the same thing with equal gaps. */
	rei_tree_layout(t, R(0, 0, 210, 150), 10);
	expect_rect(b, 110, 0, 100, 70);
	expect_rect(c, 110, 80, 100, 70);
	rei_tree_destroy(t);
}

static void test_insert_beside(void)
{
	/* Next to a window in a row of the same direction: just beside it. */
	struct rei_tree *t = rei_tree_create(REI_LAYOUT_SPLIT_H);
	struct rei_node *a = add(t, NULL, 'a');
	struct rei_node *b = add(t, a, 'b');
	struct rei_node *c = rei_tree_insert_beside(t, a, (void *)(intptr_t)'c', REI_DIR_RIGHT);
	CHECK(c && rei_tree_check(t));
	expect_order(t, "acb");
	struct rei_node *d = rei_tree_insert_beside(t, a, (void *)(intptr_t)'d', REI_DIR_LEFT);
	CHECK(d);
	expect_order(t, "dacb");

	/* Across the row's direction: the target gets a container, stacked. */
	struct rei_node *e = rei_tree_insert_beside(t, b, (void *)(intptr_t)'e', REI_DIR_DOWN);
	CHECK(e && rei_tree_check(t));
	expect_order(t, "dacbe");
	rei_tree_layout(t, R(0, 0, 400, 210), 10);
	CHECK(b->rect.x == e->rect.x && b->rect.w == e->rect.w);   /* same column */
	CHECK(b->rect.y < e->rect.y);                               /* e below b */
	struct rei_node *f = rei_tree_insert_beside(t, b, (void *)(intptr_t)'f', REI_DIR_UP);
	CHECK(f && rei_tree_check(t));
	expect_order(t, "dacfbe");

	/* A lone window just changes its container's direction. */
	rei_tree_destroy(t);
	t = rei_tree_create(REI_LAYOUT_SPLIT_H);
	a = add(t, NULL, 'a');
	b = rei_tree_insert_beside(t, a, (void *)(intptr_t)'b', REI_DIR_DOWN);
	CHECK(b && rei_tree_check(t));
	rei_tree_layout(t, R(0, 0, 100, 210), 10);
	CHECK(a->rect.w == 100 && b->rect.w == 100 && a->rect.y < b->rect.y);
	rei_tree_destroy(t);
}

static void test_neighbor(void)
{
	struct rei_tree *t = rei_tree_create(REI_LAYOUT_SPLIT_H);
	struct rei_node *a = add(t, NULL, 'a');
	struct rei_node *b = add(t, a, 'b');
	rei_tree_split(t, b, REI_LAYOUT_SPLIT_V);
	struct rei_node *c = add(t, b, 'c');   /* a | [b / c] */
	rei_tree_layout(t, R(0, 0, 210, 210), 10);

	CHECK(rei_tree_neighbor(t, a, REI_DIR_RIGHT) == b);  /* b and c are equally near: the first */
	CHECK(rei_tree_neighbor(t, b, REI_DIR_DOWN) == c);
	CHECK(rei_tree_neighbor(t, c, REI_DIR_UP) == b);
	CHECK(rei_tree_neighbor(t, b, REI_DIR_LEFT) == a);
	CHECK(rei_tree_neighbor(t, c, REI_DIR_LEFT) == a);
	CHECK(rei_tree_neighbor(t, a, REI_DIR_LEFT) == NULL);
	CHECK(rei_tree_neighbor(t, a, REI_DIR_UP) == NULL);
	CHECK(rei_tree_neighbor(t, c, REI_DIR_DOWN) == NULL);

	/* Direction scoring is usable across non-tree rectangles too. */
	struct rei_rect from = R(0, 0, 100, 100), right = R(200, 0, 100, 100), diag = R(200, 500, 100, 100);
	CHECK(rei_rect_direction_score(&from, &right, REI_DIR_RIGHT) >= 0);
	CHECK(rei_rect_direction_score(&from, &right, REI_DIR_LEFT) < 0);
	CHECK(rei_rect_direction_score(&from, &diag, REI_DIR_RIGHT) < 0);  /* no overlap: not "right" */
	struct rei_rect near_r = R(150, 0, 100, 100), far_r = R(400, 0, 100, 100);
	CHECK(rei_rect_direction_score(&from, &near_r, REI_DIR_RIGHT) <
	      rei_rect_direction_score(&from, &far_r, REI_DIR_RIGHT));  /* nearer wins */
	rei_tree_destroy(t);
}

/* ---- randomised: invariants hold under any sequence of operations ------- */

static unsigned long long rng_state = 88172645463325252ull;
static unsigned rnd(unsigned n)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return (unsigned)(rng_state % n);
}

static bool rects_overlap(const struct rei_rect *a, const struct rei_rect *b)
{
	return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

static void dump(const struct rei_node *n, int depth)
{
	fprintf(stderr, "%*s%s", depth * 2, "", n->is_split ? (n->layout == REI_LAYOUT_SPLIT_H ? "H" : "V") : "leaf");
	fprintf(stderr, " w=%.2f rect=(%d,%d %dx%d)\n", n->weight, n->rect.x, n->rect.y, n->rect.w, n->rect.h);
	const struct rei_node *c;
	wl_list_for_each(c, &n->children, link)
		dump(c, depth + 1);
}

static void check_geometry(struct rei_tree *t, struct rei_rect area, int gap)
{
	rei_tree_layout(t, area, gap);
	size_t n = rei_tree_leaf_count(t);
	struct rei_node **leaves = calloc(n ? n : 1, sizeof *leaves);
	rei_tree_leaves(t, leaves, n);

	long covered = 0;
	for (size_t i = 0; i < n; i++) {
		const struct rei_rect *r = &leaves[i]->rect;
		CHECK(r->w >= 0 && r->h >= 0);
		CHECK(r->x >= area.x && r->y >= area.y);
		CHECK(r->x + r->w <= area.x + area.w && r->y + r->h <= area.y + area.h);
		covered += (long)r->w * r->h;
		for (size_t j = i + 1; j < n; j++) {
			if (rects_overlap(r, &leaves[j]->rect)) {
				fprintf(stderr, "windows overlap (gap %d, area %dx%d): leaf %zu (%d,%d %dx%d) and leaf %zu (%d,%d %dx%d)\n",
					gap, area.w, area.h, i, r->x, r->y, r->w, r->h, j,
					leaves[j]->rect.x, leaves[j]->rect.y, leaves[j]->rect.w, leaves[j]->rect.h);
				dump(t->root, 0);
				exit(1);
			}
		}
	}
	/* With no gap and any windows at all, the screen is filled completely. */
	if (gap == 0 && n > 0)
		CHECK(covered == (long)area.w * area.h);
	free(leaves);
}

static void test_random(int gap)
{
	struct rei_tree *t = rei_tree_create(REI_LAYOUT_SPLIT_H);
	struct rei_node *leaves[40];
	size_t count = 0;
	int next_id = 0;

	for (int step = 0; step < 6000; step++) {
		unsigned op = rnd(10);
		if ((op < 4 || count == 0) && count < 40) {
			struct rei_node *after = (count && rnd(3)) ? leaves[rnd((unsigned)count)] : NULL;
			leaves[count++] = rei_tree_add_leaf(t, after, (void *)(intptr_t)(next_id++ % 100 + 1));
		} else if (op < 6 && count) {
			unsigned i = rnd((unsigned)count);
			rei_tree_remove_leaf(t, leaves[i]);
			leaves[i] = leaves[--count];
		} else if (op < 7 && count) {
			rei_tree_split(t, leaves[rnd((unsigned)count)],
				       rnd(2) ? REI_LAYOUT_SPLIT_H : REI_LAYOUT_SPLIT_V);
		} else if (op < 8 && count && count < 40) {
			struct rei_node *target = leaves[rnd((unsigned)count)];
			leaves[count] = rei_tree_insert_beside(t, target, (void *)(intptr_t)1,
							       (enum rei_direction)rnd(4));
			count++;
		} else if (count) {
			rei_tree_move(t, leaves[rnd((unsigned)count)], (enum rei_direction)rnd(4));
		}

		if (!rei_tree_check(t)) {
			fprintf(stderr, "invariant broken at step %d (op %u)\n", step, op);
			exit(1);
		}
		CHECK(rei_tree_leaf_count(t) == count);
		if (step % 7 == 0)
			check_geometry(t, R(5, 7, 1000 + rnd(300), 700 + rnd(200)), gap);
	}
	rei_tree_destroy(t);
}

int main(void)
{
	test_single_and_two();
	test_three_and_nested();
	test_weights();
	test_remove_and_collapse();
	test_split_semantics();
	test_move();
	test_separate_gaps();
	test_insert_beside();
	test_neighbor();
	test_random(0);
	test_random(10);
	puts("test_layout: ok");
	return 0;
}
