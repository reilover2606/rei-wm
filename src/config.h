#ifndef REI_CONFIG_H
#define REI_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "theme.h"
#include "toolbar.h"
#include "util/toml.h"

struct rei_output_config {
	char *name;
	int32_t width, height;
	int32_t refresh_mhz;  /* Millihertz: 60000 == 60 Hz. */
	bool has_mode;        /* width/height/refresh/mode was given (else: the display's own choice). */
	bool enabled;         /* false: leave this monitor off (DRM only). */
	double scale;
	bool has_position;
	int32_t x, y;
};

struct rei_shortcut_config {
	char *name;     /* What the menu shows. */
	char *command;  /* Run with /bin/sh -c. */
};

struct rei_binding_config {
	char *combo;   /* "Mod+Shift+q" */
	char *action;  /* "close", "exec foot", ... */
};

struct rei_config {
	char *path;  /* File the config was loaded from (NULL for defaults). */

	/* [general] */
	char *backend;          /* "auto", "headless", "nested" or "drm". */
	char *composition_key;  /* "super", "alt", "ctrl" or "shift". */

	bool xwayland;             /* Run Xwayland so X11 programs work. */
	bool focus_follows_mouse;  /* Moving the pointer onto a window focuses it. */

	/* [layout] */
	bool float_new_windows;    /* new_windows = "float": don't tile new windows. */
	int32_t gap_outer;  /* Between windows and the edge of the desktop. */
	int32_t gap_inner;  /* Between adjacent windows. */

	/* [input] — empty strings mean "library default". */
	char *kb_rules, *kb_model, *kb_layout, *kb_variant, *kb_options;
	int32_t repeat_rate;   /* Key repeats per second; 0 disables repeat. */
	int32_t repeat_delay;  /* Milliseconds before repeating starts. */
	/* Pointing devices (libinput, i.e. the DRM backend). */
	bool tap_to_click;
	bool natural_scroll;
	bool left_handed;
	double pointer_accel;  /* -1.0 (slowest) to 1.0 (fastest); 0 is the default curve. */

	/* [keybindings]: overrides on top of the built-in defaults. */
	struct rei_binding_config *bindings;
	size_t n_bindings;

	/* [audio] */
	bool audio_enabled;
	int32_t volume_step;  /* Percent per key press or wheel notch. */
	int32_t max_volume;   /* Percent. */

	/* [toolbar] */
	bool toolbar_enabled;
	enum rei_bar_position toolbar_position;
	int32_t toolbar_thickness;   /* Pixels. */
	enum rei_tray_mode toolbar_tray;  /* The application tray in the middle of the bar. */
	bool toolbar_sysinfo;             /* Show CPU and RAM use, left of the clock. */
	bool toolbar_battery;             /* Show the battery, if the machine has one. */
	enum rei_tray_scope toolbar_tray_scope;
	int32_t toolbar_tray_min_width;   /* Percent of its natural width a tile may shrink to. */
	char *flavor_icons;          /* [theme] flavor_icons: a directory of icons for the iconable elements; "" = off. */
	char *toolbar_font;          /* Pango font description. */
	char *toolbar_clock_format;  /* strftime; empty means no clock. */
	uint32_t toolbar_background, toolbar_foreground, toolbar_highlight;  /* 0xRRGGBB */

	/* [start_menu] and [[shortcut]] */
	bool start_enabled;
	char *start_label;              /* The button's text. */
	char *start_shutdown_command;   /* Run for "Shutdown". */
	char *start_reboot_command;
	char *start_logout_command;     /* Empty: "Logout" simply ends rei-wm. */
	bool start_confirm_power;       /* Ask for a second click before logging out, shutting down or rebooting. */
	char *start_run_font;           /* Pango font of the Run prompt. */
	struct rei_shortcut_config *shortcuts;
	size_t n_shortcuts;

	/* [theme]: the look of the UI, from the base colours above plus the [theme.*] overrides. */
	struct rei_theme theme;

	/* [decoration] */
	int32_t notch_height;  /* Pixels; 0 turns the window notches off. */

	/* [wallpaper] */
	char *wallpaper_path;     /* NULL: no image, just the background colour. */
	char *wallpaper_mode;     /* "fill", "fit", "stretch", "center" or "tile". */
	uint32_t background_rgb;  /* 0xRRGGBB; behind the wallpaper and windows. */

	/* [[output]] */
	struct rei_output_config *outputs;
	size_t n_outputs;

	/* The full parsed document, kept so subsystems added later (keybindings,
	 * toolbar, ...) can read their own sections without touching this file. */
	struct toml_table *root;
};

/* ~/.config/rei-wm/config.toml, honouring XDG_CONFIG_HOME. Caller frees. */
char *rei_config_default_path(void);

/* The text written to a new config file. */
const char *rei_config_default_text(void);

/* Built-in defaults, as if from an empty file. Never fails. */
struct rei_config *rei_config_defaults(void);

/* Parses and validates TOML text. On failure returns NULL and fills err. */
struct rei_config *rei_config_parse(const char *text, char *err, size_t errlen);

/*
 * Loads `path`. If the file does not exist and `create_if_missing` is set, the
 * default config is written there first (failure to write is only a warning).
 * On failure returns NULL and fills err.
 */
struct rei_config *rei_config_load(const char *path, bool create_if_missing,
				   char *err, size_t errlen);

void rei_config_free(struct rei_config *cfg);

#endif
