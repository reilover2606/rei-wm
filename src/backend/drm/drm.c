#include <dirent.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <event2/event.h>
#include <fcntl.h>
#include <gbm.h>
#include <libudev.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <GLES3/gl3.h>

#include "backend/backend.h"
#include "backend/drm/drm.h"
#include "backend/drm/drm_logic.h"
#include "backend/drm/session.h"
#include "cursor.h"
#include "pointer_cursor.h"
#include "dmabuf.h"
#include "input/libinput_input.h"
#include "proto/linux_dmabuf.h"
#include "proto/syncobj.h"
#include "render/gles.h"
#include "render_thread.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"

/*
 * The DRM/KMS backend: drives displays directly, from a text console.
 *
 * Moving parts, and which thread touches them:
 *
 *  - The session (libseat) opens the GPU and input devices and tells us when a VT
 *    switch takes them away. Event thread.
 *  - Each output has a swapchain of two GBM buffers, imported into EGL as render
 *    targets. The output's render thread draws into one and queues an atomic
 *    page flip; the kernel's flip-complete event is read on the event thread,
 *    which tells the render thread (rei_render_notify_vblank). Frames are
 *    reported as presented only then, so frame callbacks follow the real refresh.
 *  - When one opaque client dma-buf covers a whole output, the render thread
 *    scans that buffer out directly instead of drawing (checked first with an
 *    atomic TEST_ONLY commit).
 *  - The pointer is a hardware cursor where the hardware allows, moved on the
 *    event thread without waking the renderer.
 *  - A udev monitor reports monitors being plugged and unplugged.
 */

#define N_SWAP 2
#define N_SCANOUT_CACHE 6

struct drm_backend;
struct drm_device;

struct drm_plane {
	uint32_t id;
	enum rei_drm_plane_type type;
	uint32_t possible_crtcs;
	uint32_t p_fb_id, p_crtc_id, p_src_x, p_src_y, p_src_w, p_src_h;
	uint32_t p_crtc_x, p_crtc_y, p_crtc_w, p_crtc_h;
	struct rei_format_mod *formats;
	size_t n_formats;
};

struct drm_crtc {
	uint32_t id;
	uint32_t p_active, p_mode_id;
};

struct swap_buf {
	struct gbm_bo *bo;
	uint32_t fb_id;
	struct rei_gles_buffer *glbuf;
};

struct scanout_entry {
	uint64_t dmabuf_id;  /* 0: free slot */
	uint32_t fb_id;
	uint32_t handles[REI_DMABUF_MAX_PLANES];
	int n_handles;
	uint64_t last_used;
};

struct drm_output {
	struct wl_list link;
	struct drm_backend *be;
	struct drm_device *dev;   /* the GPU the monitor is on (and which draws for it) */
	struct rei_output *out;  /* Event thread only; NULL while being torn down. */

	char name[32];
	uint32_t connector_id, p_connector_crtc_id;
	uint32_t crtc_id;
	size_t crtc_index;
	int plane_index;
	drmModeModeInfo mode;

	/* Render thread. */
	struct swap_buf buf[N_SWAP];
	int next;
	uint32_t mode_blob;
	uint32_t current_fb;
	struct scanout_entry scanout[N_SCANOUT_CACHE];
	uint64_t scanout_clock;
	uint64_t scanout_rejected_id;
	atomic_bool needs_modeset;

	/* Hardware cursor (event thread, except hw_cursor_active). */
	struct gbm_bo *cursor_bo;
	int cursor_w, cursor_h;
	bool crtc_on;            /* The first frame has reached the screen. */
	bool cursor_shown;
	uint64_t cursor_img_id;   /* the pixbuf in the cursor buffer */
	int32_t cursor_px, cursor_py;
	atomic_bool hw_cursor_active;
};

/* One GPU. Each output is drawn by, and scanned out from, the GPU its monitor is plugged into, so a machine with a
 * monitor on each of two GPUs (a laptop's panel on the integrated one, a monitor on the discrete one) works without
 * copying frames between them. */
struct drm_device {
	struct wl_list link;   /* drm_backend.devices */
	struct drm_backend *be;
	int fd, device_id;
	char *device_path;
	dev_t devnum;
	dev_t render_node;     /* what clients open to allocate buffers (linux-dmabuf feedback names it) */
	bool boot_vga;
	bool timestamps_monotonic, addfb_modifiers;
	uint64_t cursor_w, cursor_h;
	struct gbm_device *gbm;
	struct event *drm_event;

	struct drm_crtc *crtcs;
	size_t n_crtcs;
	struct drm_plane *planes;
	size_t n_planes;

	struct rei_format_mod *egl_formats;  /* What this GPU's renderer can draw into and sample. */
	size_t n_egl_formats;
};

struct drm_backend {
	struct rei_backend base;
	struct rei_session *session;
	struct rei_libinput *libinput;
	bool session_ready;
	atomic_bool active;
	atomic_bool sleeping;   /* display sleep: the monitors are switched off (see set_display_power) */

	struct wl_list devices;   /* drm_device, the main one first */
	struct drm_device *main;  /* the first: the one linux-dmabuf feedback and explicit synchronization are about */

	struct udev *udev;
	struct udev_monitor *monitor;
	struct event *monitor_event;
	struct event *rescan_timer;

	struct wl_list outputs;  /* drm_output, of every device */
	bool scanout_enabled, hw_cursor_enabled;
};

/* ------------------------------------------------------------------------ */
/* Properties                                                               */
/* ------------------------------------------------------------------------ */

