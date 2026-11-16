/*
 * Text effects drawn by fragment shaders, end to end in the headless compositor:
 *   - the toolbar's text with a glow, with a metallic shine over its upper half, and blinking;
 *   - a window shader on the windows of one application ([theme.window_text]): the built-in glow around
 *     bright pixels, a custom GLSL file written the way picom's are, a file that does not compile, and
 *     the --window-shader-fg-rule option;
 *   - all of it following a live config change.
 */
#define SOCKET "rei-textfx-test"
#include "testlib.h"

#include <signal.h>

static const char *g_base =
	"[toolbar]\nenabled = true\nclock_format = \"CLOCK\"\nbattery = false\nsysinfo = false\n"
	"background = \"#101010\"\nforeground = \"#e0e0e0\"\n[start_menu]\nenabled = false\n[audio]\nenabled = false\n"
	"[wallpaper]\ncolor = \"#000000\"\n[[output]]\nname = \"OUT\"\nwidth = 800\nheight = 600\n";

/* Replaces the config with the base plus `extra` and waits until rei-wm has taken it (or refused it). */
static void set_config(const char *extra, bool expect_ok)
{
	char text[4000], path[300], tmp[320];
	snprintf(text, sizeof text, "%s%s", g_base, extra);
	snprintf(path, sizeof path, "%s/config.toml", g_dir);
	snprintf(tmp, sizeof tmp, "%s.new", path);
	write_text(tmp, text);
	int ok = log_count("config reloaded"), bad = log_count("config not reloaded");
	CHECK(rename(tmp, path) == 0);
	CHECK(kill(g_pid, SIGHUP) == 0);
	for (int t = 0;; t += 20) {
		CHECK(t < 5000);
		if (log_count("config reloaded") > ok) {
			CHECK(expect_ok);
			break;
		}
		if (log_count("config not reloaded") > bad) {
			CHECK(!expect_ok);
			break;
		}
		sleep_ms(20);
	}
	settle_all();
}

static const uint8_t *px(const struct ppm *img, int x, int y)
{
	return img->rgb + ((size_t)y * (size_t)img->w + (size_t)x) * 3;
}

/* The clock's corner of the bar. */
#define CX0 700
#define CX1 800
#define CY0 0
#define CY1 28

struct stats {
	int lit;        /* pixels clearly brighter than the bar */
	int red;        /* red-dominant pixels */
	int bright_upper, bright_lower;   /* near-white pixels above / below the middle of the text */
	int top, bottom;                  /* rows the text spans */
};

static struct stats clock_stats(const struct ppm *img)
{
	struct stats s = { 0, 0, 0, 0, 999, -1 };
	for (int y = CY0; y < CY1; y++)
		for (int x = CX0; x < CX1; x++) {
			const uint8_t *p = px(img, x, y);
			int lum = p[0] + p[1] + p[2];
			if (lum > 0x10 * 3 + 90) {
				s.lit++;
				s.top = y < s.top ? y : s.top;
				s.bottom = y > s.bottom ? y : s.bottom;
			}
			if (p[0] > p[1] + 50 && p[0] > p[2] + 50)
				s.red++;
		}
	int mid = (s.top + s.bottom) / 2;
	for (int y = CY0; y < CY1; y++)
		for (int x = CX0; x < CX1; x++) {
			const uint8_t *p = px(img, x, y);
			if (p[0] > 200 && p[1] > 200 && p[2] > 200) {
				if (y <= mid)
					s.bright_upper++;
				else
					s.bright_lower++;
			}
		}
	return s;
}

static void keep(const char *name)
{
	if (!getenv("REI_SHOT_DIR"))
		return;
	char cmd[600];
	snprintf(cmd, sizeof cmd, "cp %s %s/%s.ppm", g_shot, getenv("REI_SHOT_DIR"), name);
	CHECK(system(cmd) == 0);
}

