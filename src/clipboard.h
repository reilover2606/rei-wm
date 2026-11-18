#ifndef REI_CLIPBOARD_H
#define REI_CLIPBOARD_H

struct rei_server;
struct rei_selection_source;

/*
 * Copy and paste between terminals and everything else.
 *
 * Terminals (urxvt, xterm, foot...) copy by highlighting: that sets the primary selection, which a middle click
 * pastes, in the terminals and in programs that use the primary selection. Programs paste with Ctrl+V from the
 * clipboard, which a highlight does not touch. So when the window being typed into is a terminal (the apps of
 * [clipboard] terminals) and it sets the primary selection, the compositor also makes that text the clipboard:
 * it reads the text from the terminal at once and keeps it, so it can be pasted with Ctrl+V into any program, even
 * after the terminal's highlight is gone or the terminal has been closed. Disabled by
 * [clipboard] terminal_selection_to_clipboard = false.
 */
void rei_clipboard_primary_set(struct rei_server *server, struct rei_selection_source *src);
void rei_clipboard_finish(struct rei_server *server);

#endif
