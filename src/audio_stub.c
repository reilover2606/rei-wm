#include "audio.h"

#include <stdio.h>

#include "server.h"

/* Built without libpipewire: there is no volume widget, and the media keys do nothing. */
void rei_audio_start(struct rei_server *server) { (void)server; }
void rei_audio_stop(struct rei_server *server) { (void)server; }
struct rei_audio_state rei_audio_state(const struct rei_server *server) { (void)server; return (struct rei_audio_state){ 0 }; }
bool rei_audio_change_volume(struct rei_server *server, int percent) { (void)server; (void)percent; return false; }
bool rei_audio_toggle_mute(struct rei_server *server) { (void)server; return false; }
void rei_audio_label(const struct rei_audio_state *st, char *out, unsigned long n)
{
	(void)st; (void)n;
	out[0] = '\0';
}
