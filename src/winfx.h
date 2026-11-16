#ifndef REI_WINFX_H
#define REI_WINFX_H

#include "fx.h"

struct rei_server;
struct rei_view;

/*
 * Window shaders: which windows are drawn through a fragment shader, and with what parameters.
 *
 * Two sources, tried in this order:
 *   1. the command line, as with picom: --window-shader-fg SHADER (every window) and
 *      --window-shader-fg-rule SHADER:APP (the windows of one application), where SHADER is "glow" or the
 *      path of a GLSL file and APP an app id / X11 class (case-insensitive);
 *   2. the theme's [theme.window_text] element (apps, shader, the glow and blink settings).
 */
#define REI_MAX_WINDOW_SHADERS 16

/* (Re)reads the rules: at start-up and after a config reload. A shader that cannot be read is skipped
 * with a warning. */
void rei_winfx_configure(struct rei_server *server);
void rei_winfx_finish(struct rei_server *server);
/* The effect for a window, or NULL for none. Valid until the next configure. */
const struct rei_fx *rei_winfx_for(struct rei_server *server, const struct rei_view *view);

#endif
