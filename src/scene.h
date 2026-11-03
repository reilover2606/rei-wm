#ifndef REI_SCENE_H
#define REI_SCENE_H

#include "snapshot.h"

struct rei_server;

/*
 * Walks the live compositor state (event thread only) and produces an
 * immutable snapshot for the render threads, back to front:
 *   wallpaper per output, windows with their popups, the cursor.
 */
struct rei_scene_snapshot *rei_scene_build(struct rei_server *server, uint64_t serial);

#endif
