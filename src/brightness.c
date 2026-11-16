#include "brightness.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "input/actions.h"
#include "msgbuf.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"

static const char *backlight_dir(void)
{
	const char *d = getenv("REI_BACKLIGHT_DIR");   /* a test hook */
	return d && d[0] ? d : "/sys/class/backlight";
}

static bool read_long(const char *dir, const char *name, const char *file, long *out)
{
	char path[600];
	snprintf(path, sizeof path, "%s/%s/%s", dir, name, file);
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	long v;
	bool ok = fscanf(f, "%ld", &v) == 1;
	fclose(f);
	if (ok)
		*out = v;
	return ok;
}

static int type_rank(const char *type)
{
	return !strcmp(type, "raw") ? 3 : !strcmp(type, "platform") ? 2 : !strcmp(type, "firmware") ? 1 : 0;
}

bool rei_backlight_find(const char *dir, const char *want, struct rei_backlight *out)
{
	DIR *d = opendir(dir);
	if (!d)
		return false;
	bool found = false;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		struct rei_backlight b;
		memset(&b, 0, sizeof b);
		snprintf(b.name, sizeof b.name, "%s", de->d_name);
		if (!read_long(dir, b.name, "max_brightness", &b.max) || b.max <= 0 ||
		    !read_long(dir, b.name, "brightness", &b.cur))
			continue;
		char tpath[600];
		snprintf(tpath, sizeof tpath, "%s/%s/type", dir, b.name);
		FILE *tf = fopen(tpath, "r");
		if (tf) {
			if (fscanf(tf, "%15s", b.type) != 1)
				b.type[0] = '\0';
			fclose(tf);
		}
		if (want && want[0]) {
			if (!strcmp(want, b.name)) {
				*out = b;
				found = true;
				break;
			}
			continue;
		}
		if (!found || type_rank(b.type) > type_rank(out->type) ||
		    (type_rank(b.type) == type_rank(out->type) && b.max > out->max)) {
			*out = b;
			found = true;
		}
	}
	closedir(d);
	return found;
}

/* ---- Setting it ---- */

/* A name that is safe to put in a command. */
static bool plain_name(const char *s)
{
	if (!s[0])
		return false;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-' && *s != '.' && *s != ':')
			return false;
	return true;
}

static void fill_command(const char *tmpl, const struct rei_backlight *b, long value, int percent, char *out, size_t n)
{
	size_t o = 0;
	for (const char *p = tmpl; *p && o + 1 < n;) {
		char sub[300] = "";
		size_t len = 0;
		if (!strncmp(p, "{value}", 7)) {
			snprintf(sub, sizeof sub, "%ld", value);
			len = 7;
		} else if (!strncmp(p, "{percent}", 9)) {
			snprintf(sub, sizeof sub, "%d", percent);
			len = 9;
		} else if (!strncmp(p, "{device}", 8)) {
			snprintf(sub, sizeof sub, "%s", b->name);
			len = 8;
		}
		if (len) {
			for (const char *q = sub; *q && o + 1 < n; q++)
				out[o++] = *q;
			p += len;
		} else {
			out[o++] = *p++;
		}
	}
	out[o] = '\0';
}

static bool write_direct(const char *dir, const struct rei_backlight *b, long value)
{
	if (getenv("REI_BACKLIGHT_NOWRITE"))   /* a test hook: act as if the file were not ours to write */
		return false;
	char path[600];
	snprintf(path, sizeof path, "%s/%s/brightness", dir, b->name);
	FILE *f = fopen(path, "w");
	if (!f)
		return false;
	bool ok = fprintf(f, "%ld\n", value) > 0;
	ok = fclose(f) == 0 && ok;
	return ok;
}

static void say(struct rei_server *s, const char *text)
{
	rei_msg_push_keyed(s, "brightness", text);
}

void rei_brightness_change(struct rei_server *s, int percent)
{
	const struct rei_config *cfg = s->config;
	static struct rei_backlight last;   /* what we asked for last: the file may not show it yet (logind is asynchronous) */
	static int64_t last_ms;
	const char *dir = backlight_dir();
	struct rei_backlight b;
	if (!rei_backlight_find(dir, cfg->brightness_device, &b)) {
		static bool warned;
		if (!warned)
			rei_warn("brightness: no backlight found in %s (a desktop, or a driver without one)", dir);
		warned = true;
		say(s, "No backlight to control");
		return;
	}
	int64_t now = (int64_t)(rei_now_ns() / 1000000ull);
	if (!strcmp(last.name, b.name) && now - last_ms < 1500)
		b.cur = last.cur;   /* quick presses add up */

	int step = percent != 0 ? (percent < 0 ? -percent : percent) : cfg->brightness_step;
	bool up = percent >= 0;
	long amount = (long)lround((double)b.max * step / 100.0);
	if (amount < 1)
		amount = 1;
	if (b.cur * 5 < b.max)   /* the dim end is where the eye is most sensitive: finer steps */
		amount = amount / 4 > 0 ? amount / 4 : 1;
	long floor_v = b.max / 100 > 0 ? b.max / 100 : 1;
	long target = up ? b.cur + amount : b.cur - amount;
	target = target < floor_v ? floor_v : target > b.max ? b.max : target;
	int pct = (int)lround(100.0 * (double)target / (double)b.max);

	char cmd[900];
	bool ok = true;
	if (cfg->brightness_command && cfg->brightness_command[0]) {
		fill_command(cfg->brightness_command, &b, target, pct, cmd, sizeof cmd);
		ok = rei_msg_run_captured(s, cmd);
	} else if (!write_direct(dir, &b, target)) {
		if (!plain_name(b.name)) {
			say(s, "Cannot change the brightness");
			return;
		}
		/* Not ours to write: logind sets it for the session on the active seat. */
		snprintf(cmd, sizeof cmd,
			 "dbus-send --system --print-reply --dest=org.freedesktop.login1 /org/freedesktop/login1/session/auto "
			 "org.freedesktop.login1.Session.SetBrightness string:backlight string:%s uint32:%ld >/dev/null",
			 b.name, target);
		ok = rei_msg_run_captured(s, cmd);
	}
	if (!ok) {
		say(s, "Cannot change the brightness");
		return;
	}
	last = b;
	last.cur = target;
	last_ms = now;
	char text[40];
	snprintf(text, sizeof text, "Brightness: %d%%", pct);
	say(s, text);
}