static uint32_t find_prop(int fd, uint32_t obj_id, uint32_t obj_type, const char *name,
			  uint64_t *value)
{
	uint32_t id = 0;
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj_id, obj_type);
	if (!props)
		return 0;
	for (uint32_t i = 0; i < props->count_props && !id; i++) {
		drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
		if (!p)
			continue;
		if (strcmp(p->name, name) == 0) {
			id = p->prop_id;
			if (value)
				*value = props->prop_values[i];
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return id;
}

static bool add_prop(drmModeAtomicReq *req, uint32_t obj, uint32_t prop, uint64_t value)
{
	return prop != 0 && drmModeAtomicAddProperty(req, obj, prop, value) >= 0;
}

/* ------------------------------------------------------------------------ */
/* Device resources                                                         */
/* ------------------------------------------------------------------------ */

static void load_plane(struct drm_device *dev, struct drm_plane *pl, uint32_t id)
{
	pl->id = id;
	drmModePlane *p = drmModeGetPlane(dev->fd, id);
	if (!p)
		return;
	pl->possible_crtcs = p->possible_crtcs;

	uint64_t type = REI_DRM_PLANE_OVERLAY;
	find_prop(dev->fd, id, DRM_MODE_OBJECT_PLANE, "type", &type);
	pl->type = (enum rei_drm_plane_type)type;

	const struct { const char *name; uint32_t *dst; } props[] = {
		{ "FB_ID", &pl->p_fb_id }, { "CRTC_ID", &pl->p_crtc_id },
		{ "SRC_X", &pl->p_src_x }, { "SRC_Y", &pl->p_src_y },
		{ "SRC_W", &pl->p_src_w }, { "SRC_H", &pl->p_src_h },
		{ "CRTC_X", &pl->p_crtc_x }, { "CRTC_Y", &pl->p_crtc_y },
		{ "CRTC_W", &pl->p_crtc_w }, { "CRTC_H", &pl->p_crtc_h },
	};
	for (size_t i = 0; i < REI_ARRAY_LEN(props); i++)
		*props[i].dst = find_prop(dev->fd, id, DRM_MODE_OBJECT_PLANE, props[i].name, NULL);

	/* Preferably the IN_FORMATS blob, which also lists modifiers. */
	uint64_t blob_id = 0;
	if (find_prop(dev->fd, id, DRM_MODE_OBJECT_PLANE, "IN_FORMATS", &blob_id) && blob_id) {
		drmModePropertyBlobRes *blob = drmModeGetPropertyBlob(dev->fd, (uint32_t)blob_id);
		if (blob) {
			pl->n_formats = rei_drm_parse_in_formats(blob->data, blob->length, &pl->formats);
			drmModeFreePropertyBlob(blob);
		}
	}
	if (pl->n_formats == 0) {
		/* No modifier information: assume the plane takes the implicit layout. */
		pl->formats = rei_xcalloc(p->count_formats * 2 + 1, sizeof *pl->formats);
		for (uint32_t i = 0; i < p->count_formats; i++) {
			pl->formats[pl->n_formats++] = (struct rei_format_mod){ p->formats[i], REI_MOD_LINEAR };
			pl->formats[pl->n_formats++] = (struct rei_format_mod){ p->formats[i], REI_MOD_INVALID };
		}
	}
	drmModeFreePlane(p);
}

static bool load_resources(struct drm_device *dev)
{
	drmModeRes *res = drmModeGetResources(dev->fd);
	if (!res) {
		rei_error("drm: drmModeGetResources failed: %s", strerror(errno));
		return false;
	}
	dev->n_crtcs = (size_t)res->count_crtcs;
	dev->crtcs = rei_xcalloc(dev->n_crtcs ? dev->n_crtcs : 1, sizeof *dev->crtcs);
	for (size_t i = 0; i < dev->n_crtcs; i++) {
		dev->crtcs[i].id = res->crtcs[i];
		dev->crtcs[i].p_active = find_prop(dev->fd, res->crtcs[i], DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL);
		dev->crtcs[i].p_mode_id = find_prop(dev->fd, res->crtcs[i], DRM_MODE_OBJECT_CRTC, "MODE_ID", NULL);
	}
	drmModeFreeResources(res);

	drmModePlaneRes *pres = drmModeGetPlaneResources(dev->fd);
	if (!pres) {
		rei_error("drm: drmModeGetPlaneResources failed: %s", strerror(errno));
		return false;
	}
	dev->n_planes = pres->count_planes;
	dev->planes = rei_xcalloc(dev->n_planes ? dev->n_planes : 1, sizeof *dev->planes);
	for (size_t i = 0; i < dev->n_planes; i++)
		load_plane(dev, &dev->planes[i], pres->planes[i]);
	drmModeFreePlaneResources(pres);
	return true;
}

static void free_resources(struct drm_device *dev)
{
	for (size_t i = 0; i < dev->n_planes; i++)
		free(dev->planes[i].formats);
	free(dev->planes);
	free(dev->crtcs);
	dev->planes = NULL;
	dev->crtcs = NULL;
	dev->n_planes = dev->n_crtcs = 0;
}

/* ------------------------------------------------------------------------ */
/* Atomic commits                                                           */
/* ------------------------------------------------------------------------ */

static bool build_plane_request(struct drm_output *o, drmModeAtomicReq *req, uint32_t fb_id,
				bool modeset)
{
	struct drm_device *dev = o->dev;
	const struct drm_plane *pl = &dev->planes[o->plane_index];
	uint64_t w = o->mode.hdisplay, h = o->mode.vdisplay;

	bool ok = add_prop(req, pl->id, pl->p_fb_id, fb_id) &&
		  add_prop(req, pl->id, pl->p_crtc_id, o->crtc_id) &&
		  add_prop(req, pl->id, pl->p_src_x, 0) && add_prop(req, pl->id, pl->p_src_y, 0) &&
		  add_prop(req, pl->id, pl->p_src_w, w << 16) && add_prop(req, pl->id, pl->p_src_h, h << 16) &&
		  add_prop(req, pl->id, pl->p_crtc_x, 0) && add_prop(req, pl->id, pl->p_crtc_y, 0) &&
		  add_prop(req, pl->id, pl->p_crtc_w, w) && add_prop(req, pl->id, pl->p_crtc_h, h);
	if (!ok)
		return false;

	if (modeset) {
		const struct drm_crtc *crtc = &dev->crtcs[o->crtc_index];
		ok = add_prop(req, o->connector_id, o->p_connector_crtc_id, o->crtc_id) &&
		     add_prop(req, crtc->id, crtc->p_mode_id, o->mode_blob) &&
		     add_prop(req, crtc->id, crtc->p_active, 1);
		/* Whatever a previous owner left on other planes of this CRTC goes. */
		for (size_t i = 0; ok && i < dev->n_planes; i++) {
			const struct drm_plane *other = &dev->planes[i];
			if ((int)i == o->plane_index || other->type == REI_DRM_PLANE_CURSOR ||
			    !(other->possible_crtcs & (1u << o->crtc_index)))
				continue;
			add_prop(req, other->id, other->p_fb_id, 0);
			add_prop(req, other->id, other->p_crtc_id, 0);
		}
	}
	return ok;
}

static bool commit_frame(struct drm_output *o, uint32_t fb_id, bool modeset)
{
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	if (!req)
		return false;
	bool ok = build_plane_request(o, req, fb_id, modeset);
	if (ok) {
		uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT;
		flags |= modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET : DRM_MODE_ATOMIC_NONBLOCK;
		if (drmModeAtomicCommit(o->dev->fd, req, flags, o->dev) != 0) {
			rei_debug("drm: %s: atomic commit failed: %s", o->name, strerror(errno));
			ok = false;
		}
	}
	drmModeAtomicFree(req);
	return ok;
}

static bool test_frame(struct drm_output *o, uint32_t fb_id)
{
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	if (!req)
		return false;
	bool ok = build_plane_request(o, req, fb_id, false) &&
		  drmModeAtomicCommit(o->dev->fd, req, DRM_MODE_ATOMIC_TEST_ONLY, NULL) == 0;
	drmModeAtomicFree(req);
	return ok;
}

/* Turns a monitor off. Event thread, with the render thread already stopped. */
static void disable_output(struct drm_output *o)
{
	struct drm_backend *be = o->be;
	struct drm_device *dev = o->dev;
	if (!atomic_load(&be->active) || o->plane_index < 0)
		return;
	const struct drm_plane *pl = &dev->planes[o->plane_index];
	const struct drm_crtc *crtc = &dev->crtcs[o->crtc_index];
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	if (!req)
		return;
	add_prop(req, pl->id, pl->p_fb_id, 0);
	add_prop(req, pl->id, pl->p_crtc_id, 0);
	add_prop(req, o->connector_id, o->p_connector_crtc_id, 0);
	add_prop(req, crtc->id, crtc->p_active, 0);
	add_prop(req, crtc->id, crtc->p_mode_id, 0);
	drmModeAtomicCommit(dev->fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	drmModeAtomicFree(req);
}

/* ------------------------------------------------------------------------ */
/* Swapchain and scanout buffers (render thread)                            */
/* ------------------------------------------------------------------------ */

static uint32_t add_fb(struct drm_device *dev, int32_t w, int32_t h, uint32_t format,
		       const uint32_t handles[4], const uint32_t strides[4],
		       const uint32_t offsets[4], uint64_t modifier)
{
	uint32_t fb = 0;
	uint64_t mods[4] = { 0 };
	for (int i = 0; i < 4; i++)
		mods[i] = handles[i] ? modifier : 0;  /* Planes that are not used carry no modifier. */
	int ret;
	if (dev->addfb_modifiers && modifier != REI_MOD_INVALID)
		ret = drmModeAddFB2WithModifiers(dev->fd, (uint32_t)w, (uint32_t)h, format, handles,
						 strides, offsets, mods, &fb, DRM_MODE_FB_MODIFIERS);
	else
		ret = drmModeAddFB2(dev->fd, (uint32_t)w, (uint32_t)h, format, handles, strides,
				    offsets, &fb, 0);
	if (ret != 0) {
		rei_warn("drm: AddFB2 failed for %dx%d %.4s modifier 0x%llx (%s): %s", w, h,
			 (const char *)&format, (unsigned long long)modifier,
			 dev->addfb_modifiers && modifier != REI_MOD_INVALID ? "with modifiers" : "implicit",
			 strerror(errno));
		return 0;
	}
	return fb;
}

/* Attempt 0: layouts both the plane and the renderer share. 1: let the driver
 * choose. 2: plain linear memory, which everything can scan out. */
static struct gbm_bo *alloc_bo(struct drm_output *o, int attempt, const uint64_t *mods, unsigned n_mods)
{
	struct drm_device *dev = o->dev;
	uint32_t w = (uint32_t)o->mode.hdisplay, h = (uint32_t)o->mode.vdisplay;
	uint32_t use = GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING;
	if (attempt == 0)
		return n_mods ? gbm_bo_create_with_modifiers2(dev->gbm, w, h, GBM_FORMAT_XRGB8888, mods,
							      n_mods, use) : NULL;
	if (attempt == 1)
		return gbm_bo_create(dev->gbm, w, h, GBM_FORMAT_XRGB8888, use);
	return gbm_bo_create(dev->gbm, w, h, GBM_FORMAT_XRGB8888, use | GBM_BO_USE_LINEAR);
}

static void swap_buf_destroy(struct drm_output *o, struct rei_gles *gles, struct swap_buf *b)
{
	if (b->glbuf)
		rei_gles_buffer_destroy(gles, b->glbuf);
	if (b->fb_id)
		drmModeRmFB(o->dev->fd, b->fb_id);
	if (b->bo)
		gbm_bo_destroy(b->bo);
	*b = (struct swap_buf){ 0 };
}

static bool swap_buf_try(struct drm_output *o, struct rei_gles *gles, struct swap_buf *b, int attempt,
			 const uint64_t *mods, unsigned n_mods)
{
	struct drm_device *dev = o->dev;
	b->bo = alloc_bo(o, attempt, mods, n_mods);
	if (!b->bo)
		return false;

	int n_planes = gbm_bo_get_plane_count(b->bo);
	uint32_t handles[4] = { 0 }, strides[4] = { 0 }, offsets[4] = { 0 };
	struct rei_dmabuf proto = {
		.width = o->mode.hdisplay, .height = o->mode.vdisplay,
		.format = DRM_FORMAT_XRGB8888, .modifier = gbm_bo_get_modifier(b->bo),
		.n_planes = n_planes > REI_DMABUF_MAX_PLANES ? REI_DMABUF_MAX_PLANES : n_planes,
		.fd = { -1, -1, -1, -1 },
	};
	for (int i = 0; i < proto.n_planes; i++) {
		handles[i] = gbm_bo_get_handle_for_plane(b->bo, i).u32;
		strides[i] = gbm_bo_get_stride_for_plane(b->bo, i);
		offsets[i] = gbm_bo_get_offset(b->bo, i);
		proto.fd[i] = gbm_bo_get_fd_for_plane(b->bo, i);
		proto.stride[i] = strides[i];
		proto.offset[i] = offsets[i];
	}
	rei_info("drm: %s: trying %s buffers: %d plane(s), modifier 0x%llx, stride %u", o->name,
		 attempt == 0 ? "shared-layout" : attempt == 1 ? "driver-chosen" : "linear", n_planes,
		 (unsigned long long)proto.modifier, strides[0]);

	b->fb_id = add_fb(dev, o->mode.hdisplay, o->mode.vdisplay, DRM_FORMAT_XRGB8888, handles, strides,
			  offsets, proto.modifier);
	struct rei_dmabuf *d = rei_dmabuf_create(&proto);  /* Takes the fds. */
	if (d) {
		b->glbuf = rei_gles_buffer_import(gles, d);
		rei_dmabuf_unref(d);
	}
	if (!b->fb_id || !b->glbuf) {
		rei_warn("drm: %s: that layout does not work (%s)", o->name,
			 !b->fb_id ? "framebuffer creation failed" : "EGL import failed");
		swap_buf_destroy(o, gles, b);
		return false;
	}
	return true;
}

static bool swapchain_create(struct drm_output *o, struct rei_gles *gles)
{
	struct drm_device *dev = o->dev;
	const struct drm_plane *pl = &dev->planes[o->plane_index];

	/* Layouts both the plane can scan out and the renderer can draw into. */
	struct rei_format_mod *both = NULL;
	size_t n_both = rei_format_mod_intersect(pl->formats, pl->n_formats, dev->egl_formats,
						 dev->n_egl_formats, &both);
	uint64_t *mods = rei_xcalloc(n_both + 1, sizeof *mods);
	unsigned n_mods = 0;
	for (size_t i = 0; i < n_both; i++) {
		if (both[i].format == DRM_FORMAT_XRGB8888 && both[i].modifier != REI_MOD_INVALID)
			mods[n_mods++] = both[i].modifier;
	}
	free(both);

	/* Try the best layout first and settle for the first that fully works. */
	bool ok = false;
	const char *linear = getenv("REI_DRM_LINEAR");   /* A debugging knob: skip tiled/compressed layouts. */
	for (int attempt = linear && linear[0] == '1' ? 2 : 0; attempt < 3 && !ok; attempt++) {
		if (attempt == 0 && n_mods == 0)
			continue;
		ok = true;
		for (int i = 0; i < N_SWAP && ok; i++)
			ok = swap_buf_try(o, gles, &o->buf[i], attempt, mods, n_mods);
		if (!ok) {
			for (int i = 0; i < N_SWAP; i++)
				swap_buf_destroy(o, gles, &o->buf[i]);
		}
	}
	free(mods);
	if (!ok) {
		rei_error("drm: %s: cannot create scanout buffers in any layout", o->name);
		return false;
	}
	rei_info("drm: %s: %dx%d scanout buffers ready (modifier 0x%llx)", o->name,
		 o->mode.hdisplay, o->mode.vdisplay,
		 (unsigned long long)gbm_bo_get_modifier(o->buf[0].bo));
	return true;
}

static void scanout_entry_free(struct drm_output *o, struct scanout_entry *e)
{
	if (!e->dmabuf_id)
		return;
	drmModeRmFB(o->dev->fd, e->fb_id);
	for (int i = 0; i < e->n_handles; i++)
		drmCloseBufferHandle(o->dev->fd, e->handles[i]);
	*e = (struct scanout_entry){ 0 };
}

/* A framebuffer for a client's buffer, imported once and remembered. 0 on failure. */
static uint32_t scanout_fb(struct drm_output *o, const struct rei_dmabuf *d)
{
	struct scanout_entry *victim = NULL;
	for (int i = 0; i < N_SCANOUT_CACHE; i++) {
		struct scanout_entry *e = &o->scanout[i];
		if (e->dmabuf_id == d->id) {
			e->last_used = ++o->scanout_clock;
			return e->fb_id;
		}
		/* Reuse a free slot, else the least recently used one -- but never the
		 * framebuffer that is on screen: removing that would blank the output. */
		if (!e->dmabuf_id) {
			if (!victim || victim->dmabuf_id)
				victim = e;
		} else if (e->fb_id != o->current_fb &&
			   (!victim || (victim->dmabuf_id && e->last_used < victim->last_used))) {
			victim = e;
		}
	}
	if (!victim)
		return 0;
	scanout_entry_free(o, victim);

	struct scanout_entry e = { .dmabuf_id = d->id };
	uint32_t handles[4] = { 0 };
	for (int i = 0; i < d->n_planes; i++) {
		if (drmPrimeFDToHandle(o->dev->fd, d->fd[i], &handles[i]) != 0) {
			rei_debug("drm: %s: cannot import a client buffer: %s", o->name, strerror(errno));
			for (int j = 0; j < i; j++)
				drmCloseBufferHandle(o->dev->fd, handles[j]);
			return 0;
		}
		e.handles[i] = handles[i];
	}
	e.n_handles = d->n_planes;
	e.fb_id = add_fb(o->dev, d->width, d->height, d->format, handles, d->stride, d->offset, d->modifier);
	if (!e.fb_id) {
		for (int i = 0; i < e.n_handles; i++)
			drmCloseBufferHandle(o->dev->fd, e.handles[i]);
		return 0;
	}
	e.last_used = ++o->scanout_clock;
	*victim = e;
	return e.fb_id;
}

/* The framebuffer to show instead of composing, or 0. */
static uint32_t try_scanout(struct drm_output *o, const struct rei_scene_snapshot *snap,
			    const struct rei_gles_target *t)
{
	double scale = t->scale > 0 ? t->scale : 1.0;
	struct rei_drm_view_rect vr = {
		.x = (float)t->origin_x, .y = (float)t->origin_y,
		.w = (float)(t->width / scale), .h = (float)(t->height / scale),
		.pixel_w = t->width, .pixel_h = t->height,
	};
	const struct rei_drawable *cand = rei_drm_scanout_candidate(snap, &vr);
	if (!cand || cand->dmabuf->id == o->scanout_rejected_id)
		return 0;

	const struct drm_plane *pl = &o->dev->planes[o->plane_index];
	const struct rei_dmabuf *d = cand->dmabuf;
	uint32_t fb = 0;
	if (rei_format_mod_supported(pl->formats, pl->n_formats, d->format, d->modifier))
		fb = scanout_fb(o, d);
	if (fb && test_frame(o, fb)) {
		rei_debug("drm: %s: scanning out a client buffer directly", o->name);
		return fb;
	}
	/* Remember the verdict so a refusal is not retried on every frame. */
	o->scanout_rejected_id = d->id;
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Render thread hooks                                                      */
/* ------------------------------------------------------------------------ */

static bool drm_render_init(void *data, struct rei_gles *gles)
{
	struct drm_output *o = data;
	if (!gles)
		return false;
	if (!rei_gles_supports_dmabuf(gles)) {
		rei_error("drm: %s: the OpenGL driver cannot render into dma-bufs", o->name);
		return false;
	}
	return swapchain_create(o, gles);
}

static enum rei_render_result drm_render(void *data, struct rei_gles *gles,
					 const struct rei_scene_snapshot *snap,
					 const struct rei_gles_target *target)
{
	struct drm_output *o = data;
	if (!atomic_load(&o->be->active))
		return REI_RENDER_FAILED;

	bool modeset = atomic_load(&o->needs_modeset);
	uint32_t fb = 0;
	if (!modeset && o->be->scanout_enabled)
		fb = try_scanout(o, snap, target);

	bool composed = fb == 0;
	if (composed) {
		struct swap_buf *b = &o->buf[o->next];
		struct rei_gles_target t = *target;
		t.draw_cursor = !atomic_load(&o->hw_cursor_active);
		if (!rei_gles_draw_into(gles, snap, &t, b->glbuf))
			return REI_RENDER_FAILED;
		/* The kernel orders the flip after the GPU through the buffer's implicit
		 * fence, but not every driver stack does; this makes it unconditional. */
		glFinish();
		fb = b->fb_id;
	}

	if (!commit_frame(o, fb, modeset))
		return REI_RENDER_FAILED;
	if (composed)
		o->next = (o->next + 1) % N_SWAP;
	if (modeset)
		atomic_store(&o->needs_modeset, false);
	o->current_fb = fb;
	return REI_RENDER_FLIP;
}

static void drm_set_paused(void *data, bool paused)
{
	/* Display sleep pauses rendering and then (here, on the render thread, so after any frame in flight) switches
	 * the monitor off. A VT switch pauses too, but then the kernel has already taken the display away. */
	struct drm_output *o = data;
	if (paused && atomic_load(&o->be->sleeping))
		disable_output(o);
}

static void drm_render_fini(void *data)
{
	struct drm_output *o = data;
	/* The context is still current here, so GL objects can be deleted. */
	for (int i = 0; i < N_SWAP; i++) {
		if (o->buf[i].glbuf)
			rei_gles_buffer_destroy(NULL, o->buf[i].glbuf);
		o->buf[i].glbuf = NULL;
		if (atomic_load(&o->be->active)) {
			if (o->buf[i].fb_id)
				drmModeRmFB(o->dev->fd, o->buf[i].fb_id);
		}
		if (o->buf[i].bo)
			gbm_bo_destroy(o->buf[i].bo);
		o->buf[i] = (struct swap_buf){ 0 };
	}
	for (int i = 0; i < N_SCANOUT_CACHE; i++)
		scanout_entry_free(o, &o->scanout[i]);
}

/* ------------------------------------------------------------------------ */
/* Hardware cursor (event thread)                                           */
/* ------------------------------------------------------------------------ */

static void cursor_setup(struct drm_output *o)
{
	struct drm_backend *be = o->be;
	struct drm_device *dev = o->dev;
	if (!be->hw_cursor_enabled || dev->cursor_w == 0 || dev->cursor_h == 0)
		return;
	o->cursor_w = (int)dev->cursor_w;
	o->cursor_h = (int)dev->cursor_h;
	o->cursor_bo = gbm_bo_create(dev->gbm, (uint32_t)o->cursor_w, (uint32_t)o->cursor_h,
				     GBM_FORMAT_ARGB8888, GBM_BO_USE_CURSOR | GBM_BO_USE_WRITE);
	o->cursor_img_id = 0;   /* the picture is uploaded when it is first shown, and whenever it changes */
	if (!o->cursor_bo)
		rei_debug("drm: %s: no hardware cursor buffer", o->name);
}

/* Puts the pointer's picture into the cursor buffer. False if it cannot be shown by the hardware (too big,
 * or drawn scaled), in which case the compositor draws it. */
static bool cursor_upload(struct drm_output *o, const struct rei_cursor_image *img)
{
	const struct rei_pixbuf *pb = img->pixbuf;
	if (pb->id == o->cursor_img_id)
		return true;
	if (pb->width > o->cursor_w || pb->height > o->cursor_h || pb->width != img->width || pb->height != img->height)
		return false;
	/* The cursor plane wants premultiplied ARGB, exactly what a pixbuf holds. */
	size_t stride = (size_t)o->cursor_w * 4;
	uint8_t *buf = rei_xcalloc((size_t)o->cursor_h, stride);
	for (int32_t y = 0; y < pb->height; y++)
		memcpy(buf + (size_t)y * stride, pb->data + (size_t)y * (size_t)pb->width * 4, (size_t)pb->width * 4);
	bool ok = gbm_bo_write(o->cursor_bo, buf, stride * (size_t)o->cursor_h) == 0;
	free(buf);
	if (!ok)
		return false;
	o->cursor_img_id = pb->id;
	o->cursor_shown = false;   /* hand the buffer to the plane again, so it picks the new picture up */
	return true;
}

static void cursor_hide(struct drm_output *o)
{
	if (o->cursor_shown)
		drmModeSetCursor(o->dev->fd, o->crtc_id, 0, 0, 0);
	o->cursor_shown = false;
}

/* Puts the cursor where the pointer is. Returns whether the hardware cursor is
 * in charge of this output. */
static bool cursor_update(struct drm_output *o)
{
	struct drm_backend *be = o->be;
	struct rei_server *s = be->base.server;
	if (!o->cursor_bo || !o->out || !o->crtc_on || !atomic_load(&be->active) ||
	    o->out->scale != 1.0) {
		if (atomic_exchange(&o->hw_cursor_active, false))
			rei_server_schedule_damage(s);
		cursor_hide(o);
		return false;
	}

	const struct rei_cursor_image *img = rei_pointer_cursor_image(s);
	if (!img) {   /* the client under the pointer wants none: neither plane nor compositor draws it */
		cursor_hide(o);
		if (!atomic_exchange(&o->hw_cursor_active, true))
			rei_server_schedule_damage(s);
		return true;
	}
	if (!cursor_upload(o, img)) {
		if (atomic_exchange(&o->hw_cursor_active, false))
			rei_server_schedule_damage(s);
		cursor_hide(o);
		return false;
	}
	struct rei_drm_view_rect vr = {
		.x = (float)o->out->x, .y = (float)o->out->y,
		.w = (float)o->out->width, .h = (float)o->out->height,
		.pixel_w = o->out->width, .pixel_h = o->out->height,
	};
	int32_t px, py;
	bool visible;
	rei_drm_cursor_place((float)(s->cursor_x - img->hotspot_x), (float)(s->cursor_y - img->hotspot_y),
			     img->width, img->height, &vr, 1.0, &px, &py, &visible);

	if (!visible) {
		cursor_hide(o);
	} else {
		if (!o->cursor_shown) {
			uint32_t handle = gbm_bo_get_handle(o->cursor_bo).u32;
			if (drmModeSetCursor(o->dev->fd, o->crtc_id, handle, (uint32_t)o->cursor_w,
					     (uint32_t)o->cursor_h) != 0) {
				rei_warn("drm: %s: hardware cursor refused (%s); drawing it in software",
					 o->name, strerror(errno));
				gbm_bo_destroy(o->cursor_bo);
				o->cursor_bo = NULL;
				if (atomic_exchange(&o->hw_cursor_active, false))
					rei_server_schedule_damage(s);
				return false;
			}
			o->cursor_shown = true;
			o->cursor_px = o->cursor_py = INT32_MIN;
		}
		if (px != o->cursor_px || py != o->cursor_py) {
			drmModeMoveCursor(o->dev->fd, o->crtc_id, px, py);
			o->cursor_px = px;
			o->cursor_py = py;
		}
	}
	if (!atomic_exchange(&o->hw_cursor_active, true))
		rei_server_schedule_damage(s);  /* Redraw without the software cursor. */
	return true;
}

static bool drm_cursor_moved(struct rei_backend *b)
{
	struct drm_backend *be = (struct drm_backend *)b;
	bool all = !wl_list_empty(&be->outputs);
	struct drm_output *o;
	wl_list_for_each(o, &be->outputs, link)
		all = cursor_update(o) && all;
	return all;
}

/* ------------------------------------------------------------------------ */
/* Page flip events                                                         */
/* ------------------------------------------------------------------------ */

static void on_page_flip(int fd, unsigned int seq, unsigned int sec, unsigned int usec,
			 unsigned int crtc_id, void *data)
{
	(void)fd;
	(void)seq;
	(void)data;
	struct drm_device *dev = data;
	struct drm_output *o;
	wl_list_for_each(o, &dev->be->outputs, link) {
		if (o->dev != dev || o->crtc_id != crtc_id || !o->out)
			continue;
		if (!o->crtc_on) {
			o->crtc_on = true;
			cursor_update(o);  /* The CRTC is live now: the cursor can be set. */
		}
		uint64_t ns = dev->timestamps_monotonic
			? (uint64_t)sec * 1000000000ull + (uint64_t)usec * 1000ull : 0;
		rei_render_notify_vblank(o->out->render, ns);
		return;
	}
}

static void on_drm_readable(evutil_socket_t fd, short what, void *arg)
{
	(void)fd;
	(void)what;
	struct drm_device *dev = arg;
	drmEventContext ctx = { .version = 3, .page_flip_handler2 = on_page_flip };
	drmHandleEvent(dev->fd, &ctx);
}

/* ------------------------------------------------------------------------ */
/* Outputs                                                                  */
/* ------------------------------------------------------------------------ */

/* The [[output]] table for a connector: the one with its name (eDP-1, HDMI-A-1, ...), else the first
 * table that names no output, which then serves as the settings for every monitor not named. */
static const struct rei_output_config *config_for(const struct rei_server *s, const char *name)
{
	const struct rei_output_config *unnamed = NULL;
	for (size_t i = 0; i < s->config->n_outputs; i++) {
		const struct rei_output_config *oc = &s->config->outputs[i];
		if (oc->name && strcmp(oc->name, name) == 0)
			return oc;
		if (!oc->name && !unnamed)
			unnamed = oc;
	}
	return unnamed;
}

static struct drm_output *output_for_connector(struct drm_device *dev, uint32_t connector_id)
{
	struct drm_output *o;
	wl_list_for_each(o, &dev->be->outputs, link)
		if (o->dev == dev && o->connector_id == connector_id)
			return o;
	return NULL;
}

static void output_destroy(struct drm_output *o)
{
	struct drm_backend *be = o->be;
	rei_info("drm: removing output %s", o->name);

	/* Stops and joins the render thread, which frees the swapchain. */
	if (o->out) {
		struct rei_output *out = o->out;
		o->out = NULL;
		rei_server_remove_output(be->base.server, out);
	}
	if (atomic_load(&be->active)) {
		cursor_hide(o);
		disable_output(o);
	}
	if (o->cursor_bo)
		gbm_bo_destroy(o->cursor_bo);
	if (o->mode_blob)
		drmModeDestroyPropertyBlob(o->dev->fd, o->mode_blob);
	wl_list_remove(&o->link);
	free(o);
}

static bool output_create(struct drm_device *dev, drmModeConnector *conn, size_t crtc_index,
			  const struct rei_drm_mode *modes, int mode_index)
{
	struct drm_backend *be = dev->be;
	struct rei_server *server = be->base.server;
	struct drm_output *o = rei_xcalloc(1, sizeof *o);
	o->be = be;
	o->dev = dev;
	o->connector_id = conn->connector_id;
	o->crtc_index = crtc_index;
	o->crtc_id = dev->crtcs[crtc_index].id;
	o->mode = conn->modes[mode_index];
	rei_drm_connector_name(o->name, sizeof o->name, conn->connector_type, conn->connector_type_id);
	atomic_init(&o->needs_modeset, true);
	atomic_init(&o->hw_cursor_active, false);

	struct rei_drm_plane_info *infos = rei_xcalloc(dev->n_planes ? dev->n_planes : 1, sizeof *infos);
	for (size_t i = 0; i < dev->n_planes; i++)
		infos[i] = (struct rei_drm_plane_info){ dev->planes[i].type, dev->planes[i].possible_crtcs };
	o->plane_index = rei_drm_pick_plane(infos, dev->n_planes, crtc_index, REI_DRM_PLANE_PRIMARY, 0);
	free(infos);
	o->p_connector_crtc_id = find_prop(dev->fd, o->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL);
	if (o->plane_index < 0 || !o->p_connector_crtc_id ||
	    drmModeCreatePropertyBlob(dev->fd, &o->mode, sizeof o->mode, &o->mode_blob) != 0) {
		rei_error("drm: %s: cannot set up the output (no primary plane or mode blob)", o->name);
		if (o->mode_blob)
			drmModeDestroyPropertyBlob(dev->fd, o->mode_blob);
		free(o);
		return false;
	}
	cursor_setup(o);
	wl_list_insert(be->outputs.prev, &o->link);

	const struct rei_output_config *oc = config_for(server, o->name);
	if (oc)
		rei_info("drm: %s: using the [[output]] table %s%s (scale %g)", o->name, oc->name ? "named " : "without a name",
			 oc->name ? oc->name : "", oc->scale);
	else
		rei_info("drm: %s: no [[output]] table names it (defaults: preferred mode, scale 1)", o->name);
	struct rei_output_params p = {
		.name = o->name,
		.width = o->mode.hdisplay,
		.height = o->mode.vdisplay,
		.refresh_mhz = modes[mode_index].refresh_mhz,
		.scale = oc ? oc->scale : 1.0,
		.has_position = oc ? oc->has_position : false,
		.x = oc ? oc->x : 0,
		.y = oc ? oc->y : 0,
		.render_ops = {
			.gles = { REI_GLES_GBM, dev->gbm },
			.init = drm_render_init,
			.render = drm_render,
			.set_paused = drm_set_paused,
			.fini = drm_render_fini,
		},
		.render_ops_data = o,
		.backend_data = o,
	};
	rei_info("drm: output %s: %dx%d@%.3f on CRTC %u (primary plane %u) of %s%s", o->name,
		 p.width, p.height, p.refresh_mhz / 1000.0, o->crtc_id, dev->planes[o->plane_index].id,
		 dev->device_path, o->cursor_bo ? ", hardware cursor" : ", software cursor");
	o->out = rei_server_add_output(server, &p);
	if (!o->out) {
		wl_list_remove(&o->link);
		if (o->cursor_bo)
			gbm_bo_destroy(o->cursor_bo);
		drmModeDestroyPropertyBlob(dev->fd, o->mode_blob);
		free(o);
		return false;
	}
	return true;
}

/* Brings the outputs of one GPU in line with the connectors that are plugged into it. */
static void scan_connectors(struct drm_device *dev)
{
	struct drm_backend *be = dev->be;
	drmModeRes *res = drmModeGetResources(dev->fd);
	if (!res) {
		rei_warn("drm: cannot read the connector list: %s", strerror(errno));
		return;
	}
	struct rei_server *server = be->base.server;

	struct candidate {
		drmModeConnector *conn;
		struct rei_drm_mode *modes;
		int mode_index;
	};
	struct candidate *cands = rei_xcalloc((size_t)res->count_connectors + 1, sizeof *cands);
	size_t n_cands = 0;
	uint32_t taken = 0;  /* CRTC indices already used by running outputs. */

	/* Drop outputs whose monitor has gone, and note which CRTCs the rest use. */
	struct drm_output *o, *tmp;
	wl_list_for_each_safe(o, tmp, &be->outputs, link) {
		if (o->dev != dev)
			continue;
		bool still = false;
		for (int i = 0; i < res->count_connectors && !still; i++) {
			if ((uint32_t)res->connectors[i] != o->connector_id)
				continue;
			drmModeConnector *c = drmModeGetConnector(dev->fd, o->connector_id);
			still = c && c->connection == DRM_MODE_CONNECTED;
			const struct rei_output_config *oc = config_for(server, o->name);
			if (oc && !oc->enabled)
				still = false;
			drmModeFreeConnector(c);
		}
		if (!still)
			output_destroy(o);
		else
			taken |= 1u << o->crtc_index;
	}

	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(dev->fd, res->connectors[i]);
		if (!c)
			continue;
		char name[32];
		rei_drm_connector_name(name, sizeof name, c->connector_type, c->connector_type_id);
		const struct rei_output_config *oc = config_for(server, name);
		if (c->connection != DRM_MODE_CONNECTED || c->count_modes == 0 ||
		    output_for_connector(dev, c->connector_id) || (oc && !oc->enabled)) {
			drmModeFreeConnector(c);
			continue;
		}

		struct rei_drm_mode *modes = rei_xcalloc((size_t)c->count_modes, sizeof *modes);
		for (int m = 0; m < c->count_modes; m++) {
			const drmModeModeInfo *mi = &c->modes[m];
			bool interlaced = mi->flags & DRM_MODE_FLAG_INTERLACE;
			bool dblscan = mi->flags & DRM_MODE_FLAG_DBLSCAN;
			modes[m] = (struct rei_drm_mode){
				.width = mi->hdisplay, .height = mi->vdisplay,
				.refresh_mhz = rei_drm_refresh_mhz(mi->clock, mi->htotal, mi->vtotal,
								   interlaced, dblscan, mi->vscan),
				.preferred = mi->type & DRM_MODE_TYPE_PREFERRED,
				.interlaced = interlaced, .doublescan = dblscan,
			};
		}
		int pick = oc && oc->has_mode
			? rei_drm_pick_mode(modes, (size_t)c->count_modes, oc->width, oc->height,
					    oc->refresh_mhz)
			: rei_drm_pick_mode(modes, (size_t)c->count_modes, 0, 0, 0);
		if (pick < 0) {
			rei_warn("drm: %s: no %dx%d mode; using the preferred one", name,
				 oc->width, oc->height);
			pick = rei_drm_pick_mode(modes, (size_t)c->count_modes, 0, 0, 0);
		}
		cands[n_cands++] = (struct candidate){ c, modes, pick };
	}

	/* Give each new monitor a CRTC of its own. */
	uint32_t *possible = rei_xcalloc(n_cands ? n_cands : 1, sizeof *possible);
	int *current = rei_xcalloc(n_cands ? n_cands : 1, sizeof *current);
	int *assigned = rei_xcalloc(n_cands ? n_cands : 1, sizeof *assigned);
	for (size_t i = 0; i < n_cands; i++) {
		drmModeConnector *c = cands[i].conn;
		current[i] = -1;
		for (int e = 0; e < c->count_encoders; e++) {
			drmModeEncoder *enc = drmModeGetEncoder(dev->fd, c->encoders[e]);
			if (!enc)
				continue;
			possible[i] |= enc->possible_crtcs;
			if (c->encoders[e] == c->encoder_id) {
				for (size_t k = 0; k < dev->n_crtcs; k++)
					if (dev->crtcs[k].id == enc->crtc_id)
						current[i] = (int)k;
			}
			drmModeFreeEncoder(enc);
		}
		possible[i] &= ~taken;
	}
	rei_drm_assign_crtcs(possible, current, n_cands, dev->n_crtcs, assigned);

	for (size_t i = 0; i < n_cands; i++) {
		char name[32];
		rei_drm_connector_name(name, sizeof name, cands[i].conn->connector_type,
				       cands[i].conn->connector_type_id);
		if (assigned[i] < 0)
			rei_warn("drm: %s: no free CRTC; leaving this monitor off", name);
		else
			output_create(dev, cands[i].conn, (size_t)assigned[i], cands[i].modes,
				      cands[i].mode_index);
		drmModeFreeConnector(cands[i].conn);
		free(cands[i].modes);
	}
	free(possible);
	free(current);
	free(assigned);
	free(cands);
	drmModeFreeResources(res);

	drm_cursor_moved(&be->base);
}

/* Every GPU's outputs. */
static void scan_all(struct drm_backend *be)
{
	struct drm_device *dev;
	wl_list_for_each(dev, &be->devices, link)
		scan_connectors(dev);
	if (wl_list_empty(&be->outputs))
		rei_warn("drm: no monitor is connected");
}

/* ------------------------------------------------------------------------ */
/* Hotplug                                                                  */
/* ------------------------------------------------------------------------ */

static void probe_devices(struct drm_backend *be);

static void on_rescan(evutil_socket_t fd, short what, void *arg)
{
	(void)fd;
	(void)what;
	struct drm_backend *be = arg;
	if (!atomic_load(&be->active))
		return;  /* Picked up when the session comes back. */
	rei_info("drm: monitors changed; rescanning");
	probe_devices(be);   /* a GPU that had no monitor when we started may have one now */
	scan_all(be);
	rei_server_schedule_damage(be->base.server);
}

static void on_monitor_readable(evutil_socket_t fd, short what, void *arg)
{
	(void)fd;
	(void)what;
	struct drm_backend *be = arg;
	struct udev_device *ud;
	while ((ud = udev_monitor_receive_device(be->monitor))) {
		const char *hotplug = udev_device_get_property_value(ud, "HOTPLUG");
		if (udev_device_get_devnum(ud) != 0 && hotplug && !strcmp(hotplug, "1")) {
			/* A monitor takes a moment to settle; coalesce bursts of events. (Any GPU of ours, or one we have
			 * not opened because it had no monitor: probe_devices looks at those.) */
			struct timeval tv = { .tv_sec = 0, .tv_usec = 250000 };
			evtimer_add(be->rescan_timer, &tv);
		}
		udev_device_unref(ud);
	}
}

/* ------------------------------------------------------------------------ */
/* Session: VT switches                                                     */
/* ------------------------------------------------------------------------ */

static void session_disable(void *data)
{
	struct drm_backend *be = data;
	if (!be->session_ready)
		return;
	atomic_store(&be->active, false);
	struct drm_output *o;
	wl_list_for_each(o, &be->outputs, link) {
		o->crtc_on = false;
		o->cursor_shown = false;
		atomic_store(&o->hw_cursor_active, false);
		if (o->out)
			rei_render_set_paused(o->out->render, true);
	}
	if (be->libinput)
		rei_libinput_suspend(be->libinput);
}

static void session_enable(void *data)
{
	struct drm_backend *be = data;
	if (!be->session_ready)
		return;
	atomic_store(&be->active, true);
	atomic_store(&be->sleeping, false);   /* coming back to the VT lights the monitors again */
	if (be->libinput)
		rei_libinput_resume(be->libinput);

	/* Monitors may have changed while another session had the display. */
	scan_all(be);
	struct drm_output *o;
	wl_list_for_each(o, &be->outputs, link) {
		o->current_fb = 0;
		atomic_store(&o->needs_modeset, true);
		o->scanout_rejected_id = 0;
		if (o->out)
			rei_render_set_paused(o->out->render, false);
	}
	rei_server_schedule_damage(be->base.server);
}

/* Display sleep. Off: hide the hardware cursor (a plane on a switched-off CRTC would be refused), stop the render
 * thread, which then switches the monitor off. On: the first frame after is a full modeset. */
static void drm_set_display_power(struct rei_backend *b, bool on)
{
	struct drm_backend *be = (struct drm_backend *)b;
	if (!be->session_ready || atomic_load(&be->sleeping) == !on)
		return;
	atomic_store(&be->sleeping, !on);
	struct drm_output *o;
	wl_list_for_each(o, &be->outputs, link) {
		if (!o->out)
			continue;
		if (!on) {
			o->crtc_on = false;
			cursor_hide(o);
			atomic_store(&o->hw_cursor_active, false);
			rei_render_set_paused(o->out->render, true);
		} else {
			o->current_fb = 0;
			atomic_store(&o->needs_modeset, true);
			o->scanout_rejected_id = 0;
			rei_render_set_paused(o->out->render, false);
		}
	}
	if (on)
		rei_server_schedule_damage(be->base.server);
}

static void drm_switch_vt(struct rei_backend *b, unsigned vt)
{
	struct drm_backend *be = (struct drm_backend *)b;
	if (be->session)
		rei_session_switch_vt(be->session, vt);
}

/* ------------------------------------------------------------------------ */
/* Device selection and start-up                                            */
/* ------------------------------------------------------------------------ */

struct candidate_dev {
	char *path;
	bool boot_vga;
};

static int cmp_candidates(const void *a, const void *b)
{
	const struct candidate_dev *x = a, *y = b;
	if (x->boot_vga != y->boot_vga)
		return x->boot_vga ? -1 : 1;
	return strcmp(x->path, y->path);
}

/* The GPU device nodes on our seat, the boot GPU first. */
static size_t list_gpus(struct drm_backend *be, struct candidate_dev **out)
{
	*out = NULL;
	const char *forced = getenv("REI_DRM_DEVICE");
	if (forced && forced[0]) {
		*out = rei_xcalloc(1, sizeof **out);
		(*out)[0].path = rei_xstrdup(forced);
		return 1;
	}

	const char *seat = rei_session_seat_name(be->session);
	struct udev_enumerate *e = udev_enumerate_new(be->udev);
	udev_enumerate_add_match_subsystem(e, "drm");
	udev_enumerate_add_match_sysname(e, "card[0-9]*");
	udev_enumerate_scan_devices(e);

	size_t n = 0, cap = 4;
	struct candidate_dev *list = rei_xcalloc(cap, sizeof *list);
	struct udev_list_entry *entry;
	udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e)) {
		struct udev_device *dev = udev_device_new_from_syspath(be->udev, udev_list_entry_get_name(entry));
		if (!dev)
			continue;
		const char *dev_seat = udev_device_get_property_value(dev, "ID_SEAT");
		const char *node = udev_device_get_devnode(dev);
		if (node && !strcmp(dev_seat ? dev_seat : "seat0", seat)) {
			struct udev_device *pci = udev_device_get_parent_with_subsystem_devtype(dev, "pci", NULL);
			const char *vga = pci ? udev_device_get_sysattr_value(pci, "boot_vga") : NULL;
			if (n == cap) {
				cap *= 2;
				list = rei_xrealloc(list, cap * sizeof *list);
			}
			list[n++] = (struct candidate_dev){ rei_xstrdup(node), vga && !strcmp(vga, "1") };
		}
		udev_device_unref(dev);
	}
	udev_enumerate_unref(e);
	qsort(list, n, sizeof *list, cmp_candidates);
	*out = list;
	return n;
}

