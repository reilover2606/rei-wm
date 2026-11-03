#include "cursor.h"

#include <string.h>

/* 'X' = black outline, '.' = white fill, ' ' = transparent. */
static const char *const arrow[] = {
	"X           ",
	"XX          ",
	"X.X         ",
	"X..X        ",
	"X...X       ",
	"X....X      ",
	"X.....X     ",
	"X......X    ",
	"X.......X   ",
	"X........X  ",
	"X.....XXXXX ",
	"X..X..X     ",
	"X.X X..X    ",
	"XX  X..X    ",
	"X    X..X   ",
	"     X..X   ",
	"      X..X  ",
	"      X..X  ",
	"       XX   ",
};
#define ARROW_W 12
#define ARROW_H ((int)(sizeof arrow / sizeof arrow[0]))

const struct rei_cursor_image *rei_cursor_default(void)
{
	static struct rei_cursor_image image;
	if (image.pixbuf)
		return &image;

	struct rei_pixbuf *pb = rei_pixbuf_create(ARROW_W, ARROW_H, false);
	if (!pb)
		return NULL;
	for (int y = 0; y < ARROW_H; y++) {
		for (int x = 0; x < ARROW_W; x++) {
			uint8_t *p = pb->data + ((size_t)y * ARROW_W + (size_t)x) * 4;
			char c = arrow[y][x];
			uint8_t v = (c == '.') ? 255 : 0;
			if (c == 'X' || c == '.') {
				p[0] = p[1] = p[2] = v;  /* B, G, R */
				p[3] = 255;
			}
		}
	}
	image.pixbuf = pb;
	image.hotspot_x = 0;
	image.hotspot_y = 0;
	return &image;
}
