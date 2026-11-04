#ifndef REI_TEST_PNGW_H
#define REI_TEST_PNGW_H
/* A tiny PNG writer for tests (uncompressed deflate): a solid RGBA colour. Call crc_init() once. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef CHECK
#include "check.h"
#endif

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
static void write_png(const char *path, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha)
{
	size_t row = 1 + (size_t)w * 4, raw_len = row * (size_t)h;
	uint8_t *raw = calloc(raw_len, 1);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			uint8_t *p = raw + (size_t)y * row + 1 + (size_t)x * 4;
			p[0] = r; p[1] = g; p[2] = b; p[3] = alpha;
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


#endif