static bool device_has_monitor(int fd)
{
	drmModeRes *res = drmModeGetResources(fd);
	bool found = false;
	for (int i = 0; res && i < res->count_connectors && !found; i++) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		found = c && c->connection == DRM_MODE_CONNECTED;
		drmModeFreeConnector(c);
	}
	drmModeFreeResources(res);
	return found;
}

/* The render node of a GPU (what clients open to allocate buffers), else its primary node: the device that
 * linux-dmabuf feedback names. 0 if unknown. */
static dev_t render_node_of(int fd)
{
	dev_t result = 0;
	drmDevicePtr dev = NULL;
	if (drmGetDevice2(fd, 0, &dev) == 0) {
		struct stat st;
		if ((dev->available_nodes & (1 << DRM_NODE_RENDER)) && stat(dev->nodes[DRM_NODE_RENDER], &st) == 0)
			result = st.st_rdev;
		else if ((dev->available_nodes & (1 << DRM_NODE_PRIMARY)) && stat(dev->nodes[DRM_NODE_PRIMARY], &st) == 0)
			result = st.st_rdev;
		drmFreeDevice(&dev);
	}
	return result;
}

/* Opens one GPU through the session and checks that it can do atomic mode setting. NULL if it cannot. */
static struct drm_device *device_open(struct drm_backend *be, const char *path, bool boot_vga)
{
	int id;
	int fd = rei_session_open(be->session, path, &id);
	if (fd < 0)
		return NULL;
	uint64_t cap = 0;
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0 ||
	    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0 ||
	    drmGetCap(fd, DRM_CAP_ADDFB2_MODIFIERS, &cap) != 0) {
		rei_warn("drm: %s does not support atomic mode setting; skipping", path);
		rei_session_close(be->session, id, fd);
		return NULL;
	}
	struct drm_device *dev = rei_xcalloc(1, sizeof *dev);
	dev->be = be;
	dev->fd = fd;
	dev->device_id = id;
	dev->device_path = rei_xstrdup(path);
	dev->boot_vga = boot_vga;
	wl_list_init(&dev->link);
	uint64_t v = 0;
	dev->addfb_modifiers = drmGetCap(fd, DRM_CAP_ADDFB2_MODIFIERS, &v) == 0 && v;
	dev->timestamps_monotonic = drmGetCap(fd, DRM_CAP_TIMESTAMP_MONOTONIC, &v) == 0 && v;
	if (drmGetCap(fd, DRM_CAP_CURSOR_WIDTH, &dev->cursor_w) != 0)
		dev->cursor_w = 64;
	if (drmGetCap(fd, DRM_CAP_CURSOR_HEIGHT, &dev->cursor_h) != 0)
		dev->cursor_h = 64;
	struct stat st;
	if (fstat(fd, &st) == 0)
		dev->devnum = st.st_rdev;
	dev->render_node = render_node_of(fd);
	return dev;
}

