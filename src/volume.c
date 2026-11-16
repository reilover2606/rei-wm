#include "volume.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "mpd.h"
#include "server.h"

/* The MPD volume is the one in use. */
static bool mpd_target(const struct rei_server *s, struct rei_mpd_info *info)
{
	rei_mpd_info_get(s, info);
	if (!info->connected || info->volume < 0)
		return false;
	switch (s->config->volume_target) {
	case REI_VOLUME_SINK:
	case REI_VOLUME_BOTH:
		return false;
	case REI_VOLUME_MPD:
		return true;
	default:
		return info->state == REI_MPD_PLAYING;
	}
}

bool rei_volume_is_mpd(const struct rei_server *s)
{
	struct rei_mpd_info info;
	return mpd_target(s, &info);
}

struct rei_audio_state rei_volume_state(const struct rei_server *s)
{
	struct rei_mpd_info info;
	if (mpd_target(s, &info))
		return (struct rei_audio_state){ .available = true, .percent = info.volume, .muted = info.muted };
	return rei_audio_state(s);
}

bool rei_volume_change(struct rei_server *s, int percent)
{
	if (percent == 0)
		percent = s->config->volume_step;
	struct rei_mpd_info info;
	if (mpd_target(s, &info)) {
		rei_mpd_set_volume(s, info.volume + percent);
		return true;
	}
	bool ok = rei_audio_change_volume(s, percent);
	if (s->config->volume_target == REI_VOLUME_BOTH) {   /* MPD follows the sink */
		rei_mpd_info_get(s, &info);
		struct rei_audio_state a = rei_audio_state(s);
		if (info.connected && info.volume >= 0 && a.available)
			rei_mpd_set_volume(s, a.muted ? 0 : a.percent);
	}
	return ok;
}

bool rei_volume_toggle_mute(struct rei_server *s)
{
	struct rei_mpd_info info;
	if (mpd_target(s, &info)) {
		rei_mpd_toggle_mute(s);
		return true;
	}
	bool ok = rei_audio_toggle_mute(s);
	if (s->config->volume_target == REI_VOLUME_BOTH) {
		rei_mpd_info_get(s, &info);
		struct rei_audio_state a = rei_audio_state(s);
		if (info.connected && info.volume >= 0 && a.available)
			rei_mpd_set_volume(s, a.muted ? 0 : a.percent);
	}
	return ok;
}

void rei_volume_label(const struct rei_server *s, char *out, unsigned long n)
{
	struct rei_mpd_info info;
	if (mpd_target(s, &info)) {
		if (info.muted)
			snprintf(out, n, "♪ muted");
		else
			snprintf(out, n, "♪ %d%%", info.volume);
		return;
	}
	struct rei_audio_state a = rei_audio_state(s);
	rei_audio_label(&a, out, n);
}
