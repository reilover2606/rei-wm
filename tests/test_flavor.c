/* Flavor icons: only square PNGs of at most 64x64 are accepted; transparency is kept; the choice
 * for a key is stable; and the RAM text. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "icons.h"
#include "pixbuf.h"
#include "pngw.h"
#include "sysinfo.h"
#include "theme.h"

static char g_root[] = "/tmp/rei-flavor-test-XXXXXX";

static void make(const char *name, int w, int h, uint8_t a)
{
	char path[300];
	snprintf(path, sizeof path, "%s/%s", g_root, name);
	write_png(path, w, h, 200, 30, 40, a);
}

int main(void)
{
	crc_init();
	CHECK(mkdtemp(g_root));

	/* Off by default. */
	CHECK(rei_flavor_icon("x") == NULL && rei_flavor_icons_count() == 0);
	CHECK(rei_flavor_icons_configure("") == 0);

	make("ok32.png", 32, 32, 255);
	make("ok64.png", 64, 64, 255);
	make("ok16.png", 16, 16, 128);   /* half transparent */
	make("big65.png", 65, 65, 255);  /* too large */
	make("wide.png", 32, 16, 255);   /* not 1:1 */
	make("tall.png", 16, 32, 255);
	{
		char path[300];
		snprintf(path, sizeof path, "%s/notes.txt", g_root);
		FILE *f = fopen(path, "w");
		CHECK(f);
		fputs("not an image", f);
		fclose(f);
		snprintf(path, sizeof path, "%s/broken.png", g_root);
		f = fopen(path, "w");
		CHECK(f);
		fputs("not really a png", f);
		fclose(f);
	}

	CHECK(rei_flavor_icons_configure(g_root) == 3);   /* only the three valid ones */
	CHECK(rei_flavor_icons_count() == 3);

	/* Stable per key; every answer is one of the accepted sizes; keys spread over the set. */
	int seen[3] = { 0 };
	int sizes[3] = { 16, 32, 64 };
	for (int i = 0; i < 200; i++) {
		char key[32];
		snprintf(key, sizeof key, "app-%d", i);
		struct rei_pixbuf *a = rei_flavor_icon(key), *b = rei_flavor_icon(key);
		CHECK(a && a == b);
		CHECK(a->width == a->height && a->width <= REI_ICON_MAX_SIZE);
		for (int k = 0; k < 3; k++)
			if (a->width == sizes[k])
				seen[k]++;
		if (a->width == 16) {   /* transparency is kept: alpha 128, colour premultiplied */
			CHECK(a->data[3] == 128 && a->data[2] < 200 && a->data[2] > 80);
		}
	}
	CHECK(seen[0] > 0 && seen[1] > 0 && seen[2] > 0);

	/* A missing directory leaves nothing; turning it off clears it. */
	CHECK(rei_flavor_icons_configure("/nonexistent/rei-flavor") == 0 && rei_flavor_icon("x") == NULL);
	CHECK(rei_flavor_icons_configure(g_root) == 3);
	CHECK(rei_flavor_icons_configure(NULL) == 0 && rei_flavor_icon("x") == NULL);

	/* The iconable category. */
	CHECK(rei_element_iconable(REI_ELEM_TRAY_TILE) && rei_element_iconable(REI_ELEM_START_BUTTON) &&
	      rei_element_iconable(REI_ELEM_MENU_ROW));
	CHECK(!rei_element_iconable(REI_ELEM_TOOLTIP) && !rei_element_iconable(REI_ELEM_DIALOG));

	/* RAM as values, one decimal. */
	char text[40];
	rei_sysinfo_format_mem(5452595, 16777216, text, sizeof text);
	CHECK(strcmp(text, "5.2/16.0GB") == 0);
	rei_sysinfo_format_mem(1572864, 8388608, text, sizeof text);
	CHECK(strcmp(text, "1.5/8.0GB") == 0);

	rei_icons_cleanup();
	char cmd[400];
	snprintf(cmd, sizeof cmd, "rm -rf %s", g_root);
	CHECK(system(cmd) == 0);
	printf("flavor: ok\n");
	return 0;
}
