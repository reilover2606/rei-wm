#include "theme_cli.h"

#include <cairo.h>
#include <stdio.h>
#include <string.h>

#include <stdlib.h>

#include "config.h"
#include "render/gles.h"
#include "shader.h"
#include "snapshot.h"
#include "toolbar.h"
#include "util/util.h"

static void print_theme(const struct rei_theme *theme)
{
	for (int e = 0; e < REI_ELEM_COUNT; e++) {
		const struct rei_style *s = &theme->e[e];
		const char *n = rei_element_name((enum rei_element)e);
		printf("%s.color=#%06x\n", n, s->color);
		printf("%s.active_color=#%06x\n", n, s->active_color);
		printf("%s.inactive_color=#%06x\n", n, s->inactive_color);
		printf("%s.text_color=#%06x\n", n, s->text_color);
		printf("%s.active_text_color=#%06x\n", n, s->active_text_color);
		printf("%s.inactive_text_color=#%06x\n", n, s->inactive_text_color);
		printf("%s.border_color=#%06x\n", n, s->border_color);
		printf("%s.border_width=%g\n", n, s->border_width);
		printf("%s.shape=%s\n", n, rei_shape_name(s->shape));
		printf("%s.radius=%g\n", n, s->radius);
		printf("%s.opacity=%g\n", n, s->opacity);
		printf("%s.shadow_color=#%06x\n", n, s->shadow.color);
		printf("%s.shadow_opacity=%g\n", n, s->shadow.opacity);
		printf("%s.shadow_offset_x=%g\n", n, s->shadow.dx);
		printf("%s.shadow_offset_y=%g\n", n, s->shadow.dy);
		printf("%s.shadow_blur=%g\n", n, s->shadow.blur);
		printf("%s.glow_color=#%06x\n", n, s->glow.color);
		printf("%s.glow_size=%g\n", n, s->glow.size);
		printf("%s.glow_opacity=%g\n", n, s->glow.opacity);
		printf("%s.text_glow_size=%g\n", n, s->text_fx.glow_size);
		printf("%s.text_glow_strength=%g\n", n, s->text_fx.glow_strength);
		printf("%s.text_glow_color=#%06x\n", n, s->text_fx.glow_color);
		printf("%s.text_glow_tint=%g\n", n, s->text_fx.glow_tint);
		printf("%s.text_blink=%g\n", n, s->text_fx.blink);
		printf("%s.text_shine=%g\n", n, s->text_fx.shine);
		printf("%s.text_shine_color=#%06x\n", n, s->text_fx.shine_color);
		if (rei_element_button_parent((enum rei_element)e) >= 0)
			printf("%s.mode=%s\n", n, rei_button_mode_name(s->button_mode));
		if (e == REI_ELEM_WINDOW_TEXT) {
			printf("%s.apps=%s\n", n, theme->window_apps);
			printf("%s.shader=%s\n", n, theme->window_shader);
			printf("%s.glow_threshold=%g\n", n, theme->window_threshold);
			printf("%s.glow_area_limit=%g\n", n, theme->window_area_limit);
		}
		if (rei_element_iconable((enum rei_element)e)) {
			printf("%s.icon=%s\n", n, s->icon ? "true" : "false");
			printf("%s.icon_size=%g\n", n, s->icon_size);
		}
	}
}

void rei_theme_print(const struct rei_config *cfg)
{
	print_theme(&cfg->theme);
}

void rei_theme_print_defaults(const struct rei_config *cfg)
{
	struct rei_theme t;
	rei_theme_defaults(&t, cfg->toolbar_background, cfg->toolbar_foreground, cfg->toolbar_highlight);
	print_theme(&t);
}

/* ------------------------------------------------------------------------ */

/* The pictures of the preview, in the order they go on top of each other. They are composed at the end by the
 * real renderer, so that what a shader does to text (glow, shine) shows as it will on screen. */
struct item {
	struct rei_pixbuf *pb;
	double x, y, alpha;
	struct rei_fx fx;   /* for a sample window */
};
static struct item g_items[64];
static int g_n_items;

static void blit(cairo_t *cr, const struct rei_pixbuf *pb, double x, double y, double alpha)
{
	(void)cr;
	if (!pb || g_n_items == (int)REI_ARRAY_LEN(g_items))
		return;
	g_items[g_n_items++] = (struct item){ rei_pixbuf_ref((struct rei_pixbuf *)pb), x, y, alpha, { 0 } };
}

