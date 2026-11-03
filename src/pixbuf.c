#include "pixbuf.h"

#include <stdlib.h>
#include <string.h>

#include "util/util.h"

#define MAX_DIMENSION 32768
#define MAX_BYTES ((size_t)1 << 30)

static atomic_uint_fast64_t next_id = 1;

struct rei_pixbuf *rei_pixbuf_create(int32_t width, int32_t height, bool opaque)
{
	if (width <= 0 || height <= 0 || width > MAX_DIMENSION || height > MAX_DIMENSION)
		return NULL;
	size_t bytes = (size_t)width * (size_t)height * 4;
	if (bytes > MAX_BYTES)
		return NULL;

	struct rei_pixbuf *p = malloc(sizeof *p + bytes);
	if (!p)
		return NULL;
	atomic_init(&p->refs, 1);
	p->id = atomic_fetch_add(&next_id, 1);
	p->width = width;
	p->height = height;
	p->opaque = opaque;
	memset(p->data, 0, bytes);
	return p;
}

struct rei_pixbuf *rei_pixbuf_ref(struct rei_pixbuf *p)
{
	atomic_fetch_add_explicit(&p->refs, 1, memory_order_relaxed);
	return p;
}

void rei_pixbuf_unref(struct rei_pixbuf *p)
{
	if (!p)
		return;
	if (atomic_fetch_sub_explicit(&p->refs, 1, memory_order_acq_rel) == 1)
		free(p);
}
