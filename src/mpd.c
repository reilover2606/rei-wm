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
#include "pixbuf.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"
#include "../third_party/stb_image.h"

#define RETRY_MS 2000
#define LAUNCH_RETRY_MS 400
#define LAUNCH_ATTEMPTS 25
#define MAX_QUEUE 16
#define RBUF 65536

enum kind { K_STATUS, K_SONG, K_CMD, K_PASSWORD, K_ART, K_ART_DIR, K_IGNORE };

#define ART_MAX_BYTES (16 << 20)
#define ART_MAX_PX 640

struct req {
	char text[1200];
	enum kind kind;
	int gen;   /* the song's art generation when it was asked (an answer for another song is dropped) */
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
	int inflight_gen;
	bool idling, noidle_sent;
	bool refresh_after_idle;

	/* The answer being collected (lines up to OK or ACK). */
	char resp[8192];
	size_t resp_len;

	/* What MPD said. */
	enum rei_mpd_state state;
	int volume;         /* -1: no mixer */
	int premute;        /* the volume before it was "muted" (0: not muted) */
	bool shuffle, repeat, single;   /* random, repeat, single (the loop of one song needs repeat and single) */
	long songid;
	char title[256];
	bool known;         /* the first status has been seen (so the first song is not announced) */
	long announced;     /* the songid last announced */
	char pending_state[64];
	long pending_songid;
	int pending_volume;
	bool pending_random, pending_repeat, pending_single;
	int pending_elapsed_ms, pending_duration_ms;
	int elapsed_ms, duration_ms;   /* as of `stamp_ns` */
	uint64_t stamp_ns;

