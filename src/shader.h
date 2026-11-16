#ifndef REI_SHADER_H
#define REI_SHADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fx.h"

struct rei_text_fx;

/*
 * GLSL fragment shaders for effects: the built-in ones for the compositor's own text (glow, blink, a
 * metallic shine) and for windows (glow/bloom), and any file a user names for a window.
 *
 * A shader is written the way picom's window shaders are, so those can be used unchanged in most cases:
 *
 *     vec4 window_shader() {
 *         vec4 c = texture(tex, texcoord / u_size);    // or rei_texel(texcoord)
 *         return default_post_processing(c);
 *     }
 *
 * with, already declared:
 *     sampler2D tex            the picture (premultiplied alpha, RGBA)
 *     vec2 texcoord            this fragment's position in it, in pixels
 *     vec2 u_size              its size in pixels
 *     float u_px               texture pixels per logical pixel (2 on a 2x output)
 *     float time               milliseconds, for animation (a shader that mentions it is redrawn continuously)
 *     float opacity            the window's overall opacity
 *     vec4 u_p[4]              the effect's parameters (see rei_fx)
 *     float u_blink            1 or 0, per the effect's blink period
 *     vec4 rei_texel(vec2 px)  the picture at a pixel position (clamped to its edges)
 *     vec4 rei_glow(vec2 px, float radius, float threshold)   the bright parts around px, blurred
 *     vec4 default_post_processing(vec4 c)    applies the opacity; call it on the result
 *
 * The language is GLSL ES 3.00. A file's own "#version" line and its declarations of the names above are
 * dropped, and texture2D() is accepted as a synonym of texture().
 *
 * Shaders are created on the event thread, never change, and live until exit, so a scene snapshot can
 * point at one without counting references.
 */
struct rei_shader;

uint64_t rei_shader_id(const struct rei_shader *s);
/* The whole fragment program, ready to compile. */
const char *rei_shader_source(const struct rei_shader *s);
const char *rei_shader_name(const struct rei_shader *s);
/* Whether it reads `time` (NULL: no). */
bool rei_shader_uses_time(const struct rei_shader *s);

/* The shader for text layers: glow, shine and blink, each in the amount its parameters say. */
const struct rei_shader *rei_shader_text(void);
/* The built-in window shader: bloom around the bright parts (the text of a terminal), and blink. */
const struct rei_shader *rei_shader_window_glow(void);
/* "glow", or a file (a leading ~/ is the home directory). NULL with a message if it cannot be read. A file
 * is read again when it has changed. */
const struct rei_shader *rei_shader_named(const char *name_or_path, char *err, size_t errlen);

/* Whether any of a style's text effects is on. */
bool rei_text_fx_enabled(const struct rei_text_fx *t);
/* The drawable effect for a text layer with these settings. */
void rei_fx_for_text(const struct rei_text_fx *t, struct rei_fx *out);
/* ...and for a window: glow (threshold: how bright a pixel must be to glow, 0..1) and blink from the same
 * settings, through `shader` (rei_shader_window_glow() or a custom one). */
void rei_fx_for_window(const struct rei_text_fx *t, float threshold, const struct rei_shader *shader,
		       struct rei_fx *out);

#endif