/* Without OpenGL: the pictures painted one over the other, text layers as plain text. */
static void paint_item(cairo_t *cr, const struct rei_pixbuf *pb, double x, double y, double alpha, bool upper_half)
{
	int h = upper_half ? pb->height / 2 : pb->height;
	cairo_surface_t *s = cairo_image_surface_create_for_data((unsigned char *)pb->data, CAIRO_FORMAT_ARGB32,
								 pb->width, h, pb->width * 4);
	cairo_set_source_surface(cr, s, x, y);
	cairo_paint_with_alpha(cr, alpha);
	cairo_surface_destroy(s);
}

/* Composes the base picture and the items into `surface`. */
static void compose(cairo_surface_t *surface, int W, int H)
{
	cairo_surface_flush(surface);
	struct rei_gles *g = rei_gles_create(NULL);
	struct rei_pixbuf *base = g ? rei_pixbuf_create_uninit(W, H, true) : NULL;
	if (base) {
		const uint8_t *src = cairo_image_surface_get_data(surface);
		int stride = cairo_image_surface_get_stride(surface);
		for (int y = 0; y < H; y++)
			memcpy(base->data + (size_t)y * (size_t)W * 4, src + (size_t)y * (size_t)stride, (size_t)W * 4);
		struct rei_scene_snapshot *snap = rei_snapshot_create(1);
		rei_snapshot_add(snap, base, 0, 0, (float)W, (float)H, true);
		for (int i = 0; i < g_n_items; i++) {
			const struct item *it = &g_items[i];
			size_t first = snap->n_items;
			rei_snapshot_add(snap, it->pb, (float)it->x, (float)it->y, (float)it->pb->width,
					 (float)it->pb->height, false);
			rei_snapshot_set_alpha_from(snap, first, (float)it->alpha);
			rei_snapshot_set_fx_from(snap, first, &it->fx);
		}
		struct rei_gles_target target = { .width = W, .height = H, .scale = 1.0 };
		uint8_t *rgba = rei_xcalloc((size_t)W * (size_t)H, 4);
		bool ok = rei_gles_draw(g, snap, &target) && rei_gles_read_pixels(g, rgba);
		rei_snapshot_unref(snap);
		rei_pixbuf_unref(base);
		if (ok) {
			uint8_t *dst = cairo_image_surface_get_data(surface);
			int stride2 = cairo_image_surface_get_stride(surface);
			for (int y = 0; y < H; y++)
				for (int x = 0; x < W; x++) {
					const uint8_t *p = rgba + ((size_t)y * (size_t)W + (size_t)x) * 4;
					uint8_t *q = dst + (size_t)y * (size_t)stride2 + (size_t)x * 4;
					q[0] = p[2]; q[1] = p[1]; q[2] = p[0]; q[3] = 255;
				}
			cairo_surface_mark_dirty(surface);
		}
		free(rgba);
		rei_gles_destroy(g);
		if (ok)
			return;
	} else if (g) {
		rei_gles_destroy(g);
	}
	cairo_t *cr = cairo_create(surface);
	for (int i = 0; i < g_n_items; i++) {
		const struct item *it = &g_items[i];
		paint_item(cr, it->pb, it->x, it->y, it->alpha, false);
		for (int k = 0; k < it->pb->n_layers; k++)
			paint_item(cr, it->pb->layers[k].pix, it->x, it->y, it->alpha, true);
	}
	cairo_destroy(cr);
}