static void device_free(struct drm_device *dev)
{
	struct drm_backend *be = dev->be;
	if (dev->drm_event)
		event_free(dev->drm_event);
	free_resources(dev);
	free(dev->egl_formats);
	if (dev->gbm)
		gbm_device_destroy(dev->gbm);
	if (be->session && dev->fd >= 0)
		rei_session_close(be->session, dev->device_id, dev->fd);
	if (!wl_list_empty(&dev->link))
		wl_list_remove(&dev->link);
	free(dev->device_path);
	free(dev);
}

/* GBM, the planes and CRTCs, what the renderer can use, and the page-flip events. False if the GPU is not usable. */
static bool device_start(struct drm_device *dev)
{
	struct drm_backend *be = dev->be;
	dev->gbm = gbm_create_device(dev->fd);
	if (!dev->gbm) {
		rei_error("drm: cannot create a GBM device on %s", dev->device_path);
		return false;
	}
	if (!load_resources(dev))
		return false;
	/* What the renderer can draw into and sample: scanout buffers and client buffers. */
	struct rei_format_mod *egl = NULL;
	size_t n_egl = (size_t)rei_gles_query_dmabuf_formats(dev->gbm, &egl);
	if (n_egl == 0) {
		rei_error("drm: %s: the OpenGL driver reports no dma-buf formats; cannot use this GPU "
			  "(GBM/EGL not usable on it?)", dev->device_path);
		return false;
	}
	dev->egl_formats = egl;
	dev->n_egl_formats = n_egl;
	dev->drm_event = event_new(be->base.server->base, dev->fd, EV_READ | EV_PERSIST, on_drm_readable, dev);
	event_add(dev->drm_event, NULL);

	drmVersionPtr ver = drmGetVersion(dev->fd);
	rei_info("drm: using %s (driver %s%s%s), render node %u:%u, %zu CRTC(s)", dev->device_path,
		 ver && ver->name ? ver->name : "?", dev->boot_vga ? ", the boot GPU" : "",
		 dev->addfb_modifiers ? "" : ", no framebuffer modifiers", major(dev->render_node), minor(dev->render_node),
		 dev->n_crtcs);
	if (ver)
		drmFreeVersion(ver);
	return true;
}

