/*
 * Icon lookup for the tray: desktop entries (by file name, StartupWMClass and Exec),
 * themed PNGs, and the letter fallback.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "icons.h"
#include "pixbuf.h"
#include "toolbar.h"

static uint32_t crc_table[256];

static void crc_init(void)
{
	for (uint32_t n = 0; n < 256; n++) {
		uint32_t c = n;
		for (int k = 0; k < 8; k++)
			c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
		crc_table[n] = c;
	}
}

static uint32_t crc(uint32_t c, const uint8_t *buf, size_t len)
{
	c = ~c;
	for (size_t i = 0; i < len; i++)
		c = crc_table[(c ^ buf[i]) & 0xff] ^ (c >> 8);
	return ~c;
}

static void be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
	uint8_t head[8];
	be32(head, len);
	memcpy(head + 4, type, 4);
	fwrite(head, 1, 8, f);
	if (len)
		fwrite(data, 1, len, f);
	uint32_t c = crc(0, (const uint8_t *)type, 4);
	c = crc(c, data, len);
	uint8_t tail[4];
	be32(tail, c);
	fwrite(tail, 1, 4, f);
}

/* A solid-colour RGBA PNG, with the pixel data in uncompressed deflate blocks. */
static void write_png(const char *path, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
	size_t row = 1 + (size_t)w * 4, raw_len = row * (size_t)h;
	uint8_t *raw = calloc(raw_len, 1);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			uint8_t *p = raw + (size_t)y * row + 1 + (size_t)x * 4;
			p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
		}
	}
	/* zlib: header, stored blocks of at most 65535 bytes, adler32. */
	size_t cap = raw_len + raw_len / 65535 * 5 + 64;
	uint8_t *z = malloc(cap);
	size_t n = 0;
	z[n++] = 0x78;
	z[n++] = 0x01;
	for (size_t off = 0; off < raw_len;) {
		size_t blk = raw_len - off > 65535 ? 65535 : raw_len - off;
		z[n++] = off + blk == raw_len;
		z[n++] = (uint8_t)(blk & 0xff);
		z[n++] = (uint8_t)(blk >> 8);
		z[n++] = (uint8_t)(~blk & 0xff);
		z[n++] = (uint8_t)((~blk >> 8) & 0xff);
		memcpy(z + n, raw + off, blk);
		n += blk;
		off += blk;
	}
	uint32_t a = 1, bb = 0;
	for (size_t i = 0; i < raw_len; i++) {
		a = (a + raw[i]) % 65521;
		bb = (bb + a) % 65521;
	}
	be32(z + n, (bb << 16) | a);
	n += 4;

	FILE *f = fopen(path, "wb");
	CHECK(f);
	fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
	uint8_t ihdr[13];
	be32(ihdr, (uint32_t)w);
	be32(ihdr + 4, (uint32_t)h);
	ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = ihdr[11] = ihdr[12] = 0;
	chunk(f, "IHDR", ihdr, 13);
	chunk(f, "IDAT", z, (uint32_t)n);
	chunk(f, "IEND", NULL, 0);
	fclose(f);
	free(raw);
	free(z);
}

static void write_text(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");
	CHECK(f);
	fputs(text, f);
	fclose(f);
}

static bool is_colour(const struct rei_pixbuf *p, uint8_t r, uint8_t g, uint8_t b)
{
	/* BGRA, premultiplied; the picture is opaque everywhere. */
	const uint8_t *d = p->data;
	return d[0] == b && d[1] == g && d[2] == r && d[3] == 255;
}

int main(void)
{
	crc_init();
	char base[] = "/tmp/rei-icons-test-XXXXXX";
	CHECK(mkdtemp(base));
	char path[600];
	snprintf(path, sizeof path, "%s/applications", base);
	mkdir(path, 0755);
	snprintf(path, sizeof path, "%s/icons", base);
	mkdir(path, 0755);
	snprintf(path, sizeof path, "%s/icons/hicolor", base);
	mkdir(path, 0755);
	snprintf(path, sizeof path, "%s/icons/hicolor/48x48", base);
	mkdir(path, 0755);
	snprintf(path, sizeof path, "%s/icons/hicolor/48x48/apps", base);
	mkdir(path, 0755);

	/* A desktop entry whose file name is the app id, one found by StartupWMClass, one by Exec. */
	snprintf(path, sizeof path, "%s/applications/org.example.Alpha.desktop", base);
	write_text(path, "[Desktop Entry]\nName=Alpha App\nIcon=alpha-icon\nExec=/usr/bin/alpha %u\n");
	snprintf(path, sizeof path, "%s/applications/bravo.desktop", base);
	write_text(path, "[Desktop Entry]\nName=Bravo\nIcon=bravo-icon\nStartupWMClass=BravoClass\n");
	snprintf(path, sizeof path, "%s/applications/charlie-launcher.desktop", base);
	write_text(path, "[Desktop Entry]\nName=Charlie\nIcon=charlie-icon\nExec=env FOO=1 charlie\n");
	snprintf(path, sizeof path, "%s/icons/hicolor/48x48/apps/alpha-icon.png", base);
	write_png(path, 8, 8, 200, 10, 20);
	snprintf(path, sizeof path, "%s/icons/hicolor/48x48/apps/bravo-icon.png", base);
	write_png(path, 8, 8, 10, 200, 20);
	/* An app whose id is itself an icon name, with no desktop entry. */
	snprintf(path, sizeof path, "%s/icons/hicolor/48x48/apps/plainapp.png", base);
	write_png(path, 8, 8, 10, 20, 200);

	setenv("XDG_DATA_HOME", base, 1);
	setenv("XDG_DATA_DIRS", "/nonexistent", 1);

	struct rei_pixbuf *a = rei_icon_for_app("Alpha", "x");   /* the last part of org.example.Alpha */
	CHECK(a && a->width == 8 && is_colour(a, 200, 10, 20));
	CHECK(rei_icon_for_app("Alpha", "x") == a);               /* cached */
	struct rei_pixbuf *a2 = rei_icon_for_app("org.example.Alpha", "x");
	CHECK(a2 && is_colour(a2, 200, 10, 20));
	CHECK(strcmp(rei_icon_app_name("Alpha"), "Alpha App") == 0);

	struct rei_pixbuf *b = rei_icon_for_app("BravoClass", "x");   /* StartupWMClass, any case */
	CHECK(b && is_colour(b, 10, 200, 20));
	struct rei_pixbuf *b2 = rei_icon_for_app("bravoclass", "x");
	CHECK(b2 && is_colour(b2, 10, 200, 20));

	struct rei_pixbuf *p = rei_icon_for_app("plainapp", "x");   /* the id names the icon */
	CHECK(p && is_colour(p, 10, 20, 200));

	/* Charlie's entry has no icon file: a letter tile built from its name, not a crash. */
	struct rei_pixbuf *c = rei_icon_for_app("charlie", "x");
	CHECK(c && c->width == 48 && c->height == 48);
	CHECK(strcmp(rei_icon_app_name("charlie"), "Charlie") == 0);

	/* Nothing at all known: a letter tile, distinct colours for distinct names. */
	struct rei_pixbuf *u1 = rei_icon_for_app("", "Unknown One");
	struct rei_pixbuf *u2 = rei_icon_for_app("", "Zed");
	CHECK(u1 && u2 && u1 != u2);
	CHECK(rei_icon_app_name("no-such-app") == NULL);
	CHECK(memcmp(u1->data + (24 * 48 + 8) * 4, u2->data + (24 * 48 + 8) * 4, 4) != 0);

	rei_icons_cleanup();
	puts("icons: ok");
	return 0;
}
