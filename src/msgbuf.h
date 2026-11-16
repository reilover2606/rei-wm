#ifndef REI_MSGBUF_H
#define REI_MSGBUF_H

#include <stdbool.h>

#include "snapshot.h"

struct rei_server;

/*
 * The message column: an invisible strip of text along the right (or left) edge of the screen.
 *
 * rei_msg_push shows a message for [messages] duration seconds, fading near the end. Each newer
 * message pushes the older ones up and dims them a step (the oldest on screen is nearly invisible).
 * The "messages" action (Mod+n) opens the stack view: every message in the history at full strength,
 * newest at the bottom, with a translucent backdrop; the mouse wheel scrolls it and Escape closes it.
 */
void rei_msg_push(struct rei_server *server, const char *text);
/* Like rei_msg_push, but if the newest message was pushed with the same `key` and is still on screen it is replaced
 * (and its time starts again): a held key's "Brightness: 60%" is one note that changes, not a message per press. */
void rei_msg_push_keyed(struct rei_server *server, const char *key, const char *text);

/* Runs `command` like rei_spawn, but its output (stdout and stderr) becomes messages. No output, no
 * message. Used by the Run prompt. */
bool rei_msg_run_captured(struct rei_server *server, const char *command);

void rei_msg_toggle_stack(struct rei_server *server);
bool rei_msg_stack_open(const struct rei_server *server);
void rei_msg_stack_close(struct rei_server *server);
/* Whether (x, y) is on the open stack view. */
bool rei_msg_stack_at(const struct rei_server *server, double x, double y);
/* The pointer over the open stack view: which music button it is on (hover highlight), and a press (a button,
 * or a click on the blurred background, which closes it). */
void rei_msg_stack_motion(struct rei_server *server, double x, double y);
int rei_msg_stack_hover(const struct rei_server *server);   /* the button index, or -1 */
void rei_msg_stack_press(struct rei_server *server, double x, double y);
/* The wheel over the stack view: positive scrolls toward newer messages, negative toward older ones. */
void rei_msg_stack_scroll(struct rei_server *server, double value);

void rei_msg_emit(struct rei_server *server, struct rei_scene_snapshot *snap);
void rei_msg_finish(struct rei_server *server);

/* The text as it will be shown: control sequences removed, valid UTF-8, trimmed and capped.
 * Returns a new string, or NULL if nothing is left. Exposed for tests. */
char *rei_msg_sanitize(const char *raw, unsigned long len);

#endif
