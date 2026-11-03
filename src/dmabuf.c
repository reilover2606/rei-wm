#include "dmabuf.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/util.h"

static atomic_uint_fast64_t next_id = 1;

bool rei_format_is_opaque(uint32_t f)
{
	return f == REI_FORMAT_XRGB8888 || f == REI_FORMAT_XBGR8888 ||
	       f == REI_FOURCC('R', 'G', '2', '4') || f == REI_FOURCC('B', 'G', '2', '4') ||
	       f == REI_FOURCC('R', 'G', '1', '6');
}

int rei_format_bpp(uint32_t f)
{
	switch (f) {
	case REI_FORMAT_ARGB8888:
	case REI_FORMAT_XRGB8888:
	case REI_FORMAT_ABGR8888:
	case REI_FORMAT_XBGR8888:
		return 4;
	case REI_FOURCC('R', 'G', '2', '4'):
	case REI_FOURCC('B', 'G', '2', '4'):
		return 3;
	case REI_FOURCC('R', 'G', '1', '6'):
		return 2;
	default:
		return 0;
	}
}

struct rei_dmabuf *rei_dmabuf_create(const struct rei_dmabuf *proto)
{
	struct rei_dmabuf *d = NULL;
	if (proto->n_planes >= 1 && proto->n_planes <= REI_DMABUF_MAX_PLANES &&
	    proto->width > 0 && proto->height > 0)
		d = rei_xcalloc(1, sizeof *d);

	if (!d) {
		for (int i = 0; i < proto->n_planes && i < REI_DMABUF_MAX_PLANES; i++) {
			if (proto->fd[i] >= 0)
				close(proto->fd[i]);
		}
		return NULL;
	}
	*d = *proto;
	atomic_init(&d->refs, 1);
	d->id = atomic_fetch_add(&next_id, 1);
	d->opaque = rei_format_is_opaque(d->format);
	return d;
}

struct rei_dmabuf *rei_dmabuf_ref(struct rei_dmabuf *d)
{
	atomic_fetch_add_explicit(&d->refs, 1, memory_order_relaxed);
	return d;
}

void rei_dmabuf_unref(struct rei_dmabuf *d)
{
	if (!d)
		return;
	if (atomic_fetch_sub_explicit(&d->refs, 1, memory_order_acq_rel) != 1)
		return;
	for (int i = 0; i < d->n_planes; i++) {
		if (d->fd[i] >= 0)
			close(d->fd[i]);
	}
	free(d);
}

bool rei_format_mod_supported(const struct rei_format_mod *list, size_t n,
			      uint32_t format, uint64_t modifier)
{
	for (size_t i = 0; i < n; i++)
		if (list[i].format == format && list[i].modifier == modifier)
			return true;
	return false;
}

size_t rei_format_mod_intersect(const struct rei_format_mod *a, size_t na,
				const struct rei_format_mod *b, size_t nb,
				struct rei_format_mod **out)
{
	*out = NULL;
	if (na == 0 || nb == 0)
		return 0;
	struct rei_format_mod *list = rei_xcalloc(na, sizeof *list);
	size_t count = 0;
	for (size_t i = 0; i < na; i++)
		if (rei_format_mod_supported(b, nb, a[i].format, a[i].modifier))
			list[count++] = a[i];
	if (count == 0) {
		free(list);
		return 0;
	}
	*out = list;
	return count;
}
