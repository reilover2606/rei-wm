/*
 * Wallpaper groups, end to end in the headless compositor with real picture files: a group of one picture, a folder,
 * a list of files; a group that cycles; a workspace with a group of its own while the others show the "any" group;
 * what changing the groups in the config does; and a plain [wallpaper] path still working.
 *
 * The pictures are solid colours, shown stretched, so the colour of any pixel says which one is on screen.
 */
#define SOCKET "rei-wallpapers-test"
#include "testlib.h"

#include <signal.h>

#include "config.h"
#include "pngw.h"
#include "wallpapers.h"

static char g_pics[400];

static void pic(const char *dir, const char *name, int r, int g, int b)
{
	char path[700];
	snprintf(path, sizeof path, "%s/%s/%s", g_pics, dir, name);
	write_png(path, 16, 16, (uint8_t)r, (uint8_t)g, (uint8_t)b, 255);
}

static void make_dir(const char *name)
{
	char path[600];
	snprintf(path, sizeof path, "%s/%s", g_pics, name);
	CHECK(mkdir(path, 0700) == 0);
}

static bool shows(int r, int g, int b)
{
	struct ppm img = screenshot();
	bool ok = near_px(&img, 200, 450, r, g, b, 3) && near_px(&img, 700, 100, r, g, b, 3);   /* (not under the pointer) */
	free(img.rgb);
	return ok;
}

static void wait_shows(int r, int g, int b, const char *what)
{
	for (int t = 0; t < 6000; t += 100) {
		if (shows(r, g, b))
			return;
		sleep_ms(100);
	}
	struct ppm img = screenshot();
	const uint8_t *p = img.rgb + ((size_t)450 * (size_t)img.w + 200) * 3;
	fprintf(stderr, "%s: the screen shows (%d,%d,%d), expected (%d,%d,%d)\n", what, p[0], p[1], p[2], r, g, b);
	exit(1);
}

static void reload(const char *text)
{
	char path[500], tmp[520];
	snprintf(path, sizeof path, "%s/config.toml", g_dir);
	snprintf(tmp, sizeof tmp, "%s.new", path);
	write_text(tmp, text);
	int ok = log_count("config reloaded"), bad = log_count("config not reloaded");
	CHECK(rename(tmp, path) == 0);
	CHECK(kill(g_pid, SIGHUP) == 0);
	for (int t = 0;; t += 20) {
		CHECK(t < 5000);
		if (log_count("config reloaded") > ok)
			break;
		CHECK(log_count("config not reloaded") == bad);
		sleep_ms(20);
	}
	settle_all();
}

static char g_cfg[6000];

