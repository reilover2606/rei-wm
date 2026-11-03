#include "input/actions.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "desktop.h"
#include "proto/xdg_shell.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"

extern char **environ;

/* A copy of the environment with WAYLAND_DISPLAY set to our socket. Built before
 * forking because the child of a multithreaded process may only call
 * async-signal-safe functions, and setenv() is not one of them. */
static char **build_environment(const char *socket_name)
{
	size_t n = 0;
	while (environ[n])
		n++;

	char **env = rei_xcalloc(n + 2, sizeof *env);
	size_t out = 0;
	for (size_t i = 0; i < n; i++) {
		if (strncmp(environ[i], "WAYLAND_DISPLAY=", 16) != 0)
			env[out++] = rei_xstrdup(environ[i]);
	}
	char *wd = NULL;
	if (asprintf(&wd, "WAYLAND_DISPLAY=%s", socket_name) < 0)
		abort();
	env[out++] = wd;
	return env;
}

static void free_environment(char **env)
{
	for (char **e = env; *e; e++)
		free(*e);
	free(env);
}

bool rei_spawn(struct rei_server *s, const char *command)
{
	char **env = build_environment(s->socket_name);
	bool ok = false;

	pid_t pid = fork();
	if (pid < 0) {
		rei_error("cannot start '%s': fork: %s", command, strerror(errno));
	} else if (pid == 0) {
		/* Double fork: the grandchild is adopted by init, so there is never a
		 * zombie to reap and no SIGCHLD handling is needed here. */
		setsid();
		pid_t inner = fork();
		if (inner == 0) {
			/* Dispositions we changed must not leak into the program. */
			signal(SIGPIPE, SIG_DFL);
			sigset_t none;
			sigemptyset(&none);
			sigprocmask(SIG_SETMASK, &none, NULL);
			char *const argv[] = { "sh", "-c", (char *)command, NULL };
			execve("/bin/sh", argv, env);
			_exit(127);
		}
		_exit(inner < 0 ? 1 : 0);
	} else {
		int status = 0;
		waitpid(pid, &status, 0);
		ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
		if (!ok)
			rei_error("cannot start '%s'", command);
	}
	free_environment(env);
	return ok;
}

void rei_actions_run(struct rei_server *s, const struct rei_action *action)
{
	switch (action->type) {
	case REI_ACTION_NONE:
		break;
	case REI_ACTION_EXEC:
		rei_info("exec: %s", action->arg);
		rei_spawn(s, action->arg);
		break;
	case REI_ACTION_CLOSE: {
		struct rei_view *v = rei_wm_active_view(s);
		if (v)
			rei_xdg_toplevel_close(v->toplevel);
		break;
	}
	case REI_ACTION_QUIT:
		rei_info("quit requested by keybinding");
		rei_server_stop(s);
		break;
	case REI_ACTION_FOCUS_NEXT:
		rei_wm_cycle(s, true);
		break;
	case REI_ACTION_FOCUS_PREV:
		rei_wm_cycle(s, false);
		break;
	case REI_ACTION_FOCUS_DIR:
		rei_wm_focus_direction(s, (enum rei_direction)action->value);
		break;
	case REI_ACTION_MOVE_DIR:
		rei_wm_move_direction(s, (enum rei_direction)action->value);
		break;
	case REI_ACTION_SPLIT:
		rei_wm_split(s, (enum rei_layout)action->value);
		break;
	case REI_ACTION_TOGGLE_FLOATING:
		rei_wm_toggle_floating(s);
		break;
	case REI_ACTION_FULLSCREEN:
		rei_wm_toggle_fullscreen(s);
		break;
	case REI_ACTION_WORKSPACE:
		rei_wm_goto_workspace(s, action->value);
		break;
	case REI_ACTION_MOVE_TO_WORKSPACE:
		rei_wm_move_to_workspace(s, action->value);
		break;
	}
	rei_server_schedule_flush(s);
}
