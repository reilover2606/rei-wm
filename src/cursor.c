#include "cursor.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "util/log.h"
#include "util/util.h"

/* ------------------------------------------------------------------------ */
/* Shapes and the names themes know them by                                 */
/* ------------------------------------------------------------------------ */

/* The first name is the one wp_cursor_shape_v1 and CSS use; the rest are the older X11 / KDE / GNOME
 * spellings, tried in order when a theme has no file of the modern name. */
static const struct {
	const char *name;
	const char *alias[6];
} shapes[REI_SHAPE_COUNT] = {
	[REI_SHAPE_DEFAULT] = { "default", { "left_ptr", "arrow", "top_left_arrow" } },
	[REI_SHAPE_CONTEXT_MENU] = { "context-menu", { "context_menu", "left_ptr" } },
	[REI_SHAPE_HELP] = { "help", { "question_arrow", "whats_this", "left_ptr_help" } },
	[REI_SHAPE_POINTER] = { "pointer", { "hand2", "pointing_hand", "hand1", "hand" } },
	[REI_SHAPE_PROGRESS] = { "progress", { "left_ptr_watch", "half-busy", "watch" } },
	[REI_SHAPE_WAIT] = { "wait", { "watch", "clock" } },
	[REI_SHAPE_CELL] = { "cell", { "plus", "cross" } },
	[REI_SHAPE_CROSSHAIR] = { "crosshair", { "cross", "tcross" } },
	[REI_SHAPE_TEXT] = { "text", { "xterm", "ibeam" } },
	[REI_SHAPE_VERTICAL_TEXT] = { "vertical-text", { "vertical_text", "xterm" } },
	[REI_SHAPE_ALIAS] = { "alias", { "dnd-link", "link" } },
	[REI_SHAPE_COPY] = { "copy", { "dnd-copy", "dnd_copy" } },
	[REI_SHAPE_MOVE] = { "move", { "fleur", "size_all", "dnd-move" } },
	[REI_SHAPE_NO_DROP] = { "no-drop", { "dnd-no-drop", "forbidden", "crossed_circle" } },
	[REI_SHAPE_NOT_ALLOWED] = { "not-allowed", { "crossed_circle", "forbidden", "circle", "dnd-none" } },
	[REI_SHAPE_GRAB] = { "grab", { "openhand", "hand1", "fleur" } },
	[REI_SHAPE_GRABBING] = { "grabbing", { "closedhand", "dnd-move", "fleur" } },
	[REI_SHAPE_E_RESIZE] = { "e-resize", { "right_side", "sb_h_double_arrow" } },
	[REI_SHAPE_N_RESIZE] = { "n-resize", { "top_side", "sb_v_double_arrow" } },
	[REI_SHAPE_NE_RESIZE] = { "ne-resize", { "top_right_corner", "size_bdiag" } },
	[REI_SHAPE_NW_RESIZE] = { "nw-resize", { "top_left_corner", "size_fdiag" } },
	[REI_SHAPE_S_RESIZE] = { "s-resize", { "bottom_side", "sb_v_double_arrow" } },
	[REI_SHAPE_SE_RESIZE] = { "se-resize", { "bottom_right_corner", "size_fdiag" } },
	[REI_SHAPE_SW_RESIZE] = { "sw-resize", { "bottom_left_corner", "size_bdiag" } },
	[REI_SHAPE_W_RESIZE] = { "w-resize", { "left_side", "sb_h_double_arrow" } },
	[REI_SHAPE_EW_RESIZE] = { "ew-resize", { "sb_h_double_arrow", "h_double_arrow", "size_hor", "col-resize" } },
	[REI_SHAPE_NS_RESIZE] = { "ns-resize", { "sb_v_double_arrow", "v_double_arrow", "size_ver", "row-resize" } },
	[REI_SHAPE_NESW_RESIZE] = { "nesw-resize", { "size_bdiag", "fd_double_arrow", "top_right_corner" } },
	[REI_SHAPE_NWSE_RESIZE] = { "nwse-resize", { "size_fdiag", "bd_double_arrow", "top_left_corner" } },
	[REI_SHAPE_COL_RESIZE] = { "col-resize", { "sb_h_double_arrow", "split_h", "h_double_arrow" } },
	[REI_SHAPE_ROW_RESIZE] = { "row-resize", { "sb_v_double_arrow", "split_v", "v_double_arrow" } },
	[REI_SHAPE_ALL_SCROLL] = { "all-scroll", { "fleur", "size_all" } },
	[REI_SHAPE_ZOOM_IN] = { "zoom-in", { "zoom_in" } },
	[REI_SHAPE_ZOOM_OUT] = { "zoom-out", { "zoom_out" } },
	[REI_SHAPE_DND_ASK] = { "dnd-ask", { "copy", "alias" } },
	[REI_SHAPE_ALL_RESIZE] = { "all-resize", { "move", "fleur", "size_all" } },
};

