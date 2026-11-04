#include <execinfo.h>
#include <getopt.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#ifdef REI_HAVE_DRM
#include "backend/drm/drm.h"
#endif
#include "server.h"
#include "util/log.h"

#define REI_VERSION "0.1.0"

static void usage(FILE *out)
{
	fputs(
"Usage: rei-wm [options]\n"
"\n"
"  -c, --config PATH    config file (default: ~/.config/rei-wm/config.toml,\n"
"                       created on first run)\n"
"  -b, --backend NAME   auto, drm, headless or nested (overrides the config)\n"
"  -s, --socket NAME    Wayland socket name (default: first free wayland-N)\n"
"  -v, --verbose        debug logging (same as REI_LOG=debug)\n"
"      --check-config   validate the config and exit\n"
"      --print-default-config  write the default config file text to stdout\n"
"      --drm-info       list the displays, modes and planes the DRM backend would see\n"
"  -h, --help           show this help\n"
"  -V, --version        show the version\n"
"\n"
"Environment: REI_LOG=error|warn|info|debug\n"
"Signals:     SIGINT/SIGTERM quit; SIGUSR1 forces a redraw (debugging aid)\n",
		out);
}

/*
 * If rei-wm crashes (a segfault, or glibc aborting on a corrupted heap), print where, so a
 * bug report can say more than "it crashed". Not strictly async-signal-safe, but the process is
 * already dying and this has proven reliable in practice.
 */
static void crash_handler(int sig)
{
	static volatile sig_atomic_t in_handler;
	if (in_handler)
		_exit(128 + sig);
	in_handler = 1;
	void *frames[64];
	int n = backtrace(frames, 64);
	const char head[] = "\nrei-wm crashed (signal ";
	ssize_t r = write(STDERR_FILENO, head, sizeof head - 1);
	char num[8] = { (char)('0' + sig / 10), (char)('0' + sig % 10), ')', ':', '\n', 0 };
	r = write(STDERR_FILENO, num, 5);
	(void)r;
	backtrace_symbols_fd(frames, n, STDERR_FILENO);
	signal(sig, SIG_DFL);
	raise(sig);
}

static void install_crash_handler(void)
{
	void *warm[2];
	backtrace(warm, 2);  /* Loads libgcc now, not inside the handler. */
	static const int sigs[] = { SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL };
	for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
		signal(sigs[i], crash_handler);
}

int main(int argc, char **argv)
{
	static const struct option long_opts[] = {
		{ "config", required_argument, NULL, 'c' },
		{ "backend", required_argument, NULL, 'b' },
		{ "socket", required_argument, NULL, 's' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "check-config", no_argument, NULL, 1000 },
		{ "drm-info", no_argument, NULL, 1001 },
		{ "print-default-config", no_argument, NULL, 1002 },
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ 0 },
	};

	const char *config_path = NULL;
	struct rei_server_options opts = { 0 };
	bool verbose = false, check_only = false;

	rei_log_set_thread_name("main");
	install_crash_handler();
	const char *env_level = getenv("REI_LOG");
	if (env_level) {
		int lvl = rei_log_parse_level(env_level);
		if (lvl >= 0)
			rei_log_set_level((enum rei_log_level)lvl);
		else
			rei_warn("ignoring unknown REI_LOG value '%s'", env_level);
	}

	int c;
	while ((c = getopt_long(argc, argv, "c:b:s:vhV", long_opts, NULL)) != -1) {
		switch (c) {
		case 'c': config_path = optarg; break;
		case 'b':
			if (strcmp(optarg, "auto") && strcmp(optarg, "headless") &&
			    strcmp(optarg, "nested") && strcmp(optarg, "drm")) {
				fprintf(stderr, "rei-wm: unknown backend '%s'\n", optarg);
				return 2;
			}
			opts.backend_override = optarg;
			break;
		case 's': opts.socket_name = optarg; break;
		case 'v': verbose = true; break;
		case 1000: check_only = true; break;
		case 1002: fputs(rei_config_default_text(), stdout); return 0;
		case 1001:
#ifdef REI_HAVE_DRM
			return rei_drm_print_info();
#else
			fprintf(stderr, "rei-wm: this build has no DRM backend\n");
			return 1;
#endif
		case 'h': usage(stdout); return 0;
		case 'V': puts("rei-wm " REI_VERSION); return 0;
		default: usage(stderr); return 2;
		}
	}
	if (optind < argc) {
		usage(stderr);
		return 2;
	}
	if (verbose)
		rei_log_set_level(REI_LOG_DEBUG);

	/* An explicit --config must exist; only the default path is auto-created. */
	char *default_path = NULL;
	bool create = false;
	if (!config_path) {
		default_path = rei_config_default_path();
		if (!default_path) {
			fprintf(stderr, "rei-wm: cannot determine config location "
				"(set HOME or XDG_CONFIG_HOME, or pass --config)\n");
			return 1;
		}
		config_path = default_path;
		create = true;
	}

	char err[512];
	struct rei_config *config = rei_config_load(config_path, create, err, sizeof err);
	free(default_path);
	if (!config) {
		fprintf(stderr, "rei-wm: config error: %s\n", err);
		return 1;
	}
	if (check_only) {
		printf("%s: ok\n", config->path ? config->path : "(defaults)");
		rei_config_free(config);
		return 0;
	}

	rei_info("rei-wm " REI_VERSION " starting");
	struct rei_server *server = rei_server_create(config, &opts);
	if (!server) {
		rei_config_free(config);
		return 1;
	}

	int rc = rei_server_run(server);
	rei_server_destroy(server);
	rei_config_free(config);
	rei_info("bye");
	return rc;
}
