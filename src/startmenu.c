#include "startmenu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input/actions.h"
#include "popup.h"
#include "runprompt.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"

enum row_kind { ROW_SHORTCUT, ROW_RUN, ROW_LOGOUT, ROW_SHUTDOWN, ROW_REBOOT };

struct rei_startmenu {
	enum row_kind kind[REI_MENU_MAX_ROWS];
	int shortcut[REI_MENU_MAX_ROWS];   /* For shortcut rows: index into the config. */
	int n;
	int confirm_row;                   /* The power row waiting for its second click, or -1. */
	char plain_label[REI_MENU_MAX_ROWS][96];
};

static const char *power_label(enum row_kind k)
{
	return k == ROW_LOGOUT ? "Logout" : k == ROW_SHUTDOWN ? "Shutdown" : "Reboot";
}

static const char *power_confirm_label(enum row_kind k)
{
	return k == ROW_LOGOUT ? "Click again to log out" : k == ROW_SHUTDOWN ? "Click again to shut down"
									       : "Click again to reboot";
}

static void run_power(struct rei_server *s, enum row_kind k)
{
	const struct rei_config *cfg = s->config;
	if (k == ROW_LOGOUT) {
		if (cfg->start_logout_command[0]) {
			rei_info("start menu: logout: %s", cfg->start_logout_command);
			rei_spawn(s, cfg->start_logout_command);
		} else {
			rei_info("start menu: logout: ending rei-wm");
			rei_server_stop(s);
		}
		return;
	}
	const char *cmd = k == ROW_SHUTDOWN ? cfg->start_shutdown_command : cfg->start_reboot_command;
	rei_info("start menu: %s: %s", k == ROW_SHUTDOWN ? "shutdown" : "reboot", cmd);
	rei_spawn(s, cmd);
}

static bool pick(struct rei_server *s, int row, void *data)
{
	struct rei_startmenu *m = data;
	if (row < 0 || row >= m->n)
		return true;
	enum row_kind k = m->kind[row];

	if (k == ROW_SHORTCUT) {
		const struct rei_shortcut_config *sc = &s->config->shortcuts[m->shortcut[row]];
		rei_info("start menu: %s: %s", sc->name, sc->command);
		rei_spawn(s, sc->command);
		return true;
	}
	if (k == ROW_RUN) {
		rei_popup_close(s);       /* Close first: the prompt takes over the keyboard. */
		rei_prompt_open(s);
		return false;
	}

	/* Logout, Shutdown, Reboot. */
	if (s->config->start_confirm_power && m->confirm_row != row) {
		struct rei_menu_state *st = rei_popup_state(s);
		if (!st)
			return true;
		if (m->confirm_row >= 0) {  /* Another row was waiting: put its label back. */
			snprintf(st->rows[m->confirm_row].label, sizeof st->rows[m->confirm_row].label, "%s",
				 power_label(m->kind[m->confirm_row]));
			st->rows[m->confirm_row].emphasis = false;
		}
		m->confirm_row = row;
		snprintf(st->rows[row].label, sizeof st->rows[row].label, "%s", power_confirm_label(k));
		st->rows[row].emphasis = true;
		rei_popup_redraw(s);
		return false;     /* Stay open for the second click. */
	}
	rei_popup_close(s);
	run_power(s, k);
	return false;
}

void rei_startmenu_open(struct rei_server *s, struct rei_output *o, struct rei_rect bar,
			enum rei_bar_position position, struct rei_rect anchor)
{
	const struct rei_config *cfg = s->config;
	if (!s->startmenu)
		s->startmenu = rei_xcalloc(1, sizeof *s->startmenu);
	struct rei_startmenu *m = s->startmenu;
	memset(m, 0, sizeof *m);
	m->confirm_row = -1;

	struct rei_menu_state st;
	memset(&st, 0, sizeof st);
	/* Top to bottom: shortcuts, then Run, then the power unit. */
	for (size_t i = 0; i < cfg->n_shortcuts && m->n < REI_MENU_MAX_ROWS - 4; i++) {
		m->kind[m->n] = ROW_SHORTCUT;
		m->shortcut[m->n] = (int)i;
		snprintf(st.rows[m->n].label, sizeof st.rows[m->n].label, "%.90s", cfg->shortcuts[i].name);
		m->n++;
	}
	m->kind[m->n] = ROW_RUN;
	snprintf(st.rows[m->n].label, sizeof st.rows[m->n].label, "Run");
	st.rows[m->n].gap_before = cfg->n_shortcuts ? 9 : 0;
	m->n++;
	static const enum row_kind power[] = { ROW_LOGOUT, ROW_SHUTDOWN, ROW_REBOOT };
	for (int i = 0; i < 3; i++) {
		m->kind[m->n] = power[i];
		snprintf(st.rows[m->n].label, sizeof st.rows[m->n].label, "%s", power_label(power[i]));
		st.rows[m->n].gap_before = i == 0 ? 9 : 0;
		m->n++;
	}
	st.n_rows = m->n;
	st.hover = -1;
	snprintf(st.font, sizeof st.font, "%s", cfg->toolbar_font);
	st.menu = cfg->theme.e[REI_ELEM_MENU];
	st.row = cfg->theme.e[REI_ELEM_MENU_ROW];
	rei_popup_open(s, o, "start", &st, pick, m, bar, position, anchor);
}

void rei_startmenu_finish(struct rei_server *s)
{
	free(s->startmenu);
	s->startmenu = NULL;
}
