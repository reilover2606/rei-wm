#ifndef REI_THEME_CLI_H
#define REI_THEME_CLI_H

struct rei_config;

/*
 * Two helpers for the settings app's theme editor (and for anyone curious):
 *
 *   rei-wm --print-theme [--config FILE]
 *       prints the effective theme, one "element.property=value" line per value: the built-in look
 *       (derived from the toolbar colours) with the file's [theme.*] tables applied. Colours are "#rrggbb".
 *
 *   rei-wm --theme-preview OUT.png [--config FILE]
 *       draws one of each themed element (toolbar with its start button, tray, readout, battery and
 *       volume; the Start menu with a hovered row; a tooltip; a dialog; the volume popup; the Run prompt;
 *       the message column and its stack backdrop) with the real renderer and writes a PNG. No display is needed.
 */
void rei_theme_print(const struct rei_config *config);
/* The built-in look for the config's toolbar colours, ignoring its [theme.*] tables. */
void rei_theme_print_defaults(const struct rei_config *config);
int rei_theme_preview(const struct rei_config *config, const char *png_path);

#endif
