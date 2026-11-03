#ifndef REI_DRM_H
#define REI_DRM_H

/* Prints what the DRM devices on this machine offer, without starting the
 * compositor: connectors, modes, planes and formats. Returns an exit code. */
int rei_drm_print_info(void);

#endif
