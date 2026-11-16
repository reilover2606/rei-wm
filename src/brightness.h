#ifndef REI_BRIGHTNESS_H
#define REI_BRIGHTNESS_H

#include <stdbool.h>

struct rei_server;

/*
 * Screen brightness: the brightness keys (brightness_up / brightness_down, bound to XF86MonBrightnessUp and
 * XF86MonBrightnessDown by default: the codes laptops send for their brightness keys, whichever F-keys those are).
 *
 * The backlight is a directory under /sys/class/backlight (REI_BACKLIGHT_DIR for tests): `brightness` is
 * written, between 1% of `max_brightness` (the screen never goes black) and the maximum. A normal user cannot
 * write it unless a udev rule allows it, so when writing fails the change is asked of logind, which lets the
 * session on the active seat do exactly this (dbus-send, run in the background: nothing here waits on it).
 * [brightness] command replaces both with a command of your own ("{value}", "{percent}" and "{device}" are filled in).
 * The result is shown as a message ("Brightness: 60%") that a following key press updates in place.
 */
struct rei_backlight {
	char name[256];
	char type[16];   /* raw, platform or firmware */
	long cur, max;
};

/* The backlight to use under `dir`: the one called `want` if that is set, else the finest (raw before platform
 * before firmware, then the larger range). False if there is none. Exposed for tests. */
bool rei_backlight_find(const char *dir, const char *want, struct rei_backlight *out);

/* Brightens (percent > 0) or dims by `percent` points of the range; 0 means [brightness] step. */
void rei_brightness_change(struct rei_server *server, int percent);

#endif
