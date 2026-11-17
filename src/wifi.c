#include "wifi.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <wayland-server-core.h>
#include <event2/event.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "desktop.h"
#include "proto/xdg_shell.h"
#include "xwayland.h"
#include "input/actions.h"
#include "msgbuf.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"

#define POLL_MS 3000

struct rei_wifi {
	struct event *timer;
	struct rei_wifi_info info;
	bool on;
	pid_t group;   /* the session of the instance the last click started (0: none) */
};

static bool is_dir(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* The link quality (0..70) of `iface` from /proc/net/wireless, or -1. */
static int link_quality(const char *procfile, const char *iface)
{
	FILE *f = fopen(procfile, "r");
	if (!f)
		return -1;
	char line[256];
	int q = -1;
	size_t n = strlen(iface);
	while (fgets(line, sizeof line, f)) {
		const char *p = line;
		while (*p == ' ')
			p++;
		if (strncmp(p, iface, n) != 0 || p[n] != ':')
			continue;
		/* "wlan0: 0000   58.  -52.  -256 ...": the status, then the link quality */
		char status[16];
		double link;
		if (sscanf(p + n + 1, "%15s %lf", status, &link) == 2)
			q = (int)link;
		break;
	}
	fclose(f);
	return q;
}

bool rei_wifi_read(const char *netdir, const char *procfile, struct rei_wifi_info *out)
{
	memset(out, 0, sizeof *out);
	DIR *d = opendir(netdir);
	if (!d)
		return false;
	int best_q = -2;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		char path[600];
		snprintf(path, sizeof path, "%s/%s/wireless", netdir, de->d_name);
		bool wireless = is_dir(path);
		if (!wireless) {
			snprintf(path, sizeof path, "%s/%s/phy80211", netdir, de->d_name);
			wireless = access(path, F_OK) == 0;
		}
		if (!wireless)
			continue;
		out->present = true;
		snprintf(path, sizeof path, "%s/%s/operstate", netdir, de->d_name);
		char state[32] = "";
		FILE *f = fopen(path, "r");
		if (f) {
			if (fscanf(f, "%31s", state) != 1)
				state[0] = '\0';
			fclose(f);
		}
		bool up = !strcmp(state, "up");
		int q = up ? link_quality(procfile, de->d_name) : -2;
		if (up && q == -1)
			q = 70;   /* connected, but no table to say how well: show it as strong */
		if (!out->iface[0] || (up && best_q == -2) || (up && q > best_q)) {
			snprintf(out->iface, sizeof out->iface, "%s", de->d_name);
			if (up)
				best_q = q;
		}
		if (up)
			out->connected = true;
	}
	closedir(d);
	if (out->connected)
		out->level = best_q >= 46 ? 3 : best_q >= 24 ? 2 : 1;
	return out->present;
}

static void poll(struct rei_server *s)
{
	struct rei_wifi *w = s->wifi;
	const char *netdir = getenv("REI_WIFI_DIR");    /* test hooks */
	const char *procfile = getenv("REI_WIFI_PROC");
	struct rei_wifi_info now;
	rei_wifi_read(netdir && netdir[0] ? netdir : "/sys/class/net",
		      procfile && procfile[0] ? procfile : "/proc/net/wireless", &now);
	if (now.present != w->info.present || now.connected != w->info.connected || now.level != w->info.level) {
		w->info = now;
		if (s->damage_event)   /* (not yet, while the server is still starting up) */
			rei_server_schedule_damage(s);   /* the toolbar redraws */
	}
}

static void on_timer(evutil_socket_t fd, short what, void *arg)
{
	(void)fd; (void)what;
	struct rei_server *s = arg;
	if (!s->wifi || !s->wifi->on)
		return;
	poll(s);
	struct timeval tv = { POLL_MS / 1000, (POLL_MS % 1000) * 1000 };
	evtimer_add(s->wifi->timer, &tv);
}

void rei_wifi_configure(struct rei_server *s)
{
	bool want = s->config->toolbar_wifi;
	if (!s->wifi) {
		if (!want)
			return;
		s->wifi = rei_xcalloc(1, sizeof *s->wifi);
		s->wifi->timer = evtimer_new(s->base, on_timer, s);
	}
	struct rei_wifi *w = s->wifi;
	if (want == w->on)
		return;
	w->on = want;
	if (want) {
		poll(s);
		struct timeval tv = { POLL_MS / 1000, (POLL_MS % 1000) * 1000 };
		evtimer_add(w->timer, &tv);
	} else {
		evtimer_del(w->timer);
		memset(&w->info, 0, sizeof w->info);
		if (s->damage_event)
			rei_server_schedule_damage(s);
	}
}

void rei_wifi_stop(struct rei_server *s)
{
	struct rei_wifi *w = s->wifi;
	if (!w)
		return;
	s->wifi = NULL;
	if (w->timer)
		event_free(w->timer);
	free(w);
}

struct rei_wifi_info rei_wifi_state(const struct rei_server *s)
{
	struct rei_wifi_info none;
	memset(&none, 0, sizeof none);
	return s->wifi && s->wifi->on ? s->wifi->info : none;
}

/* Whether the instance the icon started is still running (anything left in its session). */
static bool instance_alive(const struct rei_wifi *w)
{
	return w && w->group > 0 && (kill(-w->group, 0) == 0 || errno == EPERM);
}

void rei_wifi_click(struct rei_server *s)
{
	const char *cmd = s->config->toolbar_wifi_command;
	if (!cmd || !cmd[0])
		return;
	/* Clicking again closes what the first click opened, rather than opening a second. */
	if (instance_alive(s->wifi)) {
		rei_info("wifi: closing %s", cmd);
		kill(-s->wifi->group, SIGTERM);
		s->wifi->group = 0;
		return;
	}
	rei_info("wifi: %s", cmd);
	pid_t group = 0;
	rei_msg_run_captured_group(s, cmd, &group);   /* (if it cannot be run, its error is a message) */
	if (s->wifi)
		s->wifi->group = group;
}

bool rei_wifi_owns_view(struct rei_server *s, const struct rei_view *v)
{
	if (!instance_alive(s->wifi))
		return false;
	pid_t pid = 0;
	if (v->xwin) {
		pid = rei_xwindow_pid(v->xwin);
	} else if (v->toplevel && v->toplevel->resource) {
		uid_t uid;
		gid_t gid;
		wl_client_get_credentials(wl_resource_get_client(v->toplevel->resource), &pid, &uid, &gid);
	}
	return pid > 0 && getsid(pid) == s->wifi->group;
}
