/*
 * End-to-end test of config hot reload: gaps, keybindings and the toolbar change while
 * the compositor runs, and an invalid file leaves the old settings in force.
 */
#define SOCKET "rei-reload-test"
#include "testlib.h"

#define OUT_W 800
#define OUT_H 600
#define KEY_Q 16
#define KEY_X 45

static char g_cfg[256];

static void write_config_tb(bool toolbar, const char *extra)
{
	char text[1200];
	snprintf(text, sizeof text,
		 "[toolbar]\nenabled = %s\nthickness = 28\n[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n%s",
		 toolbar ? "true" : "false", OUT_W, OUT_H, extra);
	/* Write like an editor does: a new file renamed over the old one. */
	char tmp[300];
	snprintf(tmp, sizeof tmp, "%s.new", g_cfg);
	write_text(tmp, text);
	CHECK(rename(tmp, g_cfg) == 0);
}

static void write_config(const char *extra) { write_config_tb(false, extra); }

static void wait_reloaded(int n)
{
	for (int t = 0; log_count("config reloaded") < n; t += 20) {
		CHECK(t < 8000);
		sleep_ms(20);
	}
}

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-reload-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);
	snprintf(g_cfg, sizeof g_cfg, "%s/config.toml", g_dir);

	/* start_compositor writes this file itself; it is replaced by renames below. */
	char config[400];
	snprintf(config, sizeof config,
		 "[toolbar]\nenabled = false\n[layout]\ngap_outer = 4\ngap_inner = 4\n"
		 "[[output]]\nname = \"OUT\"\nwidth = %d\nheight = %d\n", OUT_W, OUT_H);
	start_compositor(argv[1], config, true);
	CHECK(log_has("watching"));

	struct client c;
	client_connect(&c);
	g_clients[0] = &c;
	client_bind_keyboard(&c);

	struct window w;
	open_win(&c, &w, "W", 0x00ff0000);
	expect_size(&w, 792, 574, true, "gap_outer = 4: the window fills the area minus 4px each side");

	/* 1. Gaps change live, and the window is resized to match. */
	write_config("[layout]\ngap_outer = 30\ngap_inner = 4\n");
	wait_reloaded(1);
	settle_all();
	expect_size(&w, 740, 540, true, "after reload gap_outer = 30");

	/* 2. A keybinding is removed and another added, without a restart. */
	write_config("[layout]\ngap_outer = 30\ngap_inner = 4\n[keybindings]\n"
		     "\"Mod+Shift+q\" = \"none\"\n\"Mod+x\" = \"close\"\n");
	wait_reloaded(2);
	settle_all();
	w.close_requested = false;
	mod_shift(KEY_Q);
	settle_all();
	CHECK(!w.close_requested);
	mod(KEY_X);
	settle_all();
	CHECK(w.close_requested);

	/* 3. A broken file is rejected and the previous settings stay. */
	int before = log_count("config reloaded");
	write_config("[layout]\ngap_outer = \"wide\"\n");
	for (int t = 0; log_count("config not reloaded") < 1; t += 20) {
		CHECK(t < 8000);
		sleep_ms(20);
	}
	settle_all();
	CHECK(log_count("config reloaded") == before);
	expect_size(&w, 740, 540, true, "an invalid config changes nothing");

	/* 4. The toolbar can be switched on live: the window area shrinks below it. */
	write_config_tb(true, "[layout]\ngap_outer = 30\ngap_inner = 4\n");
	wait_reloaded(before + 1);
	settle_all();
	CHECK(w.cfg_h < 540);

	/* 5. SIGHUP reloads too. */
	write_config("[layout]\ngap_outer = 10\ngap_inner = 4\n");
	wait_reloaded(before + 2);
	settle_all();
	expect_size(&w, 780, 568, true, "back to a small gap");
	kill(g_pid, SIGHUP);
	wait_reloaded(before + 3);

	inject_lines("key 125 1\nkey 42 1\nkey 18 1\nkey 18 0\nkey 42 0\nkey 125 0\n");
	for (int t = 0;; t += 10) {
		int status;
		pid_t r = waitpid(g_pid, &status, WNOHANG);
		if (r == g_pid) {
			g_pid = 0;
			CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
			break;
		}
		CHECK(r == 0 && t < 5000);
		sleep_ms(10);
	}
	wl_display_disconnect(c.d);
	puts("reload: ok");
	return 0;
}
