/*
 * The wifi item of the toolbar, against a fake /sys/class/net and /proc/net/wireless (REI_WIFI_DIR, REI_WIFI_PROC):
 * connected it is a signal icon in the bar's text colour (more arcs lit for a stronger signal), not connected it is
 * red with a strike through, it follows a change, it is absent on a machine without a wireless interface, and a click
 * runs [toolbar] wifi_command.
 */
#define SOCKET "rei-wifi-test"
#include "testlib.h"

#include <signal.h>

#include "wifi.h"

static char g_net[400], g_proc[400];

static void set_iface(const char *name, const char *operstate, bool wireless)
{
	char dir[600], path[700];
	snprintf(dir, sizeof dir, "%s/%s", g_net, name);
	mkdir(dir, 0700);
	snprintf(path, sizeof path, "%s/wireless", dir);
	if (wireless)
		mkdir(path, 0700);
	snprintf(path, sizeof path, "%s/operstate", dir);
	write_text(path, operstate);
}

static void set_quality(const char *iface, int q)
{
	char text[400];
	snprintf(text, sizeof text,
		 "Inter-| sta-|   Quality        |   Discarded packets               | Missed | WE\n"
		 " face | tus | link level noise |  nwid  crypt   frag  retry   misc | beacon | 22\n"
		 "%6s: 0000   %d.  -50.  -256        0      0      0      0      0        0\n", iface, q);
	write_text(g_proc, text);
}

struct icon {
	int red, lit, x, y;   /* red pixels, bright (text-coloured, full strength) pixels, a red pixel's position */
};

static struct icon look(void)
{
	struct ppm img = screenshot();
	struct icon ic = { 0, 0, -1, -1 };
	long sx = 0, sy = 0;
	for (int y = 0; y < 28; y++)
		for (int x = 560; x < 780; x++) {
			const uint8_t *p = img.rgb + ((size_t)y * (size_t)img.w + (size_t)x) * 3;
			if (p[0] > 150 && p[1] < 120 && p[2] < 120) {
				ic.red++;
				sx += x;
				sy += y;
			} else if (p[0] > 190 && p[1] > 190 && p[2] > 190 && x < 740) {   /* left of the clock's text */
				ic.lit++;
			}
		}
	if (ic.red) {
		ic.x = (int)(sx / ic.red);
		ic.y = (int)(sy / ic.red);
	}
	free(img.rgb);
	return ic;
}

static int read_pid(const char *path)
{
	FILE *f = fopen(path, "r");
	int pid = 0;
	if (f) {
		if (fscanf(f, "%d", &pid) != 1)
			pid = 0;
		fclose(f);
	}
	return pid;
}

/* Alive and not a zombie (nothing may be reaping what the compositor's spawn left to init). */
static bool pid_running(int pid)
{
	char path[64], line[256];
	snprintf(path, sizeof path, "/proc/%d/stat", pid);
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	bool alive = true;
	if (fgets(line, sizeof line, f)) {
		const char *p = strrchr(line, ')');
		alive = !(p && p[1] == ' ' && p[2] == 'Z');
	}
	fclose(f);
	return alive;
}

