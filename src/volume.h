#ifndef REI_VOLUME_H
#define REI_VOLUME_H

#include <stdbool.h>

#include "audio.h"

struct rei_server;

/*
 * The one volume: what the media keys, the wheel, the toolbar's volume text and the slider change and show.
 *
 * It is the PipeWire sink's volume (audio.h), except that MPD has a volume of its own that the sink does not
 * affect when MPD plays straight to the sound card or uses its software mixer. [audio] volume_target says
 * which one is "the volume":
 *   "auto" (default): MPD's while it is connected, has a mixer and is playing; the sink's otherwise
 *   "sink":           always the sink's
 *   "mpd":            MPD's whenever it is connected (and has a mixer)
 *   "both":           the sink's, and MPD is set to the same percentage
 * MPD's volume shows with a note in front ("♪ 60%") so it is clear which one the keys are on; it has no mute, so
 * "mute" sets it to 0 and the next one puts it back.
 */
struct rei_audio_state rei_volume_state(const struct rei_server *server);
bool rei_volume_change(struct rei_server *server, int percent);   /* 0: the configured step */
bool rei_volume_toggle_mute(struct rei_server *server);
/* The text for the toolbar ("73%", "♪ 60%", "muted"; "" when there is nothing to control). */
void rei_volume_label(const struct rei_server *server, char *out, unsigned long n);
/* Whether the volume shown is MPD's. */
bool rei_volume_is_mpd(const struct rei_server *server);

#endif
