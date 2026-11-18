#ifndef REI_SELECTION_H
#define REI_SELECTION_H

#include <stdbool.h>
#include <stddef.h>
#include <wayland-server-core.h>

struct rei_server;

/*
 * What a selection (the clipboard, or the primary selection: the text you have highlighted) is offered from. The
 * two protocols (wl_data_device, zwp_primary_selection) both fill one of these for a client's wl_data_source /
 * primary selection source, and the compositor makes its own, holding the text itself, when it mirrors a
 * terminal's highlighted text to the clipboard (see clipboard.h).
 */
enum rei_selection_kind { REI_SEL_CLIPBOARD, REI_SEL_PRIMARY };

struct rei_selection_source;

/* A source whose data is not a client's wl_data_source: the X11 bridge's (an X11 program owns the selection). */
struct rei_selection_ops {
	void (*send)(struct rei_selection_source *src, const char *mime, int fd);   /* write `mime` to `fd` (caller closes it) */
	void (*cancelled)(struct rei_selection_source *src);   /* no longer the selection: free it */
};

struct rei_selection_source {
	struct rei_server *server;
	enum rei_selection_kind kind;
	struct wl_resource *resource;   /* the client's source object; NULL when the compositor owns it */
	struct wl_array mime_types;     /* NUL-terminated strings, concatenated */
	size_t n_mimes;
	struct wl_signal destroy_signal;
	char *text;                     /* compositor-owned: what it serves, in every mime type it offers */
	size_t text_len;
	bool mirror;                    /* made by the compositor from another selection: not mirrored again */
	const struct rei_selection_ops *ops;   /* the X11 bridge: how to fetch the data (resource and text are NULL) */
	void *ops_data;
};

/* A compositor-owned source serving `text` as text. */
struct rei_selection_source *rei_selection_source_new_text(struct rei_server *server, enum rei_selection_kind kind,
							   const char *text, size_t len, bool mirror);
void rei_selection_source_add_mime(struct rei_selection_source *src, const char *mime);
bool rei_selection_source_has_mime(const struct rei_selection_source *src, const char *mime);
/* The best plain-text type it offers, or NULL if none. */
const char *rei_selection_source_text_mime(const struct rei_selection_source *src);
/* Asks the source to write `mime` to `fd` (which the caller still owns and closes). */
void rei_selection_source_send(struct rei_selection_source *src, const char *mime, int fd);
/* The source is no longer the selection: a client's is told so; a compositor-owned one is freed. */
void rei_selection_source_cancel(struct rei_selection_source *src);
/* Writes `len` bytes of `data` to `fd` (copied; the caller still closes `fd`) without ever blocking the event loop. */
void rei_selection_write_async(struct rei_server *server, int fd, const void *data, size_t len);
/* Frees a compositor-owned source (after telling whoever listens that it is gone). */
void rei_selection_source_free(struct rei_selection_source *src);

#endif
