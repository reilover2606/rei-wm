#include "snapshot.h"

#include <stdlib.h>

#include "util/util.h"

struct rei_scene_snapshot *rei_snapshot_create(uint64_t serial)
{
	struct rei_scene_snapshot *s = rei_xcalloc(1, sizeof *s);
	atomic_init(&s->refs, 1);
	s->serial = serial;
	return s;
}

struct rei_scene_snapshot *rei_snapshot_ref(struct rei_scene_snapshot *s)
{
	atomic_fetch_add_explicit(&s->refs, 1, memory_order_relaxed);
	return s;
}

void rei_snapshot_unref(struct rei_scene_snapshot *s)
{
	if (!s)
		return;
	if (atomic_fetch_sub_explicit(&s->refs, 1, memory_order_acq_rel) == 1)
		free(s);
}