const char *rei_cursor_shape_name(enum rei_cursor_shape shape)
{
	return shape > REI_SHAPE_NONE && shape < REI_SHAPE_COUNT ? shapes[shape].name : NULL;
}

enum rei_cursor_shape rei_cursor_shape_from_name(const char *name)
{
	for (int i = REI_SHAPE_DEFAULT; i < REI_SHAPE_COUNT; i++)
		if (name && !strcmp(shapes[i].name, name))
			return (enum rei_cursor_shape)i;
	return REI_SHAPE_NONE;
}

/* ------------------------------------------------------------------------ */
/* The XCursor file format                                                  */
/* ------------------------------------------------------------------------ */

#define XCUR_MAGIC 0x72756358u   /* "Xcur" */
#define XCUR_IMAGE_TYPE 0xfffd0002u
#define XCUR_MAX_DIM 512u

static bool rd32(const uint8_t *d, size_t len, size_t off, uint32_t *out)
{
	if (off > len || len - off < 4)
		return false;
	*out = (uint32_t)d[off] | (uint32_t)d[off + 1] << 8 | (uint32_t)d[off + 2] << 16 | (uint32_t)d[off + 3] << 24;
	return true;
}

/* One image chunk at `off`. */
static struct rei_pixbuf *parse_chunk(const uint8_t *data, size_t len, size_t off, int32_t *hot_x, int32_t *hot_y,
				      uint32_t *delay)
{
	uint32_t chdr, ctype, csub, cver, w, h, xh, yh;
	if (!rd32(data, len, off, &chdr) || !rd32(data, len, off + 4, &ctype) || !rd32(data, len, off + 8, &csub) ||
	    !rd32(data, len, off + 12, &cver) || !rd32(data, len, off + 16, &w) || !rd32(data, len, off + 20, &h) ||
	    !rd32(data, len, off + 24, &xh) || !rd32(data, len, off + 28, &yh) || !rd32(data, len, off + 32, delay))
		return NULL;
	if (ctype != XCUR_IMAGE_TYPE || w == 0 || h == 0 || w > XCUR_MAX_DIM || h > XCUR_MAX_DIM || chdr < 36)
		return NULL;
	size_t pix_off = off + chdr;
	size_t n = (size_t)w * h;
	if (pix_off > len || (len - pix_off) / 4 < n)
		return NULL;
	struct rei_pixbuf *pb = rei_pixbuf_create_uninit((int32_t)w, (int32_t)h, false);
	if (!pb)
		return NULL;
	/* The file holds premultiplied ARGB, little-endian: in memory that is B, G, R, A, which is also what a
	 * pixbuf holds. A damaged file with a colour above its alpha is clamped, not drawn as a glitch. */
	memcpy(pb->data, data + pix_off, n * 4);
	for (size_t i = 0; i < n; i++) {
		uint8_t *p = pb->data + i * 4;
		for (int c = 0; c < 3; c++)
			if (p[c] > p[3])
				p[c] = p[3];
	}
	*hot_x = (int32_t)(xh < w ? xh : w - 1);
	*hot_y = (int32_t)(yh < h ? yh : h - 1);
	return pb;
}

