#ifndef REI_POPUP_H
#define REI_POPUP_H

#include <stdbool.h>

#include "geom.h"
#include "snapshot.h"
#include "toolbar.h"

struct rei_server;
struct rei_output;

/*
 * A pop-up list that opens beside the toolbar: the tray's window list and the Start menu are
 * both one of these. Only one is open at a time (opening another closes the first). It takes
 * the next pointer press: a press on a row calls `pick`, a press anywhere else just closes it.
 */

/* Called when a row is picked. Return true to close the popup, false to keep it open (e.g. to
 * ask for confirmation by changing a row's label with rei_popup_state()/rei_popup_redraw()). */
typedef bool (*rei_popup_pick_fn)(struct rei_server *server, int row, void *data);

void rei_popup_open(struct rei_server *server, struct rei_output *output, const char *key,
		    const struct rei_menu_state *state, rei_popup_pick_fn pick, void *data,
		    struct rei_rect bar, enum rei_bar_position position, struct rei_rect anchor);
void rei_popup_close(struct rei_server *server);
bool rei_popup_is_open(const struct rei_server *server);
/* Whether the open popup was opened with this key (to toggle it with a second click). */
bool rei_popup_is_for(const struct rei_server *server, const char *key);
void rei_popup_press(struct rei_server *server, double x, double y);
void rei_popup_motion(struct rei_server *server, double x, double y);
void rei_popup_emit(struct rei_server *server, struct rei_scene_snapshot *snap);

/* The open popup's rows, to change; call rei_popup_redraw() afterwards. NULL if none is open. */
struct rei_menu_state *rei_popup_state(struct rei_server *server);
void rei_popup_redraw(struct rei_server *server);

void rei_popup_finish(struct rei_server *server);

#endif
