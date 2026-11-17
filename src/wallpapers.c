#include "wallpapers.h"

#include <dirent.h>
#include <event2/event.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "config.h"
#include "desktop.h"
#include "output.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"
#include "wallpaper.h"

#define CACHE_BYTES ((size_t)256 << 20)
#define MAX_BAD 64

struct group {
	const struct rei_wallpaper_group *cfg;
	char **files;
	size_t n_files;
	size_t index;        /* the picture shown now */
	int64_t next_ms;     /* when it moves on (a group with several pictures) */
};

struct entry {
	char *file;
	int32_t w, h;
	int mode;
	uint32_t bg;
	struct rei_pixbuf *pix;
	uint64_t used;
};

struct rei_wallpapers {
	struct rei_server *server;
	struct group *groups;
	size_t n_groups;
	struct rei_wallpaper_group legacy;   /* [wallpaper] path as a group, when there are no groups */
	char *legacy_path;
	char *legacy_paths[1];
	struct entry *cache;
	size_t n_cache, cap_cache;
	uint64_t clock;
	struct event *timer;
	char *bad[MAX_BAD];                  /* pictures that would not load (said once) */
	size_t n_bad;
};

static int64_t now_ms(void)
{
	return (int64_t)(rei_now_ns() / 1000000ull);
}

/* How long a group stays on a picture (REI_WALLPAPER_INTERVAL_MS is a test hook). */
static int64_t interval_ms(const struct group *g)
{
	const char *e = getenv("REI_WALLPAPER_INTERVAL_MS");
	return e && atoi(e) > 0 ? atoi(e) : (int64_t)g->cfg->interval * 1000;
}

/* ---- Listing ---- */

static bool is_image_name(const char *name)
{
	static const char *const exts[] = { ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif" };
	size_t n = strlen(name);
	for (size_t i = 0; i < REI_ARRAY_LEN(exts); i++) {
		size_t e = strlen(exts[i]);
		if (n > e && strcasecmp(name + n - e, exts[i]) == 0)
			return true;
	}
	return false;
}

static int cmp_names(const void *a, const void *b)
{
	return strcasecmp(*(char *const *)a, *(char *const *)b);
}

size_t rei_wallpaper_list(const char *path, char ***out)
{
	*out = NULL;
	struct stat st;
	if (stat(path, &st) != 0)
		return 0;
	if (!S_ISDIR(st.st_mode)) {
		*out = rei_xcalloc(1, sizeof **out);
		(*out)[0] = rei_xstrdup(path);
		return 1;
	}
	DIR *d = opendir(path);
	if (!d)
		return 0;
	char **list = NULL;
	size_t n = 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.' || !is_image_name(de->d_name))
			continue;
		size_t l = strlen(path) + strlen(de->d_name) + 2;
		char *full = rei_xcalloc(l, 1);
		snprintf(full, l, "%s/%s", path, de->d_name);
		struct stat fs;
		if (stat(full, &fs) != 0 || !S_ISREG(fs.st_mode)) {
			free(full);
			continue;
		}
		list = rei_xrealloc(list, (n + 1) * sizeof *list);
		list[n++] = full;
	}
	closedir(d);
	if (n > 1)
		qsort(list, n, sizeof *list, cmp_names);
	*out = list;
	return n;
}

static void group_scan(struct group *g)
{
	for (size_t i = 0; i < g->n_files; i++)
		free(g->files[i]);
	free(g->files);
	g->files = NULL;
	g->n_files = 0;
	for (size_t p = 0; p < g->cfg->n_paths; p++) {
		char **list;
		size_t n = rei_wallpaper_list(g->cfg->paths[p], &list);
		g->files = rei_xrealloc(g->files, (g->n_files + n + 1) * sizeof *g->files);
		for (size_t i = 0; i < n; i++)
			g->files[g->n_files++] = list[i];
		free(list);
		if (n == 0)
			rei_warn("wallpaper: nothing to show at '%s'", g->cfg->paths[p]);
	}
}

/* ---- Which group ---- */

static const struct rei_wallpaper_group *legacy_group(const struct rei_wallpapers *w)
{
	return w->legacy.n_paths ? &w->legacy : NULL;
}

const struct rei_wallpaper_group *rei_wallpapers_group_for(const struct rei_config *cfg, int workspace)
{
	const struct rei_wallpaper_group *any = NULL;
	for (size_t i = 0; i < cfg->n_wallpaper_groups; i++) {
		const struct rei_wallpaper_group *g = &cfg->wallpaper_groups[i];
		for (size_t k = 0; k < g->n_workspaces; k++)
			if (g->workspaces[k] == workspace)
				return g;
		if (g->any)
			any = g;
	}
	return any;
}

static struct group *runtime_for(struct rei_wallpapers *w, const struct rei_wallpaper_group *cfg)
{
	for (size_t i = 0; i < w->n_groups; i++)
		if (w->groups[i].cfg == cfg)
			return &w->groups[i];
	return NULL;
}