static void click_icon(int x, int y)
{
	inject("motion_abs %d %d", x, y);
	inject("button 272 1");
	inject("button 272 0");
	settle_all();
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-wifi-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);
	snprintf(g_net, sizeof g_net, "%s/net", g_dir);
	CHECK(mkdir(g_net, 0700) == 0);
	snprintf(g_proc, sizeof g_proc, "%s/wireless", g_dir);

	/* ---- Reading (pure). ---- */
	struct rei_wifi_info info;
	set_iface("eth0", "up\n", false);
	CHECK(!rei_wifi_read(g_net, g_proc, &info) && !info.present);          /* no wireless interface */
	set_iface("wlan0", "down\n", true);
	CHECK(rei_wifi_read(g_net, g_proc, &info) && info.present && !info.connected && info.level == 0);
	set_iface("wlan0", "up\n", true);
	set_quality("wlan0", 60);
	CHECK(rei_wifi_read(g_net, g_proc, &info) && info.connected && info.level == 3 && !strcmp(info.iface, "wlan0"));
	set_quality("wlan0", 30);
	CHECK(rei_wifi_read(g_net, g_proc, &info) && info.level == 2);
	set_quality("wlan0", 8);
	CHECK(rei_wifi_read(g_net, g_proc, &info) && info.level == 1);
	set_iface("wlan0", "dormant\n", true);   /* up but not associated yet */
	CHECK(rei_wifi_read(g_net, g_proc, &info) && !info.connected);
	set_iface("wlan0", "up\n", true);
	CHECK(rei_wifi_read("/nonexistent-net", g_proc, &info) == false);

	/* ---- In the bar. ---- */
	char started[500];
	snprintf(started, sizeof started, "%s/wifi-clicked", g_dir);
	setenv("REI_WIFI_DIR", g_net, 1);
	setenv("REI_WIFI_PROC", g_proc, 1);
	char config[1500];
	snprintf(config, sizeof config,
		 "[toolbar]\nenabled = true\nclock_format = \"CLOCK\"\nbattery = false\nsysinfo = false\nbackground = \"#101010\"\n"
		 "foreground = \"#e8e8e8\"\nwifi_command = \"echo $$ > %s; exec sleep 300\"\n[start_menu]\nenabled = false\n[audio]\nenabled = false\n"
		 "[[output]]\nname = \"OUT\"\nwidth = 800\nheight = 600\n", started);
	set_quality("wlan0", 60);
	start_compositor(argv[1], config, true);
	struct client c;
	client_connect(&c);
	g_clients[0] = &c;
	client_bind_pointer(&c);
	inject("motion_abs 400 400");
	settle_all();

	struct icon strong = look();
	fprintf(stderr, "connected, strong: red %d lit %d\n", strong.red, strong.lit);
	CHECK(strong.red == 0 && strong.lit > 20);   /* an icon, in the text colour */
	if (getenv("REI_SHOT_DIR")) {
		struct ppm im = screenshot();
		free(im.rgb);
		char cmd[600];
		snprintf(cmd, sizeof cmd, "cp %s %s/wifi-connected.ppm", g_shot, getenv("REI_SHOT_DIR"));
		CHECK(system(cmd) == 0);
	}

	/* A weaker signal lights fewer arcs (the poll takes up to three seconds to notice). */
	set_quality("wlan0", 8);
	int weak_lit = strong.lit;
	for (int t = 0; t < 6000 && weak_lit >= strong.lit; t += 250) {
		sleep_ms(250);
		weak_lit = look().lit;
	}
	fprintf(stderr, "weak: lit %d\n", weak_lit);
	CHECK(weak_lit < strong.lit);

	/* Not connected: red, with the strike. */
	set_iface("wlan0", "down\n", true);
	struct icon down = look();
	for (int t = 0; t < 6000 && down.red == 0; t += 250) {
		sleep_ms(250);
		down = look();
	}
	fprintf(stderr, "not connected: red %d at %d,%d\n", down.red, down.x, down.y);
	CHECK(down.red > 25);
	if (getenv("REI_SHOT_DIR")) {
		struct ppm im = screenshot();
		free(im.rgb);
		char cmd[600];
		snprintf(cmd, sizeof cmd, "cp %s %s/wifi-disconnected.ppm", g_shot, getenv("REI_SHOT_DIR"));
		CHECK(system(cmd) == 0);
	}

	/* A click on it runs the wifi program. */
	CHECK(access(started, F_OK) != 0);
	inject("motion_abs %d %d", down.x, down.y);
	inject("button 272 1");
	inject("button 272 0");
	settle_all();
	for (int t = 0; access(started, F_OK) != 0; t += 30) {
		CHECK(t < 4000);
		sleep_ms(30);
	}
	/* A second click closes that instance rather than starting another; a third starts one again. */
	int first = read_pid(started);
	CHECK(first > 0 && pid_running(first));
	click_icon(down.x, down.y);
	for (int t = 0; pid_running(first); t += 30) {
		CHECK(t < 4000);
		sleep_ms(30);
	}
	unlink(started);
	click_icon(down.x, down.y);
	for (int t = 0; access(started, F_OK) != 0; t += 30) {
		CHECK(t < 4000);
		sleep_ms(30);
	}
	int second = read_pid(started);
	CHECK(second > 0 && second != first && pid_running(second));
	/* A click beside it does nothing. */
	inject("motion_abs %d 14", down.x - 60);
	inject("button 272 1");
	inject("button 272 0");
	settle_all();
	sleep_ms(300);
	CHECK(pid_running(second));
	click_icon(down.x, down.y);   /* (closes it again) */
	for (int t = 0; pid_running(second); t += 30) {
		CHECK(t < 4000);
		sleep_ms(30);
	}
	CHECK(stop_compositor() == 0);

	/* ---- A machine without a wireless interface: no item. ---- */
	char cmd0[1500];
	snprintf(cmd0, sizeof cmd0, "rm -f %s/input.fifo %s/%s %s/%s.lock; rm -rf %s/wlan0", g_dir, g_dir, SOCKET, g_dir, SOCKET, g_net);
	CHECK(system(cmd0) == 0);
	g_pid = 0;
	close(g_fifo_fd);
	g_fifo_fd = -1;
	g_nwins = 0;
	start_compositor(argv[1], config, true);
	struct client c2;
	client_connect(&c2);
	g_clients[0] = &c2;
	settle_all();
	CHECK(look().red == 0);
	CHECK(stop_compositor() == 0);
	puts("wifi: ok");
	return 0;
}
