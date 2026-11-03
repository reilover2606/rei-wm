#include "scene.h"

#include "cursor.h"
#include "desktop.h"
#include "proto/xdg_shell.h"
#include "server.h"

struct rei_scene_snapshot *rei_scene_build(struct rei_server *s, uint64_t serial)
{
	struct rei_scene_snapshot *snap = rei_snapshot_create(serial);

	uint32_t bg = s->config->background_rgb;
	snap->clear_color[0] = (float)((bg >> 16) & 0xff) / 255.0f;
	snap->clear_color[1] = (float)((bg >> 8) & 0xff) / 255.0f;
	snap->clear_color[2] = (float)(bg & 0xff) / 255.0f;

	struct rei_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		if (!o->wallpaper)
			continue;
		int32_t w, h;
		rei_output_logical_size(o, &w, &h);
		rei_snapshot_add(snap, o->wallpaper, (float)o->x, (float)o->y,
				 (float)w, (float)h, true);
	}

	/* Each output draws its own workspace's windows. */
	wl_list_for_each(o, &s->outputs, link)
		rei_wm_emit_views(s, o, snap);
	rei_wm_emit_decorations(s, snap);
	rei_xdg_emit_popups(s, snap);

	const struct rei_cursor_image *cur = rei_cursor_default();
	if (cur) {
		rei_snapshot_add(snap, cur->pixbuf,
				 (float)(s->cursor_x - cur->hotspot_x),
				 (float)(s->cursor_y - cur->hotspot_y),
				 (float)cur->pixbuf->width, (float)cur->pixbuf->height, false);
	}
	return snap;
}
