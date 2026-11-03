#include "layout.h"

#include <math.h>
#include <stdlib.h>

#include "util/util.h"

/* ------------------------------------------------------------------------ */
/* Node helpers                                                             */
/* ------------------------------------------------------------------------ */

static struct rei_node *node_new(bool is_split, enum rei_layout layout, void *data)
{
	struct rei_node *n = rei_xcalloc(1, sizeof *n);
	n->is_split = is_split;
	n->layout = layout;
	n->weight = 1.0;
	n->data = data;
	wl_list_init(&n->link);
	wl_list_init(&n->children);
	return n;
}

static size_t child_count(const struct rei_node *n)
{
	return (size_t)wl_list_length(&n->children);
}

static struct rei_node *first_child(const struct rei_node *n)
{
	struct rei_node *c;
	return wl_list_empty(&n->children) ? NULL : wl_container_of(n->children.next, c, link);
}

static struct rei_node *last_child(const struct rei_node *n)
{
	struct rei_node *c;
	return wl_list_empty(&n->children) ? NULL : wl_container_of(n->children.prev, c, link);
}

static struct rei_node *next_sibling(const struct rei_node *n)
{
	if (n->link.next == &n->parent->children)
		return NULL;
	struct rei_node *s;
	return wl_container_of(n->link.next, s, link);
}

static struct rei_node *prev_sibling(const struct rei_node *n)
{
	if (n->link.prev == &n->parent->children)
		return NULL;
	struct rei_node *s;
	return wl_container_of(n->link.prev, s, link);
}

/* The average weight of a split's children, so a newcomer gets a typical share
 * and existing proportions are preserved. */
static double mean_weight(const struct rei_node *split)
{
	size_t n = 0;
	double sum = 0;
	struct rei_node *c;
	wl_list_for_each(c, &split->children, link) {
		sum += c->weight;
		n++;
	}
	return n ? sum / (double)n : 1.0;
}

static void detach(struct rei_node *n)
{
	wl_list_remove(&n->link);
	wl_list_init(&n->link);
	n->parent = NULL;
}

/* Inserts `n` into `parent`'s children right after `ref` (NULL: at the start). */
static void insert_after(struct rei_node *parent, struct rei_node *ref, struct rei_node *n)
{
	struct wl_list *pos = ref ? &ref->link : &parent->children;
	n->weight = mean_weight(parent);
	n->parent = parent;
	wl_list_insert(pos, &n->link);
}

static void insert_before(struct rei_node *parent, struct rei_node *ref, struct rei_node *n)
{
	struct wl_list *pos = ref ? ref->link.prev : parent->children.prev;
	n->weight = mean_weight(parent);
	n->parent = parent;
	wl_list_insert(pos, &n->link);
}

static void node_free_tree(struct rei_node *n)
{
	struct rei_node *c, *tmp;
	wl_list_for_each_safe(c, tmp, &n->children, link)
		node_free_tree(c);
	free(n);
}

/* ------------------------------------------------------------------------ */
/* Tidying                                                                  */
/* ------------------------------------------------------------------------ */