/* A stand-in for a terminal window, for [theme.window_text]: a few lines of bright text on black. */
static struct rei_pixbuf *sample_terminal(int w, int h)
{
	struct rei_pixbuf *pb = rei_pixbuf_create(w, h, true);
	if (!pb)
		return NULL;
	cairo_surface_t *s = cairo_image_surface_create_for_data(pb->data, CAIRO_FORMAT_RGB24, w, h, w * 4);
	cairo_t *cr = cairo_create(s);
	cairo_set_source_rgb(cr, 0.03, 0.03, 0.05);
	cairo_paint(cr);
	cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 13);
	static const struct { const char *text; double r, g, b; } lines[] = {
		{ "scribe@thinkpad ~ $ ls --color", 0.35, 1.0, 0.45 },
		{ "Documents  Music  rei-wm  notes.txt", 0.45, 0.75, 1.0 },
		{ "scribe@thinkpad ~ $ echo glow", 0.35, 1.0, 0.45 },
		{ "glow", 0.95, 0.95, 0.95 },
	};
	for (size_t i = 0; i < REI_ARRAY_LEN(lines); i++) {
		cairo_set_source_rgb(cr, lines[i].r, lines[i].g, lines[i].b);
		cairo_move_to(cr, 10, 20 + 18.0 * (double)i);
		cairo_show_text(cr, lines[i].text);
	}
	cairo_destroy(cr);
	cairo_surface_destroy(s);
	return pb;
}

static void label(cairo_t *cr, const char *text, double x, double y, uint32_t fg)
{
	cairo_set_source_rgba(cr, ((fg >> 16) & 0xff) / 255.0, ((fg >> 8) & 0xff) / 255.0, (fg & 0xff) / 255.0, 0.7);
	cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 11);
	cairo_move_to(cr, x, y);
	cairo_show_text(cr, text);
}