/* What clients may send us: the 8-bit RGB formats every GPU in use can sample. With several GPUs, only the layouts they all
 * can sample, so that a client's buffer shows on every monitor (a layout one GPU invented may not import on another). */
static void advertise_formats(struct drm_backend *be, bool first)
{
	static const uint32_t client_formats[] = { REI_FORMAT_ARGB8888, REI_FORMAT_XRGB8888,
						   REI_FORMAT_ABGR8888, REI_FORMAT_XBGR8888 };
	struct rei_format_mod *common = NULL, *main_list = NULL;
	size_t n_common = 0, n_main = 0;
	bool have = false;
	size_t n_devs = 0;
	struct drm_device *dev;
	wl_list_for_each(dev, &be->devices, link) {
		struct rei_format_mod *mine = rei_xcalloc(dev->n_egl_formats ? dev->n_egl_formats : 1, sizeof *mine);
		size_t n_mine = 0;
		for (size_t i = 0; i < dev->n_egl_formats; i++)
			for (size_t f = 0; f < REI_ARRAY_LEN(client_formats); f++)
				if (dev->egl_formats[i].format == client_formats[f])
					mine[n_mine++] = dev->egl_formats[i];
		n_devs++;
		if (dev == be->main) {
			main_list = rei_xcalloc(n_mine ? n_mine : 1, sizeof *main_list);
			memcpy(main_list, mine, n_mine * sizeof *mine);
			n_main = n_mine;
		}
		if (!have) {
			common = mine;
			n_common = n_mine;
			have = true;
		} else {
			struct rei_format_mod *both = NULL;
			size_t nb = rei_format_mod_intersect(common, n_common, mine, n_mine, &both);
			free(common);
			free(mine);
			common = both;
			n_common = nb;
		}
	}
	if (n_common == 0 && n_devs > 1) {
		rei_warn("drm: the GPUs have no buffer layout in common; windows may not show on all monitors "
			 "(advertising the main GPU's layouts only)");
		free(common);
		common = main_list;
		n_common = n_main;
		main_list = NULL;
	}
	if (n_devs > 1)
		rei_info("drm: %zu GPUs: advertising the %zu buffer layouts all of them can use", n_devs, n_common);
	if (first)
		rei_linux_dmabuf_init(be->base.server, common, n_common, be->main ? be->main->render_node : 0);
	else
		rei_linux_dmabuf_update_formats(be->base.server, common, n_common);
	free(common);
	free(main_list);
}