int rei_xcursor_parse_frames(const uint8_t *data, size_t len, int want_size, int *nominal, struct rei_pixbuf **pix,
			     int32_t *hot_x, int32_t *hot_y, uint32_t *delay_ms, int max)
{
	uint32_t magic, header, version, ntoc;
	if (!rd32(data, len, 0, &magic) || magic != XCUR_MAGIC || !rd32(data, len, 4, &header) ||
	    !rd32(data, len, 8, &version) || !rd32(data, len, 12, &ntoc) || header < 16 || ntoc > 4096)
		return 0;

	/* The nominal size closest to the one wanted (a file has several sizes). */
	uint32_t best_size = 0;
	long best_diff = -1;
	for (uint32_t i = 0; i < ntoc; i++) {
		size_t e = header + (size_t)i * 12;
		uint32_t type, subtype, pos;
		if (!rd32(data, len, e, &type) || !rd32(data, len, e + 4, &subtype) || !rd32(data, len, e + 8, &pos))
			return 0;
		if (type != XCUR_IMAGE_TYPE || subtype == 0)
			continue;
		long diff = labs((long)subtype - (long)want_size);
		if (best_diff < 0 || diff < best_diff) {
			best_diff = diff;
			best_size = subtype;
		}
	}
	if (best_diff < 0)
		return 0;

	/* Its images in file order: more than one is an animation. */
	int n = 0;
	for (uint32_t i = 0; i < ntoc && n < max; i++) {
		size_t e = header + (size_t)i * 12;
		uint32_t type, subtype, pos;
		if (!rd32(data, len, e, &type) || !rd32(data, len, e + 4, &subtype) || !rd32(data, len, e + 8, &pos))
			break;
		if (type != XCUR_IMAGE_TYPE || subtype != best_size)
			continue;
		pix[n] = parse_chunk(data, len, pos, &hot_x[n], &hot_y[n], &delay_ms[n]);
		if (!pix[n]) {
			if (n == 0)
				return 0;   /* the first image is damaged: the file is no good */
			break;          /* later frames missing: play what there is */
		}
		n++;
	}
	*nominal = (int)best_size;
	return n;
}

struct rei_pixbuf *rei_xcursor_parse(const uint8_t *data, size_t len, int want_size, int *nominal,
				     int32_t *hot_x, int32_t *hot_y)
{
	struct rei_pixbuf *pix;
	uint32_t delay;
	return rei_xcursor_parse_frames(data, len, want_size, nominal, &pix, hot_x, hot_y, &delay, 1) == 1 ? pix : NULL;
}

/* ------------------------------------------------------------------------ */
/* Where themes live                                                        */
/* ------------------------------------------------------------------------ */

#define DEFAULT_PATH                                                                               \
	"~/.local/share/icons:~/.icons:/usr/share/icons:/usr/share/pixmaps:/usr/share/cursors:"    \
	"/usr/share/cursors/xorg-x11"

char *rei_cursor_default_path(void)
{
	return rei_xstrdup(DEFAULT_PATH);
}

char *rei_cursor_search_path(void)
{
	const char *e = getenv("XCURSOR_PATH");
	return rei_xstrdup(e && e[0] ? e : DEFAULT_PATH);
}

/* Splits a colon-separated path into directories, expanding a leading "~/". */
static char **split_path(const char *path, size_t *count)
{
	char **out = NULL;
	size_t n = 0;
	const char *home = getenv("HOME");
	const char *p = path;
	while (*p) {
		const char *end = strchr(p, ':');
		size_t l = end ? (size_t)(end - p) : strlen(p);
		if (l > 0) {
			char *d;
			if (p[0] == '~' && (l == 1 || p[1] == '/')) {
				if (home && home[0]) {
					d = rei_xcalloc(strlen(home) + l + 1, 1);
					strcpy(d, home);
					memcpy(d + strlen(home), p + 1, l - 1);
				} else {
					d = NULL;
				}
			} else {
				d = rei_xstrndup(p, l);
			}
			if (d) {
				out = rei_xrealloc(out, (n + 1) * sizeof *out);
				out[n++] = d;
			}
		}
		if (!end)
			break;
		p = end + 1;
	}
	*count = n;
	return out;
}

static void free_list(char **l, size_t n)
{
	for (size_t i = 0; i < n; i++)
		free(l[i]);
	free(l);
}

