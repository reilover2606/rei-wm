#ifndef REI_XWAYLAND_SEL_H
#define REI_XWAYLAND_SEL_H

#include <stdbool.h>
#include <xcb/xcb.h>

#include "proto/selection.h"

struct rei_server;
struct rei_xsel;

/*
 * The X11 side of copy and paste. Xwayland does not connect X11 selections to the Wayland ones; the window manager has
 * to, so X11 programs (urxvt, xterm, GTK2...) could neither copy to nor paste from anything else. This bridges the
 * two selections, CLIPBOARD and PRIMARY, in both directions, for text and any type named like a MIME type:
 *
 *   - an X11 program takes a selection: its types are offered to Wayland as a source that fetches the data from
 *     it (through the X11 selection protocol) when a Wayland program pastes;
 *   - a Wayland program (or the compositor, mirroring a terminal's text) takes it: rei-wm owns the X11 selection
 *     and answers X11 programs' requests from the Wayland source.
 *
 * Transfers go through INCR-less properties (a selection larger than the X server's request size, many megabytes,
 * is not transferred).
 */
struct rei_xsel *rei_xsel_create(struct rei_server *server, xcb_connection_t *conn, xcb_window_t wm_window,
				 xcb_window_t root);
void rei_xsel_destroy(struct rei_xsel *sel);
/* Handles the events of the selection protocol; false if `ev` is none of them. */
bool rei_xsel_handle_event(struct rei_xsel *sel, xcb_generic_event_t *ev);

#endif
