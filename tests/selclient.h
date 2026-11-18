#ifndef REI_TEST_SELCLIENT_H
#define REI_TEST_SELCLIENT_H
/* Test client side of the clipboard (wl_data_device) and the primary selection: what a client has been told, a
 * source that answers requests at once, and helpers to read what an offer gives. Include after testlib.h and
 * primary-selection-unstable-v1-client-protocol.h. */

static uint32_t global_id(const struct client *c, const char *name)
{
	for (int i = 0; i < c->n_globals; i++)
		if (!strcmp(c->globals[i].name, name))
			return c->globals[i].id;
	CHECK(!"global missing");
	return 0;
}

/* What one client has been told about the clipboard and the primary selection. */
struct seen {
	struct client *c;
	bool clip_offer, prim_offer;
	struct wl_data_offer *clip;
	struct zwp_primary_selection_offer_v1 *prim;
	int n_clip_mimes, n_prim_mimes;
	char clip_mime[4][80], prim_mime[4][80];
	/* as a source: a request to write */
	bool prim_send;
	char send_mime[80];
	int send_fd;
	bool prim_cancelled;
	const char *text;   /* what to answer a request with */
};

static void off_offer(void *d, struct wl_data_offer *o, const char *mime)
{
	(void)o;
	struct seen *s = d;
	if (s->n_clip_mimes < 4)
		snprintf(s->clip_mime[s->n_clip_mimes++], 80, "%s", mime);
}
static void off_sa(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; (void)a; }
static const struct wl_data_offer_listener offer_listener = { off_offer, off_sa, off_sa };
static void dd_data_offer(void *d, struct wl_data_device *dev, struct wl_data_offer *o)
{
	(void)dev;
	struct seen *s = d;
	s->n_clip_mimes = 0;
	wl_data_offer_add_listener(o, &offer_listener, s);
}
static void dd_enter(void *d, struct wl_data_device *dev, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y,
		     struct wl_data_offer *o)
{
	(void)d; (void)dev; (void)s; (void)sf; (void)x; (void)y; (void)o;
}
static void dd_leave(void *d, struct wl_data_device *dev) { (void)d; (void)dev; }
static void dd_motion(void *d, struct wl_data_device *dev, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
	(void)d; (void)dev; (void)t; (void)x; (void)y;
}
static void dd_drop(void *d, struct wl_data_device *dev) { (void)d; (void)dev; }
static void dd_selection(void *d, struct wl_data_device *dev, struct wl_data_offer *o)
{
	(void)dev;
	struct seen *s = d;
	s->clip_offer = o != NULL;
	s->clip = o;
}
static const struct wl_data_device_listener dd_listener = { dd_data_offer, dd_enter, dd_leave, dd_motion, dd_drop, dd_selection };

static void poff_offer(void *d, struct zwp_primary_selection_offer_v1 *o, const char *mime)
{
	(void)o;
	struct seen *s = d;
	if (s->n_prim_mimes < 4)
		snprintf(s->prim_mime[s->n_prim_mimes++], 80, "%s", mime);
}
static const struct zwp_primary_selection_offer_v1_listener poffer_listener = { poff_offer };
static void pd_data_offer(void *d, struct zwp_primary_selection_device_v1 *dev, struct zwp_primary_selection_offer_v1 *o)
{
	(void)dev;
	struct seen *s = d;
	s->n_prim_mimes = 0;
	zwp_primary_selection_offer_v1_add_listener(o, &poffer_listener, s);
}
static void pd_selection(void *d, struct zwp_primary_selection_device_v1 *dev, struct zwp_primary_selection_offer_v1 *o)
{
	(void)dev;
	struct seen *s = d;
	s->prim_offer = o != NULL;
	s->prim = o;
}
static const struct zwp_primary_selection_device_v1_listener pd_listener = { pd_data_offer, pd_selection };

static void ps_send(void *d, struct zwp_primary_selection_source_v1 *src, const char *mime, int32_t fd)
{
	(void)src;
	struct seen *s = d;
	snprintf(s->send_mime, sizeof s->send_mime, "%s", mime);
	/* Answer at once: this is what a terminal does when asked for its highlighted text. */
	size_t n = strlen(s->text);
	CHECK(write(fd, s->text, n) == (ssize_t)n);
	close(fd);
	s->prim_send = true;
}
static void ps_cancelled(void *d, struct zwp_primary_selection_source_v1 *src)
{
	(void)src;
	((struct seen *)d)->prim_cancelled = true;
}
static const struct zwp_primary_selection_source_v1_listener ps_listener = { ps_send, ps_cancelled };

static void roundtrips(struct client *a, struct client *b)
{
	for (int i = 0; i < 3; i++) {
		CHECK(wl_display_roundtrip(a->d) >= 0);
		CHECK(wl_display_roundtrip(b->d) >= 0);
	}
}

/* Reads what an offer gives, answering the owner's requests as they come. */
static void read_clip(struct client *reader, struct client *owner, struct wl_data_offer *o, const char *mime, char *out, size_t n)
{
	int p[2];
	CHECK(pipe(p) == 0);
	wl_data_offer_receive(o, mime, p[1]);
	close(p[1]);
	for (int i = 0; i < 4; i++) {
		CHECK(wl_display_roundtrip(reader->d) >= 0);
		CHECK(wl_display_roundtrip(owner->d) >= 0);
	}
	sleep_ms(50);
	memset(out, 0, n);
	ssize_t r = read(p[0], out, n - 1);
	CHECK(r >= 0);
	close(p[0]);
}

static void read_prim(struct client *reader, struct client *owner, struct zwp_primary_selection_offer_v1 *o, const char *mime,
		      char *out, size_t n)
{
	int p[2];
	CHECK(pipe(p) == 0);
	zwp_primary_selection_offer_v1_receive(o, mime, p[1]);
	close(p[1]);
	for (int i = 0; i < 4; i++) {
		CHECK(wl_display_roundtrip(reader->d) >= 0);
		CHECK(wl_display_roundtrip(owner->d) >= 0);
	}
	sleep_ms(50);
	memset(out, 0, n);
	ssize_t r = read(p[0], out, n - 1);
	CHECK(r >= 0);
	close(p[0]);
}

#endif