/* A window-sized buffer: black, with a white 6x6 block at (100,100). */
static struct wl_buffer *block_buffer(struct client *c, int w, int h)
{
	size_t size = (size_t)w * (size_t)h * 4;
	int fd = memfd_create("block", MFD_CLOEXEC);
	CHECK(fd >= 0 && ftruncate(fd, (off_t)size) == 0);
	uint32_t *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	CHECK(p != MAP_FAILED);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			p[y * w + x] = (x >= 100 && x < 106 && y >= 100 && y < 106) ? 0xffffffff : 0xff000000;
	munmap(p, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(c->shm, fd, (int32_t)size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
}

/* A highlighted line, as in a file manager's selection: a white bar (x 20..420, y 200..215) with dark "text" on
 * it (a 6x6 block at (100,204)), and ordinary bright "text" (a white 6x6 block at (100,100)) on the black. */
static struct wl_buffer *bar_buffer(struct client *c, int w, int h)
{
	size_t size = (size_t)w * (size_t)h * 4;
	int fd = memfd_create("bar", MFD_CLOEXEC);
	CHECK(fd >= 0 && ftruncate(fd, (off_t)size) == 0);
	uint32_t *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	CHECK(p != MAP_FAILED);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint32_t v = 0xff000000;
			if (x >= 20 && x < 420 && y >= 200 && y < 216)
				v = (x >= 100 && x < 106 && y >= 204 && y < 210) ? 0xff000000 : 0xffffffff;
			else if (x >= 100 && x < 106 && y >= 100 && y < 106)
				v = 0xffffffff;
			p[y * w + x] = v;
		}
	munmap(p, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(c->shm, fd, (int32_t)size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
}

static int lum_at(int x, int y)
{
	struct ppm img = screenshot();
	const uint8_t *p = px(&img, x, y);
	int l = p[0] + p[1] + p[2];
	free(img.rgb);
	return l;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-textfx-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);
	start_compositor(argv[1], g_base, true);

	struct client c;
	client_connect(&c);
	g_clients[0] = &c;
	client_bind_pointer(&c);
	inject("motion_abs 400 590");
	settle_all();

	/* ---- The bar's text, plain. ---- */
	struct ppm img = screenshot();
	struct stats plain = clock_stats(&img);
	free(img.rgb);
	CHECK(plain.lit > 40 && plain.red == 0);

	/* ---- Glow: red light around the letters, which are still there. ---- */
	set_config("[theme.toolbar]\ntext_glow_size = 6\ntext_glow_strength = 2\ntext_glow_color = \"#ff0000\"\ntext_glow_tint = 1\n", true);
	img = screenshot();
	keep("textfx-glow");
	struct stats glow = clock_stats(&img);
	free(img.rgb);
	fprintf(stderr, "plain: lit %d; glow: lit %d red %d\n", plain.lit, glow.lit, glow.red);
	CHECK(glow.red > 60);
	CHECK(glow.lit >= plain.lit * 3 / 4);
	CHECK(log_has("compiled the shader 'text'"));

	/* ---- Shine: dark blue letters whose upper half gleams white, and only the upper half. ---- */
	set_config("[theme.toolbar]\ntext_color = \"#203090\"\n", true);
	img = screenshot();
	struct stats dark = clock_stats(&img);
	free(img.rgb);
	CHECK(dark.bright_upper == 0 && dark.bright_lower == 0);
	set_config("[theme.toolbar]\ntext_color = \"#203090\"\ntext_shine = 1\ntext_shine_color = \"#ffffff\"\n", true);
	img = screenshot();
	keep("textfx-shine");
	struct stats shine = clock_stats(&img);
	free(img.rgb);
	fprintf(stderr, "shine: upper %d lower %d (rows %d..%d)\n", shine.bright_upper, shine.bright_lower, shine.top, shine.bottom);
	CHECK(shine.bright_upper >= 8 && shine.bright_lower == 0);

	/* ---- Blink: the text comes and goes by itself. ---- */
	set_config("[theme.toolbar]\ntext_blink = 0.3\n", true);
	int on = 0, off = 0, changes = 0, last = -1;
	for (int i = 0; i < 80 && changes < 4; i++) {
		img = screenshot();
		struct stats b = clock_stats(&img);
		free(img.rgb);
		int state = b.lit > 40 ? 1 : b.lit == 0 ? 0 : -1;
		CHECK(state >= 0);   /* fully there or fully gone, never half */
		on += state == 1;
		off += state == 0;
		if (last >= 0 && state != last)
			changes++;
		last = state;
		sleep_ms(25);
	}
	fprintf(stderr, "blink: on %d off %d changes %d\n", on, off, changes);
	CHECK(on > 0 && off > 0 && changes >= 3);
	set_config("", true);
	img = screenshot();
	struct stats again = clock_stats(&img);
	free(img.rgb);
	CHECK(again.lit == plain.lit);   /* and all of it off again is exactly the plain bar */

	/* ---- A window shader: the windows of "glowterm" bloom around bright pixels. ---- */
	g_app_id = "glowterm";
	struct window w;
	open_win(&c, &w, "term", 0xff000000);
	inject("motion_abs 400 300");
	settle_all();
	const struct ev *e = ev_last(&c, EV_PTR_ENTER);
	CHECK(e);
	int ox = 400 - (int)e->x, oy = 300 - (int)e->y;   /* the window's corner on screen */
	wl_surface_attach(w.surface, block_buffer(&c, w.buf_w, w.buf_h), 0, 0);
	wl_surface_damage(w.surface, 0, 0, w.buf_w, w.buf_h);
	wl_surface_commit(w.surface);
	inject("motion_abs 795 595");
	settle_all();

	set_config("[theme.window_text]\napps = \"someone-else\"\ntext_glow_size = 8\ntext_glow_strength = 2\n", true);
	CHECK(lum_at(ox + 103, oy + 103) > 700);     /* the block */
	CHECK(lum_at(ox + 110, oy + 103) < 12);      /* nothing next to it: the rule is for another program */

	set_config("[theme.window_text]\napps = \"other, GlowTerm\"\ntext_glow_size = 8\ntext_glow_strength = 2\n", true);
	int near_block = lum_at(ox + 110, oy + 103), far = lum_at(ox + 160, oy + 160);
	fprintf(stderr, "window glow: next to the block %d, far away %d\n", near_block, far);
	CHECK(near_block > 40 && far < 12);
	CHECK(lum_at(ox + 103, oy + 103) > 700);
	CHECK(log_has("compiled the shader 'glow'"));

	/* The shader ran once for this picture; redrawing the screen (the pointer moving about) does not run it
	 * again, only a change of the window's content does. */
	int runs = log_count("ran the shader 'glow'");
	CHECK(runs >= 1);
	for (int i = 0; i < 20; i++)
		inject("motion_abs %d 595", 300 + i * 10);
	settle_all();
	CHECK(lum_at(ox + 110, oy + 103) > 40);
	CHECK(log_count("ran the shader 'glow'") == runs);
	wl_surface_attach(w.surface, block_buffer(&c, w.buf_w, w.buf_h), 0, 0);
	wl_surface_damage(w.surface, 0, 0, w.buf_w, w.buf_h);
	wl_surface_commit(w.surface);
	settle_all();
	CHECK(lum_at(ox + 110, oy + 103) > 40);
	CHECK(log_count("ran the shader 'glow'") == runs + 1);

	/* A highlighted line: its dark text stays readable, where bloom would wash it out. Ordinary text still glows. */
	wl_surface_attach(w.surface, bar_buffer(&c, w.buf_w, w.buf_h), 0, 0);
	wl_surface_damage(w.surface, 0, 0, w.buf_w, w.buf_h);
	wl_surface_commit(w.surface);
	set_config("[theme.window_text]\napps = \"glowterm\"\ntext_glow_size = 8\ntext_glow_strength = 2\n", true);
	int ink = lum_at(ox + 103, oy + 207), ordinary = lum_at(ox + 110, oy + 103);
	fprintf(stderr, "highlighted line: its text %d; ordinary text's glow %d\n", ink, ordinary);
	CHECK(ink < 60);          /* dark on the bar, as drawn */
	CHECK(ordinary > 40);     /* the glow beside ordinary text is still there */
	set_config("[theme.window_text]\napps = \"glowterm\"\ntext_glow_size = 8\ntext_glow_strength = 2\nglow_area_limit = 1\n", true);
	int washed = lum_at(ox + 103, oy + 207);
	fprintf(stderr, "with the limit off: %d\n", washed);
	CHECK(washed > 300);      /* ...which is what the limit prevents */
	wl_surface_attach(w.surface, block_buffer(&c, w.buf_w, w.buf_h), 0, 0);
	wl_surface_damage(w.surface, 0, 0, w.buf_w, w.buf_h);
	wl_surface_commit(w.surface);
	settle_all();

	/* The size at 0 turns it off. */
	set_config("[theme.window_text]\napps = \"glowterm\"\ntext_glow_size = 0\n", true);
	CHECK(lum_at(ox + 110, oy + 103) < 12);

	/* ---- A shader file, written as for picom. ---- */
	char shader[300], cfg[900];
	snprintf(shader, sizeof shader, "%s/invert.glsl", g_dir);
	write_text(shader,
		   "#version 330\n"
		   "in vec2 texcoord;\n"
		   "uniform sampler2D tex;\n"
		   "uniform float opacity;\n"
		   "vec4 default_post_processing(vec4 c);\n"
		   "vec4 window_shader() {\n"
		   "    vec4 c = texelFetch(tex, ivec2(texcoord), 0);\n"
		   "    c.rgb = vec3(c.a) - c.rgb;\n"
		   "    return default_post_processing(c);\n"
		   "}\n");
	snprintf(cfg, sizeof cfg, "[theme.window_text]\napps = \"glowterm\"\nshader = \"%s\"\n", shader);
	set_config(cfg, true);
	CHECK(lum_at(ox + 160, oy + 160) > 700);     /* black became white */
	CHECK(lum_at(ox + 103, oy + 103) < 12);      /* and the block black */

	/* One that does not compile: said in the log, and the window is drawn as it is. */
	char broken[300];
	snprintf(broken, sizeof broken, "%s/broken.glsl", g_dir);
	write_text(broken, "vec4 window_shader() { return no_such_thing; }\n");
	snprintf(cfg, sizeof cfg, "[theme.window_text]\napps = \"glowterm\"\nshader = \"%s\"\n", broken);
	set_config(cfg, true);
	CHECK(lum_at(ox + 160, oy + 160) < 12);
	CHECK(log_has("does not compile"));

	/* One that is not there: the config is refused, the previous one stays. */
	snprintf(cfg, sizeof cfg, "[theme.window_text]\napps = \"glowterm\"\nshader = \"%s/nothing.glsl\"\n", g_dir);
	set_config(cfg, false);
	CHECK(log_has("theme.window_text.shader: cannot read"));

	/* For looking at (only with REI_SHOT_DIR): a terminal-like window with and without the glow, and a bar whose
	 * text glows. The "terminal" is a picture ImageMagick draws. */
	if (getenv("REI_SHOT_DIR")) {
		char raw[300], cmd[4000];
		snprintf(raw, sizeof raw, "%s/term.bgra", g_dir);
		snprintf(cmd, sizeof cmd,
			 "convert -size %dx%d xc:'#07080c' -font DejaVu-Sans-Mono -pointsize 15 "
			 "-fill '#3dff74' -annotate +14+30 'scribe@thinkpad ~/rei-wm $ ninja -C build' "
			 "-fill '#c8d0d8' -annotate +14+52 '[145/145] Linking target src/rei-wm' "
			 "-fill '#3dff74' -annotate +14+74 'scribe@thinkpad ~/rei-wm $ git log --oneline | head -3' "
			 "-fill '#ffcf40' -annotate +14+96 '8c90b0d' -fill '#c8d0d8' -annotate +86+96 'Cursors: play animated XCursor themes' "
			 "-fill '#ffcf40' -annotate +14+118 '8bc6858' -fill '#c8d0d8' -annotate +86+118 'Cursors: XCursor themes, shapes by context' "
			 "-fill '#ffcf40' -annotate +14+140 '0507612' -fill '#c8d0d8' -annotate +86+140 'Watchdog: a VT switch is not a hang' "
			 "-fill '#5aa8ff' -annotate +14+162 'src/  tests/  settings/  docs/' "
			 "-fill '#5aa8ff' -annotate +14+184 'build/  src/' "
			 "-fill '#d8dde4' -draw 'rectangle 0,194 %d,214' -fill '#101418' -annotate +14+210 ' docs/THEME.md                        14.2K' "
			 "-fill '#c8d0d8' -annotate +14+232 'README.md' "
			 "-depth 8 bgra:%s", w.buf_w, w.buf_h, w.buf_w, raw);
		int rc_convert = system(cmd);
		if (rc_convert != 0)
			fprintf(stderr, "(no sample terminal picture: convert failed: %d)\n", rc_convert);
		if (rc_convert == 0) {
			size_t size = (size_t)w.buf_w * (size_t)w.buf_h * 4;
			int fd = memfd_create("term", MFD_CLOEXEC);
			CHECK(fd >= 0 && ftruncate(fd, (off_t)size) == 0);
			void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			FILE *f = fopen(raw, "rb");
			CHECK(mem != MAP_FAILED && f && fread(mem, 1, size, f) == size);
			fclose(f);
			munmap(mem, size);
			struct wl_shm_pool *pool = wl_shm_create_pool(c.shm, fd, (int32_t)size);
			struct wl_buffer *tb = wl_shm_pool_create_buffer(pool, 0, w.buf_w, w.buf_h, w.buf_w * 4, WL_SHM_FORMAT_XRGB8888);
			wl_shm_pool_destroy(pool);
			close(fd);
			wl_surface_attach(w.surface, tb, 0, 0);
			wl_surface_damage(w.surface, 0, 0, w.buf_w, w.buf_h);
			wl_surface_commit(w.surface);
			set_config("[theme.window_text]\napps = \"glowterm\"\ntext_glow_size = 0\n", true);
			img = screenshot();
			free(img.rgb);
			keep("textfx-terminal-plain");
			set_config("[theme.window_text]\napps = \"glowterm\"\nglow_area_limit = 1\n", true);
			img = screenshot();
			free(img.rgb);
			keep("textfx-terminal-selection-washed");
			set_config("[theme.window_text]\napps = \"glowterm\"\n"
				   "[theme.toolbar]\ntext_glow_size = 5\ntext_glow_strength = 1.5\n"
				   "[theme.tray_tile]\ntext_glow_size = 5\ntext_glow_color = \"#40a0ff\"\ntext_glow_tint = 1\n", true);
			img = screenshot();
			free(img.rgb);
			keep("textfx-terminal-glow");
		}
	}

	CHECK(stop_compositor() == 0);

	/* ---- The same from the command line, as picom has it. ---- */
	char cmd0[800], rule[400];
	snprintf(cmd0, sizeof cmd0, "rm -f %s/input.fifo %s/%s %s/%s.lock", g_dir, g_dir, SOCKET, g_dir, SOCKET);
	CHECK(system(cmd0) == 0);
	g_pid = 0;
	close(g_fifo_fd);
	g_fifo_fd = -1;
	g_nwins = 0;
	snprintf(rule, sizeof rule, "%s:glowterm", shader);
	g_extra_args[0] = "--window-shader-fg-rule";
	g_extra_args[1] = rule;
	char base2[2000];
	snprintf(base2, sizeof base2, "%s[theme.window_text]\napps = \"\"\n", g_base);
	start_compositor(argv[1], base2, true);
	struct client c2;
	client_connect(&c2);
	g_clients[0] = &c2;
	client_bind_pointer(&c2);
	struct window w2;
	open_win(&c2, &w2, "term", 0xff000000);
	inject("motion_abs 400 300");
	settle_all();
	e = ev_last(&c2, EV_PTR_ENTER);
	CHECK(e);
	ox = 400 - (int)e->x;
	oy = 300 - (int)e->y;
	inject("motion_abs 795 595");
	settle_all();
	CHECK(lum_at(ox + 160, oy + 160) > 700);   /* inverted by the rule */
	g_app_id = "plain-app";
	struct window w3;
	open_win(&c2, &w3, "other", 0xff000000);   /* another program's window is left alone */
	inject("motion_abs 795 595");
	settle_all();
	img = screenshot();
	int white = 0, black = 0;
	for (int y = 40; y < 590; y += 5)
		for (int x = 10; x < 790; x += 5) {
			const uint8_t *p = px(&img, x, y);
			white += p[0] > 240 && p[1] > 240 && p[2] > 240;
			black += p[0] < 8 && p[1] < 8 && p[2] < 8;
		}
	free(img.rgb);
	fprintf(stderr, "rule: white %d black %d\n", white, black);
	CHECK(white > 1000 && black > 1000);
	CHECK(stop_compositor() == 0);

	puts("textfx: ok");
	return 0;
}
