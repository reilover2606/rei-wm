#ifndef REI_WALLPAPERS_H
#define REI_WALLPAPERS_H

#include <stdbool.h>
#include <stddef.h>

#include "pixbuf.h"

struct rei_server;
struct rei_output;
struct rei_config;
struct rei_wallpaper_group;

/*
 * Wallpaper groups. A group is one picture or many: a list of image files and directories (a directory stands
 * for the images in it, in name order, looked at again each time the group moves on). A group with several
 * pictures shows them one after another, changing every `interval` seconds (10 minutes by default), the same
 * one on every output and workspace that shows the group.
 *
 * Which group a workspace shows: the one that lists that workspace in `workspaces`; any other workspace
 * shows the "any" group (`any = true`; there can be only one); with none of that, the plain background colour.
 * A [wallpaper] path given without any groups is the same as one single-picture "any" group.
 *
 * What is drawn is rendered at the output's size and kept in a cache keyed by picture, size and fit mode, so
 * going back to a workspace costs nothing. Everything runs on the event thread.
 */

/* (Re)builds the groups from the config in effect and starts the timer that moves them on. */
void rei_wallpapers_configure(struct rei_server *server);
void rei_wallpapers_stop(struct rei_server *server);

/* The picture to draw behind `output`'s current workspace, at the output's size; NULL for the plain colour.
 * Owned by the cache: valid until the next call. */
struct rei_pixbuf *rei_wallpapers_pixbuf(struct rei_server *server, struct rei_output *output);

/* The group that shows on `workspace`, or NULL (a [wallpaper] path counts as an any group). Exposed for tests. */
const struct rei_wallpaper_group *rei_wallpapers_group_for(const struct rei_config *config, int workspace);

/* The pictures a path stands for: a file is itself; a directory is its image files (png, jpg, jpeg, bmp, tga,
 * gif), by name, case-insensitively, not looking into subdirectories. Returns how many; each is malloc'd, the
 * array is too (free both). Exposed for tests. */
size_t rei_wallpaper_list(const char *path, char ***out);

#endif
