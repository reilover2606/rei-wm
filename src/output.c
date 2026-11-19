#include "output.h"

#include <math.h>
#include <string.h>

#include "util/log.h"

void rei_output_logical_size(const struct rei_output *o, int32_t *w, int32_t *h)
{
	double s = o->scale > 0 ? o->scale : 1.0;
	int32_t lw = (int32_t)lround(o->width / s);
	int32_t lh = (int32_t)lround(o->height / s);
	if (w)
		*w = lw > 0 ? lw : 1;
	if (h)
		*h = lh > 0 ? lh : 1;
}

void rei_outputs_layout(struct wl_list *outputs)
{
	struct rei_output *o;
	int32_t next_x = 0;
	bool any = false;

	wl_list_for_each(o, outputs, link) {
		if (!o->has_position)
			continue;
		int32_t w;
		rei_output_logical_size(o, &w, NULL);
		if (!any || o->x + w > next_x)
			next_x = o->x + w;
		any = true;
	}

	wl_list_for_each(o, outputs, link) {
		if (o->has_position)
			continue;
		int32_t w;
		rei_output_logical_size(o, &w, NULL);
		o->x = next_x;
		o->y = 0;
		next_x += w;
	}
}

void rei_output_frame_stats(struct rei_output *o, uint64_t present_ns)
{
	if (present_ns == 0)
		return;
	if (o->pace.last_ns) {
		uint64_t d = present_ns - o->pace.last_ns;
		o->pace.sum_ns += d;
		o->pace.frames++;
		if (d > o->pace.worst_ns)
			o->pace.worst_ns = d;
		uint64_t refresh_ns = o->refresh_mhz > 0 ? 1000000000000ull / (uint64_t)o->refresh_mhz : 16666667ull;
		if (d > refresh_ns + refresh_ns / 2)
			o->pace.late++;
	}
	o->pace.last_ns = present_ns;
	if (o->pace.window_start_ns == 0)
		o->pace.window_start_ns = present_ns;
	if (present_ns - o->pace.window_start_ns >= 5000000000ull) {
		if (o->pace.frames > 0)
			rei_debug("frames on %s: %u in %.1f s, average spacing %.2f ms, worst %.2f ms, %u late",
				  o->name, o->pace.frames, (double)(present_ns - o->pace.window_start_ns) / 1e9,
				  (double)o->pace.sum_ns / o->pace.frames / 1e6, (double)o->pace.worst_ns / 1e6, o->pace.late);
		memset(&o->pace, 0, sizeof o->pace);
		o->pace.last_ns = present_ns;
		o->pace.window_start_ns = present_ns;
	}
}
