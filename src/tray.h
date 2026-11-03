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

/*
 * The application tray: the toolbar's middle section, listing the windows of the
 * workspaces that belong to an output, grouped by application. Nothing is saved or
 * pinned; it is only what is running.
 */

#define REI_TRAY_MAX_GROUPS 24
#define REI_TRAY_MAX_VIEWS 32

struct rei_tray_group {
	char app_id[96];
	char label[48];    /* The window's title if it is alone; else the application's name and a count. */
	int count;         /* Windows (more than are listed in `views` are still counted). */
	bool active;       /* One of them has the focus. */
	bool minimized;    /* Every one of them is hidden. */
	struct rei_view *views[REI_TRAY_MAX_VIEWS];
	int n_views;
};

/* The groups for `output`, in the order their first window appeared. Returns the count. */
size_t rei_tray_collect(struct rei_server *server, const struct rei_output *output,
			struct rei_tray_group *out, size_t cap);

/* The window title and the application id of a window (never NULL, maybe empty). */
const char *rei_view_title(const struct rei_view *view);
const char *rei_view_app_id(const struct rei_view *view);

/* ---- The window list that opens from a tile of several windows ---- */

/* Opens the list for `group`, below/beside the bar's tile `anchor` (global coordinates). */
void rei_tray_menu_open(struct rei_server *server, struct rei_output *output,
			const struct rei_tray_group *group, struct rei_rect bar,
			enum rei_bar_position position, struct rei_rect anchor);
void rei_tray_menu_close(struct rei_server *server);
bool rei_tray_menu_is_open(const struct rei_server *server);
/* Whether the open list belongs to this application (to toggle it with a second click). */
bool rei_tray_menu_is_for(const struct rei_server *server, const char *app_id);
/* A pointer press while the list is open. Always consumed: a press on a row picks that
 * window, anywhere else just closes the list. */
void rei_tray_menu_press(struct rei_server *server, double x, double y);
void rei_tray_menu_motion(struct rei_server *server, double x, double y);
void rei_tray_menu_emit(struct rei_server *server, struct rei_scene_snapshot *snap);
/* A window is going away: closes the list if it shows that window. */
void rei_tray_menu_view_gone(struct rei_server *server, const struct rei_view *view);

void rei_tray_finish(struct rei_server *server);

#endif
