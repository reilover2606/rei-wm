#include "mpd.h"

#include <arpa/inet.h>
#include <errno.h>
#include <event2/event.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "config.h"
#include "input/actions.h"
#include "msgbuf.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"

#define RETRY_MS 2000
#define LAUNCH_RETRY_MS 400
#define LAUNCH_ATTEMPTS 25
#define MAX_QUEUE 16
#define RBUF 65536

enum kind { K_STATUS, K_SONG, K_CMD, K_PASSWORD };

struct req {
	char text[600];
	enum kind kind;
};

struct rei_mpd {
	struct rei_server *server;
	int fd;
	struct event *readable, *writable, *retry;
	enum { M_DOWN, M_CONNECTING, M_GREETING, M_READY } phase;

	char rbuf[RBUF];
	size_t rlen;
	char *wbuf;
	size_t wlen, wcap;

	struct req queue[MAX_QUEUE];
	int nq;
	bool busy;          /* a request is in flight */
	enum kind inflight;
	bool idling, noidle_sent;
	bool refresh_after_idle;

	/* The answer being collected (lines up to OK or ACK). */
	char resp[8192];
	size_t resp_len;

	/* What MPD said. */
	enum rei_mpd_state state;
	long songid;
	char title[256];
	bool known;         /* the first status has been seen (so the first song is not announced) */
	long announced;     /* the songid last announced */
	char pending_state[64];
	long pending_songid;

	/* Settings in use. */
	bool enabled;
	char host[320];
	int port;
	char password[512];
	char start_command[256];
	bool announce;
	int launch_left;
};

static struct rei_mpd *of(const struct rei_server *s)
{
	return s->mpd;
}

/* ------------------------------------------------------------------------ */
/* Text                                                                     */
/* ------------------------------------------------------------------------ */

void rei_mpd_format_song(const char *artist, const char *title, const char *name, const char *file,
			 char *out, unsigned long outlen)
{
	const char *t = title && title[0] ? title : (name && name[0] ? name : NULL);
	if (!t && file && file[0]) {
		const char *slash = strrchr(file, '/');
		t = slash ? slash + 1 : file;
	}
	if (!t)
		t = "";
	if (artist && artist[0] && t[0])
		snprintf(out, outlen, "%s - %s", artist, t);
	else
		snprintf(out, outlen, "%s", t);
	rei_utf8_trim(out);
}

/* The value of "key: value" in an answer, or NULL. */
static const char *field(const char *resp, const char *key, char *buf, size_t buflen)
{
	size_t kl = strlen(key);
	for (const char *p = resp; *p;) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		if (len > kl + 1 && strncmp(p, key, kl) == 0 && p[kl] == ':') {
			const char *v = p + kl + 1;
			while (*v == ' ' && v < p + len)
				v++;
			size_t vl = (size_t)(p + len - v);
			if (vl >= buflen)
				vl = buflen - 1;
			memcpy(buf, v, vl);
			buf[vl] = '\0';
			return buf;
		}
		if (!nl)
			break;
		p = nl + 1;
	}
	return NULL;
}

/* ------------------------------------------------------------------------ */
/* The connection                                                           */
/* ------------------------------------------------------------------------ */

static void changed(struct rei_mpd *m)
{
	rei_server_schedule_damage(m->server);   /* the controls, if open, show it */
}

static void close_connection(struct rei_mpd *m, const char *why)
{
	if (m->readable)
		event_free(m->readable);
	if (m->writable)
		event_free(m->writable);
	m->readable = m->writable = NULL;
	if (m->fd >= 0)
		close(m->fd);
	m->fd = -1;
	bool was_up = m->phase == M_READY;
	m->phase = M_DOWN;
	m->rlen = m->wlen = 0;
	m->nq = 0;
	m->busy = m->idling = m->noidle_sent = m->refresh_after_idle = false;
	m->resp_len = 0;
	m->known = false;
	m->state = REI_MPD_STOPPED;
	m->title[0] = '\0';
	m->songid = -1;
	if (was_up) {
		rei_info("mpd: disconnected (%s)", why);
		changed(m);
	}
}

static void schedule_retry(struct rei_mpd *m, int ms)
{
	if (!m->enabled || !m->retry)
		return;
	struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
	evtimer_add(m->retry, &tv);
}

