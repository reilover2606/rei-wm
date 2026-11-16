#ifndef REI_VOLPOP_H
#define REI_VOLPOP_H

#include <stdbool.h>

#include "geom.h"
#include "snapshot.h"
#include "toolbar.h"

struct rei_server;
struct rei_output;

/*
 * The volume popup: click the toolbar's volume widget and a small panel opens beside the bar with a
 * speaker button (click: mute) and a slider (click or drag: set the volume; the mouse wheel works
 * too). It follows the volume live, so the media keys move the slider. A press anywhere else, or
 * Escape, closes it.
 */
void rei_volpop_open(struct rei_server *server, struct rei_output *output, struct rei_rect bar,
		     enum rei_bar_position position, struct rei_rect anchor);
void rei_volpop_close(struct rei_server *server);
bool rei_volpop_is_open(const struct rei_server *server);
bool rei_volpop_contains(const struct rei_server *server, double x, double y);

/* A press at (x, y): true if it landed in the popup (and was acted on). */
bool rei_volpop_press(struct rei_server *server, double x, double y);
void rei_volpop_motion(struct rei_server *server, double x, double y);
void rei_volpop_release(struct rei_server *server);

void rei_volpop_emit(struct rei_server *server, struct rei_scene_snapshot *snap);
void rei_volpop_finish(struct rei_server *server);

#endif
