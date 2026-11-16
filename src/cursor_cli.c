#include "cursor_cli.h"

#include <cairo.h>
#include <stdio.h>
#include <stdlib.h>

#include "cursor.h"

void rei_cursor_cli_list(void)
{
	size_t n;
	char **names = rei_cursor_list_themes(&n);
	for (size_t i = 0; i < n; i++)
		printf("%s\n", names[i]);
	rei_cursor_free_names(names, n);
}

int rei_cursor_cli_preview(const char *theme, int size, const char *png_path)
{
	static const enum rei_cursor_shape shown[] = {
		REI_SHAPE_DEFAULT, REI_SHAPE_POINTER, REI_SHAPE_TEXT, REI_SHAPE_HELP, REI_SHAPE_WAIT,
		REI_SHAPE_CROSSHAIR, REI_SHAPE_MOVE, REI_SHAPE_NOT_ALLOWED, REI_SHAPE_NS_RESIZE,
		REI_SHAPE_EW_RESIZE, REI_SHAPE_GRAB, REI_SHAPE_NWSE_RESIZE,
	};
	const int n = (int)(sizeof shown / sizeof shown[0]);
	struct rei_cursor_theme *t = rei_cursor_theme_open(theme, size, 1);
	int cell = size + 16;
	int w = cell * n, h = cell * 2;
	cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *cr = cairo_create(surf);
	cairo_set_source_rgb(cr, 0.93, 0.93, 0.93);
	cairo_rectangle(cr, 0, 0, w, cell);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.16, 0.16, 0.18);
	cairo_rectangle(cr, 0, cell, w, cell);
	cairo_fill(cr);

	for (int row = 0; row < 2; row++) {
		for (int i = 0; i < n; i++) {
			const struct rei_cursor_image *img = rei_cursor_theme_shape(t, shown[i]);
			struct rei_pixbuf *pb = img->pixbuf;
			cairo_surface_t *src = cairo_image_surface_create_for_data(pb->data, CAIRO_FORMAT_ARGB32,
										  pb->width, pb->height, pb->width * 4);
			double x = i * cell + (cell - img->width) / 2.0, y = row * cell + (cell - img->height) / 2.0;
			cairo_save(cr);
			cairo_translate(cr, x, y);
			cairo_scale(cr, (double)img->width / pb->width, (double)img->height / pb->height);
			cairo_set_source_surface(cr, src, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
			cairo_paint(cr);
			cairo_restore(cr);
			cairo_surface_destroy(src);
		}
	}
	cairo_destroy(cr);
	cairo_status_t rc = cairo_surface_write_to_png(surf, png_path);
	cairo_surface_destroy(surf);
	rei_cursor_theme_close(t);
	return rc == CAIRO_STATUS_SUCCESS ? 0 : 1;
}