static void flush_write(struct rei_mpd *m)
{
	while (m->wlen > 0) {
		ssize_t n = send(m->fd, m->wbuf, m->wlen, MSG_NOSIGNAL);
		if (n > 0) {
			memmove(m->wbuf, m->wbuf + n, m->wlen - (size_t)n);
			m->wlen -= (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (m->writable)
				event_add(m->writable, NULL);
			return;
		}
		close_connection(m, strerror(errno));
		schedule_retry(m, RETRY_MS);
		return;
	}
}

static void send_text(struct rei_mpd *m, const char *text)
{
	size_t n = strlen(text);
	if (m->wlen + n + 1 > m->wcap) {
		m->wcap = (m->wlen + n + 1) * 2;
		m->wbuf = rei_xrealloc(m->wbuf, m->wcap);
	}
	memcpy(m->wbuf + m->wlen, text, n);
	m->wlen += n;
	m->wbuf[m->wlen++] = '\n';
	flush_write(m);
}

static void pump(struct rei_mpd *m);

static void enqueue(struct rei_mpd *m, enum kind kind, const char *text)
{
	if (m->phase != M_READY || m->nq == MAX_QUEUE)
		return;
	struct req *r = &m->queue[m->nq++];
	r->kind = kind;
	snprintf(r->text, sizeof r->text, "%s", text);
	pump(m);
}

/* Sends the next request, or goes back to waiting for the player to change. */
static void pump(struct rei_mpd *m)
{
	if (m->phase != M_READY)
		return;
	if (m->busy) {
		if (m->idling && m->nq > 0 && !m->noidle_sent) {   /* something to say: end the wait first */
			m->noidle_sent = true;
			send_text(m, "noidle");
		}
		return;
	}
	if (m->nq > 0) {
		struct req r = m->queue[0];
		memmove(m->queue, m->queue + 1, (size_t)(m->nq - 1) * sizeof m->queue[0]);
		m->nq--;
		m->busy = true;
		m->inflight = r.kind;
		m->resp_len = 0;
		send_text(m, r.text);
		return;
	}
	if (m->refresh_after_idle) {
		m->refresh_after_idle = false;
		enqueue(m, K_STATUS, "status");
		enqueue(m, K_SONG, "currentsong");
		return;
	}
	/* Nothing to do: wait for the player to change. */
	m->busy = true;
	m->idling = true;
	m->noidle_sent = false;
	m->resp_len = 0;
	send_text(m, "idle player");
}

/* The status and the current song are both known: apply them and announce a new song. */
static void apply(struct rei_mpd *m, const char *song_resp)
{
	char v[512];
	char artist[256] = "", title[256] = "", name[256] = "", file[512] = "";
	field(song_resp, "Artist", artist, sizeof artist);
	field(song_resp, "Title", title, sizeof title);
	field(song_resp, "Name", name, sizeof name);
	field(song_resp, "file", file, sizeof file);
	char text[256];
	rei_mpd_format_song(artist, title, name, file, text, sizeof text);

	enum rei_mpd_state st = REI_MPD_STOPPED;
	if (!strcmp(m->pending_state, "play"))
		st = REI_MPD_PLAYING;
	else if (!strcmp(m->pending_state, "pause"))
		st = REI_MPD_PAUSED;
	(void)v;

	bool differs = st != m->state || strcmp(text, m->title) != 0;
	long id = m->pending_songid;
	bool announce = m->known && m->announce && st == REI_MPD_PLAYING && text[0] && id != m->announced &&
			(id != m->songid || m->state != REI_MPD_PLAYING);
	m->state = st;
	m->songid = id;
	snprintf(m->title, sizeof m->title, "%s", text);
	if (st == REI_MPD_PLAYING && text[0])
		m->announced = id;
	m->known = true;
	if (announce) {
		char msg[300];
		snprintf(msg, sizeof msg, "Now playing: %s", text);
		rei_msg_push(m->server, msg);
	}
	if (differs)
		changed(m);
}

static void handle_response(struct rei_mpd *m, bool ok)
{
	enum kind k = m->inflight;
	bool was_idle = m->idling;
	m->busy = false;
	m->idling = false;
	m->noidle_sent = false;
	m->resp[m->resp_len] = '\0';
	if (was_idle) {
		if (ok && strstr(m->resp, "changed:"))
			m->refresh_after_idle = true;
	} else if (!ok) {
		rei_debug("mpd: refused: %s", m->resp);
	} else if (k == K_STATUS) {
		char v[64];
		m->pending_state[0] = '\0';
		if (field(m->resp, "state", v, sizeof v))
			snprintf(m->pending_state, sizeof m->pending_state, "%s", v);
		m->pending_songid = field(m->resp, "songid", v, sizeof v) ? atol(v) : -1;
	} else if (k == K_SONG) {
		apply(m, m->resp);
	} else if (k == K_CMD) {
		m->refresh_after_idle = true;   /* (the idle that follows would say so too; this is just quicker) */
	}
	m->resp_len = 0;
	pump(m);
}

/* Takes complete lines from the read buffer. */
static void parse(struct rei_mpd *m)
{
	for (;;) {
		char *nl = memchr(m->rbuf, '\n', m->rlen);
		if (!nl)
			break;
		size_t len = (size_t)(nl - m->rbuf);
		m->rbuf[len] = '\0';
		const char *line = m->rbuf;
		if (m->phase == M_GREETING) {
			if (strncmp(line, "OK MPD", 6) != 0) {
				close_connection(m, "not an MPD server");
				schedule_retry(m, RETRY_MS);
				return;
			}
			m->phase = M_READY;
			rei_info("mpd: connected (%s)", line);
			if (m->password[0]) {
				char cmd[600];
				snprintf(cmd, sizeof cmd, "password %s", m->password);
				enqueue(m, K_PASSWORD, cmd);
			}
			enqueue(m, K_STATUS, "status");
			enqueue(m, K_SONG, "currentsong");
			changed(m);
		} else if (!strcmp(line, "OK") || !strncmp(line, "ACK", 3)) {
			bool ok = line[0] == 'O';
			if (!ok && m->resp_len + len + 2 < sizeof m->resp) {
				memcpy(m->resp + m->resp_len, line, len);
				m->resp_len += len;
			}
			size_t rest = m->rlen - len - 1;
			memmove(m->rbuf, nl + 1, rest);
			m->rlen = rest;
			handle_response(m, ok);
			if (m->phase != M_READY)
				return;
			continue;
		} else if (m->busy && m->resp_len + len + 2 < sizeof m->resp) {
			memcpy(m->resp + m->resp_len, line, len);
			m->resp_len += len;
			m->resp[m->resp_len++] = '\n';
		}
		size_t rest = m->rlen - len - 1;
		memmove(m->rbuf, nl + 1, rest);
		m->rlen = rest;
	}
	if (m->rlen == sizeof m->rbuf) {   /* a line longer than the buffer: not MPD */
		close_connection(m, "line too long");
		schedule_retry(m, RETRY_MS);
	}
}

static void on_readable(evutil_socket_t fd, short what, void *arg)
{
	(void)what;
	struct rei_mpd *m = arg;
	for (;;) {
		ssize_t n = recv(fd, m->rbuf + m->rlen, sizeof m->rbuf - m->rlen, 0);
		if (n > 0) {
			m->rlen += (size_t)n;
			parse(m);
			if (m->phase == M_DOWN)
				return;
			if (m->rlen < sizeof m->rbuf)
				continue;
			break;
		}
		if (n == 0) {
			close_connection(m, "closed by MPD");
			schedule_retry(m, RETRY_MS);
			return;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;
		close_connection(m, strerror(errno));
		schedule_retry(m, RETRY_MS);
		return;
	}
}

static void on_writable(evutil_socket_t fd, short what, void *arg)
{
	(void)what;
	struct rei_mpd *m = arg;
	if (m->phase == M_CONNECTING) {
		int err = 0;
		socklen_t l = sizeof err;
		getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l);
		if (err) {
			close_connection(m, strerror(err));
			schedule_retry(m, RETRY_MS);
			return;
		}
		m->phase = M_GREETING;
	}
	flush_write(m);
}

/* Where to connect. Fills `un` / `in`; returns the address length, or 0 (with a reason logged once). */
static socklen_t resolve(struct rei_mpd *m, struct sockaddr_storage *ss, int *family)
{
	const char *host = m->host;
	char pw_split[512];
	if (!host[0]) {
		host = getenv("MPD_HOST");
		if (!host || !host[0]) {
			const char *rt = getenv("XDG_RUNTIME_DIR");
			static char sock[300];
			if (rt && rt[0]) {
				snprintf(sock, sizeof sock, "%s/mpd/socket", rt);
				if (access(sock, F_OK) == 0)
					host = sock;
			}
			if (!host || !host[0]) {
				const char *home = getenv("HOME");
				if (home && home[0]) {   /* some setups keep the socket in ~/.mpd */
					snprintf(sock, sizeof sock, "%s/.mpd/socket", home);
					if (access(sock, F_OK) == 0)
						host = sock;
				}
			}
			if (!host || !host[0])
				host = "localhost";
		}
	}
	snprintf(pw_split, sizeof pw_split, "%s", host);
	char *at = strchr(pw_split, '@');
	char *hostpart = pw_split;
	if (at) {   /* "password@host" */
		*at = '\0';
		hostpart = at + 1;
		if (!m->password[0])
			snprintf(m->password, sizeof m->password, "%s", pw_split);
	}
	if (hostpart[0] == '/' || hostpart[0] == '~') {
		struct sockaddr_un *un = (struct sockaddr_un *)ss;
		memset(un, 0, sizeof *un);
		un->sun_family = AF_UNIX;
		const char *home = getenv("HOME");
		char full[512];
		if (hostpart[0] == '~' && home)
			snprintf(full, sizeof full, "%s%s", home, hostpart + 1);
		else
			snprintf(full, sizeof full, "%s", hostpart);
		if (strlen(full) >= sizeof un->sun_path) {
			rei_warn("mpd: the socket path is too long");
			return 0;
		}
		strcpy(un->sun_path, full);
		*family = AF_UNIX;
		return (socklen_t)sizeof *un;
	}
	int port = m->port;
	const char *pe = getenv("MPD_PORT");
	if (pe && atoi(pe) > 0 && port == 6600)
		port = atoi(pe);
	struct sockaddr_in *in = (struct sockaddr_in *)ss;
	memset(in, 0, sizeof *in);
	in->sin_family = AF_INET;
	in->sin_port = htons((uint16_t)port);
	/* Only addresses and "localhost": a name lookup would block the event loop. */
	if (!strcmp(hostpart, "localhost"))
		inet_pton(AF_INET, "127.0.0.1", &in->sin_addr);
	else if (inet_pton(AF_INET, hostpart, &in->sin_addr) != 1) {
		static bool warned;
		if (!warned)
			rei_warn("mpd: host '%s' is not an IP address, \"localhost\" or a socket path; "
				 "a name lookup would stall the compositor", hostpart);
		warned = true;
		return 0;
	}
	*family = AF_INET;
	return (socklen_t)sizeof *in;
}

static void try_connect(struct rei_mpd *m)
{
	if (!m->enabled || m->phase != M_DOWN)
		return;
	struct sockaddr_storage ss;
	int family = AF_INET;
	socklen_t len = resolve(m, &ss, &family);
	if (!len)
		return;
	int fd = socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	if (family == AF_INET) {
		int one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	}
	int rc = connect(fd, (struct sockaddr *)&ss, len);
	if (rc < 0 && errno != EINPROGRESS) {
		close(fd);
		return;   /* not running: the retry timer asks again */
	}
	m->fd = fd;
	m->phase = rc == 0 ? M_GREETING : M_CONNECTING;
	m->rlen = m->wlen = 0;
	m->readable = event_new(m->server->base, fd, EV_READ | EV_PERSIST, on_readable, m);
	m->writable = event_new(m->server->base, fd, EV_WRITE, on_writable, m);
	event_add(m->readable, NULL);
	if (m->phase == M_CONNECTING)
		event_add(m->writable, NULL);
}

static void on_retry(evutil_socket_t fd, short what, void *arg)
{
	(void)fd; (void)what;
	struct rei_mpd *m = arg;
	if (!m->enabled)
		return;
	if (m->phase == M_DOWN) {
		try_connect(m);
		int next = m->launch_left > 0 ? LAUNCH_RETRY_MS : RETRY_MS;
		if (m->launch_left > 0)
			m->launch_left--;
		schedule_retry(m, m->phase == M_DOWN ? next : RETRY_MS);
	} else if (m->phase == M_CONNECTING || m->phase == M_GREETING) {
		/* Still not up a whole retry period later: start over. */
		close_connection(m, "no answer");
		schedule_retry(m, RETRY_MS);
	} else {
		m->launch_left = 0;
	}
}

/* ------------------------------------------------------------------------ */
/* Public                                                                   */
/* ------------------------------------------------------------------------ */

void rei_mpd_configure(struct rei_server *s)
{
	const struct rei_config *cfg = s->config;
	if (!s->mpd) {
		if (!cfg->mpd_enabled)
			return;
		s->mpd = rei_xcalloc(1, sizeof *s->mpd);
		s->mpd->server = s;
		s->mpd->fd = -1;
		s->mpd->songid = -1;
		s->mpd->announced = -2;
		s->mpd->retry = evtimer_new(s->base, on_retry, s->mpd);
	}
	struct rei_mpd *m = s->mpd;
	bool same = m->enabled == cfg->mpd_enabled && !strcmp(m->host, cfg->mpd_host ? cfg->mpd_host : "") &&
		    m->port == cfg->mpd_port;
	m->enabled = cfg->mpd_enabled;
	snprintf(m->host, sizeof m->host, "%s", cfg->mpd_host ? cfg->mpd_host : "");
	m->port = cfg->mpd_port;
	m->password[0] = '\0';
	snprintf(m->start_command, sizeof m->start_command, "%s", cfg->mpd_start_command ? cfg->mpd_start_command : "mpd");
	m->announce = cfg->mpd_announce;
	if (!m->enabled) {
		if (m->phase != M_DOWN)
			close_connection(m, "disabled");
		evtimer_del(m->retry);
		changed(m);
		return;
	}
	if (!same && m->phase != M_DOWN)
		close_connection(m, "settings changed");
	if (m->phase == M_DOWN) {
		try_connect(m);
		schedule_retry(m, RETRY_MS);
	}
}

void rei_mpd_stop(struct rei_server *s)
{
	struct rei_mpd *m = s->mpd;
	if (!m)
		return;
	s->mpd = NULL;
	m->enabled = false;
	if (m->retry)
		event_free(m->retry);
	if (m->readable)
		event_free(m->readable);
	if (m->writable)
		event_free(m->writable);
	if (m->fd >= 0)
		close(m->fd);
	free(m->wbuf);
	free(m);
}

void rei_mpd_info_get(const struct rei_server *s, struct rei_mpd_info *out)
{
	memset(out, 0, sizeof *out);
	const struct rei_mpd *m = of(s);
	out->enabled = s->config->mpd_enabled;
	if (!m)
		return;
	out->connected = m->phase == M_READY;
	out->state = m->state;
	snprintf(out->title, sizeof out->title, "%s", m->title);
}

void rei_mpd_play(struct rei_server *s)
{
	struct rei_mpd *m = of(s);
	if (!m || m->phase != M_READY)
		return;
	if (m->state == REI_MPD_PAUSED)
		enqueue(m, K_CMD, "pause 0");
	else if (m->state == REI_MPD_STOPPED)
		enqueue(m, K_CMD, "play");
}

void rei_mpd_pause(struct rei_server *s)
{
	struct rei_mpd *m = of(s);
	if (m && m->phase == M_READY && m->state == REI_MPD_PLAYING)
		enqueue(m, K_CMD, "pause 1");
}

void rei_mpd_toggle(struct rei_server *s)
{
	struct rei_mpd *m = of(s);
	if (!m)
		return;
	if (m->state == REI_MPD_PLAYING)
		rei_mpd_pause(s);
	else
		rei_mpd_play(s);
}

void rei_mpd_next(struct rei_server *s)
{
	struct rei_mpd *m = of(s);
	if (m && m->phase == M_READY)
		enqueue(m, K_CMD, "next");
}

void rei_mpd_launch(struct rei_server *s)
{
	struct rei_mpd *m = of(s);
	if (!m || !m->enabled || m->phase == M_READY)
		return;
	rei_msg_push(s, "Starting MPD…");
	if (!rei_spawn(s, m->start_command))
		rei_msg_push(s, "MPD could not be started");
	m->launch_left = LAUNCH_ATTEMPTS;   /* look for it more often for a few seconds */
	schedule_retry(m, LAUNCH_RETRY_MS);
}