static const char *config(const char *groups)
{
	snprintf(g_cfg, sizeof g_cfg, "[toolbar]\nenabled = false\n[wallpaper]\nmode = \"stretch\"\ncolor = \"#000000\"\n%s"
		 "[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n", groups, OUT_W, OUT_H);
	return g_cfg;
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-wallpapers-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);
	snprintf(g_pics, sizeof g_pics, "%s/pics", g_dir);
	CHECK(mkdir(g_pics, 0700) == 0);
	crc_init();
	make_dir("walls");
	pic("walls", "b-green.png", 0, 255, 0);       /* name order: a, b, c */
	pic("walls", "a-red.png", 255, 0, 0);
	pic("walls", "C-blue.png", 0, 0, 255);
	char junk[700];
	snprintf(junk, sizeof junk, "%s/walls/notes.txt", g_pics);
	write_text(junk, "not a picture");
	make_dir("solo");
	pic("solo", "yellow.png", 255, 255, 0);
	pic("solo", "cyan.png", 0, 255, 255);

	/* ---- Listing and choosing a group (pure). ---- */
	char **list;
	char path[700];
	snprintf(path, sizeof path, "%s/walls", g_pics);
	size_t n = rei_wallpaper_list(path, &list);
	CHECK(n == 3);   /* the text file is not a picture */
	CHECK(strstr(list[0], "a-red.png") && strstr(list[1], "b-green.png") && strstr(list[2], "C-blue.png"));   /* by name, ignoring case */
	for (size_t i = 0; i < n; i++)
		free(list[i]);
	free(list);
	snprintf(path, sizeof path, "%s/solo/yellow.png", g_pics);
	n = rei_wallpaper_list(path, &list);
	CHECK(n == 1 && !strcmp(list[0], path));   /* a file is itself */
	free(list[0]);
	free(list);
	CHECK(rei_wallpaper_list("/nonexistent-pictures", &list) == 0);

	/* ---- Config checks. ---- */
	char err[300];
	struct rei_config *c = rei_config_parse("[[wallpaper_group]]\nname = \"a\"\nany = true\n[[wallpaper_group]]\nname = \"b\"\nany = true\n", err, sizeof err);
	CHECK(!c && strstr(err, "only one group can be the \"any\" group"));
	c = rei_config_parse("[[wallpaper_group]]\nname = \"a\"\nworkspaces = [1, 2]\n[[wallpaper_group]]\nname = \"b\"\nworkspaces = [2]\n", err, sizeof err);
	CHECK(!c && strstr(err, "workspace 2 is given to two groups"));
	c = rei_config_parse("[[wallpaper_group]]\nname = \"a\"\ninterval = 1\n", err, sizeof err);
	CHECK(!c && strstr(err, "interval"));
	c = rei_config_parse("[[wallpaper_group]]\nname = \"a\"\nworkspaces = [0]\n", err, sizeof err);
	CHECK(!c && strstr(err, "workspaces"));
	c = rei_config_parse("[[wallpaper_group]]\nname = \"a\"\npaths = \"/x\"\n", err, sizeof err);
	CHECK(!c && strstr(err, "paths"));
	c = rei_config_parse("[[wallpaper_group]]\nname = \"One\"\npath = \"/a.png\"\nany = true\n"
			     "[[wallpaper_group]]\nname = \"Two\"\npaths = [\"/b.png\", \"/dir\"]\ninterval = 30\nworkspaces = [2, 5]\n", err, sizeof err);
	CHECK(c && c->n_wallpaper_groups == 2);
	CHECK(c->wallpaper_groups[0].any && c->wallpaper_groups[0].n_paths == 1 && c->wallpaper_groups[0].interval == 600);
	CHECK(c->wallpaper_groups[1].n_paths == 2 && c->wallpaper_groups[1].interval == 30 && c->wallpaper_groups[1].n_workspaces == 2);
	CHECK(rei_wallpapers_group_for(c, 2) == &c->wallpaper_groups[1] && rei_wallpapers_group_for(c, 5) == &c->wallpaper_groups[1]);
	CHECK(rei_wallpapers_group_for(c, 1) == &c->wallpaper_groups[0] && rei_wallpapers_group_for(c, 9) == &c->wallpaper_groups[0]);   /* the any group */
	rei_config_free(c);
	c = rei_config_parse("[[wallpaper_group]]\nname = \"Two\"\npaths = [\"/b.png\"]\nworkspaces = [2]\n", err, sizeof err);
	CHECK(c && rei_wallpapers_group_for(c, 2) && !rei_wallpapers_group_for(c, 1));   /* no any group: the colour */
	rei_config_free(c);

	/* ---- In the compositor. ---- */
	setenv("REI_WALLPAPER_INTERVAL_MS", "1500", 1);
	char groups[4000];
	snprintf(groups, sizeof groups,
		 "[[wallpaper_group]]\nname = \"Folder\"\npaths = [\"%s/walls\"]\nany = true\n"
		 "[[wallpaper_group]]\nname = \"Solo\"\npaths = [\"%s/solo/yellow.png\"]\nworkspaces = [2]\n",
		 g_pics, g_pics);
	start_compositor(argv[1], config(groups), true);
	struct client cl;
	client_connect(&cl);
	g_clients[0] = &cl;
	client_bind_keyboard(&cl);
	client_bind_pointer(&cl);
	inject("motion_abs 400 300");
	settle_all();

	/* Workspace 1 shows the any group, which starts on its first picture; workspace 2 its own. */
	wait_shows(255, 0, 0, "workspace 1: the any group's first picture");
	mod(3);   /* Mod+2 */
	settle_all();
	wait_shows(255, 255, 0, "workspace 2: its own group");
	/* The folder's group moves on by itself while it is not on screen, and this one, a single picture, stays. */
	sleep_ms(1800);
	CHECK(shows(255, 255, 0));
	mod(2);   /* Mod+1 */
	settle_all();
	/* ...it has moved on at least once (green or blue by now), in name order. */
	bool moved = shows(0, 255, 0) || shows(0, 0, 255);
	CHECK(moved);
	/* And it keeps cycling through all three, back to the start. */
	bool seen_red = false, seen_green = false, seen_blue = false;
	for (int t = 0; t < 12000 && !(seen_red && seen_green && seen_blue); t += 150) {
		struct ppm img = screenshot();
		seen_red |= near_px(&img, 200, 450, 255, 0, 0, 3);
		seen_green |= near_px(&img, 200, 450, 0, 255, 0, 3);
		seen_blue |= near_px(&img, 200, 450, 0, 0, 255, 3);
		free(img.rgb);
		sleep_ms(150);
	}
	CHECK(seen_red && seen_green && seen_blue);

	/* ---- Changing the groups in the config. A list of files, as a group of its own. ---- */
	snprintf(groups, sizeof groups,
		 "[[wallpaper_group]]\nname = \"Pair\"\npaths = [\"%s/solo/cyan.png\", \"%s/solo/yellow.png\"]\ninterval = 5\nany = true\n"
		 "[[wallpaper_group]]\nname = \"Folder\"\npaths = [\"%s/walls\"]\nworkspaces = [1]\n", g_pics, g_pics, g_pics);
	reload(config(groups));
	/* Workspace 1 now has its own group (the folder): the any group is for the others. */
	bool on_folder = false;
	for (int t = 0; t < 4000 && !on_folder; t += 100) {
		sleep_ms(100);
		on_folder = shows(255, 0, 0) || shows(0, 255, 0) || shows(0, 0, 255);
	}
	CHECK(on_folder);
	mod(3);   /* workspace 2: no group of its own now, so the any group */
	settle_all();
	wait_shows(0, 255, 255, "workspace 2: the any group's first picture (cyan)");
	mod(4);   /* workspace 3: the same */
	settle_all();
	CHECK(shows(0, 255, 255) || shows(255, 255, 0));

	/* No any group at all: a workspace with no group of its own shows the plain colour. */
	snprintf(groups, sizeof groups, "[[wallpaper_group]]\nname = \"Solo\"\npaths = [\"%s/solo/yellow.png\"]\nworkspaces = [2]\n", g_pics);
	reload(config(groups));
	wait_shows(0, 0, 0, "workspace 3 with no any group: the colour");
	mod(3);
	settle_all();
	wait_shows(255, 255, 0, "workspace 2");

	/* A picture that cannot be loaded is skipped. */
	char bad[700];
	snprintf(bad, sizeof bad, "%s/walls/zz-broken.png", g_pics);
	write_text(bad, "this is not a png");
	snprintf(groups, sizeof groups, "[[wallpaper_group]]\nname = \"F\"\npaths = [\"%s/walls\"]\nany = true\n", g_pics);
	reload(config(groups));
	mod(2);
	settle_all();
	for (int i = 0; i < 12; i++) {   /* it cycles past the broken one without ever showing nothing */
		CHECK(shows(255, 0, 0) || shows(0, 255, 0) || shows(0, 0, 255));
		sleep_ms(500);
	}
	CHECK(log_has("wallpaper not loaded"));
	CHECK(stop_compositor() == 0);

	/* ---- The old [wallpaper] path, with no groups. ---- */
	char cmd0[1500];
	snprintf(cmd0, sizeof cmd0, "rm -f %s/input.fifo %s/%s %s/%s.lock %s", g_dir, g_dir, SOCKET, g_dir, SOCKET, bad);
	CHECK(system(cmd0) == 0);
	g_pid = 0;
	close(g_fifo_fd);
	g_fifo_fd = -1;
	char legacy[1200];
	snprintf(legacy, sizeof legacy, "path = \"%s/solo/cyan.png\"\n", g_pics);
	start_compositor(argv[1], config(legacy), true);
	struct client cl2;
	client_connect(&cl2);
	g_clients[0] = &cl2;
	wait_shows(0, 255, 255, "the old path");
	CHECK(stop_compositor() == 0);
	puts("wallpapers: ok");
	return 0;
}