/* Collapses splits that have become pointless, walking up from `split`. */
static void cleanup(struct rei_tree *tree, struct rei_node *split)
{
	while (split && split != tree->root) {
		struct rei_node *parent = split->parent;
		size_t n = child_count(split);

		if (n == 0) {
			detach(split);
			free(split);
			split = parent;
			continue;
		}
		if (n == 1) {
			/* Replace the split with its only child, in the same place. */
			struct rei_node *child = first_child(split);
			detach(child);
			child->weight = split->weight;
			child->parent = parent;
			wl_list_insert(&split->link, &child->link);
			detach(split);
			free(split);
		}
		break;
	}

	/* A root whose only child is a split can adopt that split's contents. */
	struct rei_node *root = tree->root;
	while (child_count(root) == 1 && first_child(root)->is_split) {
		struct rei_node *only = first_child(root);
		root->layout = only->layout;
		detach(only);
		struct rei_node *c, *tmp;
		wl_list_for_each_safe(c, tmp, &only->children, link) {
			wl_list_remove(&c->link);
			wl_list_insert(root->children.prev, &c->link);
			c->parent = root;
		}
		free(only);
	}
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

struct rei_tree *rei_tree_create(enum rei_layout layout)
{
	struct rei_tree *t = rei_xcalloc(1, sizeof *t);
	t->root = node_new(true, layout, NULL);
	return t;
}

void rei_tree_destroy(struct rei_tree *tree)
{
	if (!tree)
		return;
	node_free_tree(tree->root);
	free(tree);
}

struct rei_node *rei_tree_add_leaf(struct rei_tree *tree, struct rei_node *after, void *data)
{
	struct rei_node *leaf = node_new(false, REI_LAYOUT_SPLIT_H, data);
	if (after && !after->is_split)
		insert_after(after->parent, after, leaf);
	else
		insert_after(after ? after : tree->root, last_child(after ? after : tree->root), leaf);
	return leaf;
}

void rei_tree_remove_leaf(struct rei_tree *tree, struct rei_node *leaf)
{
	struct rei_node *parent = leaf->parent;
	detach(leaf);
	free(leaf);
	cleanup(tree, parent);
}

void rei_tree_split(struct rei_tree *tree, struct rei_node *node, enum rei_layout layout)
{
	struct rei_node *parent = node->parent;
	if (!parent) {  /* The root itself. */
		node->layout = layout;
		return;
	}
	if (child_count(parent) == 1) {
		parent->layout = layout;
		return;
	}

	/* Wrap `node` in a fresh split that takes its place. */
	struct rei_node *wrapper = node_new(true, layout, NULL);
	wrapper->weight = node->weight;
	wrapper->parent = parent;
	wl_list_insert(&node->link, &wrapper->link);
	detach(node);
	node->weight = 1.0;
	node->parent = wrapper;
	wl_list_insert(&wrapper->children, &node->link);
	(void)tree;
}

static bool dir_forward(enum rei_direction d) { return d == REI_DIR_RIGHT || d == REI_DIR_DOWN; }

static enum rei_layout dir_axis(enum rei_direction d)
{
	return (d == REI_DIR_LEFT || d == REI_DIR_RIGHT) ? REI_LAYOUT_SPLIT_H : REI_LAYOUT_SPLIT_V;
}

bool rei_tree_move(struct rei_tree *tree, struct rei_node *leaf, enum rei_direction dir)
{
	enum rei_layout axis = dir_axis(dir);
	bool forward = dir_forward(dir);

	/* The nearest ancestor that lays its children out along the direction of
	 * travel, and which of its children we are inside. */
	struct rei_node *child = leaf, *anc = leaf->parent;
	while (anc && anc->layout != axis) {
		child = anc;
		anc = anc->parent;
	}

	struct rei_node *old_parent = leaf->parent;

	if (!anc) {
		/* No container runs this way (e.g. moving right in a single column):
		 * give the tree a new root that does, with everything so far as one
		 * child, and put the window beside it. */
		struct rei_node *root = tree->root;
		struct rei_node *old_root_contents = node_new(true, root->layout, NULL);
		detach(leaf);
		struct rei_node *c, *tmp;
		wl_list_for_each_safe(c, tmp, &root->children, link) {
			wl_list_remove(&c->link);
			wl_list_insert(old_root_contents->children.prev, &c->link);
			c->parent = old_root_contents;
		}
		root->layout = axis;
		old_root_contents->parent = root;
		wl_list_insert(&root->children, &old_root_contents->link);
		if (forward)
			insert_after(root, old_root_contents, leaf);
		else
			insert_before(root, old_root_contents, leaf);
		/* Tidy once, from the one node that can have been left pointless. Calling
		 * cleanup() twice would risk touching a node the first call freed. */
		cleanup(tree, old_parent == root ? old_root_contents : old_parent);
		return true;
	}

	if (child == leaf) {
		/* Moving within the container we are directly in. */
		struct rei_node *sib = forward ? next_sibling(leaf) : prev_sibling(leaf);
		if (sib && !sib->is_split) {
			detach(leaf);  /* Swap places with the neighbouring window. */
			if (forward)
				insert_after(anc, sib, leaf);
			else
				insert_before(anc, sib, leaf);
			return true;
		}
		if (sib) {
			/* Into the neighbouring container, at the edge we come from. */
			detach(leaf);
			if (forward)
				insert_after(sib, NULL, leaf);
			else
				insert_after(sib, last_child(sib), leaf);
			cleanup(tree, old_parent);
			return true;
		}
		if (anc == tree->root)
			return false;  /* Already at the edge of the workspace. */
		/* Out of the container, to just beside it. */
		struct rei_node *grand = anc->parent;
		detach(leaf);
		if (forward)
			insert_after(grand, anc, leaf);
		else
			insert_before(grand, anc, leaf);
		cleanup(tree, old_parent);
		return true;
	}

	/* We are inside a container that sits in `anc`, which runs the right way:
	 * step out of it to its near side. */
	detach(leaf);
	if (forward)
		insert_after(anc, child, leaf);
	else
		insert_before(anc, child, leaf);
	cleanup(tree, old_parent);
	return true;
}

/* ------------------------------------------------------------------------ */
/* Geometry                                                                 */
/* ------------------------------------------------------------------------ */

static void layout_node(struct rei_node *n, struct rei_rect r, int gap)
{
	n->rect = r;
	if (!n->is_split)
		return;

	size_t count = child_count(n);
	if (count == 0)
		return;

	bool horizontal = n->layout == REI_LAYOUT_SPLIT_H;
	int extent = horizontal ? r.w : r.h;

	/* If the gaps alone don't fit, shrink them so every child still lies inside
	 * this container (a degenerate case: more windows than pixels). */
	if (count > 1 && gap * (int)(count - 1) > extent)
		gap = extent / (int)(count - 1);
	int total = extent - gap * (int)(count - 1);
	if (total < 0)
		total = 0;

	double weight_sum = 0;
	struct rei_node *c;
	wl_list_for_each(c, &n->children, link)
		weight_sum += c->weight > 0 ? c->weight : 1.0;

	/* Cumulative rounding: every boundary is rounded once from its exact
	 * position, so sizes differ by at most a pixel and always add up exactly. */
	double acc = 0;
	int prev_edge = 0, i = 0;
	wl_list_for_each(c, &n->children, link) {
		acc += c->weight > 0 ? c->weight : 1.0;
		int edge = (i == (int)count - 1) ? total : (int)lround((double)total * acc / weight_sum);
		int size = edge - prev_edge;
		int start = prev_edge + gap * i;

		struct rei_rect cr = r;
		if (horizontal) {
			cr.x = r.x + start;
			cr.w = size;
		} else {
			cr.y = r.y + start;
			cr.h = size;
		}
		layout_node(c, cr, gap);
		prev_edge = edge;
		i++;
	}
}

void rei_tree_layout(struct rei_tree *tree, struct rei_rect area, int gap)
{
	layout_node(tree->root, area, gap < 0 ? 0 : gap);
}

static size_t collect(const struct rei_node *n, struct rei_node **out, size_t cap, size_t count)
{
	struct rei_node *c;
	wl_list_for_each(c, &n->children, link) {
		if (c->is_split) {
			count = collect(c, out, cap, count);
		} else {
			if (count < cap && out)
				out[count] = c;
			count++;
		}
	}
	return count;
}

size_t rei_tree_leaves(const struct rei_tree *tree, struct rei_node **out, size_t cap)
{
	size_t n = collect(tree->root, out, cap, 0);
	return n < cap ? n : cap;
}

size_t rei_tree_leaf_count(const struct rei_tree *tree)
{
	return collect(tree->root, NULL, 0, 0);
}

double rei_rect_direction_score(const struct rei_rect *from, const struct rei_rect *to,
				enum rei_direction dir)
{
	double fx = from->x + from->w / 2.0, fy = from->y + from->h / 2.0;
	double tx = to->x + to->w / 2.0, ty = to->y + to->h / 2.0;

	/* Distance along the direction of travel, and how far off-axis. */
	double along;
	switch (dir) {
	case REI_DIR_LEFT: along = fx - tx; break;
	case REI_DIR_RIGHT: along = tx - fx; break;
	case REI_DIR_UP: along = fy - ty; break;
	default: along = ty - fy; break;
	}
	if (along <= 0)
		return -1.0;

	bool horizontal = dir == REI_DIR_LEFT || dir == REI_DIR_RIGHT;
	double lo1 = horizontal ? from->y : from->x, hi1 = lo1 + (horizontal ? from->h : from->w);
	double lo2 = horizontal ? to->y : to->x, hi2 = lo2 + (horizontal ? to->h : to->w);
	/* Moving "up" must never land on something off to the side: the two have to
	 * overlap on the other axis. */
	if (fmin(hi1, hi2) - fmax(lo1, lo2) <= 0)
		return -1.0;

	/* Nearest along the direction first; among equals, the one whose centre is
	 * closest to ours across it. */
	double across = horizontal ? fabs(ty - fy) : fabs(tx - fx);
	return along + across * 0.001;
}

struct rei_node *rei_tree_neighbor(struct rei_tree *tree, struct rei_node *from,
				   enum rei_direction dir)
{
	size_t n = rei_tree_leaf_count(tree);
	struct rei_node **leaves = rei_xcalloc(n ? n : 1, sizeof *leaves);
	rei_tree_leaves(tree, leaves, n);

	struct rei_node *best = NULL;
	double best_score = 0;
	for (size_t i = 0; i < n; i++) {
		if (leaves[i] == from)
			continue;
		double s = rei_rect_direction_score(&from->rect, &leaves[i]->rect, dir);
		if (s >= 0 && (!best || s < best_score)) {
			best = leaves[i];
			best_score = s;
		}
	}
	free(leaves);
	return best;
}

/* ------------------------------------------------------------------------ */
/* Checking                                                                 */
/* ------------------------------------------------------------------------ */

static bool check_node(const struct rei_tree *tree, const struct rei_node *n)
{
	if (!(n->weight > 0))
		return false;
	if (!n->is_split)
		return wl_list_empty(&n->children);

	size_t count = child_count(n);
	/* A container is never empty. One with a single child is legitimate: that is
	 * what "split" creates, and the next window opens inside it. */
	if (n != tree->root && count < 1)
		return false;
	const struct rei_node *c;
	wl_list_for_each(c, &n->children, link) {
		if (c->parent != n || !check_node(tree, c))
			return false;
	}
	return true;
}

bool rei_tree_check(const struct rei_tree *tree)
{
	if (!tree->root || !tree->root->is_split || tree->root->parent)
		return false;
	if (child_count(tree->root) == 1 && first_child(tree->root)->is_split)
		return false;
	return check_node(tree, tree->root);
}
