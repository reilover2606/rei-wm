#include <dirent.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "backend/drm/drm.h"
#include "backend/drm/drm_logic.h"
#include "util/util.h"

/*
 * `rei-wm --drm-info`: a read-only look at the GPUs. It opens the device nodes
 * directly (no session, no mode setting), so it works from inside a desktop
 * session or over SSH, and says whether the DRM backend has what it needs.
 */

static const char *plane_type_name(uint64_t t)
{
	return t == 1 ? "primary" : t == 2 ? "cursor" : "overlay";
}

static uint64_t plane_prop(int fd, uint32_t id, const char *name)
{
	uint64_t value = 0;
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, id, DRM_MODE_OBJECT_PLANE);
	for (uint32_t i = 0; props && i < props->count_props; i++) {
		drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
		if (p && !strcmp(p->name, name))
			value = props->prop_values[i];
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return value;
}

static void describe(int fd, const char *path)
{
	printf("\n== %s ==\n", path);
	drmVersionPtr ver = drmGetVersion(fd);
	if (ver) {
		printf("driver: %s %d.%d.%d (%s)\n", ver->name, ver->version_major, ver->version_minor,
		       ver->version_patchlevel, ver->desc);
		drmFreeVersion(ver);
	}
	uint64_t cap;
	bool atomic = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0 &&
		      drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0;
	printf("atomic modesetting: %s\n", atomic ? "yes" : "NO (the DRM backend needs it)");
	printf("framebuffer modifiers: %s\n",
	       drmGetCap(fd, DRM_CAP_ADDFB2_MODIFIERS, &cap) == 0 && cap ? "yes" : "no");
	uint64_t cw = 0, ch = 0;
	drmGetCap(fd, DRM_CAP_CURSOR_WIDTH, &cw);
	drmGetCap(fd, DRM_CAP_CURSOR_HEIGHT, &ch);
	printf("hardware cursor size: %llux%llu\n", (unsigned long long)cw, (unsigned long long)ch);

	drmModeRes *res = drmModeGetResources(fd);
	if (!res) {
		printf("cannot read resources: %s\n", strerror(errno));
		return;
	}
	printf("CRTCs: %d\n", res->count_crtcs);

	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		if (!c)
			continue;
		char name[32];
		rei_drm_connector_name(name, sizeof name, c->connector_type, c->connector_type_id);
		printf("\nconnector %s: %s", name,
		       c->connection == DRM_MODE_CONNECTED ? "connected" :
		       c->connection == DRM_MODE_DISCONNECTED ? "disconnected" : "unknown");
		if (c->connection == DRM_MODE_CONNECTED)
			printf(", %ux%u mm", c->mmWidth, c->mmHeight);
		printf("\n");
		for (int m = 0; m < c->count_modes && c->connection == DRM_MODE_CONNECTED; m++) {
			const drmModeModeInfo *mi = &c->modes[m];
			bool il = mi->flags & DRM_MODE_FLAG_INTERLACE, ds = mi->flags & DRM_MODE_FLAG_DBLSCAN;
			printf("  mode %ux%u@%.3f%s%s%s\n", mi->hdisplay, mi->vdisplay,
			       rei_drm_refresh_mhz(mi->clock, mi->htotal, mi->vtotal, il, ds, mi->vscan) / 1000.0,
			       (mi->type & DRM_MODE_TYPE_PREFERRED) ? " (preferred)" : "",
			       il ? " interlaced" : "", ds ? " doublescan" : "");
		}
		if (c->connection == DRM_MODE_CONNECTED)
			printf("  config name: \"%s\"\n", name);
		drmModeFreeConnector(c);
	}

	drmModePlaneRes *pres = drmModeGetPlaneResources(fd);
	for (uint32_t i = 0; pres && i < pres->count_planes; i++) {
		drmModePlane *p = drmModeGetPlane(fd, pres->planes[i]);
		if (!p)
			continue;
		printf("\nplane %u: %s, usable with CRTC mask 0x%x\n", p->plane_id,
		       plane_type_name(plane_prop(fd, p->plane_id, "type")), p->possible_crtcs);
		uint64_t blob_id = plane_prop(fd, p->plane_id, "IN_FORMATS");
		struct rei_format_mod *fm = NULL;
		size_t n = 0;
		if (blob_id) {
			drmModePropertyBlobRes *blob = drmModeGetPropertyBlob(fd, (uint32_t)blob_id);
			if (blob) {
				n = rei_drm_parse_in_formats(blob->data, blob->length, &fm);
				drmModeFreePropertyBlob(blob);
			}
		}
		if (n) {
			printf("  %zu format/modifier pairs; XRGB8888: %s, ARGB8888: %s\n", n,
			       "see below", "see below");
			bool xr = false, ar = false;
			for (size_t k = 0; k < n; k++) {
				xr = xr || fm[k].format == DRM_FORMAT_XRGB8888;
				ar = ar || fm[k].format == DRM_FORMAT_ARGB8888;
			}
			printf("  XRGB8888: %s, ARGB8888: %s\n", xr ? "yes" : "no", ar ? "yes" : "no");
		} else {
			printf("  %u formats (no modifier information)\n", p->count_formats);
		}
		free(fm);
		drmModeFreePlane(p);
	}
	drmModeFreePlaneResources(pres);
	drmModeFreeResources(res);
}

int rei_drm_print_info(void)
{
	DIR *d = opendir("/dev/dri");
	if (!d) {
		fprintf(stderr, "rei-wm: cannot open /dev/dri: %s\n", strerror(errno));
		return 1;
	}
	int found = 0, failed = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "card", 4) != 0)
			continue;
		char path[300];
		snprintf(path, sizeof path, "/dev/dri/%s", e->d_name);
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			printf("\n== %s ==\ncannot open: %s\n", path, strerror(errno));
			failed++;
			continue;
		}
		found++;
		describe(fd, path);
		close(fd);
	}
	closedir(d);
	if (!found) {
		printf("\nNo GPU could be opened%s.\n",
		       failed ? " (permission denied? you need to be on a logged-in local session, "
				"or in the 'video' group)" : "");
		return 1;
	}
	return 0;
}
