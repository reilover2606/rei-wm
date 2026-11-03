#ifndef REI_CONFIG_H
#define REI_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util/toml.h"

struct rei_output_config {
	char *name;
	int32_t width, height;
	int32_t refresh_mhz;  /* Millihertz: 60000 == 60 Hz. */
	double scale;
	bool has_position;
	int32_t x, y;
};

struct rei_config {
	char *path;  /* File the config was loaded from (NULL for defaults). */

	/* [general] */
	char *backend;          /* "auto", "headless" or "nested". */
	char *composition_key;  /* "super", "alt", "ctrl" or "shift". */

	/* [layout] */
	int32_t gap_outer;  /* Between windows and the edge of the desktop. */
	int32_t gap_inner;  /* Between adjacent windows. */

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