	/* The cover. `art_gen` counts songs: an answer for an earlier one is dropped. */
	char file[512];
	int art_gen;
	struct rei_pixbuf *art;
	uint8_t *art_buf;           /* the picture as MPD sends it, in chunks */
	size_t art_len, art_total;
	bool art_dir_tried;
	uint8_t *chunk;             /* the chunk being received (raw bytes after a "binary: N" line) */
	size_t chunk_len, bin_need;
	bool album_art;

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

static void clear_art(struct rei_mpd *m)
{
	m->art_gen++;
	free(m->art_buf);
	m->art_buf = NULL;
	m->art_len = m->art_total = 0;
	m->art_dir_tried = false;
	if (m->art) {
		rei_pixbuf_unref(m->art);
		m->art = NULL;
	}
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
	m->volume = -1;
	m->premute = 0;
	m->shuffle = m->repeat = m->single = false;
	m->title[0] = '\0';
	m->songid = -1;
	m->elapsed_ms = m->duration_ms = -1;
	m->file[0] = '\0';
	m->bin_need = m->chunk_len = 0;
	clear_art(m);
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
	if (m->phase != M_READY)
		return;
	/* A slider drag or a held key sends many volumes: only the last one matters. */
	if (kind == K_CMD && !strncmp(text, "setvol", 6) && m->nq > 0 && m->queue[m->nq - 1].kind == K_CMD &&
	    !strncmp(m->queue[m->nq - 1].text, "setvol", 6)) {
		snprintf(m->queue[m->nq - 1].text, sizeof m->queue[0].text, "%s", text);
		return;
	}
	if (m->nq == MAX_QUEUE)
		return;
	struct req *r = &m->queue[m->nq++];
	r->kind = kind;
	r->gen = m->art_gen;
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
		m->inflight_gen = r.gen;
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
	send_text(m, "idle player mixer");   /* (the mixer: someone else turned the volume) */
}

/* ------------------------------------------------------------------------ */
/* The cover                                                                */
/* ------------------------------------------------------------------------ */

/* `s` as an MPD argument: in double quotes, with \ and " escaped. */
static void quote(char *out, size_t n, const char *s)
{
	size_t o = 0;
	if (o < n)
		out[o++] = '"';
	for (; *s && o + 3 < n; s++) {
		if (*s == '"' || *s == '\\')
			out[o++] = '\\';
		out[o++] = *s;
	}
	if (o < n)
		out[o++] = '"';
	out[o < n ? o : n - 1] = '\0';
}

static void ask_art(struct rei_mpd *m, enum kind kind, size_t offset)
{
	char q[1100], cmd[1200];
	quote(q, sizeof q, m->file);
	snprintf(cmd, sizeof cmd, "%s %s %zu", kind == K_ART ? "readpicture" : "albumart", q, offset);
	enqueue(m, kind, cmd);
}

/* The whole picture has arrived: decode it, shrink it to fit ART_MAX_PX, keep it. */
static void art_finish(struct rei_mpd *m)
{
	int w = 0, h = 0, c = 0;
	uint8_t *px = stbi_load_from_memory(m->art_buf, (int)m->art_len, &w, &h, &c, 4);
	free(m->art_buf);
	m->art_buf = NULL;
	m->art_len = m->art_total = 0;
	if (!px || w <= 0 || h <= 0 || w > 16384 || h > 16384) {
		rei_debug("mpd: the cover could not be decoded");
		if (px)
			stbi_image_free(px);
		return;
	}
	int f = 1;
	while ((w / f > ART_MAX_PX) || (h / f > ART_MAX_PX))
		f++;
	int ow = w / f, oh = h / f;
	struct rei_pixbuf *pb = ow > 0 && oh > 0 ? rei_pixbuf_create_uninit(ow, oh, false) : NULL;
	if (pb) {
		for (int y = 0; y < oh; y++)
			for (int x = 0; x < ow; x++) {
				unsigned r = 0, g = 0, b = 0, a = 0;
				for (int dy = 0; dy < f; dy++)
					for (int dx = 0; dx < f; dx++) {
						const uint8_t *s = px + ((size_t)(y * f + dy) * (size_t)w + (size_t)(x * f + dx)) * 4;
						r += s[0]; g += s[1]; b += s[2]; a += s[3];
					}
				unsigned n = (unsigned)(f * f);
				r /= n; g /= n; b /= n; a /= n;
				uint8_t *d = pb->data + ((size_t)y * (size_t)ow + (size_t)x) * 4;
				d[0] = (uint8_t)(b * a / 255);   /* premultiplied B, G, R, A */
				d[1] = (uint8_t)(g * a / 255);
				d[2] = (uint8_t)(r * a / 255);
				d[3] = (uint8_t)a;
			}
	}
	stbi_image_free(px);
	if (!pb)
		return;
	rei_pixbuf_unref(m->art);
	m->art = pb;
	rei_debug("mpd: cover %dx%d (from %dx%d)", ow, oh, w, h);
	changed(m);
}

/* An answer to readpicture / albumart: `ok`, with a chunk received (m->chunk) or none. */
static void art_answer(struct rei_mpd *m, enum kind k, bool ok)
{
	char v[64];
	bool got = ok && m->chunk_len > 0;
	if (got) {
		if (m->art_len == 0 && field(m->resp, "size", v, sizeof v))
			m->art_total = (size_t)strtoull(v, NULL, 10);
		if (m->art_total == 0 || m->art_total > ART_MAX_BYTES || m->art_len + m->chunk_len > m->art_total) {
			got = false;
		} else {
			if (!m->art_buf)
				m->art_buf = rei_xrealloc(NULL, m->art_total);
			memcpy(m->art_buf + m->art_len, m->chunk, m->chunk_len);
			m->art_len += m->chunk_len;
		}
	}
	m->chunk_len = 0;
	if (got) {
		if (m->art_len < m->art_total)
			ask_art(m, k, m->art_len);   /* the next piece */
		else
			art_finish(m);
		return;
	}
	free(m->art_buf);
	m->art_buf = NULL;
	m->art_len = m->art_total = 0;
	if (k == K_ART && !m->art_dir_tried) {   /* no picture inside the file: the cover file next to it */
		m->art_dir_tried = true;
		ask_art(m, K_ART_DIR, 0);
	}
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

	if (strcmp(file, m->file) != 0) {   /* another song: its cover */
		snprintf(m->file, sizeof m->file, "%s", file);
		clear_art(m);
		if (m->album_art && file[0])
			ask_art(m, K_ART, 0);
	}
	m->elapsed_ms = m->pending_elapsed_ms;
	m->duration_ms = m->pending_duration_ms;
	m->stamp_ns = rei_now_ns();
	bool differs = st != m->state || strcmp(text, m->title) != 0 || m->pending_volume != m->volume ||
		       m->pending_random != m->shuffle || m->pending_repeat != m->repeat || m->pending_single != m->single;
	m->shuffle = m->pending_random;
	m->repeat = m->pending_repeat;
	m->single = m->pending_single;
	if (m->pending_volume != m->volume && m->pending_volume != 0)
		m->premute = 0;   /* someone raised it: no longer muted by us */
	m->volume = m->pending_volume;
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
	if (!was_idle && (k == K_ART || k == K_ART_DIR)) {   /* (an idle wait leaves `inflight` as it was) */
		if (m->inflight_gen == m->art_gen && m->album_art)
			art_answer(m, k, ok);
		else
			m->chunk_len = 0;
		m->resp_len = 0;
		pump(m);
		return;
	}
	if (k == K_IGNORE && !was_idle) {
		m->resp_len = 0;
		pump(m);
		return;
	}
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
		m->pending_random = field(m->resp, "random", v, sizeof v) && atoi(v) == 1;
		m->pending_repeat = field(m->resp, "repeat", v, sizeof v) && atoi(v) == 1;
		/* single is 0, 1 or "oneshot" (stop after this song, once) */
		m->pending_single = field(m->resp, "single", v, sizeof v) && !strcmp(v, "1");
		m->pending_elapsed_ms = m->pending_duration_ms = -1;
		if (field(m->resp, "elapsed", v, sizeof v))
			m->pending_elapsed_ms = (int)(atof(v) * 1000.0);
		if (field(m->resp, "duration", v, sizeof v))
			m->pending_duration_ms = (int)(atof(v) * 1000.0);
		if (m->pending_duration_ms < 0 && field(m->resp, "time", v, sizeof v)) {   /* older MPD: "elapsed:total" */
			const char *colon = strchr(v, ':');
			if (m->pending_elapsed_ms < 0)
				m->pending_elapsed_ms = atoi(v) * 1000;
			if (colon)
				m->pending_duration_ms = atoi(colon + 1) * 1000;
		}
		if (m->pending_duration_ms == 0)
			m->pending_duration_ms = -1;
		m->pending_volume = field(m->resp, "volume", v, sizeof v) ? atoi(v) : -1;
		if (m->pending_volume < 0 || m->pending_volume > 100)
			m->pending_volume = -1;
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
		if (m->bin_need > 0) {   /* raw bytes of a picture chunk, not lines */
			size_t take = m->rlen < m->bin_need ? m->rlen : m->bin_need;
			memcpy(m->chunk + m->chunk_len, m->rbuf, take);
			m->chunk_len += take;
			m->bin_need -= take;
			memmove(m->rbuf, m->rbuf + take, m->rlen - take);
			m->rlen -= take;
			if (m->bin_need > 0)
				return;
			continue;
		}
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
			if (m->album_art)
				enqueue(m, K_IGNORE, "binarylimit 262144");   /* (bigger pieces: fewer round trips) */
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
		} else if (m->busy && (m->inflight == K_ART || m->inflight == K_ART_DIR) && !strncmp(line, "binary: ", 8)) {
			size_t need = (size_t)strtoull(line + 8, NULL, 10);
			if (need > (size_t)4 << 20) {   /* more than binarylimit allows: not what was asked for */
				close_connection(m, "picture chunk too large");
				schedule_retry(m, RETRY_MS);
				return;
			}
			free(m->chunk);
			m->chunk = rei_xrealloc(NULL, need ? need : 1);
			m->chunk_len = 0;
			m->bin_need = need;
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
		s->mpd->volume = -1;
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
	bool art_was = m->album_art;
	m->album_art = cfg->mpd_album_art;
	if (art_was && !m->album_art) {   /* turned off: the cover goes */
		clear_art(m);
		changed(m);
	}
	if (!art_was && m->album_art && m->phase == M_READY && m->file[0])
		ask_art(m, K_ART, 0);
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
	free(m->art_buf);
	free(m->chunk);
	rei_pixbuf_unref(m->art);
	free(m);
}

void rei_mpd_info_get(const struct rei_server *s, struct rei_mpd_info *out)
{
	memset(out, 0, sizeof *out);
	out->volume = -1;
	const struct rei_mpd *m = of(s);
	out->enabled = s->config->mpd_enabled;
	if (!m)
		return;
	out->connected = m->phase == M_READY;
	out->state = m->state;
	out->volume = m->volume;
	out->muted = m->volume == 0 && m->premute > 0;
	out->shuffle = m->shuffle;
	out->loop_all = m->repeat && !m->single;
	out->loop_one = m->repeat && m->single;
	snprintf(out->title, sizeof out->title, "%s", m->title);
	out->elapsed_ms = -1;
	out->duration_ms = m->duration_ms > 0 ? m->duration_ms : -1;
	if (m->phase == M_READY && m->elapsed_ms >= 0 && m->state != REI_MPD_STOPPED) {
		int64_t e = m->elapsed_ms;
		if (m->state == REI_MPD_PLAYING)
			e += (int64_t)((rei_now_ns() - m->stamp_ns) / 1000000ull);   /* it moves on by itself */
		if (m->duration_ms > 0 && e > m->duration_ms)
			e = m->duration_ms;
		out->elapsed_ms = (int)e;
	}
}

void rei_mpd_seek(struct rei_server *s, double seconds)
{
	struct rei_mpd *m = of(s);
	if (!m || m->phase != M_READY || m->state == REI_MPD_STOPPED || m->duration_ms <= 0)
		return;
	if (seconds < 0)
		seconds = 0;
	if (seconds * 1000.0 > m->duration_ms)
		seconds = m->duration_ms / 1000.0;
	char cmd[48];
	snprintf(cmd, sizeof cmd, "seekcur %.3f", seconds);
	enqueue(m, K_CMD, cmd);
	m->elapsed_ms = (int)(seconds * 1000.0);   /* known now; MPD confirms it */
	m->stamp_ns = rei_now_ns();
	changed(m);
}

struct rei_pixbuf *rei_mpd_album_art(const struct rei_server *s)
{
	const struct rei_mpd *m = of(s);
	return m && m->album_art && m->phase == M_READY ? m->art : NULL;
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

void rei_mpd_set_volume(struct rei_server *s, int percent)
{
	struct rei_mpd *m = of(s);
	if (!m || m->phase != M_READY || m->volume < 0)
		return;
	percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	if (percent != 0)
		m->premute = 0;
	char cmd[32];
	snprintf(cmd, sizeof cmd, "setvol %d", percent);
	enqueue(m, K_CMD, cmd);
	if (percent != m->volume) {
		m->volume = percent;   /* known now; MPD confirms it */
		changed(m);
	}
}

void rei_mpd_toggle_mute(struct rei_server *s)
{
	struct rei_mpd *m = of(s);
	if (!m || m->phase != M_READY || m->volume < 0)
		return;
	if (m->volume == 0 && m->premute > 0) {
		int back = m->premute;
		m->premute = 0;
		rei_mpd_set_volume(s, back);
	} else if (m->volume > 0) {
		int was = m->volume;
		rei_mpd_set_volume(s, 0);
		m->premute = was;
	}
}

void rei_mpd_previous(struct rei_server *s)
{
	struct rei_mpd *m = of(s);
	if (m && m->phase == M_READY)
		enqueue(m, K_CMD, "previous");
}

static void set_flag(struct rei_mpd *m, const char *name, bool *flag, bool on)
{
	char cmd[32];
	snprintf(cmd, sizeof cmd, "%s %d", name, on ? 1 : 0);
	enqueue(m, K_CMD, cmd);
	*flag = on;   /* known now; MPD confirms it */
}

void rei_mpd_toggle_shuffle(struct rei_server *s)
{
	struct rei_mpd *m = of(s);
	if (!m || m->phase != M_READY)
		return;
	set_flag(m, "random", &m->shuffle, !m->shuffle);
	changed(m);
}

void rei_mpd_toggle_loop_all(struct rei_server *s)
{
	struct rei_mpd *m = of(s);
	if (!m || m->phase != M_READY)
		return;
	bool on = !(m->repeat && !m->single);
	set_flag(m, "single", &m->single, false);
	set_flag(m, "repeat", &m->repeat, on);
	changed(m);
}

void rei_mpd_toggle_loop_one(struct rei_server *s)
{
	struct rei_mpd *m = of(s);
	if (!m || m->phase != M_READY)
		return;
	bool on = !(m->repeat && m->single);
	set_flag(m, "repeat", &m->repeat, on);
	set_flag(m, "single", &m->single, on);
	changed(m);
}
