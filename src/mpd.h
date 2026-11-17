#ifndef REI_MPD_H
#define REI_MPD_H

#include <stdbool.h>

struct rei_server;
struct rei_pixbuf;

/*
 * A small MPD client, on the event loop (the protocol is plain text; no libmpdclient).
 *
 * rei-wm keeps one connection to MPD, which it keeps asking "tell me when the player changes" (idle), and
 * tries to reconnect every couple of seconds while MPD is not running (so the controls know whether to
 * show play/pause/skip or a button that starts it). A new song while playing is announced in the message
 * stack ("Now playing: Artist - Title"). Besides play/pause/next/previous it sets shuffle (random), loop all
 * (repeat) and loop one (repeat with single).
 *
 * Where MPD is: [mpd] host (an IP address, "localhost" or the path of a socket), else $MPD_HOST, else
 * $XDG_RUNTIME_DIR/mpd/socket when that exists, else localhost; the port from [mpd] port or $MPD_PORT.
 * A password may be given as "password@host" (or a socket path).
 */
enum rei_mpd_state { REI_MPD_STOPPED, REI_MPD_PLAYING, REI_MPD_PAUSED };

struct rei_mpd_info {
	bool enabled;     /* [mpd] enabled */
	bool connected;   /* MPD answered: its controls work */
	enum rei_mpd_state state;
	int volume;       /* 0..100, or -1 when MPD has no mixer to set it with */
	bool muted;       /* "muted" by rei-wm: the volume was set to 0 and will be put back */
	bool shuffle;     /* random */
	bool loop_all;    /* repeat the playlist */
	bool loop_one;    /* repeat the current song */
	char title[256];  /* "Artist - Title" (or the title, or the file name); "" when nothing is loaded */
	int elapsed_ms;   /* how far into the song (moves on by itself while playing); -1 when stopped or unknown */
	int duration_ms;  /* the song's length; -1 when unknown (a stream) */
};

/* (Re)reads the [mpd] settings: connects, or disconnects when disabled. Cheap when nothing changed. */
void rei_mpd_configure(struct rei_server *server);
void rei_mpd_stop(struct rei_server *server);

void rei_mpd_info_get(const struct rei_server *server, struct rei_mpd_info *out);

/* Controls (no-ops while not connected). */
void rei_mpd_play(struct rei_server *server);    /* resumes when paused, starts when stopped */
void rei_mpd_pause(struct rei_server *server);
void rei_mpd_toggle(struct rei_server *server);
void rei_mpd_next(struct rei_server *server);
void rei_mpd_previous(struct rei_server *server);
/* Shuffle (random); loop all (repeat the playlist); loop one (repeat the song). The two loops exclude each other,
 * and turning the lit one off stops looping. */
void rei_mpd_toggle_shuffle(struct rei_server *server);
void rei_mpd_toggle_loop_all(struct rei_server *server);
void rei_mpd_toggle_loop_one(struct rei_server *server);
/* MPD's own volume (setvol), 0..100. The new value is known at once, before MPD has answered, so keys pressed
 * in quick succession add up. */
void rei_mpd_set_volume(struct rei_server *server, int percent);
/* There is no mute in MPD: sets the volume to 0 and remembers it, or puts it back. */
void rei_mpd_toggle_mute(struct rei_server *server);
/* Runs [mpd] start_command (default "mpd") and keeps looking for it for a few seconds. */
void rei_mpd_launch(struct rei_server *server);

/* Jumps to `seconds` in the song being played or paused (no-op otherwise, or when its length is unknown). The new
 * position shows at once, before MPD has answered. */
void rei_mpd_seek(struct rei_server *server, double seconds);

/* The cover of the current song (from MPD's readpicture, else the cover file next to it), shrunk to at most 640 pixels;
 * NULL when there is none, or [mpd] album_art is off. Owned by the client: use it before returning to the event
 * loop, or take a reference. */
struct rei_pixbuf *rei_mpd_album_art(const struct rei_server *server);

/* The song's text from an MPD "currentsong" answer. Exposed for tests. */
void rei_mpd_format_song(const char *artist, const char *title, const char *name, const char *file,
			 char *out, unsigned long outlen);

#endif
