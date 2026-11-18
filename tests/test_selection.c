/*
 * Primary selection and the terminal-to-clipboard rule, between Wayland clients:
 *   - the primary selection works like the clipboard: the client being typed into sets it, others learn of it
 *     when they get the keyboard focus, and can read it from the owner;
 *   - when the owner is a terminal (its app id is in [clipboard] terminals), what it highlights also becomes the
 *     clipboard: read from it at once and kept, so it can be pasted (Ctrl+V) in another program, even after the
 *     terminal has gone;
 *   - an ordinary program's highlight does not touch the clipboard.
 */
#define SOCKET "rei-selection-test"
#include "testlib.h"

#include "primary-selection-unstable-v1-client-protocol.h"
#include "selclient.h"

#define KEY_H 35
#define KEY_L 38

int main(int argc, char **argv)
{
	CHECK(argc == 2);
	quiet_client_leaks();
	snprintf(g_dir, sizeof g_dir, "/tmp/rei-selection-test-XXXXXX");
	CHECK(mkdtemp(g_dir));
	chmod(g_dir, 0700);
	start_compositor(argv[1],
			 "[toolbar]\nenabled = false\n[general]\nxwayland = false\nfocus_follows_mouse = false\n"
			 "[[output]]\nname = \"OUT\"\nwidth = 800\nheight = 600\n",
			 true);

	struct client a, b;   /* A: a terminal (foot); B: an ordinary program */
	client_connect(&a);
	client_connect(&b);
	g_clients[0] = &a;
	g_clients[1] = &b;
	client_bind_keyboard(&a);
	client_bind_keyboard(&b);
	static struct seen sa, sb;
	sa.c = &a;
	sb.c = &b;
	struct wl_data_device *dda = wl_data_device_manager_get_data_device(a.ddm, a.seat);
	struct wl_data_device *ddb = wl_data_device_manager_get_data_device(b.ddm, b.seat);
	wl_data_device_add_listener(dda, &dd_listener, &sa);
	wl_data_device_add_listener(ddb, &dd_listener, &sb);
	struct zwp_primary_selection_device_manager_v1 *pma = wl_registry_bind(
		a.registry, global_id(&a, "zwp_primary_selection_device_manager_v1"), &zwp_primary_selection_device_manager_v1_interface, 1);
	struct zwp_primary_selection_device_manager_v1 *pmb = wl_registry_bind(
		b.registry, global_id(&b, "zwp_primary_selection_device_manager_v1"), &zwp_primary_selection_device_manager_v1_interface, 1);
	CHECK(pma && pmb);
	struct zwp_primary_selection_device_v1 *pda = zwp_primary_selection_device_manager_v1_get_device(pma, a.seat);
	struct zwp_primary_selection_device_v1 *pdb = zwp_primary_selection_device_manager_v1_get_device(pmb, b.seat);
	zwp_primary_selection_device_v1_add_listener(pda, &pd_listener, &sa);
	zwp_primary_selection_device_v1_add_listener(pdb, &pd_listener, &sb);

	struct window wa, wb;
	g_app_id = "foot";
	open_win(&a, &wa, "terminal", 0xff204060);
	CHECK(wa.activated);
	roundtrips(&a, &b);

	/* ---- The terminal highlights text: its primary selection... ---- */
	sa.text = "highlighted in the terminal";
	struct zwp_primary_selection_source_v1 *ps = zwp_primary_selection_device_manager_v1_create_source(pma);
	zwp_primary_selection_source_v1_add_listener(ps, &ps_listener, &sa);
	zwp_primary_selection_source_v1_offer(ps, "text/plain;charset=utf-8");
	zwp_primary_selection_source_v1_offer(ps, "text/plain");
	zwp_primary_selection_device_v1_set_selection(pda, ps, 0);
	roundtrips(&a, &b);
	CHECK(sa.prim_send);   /* the compositor asked for the text at once, to keep it */
	CHECK(!sa.prim_cancelled);

	/* ---- ...and a program that gets the focus later sees both. ---- */
	g_app_id = "rei-test";
	open_win(&b, &wb, "ordinary", 0xff406020);
	CHECK(wb.activated);
	roundtrips(&a, &b);
	CHECK(sb.prim_offer && sb.n_prim_mimes == 2);
	CHECK(sb.clip_offer);   /* the clipboard too: Ctrl+V pastes the highlighted text */
	char got[128];
	read_clip(&b, &a, sb.clip, "text/plain;charset=utf-8", got, sizeof got);
	CHECK_STR(got, "highlighted in the terminal");
	read_prim(&b, &a, sb.prim, "text/plain", got, sizeof got);   /* a middle click reads it from the terminal */
	CHECK_STR(got, "highlighted in the terminal");

	/* ---- An ordinary program highlighting text does not touch the clipboard. ---- */
	static struct seen sb2;
	sb2.c = &b;
	sb2.text = "highlighted elsewhere";
	struct zwp_primary_selection_source_v1 *ps2 = zwp_primary_selection_device_manager_v1_create_source(pmb);
	zwp_primary_selection_source_v1_add_listener(ps2, &ps_listener, &sb2);
	zwp_primary_selection_source_v1_offer(ps2, "text/plain;charset=utf-8");
	zwp_primary_selection_device_v1_set_selection(pdb, ps2, 0);
	roundtrips(&a, &b);
	CHECK(!sb2.prim_send);   /* nobody asked it for anything */
	CHECK(sa.prim_cancelled);   /* the terminal's selection is replaced, as with any primary selection */

	/* The terminal gets the focus back: it sees B's primary selection, and the clipboard still has its own text. */
	inject("motion_abs 400 300");
	input_sync();
	mod(KEY_H);   /* focus the other window */
	roundtrips(&a, &b);
	CHECK(wa.activated || wb.activated);
	if (!wa.activated) {
		mod(KEY_L);
		roundtrips(&a, &b);
	}
	CHECK(wa.activated);
	CHECK(sa.prim_offer && sa.clip_offer);
	read_clip(&a, &b, sa.clip, "text/plain", got, sizeof got);
	CHECK_STR(got, "highlighted in the terminal");

	/* ---- The terminal's text outlives the terminal: Ctrl+V still pastes it. ---- */
	close_win(&wa);
	roundtrips(&a, &b);
	CHECK(wb.activated);
	CHECK(sb.clip_offer || sb.clip);
	read_clip(&b, &a, sb.clip, "text/plain;charset=utf-8", got, sizeof got);

	zwp_primary_selection_source_v1_destroy(ps2);
	CHECK(stop_compositor() == 0);
	puts("selection: ok");
	return 0;
}
