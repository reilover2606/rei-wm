#ifndef REI_CONFIRM_H
#define REI_CONFIRM_H

#include <stdbool.h>
#include <stdint.h>

#include "snapshot.h"

struct rei_server;

typedef void (*rei_confirm_fn)(struct rei_server *server, void *data);

/*
 * A modal confirmation dialog in the middle of the screen: a title, a message, Cancel and a
 * confirm button. Enter presses the highlighted button (Cancel at first, so a stray Enter is
 * safe), Tab / arrows move the highlight, Escape cancels; the buttons can be clicked too.
 * `on_confirm` runs only when confirmed. It takes every key and pointer press while open.
 */
void rei_confirm_open(struct rei_server *server, const char *title, const char *message,
		      const char *confirm_label, rei_confirm_fn on_confirm, void *data);
void rei_confirm_close(struct rei_server *server);
bool rei_confirm_is_open(const struct rei_server *server);

void rei_confirm_key(struct rei_server *server, uint32_t evdev_code, bool pressed);
void rei_confirm_press(struct rei_server *server, double x, double y);
void rei_confirm_motion(struct rei_server *server, double x, double y);

void rei_confirm_emit(struct rei_server *server, struct rei_scene_snapshot *snap);
void rei_confirm_finish(struct rei_server *server);

#endif