static char *join(const char *a, const char *b, const char *c)
{
	size_t l = strlen(a) + strlen(b) + (c ? strlen(c) : 0) + 3;
	char *s = rei_xcalloc(l, 1);
	snprintf(s, l, "%s/%s%s%s", a, b, c ? "/" : "", c ? c : "");
	return s;
}

static bool is_dir(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static uint8_t *read_file(const char *path, size_t max, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	uint8_t *buf = NULL;
	size_t n = 0, cap = 0;
	for (;;) {
		if (n == cap) {
			if (cap >= max) {
				free(buf);
				fclose(f);
				return NULL;
			}
			cap = cap ? cap * 2 : 16384;
			buf = rei_xrealloc(buf, cap);
		}
		size_t r = fread(buf + n, 1, cap - n, f);
		if (r == 0)
			break;
		n += r;
	}
	fclose(f);
	*len = n;
	return buf;
}

/* "Inherits=a,b;c" of a theme's index.theme, as a list of names. */
static char **read_inherits(const char *index_path, size_t *count)
{
	*count = 0;
	size_t len;
	uint8_t *raw = read_file(index_path, 1 << 20, &len);
	if (!raw)
		return NULL;
	char *text = rei_xcalloc(len + 1, 1);
	memcpy(text, raw, len);
	free(raw);

	char **out = NULL;
	size_t n = 0;
	for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
		while (*line == ' ' || *line == '\t')
			line++;
		if (strncmp(line, "Inherits", 8) != 0)
			continue;
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		for (char *tok = strtok(eq + 1, ",; \t\r"); tok; tok = strtok(NULL, ",; \t\r")) {
			out = rei_xrealloc(out, (n + 1) * sizeof *out);
			out[n++] = rei_xstrdup(tok);
		}
		break;
	}
	free(text);
	*count = n;
	return out;
}

struct dir_list {
	char **dirs;
	size_t n;
};

