#include "icons.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "toolbar.h"
#include "util/log.h"
#include "util/util.h"
#include "wallpaper.h"

/* ------------------------------------------------------------------------ */
/* Desktop entries                                                          */
/* ------------------------------------------------------------------------ */

struct entry {
	char *id;        /* "org.mozilla.firefox" (file name without .desktop) */
	char *wmclass;   /* StartupWMClass */
	char *exec_base; /* "firefox" from Exec=/usr/bin/firefox %u */
	char *name, *icon;
};

static struct entry *entries;
static size_t n_entries, cap_entries;
static bool indexed;

static char *trim_dup(const char *s, size_t n)
{
	while (n && isspace((unsigned char)*s)) {
		s++;
		n--;
	}
	while (n && isspace((unsigned char)s[n - 1]))
		n--;
	char *out = rei_xcalloc(n + 1, 1);
	memcpy(out, s, n);
	return out;
}

static void add_entry(const char *dir, const char *file)
{
	char path[1024];
	snprintf(path, sizeof path, "%s/%s", dir, file);
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	struct entry e = { 0 };
	e.id = trim_dup(file, strlen(file) - strlen(".desktop"));
	bool in_main = false;
	char line[1024];
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '[') {
			in_main = strncmp(line, "[Desktop Entry]", 15) == 0;
			continue;
		}
		if (!in_main)
			continue;
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		size_t klen = (size_t)(eq - line);
		const char *val = eq + 1;
		size_t vlen = strlen(val);
		if (klen == 4 && !strncmp(line, "Name", 4) && !e.name) {
			e.name = trim_dup(val, vlen);
		} else if (klen == 4 && !strncmp(line, "Icon", 4)) {
			free(e.icon);
			e.icon = trim_dup(val, vlen);
		} else if (klen == 14 && !strncmp(line, "StartupWMClass", 14)) {
			e.wmclass = trim_dup(val, vlen);
		} else if (klen == 4 && !strncmp(line, "Exec", 4)) {
			/* The program, skipping "env" and VAR=value prefixes: "env FOO=1 /usr/bin/x %u" is x. */
			const char *p = val;
			for (;;) {
				while (isspace((unsigned char)*p))
					p++;
				const char *end = p;
				while (*end && !isspace((unsigned char)*end))
					end++;
				bool wrapper = (end - p == 3 && !strncmp(p, "env", 3)) || memchr(p, '=', (size_t)(end - p));
				if (wrapper && *end) {
					p = end;
					continue;
				}
				const char *slash = p;
				for (const char *q = p; q < end; q++)
					if (*q == '/')
						slash = q + 1;
				free(e.exec_base);
				e.exec_base = trim_dup(slash, (size_t)(end - slash));
				break;
			}
		}
	}
	fclose(f);
	if (!e.icon) {
		free(e.id);
		free(e.wmclass);
		free(e.exec_base);
		free(e.name);
		return;
	}
	if (n_entries == cap_entries) {
		cap_entries = cap_entries ? cap_entries * 2 : 128;
		entries = rei_xrealloc(entries, cap_entries * sizeof *entries);
	}
	entries[n_entries++] = e;
}

/* The directories that hold applications/ and icons/: $XDG_DATA_HOME and $XDG_DATA_DIRS. */
static size_t data_dirs(char out[][512], size_t cap)
{
	size_t n = 0;
	const char *home = getenv("XDG_DATA_HOME");
	const char *h = getenv("HOME");
	if (home && home[0]) {
		snprintf(out[n++], 512, "%s", home);
	} else if (h && h[0]) {
		snprintf(out[n++], 512, "%s/.local/share", h);
	}
	const char *dirs = getenv("XDG_DATA_DIRS");
	if (!dirs || !dirs[0])
		dirs = "/usr/local/share:/usr/share";
	char *copy = rei_xstrdup(dirs), *save = NULL;
	for (char *d = strtok_r(copy, ":", &save); d && n < cap; d = strtok_r(NULL, ":", &save))
		snprintf(out[n++], 512, "%s", d);
	free(copy);
	return n;
}

static void build_index(void)
{
	indexed = true;
	char dirs[16][512];
	size_t n = data_dirs(dirs, 16);
	for (size_t i = 0; i < n; i++) {
		char apps[600];
		snprintf(apps, sizeof apps, "%s/applications", dirs[i]);
		DIR *d = opendir(apps);
		if (!d)
			continue;
		struct dirent *e;
		while ((e = readdir(d))) {
			size_t len = strlen(e->d_name);
			if (len > 8 && strcmp(e->d_name + len - 8, ".desktop") == 0)
				add_entry(apps, e->d_name);
		}
		closedir(d);
	}
}

static const struct entry *find_entry(const char *app_id)
{
	if (!app_id || !app_id[0])
		return NULL;
	if (!indexed)
		build_index();
	for (size_t i = 0; i < n_entries; i++) {
		const struct entry *e = &entries[i];
		const char *last = strrchr(e->id, '.');  /* org.gnome.Nautilus -> Nautilus */
		if (!strcasecmp(e->id, app_id) || (e->wmclass && !strcasecmp(e->wmclass, app_id)) ||
		    (last && !strcasecmp(last + 1, app_id)))
			return e;
	}
	for (size_t i = 0; i < n_entries; i++) {  /* A weaker match: the program it runs. */
		const struct entry *e = &entries[i];
		if (e->exec_base && !strcasecmp(e->exec_base, app_id))
			return e;
	}
	return NULL;
}