static struct drm_device *device_by_path(struct drm_backend *be, const char *path)
{
	struct drm_device *dev;
	wl_list_for_each(dev, &be->devices, link)
		if (!strcmp(dev->device_path, path))
			return dev;
	return NULL;
}

/* A GPU we did not open (it had no monitor) may have one now. */
static void probe_devices(struct drm_backend *be)
{
	struct candidate_dev *list;
	size_t n = list_gpus(be, &list);
	bool added = false;
	for (size_t i = 0; i < n; i++) {
		if (!device_by_path(be, list[i].path)) {
			struct drm_device *dev = device_open(be, list[i].path, list[i].boot_vga);
			if (dev && device_has_monitor(dev->fd) && device_start(dev)) {
				wl_list_insert(be->devices.prev, &dev->link);
				rei_info("drm: %s has a monitor now", dev->device_path);
				added = true;
			} else if (dev) {
				device_free(dev);
			}
		}
		free(list[i].path);
	}
	free(list);
	if (added)
		advertise_formats(be, false);
}

static bool open_gpus(struct drm_backend *be)
{
	struct candidate_dev *list;
	size_t n = list_gpus(be, &list);
	if (n == 0) {
		rei_error("drm: no GPU found on seat '%s'", rei_session_seat_name(be->session));
		free(list);
		return false;
	}
	const char *mode = be->base.server->config->drm_devices;
	bool first_only = mode && !strcmp(mode, "first");

	struct drm_device *fallback = NULL;
	for (size_t i = 0; i < n; i++) {
		struct drm_device *dev = device_open(be, list[i].path, list[i].boot_vga);
		if (!dev)
			continue;
		if (device_has_monitor(dev->fd)) {
			if (first_only && !wl_list_empty(&be->devices)) {
				rei_info("drm: [drm] devices = \"first\": not using %s", dev->device_path);
				device_free(dev);
			} else {
				wl_list_insert(be->devices.prev, &dev->link);
			}
		} else if (!fallback) {
			fallback = dev;   /* the first GPU with no monitor: used only if no other has one */
		} else {
			device_free(dev);
		}
	}
	if (wl_list_empty(&be->devices) && fallback) {
		wl_list_insert(be->devices.prev, &fallback->link);
		fallback = NULL;
	}
	if (fallback)
		device_free(fallback);
	for (size_t i = 0; i < n; i++)
		free(list[i].path);
	free(list);
	if (wl_list_empty(&be->devices)) {
		rei_error("drm: no usable GPU (atomic KMS required)");
		return false;
	}

	/* Start them; one that cannot be used is dropped. */
	struct drm_device *dev, *tmp;
	wl_list_for_each_safe(dev, tmp, &be->devices, link)
		if (!device_start(dev))
			device_free(dev);
	if (wl_list_empty(&be->devices))
		return false;
	be->main = wl_container_of(be->devices.next, be->main, link);
	return true;
}

