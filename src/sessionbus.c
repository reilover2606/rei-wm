#include "sessionbus.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "server.h"
#include "util/log.h"
#include "util/util.h"

struct rei_sessionbus {
	char address[512];
	pid_t pid;
};

const char *rei_sessionbus_address(const struct rei_server *s)
{
	return s->sessionbus ? s->sessionbus->address : NULL;
}

void rei_sessionbus_start(struct rei_server *s)
{
	if (!s->config->session_bus_private)
		return;
	int out[2];
	if (pipe(out) < 0)
		return;
	pid_t pid = fork();
	if (pid < 0) {
		close(out[0]);
		close(out[1]);
		return;
	}
	if (pid == 0) {
		dup2(out[1], STDOUT_FILENO);
		close(out[0]);
		close(out[1]);
		prctl(PR_SET_PDEATHSIG, SIGTERM);   /* gone with rei-wm, even if it crashes */
		/* --fork would detach; we want to know the pid and be able to stop it, so stay in the
		 * foreground and read the address it prints. */
		execlp("dbus-daemon", "dbus-daemon", "--session", "--nofork", "--print-address=1", (char *)NULL);
		_exit(127);
	}
	close(out[1]);
	char addr[512] = "";
	size_t n = 0;
	while (n < sizeof addr - 1) {
		ssize_t r = read(out[0], addr + n, 1);
		if (r <= 0 || addr[n] == '\n')
			break;
		n++;
	}
	addr[n] = '\0';
	close(out[0]);   /* dbus-daemon needs nothing more from this pipe */
	if (n == 0) {
		rei_warn("session bus: dbus-daemon did not start; programs share the existing bus");
		kill(pid, SIGTERM);
		waitpid(pid, NULL, 0);
		return;
	}
	struct rei_sessionbus *b = rei_xcalloc(1, sizeof *b);
	snprintf(b->address, sizeof b->address, "%s", addr);
	b->pid = pid;
	s->sessionbus = b;
	rei_info("session bus: private bus for launched programs at %s", b->address);
}

void rei_sessionbus_stop(struct rei_server *s)
{
	struct rei_sessionbus *b = s->sessionbus;
	if (!b)
		return;
	s->sessionbus = NULL;
	kill(b->pid, SIGTERM);
	waitpid(b->pid, NULL, 0);
	free(b);
}
