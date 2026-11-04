#ifndef REI_RUNPROMPT_H
#define REI_RUNPROMPT_H

#include <stdbool.h>
#include <stdint.h>

#include "snapshot.h"

struct rei_server;

/*
 * The Run prompt: a large, see-through box in the middle of the screen that takes the keyboard.
 * Type a command (a blinking cursor shows where) and press Enter to run it; Escape or a click
 * closes it. A long command wraps onto new lines and the box grows, staying centred; once it
 * would be taller than 60% of the screen, no more can be typed.
 */
void rei_prompt_open(struct rei_server *server);
void rei_prompt_close(struct rei_server *server);
bool rei_prompt_is_open(const struct rei_server *server);

/* A key event while it is open (evdev code). The prompt consumes every key. */
void rei_prompt_key(struct rei_server *server, uint32_t evdev_code, bool pressed);
/* A pointer press while it is open: closes it. */
void rei_prompt_press(struct rei_server *server);

void rei_prompt_emit(struct rei_server *server, struct rei_scene_snapshot *snap);
void rei_prompt_finish(struct rei_server *server);

#endif
