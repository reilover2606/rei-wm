#include "wallpaper.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../third_party/stb_image.h"
#include "util/util.h"

int rei_wallpaper_parse_mode(const char *name)
{
	static const char *const names[] = { "fill", "fit", "stretch", "center", "tile" };
	for (size_t i = 0; i < REI_ARRAY_LEN(names); i++) {
		if (strcmp(name, names[i]) == 0)
			return (int)i;
	}
	return -1;
}

struct rei_image *rei_image_load(const char *path, char *err, size_t errlen)
{
	int w, h, channels;
	uint8_t *px = stbi_load(path, &w, &h, &channels, 4);
	if (!px) {
		snprintf(err, errlen, "%s: %s", path, stbi_failure_reason());
		return NULL;
	}
	struct rei_image *img = rei_xcalloc(1, sizeof *img);
	img->width = w;
	img->height = h;
	img->rgba = px;
	return img;
}

void rei_image_free(struct rei_image *img)
{
	if (!img)
		return;
	stbi_image_free(img->rgba);
	free(img);
}

struct rgb {
	float r, g, b;
};

/* Samples at continuous image coordinates (pixel centres at +0.5), clamping at
 * the edges, and composites the result over `bg`. */
static struct rgb sample(const struct rei_image *img, float u, float v, struct rgb bg)
{
	float fx = u - 0.5f, fy = v - 0.5f;
	int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
	float tx = fx - (float)x0, ty = fy - (float)y0;

	float acc[4] = { 0, 0, 0, 0 };
	for (int j = 0; j < 2; j++) {
		for (int i = 0; i < 2; i++) {
			int x = x0 + i, y = y0 + j;
			x = x < 0 ? 0 : x >= img->width ? img->width - 1 : x;
			y = y < 0 ? 0 : y >= img->height ? img->height - 1 : y;
			float wgt = (i ? tx : 1.0f - tx) * (j ? ty : 1.0f - ty);
			const uint8_t *p = img->rgba + ((size_t)y * (size_t)img->width + (size_t)x) * 4;
			float a = (float)p[3] / 255.0f;
			/* Interpolate premultiplied so transparent texels don't bleed. */
			acc[0] += wgt * (float)p[0] * a;
			acc[1] += wgt * (float)p[1] * a;
			acc[2] += wgt * (float)p[2] * a;
			acc[3] += wgt * a;
		}
	}
	float inv = 1.0f - acc[3];
	return (struct rgb){ acc[0] + bg.r * inv, acc[1] + bg.g * inv, acc[2] + bg.b * inv };
}

static uint8_t to_byte(float v)
{
	v = v < 0 ? 0 : v > 255 ? 255 : v;
	return (uint8_t)(v + 0.5f);
}

struct rei_pixbuf *rei_wallpaper_render(const struct rei_image *img,
					enum rei_wallpaper_mode mode,
					uint32_t background_rgb,
					int32_t w, int32_t h)
{
	struct rei_pixbuf *out = rei_pixbuf_create(w, h, true);
	if (!out)
		return NULL;

	struct rgb bg = {
		(float)((background_rgb >> 16) & 0xff),
		(float)((background_rgb >> 8) & 0xff),
		(float)(background_rgb & 0xff),
	};

	/* Map each output pixel back to image space as
	 * (px - off) / scale, per axis; `inside` limits where the image applies. */
	float sx = 1, sy = 1, offx = 0, offy = 0;
	bool tile = false;
	if (img) {
		float iw = (float)img->width, ih = (float)img->height;
		float fw = (float)w, fh = (float)h;
		switch (mode) {
		case REI_WALLPAPER_FILL:
			sx = sy = fmaxf(fw / iw, fh / ih);
			break;
		case REI_WALLPAPER_FIT:
			sx = sy = fminf(fw / iw, fh / ih);
			break;
		case REI_WALLPAPER_STRETCH:
			sx = fw / iw;
			sy = fh / ih;
			break;
		case REI_WALLPAPER_CENTER:
		case REI_WALLPAPER_TILE:
			sx = sy = 1.0f;
			tile = mode == REI_WALLPAPER_TILE;
			break;
		}
		offx = tile ? 0 : (fw - iw * sx) / 2.0f;
		offy = tile ? 0 : (fh - ih * sy) / 2.0f;
	}

	for (int32_t y = 0; y < h; y++) {
		uint8_t *row = out->data + (size_t)y * (size_t)w * 4;
		for (int32_t x = 0; x < w; x++) {
			struct rgb c = bg;
			if (img) {
				float u = ((float)x + 0.5f - offx) / sx;
				float v = ((float)y + 0.5f - offy) / sy;
				if (tile) {
					u = fmodf(u, (float)img->width);
					v = fmodf(v, (float)img->height);
				}
				if (u >= 0 && v >= 0 && u < (float)img->width && v < (float)img->height)
					c = sample(img, u, v, bg);
			}
			/* BGRA in memory. */
			row[x * 4 + 0] = to_byte(c.b);
			row[x * 4 + 1] = to_byte(c.g);
			row[x * 4 + 2] = to_byte(c.r);
			row[x * 4 + 3] = 255;
		}
	}
	return out;
}