/* ------------------------------------------------------------------------ */
/* Icon files                                                               */
/* ------------------------------------------------------------------------ */

static struct rei_pixbuf *load_png(const char *path)
{
	char err[128];
	struct rei_image *img = rei_image_load(path, err, sizeof err);
	if (!img)
		return NULL;
	struct rei_pixbuf *pb = rei_pixbuf_create(img->width, img->height, false);
	if (pb) {
		/* RGBA straight -> BGRA premultiplied. */
		for (int32_t i = 0; i < img->width * img->height; i++) {
			const uint8_t *s = img->rgba + (size_t)i * 4;
			uint8_t *d = pb->data + (size_t)i * 4;
			d[0] = (uint8_t)(s[2] * s[3] / 255);
			d[1] = (uint8_t)(s[1] * s[3] / 255);
			d[2] = (uint8_t)(s[0] * s[3] / 255);
			d[3] = s[3];
		}
	}
	rei_image_free(img);
	return pb;
}

static bool exists(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static struct rei_pixbuf *find_themed(const char *name)
{
	static const char *const sizes[] = { "48x48", "64x64", "128x128", "256x256", "32x32", "24x24",
					     "22x22", "16x16", "512x512" };
	static const char *const first_themes[] = { "hicolor", "Adwaita", "breeze", "Papirus", "gnome" };
	char dirs[16][512];
	size_t nd = data_dirs(dirs, 16);
	char path[1400];

	for (size_t i = 0; i < nd; i++) {
		/* The well-known themes first, in order, then any other the system has. */
		char icons[600];
		snprintf(icons, sizeof icons, "%s/icons", dirs[i]);
		for (size_t t = 0; t < REI_ARRAY_LEN(first_themes); t++) {
			for (size_t z = 0; z < REI_ARRAY_LEN(sizes); z++) {
				snprintf(path, sizeof path, "%s/%s/%s/apps/%s.png", icons, first_themes[t], sizes[z], name);
				if (exists(path))
					return load_png(path);
			}
		}
		DIR *d = opendir(icons);
		while (d) {
			struct dirent *e = readdir(d);
			if (!e)
				break;
			if (e->d_name[0] == '.')
				continue;
			for (size_t z = 0; z < REI_ARRAY_LEN(sizes); z++) {
				snprintf(path, sizeof path, "%s/%s/%s/apps/%s.png", icons, e->d_name, sizes[z], name);
				if (exists(path)) {
					closedir(d);
					return load_png(path);
				}
			}
		}
		if (d)
			closedir(d);
		snprintf(path, sizeof path, "%s/pixmaps/%s.png", dirs[i], name);
		if (exists(path))
			return load_png(path);
	}
	return NULL;
}

static struct rei_pixbuf *load_icon_named(const char *icon)
{
	if (!icon || !icon[0])
		return NULL;
	if (icon[0] == '/')
		return exists(icon) && strstr(icon, ".svg") == NULL ? load_png(icon) : NULL;
	return find_themed(icon);
}

/* ------------------------------------------------------------------------ */
/* Cache                                                                    */
/* ------------------------------------------------------------------------ */

struct cached {
	char *key;
	struct rei_pixbuf *icon;
};
static struct cached *cache;
static size_t n_cache, cap_cache;

struct rei_pixbuf *rei_icon_for_app(const char *app_id, const char *fallback_name)
{
	const char *key = (app_id && app_id[0]) ? app_id : (fallback_name ? fallback_name : "?");
	for (size_t i = 0; i < n_cache; i++)
		if (strcmp(cache[i].key, key) == 0)
			return cache[i].icon;

	struct rei_pixbuf *icon = NULL;
	const struct entry *e = find_entry(app_id);
	if (e)
		icon = load_icon_named(e->icon);
	if (!icon && app_id && app_id[0]) {
		/* No desktop entry: many applications' ids are their icon's name. */
		char lower[128];
		snprintf(lower, sizeof lower, "%s", app_id);
		for (char *p = lower; *p; p++)
			*p = (char)tolower((unsigned char)*p);
		icon = load_icon_named(app_id);
		if (!icon && strcmp(lower, app_id) != 0)
			icon = load_icon_named(lower);
	}
	if (!icon)
		icon = rei_bar_letter_icon(e && e->name ? e->name : key);

	if (n_cache == cap_cache) {
		cap_cache = cap_cache ? cap_cache * 2 : 32;
		cache = rei_xrealloc(cache, cap_cache * sizeof *cache);
	}
	cache[n_cache++] = (struct cached){ rei_xstrdup(key), icon };
	return icon;
}

const char *rei_icon_app_name(const char *app_id)
{
	const struct entry *e = find_entry(app_id);
	return e ? e->name : NULL;
}

/* ------------------------------------------------------------------------ */
/* Flavor icons                                                             */
/* ------------------------------------------------------------------------ */

static struct rei_pixbuf **flavor, **flavor_all;
static size_t n_flavor, n_flavor_all;
static char *flavor_dir;
static uint32_t flavor_seed;

int rei_flavor_icons_count(void)
{
	return (int)n_flavor;
}

int rei_flavor_icons_configure(const char *dir)
{
	if (!dir || !dir[0]) {
		n_flavor = 0;   /* (what was loaded stays alive: a drawn bar may still point at it) */
		free(flavor_dir);
		flavor_dir = NULL;
		return 0;
	}
	if (flavor_dir && strcmp(flavor_dir, dir) == 0)
		return (int)n_flavor;
	free(flavor_dir);
	flavor_dir = rei_xstrdup(dir);
	if (!flavor_seed) {
		flavor_seed = (uint32_t)time(NULL) * 2654435761u ^ (uint32_t)getpid();
		if (!flavor_seed)
			flavor_seed = 1;
	}

	char expanded[1024];
	const char *home = getenv("HOME");
	if (dir[0] == '~' && (dir[1] == '/' || dir[1] == '\0') && home)
		snprintf(expanded, sizeof expanded, "%s%s", home, dir + 1);
	else
		snprintf(expanded, sizeof expanded, "%s", dir);

	n_flavor = 0;
	free(flavor);
	flavor = NULL;
	DIR *d = opendir(expanded);
	if (!d) {
		rei_warn("flavor icons: cannot read %s", expanded);
		return 0;
	}
	/* Sorted names, so which icon a key gets does not depend on directory order. */
	char **names = NULL;
	size_t nn = 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		size_t len = strlen(de->d_name);
		if (len > 4 && strcasecmp(de->d_name + len - 4, ".png") == 0) {
			names = rei_xrealloc(names, (nn + 1) * sizeof *names);
			names[nn++] = rei_xstrdup(de->d_name);
		}
	}
	closedir(d);
	for (size_t i = 0; i < nn; i++)
		for (size_t j = i + 1; j < nn; j++)
			if (strcmp(names[j], names[i]) < 0) {
				char *t = names[i];
				names[i] = names[j];
				names[j] = t;
			}

	struct rei_pixbuf **set = rei_xcalloc(nn ? nn : 1, sizeof *set);
	size_t ns = 0;
	for (size_t i = 0; i < nn; i++) {
		char path[1400];
		snprintf(path, sizeof path, "%s/%s", expanded, names[i]);
		struct rei_pixbuf *pb = load_png(path);
		if (!pb) {
			rei_warn("flavor icons: %s: not a readable PNG; skipped", names[i]);
		} else if (pb->width != pb->height) {
			rei_warn("flavor icons: %s: %dx%d is not square (1:1); skipped", names[i], pb->width, pb->height);
			rei_pixbuf_unref(pb);
		} else if (pb->width > REI_ICON_MAX_SIZE) {
			rei_warn("flavor icons: %s: %dx%d is larger than %dx%d; skipped", names[i], pb->width, pb->height,
				 REI_ICON_MAX_SIZE, REI_ICON_MAX_SIZE);
			rei_pixbuf_unref(pb);
		} else {
			set[ns++] = pb;
		}
		free(names[i]);
	}
	free(names);
	/* Kept until exit even if the directory changes: a bar already drawn may point at them. */
	flavor_all = rei_xrealloc(flavor_all, (n_flavor_all + ns + 1) * sizeof *flavor_all);
	for (size_t i = 0; i < ns; i++)
		flavor_all[n_flavor_all++] = set[i];
	flavor = set;
	n_flavor = ns;
	rei_info("flavor icons: %zu usable from %s", ns, expanded);
	return (int)ns;
}

struct rei_pixbuf *rei_flavor_icon(const char *key)
{
	if (n_flavor == 0)
		return NULL;
	uint32_t h = 2166136261u ^ flavor_seed;
	for (const char *p = key; p && *p; p++)
		h = (h ^ (uint8_t)*p) * 16777619u;
	h ^= h >> 15;
	return flavor[h % n_flavor];
}

void rei_icons_cleanup(void)
{
	for (size_t i = 0; i < n_flavor_all; i++)
		rei_pixbuf_unref(flavor_all[i]);
	free(flavor_all);
	free(flavor);
	free(flavor_dir);
	flavor_all = flavor = NULL;
	flavor_dir = NULL;
	n_flavor = n_flavor_all = 0;

	for (size_t i = 0; i < n_cache; i++) {
		free(cache[i].key);
		rei_pixbuf_unref(cache[i].icon);
	}
	free(cache);
	cache = NULL;
	n_cache = cap_cache = 0;
	for (size_t i = 0; i < n_entries; i++) {
		free(entries[i].id);
		free(entries[i].wmclass);
		free(entries[i].exec_base);
		free(entries[i].name);
		free(entries[i].icon);
	}
	free(entries);
	entries = NULL;
	n_entries = cap_entries = 0;
	indexed = false;
}