int rei_theme_preview(const struct rei_config *cfg, const char *png_path)
{
	const int W = 1000, H = 710;
	g_n_items = 0;
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
	cairo_t *cr = cairo_create(surface);
	uint32_t bg = cfg->background_rgb;
	/* The desktop, with a faint "window" so translucent things have something to show over. */
	cairo_set_source_rgb(cr, ((bg >> 16) & 0xff) / 255.0, ((bg >> 8) & 0xff) / 255.0, (bg & 0xff) / 255.0);
	cairo_paint(cr);
	cairo_set_source_rgb(cr, 0.28, 0.45, 0.72);
	cairo_rectangle(cr, 10, 310, 330, 220);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.85, 0.55, 0.25);
	cairo_rectangle(cr, 340, 310, 200, 220);
	cairo_fill(cr);
	uint32_t hint = cfg->toolbar_foreground;

	/* The toolbar. */
	struct rei_bar_state st;
	memset(&st, 0, sizeof st);
	st.position = REI_BAR_TOP;
	st.thickness = cfg->toolbar_thickness > 0 ? cfg->toolbar_thickness : 28;
	st.length = W - 20;
	st.scale = 1;
	st.n_buttons = 3;
	st.buttons[0] = (struct rei_bar_button){ 1, REI_BAR_BUTTON_FOCUSED };
	st.buttons[1] = (struct rei_bar_button){ 2, REI_BAR_BUTTON_VISIBLE };
	st.buttons[2] = (struct rei_bar_button){ 3, REI_BAR_BUTTON_HIDDEN };
	st.hover = -1;
	snprintf(st.clock, sizeof st.clock, "12:34:56");
	snprintf(st.font, sizeof st.font, "%s", cfg->toolbar_font);
	st.tray_mode = REI_TRAY_ICONS_NAMES;
	st.tray_min_percent = 50;
	st.tray_hover = -1;
	st.drag_index = -1;
	struct rei_pixbuf *icons[3];
	static const char *const names[3] = { "Terminal", "Browser", "Editor" };
	for (int i = 0; i < 3; i++) {
		icons[i] = rei_bar_letter_icon(names[i]);
		snprintf(st.tray[i].label, sizeof st.tray[i].label, "%s", i == 1 ? "Browser (2)" : names[i]);
		st.tray[i].count = i == 1 ? 2 : 1;
		st.tray[i].active = i == 0;
		st.tray[i].minimized = i == 2;
		st.tray[i].icon = icons[i];
	}
	st.n_tray = 3;
	st.styled = true;
	st.theme = cfg->theme;
	st.start_w = rei_bar_start_extent(st.position, st.thickness);
	snprintf(st.start_label, sizeof st.start_label, "%s", cfg->start_label);
	st.sysinfo_on = true;
	st.sysinfo_cpu = 37;
	st.sysinfo_mem_used_kb = 5452595;
	st.sysinfo_mem_total_kb = 16777216;
	st.mode_w = st.thickness;
	st.wifi_present = true;
	st.wifi_connected = true;
	st.wifi_level = 2;
	st.battery_present = true;
	st.battery_percent = 63;
	snprintf(st.volume, sizeof st.volume, "73%%");
	st.background = cfg->toolbar_background;
	st.foreground = cfg->toolbar_foreground;
	st.highlight = cfg->toolbar_highlight;
	struct rei_pixbuf *bar = rei_bar_render(&st);
	blit(cr, bar, 10, 10, 1.0);
	rei_pixbuf_unref(bar);
	for (int i = 0; i < 3; i++)
		rei_pixbuf_unref(icons[i]);

	/* The Start menu, with a hovered row. */
	label(cr, "Start menu", 10, 56, hint);
	struct rei_menu_state ms;
	memset(&ms, 0, sizeof ms);
	static const char *const rows[] = { "Terminal", "Browser", "Run", "Logout", "Shutdown", "Reboot" };
	for (int i = 0; i < 6; i++) {
		snprintf(ms.rows[i].label, sizeof ms.rows[i].label, "%s", rows[i]);
		ms.rows[i].gap_before = (i == 2 || i == 3) ? 9 : 0;
	}
	ms.n_rows = 6;
	ms.hover = 1;
	ms.scale = 1;
	snprintf(ms.font, sizeof ms.font, "%s", cfg->toolbar_font);
	ms.menu = cfg->theme.e[REI_ELEM_MENU];
	ms.row = cfg->theme.e[REI_ELEM_MENU_ROW];
	int mw, mh, mrow, mm;
	rei_bar_menu_size(&ms, &mw, &mh, &mrow, &mm);
	struct rei_pixbuf *menu = rei_bar_menu_render(&ms);
	blit(cr, menu, 20 - mm, 66 - mm, 1.0);
	rei_pixbuf_unref(menu);

	/* A tooltip. */
	label(cr, "Tooltip", 230, 56, hint);
	int tw, th, tm;
	struct rei_pixbuf *tip = rei_bar_tooltip_render("Terminal \xe2\x80\x93 ~/src", cfg->toolbar_font,
							&cfg->theme.e[REI_ELEM_TOOLTIP], 1, &tw, &th, &tm);
	blit(cr, tip, 240 - tm, 66 - tm, 1.0);
	rei_pixbuf_unref(tip);

	/* The volume popup. */
	label(cr, "Volume popup", 230, 116, hint);
	struct rei_volpop_state vs;
	memset(&vs, 0, sizeof vs);
	vs.percent = 73;
	vs.max = 100;
	vs.hover = 2;
	vs.scale = 1;
	snprintf(vs.font, sizeof vs.font, "%s", cfg->toolbar_font);
	vs.box = cfg->theme.e[REI_ELEM_MENU];
	vs.row = cfg->theme.e[REI_ELEM_MENU_ROW];
	int vm;
	struct rei_pixbuf *vol = rei_bar_volpop_render(&vs, &vm);
	blit(cr, vol, 240 - vm, 126 - vm, 1.0);
	rei_pixbuf_unref(vol);

	/* The Run prompt (its own large font, so only a short line). */
	label(cr, "Run prompt", 230, 206, hint);
	struct rei_prompt_state ps;
	memset(&ps, 0, sizeof ps);
	snprintf(ps.text, sizeof ps.text, "firefox");
	ps.cursor_on = true;
	ps.box_w = 300;
	ps.scale = 1;
	snprintf(ps.font, sizeof ps.font, "Sans 18");
	ps.style = cfg->theme.e[REI_ELEM_RUN_PROMPT];
	int ph, pm;
	struct rei_pixbuf *prompt = rei_bar_prompt_render(&ps, &ph, &pm);
	blit(cr, prompt, 240 - pm, 220 - pm, 1.0);
	rei_pixbuf_unref(prompt);

	/* A confirmation dialog. */
	label(cr, "Dialog", 580, 56, hint);
	struct rei_confirm_state cs;
	memset(&cs, 0, sizeof cs);
	snprintf(cs.title, sizeof cs.title, "Shut down?");
	snprintf(cs.message, sizeof cs.message, "The computer will power off. Unsaved work will be lost.");
	snprintf(cs.confirm_label, sizeof cs.confirm_label, "Shut down");
	cs.selected = 1;
	cs.hover = -1;
	cs.scale = 1;
	snprintf(cs.font, sizeof cs.font, "%s", cfg->toolbar_font);
	cs.box = cfg->theme.e[REI_ELEM_DIALOG];
	rei_theme_button_style(&cfg->theme, REI_ELEM_DIALOG_BUTTONS, &cs.button);
	int cm;
	struct rei_pixbuf *dlg = rei_bar_confirm_render(&cs, &cm);
	blit(cr, dlg, 590 - cm, 66 - cm, 1.0);
	rei_pixbuf_unref(dlg);

	/* The message column over a backdrop, as in the stack view, and its fading look. */
	label(cr, "Messages (the stack view; older ones dim a step each in the compact view)", 20, 304, hint);
	int mw2 = cfg->messages_width < 260 ? cfg->messages_width : 260;
	struct rei_pixbuf *panel = rei_bar_message_panel_render(&cfg->theme.e[REI_ELEM_MESSAGES], 1, mw2 + 40, 170);
	blit(cr, panel, 25, 320, 1.0);
	rei_pixbuf_unref(panel);
	static const char *const msgs[3] = { "output of the first command", "second message", "the newest message" };
	double alphas[3] = { 0.64, 0.82, 1.0 };
	double y = 330;
	for (int i = 0; i < 3; i++) {
		struct rei_message_state m;
		memset(&m, 0, sizeof m);
		snprintf(m.text, sizeof m.text, "%s", msgs[i]);
		snprintf(m.font, sizeof m.font, "%s", cfg->messages_font);
		m.scale = 1;
		m.width = mw2;
		m.align_right = cfg->messages_right;
		m.style = cfg->theme.e[REI_ELEM_MESSAGES];
		int w2, h2;
		struct rei_pixbuf *pb = rei_bar_message_render(&m, &w2, &h2);
		blit(cr, pb, 35, y, alphas[i]);
		rei_pixbuf_unref(pb);
		y += h2 + 10;
	}

	/* The music controls (MPD) of the message stack view. */
	{
		label(cr, "mpd (music controls)", 450, 558, hint);
		struct rei_mpd_panel_state ms;
		memset(&ms, 0, sizeof ms);
		snprintf(ms.label, sizeof ms.label, "Now playing: Ada - One");
		ms.running = true;
		ms.mode = 1;
		ms.shuffle = true;
		ms.hover = 2;
		ms.scale = 1;
		snprintf(ms.font, sizeof ms.font, "%s", cfg->messages_font);
		ms.box = cfg->theme.e[REI_ELEM_MPD];
		rei_theme_button_style(&cfg->theme, REI_ELEM_MPD_TRANSPORT, &ms.button);
		rei_theme_button_style(&cfg->theme, REI_ELEM_MPD_MODES, &ms.button2);
		int mm;
		struct rei_pixbuf *mp = rei_bar_mpd_render(&ms, &mm);
		blit(cr, mp, 450 - mm, 566 - mm, 1.0);
		rei_pixbuf_unref(mp);
	}

	/* A terminal's window, through the window shader of [theme.window_text] (as if it were one of its apps). */
	label(cr, "window_text: a window of one of its apps (urxvt)", 10, 558, hint);
	struct rei_pixbuf *term = sample_terminal(420, 84);
	if (term) {
		blit(cr, term, 10, 562, 1.0);
		char serr[300];
		const struct rei_shader *sh = rei_shader_named(cfg->theme.window_shader, serr, sizeof serr);
		const struct rei_text_fx *wfx = &cfg->theme.e[REI_ELEM_WINDOW_TEXT].text_fx;
		bool custom = strcmp(cfg->theme.window_shader, "glow") != 0;
		if (sh && (custom || (wfx->glow_size > 0 && wfx->glow_strength > 0) || wfx->blink > 0))
			rei_fx_for_window(wfx, cfg->theme.window_threshold, cfg->theme.window_area_limit, sh,
					  &g_items[g_n_items - 1].fx);
		rei_pixbuf_unref(term);
	}

	cairo_destroy(cr);
	compose(surface, W, H);
	for (int i = 0; i < g_n_items; i++)
		rei_pixbuf_unref(g_items[i].pb);
	g_n_items = 0;
	cairo_status_t rc = cairo_surface_write_to_png(surface, png_path);
	cairo_surface_destroy(surface);
	rei_bar_render_cleanup();
	return rc == CAIRO_STATUS_SUCCESS ? 0 : 1;
}
