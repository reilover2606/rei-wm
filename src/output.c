#include "output.h"

#include <math.h>

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
