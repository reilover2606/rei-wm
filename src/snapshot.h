#ifndef REI_SNAPSHOT_H
#define REI_SNAPSHOT_H

#include <stdatomic.h>
#include <stdint.h>

/*
 * An immutable, reference-counted description of everything on screen at one
 * instant. The event thread builds one whenever the scene changes and hands a
 * reference to each render thread, so rendering never touches live compositor
 * state and needs no locks.
 *
 * For now it carries only a serial number; surfaces, decorations, the
 * wallpaper and the cursor are added as those subsystems arrive.
 */
struct rei_scene_snapshot {
	atomic_int refs;
	uint64_t serial;
};

struct rei_scene_snapshot *rei_snapshot_create(uint64_t serial);
struct rei_scene_snapshot *rei_snapshot_ref(struct rei_scene_snapshot *s);
void rei_snapshot_unref(struct rei_scene_snapshot *s);

#endif
