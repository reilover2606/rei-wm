#ifndef REI_LAYOUT_H
#define REI_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <wayland-util.h>

#include "geom.h"

/*
 * The tiling tree, i3 style: a workspace's tiled windows live in a tree whose
 * inner nodes are splits (children side by side, or stacked) and whose leaves
 * are windows. This module is pure data and geometry: it knows nothing about
 * Wayland, outputs or clients, which keeps it easy to test exhaustively.
 *
 * Invariants it maintains after every operation:
 *   - the root is always a split and is never removed;
 *   - no other split is ever empty (an emptied one is removed, and one left with
 *     a single child by a removal collapses into that child; a single-child
 *     split made by rei_tree_split() is kept until it is disturbed);
 *   - if the root has a single child, that child is a leaf.
 */

enum rei_layout {
	REI_LAYOUT_SPLIT_H,  /* children left to right */
	REI_LAYOUT_SPLIT_V,  /* children top to bottom */
};

enum rei_direction {
	REI_DIR_LEFT,
	REI_DIR_RIGHT,
	REI_DIR_UP,
	REI_DIR_DOWN,
};

struct rei_node {
	struct rei_node *parent;
	struct wl_list link;  /* In parent->children. */
	bool is_split;
	enum rei_layout layout;     /* Splits only. */
	struct wl_list children;    /* Splits only. */
	double weight;              /* Share of the parent's space, relative to siblings. */
	struct rei_rect rect;       /* Filled in by rei_tree_layout(). */
	void *data;                 /* Leaves only: the caller's window. */
};

struct rei_tree {
	struct rei_node *root;
};

struct rei_tree *rei_tree_create(enum rei_layout layout);
void rei_tree_destroy(struct rei_tree *tree);  /* Does not free leaf data. */

/*
 * Adds a window. It goes right after `after` if that is a leaf, at the end of
 * `after` if that is a split, and at the end of the root if `after` is NULL.
 */
struct rei_node *rei_tree_add_leaf(struct rei_tree *tree, struct rei_node *after, void *data);

/* Removes a leaf and tidies up whatever it leaves behind. */
void rei_tree_remove_leaf(struct rei_tree *tree, struct rei_node *leaf);

/*
 * "Split": makes the next window added next to `node` open in a new container
 * with the given layout. If `node` is alone in its parent, the parent simply
 * changes layout; otherwise `node` is wrapped in a new split.
 */
void rei_tree_split(struct rei_tree *tree, struct rei_node *node, enum rei_layout layout);

/*
 * Moves a leaf one step in a direction, i3 style: past a sibling window (swap),
 * into a neighbouring container, or out of its container. Returns false if it
 * is already at the edge and nothing changed.
 */
bool rei_tree_move(struct rei_tree *tree, struct rei_node *leaf, enum rei_direction dir);

/*
 * Assigns every node a rectangle. `area` is the space the whole tree fills and
 * `gap` is the exact distance kept between adjacent windows.
 */
void rei_tree_layout(struct rei_tree *tree, struct rei_rect area, int gap);

/* The same with separate gaps: `gap_x` between windows side by side, `gap_y`
 * between windows stacked. (Stacked windows need room for the focused one's
 * notch, which sits in the gap above it.) */
void rei_tree_layout_xy(struct rei_tree *tree, struct rei_rect area, int gap_x, int gap_y);

/*
 * Adds a window beside `target`, on the side given by `dir` (left of it, right of
 * it, above it or below it), by creating a split around `target` if needed. This
 * is what dropping a dragged window onto another one does.
 */
struct rei_node *rei_tree_insert_beside(struct rei_tree *tree, struct rei_node *target,
					void *data, enum rei_direction dir);

/* The leaf nearest to `from` in a direction (by position on screen), or NULL. */
struct rei_node *rei_tree_neighbor(struct rei_tree *tree, struct rei_node *from,
				   enum rei_direction dir);

size_t rei_tree_leaf_count(const struct rei_tree *tree);

/* Leaves in tree order (left-to-right/top-to-bottom, depth first). Returns the
 * number written, at most `cap`. */
size_t rei_tree_leaves(const struct rei_tree *tree, struct rei_node **out, size_t cap);

/* Checks the invariants above; for tests and assertions. */
bool rei_tree_check(const struct rei_tree *tree);

/* Scores `to` as a place to go from `from` in a direction (lower is better), for
 * navigating across floating windows and outputs as well as tiles. Negative if
 * `to` is not in that direction: it must lie beyond `from`'s centre and overlap
 * it on the other axis. */
double rei_rect_direction_score(const struct rei_rect *from, const struct rei_rect *to,
				enum rei_direction dir);

#endif