static void clear_bad(struct rei_wallpapers *w)
{
	for (size_t i = 0; i < w->n_bad; i++)
		free(w->bad[i]);
	w->n_bad = 0;
}

/* ---- The timer ---- */

static void on_timer(evutil_socket_t fd, short what, void *arg);

static void arm(struct rei_wallpapers *w)
{
	int64_t soonest = -1;
	for (size_t i = 0; i < w->n_groups; i++) {
		struct group *g = &w->groups[i];
		if (g->n_files > 1 && (soonest < 0 || g->next_ms < soonest))
			soonest = g->next_ms;
	}
	if (soonest < 0) {
		evtimer_del(w->timer);
		return;
	}
	int64_t wait = soonest - now_ms();
	if (wait < 50)
		wait = 50;
	struct timeval tv = { (time_t)(wait / 1000), (suseconds_t)((wait % 1000) * 1000) };
	evtimer_add(w->timer, &tv);
}

static void on_timer(evutil_socket_t fd, short what, void *arg)
{
	(void)fd; (void)what;
	struct rei_wallpapers *w = arg;
	int64_t now = now_ms();
	bool moved = false;
	clear_bad(w);   /* (a picture that would not load may have been fixed) */
	for (size_t i = 0; i < w->n_groups; i++) {
		struct group *g = &w->groups[i];
		if (g->n_files < 2 || g->next_ms > now)
			continue;
		group_scan(g);   /* a folder may have gained or lost pictures */
		if (g->n_files > 0)
			g->index = (g->index + 1) % g->n_files;
		g->next_ms = now + interval_ms(g);
		moved = true;
	}
	if (moved)
		rei_server_schedule_damage(w->server);
	arm(w);
}

/* ---- Setting up ---- */

static void free_groups(struct rei_wallpapers *w)
{
	for (size_t i = 0; i < w->n_groups; i++) {
		for (size_t k = 0; k < w->groups[i].n_files; k++)
			free(w->groups[i].files[k]);
		free(w->groups[i].files);
	}
	free(w->groups);
	w->groups = NULL;
	w->n_groups = 0;
}

static void drop_cache(struct rei_wallpapers *w)
{
	for (size_t i = 0; i < w->n_cache; i++) {
		free(w->cache[i].file);
		rei_pixbuf_unref(w->cache[i].pix);
	}
	w->n_cache = 0;
}

void rei_wallpapers_configure(struct rei_server *s)
{
	const struct rei_config *cfg = s->config;
	if (!s->wallpapers) {
		s->wallpapers = rei_xcalloc(1, sizeof *s->wallpapers);
		s->wallpapers->server = s;
		s->wallpapers->timer = evtimer_new(s->base, on_timer, s->wallpapers);
	}
	struct rei_wallpapers *w = s->wallpapers;

	/* Groups that go on from before keep their place in the cycle. */
	struct { char name[128]; size_t index; int64_t next_ms; } old[64];
	size_t n_old = 0;
	for (size_t i = 0; i < w->n_groups && n_old < REI_ARRAY_LEN(old); i++) {
		snprintf(old[n_old].name, sizeof old[0].name, "%s", w->groups[i].cfg->name ? w->groups[i].cfg->name : "");
		old[n_old].index = w->groups[i].index;
		old[n_old].next_ms = w->groups[i].next_ms;
		n_old++;
	}
	free_groups(w);
	drop_cache(w);   /* (the mode or the colour may have changed; pictures are cheap to draw again) */
	clear_bad(w);

	/* A [wallpaper] path with no groups is one single-picture "any" group. */
	memset(&w->legacy, 0, sizeof w->legacy);
	free(w->legacy_path);
	w->legacy_path = NULL;
	if (cfg->n_wallpaper_groups == 0 && cfg->wallpaper_path) {
		w->legacy_path = rei_xstrdup(cfg->wallpaper_path);
		w->legacy_paths[0] = w->legacy_path;
		w->legacy.name = (char *)"";
		w->legacy.paths = w->legacy_paths;
		w->legacy.n_paths = 1;
		w->legacy.interval = 600;
		w->legacy.any = true;
	}

	size_t n = cfg->n_wallpaper_groups + (legacy_group(w) ? 1 : 0);
	w->groups = rei_xcalloc(n ? n : 1, sizeof *w->groups);
	for (size_t i = 0; i < n; i++) {
		struct group *g = &w->groups[w->n_groups++];
		g->cfg = i < cfg->n_wallpaper_groups ? &cfg->wallpaper_groups[i] : legacy_group(w);
		group_scan(g);
		g->next_ms = now_ms() + interval_ms(g);
		for (size_t k = 0; k < n_old; k++)
			if (!strcmp(old[k].name, g->cfg->name ? g->cfg->name : "") && g->cfg->name && g->cfg->name[0]) {
				g->index = old[k].index;
				g->next_ms = old[k].next_ms;
			}
		if (g->n_files > 0 && g->index >= g->n_files)
			g->index = 0;
		if (g->n_files > 0)
			rei_info("wallpaper group '%s': %zu picture%s%s", g->cfg->name && g->cfg->name[0] ? g->cfg->name : "(unnamed)",
				 g->n_files, g->n_files == 1 ? "" : "s", g->cfg->any ? ", for any workspace" : "");
	}
	arm(w);
	if (s->damage_event)
		rei_server_schedule_damage(s);
}

