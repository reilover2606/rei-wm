#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "wallpaper.h"

struct rgb {
	int r, g, b;
};

static struct rgb px(const struct rei_pixbuf *p, int x, int y)
{
	const uint8_t *d = p->data + ((size_t)y * (size_t)p->width + (size_t)x) * 4;
	CHECK(d[3] == 255);  /* wallpapers are always opaque */
	return (struct rgb){ d[2], d[1], d[0] };  /* BGRA in memory */
}

static void expect(const struct rei_pixbuf *p, int x, int y, int r, int g, int b)
{
	struct rgb c = px(p, x, y);
	if (abs(c.r - r) > 1 || abs(c.g - g) > 1 || abs(c.b - b) > 1) {
		fprintf(stderr, "pixel (%d,%d): got (%d,%d,%d), expected (%d,%d,%d)\n",
			x, y, c.r, c.g, c.b, r, g, b);
		exit(1);
	}
}

static struct rei_image *solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	struct rei_image *img = malloc(sizeof *img);
	img->width = w;
	img->height = h;
	img->rgba = malloc((size_t)w * (size_t)h * 4);
	for (int i = 0; i < w * h; i++) {
		img->rgba[i * 4 + 0] = r;
		img->rgba[i * 4 + 1] = g;
		img->rgba[i * 4 + 2] = b;
		img->rgba[i * 4 + 3] = a;
	}
	return img;
}

static void free_image(struct rei_image *img)
{
	free(img->rgba);
	free(img);
}

#define BG 0x102030

int main(void)
{
	CHECK(rei_wallpaper_parse_mode("fill") == REI_WALLPAPER_FILL);
	CHECK(rei_wallpaper_parse_mode("tile") == REI_WALLPAPER_TILE);
	CHECK(rei_wallpaper_parse_mode("zoom") == -1);

	/* No image: just the background colour. */
	struct rei_pixbuf *p = rei_wallpaper_render(NULL, REI_WALLPAPER_FILL, BG, 3, 3);
	CHECK(p && p->opaque);
	expect(p, 1, 1, 0x10, 0x20, 0x30);
	rei_pixbuf_unref(p);

	/* A 2x2 image with four distinct texels. */
	struct rei_image *quad = solid(2, 2, 0, 0, 0, 255);
	const uint8_t colors[4][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 255 } };
	for (int i = 0; i < 4; i++)
		memcpy(quad->rgba + i * 4, colors[i], 3);

	/* Centre: 1:1, surrounded by the background colour. */
	p = rei_wallpaper_render(quad, REI_WALLPAPER_CENTER, BG, 6, 4);
	expect(p, 2, 1, 255, 0, 0);
	expect(p, 3, 1, 0, 255, 0);
	expect(p, 2, 2, 0, 0, 255);
	expect(p, 3, 2, 255, 255, 255);
	expect(p, 1, 1, 0x10, 0x20, 0x30);
	expect(p, 4, 2, 0x10, 0x20, 0x30);
	expect(p, 2, 0, 0x10, 0x20, 0x30);
	expect(p, 2, 3, 0x10, 0x20, 0x30);
	rei_pixbuf_unref(p);

	/* Tile: the 2x2 pattern repeats with period 2. */
	p = rei_wallpaper_render(quad, REI_WALLPAPER_TILE, BG, 5, 5);
	for (int y = 0; y < 5; y++) {
		for (int x = 0; x < 5; x++) {
			int t = (y % 2) * 2 + (x % 2);
			expect(p, x, y, colors[t][0], colors[t][1], colors[t][2]);
		}
	}
	rei_pixbuf_unref(p);
	free_image(quad);

	/* Fit letterboxes; fill covers everything; stretch ignores aspect ratio. */
	struct rei_image *wide = solid(2, 1, 255, 0, 0, 255);
	p = rei_wallpaper_render(wide, REI_WALLPAPER_FIT, BG, 4, 4);
	expect(p, 1, 0, 0x10, 0x20, 0x30);
	expect(p, 1, 3, 0x10, 0x20, 0x30);
	expect(p, 0, 1, 255, 0, 0);
	expect(p, 3, 2, 255, 0, 0);
	rei_pixbuf_unref(p);

	p = rei_wallpaper_render(wide, REI_WALLPAPER_FILL, BG, 4, 4);
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++)
			expect(p, x, y, 255, 0, 0);
	rei_pixbuf_unref(p);

	p = rei_wallpaper_render(wide, REI_WALLPAPER_STRETCH, BG, 7, 3);
	expect(p, 0, 0, 255, 0, 0);
	expect(p, 6, 2, 255, 0, 0);
	rei_pixbuf_unref(p);
	free_image(wide);

	/* A fully transparent image shows the background through it; a half
	 * transparent one blends with it. */
	struct rei_image *clear = solid(2, 2, 255, 255, 255, 0);
	p = rei_wallpaper_render(clear, REI_WALLPAPER_STRETCH, BG, 4, 4);
	expect(p, 1, 1, 0x10, 0x20, 0x30);
	rei_pixbuf_unref(p);
	free_image(clear);

	struct rei_image *half = solid(2, 2, 255, 255, 255, 128);
	p = rei_wallpaper_render(half, REI_WALLPAPER_STRETCH, BG, 4, 4);
	/* 255 * 128/255 + bg * (1 - 128/255) */
	expect(p, 1, 1, 128 + 16 * 127 / 255, 128 + 32 * 127 / 255, 128 + 48 * 127 / 255);
	rei_pixbuf_unref(p);
	free_image(half);

	puts("test_wallpaper: ok");
	return 0;
}