/* Adds the `cursors` directories of a theme and, after them, of the themes it inherits. */
static void collect_dirs(char **path, size_t n_path, const char *name, struct dir_list *out, int depth)
{
	if (depth > 8 || !name[0] || strchr(name, '/'))
		return;
	char **inh = NULL;
	size_t n_inh = 0;
	for (size_t i = 0; i < n_path; i++) {
		char *theme = join(path[i], name, NULL);
		char *cur = join(theme, "cursors", NULL);
		if (is_dir(cur)) {
			bool dup = false;
			for (size_t k = 0; k < out->n; k++)
				dup |= !strcmp(out->dirs[k], cur);
			if (!dup) {
				out->dirs = rei_xrealloc(out->dirs, (out->n + 1) * sizeof *out->dirs);
				out->dirs[out->n++] = cur;
				cur = NULL;
			}
		}
		free(cur);
		if (!inh) {
			char *idx = join(theme, "index.theme", NULL);
			inh = read_inherits(idx, &n_inh);
			free(idx);
		}
		free(theme);
	}
	for (size_t i = 0; i < n_inh; i++)
		collect_dirs(path, n_path, inh[i], out, depth + 1);
	free_list(inh, n_inh);
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

char **rei_cursor_list_themes(size_t *count)
{
	char *p = rei_cursor_search_path();
	size_t n_path;
	char **path = split_path(p, &n_path);
	free(p);
	char **out = NULL;
	size_t n = 0;
	for (size_t i = 0; i < n_path; i++) {
		DIR *d = opendir(path[i]);
		if (!d)
			continue;
		struct dirent *de;
		while ((de = readdir(d))) {
			if (de->d_name[0] == '.')
				continue;
			char *theme = join(path[i], de->d_name, "cursors");
			bool ok = is_dir(theme);
			free(theme);
			if (!ok)
				continue;
			out = rei_xrealloc(out, (n + 1) * sizeof *out);
			out[n++] = rei_xstrdup(de->d_name);
		}
		closedir(d);
	}
	free_list(path, n_path);
	if (n > 1) {
		qsort(out, n, sizeof *out, cmp_str);
		size_t w = 1;
		for (size_t i = 1; i < n; i++) {
			if (!strcmp(out[i], out[w - 1]))
				free(out[i]);
			else
				out[w++] = out[i];
		}
		n = w;
	}
	*count = n;
	return out;
}

void rei_cursor_free_names(char **names, size_t count)
{
	free_list(names, count);
}

/* ------------------------------------------------------------------------ */
/* The built-in arrow                                                       */
/* ------------------------------------------------------------------------ */

/* 'X' = black outline, '.' = white fill, ' ' = transparent. */
static const char *const arrow[] = {
	"X           ",
	"XX          ",
	"X.X         ",
	"X..X        ",
	"X...X       ",
	"X....X      ",
	"X.....X     ",
	"X......X    ",
	"X.......X   ",
	"X........X  ",
	"X.....XXXXX ",
	"X..X..X     ",
	"X.X X..X    ",
	"XX  X..X    ",
	"X    X..X   ",
	"     X..X   ",
	"      X..X  ",
	"      X..X  ",
	"       XX   ",
};
#define ARROW_W 12
#define ARROW_H ((int)(sizeof arrow / sizeof arrow[0]))

static struct rei_cursor_image builtin_image;

static const struct rei_cursor_image *builtin_arrow(void)
{
	if (builtin_image.pixbuf)
		return &builtin_image;
	struct rei_pixbuf *pb = rei_pixbuf_create(ARROW_W, ARROW_H, false);
	if (!pb)
		abort();
	for (int y = 0; y < ARROW_H; y++) {
		for (int x = 0; x < ARROW_W; x++) {
			uint8_t *p = pb->data + ((size_t)y * ARROW_W + (size_t)x) * 4;
			char c = arrow[y][x];
			uint8_t v = (c == '.') ? 255 : 0;
			if (c == 'X' || c == '.') {
				p[0] = p[1] = p[2] = v;  /* B, G, R */
				p[3] = 255;
			}
		}
	}
	builtin_image.pixbuf = pb;
	builtin_image.width = ARROW_W;
	builtin_image.height = ARROW_H;
	return &builtin_image;
}

/* ------------------------------------------------------------------------ */
/* A theme                                                                  */
/* ------------------------------------------------------------------------ */

struct rei_cursor_theme {
	char name[96];
	struct dir_list dirs;
	int size, pixel_scale;
	struct rei_cursor_image img[REI_SHAPE_COUNT];
	struct rei_cursor_image *frames[REI_SHAPE_COUNT];   /* the ring behind img[], when this theme made it */
	int n_frames[REI_SHAPE_COUNT];
	bool tried[REI_SHAPE_COUNT];
};

#define MAX_FRAMES 256

/* Loads a cursor file as a ring of frames: *out_frames, *out_n. */
static bool load_file(struct rei_cursor_theme *t, const char *dir, const char *file,
		      struct rei_cursor_image **out_frames, int *out_n)
{
	char *path = join(dir, file, NULL);
	size_t len;
	uint8_t *raw = read_file(path, 8u << 20, &len);
	free(path);
	if (!raw)
		return false;
	int want = t->size * t->pixel_scale, nominal = 0;
	struct rei_pixbuf *pix[MAX_FRAMES];
	int32_t hx[MAX_FRAMES], hy[MAX_FRAMES];
	uint32_t delay[MAX_FRAMES];
	int n = rei_xcursor_parse_frames(raw, len, want, &nominal, pix, hx, hy, delay, MAX_FRAMES);
	free(raw);
	if (n < 1)
		return false;
	/* The picture is for `nominal` pixels and is to look `size` big: scale its logical size to match. */
	double k = (double)want / nominal / t->pixel_scale;
	struct rei_cursor_image *f = rei_xcalloc((size_t)n, sizeof *f);
	for (int i = 0; i < n; i++) {
		f[i].pixbuf = pix[i];
		f[i].width = (int32_t)fmax(1.0, lround(pix[i]->width * k));
		f[i].height = (int32_t)fmax(1.0, lround(pix[i]->height * k));
		f[i].hotspot_x = (int32_t)lround(hx[i] * k);
		f[i].hotspot_y = (int32_t)lround(hy[i] * k);
		f[i].delay_ms = delay[i] < 10 ? 50 : delay[i];   /* a missing delay would spin */
		f[i].next = n > 1 ? &f[(i + 1) % n] : NULL;
	}
	*out_frames = f;
	*out_n = n;
	return true;
}

static bool load_shape(struct rei_cursor_theme *t, enum rei_cursor_shape shape, struct rei_cursor_image **out, int *n)
{
	for (size_t d = 0; d < t->dirs.n; d++) {
		if (load_file(t, t->dirs.dirs[d], shapes[shape].name, out, n))
			return true;
		for (int a = 0; a < 6 && shapes[shape].alias[a]; a++)
			if (load_file(t, t->dirs.dirs[d], shapes[shape].alias[a], out, n))
				return true;
	}
	return false;
}

static bool theme_dirs(const char *name, struct dir_list *out)
{
	char *p = rei_cursor_search_path();
	size_t n_path;
	char **path = split_path(p, &n_path);
	free(p);
	out->dirs = NULL;
	out->n = 0;
	collect_dirs(path, n_path, name, out, 0);
	free_list(path, n_path);
	return out->n > 0;
}

struct rei_cursor_theme *rei_cursor_theme_open(const char *name, int size, int pixel_scale)
{
	struct rei_cursor_theme *t = rei_xcalloc(1, sizeof *t);
	t->size = size < 8 ? 8 : size > 256 ? 256 : size;
	t->pixel_scale = pixel_scale < 1 ? 1 : pixel_scale > 4 ? 4 : pixel_scale;

	const char *env = getenv("XCURSOR_THEME");
	const char *wanted[] = { name && name[0] ? name : NULL, name && name[0] ? NULL : env, "default", "Adwaita", NULL };
	bool found = false;
	for (int i = 0; wanted[i] || i < 4; i++) {
		if (!wanted[i])
			continue;
		if (theme_dirs(wanted[i], &t->dirs)) {
			snprintf(t->name, sizeof t->name, "%s", wanted[i]);
			found = true;
			break;
		}
		free_list(t->dirs.dirs, t->dirs.n);
		t->dirs = (struct dir_list){ 0 };
		/* A theme the user named that is not installed: say so, then use the usual ones. */
		if (name && name[0] && i == 0)
			rei_warn("cursor: theme '%s' not found in %s; using another", name, DEFAULT_PATH);
	}
	if (!found) {
		size_t n;
		char **all = rei_cursor_list_themes(&n);
		if (n > 0 && theme_dirs(all[0], &t->dirs)) {
			snprintf(t->name, sizeof t->name, "%s", all[0]);
			found = true;
		}
		rei_cursor_free_names(all, n);
	}
	if (!found)
		rei_warn("cursor: no cursor theme found; using the built-in arrow");
	return t;
}

void rei_cursor_theme_close(struct rei_cursor_theme *t)
{
	if (!t)
		return;
	for (int i = 0; i < REI_SHAPE_COUNT; i++) {
		for (int k = 0; k < t->n_frames[i]; k++)
			rei_pixbuf_unref(t->frames[i][k].pixbuf);
		free(t->frames[i]);
	}
	free_list(t->dirs.dirs, t->dirs.n);
	free(t);
}

const char *rei_cursor_theme_name(const struct rei_cursor_theme *t)
{
	return t->name;
}

const struct rei_cursor_image *rei_cursor_theme_shape(struct rei_cursor_theme *t, enum rei_cursor_shape shape)
{
	if (shape <= REI_SHAPE_NONE || shape >= REI_SHAPE_COUNT)
		shape = REI_SHAPE_DEFAULT;
	if (!t->tried[shape]) {
		t->tried[shape] = true;
		if (load_shape(t, shape, &t->frames[shape], &t->n_frames[shape])) {
			t->img[shape] = t->frames[shape][0];
		} else if (shape != REI_SHAPE_DEFAULT) {
			/* The theme lacks it: the default arrow stands in (shared; the default owns it). */
			t->img[shape] = *rei_cursor_theme_shape(t, REI_SHAPE_DEFAULT);
		} else {
			t->img[shape] = *builtin_arrow();
		}
	}
	return &t->img[shape];
}