/* libinput opens input devices through the session, so no root is needed. */
struct input_dev {
	int fd, id;
};
static struct input_dev input_devs[256];

static int input_open(const char *path, int flags, void *data)
{
	(void)flags;
	struct drm_backend *be = data;
	int id;
	int fd = rei_session_open(be->session, path, &id);
	if (fd < 0)
		return -1;
	if (fd < 256) {
		input_devs[fd].fd = fd;
		input_devs[fd].id = id;
	}
	return fd;
}

static void input_close(int fd, void *data)
{
	struct drm_backend *be = data;
	if (fd >= 0 && fd < 256)
		rei_session_close(be->session, input_devs[fd].id, fd);
	else
		close(fd);
}

static bool drm_start(struct rei_backend *b)
{
	struct drm_backend *be = (struct drm_backend *)b;
	struct rei_server *server = b->server;
	static const struct rei_session_callbacks cb = { session_enable, session_disable };

	be->udev = udev_new();
	be->session = rei_session_create(server->base, &cb, be);
	if (!be->session || !be->udev)
		return false;
	if (!open_gpus(be))
		return false;
	advertise_formats(be, true);

	/* Explicit synchronization (clients' sync object points instead of the kernel's implicit ordering), on the main GPU:
	 * the sync objects clients hand over are imported there. Off with [drm] explicit_sync = false. */
	if (server->config->drm_explicit_sync)
		rei_syncobj_init_drm(server, be->main->fd);
	else
		rei_info("explicit synchronization: off ([drm] explicit_sync = false)");

	const char *no_scanout = getenv("REI_DRM_NO_SCANOUT");
	const char *no_hwc = getenv("REI_DRM_NO_HW_CURSOR");
	be->scanout_enabled = !(no_scanout && no_scanout[0] == '1');
	be->hw_cursor_enabled = !(no_hwc && no_hwc[0] == '1');

	be->monitor = udev_monitor_new_from_netlink(be->udev, "udev");
	if (be->monitor) {
		udev_monitor_filter_add_match_subsystem_devtype(be->monitor, "drm", NULL);
		udev_monitor_enable_receiving(be->monitor);
		be->monitor_event = event_new(server->base, udev_monitor_get_fd(be->monitor),
					      EV_READ | EV_PERSIST, on_monitor_readable, be);
		event_add(be->monitor_event, NULL);
	}
	be->rescan_timer = evtimer_new(server->base, on_rescan, be);

	be->session_ready = true;
	atomic_store(&be->active, rei_session_active(be->session));

	/* Input first, so the seat has its capabilities before any client binds it. */
	static const struct rei_libinput_ops input_ops = { input_open, input_close };
	be->libinput = rei_libinput_create(server, rei_session_seat_name(be->session), &input_ops, be);
	if (!be->libinput)
		return false;
	scan_all(be);
	if (wl_list_empty(&be->outputs)) {
		rei_error("drm: no monitor could be started");
		return false;
	}
	return true;
}

