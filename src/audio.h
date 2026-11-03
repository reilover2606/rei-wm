#ifndef REI_AUDIO_H
#define REI_AUDIO_H

#include <stdbool.h>

struct rei_server;

/*
 * Output volume through PipeWire: the toolbar's volume widget and the media keys.
 *
 * rei-wm is a PipeWire client. It follows the default audio sink (the "default"
 * metadata object that WirePlumber maintains; without one, the highest-priority
 * sink) and reads and writes that sink's Props: channel volumes and mute. Volume
 * is shown and set on the perceptual (cubic) scale that wpctl, pavucontrol and
 * volume sliders everywhere use: 50% means a linear gain of 0.125.
 *
 * If PipeWire is not running the widget is hidden, and rei-wm keeps trying to
 * connect in the background. Everything runs on the event thread.
 */

struct rei_audio_state {
	bool available;  /* A sink is being followed. */
	int percent;     /* 0 .. max_volume */
	bool muted;
};

void rei_audio_start(struct rei_server *server);
void rei_audio_stop(struct rei_server *server);

struct rei_audio_state rei_audio_state(const struct rei_server *server);

/* Changes the volume by `percent` points (negative lowers it), within 0..max_volume.
 * 0 means the configured step. Returns false if there is no sink. */
bool rei_audio_change_volume(struct rei_server *server, int percent);
bool rei_audio_toggle_mute(struct rei_server *server);

/* The text the toolbar shows: "73%", "muted", or "" when there is no sink. */
void rei_audio_label(const struct rei_audio_state *state, char *out, unsigned long n);

#endif
