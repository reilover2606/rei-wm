#include "input/actions.h"

#include <errno.h>
#include <fcntl.h>
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
#include "audio.h"
#include "volume.h"
#include "brightness.h"
#include "msgbuf.h"
#include "runprompt.h"
#include "sessionbus.h"
#include "xwayland.h"

extern char **environ;

/* A copy of the environment with WAYLAND_DISPLAY set to our socket and DISPLAY removed. Built before
 * forking because the child of a multithreaded process may only call
 * async-signal-safe functions, and setenv() is not one of them. */
static char **build_environment(struct rei_server *server, const char *socket_name)
{
	size_t n = 0;
	while (environ[n])
		n++;

	char **env = rei_xcalloc(n + 5, sizeof *env);
	size_t out = 0;
	for (size_t i = 0; i < n; i++) {
		/* DISPLAY would point X11 programs at some other X server (e.g. your
		 * desktop's) until rei-wm runs its own Xwayland. */
		if (strncmp(environ[i], "WAYLAND_DISPLAY=", 16) != 0 &&
		    strncmp(environ[i], "DISPLAY=", 8) != 0 &&
		    strncmp(environ[i], "XDG_RUNTIME_DIR=", 16) != 0 &&
		    !(rei_sessionbus_address(server) && strncmp(environ[i], "DBUS_SESSION_BUS_ADDRESS=", 25) == 0))
			env[out++] = rei_xstrdup(environ[i]);
	}
	char *wd = NULL;
	if (asprintf(&wd, "WAYLAND_DISPLAY=%s", socket_name) < 0)
		abort();
	env[out++] = wd;
	/* WAYLAND_DISPLAY is relative to this directory; a program that lacks it
	 * cannot find our socket and would fall back to some other display. */
	const char *xdisplay = rei_xwayland_display_name(server);
	if (xdisplay) {
		char *xd = NULL;
		if (asprintf(&xd, "DISPLAY=%s", xdisplay) < 0)
			abort();
		env[out++] = xd;
	}
	if (rei_sessionbus_address(server)) {
		char *db = NULL;
		if (asprintf(&db, "DBUS_SESSION_BUS_ADDRESS=%s", rei_sessionbus_address(server)) < 0)
			abort();
		env[out++] = db;
	}
	const char *rt = getenv("XDG_RUNTIME_DIR");
	if (rt && rt[0]) {
		char *xr = NULL;
		if (asprintf(&xr, "XDG_RUNTIME_DIR=%s", rt) < 0)
			abort();
		env[out++] = xr;
	}
	return env;
}

static void free_environment(char **env)
{
	for (char **e = env; *e; e++)
		free(*e);
	free(env);
}

/* Starts `command` through the shell, detached. With out_fd, its stdout and stderr are connected to a
 * pipe whose read end is returned there (non-blocking); its stdin is /dev/null. */
static bool spawn(struct rei_server *s, const char *command, int *out_fd, pid_t *group)
{
	if (group)
		*group = 0;
	char **env = build_environment(s, s->socket_name);
	bool ok = false;
	int pfd[2] = { -1, -1 };
	if (out_fd && pipe2(pfd, O_CLOEXEC) != 0) {
		rei_error("cannot start '%s': pipe: %s", command, strerror(errno));
		free_environment(env);
		return false;
	}

	pid_t pid = fork();
	if (pid < 0) {
		rei_error("cannot start '%s': fork: %s", command, strerror(errno));
	} else if (pid == 0) {
		/* Double fork: the grandchild is adopted by init, so there is never a
		 * zombie to reap and no SIGCHLD handling is needed here. */
		rei_child_reset_signals();
		setsid();
		pid_t inner = fork();
		if (inner == 0) {
			if (out_fd) {
				int nul = open("/dev/null", O_RDONLY);
				if (nul >= 0)
					dup2(nul, STDIN_FILENO);
				dup2(pfd[1], STDOUT_FILENO);
				dup2(pfd[1], STDERR_FILENO);
			}
			char *const argv[] = { "sh", "-c", (char *)command, NULL };
			execve("/bin/sh", argv, env);
			_exit(127);
		}
		_exit(inner < 0 ? 1 : 0);
	} else {
		int status = 0;
		waitpid(pid, &status, 0);
		ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
		if (ok && group)
			*group = pid;   /* its setsid made it the session and process group id of all it started */
		if (!ok)
			rei_error("cannot start '%s'", command);
	}
	free_environment(env);
	if (out_fd) {
		if (pfd[1] >= 0)
			close(pfd[1]);
		if (ok) {
			fcntl(pfd[0], F_SETFL, fcntl(pfd[0], F_GETFL) | O_NONBLOCK);
			*out_fd = pfd[0];
		} else if (pfd[0] >= 0) {
			close(pfd[0]);
		}
	}
	return ok;
}

bool rei_spawn(struct rei_server *s, const char *command)
{
	return spawn(s, command, NULL, NULL);
}

bool rei_spawn_capture(struct rei_server *s, const char *command, int *out_fd)
{
	return spawn(s, command, out_fd, NULL);
}

bool rei_spawn_capture_group(struct rei_server *s, const char *command, int *out_fd, pid_t *group)
{
	return spawn(s, command, out_fd, group);
}

void rei_actions_run(struct rei_server *s, const struct rei_action *action)
{
	switch (action->type) {
	case REI_ACTION_NONE:
		break;
	case REI_ACTION_RUN:
		rei_prompt_open(s);
		break;
	case REI_ACTION_MESSAGES:
		rei_msg_toggle_stack(s);
		break;
	case REI_ACTION_MINIMIZE:
		rei_wm_minimize_active(s);
		break;
	case REI_ACTION_VOLUME_UP:
		rei_volume_change(s, action->value);
		break;
	case REI_ACTION_VOLUME_DOWN:
		rei_volume_change(s, -(action->value ? action->value : s->config->volume_step));
		break;
	case REI_ACTION_BRIGHTNESS_UP:
		rei_brightness_change(s, action->value);
		break;
	case REI_ACTION_BRIGHTNESS_DOWN:
		rei_brightness_change(s, -(action->value ? action->value : s->config->brightness_step));
		break;
	case REI_ACTION_VOLUME_MUTE:
		rei_volume_toggle_mute(s);
		break;
	case REI_ACTION_EXEC:
		rei_info("exec: %s", action->arg);
		rei_spawn(s, action->arg);
		break;
	case REI_ACTION_CLOSE: {
		struct rei_view *v = rei_wm_active_view(s);
		if (v)
			rei_wm_close_view(v);
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
	case REI_ACTION_RESIZE_MODE:
		rei_wm_set_resize_mode(s, !s->resize_mode);
		break;
	case REI_ACTION_WORKSPACE:
		rei_wm_workspace_command(s, action->value);
		break;
	case REI_ACTION_MOVE_TO_WORKSPACE:
		rei_wm_move_to_workspace(s, action->value);
		break;
	}
	rei_server_schedule_flush(s);
}