static void drm_destroy(struct rei_backend *b)
{
	struct drm_backend *be = (struct drm_backend *)b;
	struct drm_output *o, *tmp;
	wl_list_for_each_safe(o, tmp, &be->outputs, link)
		output_destroy(o);

	rei_libinput_destroy(be->libinput);
	if (be->rescan_timer)
		event_free(be->rescan_timer);
	if (be->monitor_event)
		event_free(be->monitor_event);
	if (be->monitor)
		udev_monitor_unref(be->monitor);
	struct drm_device *dev, *dtmp;
	wl_list_for_each_safe(dev, dtmp, &be->devices, link)
		device_free(dev);
	rei_session_destroy(be->session);
	if (be->udev)
		udev_unref(be->udev);
	free(be);
}

static void drm_config_changed(struct rei_backend *b)
{
	struct drm_backend *be = (struct drm_backend *)b;
	if (be->libinput)
		rei_libinput_reconfigure(be->libinput);
}

static const struct rei_backend_impl drm_impl = {
	.name = "drm",
	.start = drm_start,
	.destroy = drm_destroy,
	.cursor_moved = drm_cursor_moved,
	.config_changed = drm_config_changed,
	.switch_vt = drm_switch_vt,
	.set_display_power = drm_set_display_power,
};

struct rei_backend *rei_drm_backend_create(struct rei_server *server)
{
	struct drm_backend *be = rei_xcalloc(1, sizeof *be);
	be->base.impl = &drm_impl;
	be->base.server = server;
	atomic_init(&be->active, false);
	wl_list_init(&be->outputs);
	wl_list_init(&be->devices);
	return &be->base;
}
