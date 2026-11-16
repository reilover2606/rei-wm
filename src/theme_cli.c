#include "theme_cli.h"

#include <cairo.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
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

static void blit(cairo_t *cr, const struct rei_pixbuf *pb, double x, double y, double alpha)
{
	if (!pb)
		return;
	cairo_surface_t *s = cairo_image_surface_create_for_data((unsigned char *)pb->data, CAIRO_FORMAT_ARGB32,
								 pb->width, pb->height, pb->width * 4);
	cairo_set_source_surface(cr, s, x, y);
	cairo_paint_with_alpha(cr, alpha);
	cairo_surface_destroy(s);
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
	const int W = 1000, H = 540;
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
	cs.button = cfg->theme.e[REI_ELEM_START_BUTTON];
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

	cairo_destroy(cr);
	cairo_status_t rc = cairo_surface_write_to_png(surface, png_path);
	cairo_surface_destroy(surface);
	rei_bar_render_cleanup();
	return rc == CAIRO_STATUS_SUCCESS ? 0 : 1;
}
