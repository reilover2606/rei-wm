#ifndef REI_CURSOR_CLI_H
#define REI_CURSOR_CLI_H

/*
 * Helpers for the settings app (and for anyone curious):
 *
 *   rei-wm --list-cursor-themes
 *       prints the installed cursor themes, one per line (those with a cursors/ directory in the usual icon
 *       directories, /usr/share/cursors and $XCURSOR_PATH).
 *
 *   rei-wm --cursor-preview OUT.png --cursor-theme NAME [--cursor-size N]
 *       draws the theme's pointers for a dozen shapes, each on a light and a dark ground, and writes a PNG.
 *       An empty NAME is the default theme.
 */
void rei_cursor_cli_list(void);
int rei_cursor_cli_preview(const char *theme, int size, const char *png_path);

#endif
