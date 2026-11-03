#ifndef REI_WALLPAPER_H
#define REI_WALLPAPER_H

#include <stddef.h>
#include <stdint.h>

#include "pixbuf.h"

/* A decoded image: 8-bit RGBA, straight (non-premultiplied) alpha. */
struct rei_image {
	int32_t width, height;
	uint8_t *rgba;
};

enum rei_wallpaper_mode {
	REI_WALLPAPER_FILL,
	REI_WALLPAPER_FIT,
	REI_WALLPAPER_STRETCH,
	REI_WALLPAPER_CENTER,
	REI_WALLPAPER_TILE,
};

/* Returns -1 for an unknown name. */
int rei_wallpaper_parse_mode(const char *name);

/* NULL on failure, with a reason in err. */
struct rei_image *rei_image_load(const char *path, char *err, size_t errlen);
void rei_image_free(struct rei_image *img);

/*
 * Produces an opaque w x h pixbuf showing `img` laid out per `mode` over the
 * background colour 0xRRGGBB. Pass a NULL image for a plain colour.
 */
struct rei_pixbuf *rei_wallpaper_render(const struct rei_image *img,
					enum rei_wallpaper_mode mode,
					uint32_t background_rgb,
					int32_t w, int32_t h);

#endif