void rei_wallpapers_stop(struct rei_server *s)
{
	struct rei_wallpapers *w = s->wallpapers;
	if (!w)
		return;
	s->wallpapers = NULL;
	if (w->timer)
		event_free(w->timer);
	free_groups(w);
	drop_cache(w);
	free(w->legacy_path);
	clear_bad(w);
	free(w->cache);
	free(w);
}

/* ---- Drawing ---- */

static bool is_bad(const struct rei_wallpapers *w, const char *file)
{
	for (size_t i = 0; i < w->n_bad; i++)
		if (!strcmp(w->bad[i], file))
			return true;
	return false;
}

/* Keeps the cache within its size, dropping the pictures used longest ago (never the one just used). */
static void evict(struct rei_wallpapers *w)
{
	for (;;) {
		size_t bytes = 0, oldest = (size_t)-1;
		for (size_t i = 0; i < w->n_cache; i++) {
			bytes += (size_t)w->cache[i].pix->width * (size_t)w->cache[i].pix->height * 4;
			if (w->cache[i].used != w->clock && (oldest == (size_t)-1 || w->cache[i].used < w->cache[oldest].used))
				oldest = i;
		}
		if (bytes <= CACHE_BYTES || oldest == (size_t)-1)
			return;
		free(w->cache[oldest].file);
		rei_pixbuf_unref(w->cache[oldest].pix);
		w->cache[oldest] = w->cache[--w->n_cache];
	}
}

static struct rei_pixbuf *picture(struct rei_wallpapers *w, const char *file, int32_t ow, int32_t oh)
{
	const struct rei_config *cfg = w->server->config;
	int mode = rei_wallpaper_parse_mode(cfg->wallpaper_mode);
	w->clock++;
	for (size_t i = 0; i < w->n_cache; i++) {
		struct entry *e = &w->cache[i];
		if (e->w == ow && e->h == oh && e->mode == mode && e->bg == cfg->background_rgb && !strcmp(e->file, file)) {
			e->used = w->clock;
			return e->pix;
		}
	}
	char err[256];
	struct rei_image *img = rei_image_load(file, err, sizeof err);
	if (!img) {
		rei_warn("wallpaper not loaded: %s", err);
		if (w->n_bad < MAX_BAD)
			w->bad[w->n_bad++] = rei_xstrdup(file);
		return NULL;
	}
	struct rei_pixbuf *pix = rei_wallpaper_render(img, (enum rei_wallpaper_mode)mode, cfg->background_rgb, ow, oh);
	rei_info("wallpaper: %s (%dx%d, %s)", file, img->width, img->height, cfg->wallpaper_mode);
	rei_image_free(img);
	if (!pix)
		return NULL;
	if (w->n_cache == w->cap_cache) {
		w->cap_cache = w->cap_cache ? w->cap_cache * 2 : 8;
		w->cache = rei_xrealloc(w->cache, w->cap_cache * sizeof *w->cache);
	}
	struct entry *e = &w->cache[w->n_cache++];
	*e = (struct entry){ rei_xstrdup(file), ow, oh, mode, cfg->background_rgb, pix, w->clock };
	evict(w);
	return pix;
}

struct rei_pixbuf *rei_wallpapers_pixbuf(struct rei_server *s, struct rei_output *o)
{
	struct rei_wallpapers *w = s->wallpapers;
	if (!w || o->width <= 0 || o->height <= 0)
		return NULL;
	int ws = o->workspace ? o->workspace->number : 0;
	const struct rei_wallpaper_group *cfgg = rei_wallpapers_group_for(s->config, ws);
	if (!cfgg && legacy_group(w))
		cfgg = legacy_group(w);
	if (!cfgg)
		return NULL;
	struct group *g = runtime_for(w, cfgg);
	if (!g || g->n_files == 0)
		return NULL;
	/* The current picture; one that will not load is skipped. */
	for (size_t k = 0; k < g->n_files; k++) {
		const char *file = g->files[(g->index + k) % g->n_files];
		if (is_bad(w, file))
			continue;
		struct rei_pixbuf *pix = picture(w, file, o->width, o->height);
		if (pix)
			return pix;
	}
	return NULL;
}
