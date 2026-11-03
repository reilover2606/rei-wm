#ifndef REI_TRAY_H
#define REI_TRAY_H

#include <stdbool.h>
#include <stddef.h>

#include "geom.h"
#include "snapshot.h"
#include "toolbar.h"

struct rei_server;
struct rei_output;
struct rei_view;
struct rei_workspace;

/*
 * The application tray: the toolbar's middle section, listing windows grouped by
 * application. Nothing is saved or pinned; it is only what is running. This file is
 * the data and interaction side (grouping, order, the window list, hover, tooltips,
 * dragging); the drawing is in toolbar_render.c.
 */

#define REI_TRAY_MAX_VIEWS 32
/* Windows that may sit under the overflow tile before new ones are sent to the next workspace. */
#define REI_TRAY_OVERFLOW_CAP 32

struct rei_tray_group {
	char app_id[96];
	char label[48];    /* The window's title if it is alone; else the application's name and a count. */
	int count;
	bool active;       /* One of them has the focus. */
	bool minimized;    /* Every one of them is hidden. */
	struct rei_view *views[REI_TRAY_MAX_VIEWS];
	int n_views;
};

struct rei_tray_set {
	struct rei_tray_group groups[REI_BAR_MAX_TRAY];
	size_t n_groups;
	/* Windows of applications past the list's capacity: always under the overflow tile. */
	int extra_windows;
	struct rei_view *extra_views[REI_TRAY_MAX_VIEWS];
	int n_extra_views;
};

/*
 * The groups for `output`, in the user's order (first appearance for new applications).
 * `only` limits it to one workspace; NULL uses the configured scope.
 */
void rei_tray_collect(struct rei_server *server, const struct rei_output *output,
		      const struct rei_workspace *only, struct rei_tray_set *out);

/* The window title and the application id of a window (never NULL, maybe empty). */
const char *rei_view_title(const struct rei_view *view);
const char *rei_view_app_id(const struct rei_view *view);

/* ---- The window list: opened from a stack, or from the overflow tile ---- */

void rei_tray_menu_open(struct rei_server *server, struct rei_output *output, const char *key,
			struct rei_view *const *views, int n_views, struct rei_rect bar,
			enum rei_bar_position position, struct rei_rect anchor);
void rei_tray_menu_close(struct rei_server *server);
bool rei_tray_menu_is_open(const struct rei_server *server);
bool rei_tray_menu_is_for(const struct rei_server *server, const char *key);
/* A pointer press while the list is open. Always consumed. */
void rei_tray_menu_press(struct rei_server *server, double x, double y);
void rei_tray_menu_view_gone(struct rei_server *server, const struct rei_view *view);

/* ---- Hover, tooltip and dragging (event thread) ---- */

#define REI_TRAY_OVERFLOW_KEY "\x01overflow"

/* The pointer moved: updates the hover highlight, the tooltip's timer and any drag. */
void rei_tray_pointer_moved(struct rei_server *server, double x, double y);
/* The pointer left the toolbar, or something else took over. */
void rei_tray_hover_clear(struct rei_server *server);

/* What the bar state needs to know. */
int rei_tray_hover_index(const struct rei_server *server, const struct rei_output *output,
			 const struct rei_tray_set *set);
/* The key (app id, or the overflow key) of tile `index` of an output's tray, as rei_toolbar_tray_at() numbers them. */
bool rei_tray_key_for(struct rei_server *server, struct rei_output *output, int index, char *key, size_t n);
/* Drag bookkeeping: pressing a tile may turn into a drag once the pointer has moved. */
void rei_tray_press(struct rei_server *server, struct rei_output *output, const char *key,
		    double x, double y);
bool rei_tray_dragging(const struct rei_server *server);
/* The dragged tile's group index in `set` (or -1) and the pointer's place along the bar. */
int rei_tray_drag_index(const struct rei_server *server, const struct rei_output *output,
			const struct rei_tray_set *set, int *pos);
/* Ends a press: if it was a drag, reorders and returns true (so no click follows). */
bool rei_tray_release(struct rei_server *server);

void rei_tray_emit(struct rei_server *server, struct rei_scene_snapshot *snap);  /* list and tooltip */
void rei_tray_finish(struct rei_server *server);

#endif
